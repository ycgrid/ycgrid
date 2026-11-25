// SPDX-License-Identifier: Apache-2.0
// Coverage of the functions that download (WEBSERVICE, STOCKHISTORY, TRANSLATE: fn_online.cpp, fn_text2.cpp and the
// Document's WebFetch) through a fake NetProvider: nothing goes to the network. A cell waits while its download runs,
// shows the answer once SetUdfResult brings it, and one URL is fetched once. DETECTLANGUAGE's scripts work offline.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <map>

using yc::core::Value;

namespace {

struct FakeNet : yc::engine::NetProvider {
  std::vector<std::pair<std::string, std::string>> asked;  // (key, url)
  void Fetch(const std::string& key, const std::string& url) override { asked.emplace_back(key, url); }
};

// answers the last request with `body` and recalculates
void Answer(yctest::Doc& t, FakeNet& net, const std::string& body) {
  REQUIRE_FALSE(net.asked.empty());
  t.d.SetUdfResult(net.asked.back().first, Value::String(body));
  t.d.Recalc();
}

bool IsErr(const Value& v, yc::core::Err e) { return v.IsError() && v.error() == e; }

}  // namespace

TEST_CASE("Online coverage: WEBSERVICE without a provider, with a bad URL, waiting, answered, fetched once") {
  yctest::Doc t;
  t.Set("A1", "=WEBSERVICE(\"https://example.test/a\")");
  CHECK(IsErr(t.V("A1"), yc::core::Err::Value));  // no downloads in this process
  FakeNet net;
  t.d.net = &net;
  t.Set("A2", "=WEBSERVICE(\"ftp://example.test/a\")");
  CHECK(IsErr(t.V("A2"), yc::core::Err::Value));
  t.Set("A3", "=WEBSERVICE(\"\")");
  CHECK(IsErr(t.V("A3"), yc::core::Err::Value));
  CHECK(net.asked.empty());
  t.Set("B1", "=WEBSERVICE(\"https://example.test/b\")");
  REQUIRE(net.asked.size() == 1);
  CHECK(net.asked[0].second == "https://example.test/b");
  CHECK(t.V("B1").IsPending());
  Answer(t, net, "hello");
  CHECK(t.T("B1") == "hello");
  t.Set("B2", "=WEBSERVICE(\"https://example.test/b\")&\"!\"");  // the same URL: from the cache
  CHECK(net.asked.size() == 1);
  CHECK(t.T("B2") == "hello!");
  // more than 32767 characters: #VALUE!
  t.Set("C1", "=WEBSERVICE(\"https://example.test/big\")");
  Answer(t, net, std::string(40000, 'x'));
  CHECK(IsErr(t.V("C1"), yc::core::Err::Value));
}

TEST_CASE("Online coverage: STOCKHISTORY builds Yahoo's request and lays out the answer with dates") {
  yctest::Doc t;
  FakeNet net;
  t.d.net = &net;
  // two trading days: 2024-01-02 and 2024-01-03 at midnight UTC (serials 45293, 45294)
  const std::string body =
      R"({"chart":{"result":[{"meta":{"gmtoffset":0},"timestamp":[1704153600,1704240000,1704326400],)"
      R"("indicators":{"quote":[{"close":[1.234567,2,null],"volume":[100,200,null]}]}}]}})";
  t.Set("A1", "=STOCKHISTORY(\"msft\",DATE(2024,1,2),DATE(2024,1,3),0,1,0,1,5)");
  REQUIRE(net.asked.size() == 1);
  CHECK(net.asked[0].second.find("/chart/MSFT?") != std::string::npos);
  CHECK(net.asked[0].second.find("interval=1d") != std::string::npos);
  CHECK(t.V("A1").IsPending());
  Answer(t, net, body);
  CHECK(t.T("A1") == "Date");
  CHECK(t.T("B1") == "Close");
  CHECK(t.T("C1") == "Volume");
  CHECK(t.N("A2") == 45293);
  CHECK(t.T("A2") != "45293");  // the dates get a date format
  CHECK(t.N("B2") == 1.2346);   // prices to four decimals
  CHECK(t.N("C3") == 200);
  CHECK(t.V("A4").IsEmpty());   // the day after the end date is left out
  // a stock name row above the headers, the end before the start, weekly rows and the default columns
  t.Set("E1", "=STOCKHISTORY(\"XTAI:2330\",\"2024-01-03\",\"2024-01-01\",1,2)");
  CHECK(net.asked.back().second.find("/chart/2330.TW?") != std::string::npos);
  CHECK(net.asked.back().second.find("interval=1wk") != std::string::npos);
  Answer(t, net, body);
  CHECK(t.T("E1") == "XTAI:2330");
  CHECK(t.T("E2") == "Date");
  CHECK(t.T("F2") == "Close");
  CHECK(t.N("F3") == 1.2346);
  // without headers and with a Taiwan number alone; a London ticker; a monthly interval
  t.Set("H1", "=STOCKHISTORY(\"2330\",45293,45294,2,0,1)");
  CHECK(net.asked.back().second.find("/chart/2330.TW?") != std::string::npos);
  CHECK(net.asked.back().second.find("interval=1mo") != std::string::npos);
  Answer(t, net, body);
  CHECK(t.N("H1") == 1.2346);
  t.Set("J1", "=STOCKHISTORY(\"xlon:vod\",45293)");
  CHECK(net.asked.back().second.find("/chart/VOD.L?") != std::string::npos);
  t.Set("J2", "=STOCKHISTORY(\"XNAS:AAPL\",45293)");
  CHECK(net.asked.back().second.find("/chart/AAPL?") != std::string::npos);
  // answers that aren't a chart, or a chart without days: #VALUE!, never a read past what is there (a range without
  // trading days has no "timestamp", and the const lookup asserted)
  int k = 0;
  for (const char* bad : {"not json", "[1,2]", "\"text\"", R"({"chart":{}})", R"({"chart":{"result":[]}})",
                          R"({"chart":{"result":[{"meta":{"gmtoffset":0},"indicators":{"quote":[{}]}}]}})",
                          R"({"chart":{"result":[{"timestamp":[1704153600]}]}})",
                          R"({"chart":{"result":[{"timestamp":[1704153600],"indicators":{"quote":[]}}]}})",
                          R"({"chart":{"result":[{"timestamp":["x",1704153600],"indicators":{"quote":[{"close":[null,null]}]}}]}})"}) {
    INFO(bad);
    t.Set("L1", "=STOCKHISTORY(\"T" + std::to_string(++k) + "\",45293)");
    Answer(t, net, bad);
    CHECK(IsErr(t.V("L1"), yc::core::Err::Value));
  }
  // arguments refused before anything is fetched
  const size_t before = net.asked.size();
  for (const char* f : {"=STOCKHISTORY(\"\",45293)", "=STOCKHISTORY(\"MSFT\",\"someday\")", "=STOCKHISTORY(\"MSFT\",45293,\"x\")",
                        "=STOCKHISTORY(\"MSFT\",45293,45294,3)", "=STOCKHISTORY(\"MSFT\",45293,45294,0,3)",
                        "=STOCKHISTORY(\"MSFT\",45293,45294,0,1,6)", "=STOCKHISTORY(\"MSFT\",45293,45294,0,1,\"a\")"}) {
    t.Set("N1", f);
    CHECK(IsErr(t.V("N1"), yc::core::Err::Value));
  }
  CHECK(net.asked.size() == before);
}

