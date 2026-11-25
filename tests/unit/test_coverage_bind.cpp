// SPDX-License-Identifier: Apache-2.0
// Coverage of the column frame readers (engine/bind.cpp): frames built byte by byte, as Python's sender writes them,
// with the column types the encoder never makes (Int64), validity bitmaps, and every way a frame can be malformed.
// Both readers are run on each frame: DecodeColumnFrame (bound tables) and DecodeRawFrame (plots).
#include <catch2/catch_test_macros.hpp>

#include "engine/bind.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using yc::core::Value;
using yc::engine::ColType;
using yc::engine::ColumnDesc;
using yc::engine::ColumnFrame;
using yc::engine::FrameHeader;
using yc::engine::FrameOp;
using yc::engine::RawFrame;

namespace {

// a frame written field by field, so a test can put any value in any field
struct Builder {
  struct Col {
    std::string name;
    ColType type = ColType::Float64;
    std::vector<uint8_t> values;
    std::vector<int32_t> offsets;  // Utf8
    std::vector<uint8_t> validity;  // empty: every row has a value
  };
  std::string table = "T";
  uint16_t op = 2;
  uint32_t nrows = 0;
  uint32_t key_col = 0xFFFFFFFFu;
  int64_t sent_ns = 0;
  std::vector<Col> cols;

  Col& Add(std::string name, ColType type) {
    cols.push_back({std::move(name), type, {}, {}, {}});
    return cols.back();
  }
  static void PutBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
  }

  // the bytes, and where each column's descriptor sits (to break it afterwards)
  std::vector<uint8_t> Build() const {
    const uint32_t nc = static_cast<uint32_t>(cols.size());
    std::vector<uint8_t> out(sizeof(FrameHeader) + nc * sizeof(ColumnDesc), 0);
    auto put = [&](const void* p, size_t n) {
      while (out.size() % 8) out.push_back(0);
      const uint64_t off = out.size();
      PutBytes(out, p, n);
      return off;
    };
    FrameHeader h{};
    h.magic = yc::engine::kFrameMagic;
    h.version = yc::engine::kFrameVersion;
    h.op = op;
    h.ncols = nc;
    h.nrows = nrows;
    h.key_col = key_col;
    h.sent_ns = sent_ns;
    h.name_off = static_cast<uint32_t>(put(table.data(), table.size()));
    h.name_len = static_cast<uint32_t>(table.size());
    std::vector<ColumnDesc> ds(nc);
    for (uint32_t c = 0; c < nc; ++c) {
      ColumnDesc d{};
      d.name_off = static_cast<uint32_t>(put(cols[c].name.data(), cols[c].name.size()));
      d.name_len = static_cast<uint32_t>(cols[c].name.size());
      d.type = static_cast<uint8_t>(cols[c].type);
      if (!cols[c].offsets.empty()) d.offsets_off = put(cols[c].offsets.data(), cols[c].offsets.size() * 4);
      d.values_off = put(cols[c].values.data(), cols[c].values.size());
      d.values_len = cols[c].values.size();
      if (!cols[c].validity.empty()) d.validity_off = put(cols[c].validity.data(), cols[c].validity.size());
      ds[c] = d;
    }
    while (out.size() % 8) out.push_back(0);
    h.total_len = out.size();
    std::memcpy(out.data(), &h, sizeof h);
    for (uint32_t c = 0; c < nc; ++c) std::memcpy(out.data() + sizeof(FrameHeader) + c * sizeof(ColumnDesc), &ds[c], sizeof(ColumnDesc));
    return out;
  }
};

std::vector<uint8_t> F64(std::initializer_list<double> xs) {
  std::vector<uint8_t> b;
  for (double x : xs) Builder::PutBytes(b, &x, 8);
  return b;
}
std::vector<uint8_t> I64(std::initializer_list<int64_t> xs) {
  std::vector<uint8_t> b;
  for (int64_t x : xs) Builder::PutBytes(b, &x, 8);
  return b;
}

FrameHeader HeaderOf(const std::vector<uint8_t>& b) {
  FrameHeader h;
  std::memcpy(&h, b.data(), sizeof h);
  return h;
}
void SetHeader(std::vector<uint8_t>& b, const FrameHeader& h) { std::memcpy(b.data(), &h, sizeof h); }
ColumnDesc DescOf(const std::vector<uint8_t>& b, uint32_t c) {
  ColumnDesc d;
  std::memcpy(&d, b.data() + sizeof(FrameHeader) + c * sizeof(ColumnDesc), sizeof d);
  return d;
}
void SetDesc(std::vector<uint8_t>& b, uint32_t c, const ColumnDesc& d) {
  std::memcpy(b.data() + sizeof(FrameHeader) + c * sizeof(ColumnDesc), &d, sizeof d);
}

