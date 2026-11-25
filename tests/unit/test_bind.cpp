// SPDX-License-Identifier: Apache-2.0
// [YC] bound tables: column frames by key (engine/bind.h, document_bind.cpp)
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/bind.h"
#include "engine/tape.h"
#include "io/ycg.h"
#include "model/table.h"

#include <cstdio>
#include <filesystem>

using yc::core::Value;
using yc::engine::ColumnFrame;
using yc::engine::FrameOp;

namespace {

ColumnFrame Frame(FrameOp op, std::vector<std::string> names, std::vector<std::vector<Value>> cols, int key = 0) {
  ColumnFrame f;
  f.op = op;
  f.table = "Chain";
  f.key_col = key;
  f.names = std::move(names);
  f.cols = std::move(cols);
  f.nrows = f.cols.empty() ? 0 : static_cast<uint32_t>(f.cols[0].size());
  return f;
}

Value N(double x) { return Value::Number(x); }
Value S(const char* s) { return Value::String(s); }

// encodes and decodes, so every test also checks the wire format
bool Apply(yctest::Doc& t, const ColumnFrame& f, std::string* err = nullptr, yc::engine::Document::BindResult* res = nullptr) {
  const auto bytes = yc::engine::EncodeColumnFrame(f);
  ColumnFrame g;
  std::string e;
  REQUIRE(yc::engine::DecodeColumnFrame(bytes.data(), bytes.size(), &g, &e));
  const bool ok = t.d.ApplyColumnFrame(g, err ? err : &e, res);
  t.d.Recalc();
  return ok;
}

void MakeChain(yctest::Doc& t) {
  yc::model::BoundTable def;
  def.name = "Chain";
  def.sheet = t.sheet();
  def.key = "Strike";
  yc::model::BoundColumn bid;
  bid.name = "Bid";
  bid.number_format = "0.00";
  def.columns = {bid};
  std::string err;
  REQUIRE(t.d.CreateBoundTable(def, 0, 0, {"Bid", "Ask"}, &err));
}

}  // namespace

TEST_CASE("column frames: encode and decode, and malformed frames are refused") {
  ColumnFrame f = Frame(FrameOp::Upsert, {"Strike", "Bid", "Ok", "Note"},
                        {{N(23000), N(23100)}, {N(1.5), Value()}, {Value::Bool(true), Value::Bool(false)}, {S("a"), S("\xe4\xb8\xad")}});
  f.sent_ns = 12345;
  const auto bytes = yc::engine::EncodeColumnFrame(f);
  CHECK(bytes.size() % 8 == 0);
  ColumnFrame g;
  std::string err;
  REQUIRE(yc::engine::DecodeColumnFrame(bytes.data(), bytes.size(), &g, &err));
  CHECK(g.table == "Chain");
  CHECK(g.nrows == 2);
  CHECK(g.sent_ns == 12345);
  CHECK(g.names[3] == "Note");
  CHECK(g.cols[0][1].AsDouble() == 23100);
  CHECK(g.cols[1][0].AsDouble() == 1.5);
  CHECK(g.cols[1][1].IsEmpty());
  CHECK(g.cols[2][0].boolean());
  CHECK_FALSE(g.cols[2][1].boolean());
  CHECK(g.cols[3][1].str() == "\xe4\xb8\xad");
  // truncated, bad magic, offsets outside the frame
  CHECK_FALSE(yc::engine::DecodeColumnFrame(bytes.data(), 40, &g, &err));
  auto bad = bytes;
  bad[0] = 'X';
  CHECK_FALSE(yc::engine::DecodeColumnFrame(bad.data(), bad.size(), &g, &err));
  auto shortened = bytes;
  shortened.resize(bytes.size() - 16);
  CHECK_FALSE(yc::engine::DecodeColumnFrame(shortened.data(), shortened.size(), &g, &err));
  CHECK_FALSE(err.empty());
  CHECK(yc::engine::BindKeyText(N(23000)) == "23000");
  CHECK(yc::engine::BindKeyText(N(0.1)) == "0.1");
  CHECK(yc::engine::BindKeyText(S("TXO")) == "TXO");
}

