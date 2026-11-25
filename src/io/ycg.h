// SPDX-License-Identifier: Apache-2.0
// ycg.h - .ycg file format (PLAN §15: backward compatible, upgradable)
//
// 48-byte header (little-endian):
//   magic "YCG1" | u16 format_major | u16 format_minor | u16 min_reader_major | u16 min_reader_minor
//   | u64 required_features | u64 optional_features | u32 chunk_count | u64 dir_offset | u32 reserved | u32 header_crc32c
// followed by the contents of every chunk (each compressed separately), then the chunk directory (40 bytes per entry) + the directory's crc32c.
//
// Every chunk has its own schema version and flags:
//   critical            can't be read -> open read-only (says a newer YCGrid is needed)
//   preserve_if_unknown can't be read -> keep it as is and write it back on save
// Metadata chunks are integer-keyed msgpack maps: fields are only added, numbers are never reused; readers ignore unknown fields.
// Normative spec: docs/ycg-format.md.
#pragma once

#include "model/workbook.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yc::io {

inline constexpr uint16_t kFormatMajor = 1;
inline constexpr uint16_t kFormatMinor = 0;

enum ChunkFlags : uint16_t { kChunkCritical = 1, kChunkPreserve = 2 };

constexpr uint32_t FourCC(const char (&s)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

struct SaveOptions {
  std::string layout_json;  // the UI layout (LAYOUT chunk); empty = keep the one in the workbook
  bool compress = true;
  std::string app_version;
};

struct LoadResult {
  bool ok = false;
  std::string error;
  std::vector<std::string> warnings;
  model::Workbook wb;
  bool upgraded = false;  // upgraded from an older format version (asks and keeps a .bak before the first save)
  bool needs_password = false;  // an encrypted workbook and no password given
  bool wrong_password = false;  // the password didn't decrypt it
};

std::string SaveYcgToBytes(const model::Workbook& wb, const SaveOptions& opt);
// password: for encrypted workbooks (io/crypto.h); the loaded workbook keeps it for the next save
LoadResult LoadYcgFromBytes(std::string_view bytes, const std::string& password = {});

// write to a temporary file, then replace (a crash while saving never damages the original)
bool SaveYcg(const model::Workbook& wb, const std::wstring& path, const SaveOptions& opt, std::string* error);
LoadResult LoadYcg(const std::wstring& path, const std::string& password = {});

// read a whole file (UTF-16 path)
bool ReadFileBytes(const std::wstring& path, std::string* out);
bool WriteFileAtomic(const std::wstring& path, std::string_view data, std::string* error);

// pieces of the format other code can reuse: a cell value and a style as msgpack
class MsgWriter;
class MsgReader;
void WriteCellValue(MsgWriter& w, const core::Value& v);
bool ReadCellValue(MsgReader& r, core::Value* v);
void WriteCellStyle(MsgWriter& w, const model::Style& s, const model::StyleTable& t);
bool ReadCellStyle(MsgReader& r, model::Style* s, model::StyleTable& t);

}  // namespace yc::io
