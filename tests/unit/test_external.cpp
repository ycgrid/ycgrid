// SPDX-License-Identifier: Apache-2.0
// references to other workbooks: =[Book2.ycg]Sheet1!A1
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/external.h"
#include "io/ycg.h"

namespace engine = yc::engine;
namespace io = yc::io;
using yc::core::Value;

TEST_CASE("external links: references, cached values, text, saving and breaking") {
  yctest::Doc t;
  t.d.wb_mut().path = "C:\\data\\Main.ycg";
  REQUIRE(t.Set("A1", "=[Book2.ycg]Sheet1!B2*2"));
  REQUIRE(t.Set("A2", "=SUM('[Book2.ycg]Sheet1'!B2:B4)"));
  REQUIRE(t.Set("A3", "='D:\\other\\[Prices.xlsx]Rates 2024'!A1"));
  const auto& links = *t.d.wb().external_links;
  REQUIRE(links.size() == 2);
  CHECK(links[0].path == "C:\\data\\Book2.ycg");  // a file name alone: this workbook's folder
  CHECK(links[0].sheets == std::vector<std::string>{"Sheet1"});
  CHECK(links[1].path == "D:\\other\\Prices.xlsx");
  CHECK(t.N("A1") == 0);  // nothing read yet
  // the source's values arrive
  std::map<uint64_t, Value> v;
  v[(uint64_t{1} << 16) | 1] = Value::Number(10);
  v[(uint64_t{2} << 16) | 1] = Value::Number(20);
  v[(uint64_t{3} << 16) | 1] = Value::Number(30);
  t.d.SetExternalValues(0, {v}, 1700000000, "");
  CHECK(t.N("A1") == 20);
  CHECK(t.N("A2") == 60);
  // the text: the source in the same folder is written by name, another folder with its path
  CHECK(t.F("A1") == "='[Book2.ycg]Sheet1'!B2*2");
  CHECK(t.F("A3") == "='D:\\other\\[Prices.xlsx]Rates 2024'!A1");
  // an update with an error keeps the values
  t.d.SetExternalValues(0, {}, 0, "not found");
  CHECK(t.N("A1") == 20);
  CHECK((*t.d.wb().external_links)[0].error == "not found");
  // saved with the workbook: the formulas still find their values
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.external_links->size() == 2);
  yc::engine::Document d2(std::move(res.wb));
  d2.Recalc();
  const uint32_t s2 = d2.wb().sheets[0]->id;
  CHECK(d2.GetValue(s2, 0, 0).AsDouble() == 20);
  CHECK(d2.GetValue(s2, 1, 0).AsDouble() == 60);
  // Break Link: the formulas become values
  t.d.BreakLink(0);
  CHECK(t.F("A1") == "20");
  CHECK(t.F("A2") == "60");
  CHECK(t.F("A3").rfind("=", 0) == 0);  // the other link stays
}

TEST_CASE("=PY without a Python host asks for one and waits instead of #NAME?") {
  int asked = 0;
  engine::Document::on_python_needed = [&asked] { ++asked; };
  yctest::Doc t;  // no provider: the host isn't running
  REQUIRE(t.Set("A1", "1"));
  REQUIRE(t.Set("B1", "=PY(\"x.sum()\", A1)"));
  REQUIRE(t.Set("B2", "=PY(\"x.mean()\", A1)"));
  CHECK(t.V("B1").IsPending());
  CHECK(t.V("B2").IsPending());
  CHECK(asked == 1);  // once until the host's start clears the UDF cache
  REQUIRE(t.Set("C1", "=MYUDF(A1)"));  // other Python functions need the host's module: #NAME? as before
  CHECK(t.V("C1").IsError());
  t.d.InvalidateUdfCache();
  REQUIRE(t.Set("A1", "2"));
  CHECK(asked == 2);
  engine::Document::on_python_needed = nullptr;
}