TEST_CASE("bound tables: upsert by key, new keys, formats, formulas next to the data") {
  yctest::Doc t;
  MakeChain(t);
  const auto* tab = t.d.TableAt(t.sheet(), 0, 0);
  REQUIRE(tab != nullptr);
  CHECK(tab->name == "Chain");
  CHECK(t.T("A1") == "Strike");
  CHECK(t.T("C1") == "Ask");
  yc::engine::Document::BindResult res;
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid", "Ask"}, {{N(23000), N(23100), N(23200)}, {N(10), N(8), N(6)}, {N(11), N(9), N(7)}}), nullptr, &res));
  CHECK(res.added == 3);
  CHECK(res.rows == 3);
  CHECK(t.N("A2") == 23000);
  CHECK(t.N("B4") == 6);
  CHECK(t.T("B2") == "10.00");  // the Bid column's format
  CHECK(t.d.TableAt(t.sheet(), 0, 0)->range == yc::core::Rect{0, 0, 3, 2});
  // a formula over the table follows the data
  t.Set("F1", "=SUM(Chain[Bid])");
  CHECK(t.N("F1") == 24);
  // only changed keys and columns
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Ask"}, {{N(23100)}, {N(9.5)}}), nullptr, &res));
  CHECK(res.added == 0);
  CHECK(t.N("C3") == 9.5);
  CHECK(t.N("B3") == 8);
  // a new key in the middle of updates goes to the end; a new column is added
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid", "IV"}, {{N(22900), N(23000)}, {N(12), N(10.5)}, {N(0.21), N(0.2)}})));
  CHECK(t.N("A5") == 22900);
  CHECK(t.N("B5") == 12);
  CHECK(t.N("B2") == 10.5);
  CHECK(t.T("D1") == "IV");
  CHECK(t.N("D2") == 0.2);
  CHECK(t.N("F1") == 36.5);
  // the newest send time reaches the snapshot fields
  ColumnFrame f = Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(23000)}, {N(11)}});
  f.sent_ns = 777;
  REQUIRE(Apply(t, f));
  int64_t newest = 0, oldest = 0;
  t.d.TakeInputTimes(&newest, &oldest);
  CHECK(newest == 777);
  CHECK(t.d.BoundRowTimes("chain")->t[0].load() > 0);
}