TEST_CASE("Online coverage: TRANSLATE joins the answer's segments") {
  yctest::Doc t;
  FakeNet net;
  t.d.net = &net;
  t.Set("A1", "=TRANSLATE(\"good morning, world\",\"en\",\"es\")");
  REQUIRE(net.asked.size() == 1);
  CHECK(net.asked[0].second.find("sl=en&tl=es&q=good%20morning%2C%20world") != std::string::npos);
  Answer(t, net, R"([[["Buenos dias, ","good morning, "],["mundo","world"]],null,"en"])");
  CHECK(t.T("A1") == "Buenos dias, mundo");
  t.Set("A2", "=TRANSLATE(\"x\",\"\",\"\")");  // empty languages: detected, and the UI's language
  CHECK(net.asked.back().second.find("sl=auto&tl=en") != std::string::npos);
  Answer(t, net, "{}");
  CHECK(IsErr(t.V("A2"), yc::core::Err::Value));
  t.Set("A3", "=TRANSLATE(\"\")");
  CHECK(t.T("A3").empty());
  t.Set("A4", "=TRANSLATE(\"x\",\"a-very-long-language\")");
  CHECK(IsErr(t.V("A4"), yc::core::Err::Value));
}

TEST_CASE("Online coverage: DETECTLANGUAGE reads scripts and Latin languages offline") {
  yctest::Doc t;
  const std::map<std::string, std::string> cases = {
      {"\xE3\x81\x93\xE3\x82\x93\xE6\x97\xA5", "ja"},           // kana with a kanji
      {"\xED\x95\x9C\xEA\xB5\xAD", "ko"},                       // hangul
      {"\xD0\xBF\xD1\x80\xD0\xB8", "ru"},                       // Cyrillic
      {"\xD0\xBF\xD1\x97", "uk"},                               // with a Ukrainian letter
      {"\xD0\xBF\xD1\x9E", "be"},                               // with a Belarusian letter
      {"\xD0\xBF\xD1\x9B", "sr"},                               // with a Serbian letter
      {"\xCE\xB1\xCE\xB2", "el"},                               // Greek
      {"\xD7\x90\xD7\x91", "he"},                               // Hebrew
      {"\xD8\xA8\xD8\xA7", "ar"},                               // Arabic
      {"\xD9\xBE\xD8\xA7", "fa"},                               // with a Persian letter
      {"\xD9\xB9\xD8\xA7", "ur"},                               // with an Urdu letter
      {"\xE0\xB8\x81\xE0\xB8\x82", "th"},                       // Thai
      {"\xE0\xA4\x95\xE0\xA4\x96", "hi"},                       // Devanagari
      {"\xE0\xA6\x95", "bn"},                                   // Bengali
      {"\xE0\xAE\x95", "ta"},                                   // Tamil
      {"\xE1\x83\x90", "ka"},                                   // Georgian
      {"\xD5\xA1", "hy"},                                       // Armenian
      {"\xE4\xBB\xAC\xE8\xBF\x99", "zh-Hans"},                  // simplified Chinese
      {"le chat est dans la maison", "fr"},
      {"der Hund und die Katze", "de"},
      {"el perro y el gato", "es"},
      {"och det \xC3\xA4r en katt", "sv"},
  };
  for (const auto& [text, code] : cases) {
    INFO(code);
    CHECK(t.Eval("DETECTLANGUAGE(\"" + text + "\")") == code);
  }
  t.Set("A1", "=DETECTLANGUAGE(\"123 456\")");
  CHECK(IsErr(t.V("A1"), yc::core::Err::Value));
}
