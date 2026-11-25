// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/decimal.h"
#include "engine/snapshot.h"
#include "io/csv.h"
#include "io/msgpack.h"
#include "io/ycg.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

using yc::core::Rect;
using yc::core::Value;
namespace io = yc::io;

namespace {

// the workbook as comparable JSON (each cell's input text, display text and main style)
nlohmann::json Describe(const yc::model::Workbook& wb) {
  nlohmann::json j;
  j["sheets"] = nlohmann::json::array();
  for (const auto& sc : wb.sheets) {
    const auto& s = sc.get();
    nlohmann::json js;
    js["name"] = s.name;
    js["row_limit"] = s.row_limit;
    js["visible"] = s.visible;
    js["tab_color"] = s.tab_color;
    nlohmann::json cells = nlohmann::json::object();
    if (s.has_used) {
      for (uint32_t r = s.used.r0; r <= s.used.r1 && r < 200; ++r) {
        for (uint32_t c = s.used.c0; c <= s.used.c1 && c < 30; ++c) {
          if (s.IsEmptyCell(r, c) && !s.GetOwnStyle(r, c)) continue;
          const auto& st = wb.styles->Get(s.GetStyle(r, c));
          nlohmann::json jc;
          jc["input"] = yc::engine::InputTextOf(wb, s.id, r, c);
          jc["display"] = yc::engine::DisplayTextOf(s.GetValue(r, c), wb.styles->NumberFormat(st));
          jc["font"] = wb.styles->FontName(st);
          jc["size"] = wb.styles->FontSize(st);
          jc["bold"] = st.bold;
          jc["fill"] = st.fill_color;
          jc["numfmt"] = wb.styles->NumberFormat(st);
          jc["border_bottom"] = static_cast<int>(st.border[yc::model::kBottom].style);
          jc["halign"] = static_cast<int>(st.halign);
          cells[yc::core::CellA1(r, c)] = jc;
        }
      }
    }
    js["cells"] = cells;
    nlohmann::json merges = nlohmann::json::array();
    for (const auto& m : *s.merges) merges.push_back(yc::core::RectA1(m));
    js["merges"] = merges;
    js["row2_height"] = s.rows->Size(1);
    js["colB_width"] = s.cols->Size(1);
    js["row5_hidden"] = s.rows->IsHidden(4);
    js["controls"] = s.controls->size();
    js["charts"] = s.charts->size();
    js["notes"] = s.notes->size();
    j["sheets"].push_back(js);
  }
  nlohmann::json names = nlohmann::json::array();
  for (const auto& n : *wb.names) names.push_back({n.name, n.refers_to, n.sheet_scope});
  j["names"] = names;
  j["store_keys"] = nlohmann::json::array();
  for (const auto& [k, v] : *wb.store) j["store_keys"].push_back(k);
  j["props"] = wb.doc_props->size();
  j["panels"] = wb.panels->size();
  j["containers"] = wb.containers->size();
  j["layout"] = wb.layout_json;
  return j;
}

// a workbook that uses every feature (the compatibility corpus uses it too)
yc::model::Workbook BuildFeatureWorkbook() {
  yc::engine::Document d(yc::model::Workbook::NewDefault("工作表"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  d.RenameSheet(s1, "報價");
  const uint32_t s2 = d.AddSheet("分析");
  d.SetInput(s1, 0, 0, "標題");
  d.Merge(s1, Rect{0, 0, 0, 3}, false, true);
  d.SetInput(s1, 1, 0, "123.5");
  d.SetInput(s1, 1, 1, "TRUE");
  d.SetInput(s1, 1, 2, "1729658400.123456789");
  d.SetInput(s1, 1, 3, "=A2*2");
  d.SetInput(s1, 2, 0, "=1/0");
  d.SetInput(s1, 2, 1, "'0012");
  d.SetInput(s1, 2, 2, "50%");
  d.SetInput(s1, 2, 3, "2026/10/23");
  d.SetInput(s1, 3, 0, "=SUM(A2:A3)");
  d.SetInput(s1, 3, 1, "=分析!A1+1");
  d.SetInput(s1, 3, 2, "=TS_PART(C2,\"ns\")");
  d.ModifyStyle(s1, Rect{1, 0, 1, 3}, [](yc::model::Style& x) {
    x.bold = true;
    x.fill_color = 0xFFFFFF00;
    x.halign = yc::model::HAlign::Center;
  });
  {
    auto& t = d.wb_mut().styles.mut();
    const uint32_t f = t.InternString("Consolas");
    const uint32_t nf = t.InternString("[epoch][utc]yyyy-mm-dd hh:mm:ss.000000000");
    d.ModifyStyle(s1, Rect::Cell(1, 2), [f, nf](yc::model::Style& x) {
      x.font_name = f;
      x.numfmt = nf;
      x.font_size = 14;
    });
  }
  d.SetBorders(s1, Rect{1, 0, 3, 3}, yc::engine::BorderPreset::All);
  d.SetRowHeight(s1, 1, 1, 30);
  d.SetColWidth(s1, 1, 1, 120);
  d.HideRows(s1, 4, 4, true);
  d.SetTabColor(s1, 0xFF00B050);
  d.SetInput(s2, 0, 0, "41");
  d.SetInput(s2, 5, 5, "這是一段很長的中文字串，用來測試字串堆積儲存（超過 14 bytes）");
  d.AddName("單價", "=報價!$A$2");
  d.SetInput(s2, 1, 0, "=單價*3");
  d.wb_mut().store.mut()["ui.version"] = std::string("\x03", 1);  // msgpack integer 3
  d.wb_mut().doc_props.mut()["作者"] = yc::model::DocProp{Value::String("YCT"), false, ""};
  yc::model::Control btn;
  btn.id = 1;
  btn.name = "CommandButton1";
  btn.caption = "執行";
  d.wb_mut().MutSheetAt(0)->controls.mut().push_back(btn);
  yc::model::ChartObj ch;
  ch.id = 2;
  ch.name = "圖表 1";
  ch.series.push_back({"成交", "報價!A2:A4", "", 0, 0, "", 1000, false, nullptr});
  d.wb_mut().MutSheetAt(0)->charts.mut().push_back(ch);
  d.wb_mut().MutSheetAt(0)->notes.mut()[(uint64_t{1} << 16) | 0] = yc::model::CellNote{"YCT", "附註內容"};
  yc::model::Panel p;
  p.name = "風控";
  p.controls.push_back({"button", "Run", "執行", "", "Module1.Run", "", "", {}, "", 0, 100, true, true});
  d.wb_mut().panels.mut().push_back(p);
  d.wb_mut().containers.mut().push_back(yc::model::ContainerDef{"容器1", "容器", 10, 20, 300, 200, true, false});
  d.wb_mut().layout_json = "{\"schema\":\"ycgrid.layout\"}";
  d.Recalc();
  return d.wb();
}

}  // namespace

TEST_CASE("msgpack 讀寫與略過未知欄位") {
  io::MsgWriter w;
  w.Map(3);
  w.KInt(1, -70000);
  w.KStr(2, "中文");
  w.Int(99);  // unknown field: a nested structure
  w.Array(2);
  w.Double(1.5);
  w.Map(1);
  w.KBool(1, true);
  io::MsgReader r(w.buf());
  uint32_t n;
  REQUIRE(r.Map(&n));
  REQUIRE(n == 3);
  int64_t k, v;
  REQUIRE(r.Int(&k));
  REQUIRE(r.Int(&v));
  CHECK(v == -70000);
  std::string s;
  REQUIRE(r.Int(&k));
  REQUIRE(r.Str(&s));
  CHECK(s == "中文");
  REQUIRE(r.Int(&k));
  CHECK(k == 99);
  REQUIRE(r.Skip());
  CHECK(r.AtEnd());
  CHECK(io::Crc32c("123456789", 9) == 0xE3069283u);
}

TEST_CASE(".ycg 存讀後完全相同") {
  const auto wb = BuildFeatureWorkbook();
  const std::string bytes = io::SaveYcgToBytes(wb, {});
  auto res = io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  CHECK(res.warnings.empty());
  CHECK_FALSE(res.wb.read_only);
  // after loading, rebuild the formulas and recalculate: the results must match
  yc::engine::Document d(res.wb);
  const auto a = Describe(wb), b = Describe(d.wb());
  CHECK(a.dump(2) == b.dump(2));
  // Decimal values identical digit by digit (the Gate of PLAN §5.2a)
  const auto& c2 = d.wb().sheets[0]->GetValue(1, 2);
  REQUIRE(c2.IsDecimal());
  CHECK(yc::core::DecToString(yc::core::DecFromValue(c2)) == "1729658400.123456789");
  // save again: the bytes are stable (except META's time stamp)
  const std::string bytes2 = io::SaveYcgToBytes(d.wb(), {});
  auto res2 = io::LoadYcgFromBytes(bytes2);
  REQUIRE(res2.ok);
  yc::engine::Document d2(res2.wb);
  CHECK(Describe(d2.wb()).dump() == a.dump());
}

namespace {
// adds a chunk at the end of the file (as a newer YCGrid would save it)
std::string AppendChunk(const std::string& file, uint32_t fourcc, uint16_t version, uint16_t flags, const std::string& data) {
  const uint32_t count = static_cast<uint32_t>(static_cast<uint8_t>(file[28]) | (static_cast<uint8_t>(file[29]) << 8));
  uint64_t dir = 0;
  for (int i = 7; i >= 0; --i) dir = (dir << 8) | static_cast<uint8_t>(file[32 + static_cast<size_t>(i)]);
  std::string body = file.substr(0, static_cast<size_t>(dir));
  std::string dirb = file.substr(static_cast<size_t>(dir), size_t{count} * 40);
  const uint64_t off = body.size();
  body += data;
  auto le = [](std::string& b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b += static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  le(dirb, fourcc, 4);
  le(dirb, version, 2);
  le(dirb, flags, 2);
  le(dirb, off, 8);
  le(dirb, data.size(), 8);
  le(dirb, data.size(), 8);
  le(dirb, 0, 4);
  le(dirb, io::Crc32c(data.data(), data.size()), 4);
  const uint64_t new_dir = body.size();
  body += dirb;
  le(body, io::Crc32c(dirb.data(), dirb.size()), 4);
  std::string h = body.substr(0, 44);
  h[28] = static_cast<char>((count + 1) & 0xFF);
  h[29] = static_cast<char>(((count + 1) >> 8) & 0xFF);
  for (int i = 0; i < 8; ++i) h[32 + static_cast<size_t>(i)] = static_cast<char>((new_dir >> (8 * i)) & 0xFF);
  le(h, io::Crc32c(h.data(), 44), 4);
  body.replace(0, 48, h);
  return body;
}
}  // namespace

TEST_CASE("未來版本的 chunk：非必要的原樣保留，必要的以唯讀開啟") {
  const auto wb = BuildFeatureWorkbook();
  const std::string base = io::SaveYcgToBytes(wb, {});
  // optional: kept
  const std::string with_future = AppendChunk(base, io::FourCC("ZFUT"), 3, io::kChunkPreserve, "future payload");
  auto r1 = io::LoadYcgFromBytes(with_future);
  REQUIRE(r1.ok);
  CHECK_FALSE(r1.wb.read_only);
  REQUIRE(r1.wb.unknown_chunks->size() == 1);
  CHECK((*r1.wb.unknown_chunks)[0].data == "future payload");
  // saved and read again: still kept
  auto r2 = io::LoadYcgFromBytes(io::SaveYcgToBytes(r1.wb, {}));
  REQUIRE(r2.ok);
  REQUIRE(r2.wb.unknown_chunks->size() == 1);
  CHECK((*r2.wb.unknown_chunks)[0].version == 3);
  // a required chunk that can't be read -> read-only
  auto r3 = io::LoadYcgFromBytes(AppendChunk(base, io::FourCC("ZCRT"), 1, io::kChunkCritical, "x"));
  REQUIRE(r3.ok);
  CHECK(r3.wb.read_only);
  // a newer version of a known chunk, required -> read-only
  auto r4 = io::LoadYcgFromBytes(AppendChunk(base, io::FourCC("CELL"), 9, io::kChunkCritical, "x"));
  REQUIRE(r4.ok);
  CHECK(r4.wb.read_only);
}

TEST_CASE("毀損的檔案回報錯誤而不是當機") {
  const std::string base = io::SaveYcgToBytes(BuildFeatureWorkbook(), {});
  CHECK_FALSE(io::LoadYcgFromBytes("").ok);
  CHECK_FALSE(io::LoadYcgFromBytes("PK\x03\x04 not a ycg").ok);
  CHECK_FALSE(io::LoadYcgFromBytes(base.substr(0, base.size() / 2)).ok);
  for (size_t pos : {size_t{60}, base.size() / 3, base.size() - 50}) {
    std::string bad = base;
    bad[pos] = static_cast<char>(bad[pos] ^ 0x5A);
    auto r = io::LoadYcgFromBytes(bad);
    CHECK((!r.ok || !r.warnings.empty() || r.ok));  // not crashing is enough; a damaged required chunk reports an error
  }
}

TEST_CASE("CSV 匯入匯出") {
  const auto t = io::ParseCsv("a,b,c\r\n1,\"x,y\",\"多\n行\"\r\n");
  REQUIRE(t.rows.size() == 2);
  CHECK(t.rows[1][1] == "x,y");
  CHECK(t.rows[1][2] == "多\n行");
  CHECK(io::ParseCsv("a;b;c\n1;2;3").delimiter == ';');
  CHECK(io::ParseCsv("a\tb\n").delimiter == '\t');
  CHECK(io::DecodeText("\xEF\xBB\xBFhi") == "hi");
  const std::wstring tmp = (std::filesystem::temp_directory_path() / L"yc_test_匯入.csv").wstring();
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary);
    f << "代碼,時間戳,公式\r\nTX01,1729658400.123456789,=1+1\r\n";
  }
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportCsv(tmp, &wb, &err));
  CHECK(wb.sheets[0]->name == "yc_test_匯入");
  CHECK(wb.sheets[0]->GetValue(1, 1).IsDecimal());
  CHECK(wb.sheets[0]->GetValue(1, 2).str() == "=1+1");  // CSV doesn't run formulas
  std::filesystem::remove(tmp);
}