TEST_CASE("bound tables: the user sorts the table, keys are found again; delete and replace") {
  yctest::Doc t;
  MakeChain(t);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2), N(3)}, {N(10), N(20), N(30)}})));
  // the user sorts by Bid descending: rows move
  yc::engine::SortKey key;
  key.col = 1;
  key.descending = true;
  t.d.Sort(t.sheet(), yc::core::Rect{1, 0, 3, 2}, {key}, false);
  t.d.Recalc();
  CHECK(t.N("A2") == 3);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1)}, {N(11)}})));
  CHECK(t.N("A4") == 1);
  CHECK(t.N("B4") == 11);
  CHECK(t.N("B2") == 30);
  // delete keys: the rows go, the table shrinks
  yc::engine::Document::BindResult res;
  REQUIRE(Apply(t, Frame(FrameOp::Delete, {"Strike"}, {{N(2), N(99)}}), nullptr, &res));
  CHECK(res.removed == 1);
  CHECK(t.d.TableAt(t.sheet(), 0, 0)->range.r1 == 2);
  CHECK(t.N("A3") == 1);
  // replace: exactly these rows
  REQUIRE(Apply(t, Frame(FrameOp::Replace, {"Strike", "Bid"}, {{N(7), N(8), N(9), N(10)}, {N(1), N(2), N(3), N(4)}})));
  CHECK(t.d.TableAt(t.sheet(), 0, 0)->range.r1 == 4);
  CHECK(t.N("A2") == 7);
  CHECK(t.N("B5") == 4);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(9)}, {N(33)}})));
  CHECK(t.N("B4") == 33);
  // errors: unknown table, updates without the key
  std::string err;
  ColumnFrame f = Frame(FrameOp::Upsert, {"Strike"}, {{N(1)}});
  f.table = "Nope";
  CHECK_FALSE(t.d.ApplyColumnFrame(f, &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(Apply(t, Frame(FrameOp::Upsert, {"Bid"}, {{N(1)}}, -1), &err));
}

TEST_CASE("bound tables: cells below move down when the table grows; saved and read back") {
  yctest::Doc t;
  MakeChain(t);
  t.Set("A5", "below");
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2), N(3), N(4)}, {N(1), N(2), N(3), N(4)}})));
  CHECK(t.N("A5") == 4);
  CHECK(t.T("A8") == "below");  // like Excel: the gap below the table stays
  // the definition is saved (BIND chunk); the data are cells
  yc::model::BoundTable def = *t.d.FindBoundTable("chain");
  def.stale_after = 2.5;
  def.columns[0].flash = 1;
  REQUIRE(t.d.UpdateBoundTable("Chain", [&](yc::model::BoundTable& b) {
    b.stale_after = 2.5;
    b.columns[0].flash = 1;
  }, nullptr));
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  REQUIRE(r.wb.bound_tables->size() == 1);
  CHECK(r.wb.bound_tables->front() == *t.d.FindBoundTable("Chain"));
  CHECK(r.wb.bound_tables->front().stale_after == 2.5);
  CHECK(r.wb.bound_tables->front().record);  // the default isn't written
  // record = false (the app's own tables, not replayed) is saved
  REQUIRE(t.d.UpdateBoundTable("Chain", [&](yc::model::BoundTable& b) { b.record = false; }, nullptr));
  auto r2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r2.ok);
  CHECK_FALSE(r2.wb.bound_tables->front().record);
  CHECK(r2.wb.bound_tables->front() == *t.d.FindBoundTable("Chain"));
  // column renderers (bars, trends) are saved as extra fields of the column (older readers skip them)
  REQUIRE(t.d.UpdateBoundTable("Chain", [&](yc::model::BoundTable& b) {
    b.columns[0].render = 5;
    b.columns[0].render_color = 0xFF00C8FF;
    b.columns[0].render_neg = 0xFF16A34A;
    b.columns[0].render_max = 250.0;
    b.columns[0].trend_len = 120;
  }, nullptr));
  auto r3 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r3.ok);
  CHECK(r3.wb.bound_tables->front() == *t.d.FindBoundTable("Chain"));
  CHECK(r3.wb.bound_tables->front().columns[0].trend_len == 120);
  CHECK(r3.wb.bound_tables->front().columns[0].render_max == 250.0);
  yc::engine::Document d2(r.wb);
  ColumnFrame f = Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(3)}, {N(30)}});
  std::string err;
  REQUIRE(d2.ApplyColumnFrame(f, &err));
  const auto p = yctest::Doc::P("B4");
  CHECK(d2.GetValue(t.sheet(), p.row, p.col).AsDouble() == 30);
  CHECK(t.d.RemoveBoundTable("Chain", true));
  CHECK(t.d.FindBoundTable("Chain") == nullptr);
  CHECK(t.d.TableAt(t.sheet(), 0, 0) == nullptr);
}

TEST_CASE("number formats: [mul=N] scales the value (basis points)") {
  CHECK(yc::engine::DisplayTextOf(Value::Number(0.00125), "[mul=10000]0.0\" bp\"") == "12.5 bp");
  CHECK(yc::engine::DisplayTextOf(Value::Number(-0.0003), "[mul=10000]0.0") == "-3.0");
}

