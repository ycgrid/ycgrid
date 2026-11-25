// SPDX-License-Identifier: Apache-2.0
// Coverage of the text functions' less trodden paths (fn_text2.cpp): REGEX over text beyond ASCII (CJK, a character
// outside the BMP), BAHTTEXT past a million, IMAGE's and WEBSERVICE's argument checks (no network needed), FILTERXML's
// errors, and the IMAGE value's round trip.
#include <catch2/catch_test_macros.hpp>

#include "formula/functions.h"
#include "test_helpers.h"

TEST_CASE("REGEX functions over CJK text and a character outside the BMP") {
  yctest::Doc t;
  CHECK(t.Eval("REGEXEXTRACT(\"\u50F9\u683C123\u5143\",\"\\d+\")") == "123");
  CHECK(t.Eval("REGEXEXTRACT(\"\u53F0\u5317\u5E02 \u5927\u5B89\u5340\",\"\\S+\",1)") == "\u53F0\u5317\u5E02");
  CHECK(t.Eval("REGEXREPLACE(\"\U0001F600a\U0001F600\",\"a\",\"b\")") == "\U0001F600b\U0001F600");
  CHECK(t.Eval("LEN(REGEXREPLACE(\"\U0001F600a\",\"\U0001F600\",\"x\"))") == "2");
  CHECK(t.Eval("REGEXTEST(\"\u53F0\",\"\u53F0\")") == "TRUE");
  CHECK(t.Eval("REGEXEXTRACT(\"\u53F0\u5317\u5E02\",\"\u5317.\")") == "\u5317\u5E02");
  // a pattern std::regex can't read, and look-behind (PCRE only), are #VALUE!
  CHECK(t.Eval("REGEXEXTRACT(\"abc\",\"[\")") == "#VALUE!");
  CHECK(t.Eval("REGEXTEST(\"abc\",\"(\")") == "#VALUE!");
  CHECK(t.Eval("REGEXEXTRACT(\"abc\",\"(?<=a)b\")") == "#VALUE!");
  CHECK(t.Eval("REGEXEXTRACT(\"A1\",\"a\",0,1)") == "A");
  CHECK(t.Eval("REGEXEXTRACT(\"abc\",\"x\")") == "#N/A");
}

TEST_CASE("BAHTTEXT past a million and with satang") {
  yctest::Doc t;
  const std::string m = t.Eval("BAHTTEXT(1234567.25)");
  CHECK(m.rfind("\u0E2B\u0E19\u0E36\u0E48\u0E07\u0E25\u0E49\u0E32\u0E19", 0) == 0);  // one million...
  CHECK(m.find("\u0E2A\u0E15\u0E32\u0E07\u0E04\u0E4C") != std::string::npos);  // ...satang
  CHECK(t.Eval("BAHTTEXT(2000000)") == t.Eval("BAHTTEXT(2)").substr(0, t.Eval("BAHTTEXT(2)").find("\u0E1A")) + "\u0E25\u0E49\u0E32\u0E19\u0E1A\u0E32\u0E17\u0E16\u0E49\u0E27\u0E19");
  CHECK(t.Eval("BAHTTEXT(11)") != t.Eval("BAHTTEXT(21)"));
  CHECK(t.Eval("BAHTTEXT(\"x\")") == "#VALUE!");
}

TEST_CASE("IMAGE and WEBSERVICE check their arguments before anything is fetched") {
  yctest::Doc t;
  CHECK(t.Eval("ISTEXT(IMAGE(\"https://example.com/a.png\"))") == "TRUE");
  CHECK(t.Eval("IMAGE(\"ftp://example.com/a.png\")") == "#VALUE!");
  CHECK(t.Eval("IMAGE(\"https://example.com/a.png\",\"alt\",4)") == "#VALUE!");
  CHECK(t.Eval("IMAGE(\"https://example.com/a.png\",,3)") == "#VALUE!");       // custom size without a size
  CHECK(t.Eval("IMAGE(\"https://example.com/a.png\",,3,-1,5)") == "#VALUE!");
  CHECK(t.Eval("ISTEXT(IMAGE(\"https://example.com/a.png\",\"alt\",3,20,30))") == "TRUE");
  CHECK(t.Eval("WEBSERVICE(\"\")") == "#VALUE!");
  CHECK(t.Eval("WEBSERVICE(\"file:///c:/x.txt\")") == "#VALUE!");
  CHECK(t.Eval("WEBSERVICE(REPT(\"a\",3000))") == "#VALUE!");
  // the value a cell keeps for an image reads back to the same image
  yc::formula::ImageSpec spec;
  spec.url = "https://example.com/p.png";
  spec.alt = "chart";
  spec.sizing = 3;
  spec.height = 20;
  spec.width = 30.5;
  yc::formula::ImageSpec back;
  REQUIRE(yc::formula::ParseImageValue(yc::formula::MakeImageValue(spec), &back));
  CHECK(back.url == spec.url);
  CHECK(back.alt == "chart");
  CHECK(back.sizing == 3);
  CHECK(back.height == 20);
  CHECK(back.width == 30.5);
  CHECK_FALSE(yc::formula::ParseImageValue("plain text", &back));
}

TEST_CASE("FILTERXML's errors and PHONETIC of a range") {
  yctest::Doc t;
  CHECK(t.Eval("FILTERXML(\"<a><b>1</b></a>\",\"//[\")") == "#VALUE!");
  CHECK(t.Eval("FILTERXML(\"<a><b>1</b>\",\"//b\")") == "#VALUE!");
  CHECK(t.Eval("FILTERXML(\"<a><b>1</b></a>\",\"//c\")") == "#VALUE!");
  t.Set("A1", "x");
  t.Set("A2", "5");
  t.Set("A3", "y");
  CHECK(t.Eval("PHONETIC(A1:A3)") == "xy");  // text only, as Excel's
}