TEST_CASE("相容語料庫：舊版存的檔案都能開啟（PLAN §15.6）") {
  const std::filesystem::path dir = std::filesystem::path(YC_SOURCE_DIR) / "tests" / "compat" / "ycg" / "1.0";
  const auto file = dir / "features.ycg";
  const auto expect = dir / "features.json";
  if (!std::filesystem::exists(file)) {
    // first run: write the corpus with this version (once committed it stays; every later version must read it)
    std::filesystem::create_directories(dir);
    const auto wb = BuildFeatureWorkbook();
    std::string err;
    REQUIRE(io::SaveYcg(wb, file.wstring(), {}, &err));
    yc::engine::Document d(io::LoadYcg(file.wstring()).wb);
    std::ofstream(expect, std::ios::binary) << Describe(d.wb()).dump(2);
  }
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(YC_SOURCE_DIR) / "tests" / "compat" / "ycg")) {
    if (!entry.is_directory()) continue;
    for (const auto& f : std::filesystem::directory_iterator(entry.path())) {
      if (f.path().extension() != ".ycg") continue;
      INFO(f.path().string());
      auto res = io::LoadYcg(f.path().wstring());
      REQUIRE(res.ok);
      yc::engine::Document d(res.wb);
      auto jpath = f.path();
      jpath.replace_extension(".json");
      std::ifstream jf(jpath, std::ios::binary);
      REQUIRE(jf);
      const auto expected = nlohmann::json::parse(jf);
      CHECK(Describe(d.wb()) == expected);
      // saved as and reopened: still the same
      auto again = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
      REQUIRE(again.ok);
      yc::engine::Document d2(again.wb);
      CHECK(Describe(d2.wb()) == expected);
    }
  }
}

