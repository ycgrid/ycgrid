// SPDX-License-Identifier: Apache-2.0
// Round 3, trust: =PY in a workbook whose Python code the Trust Center keeps disabled
// gives #BLOCKED! and never reaches Python; a write made with Application.EnableEvents = False is calculated without
// reporting Worksheet_Calculate
#include <catch2/catch_test_macros.hpp>

#include "core/value.h"
#include "engine/document.h"
#include "engine/engine.h"
#include "test_helpers.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine = yc::engine;
using yc::core::Err;

namespace {
struct Recorder : engine::ExternalProvider {
  std::vector<std::string> names;
  void RequestUdf(const std::string&, const std::string& name, const std::vector<std::shared_ptr<yc::formula::Array>>&, uint32_t, uint32_t, uint32_t) override { names.push_back(name); }
  void Subscribe(const std::string&, const std::vector<yc::core::Value>&) override {}
};

struct Blocked {
  explicit Blocked(bool on) { engine::Document::python_blocked = on; }
  ~Blocked() { engine::Document::python_blocked = false; }
};
}  // namespace

TEST_CASE("Round 3 trust: #BLOCKED! is an error value with Excel's ERROR.TYPE 11") {
  Err e{};
  REQUIRE(yc::core::ParseErrorText("#BLOCKED!", &e));
  CHECK(e == Err::Blocked);
  CHECK(std::string(yc::core::ErrorText(Err::Blocked)) == "#BLOCKED!");
  yctest::Doc t;
  CHECK(t.Eval("ERROR.TYPE(#BLOCKED!)") == "11");
  CHECK(t.Eval("ISERROR(#BLOCKED!)") == "TRUE");
}

TEST_CASE("Round 3 trust: =PY gives #BLOCKED! while the workbook's code is disabled, with or without a host") {
  yctest::Doc t;
  {
    Blocked b(true);
    t.Set("A1", "=PY(\"1+1\")");
    REQUIRE(t.V("A1").IsError());
    CHECK(t.V("A1").error() == Err::Blocked);  // (without the fix: #NAME? and the app started a host to run it)
    Recorder host;
    t.d.provider = &host;  // a host without the project (the Immediate window): the workbook's =PY still doesn't run
    t.Set("A2", "=PY(\"2+2\")");
    t.Set("A3", "=MYFUNC(1)");
    REQUIRE(t.V("A2").IsError());
    CHECK(t.V("A2").error() == Err::Blocked);
    CHECK(host.names == std::vector<std::string>{"MYFUNC"});  // other functions are asked (the host answers #NAME?)
    t.d.provider = nullptr;
  }
  // enabled: no longer blocked (without a host here, an unknown function's #NAME?)
  t.Set("A1", "=PY(\"1+1\")");
  REQUIRE(t.V("A1").IsError());
  CHECK(t.V("A1").error() == Err::Name);
}

TEST_CASE("Round 3 trust: a write with events off is calculated at once and reports no Worksheet_Calculate") {
  yctest::Doc t;
  std::vector<uint32_t> calc, added;
  t.Set("A1", "1");
  t.Set("B1", "=A1*2");
  t.d.TakeSheetEvents(&calc, &added);
  t.d.SetInput(t.sheet(), 0, 0, "5");  // Range("A1").Value = 5 with Application.EnableEvents = False
  t.d.RecalcWithoutEvents();
  CHECK(t.N("B1") == 10);
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(calc.empty());  // (without the fix the next publish fired Calculate after the handler turned events back on)
  t.d.SetInput(t.sheet(), 0, 0, "6");  // events on: the usual calculation reports the sheet
  t.d.Recalc();
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(calc == std::vector<uint32_t>{t.sheet()});
  t.d.auto_calc = false;  // manual calculation: nothing is calculated now (F9 later, and Calculate fires then)
  t.d.SetInput(t.sheet(), 0, 0, "7");
  t.d.RecalcWithoutEvents();
  CHECK(t.N("B1") == 12);
}

TEST_CASE("Round 3 trust: every refused call is counted by the time Call returns") {
  // Call used to answer from inside the command, before the engine added its refusals to the BlockScope: the caller
  // sometimes read 0 (a refused write reached Python as done)
  engine::Engine eng(yc::model::Workbook::NewDefault("Sheet"));
  eng.Start();
  const uint32_t sheet = eng.Current()->wb.sheets[0]->id;
  eng.Call([](engine::Document& d) {
    d.protected_view = true;
    return 0;
  });
  int missed = 0;
  for (int i = 0; i < 2000; ++i) {
    auto sink = std::make_shared<std::atomic<uint32_t>>(0);
    engine::Engine::BlockScope scope(sink);
    eng.Call([sheet](engine::Document& d) { return d.SetInput(sheet, 0, 0, "1"); });
    if (sink->load() == 0) ++missed;
  }
  CHECK(missed == 0);
  int thrown = 0;
  try {
    eng.Call([](engine::Document&) -> int { throw std::runtime_error("x"); });
  } catch (const std::runtime_error&) {
    ++thrown;
  }
  CHECK(thrown == 1);  // an exception still reaches the caller
  eng.Stop();
}

TEST_CASE("Round 3 trust: a request counts only its own refusals, also when it replaces the workbook") {
  engine::Engine eng(yc::model::Workbook::NewDefault("Sheet"));
  eng.Start();
  const uint32_t sheet = eng.Current()->wb.sheets[0]->id;
  auto refused = std::make_shared<std::atomic<uint32_t>>(0);
  {
    engine::Engine::BlockScope scope(refused);  // a write refused (read-only here; a protected sheet or Protected View alike)
    eng.Call([sheet](engine::Document& d) {
      d.protected_view = true;
      d.SetInput(sheet, 0, 0, "1");
      return 0;
    });
  }
  CHECK(refused->load() >= 1);
  auto later = std::make_shared<std::atomic<uint32_t>>(0);
  {
    engine::Engine::BlockScope scope(later);  // the next request opens another workbook (Workbook.Close, Workbooks.Open)
    eng.Replace(yc::model::Workbook::NewDefault("Sheet"));
    eng.Sync();
  }
  eng.Stop();
  CHECK(later->load() == 0);  // (without the fix: 0 - 1 wrapped around and the close reported the old refusal)
}