std::string ColumnError(const std::vector<uint8_t>& b) {
  ColumnFrame f;
  std::string err;
  return yc::engine::DecodeColumnFrame(b.data(), b.size(), &f, &err) ? std::string() : err;
}
std::string RawError(const std::vector<uint8_t>& b) {
  RawFrame f;
  std::string err;
  return yc::engine::DecodeRawFrame(b.data(), b.size(), &f, &err) ? std::string() : err;
}

// a frame with every kind of column: 3 rows, the middle one missing in some
Builder Mixed() {
  Builder b;
  b.table = "Quotes";
  b.op = 1;
  b.nrows = 3;
  b.key_col = 0;
  b.sent_ns = 99;
  b.Add("Sym", ColType::Utf8).offsets = {0, 4, 4, 7};
  b.cols.back().values = {'T', 'X', 'F', '1', 'A', 'B', 'C'};
  b.cols.back().validity = {0b101};  // row 2 missing
  b.Add("Px", ColType::Float64).values = F64({1.5, std::nan(""), 3.25});  // NaN: no value
  b.Add("Qty", ColType::Int64).values = I64({7, -2, 9000000000LL});
  b.cols.back().validity = {0b011};  // row 3 missing
  b.Add("Live", ColType::Bool).values = {0b001};
  b.cols.back().validity = {0b110};  // row 1 missing
  b.Add("Gap", ColType::Null);
  return b;
}

}  // namespace

TEST_CASE("Column frames: Int64, validity bitmaps, NaN and a Null column, read for a table and for a plot") {
  const auto bytes = Mixed().Build();
  ColumnFrame f;
  std::string err;
  REQUIRE(yc::engine::DecodeColumnFrame(bytes.data(), bytes.size(), &f, &err));
  CHECK(f.table == "Quotes");
  CHECK(f.op == FrameOp::Upsert);
  CHECK(f.key_col == 0);
  CHECK(f.sent_ns == 99);
  REQUIRE(f.cols.size() == 5);
  CHECK(f.cols[0][0].str() == "TXF1");
  CHECK(f.cols[0][1].IsEmpty());  // invalid in the bitmap, though its offsets are fine
  CHECK(f.cols[0][2].str() == "ABC");
  CHECK(f.cols[1][0].AsDouble() == 1.5);
  CHECK(f.cols[1][1].IsEmpty());
  CHECK(f.cols[2][1].AsDouble() == -2);
  CHECK(f.cols[2][0].AsDouble() == 7);
  CHECK(f.cols[2][2].IsEmpty());
  CHECK(f.cols[3][0].IsEmpty());
  CHECK_FALSE(f.cols[3][1].boolean());
  CHECK_FALSE(f.cols[3][2].boolean());
  CHECK(f.cols[4].size() == 3);
  CHECK(f.cols[4][0].IsEmpty());

  RawFrame r;
  REQUIRE(yc::engine::DecodeRawFrame(bytes.data(), bytes.size(), &r, &err));
  CHECK(r.table == "Quotes");
  CHECK(r.op == FrameOp::Upsert);
  CHECK(r.sent_ns == 99);
  CHECK(r.nrows == 3);
  REQUIRE(r.Text("Sym") != nullptr);
  CHECK((*r.Text("Sym"))[0] == "TXF1");
  CHECK((*r.Text("Sym"))[1].empty());
  CHECK(r.Num("Sym") == nullptr);  // a text column has no numbers
  REQUIRE(r.Num("Px") != nullptr);
  CHECK((*r.Num("Px"))[2] == 3.25);
  CHECK(std::isnan((*r.Num("Px"))[1]));
  const auto& qty = *r.Num("Qty");
  CHECK(qty[1] == -2);
  CHECK(qty[0] == 7);
  CHECK(std::isnan(qty[2]));  // invalid in the bitmap
  const auto& live = *r.Num("Live");
  CHECK(std::isnan(live[0]));
  CHECK(live[1] == 0);
  CHECK(live[2] == 0);
  CHECK(r.Num("Gap") != nullptr);  // a Null column reads as all NaN
  CHECK(std::isnan((*r.Num("Gap"))[0]));
  CHECK(r.Text("Px") == nullptr);
  CHECK(r.Num("Nope") == nullptr);
  CHECK(r.Text("Nope") == nullptr);
}