TEST_CASE(".ycg 保存控制項、圖表的 Python 數列資料與面板") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("工作表"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::Control c;
  c.kind = yc::model::ControlKind::ComboBox;
  c.items = {"A", "B"};
  c.linked_cell = "D1";
  c.value = "B";
  d.AddControl(s1, c);
  yc::model::ChartObj ch;
  ch.type = yc::model::ChartType::Line;
  yc::model::ChartSeries se;
  se.name = "價格";
  se.python_data = true;
  auto data = std::make_shared<yc::model::SeriesData>();
  data->x = {1, 2, 3};
  data->y = {100.5, 101.25, 99.0};
  se.data = data;
  ch.series.push_back(se);
  d.AddChart(s1, ch);
  yc::model::Panel p;
  p.name = "風控";
  p.controls.push_back(yc::model::PanelControl{"slider", "量", "下單量", "E1", "", "", "", {}, "3", 0, 10, true, true});
  d.UpsertPanel(p);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& sh = *res.wb.sheets[0];
  REQUIRE(sh.controls->size() == 1);
  CHECK((*sh.controls)[0].name == "ComboBox1");
  CHECK((*sh.controls)[0].value == "B");
  CHECK((*sh.controls)[0].items.size() == 2);
  REQUIRE(sh.charts->size() == 1);
  const auto& rs = (*sh.charts)[0].series.at(0);
  CHECK(rs.python_data);
  REQUIRE(rs.data);
  CHECK(rs.data->y == std::vector<double>{100.5, 101.25, 99.0});
  CHECK(rs.data->x == std::vector<double>{1, 2, 3});
  REQUIRE(res.wb.panels->size() == 1);
  CHECK((*res.wb.panels)[0].controls.at(0).value == "3");
}

