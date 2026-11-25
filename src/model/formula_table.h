// SPDX-License-Identifier: Apache-2.0
// formula_table.h - formula definition table (PLAN §6.3)
//
// Shared formulas: cells with the same R1C1 text share one FormulaDef (MetaTile only stores a 4-byte id).
// The table only grows and is stored in blocks: adding only COWs the last block, publishing a snapshot only copies the block pointers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yc::model {

struct FormulaDef {
  virtual ~FormulaDef() = default;
  std::string key;  // normalized R1C1 text (dedup key of shared formulas)
  enum Flags : uint32_t {
    kVolatile = 1u << 0,     // NOW, RAND, OFFSET, INDIRECT...
    kDynamicRef = 1u << 1,   // OFFSET, INDIRECT
    kUdf = 1u << 2,          // calls a Python UDF
    kStream = 1u << 3,       // stream (QUOTE / STREAM)
    kParseError = 1u << 4,   // syntax error (should never be in the table, reserved)
    kChanging = 1u << 5,     // NOW, RAND...: another answer at every calculation (calculated once in each)
  };
  uint32_t flags = 0;
};

class FormulaTable {
 public:
  static constexpr uint32_t kChunk = 4096;

  const FormulaDef* Get(uint32_t id) const {
    if (id == 0) return nullptr;
    const uint32_t i = id - 1;
    const uint32_t ci = i / kChunk;
    if (ci >= chunks_.size()) return nullptr;
    const auto& c = *chunks_[ci];
    return (i % kChunk) < c.size() ? c[i % kChunk].get() : nullptr;
  }
  std::shared_ptr<const FormulaDef> Share(uint32_t id) const {
    if (id == 0) return nullptr;
    const uint32_t i = id - 1;
    const uint32_t ci = i / kChunk;
    if (ci >= chunks_.size()) return nullptr;
    const auto& c = *chunks_[ci];
    return (i % kChunk) < c.size() ? c[i % kChunk] : nullptr;
  }

  uint32_t Append(std::shared_ptr<const FormulaDef> def) {
    if (chunks_.empty() || chunks_.back()->size() >= kChunk) {
      chunks_.push_back(std::make_shared<std::vector<std::shared_ptr<const FormulaDef>>>());
      chunks_.back()->reserve(kChunk);
    } else if (chunks_.back().use_count() > 1) {
      chunks_.back() = std::make_shared<std::vector<std::shared_ptr<const FormulaDef>>>(*chunks_.back());
    }
    chunks_.back()->push_back(std::move(def));
    return ++count_;
  }
  uint32_t size() const { return count_; }

 private:
  std::vector<std::shared_ptr<std::vector<std::shared_ptr<const FormulaDef>>>> chunks_;
  uint32_t count_ = 0;
};

}  // namespace yc::model
