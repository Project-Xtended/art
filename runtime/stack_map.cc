/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stack_map.h"

#include <iomanip>
#include <stdint.h>

#include "art_method.h"
#include "base/indenter.h"
#include "base/stats.h"
#include "oat_quick_method_header.h"
#include "scoped_thread_state_change-inl.h"

namespace art {

CodeInfo::CodeInfo(const OatQuickMethodHeader* header, DecodeFlags flags)
  : CodeInfo(header->GetOptimizedCodeInfoPtr(), flags) {
}

void CodeInfo::Decode(const uint8_t* data, DecodeFlags flags) {
  BitMemoryReader reader(data);
  uint32_t header[kNumHeaders];
  reader.ReadVarints(header);
  ForEachHeaderField([this, &header](size_t i, auto member_pointer) {
    this->*member_pointer = header[i];
  });
  ForEachBitTableField([this, &reader](size_t i, auto member_pointer) {
    auto& table = this->*member_pointer;
    if (HasBitTable(i)) {
      if (UNLIKELY(IsBitTableDeduped(i))) {
        ssize_t bit_offset = reader.NumberOfReadBits() - reader.ReadVarint();
        BitMemoryReader reader2(reader.data(), bit_offset);  // The offset is negative.
        table.Decode(reader2);
      } else {
        table.Decode(reader);
      }
    }
  }, flags);
  size_in_bits_ = reader.NumberOfReadBits();
  if (flags == AllTables) {
    DCHECK_EQ(HasInlineInfo(data), HasInlineInfo());
  }
}

size_t CodeInfo::Deduper::Dedupe(const uint8_t* code_info_data) {
  writer_.ByteAlign();
  size_t deduped_offset = writer_.NumberOfWrittenBits() / kBitsPerByte;

  // Read the existing code info and find (and keep) dedup-map iterator for each table.
  // The iterator stores BitMemoryRegion and bit_offset of previous identical BitTable.
  BitMemoryReader reader(code_info_data);
  CodeInfo code_info;  // Temporary storage for decoded data.
  ForEachHeaderField([&reader, &code_info](size_t, auto member_pointer) {
    code_info.*member_pointer = reader.ReadVarint();
  });
  std::map<BitMemoryRegion, uint32_t, BitMemoryRegion::Less>::iterator it[kNumBitTables];
  ForEachBitTableField([this, &reader, &code_info, &it](size_t i, auto member_pointer) {
    DCHECK(!code_info.IsBitTableDeduped(i));
    if (code_info.HasBitTable(i)) {
      size_t bit_table_start = reader.NumberOfReadBits();
      (code_info.*member_pointer).Decode(reader);
      BitMemoryRegion region = reader.GetReadRegion().Subregion(bit_table_start);
      it[i] = dedupe_map_.emplace(region, /* default bit_offset */ 0).first;
      if (it[i]->second != 0 && region.size_in_bits() > 32) {  // Seen before and large?
        code_info.SetBitTableDeduped(i);  // Mark as deduped before we write header.
      }
    }
  });

  // Write the code info back, but replace deduped tables with relative offsets.
  ForEachHeaderField([this, &code_info](size_t, auto member_pointer) {
    writer_.WriteVarint(code_info.*member_pointer);
  });
  ForEachBitTableField([this, &code_info, &it](size_t i, auto) {
    if (code_info.HasBitTable(i)) {
      uint32_t& bit_offset = it[i]->second;
      if (code_info.IsBitTableDeduped(i)) {
        DCHECK_NE(bit_offset, 0u);
        writer_.WriteVarint(writer_.NumberOfWrittenBits() - bit_offset);
      } else {
        bit_offset = writer_.NumberOfWrittenBits();  // Store offset in dedup map.
        writer_.WriteRegion(it[i]->first);
      }
    }
  });

  if (kIsDebugBuild) {
    CodeInfo old_code_info(code_info_data);
    CodeInfo new_code_info(writer_.data() + deduped_offset);
    ForEachHeaderField([&old_code_info, &new_code_info](size_t, auto member_pointer) {
      if (member_pointer != &CodeInfo::bit_table_flags_) {  // Expected to differ.
        DCHECK_EQ(old_code_info.*member_pointer, new_code_info.*member_pointer);
      }
    });
    ForEachBitTableField([&old_code_info, &new_code_info](size_t, auto member_pointer) {
      DCHECK((old_code_info.*member_pointer).Equals(new_code_info.*member_pointer));
    });
  }

  return deduped_offset;
}

BitTable<StackMap>::const_iterator CodeInfo::BinarySearchNativePc(uint32_t packed_pc) const {
  return std::partition_point(
      stack_maps_.begin(),
      stack_maps_.end(),
      [packed_pc](const StackMap& sm) {
        return sm.GetPackedNativePc() < packed_pc && sm.GetKind() != StackMap::Kind::Catch;
      });
}

StackMap CodeInfo::GetStackMapForNativePcOffset(uint32_t pc, InstructionSet isa) const {
  auto it = BinarySearchNativePc(StackMap::PackNativePc(pc, isa));
  // Start at the lower bound and iterate over all stack maps with the given native pc.
  for (; it != stack_maps_.end() && (*it).GetNativePcOffset(isa) == pc; ++it) {
    StackMap::Kind kind = static_cast<StackMap::Kind>((*it).GetKind());
    if (kind == StackMap::Kind::Default || kind == StackMap::Kind::OSR) {
      return *it;
    }
  }
  return stack_maps_.GetInvalidRow();
}

// Scan backward to determine dex register locations at given stack map.
// All registers for a stack map are combined - inlined registers are just appended,
// therefore 'first_dex_register' allows us to select a sub-range to decode.
void CodeInfo::DecodeDexRegisterMap(uint32_t stack_map_index,
                                    uint32_t first_dex_register,
                                    /*out*/ DexRegisterMap* map) const {
  // Count remaining work so we know when we have finished.
  uint32_t remaining_registers = map->size();

  // Keep scanning backwards and collect the most recent location of each register.
  for (int32_t s = stack_map_index; s >= 0 && remaining_registers != 0; s--) {
    StackMap stack_map = GetStackMapAt(s);
    DCHECK_LE(stack_map_index - s, kMaxDexRegisterMapSearchDistance) << "Unbounded search";

    // The mask specifies which registers where modified in this stack map.
    // NB: the mask can be shorter than expected if trailing zero bits were removed.
    uint32_t mask_index = stack_map.GetDexRegisterMaskIndex();
    if (mask_index == StackMap::kNoValue) {
      continue;  // Nothing changed at this stack map.
    }
    BitMemoryRegion mask = dex_register_masks_.GetBitMemoryRegion(mask_index);
    if (mask.size_in_bits() <= first_dex_register) {
      continue;  // Nothing changed after the first register we are interested in.
    }

    // The map stores one catalogue index per each modified register location.
    uint32_t map_index = stack_map.GetDexRegisterMapIndex();
    DCHECK_NE(map_index, StackMap::kNoValue);

    // Skip initial registers which we are not interested in (to get to inlined registers).
    map_index += mask.PopCount(0, first_dex_register);
    mask = mask.Subregion(first_dex_register, mask.size_in_bits() - first_dex_register);

    // Update registers that we see for first time (i.e. most recent value).
    DexRegisterLocation* regs = map->data();
    const uint32_t end = std::min<uint32_t>(map->size(), mask.size_in_bits());
    const size_t kNumBits = BitSizeOf<uint32_t>();
    for (uint32_t reg = 0; reg < end; reg += kNumBits) {
      // Process the mask in chunks of kNumBits for performance.
      uint32_t bits = mask.LoadBits(reg, std::min<uint32_t>(end - reg, kNumBits));
      while (bits != 0) {
        uint32_t bit = CTZ(bits);
        if (regs[reg + bit].GetKind() == DexRegisterLocation::Kind::kInvalid) {
          regs[reg + bit] = GetDexRegisterCatalogEntry(dex_register_maps_.Get(map_index));
          remaining_registers--;
        }
        map_index++;
        bits ^= 1u << bit;  // Clear the bit.
      }
    }
  }

  // Set any remaining registers to None (which is the default state at first stack map).
  if (remaining_registers != 0) {
    DexRegisterLocation* regs = map->data();
    for (uint32_t r = 0; r < map->size(); r++) {
      if (regs[r].GetKind() == DexRegisterLocation::Kind::kInvalid) {
        regs[r] = DexRegisterLocation::None();
      }
    }
  }
}

// Decode the CodeInfo while collecting size statistics.
void CodeInfo::CollectSizeStats(const uint8_t* code_info_data, /*out*/ Stats* parent) {
  Stats* codeinfo_stats = parent->Child("CodeInfo");
  BitMemoryReader reader(code_info_data);
  CodeInfo code_info;  // Temporary storage for decoded tables.
  ForEachHeaderField([&reader, &code_info](size_t, auto member_pointer) {
    code_info.*member_pointer = reader.ReadVarint();
  });
  codeinfo_stats->Child("Header")->AddBits(reader.NumberOfReadBits());
  ForEachBitTableField([codeinfo_stats, &reader, &code_info](size_t i, auto member_pointer) {
    auto& table = code_info.*member_pointer;
    size_t bit_offset = reader.NumberOfReadBits();
    if (code_info.HasBitTable(i)) {
      if (code_info.IsBitTableDeduped(i)) {
        reader.ReadVarint();
        codeinfo_stats->Child("DedupeOffset")->AddBits(reader.NumberOfReadBits() - bit_offset);
      } else {
        table.Decode(reader);
        Stats* table_stats = codeinfo_stats->Child(table.GetName());
        table_stats->AddBits(reader.NumberOfReadBits() - bit_offset);
        const char* const* column_names = table.GetColumnNames();
        for (size_t c = 0; c < table.NumColumns(); c++) {
          if (table.NumColumnBits(c) > 0) {
            Stats* column_stats = table_stats->Child(column_names[c]);
            column_stats->AddBits(table.NumRows() * table.NumColumnBits(c), table.NumRows());
          }
        }
      }
    }
  });
  codeinfo_stats->AddBytes(BitsToBytesRoundUp(reader.NumberOfReadBits()));
}

void DexRegisterMap::Dump(VariableIndentationOutputStream* vios) const {
  if (HasAnyLiveDexRegisters()) {
    ScopedIndentation indent1(vios);
    for (size_t i = 0; i < size(); ++i) {
      DexRegisterLocation reg = (*this)[i];
      if (reg.IsLive()) {
        vios->Stream() << "v" << i << ":" << reg << " ";
      }
    }
    vios->Stream() << "\n";
  }
}

void CodeInfo::Dump(VariableIndentationOutputStream* vios,
                    uint32_t code_offset,
                    bool verbose,
                    InstructionSet instruction_set) const {
  vios->Stream() << "CodeInfo BitSize=" << size_in_bits_
    << " FrameSize:" << packed_frame_size_ * kStackAlignment
    << " CoreSpillMask:" << std::hex << core_spill_mask_
    << " FpSpillMask:" << std::hex << fp_spill_mask_
    << " NumberOfDexRegisters:" << std::dec << number_of_dex_registers_
    << "\n";
  ScopedIndentation indent1(vios);
  ForEachBitTableField([this, &vios, verbose](size_t, auto member_pointer) {
    const auto& table = this->*member_pointer;
    if (table.NumRows() != 0) {
      vios->Stream() << table.GetName() << " BitSize=" << table.DataBitSize();
      vios->Stream() << " Rows=" << table.NumRows() << " Bits={";
      const char* const* column_names = table.GetColumnNames();
      for (size_t c = 0; c < table.NumColumns(); c++) {
        vios->Stream() << (c != 0 ? " " : "");
        vios->Stream() << column_names[c] << "=" << table.NumColumnBits(c);
      }
      vios->Stream() << "}\n";
      if (verbose) {
        ScopedIndentation indent1(vios);
        for (size_t r = 0; r < table.NumRows(); r++) {
          vios->Stream() << "[" << std::right << std::setw(3) << r << "]={";
          for (size_t c = 0; c < table.NumColumns(); c++) {
            vios->Stream() << (c != 0 ? " " : "");
            if (&table == static_cast<const void*>(&stack_masks_) ||
                &table == static_cast<const void*>(&dex_register_masks_)) {
              BitMemoryRegion bits = table.GetBitMemoryRegion(r, c);
              for (size_t b = 0, e = bits.size_in_bits(); b < e; b++) {
                vios->Stream() << bits.LoadBit(e - b - 1);
              }
            } else {
              vios->Stream() << std::right << std::setw(8) << static_cast<int32_t>(table.Get(r, c));
            }
          }
          vios->Stream() << "}\n";
        }
      }
    }
  });

  // Display stack maps along with (live) Dex register maps.
  if (verbose) {
    for (StackMap stack_map : stack_maps_) {
      stack_map.Dump(vios, *this, code_offset, instruction_set);
    }
  }
}

void StackMap::Dump(VariableIndentationOutputStream* vios,
                    const CodeInfo& code_info,
                    uint32_t code_offset,
                    InstructionSet instruction_set) const {
  const uint32_t pc_offset = GetNativePcOffset(instruction_set);
  vios->Stream()
      << "StackMap[" << Row() << "]"
      << std::hex
      << " (native_pc=0x" << code_offset + pc_offset
      << ", dex_pc=0x" << GetDexPc()
      << ", register_mask=0x" << code_info.GetRegisterMaskOf(*this)
      << std::dec
      << ", stack_mask=0b";
  BitMemoryRegion stack_mask = code_info.GetStackMaskOf(*this);
  for (size_t i = 0, e = stack_mask.size_in_bits(); i < e; ++i) {
    vios->Stream() << stack_mask.LoadBit(e - i - 1);
  }
  vios->Stream() << ")\n";
  code_info.GetDexRegisterMapOf(*this).Dump(vios);
  for (InlineInfo inline_info : code_info.GetInlineInfosOf(*this)) {
    inline_info.Dump(vios, code_info, *this);
  }
}

void InlineInfo::Dump(VariableIndentationOutputStream* vios,
                      const CodeInfo& code_info,
                      const StackMap& stack_map) const {
  uint32_t depth = Row() - stack_map.GetInlineInfoIndex();
  vios->Stream()
      << "InlineInfo[" << Row() << "]"
      << " (depth=" << depth
      << std::hex
      << ", dex_pc=0x" << GetDexPc();
  if (EncodesArtMethod()) {
    ScopedObjectAccess soa(Thread::Current());
    vios->Stream() << ", method=" << GetArtMethod()->PrettyMethod();
  } else {
    vios->Stream()
        << std::dec
        << ", method_index=" << code_info.GetMethodIndexOf(*this);
  }
  vios->Stream() << ")\n";
  code_info.GetInlineDexRegisterMapOf(stack_map, *this).Dump(vios);
}

}  // namespace art