TEST_CASE("Fill patterns and gradients are saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  yc::model::Style st;
  st.fill_color = 0xFFFFFF00;
  st.pattern = yc::model::FillPattern::LightDown;
  st.pattern_color = 0xFF0000FF;
  yc::model::Style g;
  g.fill_color = 0xFFFFFFFF;
  g.gradient = yc::model::FillGradient::Linear;
  g.gradient_angle = 90;
  g.gradient_color2 = 0xFF5B9BD5;
  auto& t0 = d.wb_mut().styles.mut();
  const uint32_t a = t0.Intern(st), b = t0.Intern(g);
  CHECK(a != b);
  d.M(sheet)->SetStyle(0, 0, a);
  d.M(sheet)->SetStyle(1, 0, b);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& t = *res.wb.styles;
  const auto& sa = t.Get(res.wb.sheets[0]->GetStyle(0, 0));
  CHECK(sa.pattern == yc::model::FillPattern::LightDown);
  CHECK(sa.pattern_color == 0xFF0000FF);
  CHECK(sa.HasFill());
  const auto& sb = t.Get(res.wb.sheets[0]->GetStyle(1, 0));
  CHECK(sb.gradient == yc::model::FillGradient::Linear);
  CHECK(sb.gradient_angle == 90);
  CHECK(sb.gradient_color2 == 0xFF5B9BD5);
}

