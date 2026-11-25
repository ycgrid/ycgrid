// SPDX-License-Identifier: Apache-2.0
// [YC] safety: parameter cells, the change log, settings (model/safety.h, document_safety.cpp)
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Value;
using yc::model::ParameterDef;

namespace {

ParameterDef Spread(uint32_t sheet) {
  ParameterDef p;
  p.sheet = sheet;
  p.row = 1;
  p.col = 1;  // B2
  p.name = "Spread";
  p.type = 1;
  p.has_min = p.has_max = true;
  p.min = 0;
  p.max = 5;
  p.max_step = 1;
  return p;
}

}  // namespace

TEST_CASE("parameter cells: typing is refused unless confirmed; Python may write; values are checked") {
  yctest::Doc t;
  t.Set("B2", "1.5");
  std::string err;
  REQUIRE(t.d.SetParameter(Spread(t.sheet()), &err));
  REQUIRE(t.d.ParameterAt(t.sheet(), 1, 1) != nullptr);
  // the user can't change it directly (paste, fill, delete, typing without the UI's confirmation)
  CHECK_FALSE(t.d.SetInput(t.sheet(), 1, 1, "2"));
  CHECK(t.d.block_param());
  CHECK(t.N("B2") == 1.5);
  t.d.ClearContents(t.sheet(), yc::core::Rect{0, 0, 3, 3});
  CHECK(t.N("B2") == 1.5);
  // the UI confirmed it: one edit goes through
  t.d.param_commit = true;
  CHECK(t.d.SetInput(t.sheet(), 1, 1, "2"));
  t.d.param_commit = false;
  CHECK(t.N("B2") == 2);
  // Python writes freely
  t.d.python_call = true;
  t.d.SetValue(t.sheet(), 1, 1, Value::Number(2.5), yc::engine::kSrcPython);
  t.d.python_call = false;
  CHECK(t.N("B2") == 2.5);
  // checks
  const ParameterDef p = Spread(t.sheet());
  CHECK(yc::engine::Document::CheckParameterValue(p, Value::Number(3), &err));
  CHECK_FALSE(yc::engine::Document::CheckParameterValue(p, Value::Number(6), &err));
  CHECK(err.find("5") != std::string::npos);
  CHECK_FALSE(yc::engine::Document::CheckParameterValue(p, Value::String("x"), &err));
  ParameterDef side = p;
  side.type = 5;
  side.choices = {"Buy", "Sell"};
  CHECK(yc::engine::Document::CheckParameterValue(side, Value::String("sell"), &err));
  CHECK_FALSE(yc::engine::Document::CheckParameterValue(side, Value::String("Hold"), &err));
  ParameterDef lots = p;
  lots.type = 2;
  CHECK_FALSE(yc::engine::Document::CheckParameterValue(lots, Value::Number(1.5), &err));
  CHECK(t.d.RemoveParameter(t.sheet(), 1, 1));
  CHECK(t.d.SetInput(t.sheet(), 1, 1, "4"));
}

TEST_CASE("the change log records every change of a parameter cell, and is saved") {
  yctest::Doc t;
  t.Set("B2", "1.5");
  std::string err;
  REQUIRE(t.d.SetParameter(Spread(t.sheet()), &err));
  t.d.audit_user = "trader1";
  t.d.TakeChanges();
  t.d.param_commit = true;
  REQUIRE(t.d.SetInput(t.sheet(), 1, 1, "2"));
  t.d.param_commit = false;
  t.d.TakeChanges();
  t.d.python_call = true;
  t.d.SetValue(t.sheet(), 1, 1, Value::Number(3), yc::engine::kSrcPython);
  t.d.python_call = false;
  t.d.TakeChanges();
  // a change of another cell isn't logged; the same value again isn't either
  t.Set("C3", "9");
  t.d.TakeChanges();
  const auto& log = *t.d.wb().audit_log;
  REQUIRE(log.size() == 2);
  CHECK(log[0].kind == "parameter");
  CHECK(log[0].target == "Spread (Sheet1!B2)");
  CHECK(log[0].before == "1.5");
  CHECK(log[0].after == "2");
  CHECK(log[0].source == "user");
  CHECK(log[0].user == "trader1");
  CHECK(log[1].source == "python");
  CHECK(log[1].after == "3");
  CHECK(log[1].time > 1.7e9);
  yc::model::AuditEntry kill;
  kill.kind = "kill";
  kill.target = "Kill switch";
  t.d.AddAudit(kill);
  yc::model::SafetySettings st;
  st.start_read_only = true;
  st.has_pin = true;
  st.pin_hash = yc::model::PinHash("1234");
  st.kill_macro = "risk.flatten_all";
  t.d.SetSafetySettings(st);
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  CHECK(*r.wb.parameters == *t.d.wb().parameters);
  CHECK(*r.wb.audit_log == *t.d.wb().audit_log);
  CHECK(*r.wb.safety == st);
  CHECK(r.wb.safety->pin_hash != yc::model::PinHash("0000"));
  // reopened: the first change is logged with the value the cell had when the workbook was opened
  const size_t logged = r.wb.audit_log->size();
  yc::engine::Document d2(std::move(r.wb));
  d2.TakeChanges();
  d2.python_call = true;
  d2.SetValue(t.sheet(), 1, 1, Value::Number(4), yc::engine::kSrcPython);
  d2.python_call = false;
  d2.TakeChanges();
  const auto& log2 = *d2.wb().audit_log;
  REQUIRE(log2.size() == logged + 1);
  CHECK(log2.back().before == "3");
  CHECK(log2.back().after == "4");
}

TEST_CASE("parameter cells: paste and fill are refused like typing") {
  yctest::Doc t;
  t.Set("B2", "1.5");
  t.Set("B1", "7");
  t.Set("D4", "9");
  std::string err;
  REQUIRE(t.d.SetParameter(Spread(t.sheet()), &err));
  t.d.FillDown(t.sheet(), yc::core::Rect{0, 1, 2, 1});  // B1:B3
  CHECK(t.N("B2") == 1.5);
  const auto clip = t.d.Copy(t.sheet(), yc::core::Rect{3, 3, 3, 3});  // D4
  t.d.Paste(t.sheet(), 1, 1, clip);
  CHECK(t.N("B2") == 1.5);
}

TEST_CASE("alert rules are saved with the workbook, with every flag") {
  yctest::Doc t;
  yc::model::AlertRule a;
  a.name = "Delta limit";
  a.sheet = t.sheet();
  a.range = yc::core::Rect{4, 2, 9, 2};
  a.op = 7;
  a.a = -500;
  a.b = 500;
  a.sound = false;
  a.popup = true;
  a.flash_window = true;
  a.notify = true;
  a.color = 0xFFFF0000;
  a.cooldown_s = 2.5;
  t.d.SetAlert(a);
  yc::model::AlertRule b = a;
  b.name = "Quiet";
  b.notify = b.flash_window = b.popup = false;
  b.enabled = false;
  t.d.SetAlert(b);
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  REQUIRE(r.wb.alerts->size() == 2);
  CHECK((*r.wb.alerts)[0] == a);
  CHECK((*r.wb.alerts)[1] == b);
}