TEST_CASE("bound tables: change flash direction and the refresh cap") {
  yctest::Doc t;
  yc::model::BoundTable def;
  def.name = "Chain";
  def.sheet = t.sheet();
  def.key = "Strike";
  yc::model::BoundColumn bid;
  bid.name = "Bid";
  bid.flash = 1;
  bid.max_hz = 2;  // at most twice a second on screen
  def.columns = {bid};
  std::string err;
  REQUIRE(t.d.CreateBoundTable(def, 0, 0, {"Bid"}, &err));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2)}, {N(10), N(20)}})));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1)}, {N(9)}})));  // too soon: held back
  CHECK(t.N("B2") == 10);
  CHECK(t.d.BindNextFlush() > 0);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1)}, {N(8)}})));  // the newest held-back value wins
  t.d.BindFlush(yc::engine::PerfCounterNs() + 1000000000LL);
  t.d.Recalc();
  CHECK(t.N("B2") == 8);
  CHECK(t.d.BindNextFlush() == 0);
  const auto times = t.d.BoundRowTimes("Chain");
  REQUIRE(times != nullptr);
  const auto* col = times->ColAt(1);
  REQUIRE(col != nullptr);
  CHECK(col->dir[0].load() == -1);  // 10 -> 8: down
  CHECK(col->t[0].load() > 0);
}

TEST_CASE("bound tables: values held by the refresh cap follow their key when rows move") {
  yctest::Doc t;
  yc::model::BoundTable def;
  def.name = "Chain";
  def.sheet = t.sheet();
  def.key = "Strike";
  yc::model::BoundColumn bid;
  bid.name = "Bid";
  bid.max_hz = 1;
  def.columns = {bid};
  std::string err;
  REQUIRE(t.d.CreateBoundTable(def, 0, 0, {"Bid"}, &err));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2), N(3)}, {N(10), N(20), N(30)}})));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(3)}, {N(33)}})));  // held back (row 3)
  CHECK(t.N("B4") == 30);
  // deleting strike 1 moves strike 3 up a row: the held value is written before the rows move
  REQUIRE(Apply(t, Frame(FrameOp::Delete, {"Strike"}, {{N(1)}})));
  CHECK(t.N("A3") == 3);
  CHECK(t.N("B3") == 33);
  CHECK(t.d.BindNextFlush() == 0);
  // the user swaps the rows while a value is held: the flush finds the key's new row
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(2)}, {N(22)}})));  // held (first row)
  REQUIRE(t.Set("A2", "3"));
  REQUIRE(t.Set("A3", "2"));
  t.d.BindFlush(yc::engine::PerfCounterNs() + 5000000000LL);
  t.d.Recalc();
  CHECK(t.N("B3") == 22);  // strike 2 is now on row 3
  CHECK(t.N("B2") == 20);  // row 2 (now strike 3) is untouched
  // a replace drops held values with the old rows
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(3)}, {N(31)}})));
  REQUIRE(Apply(t, Frame(FrameOp::Replace, {"Strike", "Bid"}, {{N(7)}, {N(70)}})));
  CHECK(t.d.BindNextFlush() == 0);
  CHECK(t.N("B2") == 70);
}

TEST_CASE("bound tables: a newer held value wins over an older one after the rows moved") {
  yctest::Doc t;
  yc::model::BoundTable def;
  def.name = "Chain";
  def.sheet = t.sheet();
  def.key = "Strike";
  yc::model::BoundColumn bid;
  bid.name = "Bid";
  bid.max_hz = 1;
  def.columns = {bid};
  std::string err;
  REQUIRE(t.d.CreateBoundTable(def, 0, 0, {"Bid"}, &err));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2), N(3), N(4)}, {N(10), N(20), N(30), N(40)}})));
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(4)}, {N(42)}})));  // held on the 4th row
  // the user deletes the first row: strike 4 moves up to the 3rd row
  uint32_t sid = 0;
  const yc::model::Table* tb = yc::model::FindTable(t.d.wb(), "Chain", &sid);
  REQUIRE(tb != nullptr);
  REQUIRE(t.d.TableDeleteRow(sid, tb->id, 0));
  t.d.Recalc();
  CHECK(t.N("A4") == 4);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(4)}, {N(43)}})));  // newer, held on the 3rd row
  t.d.BindFlush(yc::engine::PerfCounterNs() + 5000000000LL);
  t.d.Recalc();
  CHECK(t.N("B4") == 43);
  CHECK(t.N("B3") == 30);
  CHECK(t.d.BindNextFlush() == 0);
}