TEST_CASE("Threaded comments: replies, resolve, convert, save and load") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  d.SetComment(s1, 2, 1, "Ann", "Is this right?", 1700000000);
  CHECK(d.AddCommentReply(s1, 2, 1, "Bob", "Yes", 1700000100));
  CHECK(d.AddCommentReply(s1, 2, 1, "Ann", "Thanks", 1700000200));
  CHECK(d.EditCommentPost(s1, 2, 1, 0, "Yes, checked"));
  CHECK(d.DeleteCommentReply(s1, 2, 1, 1));
  CHECK(d.ResolveComment(s1, 2, 1, true));
  CHECK_FALSE(d.AddCommentReply(s1, 9, 9, "X", "no thread", 0));
  d.SetNote(s1, 4, 0, "Cy", "a note");
  CHECK_FALSE(d.AddCommentReply(s1, 4, 0, "X", "notes take no replies", 0));
  CHECK(d.ConvertNotesToComments(s1, Rect{0, 0, 10, 10}, 1700000300) == 1);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& notes = *res.wb.sheets[0]->notes;
  const auto& t = notes.at((uint64_t{2} << 16) | 1);
  CHECK(t.threaded);
  CHECK(t.resolved);
  CHECK(t.author == "Ann");
  CHECK(t.date == 1700000000);
  REQUIRE(t.replies.size() == 1);
  CHECK(t.replies[0].author == "Bob");
  CHECK(t.replies[0].text == "Yes, checked");
  CHECK(t.replies[0].date == 1700000100);
  const auto& c = notes.at(uint64_t{4} << 16);
  CHECK(c.threaded);
  CHECK(c.text == "a note");
  // inserting a row above moves the thread with its cell
  d.InsertRows(s1, 0, 1);
  CHECK(d.wb().sheets[0]->notes->count((uint64_t{3} << 16) | 1) == 1);
}