TEST_CASE("Column frames: the encoder picks Null, Bool, Float64 and Utf8, and reads back what it wrote") {
  ColumnFrame f;
  f.op = FrameOp::Append;
  f.table = "A";
  f.nrows = 3;
  f.names = {"none", "flags", "mixed", "text", "err"};
  f.cols = {{Value(), Value(), Value()},
            {Value::Bool(true), Value(), Value::Bool(false)},
            {Value::Number(2), Value::Bool(true), Value()},
            {Value::Number(0.5), Value::String("x"), Value::Number(-3)},
            {Value::Error(yc::core::Err::Div0), Value(), Value()}};
  const auto bytes = yc::engine::EncodeColumnFrame(f);
  CHECK(DescOf(bytes, 0).type == static_cast<uint8_t>(ColType::Null));
  CHECK(DescOf(bytes, 1).type == static_cast<uint8_t>(ColType::Bool));
  CHECK(DescOf(bytes, 2).type == static_cast<uint8_t>(ColType::Float64));
  CHECK(DescOf(bytes, 3).type == static_cast<uint8_t>(ColType::Utf8));
  CHECK(DescOf(bytes, 4).type == static_cast<uint8_t>(ColType::Utf8));
  CHECK(DescOf(bytes, 0).validity_off == 0);  // a Null column needs no bitmap
  CHECK(DescOf(bytes, 1).validity_off != 0);
  ColumnFrame g;
  std::string err;
  REQUIRE(yc::engine::DecodeColumnFrame(bytes.data(), bytes.size(), &g, &err));
  CHECK(g.op == FrameOp::Append);
  CHECK(g.key_col == -1);
  CHECK(g.cols[1][0].boolean());
  CHECK(g.cols[1][1].IsEmpty());
  CHECK(g.cols[2][1].AsDouble() == 1);  // a boolean among numbers is sent as 1
  CHECK(g.cols[2][2].IsEmpty());
  CHECK(g.cols[3][0].str() == "0.5");  // numbers in a text column as their shortest text
  CHECK(g.cols[3][2].str() == "-3");
  CHECK(g.cols[4][1].IsEmpty());
  // fewer names than columns: the rest are nameless
  f.names.resize(2);
  const auto nameless = yc::engine::EncodeColumnFrame(f);
  REQUIRE(yc::engine::DecodeColumnFrame(nameless.data(), nameless.size(), &g, &err));
  CHECK(g.names[3].empty());
  // a column shorter than nrows: the missing rows have no value
  f.cols[2].resize(1);
  const auto short_col = yc::engine::EncodeColumnFrame(f);
  REQUIRE(yc::engine::DecodeColumnFrame(short_col.data(), short_col.size(), &g, &err));
  CHECK(g.cols[2][0].AsDouble() == 2);
  CHECK(g.cols[2][2].IsEmpty());
}

TEST_CASE("Column frames: key texts of numbers, booleans and blanks") {
  CHECK(yc::engine::BindKeyText(Value::Number(-12)) == "-12");
  CHECK(yc::engine::BindKeyText(Value::Number(1e16)) == "1e+16");  // past exact integers: the shortest form
  CHECK(yc::engine::BindKeyText(Value::Number(2.5)) == "2.5");
  CHECK(yc::engine::BindKeyText(Value::Bool(true)) == "TRUE");
  CHECK(yc::engine::BindKeyText(Value::Bool(false)) == "FALSE");
  CHECK(yc::engine::BindKeyText(Value()).empty());
  const int64_t a = yc::engine::PerfCounterNs(), b = yc::engine::PerfCounterNs();
  CHECK(a > 0);
  CHECK(b >= a);
}

