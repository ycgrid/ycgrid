// SPDX-License-Identifier: Apache-2.0
// Round 9: files a trader meets. A CSV opens as typing reads each field (once, written in bulk), and no field becomes
// a formula; a save that fails says why.
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "engine/document.h"
#include "engine/engine.h"
#include "io/csv.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

using yc::core::Value;
namespace engine = yc::engine;
namespace io = yc::io;
namespace model = yc::model;

namespace {

std::wstring TempFile(const wchar_t* name, const std::string& bytes) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / (L"yc_round9_files_" + std::to_wstring(::GetCurrentProcessId()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path p = dir / name;
  std::ofstream(p, std::ios::binary) << bytes;
  return p.wstring();
}

struct Cell {
  Value v;
  std::string fmt;
};

Cell At(const model::Workbook& wb, uint32_t r, uint32_t c) {
  const model::Sheet& s = *wb.sheets[0];
  return Cell{s.GetValue(r, c), std::string(wb.styles->NumberFormat(wb.styles->Get(s.GetStyle(r, c))))};
}

}  // namespace

TEST_CASE("A CSV field never becomes a formula, whatever it starts with") {
  // = was kept as text, but +2+3, -4+5 and a phone number +886-2-1234-5678 went in as typing does: live formulas
  const std::wstring path = TempFile(L"inject.csv", "=1+1,+2+3,-4+5,-A1,+886-2-1234-5678,@SUM(1),-5,+5,(12),-$5\r\n");
  model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportCsv(path, &wb, &err));
  const char* text[] = {"=1+1", "+2+3", "-4+5", "-A1", "+886-2-1234-5678", "@SUM(1)"};
  for (uint32_t c = 0; c < 6; ++c) {
    const Cell x = At(wb, 0, c);
    CHECK(x.v.IsString());
    CHECK(x.v.str() == text[c]);
    CHECK(wb.sheets[0]->GetFormula(0, c) == 0);
  }
  // amounts are still numbers
  CHECK(At(wb, 0, 6).v.AsDouble() == -5);
  CHECK(At(wb, 0, 7).v.AsDouble() == 5);
  CHECK(At(wb, 0, 8).v.AsDouble() == -12);
  CHECK(At(wb, 0, 9).v.AsDouble() == -5);
}

TEST_CASE("A CSV's fields read as typing reads them, formats included, down long columns") {
  // the fields are read once and written a block at a time, with each column's formats as runs: the result must be
  // what typing each field gives
  const std::vector<std::string> inputs = {"2026/1/5", "00123", "5%", "1.5E+3", "TRUE", "false", "12/2024", "9:30", "9:30:15",
                                           "2026/3/1 14:05", "$1234.5", "(7)", "1/2", "3 1/4", "TXF", "#N/A", "1,234",
                                           "'5", "  42  ", "-0", "1e", "10:30PM", "Jan-26", "45%", "NT$500"};
  std::string csv;
  for (const std::string& s : inputs) csv += "\"" + s + "\"\r\n";
  model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportCsv(TempFile(L"types.csv", csv), &wb, &err));
  for (uint32_t i = 0; i < inputs.size(); ++i) {
    yctest::Doc typed;
    if (inputs[i][0] == '\'') {
      typed.d.SetValue(typed.sheet(), 0, 0, Value::String(inputs[i]));  // a CSV keeps the apostrophe (measured)
    } else if (inputs[i] == "12/2024") {
      typed.d.SetValue(typed.sheet(), 0, 0, Value::String(inputs[i]));  // only typing reads a month and year (measured)
    } else {
      typed.d.SetInput(typed.sheet(), 0, 0, inputs[i]);
    }
    const Cell want = At(typed.d.wb(), 0, 0);
    const Cell got = At(wb, i, 0);
    INFO(inputs[i]);
    CHECK(got.v.SameAs(want.v));
    CHECK(got.fmt == want.fmt);
  }
  // a long date column gets its format on every row, past the first block of rows, around a blank and a text
  std::string big = "Date,Qty\r\n";
  for (int i = 0; i < 70000; ++i) big += (i == 100 ? std::string() : i == 200 ? std::string("n/a") : "2026/1/" + std::to_string(1 + i % 28)) + "," + std::to_string(i) + "\r\n";
  model::Workbook w2;
  REQUIRE(io::ImportCsv(TempFile(L"long.csv", big), &w2, &err));
  const std::string date = At(w2, 1, 0).fmt;
  CHECK(date != "General");
  CHECK(At(w2, 70000, 0).fmt == date);
  CHECK(At(w2, 65537, 0).fmt == date);
  CHECK(At(w2, 101, 0).v.IsEmpty());
  CHECK(At(w2, 101, 0).fmt == "General");
  CHECK(At(w2, 201, 0).v.IsString());
  CHECK(At(w2, 201, 0).fmt == "General");
  CHECK(At(w2, 202, 0).fmt == date);
  CHECK(At(w2, 70000, 1).v.AsDouble() == 69999);
  CHECK(At(w2, 70000, 1).fmt == "General");
}