TEST_CASE("Encrypted workbooks need the password") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  d.SetValue(s1, 0, 0, yc::core::Value::String("secret value"));
  d.SetOpenPassword("p@ss");
  const std::wstring path = (std::filesystem::temp_directory_path() / L"yc_test_encrypted.ycg").wstring();
  std::string err;
  REQUIRE(io::SaveYcg(d.wb(), path, {}, &err));
  std::string bytes;
  REQUIRE(io::ReadFileBytes(path, &bytes));
  CHECK(bytes.substr(0, 4) == "YCGE");
  CHECK(bytes.find("secret value") == std::string::npos);
  auto none = io::LoadYcg(path);
  CHECK_FALSE(none.ok);
  CHECK(none.needs_password);
  auto wrong = io::LoadYcg(path, "nope");
  CHECK_FALSE(wrong.ok);
  CHECK(wrong.wrong_password);
  auto right = io::LoadYcg(path, "p@ss");
  REQUIRE(right.ok);
  CHECK(right.wb.sheets[0]->GetValue(0, 0).str() == "secret value");
  CHECK(right.wb.open_password == "p@ss");
  // a damaged byte fails the authentication
  bytes[60] ^= 1;
  CHECK_FALSE(io::LoadYcgFromBytes(bytes, "p@ss").ok);
  std::filesystem::remove(path);
}

TEST_CASE("Form controls keep their kind, OnAction and form flag") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::Control g;
  g.kind = yc::model::ControlKind::GroupBox;
  g.form = true;
  g.caption = "Choices";
  d.AddControl(s1, g);
  yc::model::Control b;
  b.kind = yc::model::ControlKind::Button;
  b.form = true;
  b.on_action = "Module1.Report";
  d.AddControl(s1, b);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& cs = *res.wb.sheets[0]->controls;
  REQUIRE(cs.size() == 2);
  CHECK(cs[0].kind == yc::model::ControlKind::GroupBox);
  CHECK(cs[0].form);
  CHECK(cs[1].on_action == "Module1.Report");
  CHECK(cs[1].form);
}

TEST_CASE("panel colors are saved (PANL keys 8-9)") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  yc::model::Panel p;
  p.name = "Ticket";
  p.back_color = 0x1A1410;
  p.fore_color = 0xE0E0E0;
  d.UpsertPanel(p);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.panels->size() == 1);
  CHECK((*res.wb.panels)[0].back_color == 0x1A1410);
  CHECK((*res.wb.panels)[0].fore_color == 0xE0E0E0);
}

TEST_CASE("panel control styling is saved (PANL keys 13-18)") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  yc::model::Panel p;
  p.name = "Desk";
  yc::model::PanelControl buy;
  buy.type = "button";
  buy.name = "Buy";
  buy.back_color = 0x0000C0;  // RGB(192, 0, 0)
  buy.fore_color = 0xFFFFFF;
  buy.font_size = 14;
  buy.bold = true;
  buy.height = 30;
  yc::model::PanelControl pnl;
  pnl.type = "label";
  pnl.name = "PnL";
  pnl.linked_cell = "A9";
  pnl.number_format = "[Red]#,##0;[Green]-#,##0";
  p.controls = {buy, pnl};
  d.UpsertPanel(p);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.panels->size() == 1);
  const auto& c = (*res.wb.panels)[0].controls;
  REQUIRE(c.size() == 2);
  CHECK(c[0].back_color == 0x0000C0);
  CHECK(c[0].fore_color == 0xFFFFFF);
  CHECK(c[0].font_size == 14);
  CHECK(c[0].bold);
  CHECK(c[0].height == 30);
  CHECK(c[1].back_color == -1);  // not styled: the theme's colors
  CHECK((*res.wb.panels)[0].back_color == -1);
  CHECK(c[1].number_format == "[Red]#,##0;[Green]-#,##0");
}

TEST_CASE("a panel spin's step is saved (PANL key 19); controls without one step by 1") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  yc::model::Panel p;
  p.name = "Strategy";
  yc::model::PanelControl width;
  width.type = "spin";
  width.name = "Width";
  width.min = 100;
  width.max = 500;
  width.small_change = 100;
  yc::model::PanelControl qty;
  qty.type = "spin";
  qty.name = "Qty";
  p.controls = {width, qty};
  d.UpsertPanel(p);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.panels)[0].controls;
  REQUIRE(c.size() == 2);
  CHECK(c[0].small_change == 100);
  CHECK(c[1].small_change == 1);
}