TEST_CASE("Column frames: every malformed header and column is refused by both readers, never read past") {
  const auto good = Mixed().Build();
  REQUIRE(ColumnError(good).empty());
  REQUIRE(RawError(good).empty());

  SECTION("shorter than the header, a bad magic number or version, longer than sent") {
    std::vector<uint8_t> tiny(good.begin(), good.begin() + 10);
    CHECK(ColumnError(tiny) == "The frame is shorter than its header");
    CHECK(RawError(tiny) == "The frame is shorter than its header");
    auto b = good;
    auto h = HeaderOf(b);
    h.version = 2;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "Unsupported column frame version");
    CHECK(RawError(b) == "Not a column frame");
    b = good;
    h = HeaderOf(b);
    h.total_len = b.size() + 8;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "The frame is longer than the data that was sent");
    CHECK(RawError(b) == "The frame is longer than the data that was sent");
  }
  SECTION("columns: too many, descriptors past the end") {
    auto b = good;
    auto h = HeaderOf(b);
    h.ncols = 5000;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "Too many columns in one frame");
    CHECK(RawError(b) == "Bad column count");
    h.ncols = 1000;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "The column descriptors don't fit in the frame");
    CHECK(RawError(b) == "Bad column count");
  }
  SECTION("operation, table name and key column") {
    auto b = good;
    auto h = HeaderOf(b);
    h.op = 9;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "Unknown frame operation");
    b = good;
    h = HeaderOf(b);
    h.name_len = 0xFFFFFF00u;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "The table name is outside the frame");
    CHECK(RawError(b) == "The name is outside the frame");
    b = good;
    h = HeaderOf(b);
    h.key_col = 5;
    SetHeader(b, h);
    CHECK(ColumnError(b) == "The key column index is out of range");
  }
  SECTION("a column's name and validity bitmap outside the frame") {
    auto b = good;
    auto d = DescOf(b, 1);
    d.name_off = static_cast<uint32_t>(b.size() + 1);
    SetDesc(b, 1, d);
    CHECK(ColumnError(b) == "A column name is outside the frame");
    CHECK(RawError(b) == "A column name is outside the frame");
    b = good;
    d = DescOf(b, 2);
    d.validity_off = b.size();  // no room for the one byte of bits
    SetDesc(b, 2, d);
    CHECK(ColumnError(b) == "A validity bitmap is outside the frame");
    CHECK(RawError(b) == "A validity bitmap is outside the frame");
  }
  SECTION("number, integer and boolean columns shorter than the rows") {
    auto b = good;
    auto d = DescOf(b, 1);
    d.values_len = 16;  // 3 rows need 24 bytes
    SetDesc(b, 1, d);
    CHECK(ColumnError(b) == "A number column is shorter than the rows");
    b = good;
    d = DescOf(b, 1);
    d.values_off = b.size() - 8;
    SetDesc(b, 1, d);
    CHECK(ColumnError(b) == "A number column is shorter than the rows");
    CHECK(RawError(b) == "A number column is shorter than the rows");
    b = good;
    d = DescOf(b, 2);
    d.values_len = 8;
    SetDesc(b, 2, d);
    CHECK(ColumnError(b) == "An integer column is shorter than the rows");
    b = good;
    d = DescOf(b, 3);
    d.values_off = b.size();
    SetDesc(b, 3, d);
    CHECK(ColumnError(b) == "A boolean column is shorter than the rows");
    CHECK(RawError(b) == "A boolean column is shorter than the rows");
  }
  SECTION("text columns: offsets and bytes outside the frame, offsets out of order") {
    auto b = good;
    auto d = DescOf(b, 0);
    d.offsets_off = b.size() - 4;
    SetDesc(b, 0, d);
    CHECK(ColumnError(b) == "A text column's offsets are outside the frame");
    CHECK(RawError(b) == "A text column is outside the frame");
    b = good;
    d = DescOf(b, 0);
    d.values_len = b.size();
    SetDesc(b, 0, d);
    CHECK(ColumnError(b) == "A text column's bytes are outside the frame");
    CHECK(RawError(b) == "A text column is outside the frame");
    for (const auto& offs : std::vector<std::vector<int32_t>>{{0, 4, 2, 7}, {-1, 4, 4, 7}, {0, 4, 4, 70}}) {
      Builder m = Mixed();
      m.cols[0].offsets = offs;
      const auto bad = m.Build();
      CHECK(ColumnError(bad) == "A text column's offsets are out of order");
      CHECK(RawError(bad) == "A text column's offsets are out of order");
    }
  }
  SECTION("an unknown column type: the table reader refuses it, the plot reader leaves it empty") {
    auto b = good;
    auto d = DescOf(b, 4);
    d.type = 42;
    SetDesc(b, 4, d);
    CHECK(ColumnError(b) == "Unknown column type");
    RawFrame r;
    std::string err;
    REQUIRE(yc::engine::DecodeRawFrame(b.data(), b.size(), &r, &err));
    CHECK(std::isnan(r.num[4][0]));
  }
  SECTION("no error string asked for") {
    std::vector<uint8_t> tiny(good.begin(), good.begin() + 10);
    ColumnFrame f;
    RawFrame r;
    CHECK_FALSE(yc::engine::DecodeColumnFrame(tiny.data(), tiny.size(), &f, nullptr));
    CHECK_FALSE(yc::engine::DecodeRawFrame(tiny.data(), tiny.size(), &r, nullptr));
  }
}