TEST_CASE("A save that fails says why: a read-only file, a folder that isn't there, a name Windows refuses") {
  // the messages are read in English, whatever the machine shows
  const yc::i18n::Lang lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  std::string err;
  const std::wstring ro = TempFile(L"readonly.ycg", "old");
  ::SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
  CHECK_FALSE(io::WriteFileAtomic(ro, "new", &err));
  CHECK(err == "The file is read-only.");
  ::SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
  std::ifstream back(std::filesystem::path(ro), std::ios::binary);
  std::string kept((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
  CHECK(kept == "old");
  CHECK_FALSE(std::filesystem::exists(ro + L".tmp~"));
  const std::filesystem::path dir = std::filesystem::path(ro).parent_path();
  CHECK_FALSE(io::WriteFileAtomic((dir / L"no" / L"such" / L"x.ycg").wstring(), "x", &err));
  CHECK(err == "The folder doesn't exist.");
  CHECK_FALSE(io::WriteFileAtomic((dir / L"a?b.ycg").wstring(), "x", &err));
  CHECK(err == "Windows doesn't allow this file name: it has a character such as ? * : or |.");
  const std::filesystem::path deep = dir / std::wstring(60, L'a') / std::wstring(60, L'b') / std::wstring(60, L'c') / std::wstring(60, L'd') / L"x.ycg";
  CHECK_FALSE(io::WriteFileAtomic(deep.wstring(), "x", &err));
  CHECK(err == "The path is too long for Windows (260 characters at most).");
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("A workbook keeps its calculation mode and iterative calculation settings, and opening it sets them, as in Excel") {
  // the engine's snapshot carries what the workbook is calculated with; a save writes it (WKBK key 7)
  engine::Engine eng(model::Workbook::NewDefault("Sheet"));
  eng.Start();
  eng.Call([](engine::Document& d) {
    d.iterate = true;
    d.max_iterations = 37;
    d.max_change = 0.01;
    d.auto_calc = false;  // Manual, kept in the file too
    const uint32_t s = d.wb().sheets[0]->id;
    d.SetInput(s, 0, 0, "=A1+1");  // a cycle the settings are meant for
    d.Recalc();
  });
  eng.Sync();
  const std::string bytes = io::SaveYcgToBytes(eng.Current()->wb, {});
  auto res = io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  CHECK(res.wb.calc_iterate == 1);
  CHECK(res.wb.calc_max_iterations == 37);
  CHECK(res.wb.calc_max_change == 0.01);
  CHECK(res.wb.calc_mode == 1);
  // a session with iteration off opens it: the file's settings come with it, and its cycle is calculated
  engine::Engine other(model::Workbook::NewDefault("Sheet"));
  other.Start();
  other.Call([](engine::Document& d) { d.iterate = false; });
  other.Replace(std::move(res.wb));
  other.Sync();
  CHECK(other.Call([](engine::Document& d) { return d.iterate && d.max_iterations == 37 && d.max_change == 0.01; }));
  CHECK(other.Call([](engine::Document& d) { return !d.auto_calc && d.auto_data_tables; }));
  CHECK(other.Current()->manual_calc);
  // a file saved before the settings were kept (no key 7) leaves the session's
  model::Workbook old = model::Workbook::NewDefault("Sheet");
  auto res2 = io::LoadYcgFromBytes(io::SaveYcgToBytes(old, {}));
  REQUIRE(res2.ok);
  CHECK(res2.wb.calc_iterate == -1);
  CHECK(res2.wb.calc_mode == -1);
  other.Call([](engine::Document& d) { d.iterate = false; });
  other.Replace(std::move(res2.wb));
  other.Sync();
  CHECK(other.Call([](engine::Document& d) { return !d.iterate && d.max_iterations == 37 && !d.auto_calc; }));
  other.Stop();
  eng.Stop();
}