TEST_CASE("bound tables: conditional formats over the table grow with it") {
  yctest::Doc t;
  MakeChain(t);
  yc::model::CfRule rule;
  rule.type = yc::model::CfType::Expression;
  rule.formula1 = "$B2>10";
  rule.ranges = {yc::core::Rect{1, 0, 1, 2}};  // the first (blank) data row of the new table
  t.d.AddCondFormat(t.sheet(), rule);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(1), N(2), N(3)}, {N(5), N(15), N(25)}})));
  const auto& cf = *t.d.S(t.sheet())->cond_formats;
  REQUIRE(cf.size() == 1);
  CHECK(cf[0].ranges[0] == yc::core::Rect{1, 0, 3, 2});
}

TEST_CASE("bound tables: a table as a Replace frame (snapshots, columnar reads)") {
  yctest::Doc t;
  MakeChain(t);
  ColumnFrame f;
  std::string err;
  REQUIRE(t.d.TableFrame("chain", false, &f, &err));
  CHECK(f.op == FrameOp::Replace);
  CHECK(f.nrows == 0);  // the fresh table's blank row isn't data
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid", "Ask"}, {{N(23000), N(23100)}, {N(10), N(8)}, {N(11), S("n/a")}})));
  t.Set("D1", "Mid");
  t.Set("D2", "=([@Bid]+[@Ask])/2");
  REQUIRE(t.d.TableFrame("Chain", false, &f, &err));
  CHECK(f.table == "Chain");
  CHECK(f.nrows == 2);
  REQUIRE(f.names.size() == 4);
  CHECK(f.key_col == 0);
  CHECK(f.cols[1][1].AsDouble() == 8);
  CHECK(f.cols[2][1].str() == "n/a");
  CHECK(f.cols[3][0].AsDouble() == 10.5);
  // a snapshot for replays leaves out computed columns, and applying it gives the same table
  REQUIRE(t.d.TableFrame("Chain", true, &f, &err));
  CHECK(f.names.size() == 3);
  REQUIRE(Apply(t, Frame(FrameOp::Upsert, {"Strike", "Bid"}, {{N(23000)}, {N(99)}})));
  CHECK(t.N("B2") == 99);
  REQUIRE(Apply(t, f));
  CHECK(t.N("B2") == 10);
  CHECK(t.N("D2") == 10.5);
  CHECK_FALSE(t.d.TableFrame("Nope", false, &f, &err));
}

TEST_CASE("tapes: frames with times, read back; a tape cut short keeps its whole frames") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "ycgrid_test.yctape";
  const auto a = yc::engine::EncodeColumnFrame(Frame(FrameOp::Replace, {"Strike", "Bid"}, {{N(23000)}, {N(1)}}));
  ColumnFrame other = Frame(FrameOp::Upsert, {"Sym", "Last"}, {{S("TXF")}, {N(23010)}});
  other.table = "Quotes";
  const auto b = yc::engine::EncodeColumnFrame(other);
  {
    yc::engine::TapeWriter w;
    std::string err;
    REQUIRE(w.Open(path.wstring(), 1761350400000, &err));
    REQUIRE(w.Write(0, a.data(), a.size()));
    REQUIRE(w.Write(5'000'000, b.data(), b.size()));
    REQUIRE(w.Write(4'000'000, a.data(), a.size()));  // a clock step back: replayed in order
    CHECK(w.frames() == 3);
  }
  yc::engine::TapeReader r;
  std::string err;
  REQUIRE(r.Open(path.wstring(), &err));
  const auto& ix = r.index();
  CHECK(ix.start_unix_ms == 1761350400000);
  REQUIRE(ix.t.size() == 3);
  CHECK(ix.t[1] == 5'000'000);
  CHECK(ix.t[2] == 5'000'000);
  CHECK(ix.length_ns() == 5'000'000);
  REQUIRE(ix.tables.size() == 2);
  CHECK(ix.tables[0] == "Chain");
  CHECK(ix.tables[1] == "Quotes");
  CHECK(ix.table[1] == 1);
  std::vector<uint8_t> buf;
  REQUIRE(r.Frame(1, &buf));
  ColumnFrame g;
  REQUIRE(yc::engine::DecodeColumnFrame(buf.data(), buf.size(), &g, &err));
  CHECK(g.table == "Quotes");
  CHECK(g.cols[1][0].AsDouble() == 23010);
  r.Close();
  // cut the last frame in half: two frames remain
  fs::resize_file(path, fs::file_size(path) - a.size() / 2);
  REQUIRE(r.Open(path.wstring(), &err));
  CHECK(r.index().t.size() == 2);
  r.Close();
  // not a tape
  {
    std::FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, path.wstring().c_str(), L"wb") == 0);
    std::fputs("hello, this is not a recording at all", f);
    std::fclose(f);
  }
  CHECK_FALSE(r.Open(path.wstring(), &err));
  CHECK_FALSE(err.empty());
  fs::remove(path);
}