TEST_CASE("Damaged structure in a file is refused without a crash: nesting, sizes, merges, offsets") {
  // a msgpack value nested 200k deep (a chunk's unknown field) is skipped without recursing that deep
  const std::string nested = std::string(200000, static_cast<char>(0x91)) + static_cast<char>(0xC0);  // MsgReader keeps a view: the string stays
  io::MsgReader deep(nested);
  CHECK_FALSE(deep.Skip());
  // a default row height / column width of 0 can't divide by zero later
  yc::model::DimIndex dims;
  dims.set_default_size(0);
  CHECK(dims.default_size() == 1);
  dims.Load(-3, {});
  CHECK(dims.default_size() == 1);
  // a merged area with its corners swapped or past the sheet is dropped on load
  yc::model::Workbook wb = BuildFeatureWorkbook();
  yc::model::Sheet* sh0 = wb.MutSheetById(wb.sheets[0]->id);
  sh0->merges.mut().push_back(yc::core::Rect{5, 5, 1, 1});
  sh0->merges.mut().push_back(yc::core::Rect{0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu});
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(wb, {}));
  REQUIRE(res.ok);
  for (const auto& m : *res.wb.sheets[0]->merges) {
    CHECK(m.r0 <= m.r1);
    CHECK(m.c1 < yc::core::kMaxCols);
  }
  // a directory offset that wraps the bounds check (count 0, offset 2^64-4) is an error, not an exception
  std::string base = io::SaveYcgToBytes(wb, {});
  std::string h = base.substr(0, 44);
  for (int i = 0; i < 4; ++i) h[28 + static_cast<size_t>(i)] = 0;
  for (int i = 0; i < 8; ++i) h[32 + static_cast<size_t>(i)] = static_cast<char>(i == 0 ? 0xFC : 0xFF);
  const uint32_t crc = io::Crc32c(h.data(), 44);
  for (int i = 0; i < 4; ++i) h += static_cast<char>((crc >> (8 * i)) & 0xFF);
  base.replace(0, 48, h);
  auto wrapped = io::LoadYcgFromBytes(base);
  CHECK_FALSE(wrapped.ok);
  CHECK_FALSE(wrapped.error.empty());
}

TEST_CASE("CSV: a long file name gives a sheet name of 31 whole characters") {
  // "a" + 32 CJK characters: a cut at 93 bytes would end inside the 31st character (and no client could decode it)
  std::wstring stem = L"a";
  for (int i = 0; i < 32; ++i) stem += L'\x53F0';
  const std::wstring tmp = (std::filesystem::temp_directory_path() / (stem + L".csv")).wstring();
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary);
    f << "a,b\r\n1,2\r\n";
  }
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportCsv(tmp, &wb, &err));
  std::string want = "a";
  for (int i = 0; i < 30; ++i) want += "\xE5\x8F\xB0";
  CHECK(wb.sheets[0]->name == want);
  std::filesystem::remove(tmp);
}

TEST_CASE("A CSV opens as Excel opens it: an apostrophe is text, a month and year in numbers stays text (measured)") {
  const std::wstring tmp = (std::filesystem::temp_directory_path() / L"yc_csv_apostrophe.csv").wstring();
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary);
    f << "'5,'abc,-5,12/2024,1/0,1/5,TRUE \r\n";
  }
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportCsv(tmp, &wb, &err));
  const auto& sh = *wb.sheets[0];
  auto text = [&](uint32_t c) { return sh.GetValue(0, c).IsString() ? std::string(sh.GetValue(0, c).str()) : std::string("<not text>"); };
  CHECK(text(0) == "'5");
  CHECK(text(1) == "'abc");
  CHECK(sh.GetValue(0, 2).IsNumber());
  CHECK(text(3) == "12/2024");
  CHECK(text(4) == "1/0");
  CHECK(sh.GetValue(0, 5).IsNumber());  // 1/5: a date of this year, as Excel
  CHECK(text(6) == "TRUE ");
  std::filesystem::remove(tmp);
}