TEST_CASE("tapes: topic batches (Topics.Push) round trip and are indexed with their topics") {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, Value>> items = {{"QUOTE|TXF.last", N(23010.5)}, {"QUOTE|TXF.name", S("TAIEX futures")},
                                                      {"QUOTE|halted", Value::Bool(true)}, {"QUOTE|bad", Value::Error(yc::core::Err::NA)},
                                                      {"QUOTE|empty", Value()}};
  const auto bytes = yc::engine::EncodeTopicBatch(items);
  std::vector<std::pair<std::string, Value>> back;
  REQUIRE(yc::engine::DecodeTopicBatch(bytes.data(), bytes.size(), &back));
  REQUIRE(back.size() == 5);
  CHECK(back[0].first == "QUOTE|TXF.last");
  CHECK(back[0].second.AsDouble() == 23010.5);
  CHECK(back[1].second.str() == "TAIEX futures");
  CHECK(back[2].second.boolean());
  CHECK(back[3].second.error() == yc::core::Err::NA);
  CHECK(back[4].second.IsEmpty());
  CHECK_FALSE(yc::engine::DecodeTopicBatch(bytes.data(), bytes.size() - 1, &back));  // cut short
  const fs::path path = fs::temp_directory_path() / "ycgrid_topics.yctape";
  {
    yc::engine::TapeWriter w;
    std::string err;
    REQUIRE(w.Open(path.wstring(), 0, &err));
    const auto frame = yc::engine::EncodeColumnFrame(Frame(FrameOp::Replace, {"Strike"}, {{N(1)}}));
    REQUIRE(w.Write(0, frame.data(), frame.size()));
    REQUIRE(w.Write(1000, bytes.data(), bytes.size(), yc::engine::kTapeTopics));
  }
  yc::engine::TapeReader r;
  std::string err;
  REQUIRE(r.Open(path.wstring(), &err));
  REQUIRE(r.index().t.size() == 2);
  CHECK(r.index().table[1] == yc::engine::TapeIndex::kTopicRecord);
  CHECK(r.index().topics.size() == 5);
  CHECK(r.index().tables.size() == 1);
  r.Close();
  fs::remove(path);
}

TEST_CASE("tapes: figure records and timeline markers (the time machine) are indexed; older kinds are unchanged") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "ycgrid_marks.yctape";
  {
    yc::engine::TapeWriter w;
    std::string err;
    REQUIRE(w.Open(path.wstring(), 0, &err));
    const auto frame = yc::engine::EncodeColumnFrame(Frame(FrameOp::Replace, {"Strike"}, {{N(1)}}));
    REQUIRE(w.Write(0, frame.data(), frame.size()));
    const std::string plot = "app-encoded figure request";
    REQUIRE(w.Write(500, reinterpret_cast<const uint8_t*>(plot.data()), plot.size(), yc::engine::kTapePlot));
    const std::string fill = "fill\tbuy 1 TXF @ 23001", note = "no kind";
    REQUIRE(w.Write(2000, reinterpret_cast<const uint8_t*>(fill.data()), fill.size(), yc::engine::kTapeMark));
    REQUIRE(w.Write(3000, reinterpret_cast<const uint8_t*>(note.data()), note.size(), yc::engine::kTapeMark));
  }
  yc::engine::TapeReader r;
  std::string err;
  REQUIRE(r.Open(path.wstring(), &err));
  REQUIRE(r.index().t.size() == 2);  // the frame and the figure request are replayed; markers are not records
  CHECK(r.index().table[1] == yc::engine::TapeIndex::kPlotRecord);
  std::vector<uint8_t> buf;
  REQUIRE(r.Frame(1, &buf));
  CHECK(std::string(buf.begin(), buf.end()) == "app-encoded figure request");
  REQUIRE(r.index().marks.size() == 2);
  CHECK(r.index().marks[0].kind == "fill");
  CHECK(r.index().marks[0].text == "buy 1 TXF @ 23001");
  CHECK(r.index().marks[0].t == 2000);
  CHECK(r.index().marks[1].kind == "note");
  CHECK(r.index().marks[1].text == "no kind");
  r.Close();
  fs::remove(path);
}

TEST_CASE("notebook cells are saved with the workbook (NBOK chunk)") {
  yctest::Doc t;
  CHECK(t.d.wb().notebook->empty());
  t.d.SetNotebook({"import numpy as np", "x = np.arange(10)\nx.sum()", ""});
  CHECK(t.d.wb().dirty);
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  REQUIRE(r.wb.notebook->size() == 3);
  CHECK((*r.wb.notebook)[1] == "x = np.arange(10)\nx.sum()");
  // a workbook without a notebook has no chunk
  yctest::Doc u;
  auto r2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(u.d.wb(), {}));
  REQUIRE(r2.ok);
  CHECK(r2.wb.notebook->empty());
}

TEST_CASE("chunks of removed features stay in the file (QRYS, DMOD, COAU)") {
  // Get & Transform, the Data Model and co-authoring were removed (ADR 0012): a file that has their chunks keeps them
  // as they are, and saving writes them back (hard rule 21)
  yctest::Doc t;
  std::vector<yc::model::RawChunk> old;
  for (const char* f : {"QRYS", "DMOD", "COAU"}) {
    const uint32_t cc = static_cast<uint32_t>(f[0]) | static_cast<uint32_t>(f[1]) << 8 | static_cast<uint32_t>(f[2]) << 16 |
                        static_cast<uint32_t>(f[3]) << 24;
    old.push_back(yc::model::RawChunk{cc, 1, 2 /* preserve */, std::string("\x91\xa3old", 5) + f});
  }
  t.d.wb_mut().unknown_chunks.reset(old);
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  CHECK_FALSE(r.wb.read_only);
  REQUIRE(r.wb.unknown_chunks->size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK((*r.wb.unknown_chunks)[i].fourcc == old[i].fourcc);
    CHECK((*r.wb.unknown_chunks)[i].data == old[i].data);
  }
  auto r2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(r.wb, {}));  // saved again: still there
  REQUIRE(r2.ok);
  CHECK(r2.wb.unknown_chunks->size() == 3);
}

TEST_CASE("tapes: a recording cut short (YCGrid ended mid-recording) opens with what reached the file") {
  // chaos seed 5001: a recording whose YCGrid was ended kept nothing, not even its header ("This isn't a YCGrid
  // recording"); the writer flushes the header at once and the frames at least every flush_every_ns
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "ycgrid_cut_short.yctape";
  const auto frame = yc::engine::EncodeColumnFrame(Frame(FrameOp::Replace, {"Strike"}, {{N(1)}}));
  yc::engine::TapeWriter w;
  std::string err;
  REQUIRE(w.Open(path.wstring(), 1761350400000, &err));
  {
    yc::engine::TapeReader r;  // read while the writer is still open, as after a crash
    REQUIRE(r.Open(path.wstring(), &err));
    CHECK(r.index().start_unix_ms == 1761350400000);
    CHECK(r.index().t.empty());
  }
  w.flush_every_ns = 0;  // every frame reaches the file
  REQUIRE(w.Write(0, frame.data(), frame.size()));
  REQUIRE(w.Write(1000, frame.data(), frame.size()));
  {
    yc::engine::TapeReader r;
    REQUIRE(r.Open(path.wstring(), &err));
    CHECK(r.index().t.size() == 2);
  }
  w.Close();
  fs::remove(path);
}
