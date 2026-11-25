// SPDX-License-Identifier: Apache-2.0
// AutoFill as Excel 16 does it (measured through COM, Value2 read back): blanks among the source cells, dates by
// days, weekdays, months and years, times, uneven series. Each row is a source in column A (or row 1), the fill and
// the values Excel left in the whole destination, blank cells as "_".
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "test_helpers.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using yc::core::Rect;
using yc::core::Value;
namespace model = yc::model;

namespace {

constexpr const char* D = "yyyy-mm-dd";

struct FillCase {
  const char* name;
  std::vector<std::string> seeds;  // "" blank; numbers as numbers, other text as text
  const char* fmt;                 // the seeds' number format, or nullptr
  int type;                        // XlAutoFillType
  const char* dir;                 // down, up, right, left
  int n;                           // cells in the destination
  const char* excel;               // what Excel left in the destination, cell by cell
};

std::vector<std::string> Fill(const FillCase& c) {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const bool vertical = std::string(c.dir) == "down" || std::string(c.dir) == "up";
  const bool back = std::string(c.dir) == "up" || std::string(c.dir) == "left";
  const uint32_t k = static_cast<uint32_t>(c.seeds.size()), n = static_cast<uint32_t>(c.n);
  const uint32_t first = back ? n - k : 0;
  auto cell = [&](uint32_t i) { return vertical ? std::make_pair(i, 0u) : std::make_pair(0u, i); };
  const uint32_t nf = c.fmt ? t.d.wb_mut().styles.mut().InternString(c.fmt) : 0;
  for (uint32_t i = 0; i < k; ++i) {
    const auto [r, col] = cell(first + i);
    if (c.fmt) t.d.ModifyStyle(sh, Rect::Cell(r, col), [nf](model::Style& s) { s.numfmt = nf; });
    const std::string& s = c.seeds[i];
    if (s.empty()) continue;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    t.d.SetValue(sh, r, col, end && *end == 0 ? Value::Number(v) : Value::String(s));
  }
  const auto [r0, c0] = cell(first);
  const auto [r1, c1] = cell(first + k - 1);
  const auto [d0r, d0c] = cell(0);
  const auto [d1r, d1c] = cell(n - 1);
  t.d.AutoFill(sh, Rect{r0, c0, r1, c1}, Rect{d0r, d0c, d1r, d1c}, c.type);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < n; ++i) {
    const auto [r, col] = cell(i);
    const Value& v = t.d.GetValue(sh, r, col);
    if (v.IsEmpty()) out.push_back("_");
    else if (v.IsNumber()) {
      std::ostringstream os;
      os.precision(15);
      os << v.number();
      out.push_back(os.str());
    } else if (v.IsBool()) out.push_back(v.boolean() ? "True" : "False");
    else out.push_back(std::string(v.str()));
  }
  return out;
}

bool Same(const std::string& got, const std::string& want) {
  if (got == want) return true;
  char* e1 = nullptr;
  char* e2 = nullptr;
  const double a = std::strtod(got.c_str(), &e1), b = std::strtod(want.c_str(), &e2);
  if (!e1 || *e1 || !e2 || *e2 || got == "_" || want == "_") return false;
  return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

void Check(const std::vector<FillCase>& cases) {
  for (const auto& c : cases) {
    std::vector<std::string> want;
    std::istringstream is(c.excel);
    for (std::string w; is >> w;) want.push_back(w);
    const auto got = Fill(c);
    bool ok = got.size() == want.size();
    for (size_t i = 0; ok && i < got.size(); ++i) ok = Same(got[i], want[i]);
    std::string g;
    for (const auto& s : got) g += s + " ";
    INFO(c.name << ": YCGrid " << g << "| Excel " << c.excel);
    CHECK(ok);
  }
}

}  // namespace

TEST_CASE("Basics fill: blanks repeat, and the cells around them form one series (measured)") {
  Check({
      {"1,_,5", {"1", "", "5"}, nullptr, 0, "down", 8, "1 _ 5 9 _ 13 17 _"},
      {"1,_,5 up", {"1", "", "5"}, nullptr, 0, "up", 8, "_ -11 -7 _ -3 1 _ 5"},
      {"1,_,5 right", {"1", "", "5"}, nullptr, 0, "right", 8, "1 _ 5 9 _ 13 17 _"},
      {"1,_,5 left", {"1", "", "5"}, nullptr, 0, "left", 8, "_ -11 -7 _ -3 1 _ 5"},
      {"1,_,5 series", {"1", "", "5"}, nullptr, 2, "down", 8, "1 _ 5 9 _ 13 17 _"},
      {"1,_,3,_", {"1", "", "3", ""}, nullptr, 0, "down", 8, "1 _ 3 _ 5 _ 7 _"},
      {"1,_,_,4", {"1", "", "", "4"}, nullptr, 0, "down", 10, "1 _ _ 4 7 _ _ 10 13 _"},
      {"_,1,3", {"", "1", "3"}, nullptr, 0, "down", 8, "_ 1 3 _ 5 7 _ 9"},
      {"_,1,_,3", {"", "1", "", "3"}, nullptr, 0, "down", 10, "_ 1 _ 3 _ 5 _ 7 _ 9"},
      {"1,_", {"1", ""}, nullptr, 0, "down", 8, "1 _ 2 _ 3 _ 4 _"},
      {"5,_,5", {"5", "", "5"}, nullptr, 0, "down", 8, "5 _ 5 5 _ 5 5 _"},
      {"1,_,3,_,5", {"1", "", "3", "", "5"}, nullptr, 0, "down", 10, "1 _ 3 _ 5 7 _ 9 _ 11"},
      // the best-fit line by order when the blanks are only after the numbers
      {"1,2,4,_", {"1", "2", "4", ""}, nullptr, 0, "down", 10,
       "1 2 4 _ 5.33333333333333 6.83333333333333 8.33333333333333 _ 9.83333333333333 11.3333333333333"},
      // on the line through their places when only that is straight
      {"1,2,_,4", {"1", "2", "", "4"}, nullptr, 0, "down", 10, "1 2 _ 4 5 6 _ 8 9 10"},
      // otherwise the best-fit line over the places, blank places filled too
      {"2,_,4,_,8", {"2", "", "4", "", "8"}, nullptr, 0, "down", 10,
       "2 _ 4 _ 8 9.16666666666667 10.6666666666667 12.1666666666667 13.6666666666667 15.1666666666667"},
      {"2,_,4,_,8 series", {"2", "", "4", "", "8"}, nullptr, 2, "down", 10,
       "2 _ 4 _ 8 9.16666666666667 10.6666666666667 12.1666666666667 13.6666666666667 15.1666666666667"},
      {"1,_,2,4", {"1", "", "2", "4"}, nullptr, 0, "down", 10,
       "1 _ 2 4 4.5 5.42857142857143 6.35714285714286 7.28571428571429 8.21428571428571 9.14285714285714"},
      {"1,3,_,5,7", {"1", "3", "", "5", "7"}, nullptr, 0, "down", 10, "1 3 _ 5 7 8.2 9.6 11 12.4 13.8"},
      {"1,_,3,5", {"1", "", "3", "5"}, nullptr, 0, "down", 10,
       "1 _ 3 5 6 7.28571428571429 8.57142857142857 9.85714285714286 11.1428571428571 12.4285714285714"},
      // linear and growth trends: the line (curve) over the places, every cell
      {"1,_,5 linear", {"1", "", "5"}, nullptr, 9, "down", 8, "1 _ 5 7 9 11 13 15"},
      {"1,_,5 growth", {"1", "", "5"}, nullptr, 10, "down", 8,
       "1 _ 5 11.1803398874989 25 55.9016994374947 125 279.508497187474"},
      {"1,_,5 days", {"1", "", "5"}, nullptr, 5, "down", 8, "1 _ 5 1 _ 5 1 _"},
      // text breaks a run of numbers, blanks don't
      {"1,a,_,3", {"1", "a", "", "3"}, nullptr, 0, "down", 8, "1 a _ 3 2 a _ 4"},
      {"1,_,5,a", {"1", "", "5", "a"}, nullptr, 0, "down", 8, "1 _ 5 a 9 _ 13 a"},
      {"1,_,5,_,x", {"1", "", "5", "", "x"}, nullptr, 0, "down", 10, "1 _ 5 _ x 9 _ 13 _ x"},
      {"1,a,3,_,5", {"1", "a", "3", "", "5"}, nullptr, 0, "down", 10, "1 a 3 _ 5 2 a 7 _ 9"},
      {"1,_,a,2", {"1", "", "a", "2"}, nullptr, 0, "down", 10, "1 _ a 2 2 _ a 3 3 _"},
      {"1,2,_,a", {"1", "2", "", "a"}, nullptr, 0, "down", 10, "1 2 _ a 3 4 _ a 5 6"},
      {"a,_,1", {"a", "", "1"}, nullptr, 0, "down", 8, "a _ 1 a _ 2 a _"},
  });
}

TEST_CASE("Basics fill: text series must be evenly spaced, blanks among them repeat (measured)") {
  Check({
      {"Item1,_,Item3", {"Item1", "", "Item3"}, nullptr, 0, "down", 8, "Item1 _ Item3 Item5 _ Item7 Item9 _"},
      {"Jan,_,Mar", {"Jan", "", "Mar"}, nullptr, 0, "down", 8, "Jan _ Mar May _ Jul Sep _"},
      {"Jan,_,Mar series", {"Jan", "", "Mar"}, nullptr, 2, "down", 8, "Jan _ Mar May _ Jul Sep _"},
      {"Jan,_", {"Jan", ""}, nullptr, 0, "down", 8, "Jan _ Feb _ Mar _ Apr _"},
      {"Item1,Item2,_,Item4", {"Item1", "Item2", "", "Item4"}, nullptr, 0, "down", 10,
       "Item1 Item2 _ Item4 Item5 Item6 _ Item8 Item9 Item10"},
      {"Item1,_,Item3,Item5", {"Item1", "", "Item3", "Item5"}, nullptr, 0, "down", 10,
       "Item1 _ Item3 Item5 Item1 _ Item3 Item5 Item1 _"},
      {"Item1,_,Item2,_,Item4", {"Item1", "", "Item2", "", "Item4"}, nullptr, 0, "down", 10,
       "Item1 _ Item2 _ Item4 Item1 _ Item2 _ Item4"},
      {"Item1,Item2,Item4", {"Item1", "Item2", "Item4"}, nullptr, 0, "down", 10,
       "Item1 Item2 Item4 Item1 Item2 Item4 Item1 Item2 Item4 Item1"},
      {"Jan,Feb,Apr", {"Jan", "Feb", "Apr"}, nullptr, 0, "down", 8, "Jan Feb Apr Jan Feb Apr Jan Feb"},
      {"Jan,Feb,Apr series", {"Jan", "Feb", "Apr"}, nullptr, 2, "down", 8, "Jan Feb Apr Jan Feb Apr Jan Feb"},
      {"Q1,Q2,Q4", {"Q1", "Q2", "Q4"}, nullptr, 0, "down", 8, "Q1 Q2 Q4 Q1 Q2 Q4 Q1 Q2"},
  });
}

TEST_CASE("Basics fill: times step by hours, dates repeat when they aren't a series (measured)") {
  Check({
      {"9:00 series", {"0.375"}, "h:mm", 2, "down", 8,
       "0.375 0.416666666666667 0.458333333333333 0.5 0.541666666666667 0.583333333333333 0.625 0.666666666666667"},
      {"9:00 series up", {"0.375"}, "h:mm", 2, "up", 8,
       "0.0833333333333333 0.125 0.166666666666667 0.208333333333333 0.25 0.291666666666667 0.333333333333333 0.375"},
      {"9:00 AM series", {"0.375"}, "h:mm AM/PM", 2, "down", 4, "0.375 0.416666666666667 0.458333333333333 0.5"},
      {"day 1 9:00 series", {"1.375"}, "h:mm", 2, "down", 3, "1.375 1.41666666666667 1.45833333333333"},
      {"9:00,_", {"0.375", ""}, "h:mm", 0, "down", 6, "0.375 _ 0.416666666666667 _ 0.458333333333333 _"},
      {"9:00 linear", {"0.375"}, "h:mm", 9, "down", 3, "0.375 0.375 0.375"},
      {"9:00 days", {"0.375"}, "h:mm", 5, "down", 3, "0.375 0.375 0.375"},
      {"9:00 months", {"0.375"}, "h:mm", 7, "down", 3, "0.375 0.375 0.375"},
      {"9:00 growth", {"0.375"}, "h:mm", 10, "down", 3, "0.375 0.375 0.375"},
      {"9:00,12:00,18:00", {"0.375", "0.5", "0.75"}, "h:mm", 0, "down", 8, "0.375 0.5 0.75 0.375 0.5 0.75 0.375 0.5"},
      {"date linear", {"45292"}, D, 9, "down", 3, "45292 45292 45292"},
      {"date growth", {"45292"}, D, 10, "down", 3, "45292 45292 45292"},
      {"two dates linear", {"45292", "45293"}, D, 9, "down", 4, "45292 45293 45292 45293"},
      {"Jan 1, Feb 1 linear", {"45292", "45323"}, D, 9, "down", 4, "45292 45323 45292 45323"},
      {"Jan 1, 2, 4", {"45292", "45293", "45295"}, D, 0, "down", 8, "45292 45293 45295 45292 45293 45295 45292 45293"},
      {"Jan 1, 2, 4 series", {"45292", "45293", "45295"}, D, 2, "down", 5, "45292 45293 45295 45292 45293"},
      {"Jan 1, Feb 1, Apr 1", {"45292", "45323", "45383"}, D, 0, "down", 5, "45292 45323 45383 45292 45323"},
      {"Jan 1,_,Jan 3", {"45292", "", "45294"}, D, 0, "down", 8, "45292 _ 45294 45296 _ 45298 45300 _"},
      {"Jan 1,2,_,4", {"45292", "45293", "", "45295"}, D, 0, "down", 10,
       "45292 45293 _ 45295 45296 45297 _ 45299 45300 45301"},
      {"Jan 1,_,3,5", {"45292", "", "45294", "45296"}, D, 0, "down", 10,
       "45292 _ 45294 45296 45292 _ 45294 45296 45292 _"},
      {"Jan 1,_,Feb 1", {"45292", "", "45323"}, D, 0, "down", 8, "45292 _ 45323 45352 _ 45383 45413 _"},
      {"Jan 1,_", {"45292", ""}, D, 0, "down", 6, "45292 _ 45293 _ 45294 _"},
      {"Apr 30,_ mmm-yy", {"45412", ""}, "mmm-yy", 0, "down", 6, "45412 _ 45443 _ 45473 _"},
      {"Apr 30, May 31", {"45412", "45443"}, D, 0, "down", 5, "45412 45443 45473 45504 45535"},
      // a date and time a month apart keeps the time; a day and three hours apart steps 1.125 days
      {"Jan 1 9:00, Feb 1 9:00", {"45292.375", "45323.375"}, "yyyy-mm-dd h:mm", 0, "down", 4,
       "45292.375 45323.375 45352.375 45383.375"},
      {"Jan 1 9:00, Jan 2 12:00", {"45292.375", "45293.5"}, "yyyy-mm-dd h:mm", 0, "down", 5,
       "45292.375 45293.5 45294.625 45295.75 45296.875"},
  });
}

TEST_CASE("Basics fill: Fill Days, Weekdays, Months and Years of dates (measured)") {
  Check({
      // the time of day is dropped
      {"Jan 1 9:00 days", {"45292.375"}, "yyyy-mm-dd h:mm", 5, "down", 3, "45292.375 45293 45294"},
      {"Jan 1 9:00 months", {"45292.375"}, "yyyy-mm-dd h:mm", 7, "down", 3, "45292.375 45323 45352"},
      {"Jan 1 9:00 years", {"45292.375"}, "yyyy-mm-dd h:mm", 8, "down", 3, "45292.375 45658 46023"},
      {"Jan 1 9:00, Jan 2 9:00 weekdays", {"45292.375", "45293.375"}, "yyyy-mm-dd h:mm", 6, "down", 4,
       "45292.375 45293.375 45294 45295"},
      // one date
      {"Feb 29 months", {"45351"}, D, 7, "down", 8, "45351 45382 45412 45443 45473 45504 45535 45565"},
      {"Feb 29 months up", {"45351"}, D, 7, "up", 8, "45138 45169 45199 45230 45260 45291 45322 45351"},
      {"Feb 28 2023 months", {"44985"}, D, 7, "down", 4, "44985 45016 45046 45077"},
      {"Jan 30 months", {"45321"}, D, 7, "down", 4, "45321 45351 45381 45412"},
      {"Feb 28 2024 months", {"45350"}, D, 7, "down", 3, "45350 45379 45410"},
      {"Feb 28 2023 years", {"44985"}, D, 8, "down", 3, "44985 45350 45716"},
      {"Feb 29 years", {"45351"}, D, 8, "down", 5, "45351 45716 46081 46446 46812"},
      {"Sun weekdays", {"45298"}, D, 6, "down", 8, "45298 45299 45300 45301 45302 45303 45306 45307"},
      // two dates a day apart: each moves a month (year) a round
      {"Jan 1, 2 months", {"45292", "45293"}, D, 7, "down", 8, "45292 45293 45323 45324 45352 45353 45383 45384"},
      {"Jan 1, 2 months up", {"45292", "45293"}, D, 7, "up", 8, "45200 45201 45231 45232 45261 45262 45292 45293"},
      {"Jan 1, 2 years", {"45292", "45293"}, D, 8, "down", 8, "45292 45293 45658 45659 46023 46024 46388 46389"},
      {"Jan 1, 2, 3 months", {"45292", "45293", "45294"}, D, 7, "down", 8, "45292 45293 45294 45323 45324 45325 45352 45353"},
      {"Jan 1, 1 months", {"45292", "45292"}, D, 7, "down", 4, "45292 45292 45323 45323"},
      {"Jan 1, 1 years", {"45292", "45292"}, D, 8, "down", 4, "45292 45292 45658 45658"},
      {"Jan 2, 1 months", {"45293", "45292"}, D, 7, "down", 6, "45293 45292 45324 45323 45353 45352"},
      // a month apart: by months, Days keeps the 31 days, Years moves each a year a round
      {"Jan 1, Feb 1 days", {"45292", "45323"}, D, 5, "down", 8, "45292 45323 45354 45385 45416 45447 45478 45509"},
      {"Jan 1, Feb 1 years", {"45292", "45323"}, D, 8, "down", 8, "45292 45323 45658 45689 46023 46054 46388 46419"},
      {"Jan 1, Feb 1 years up", {"45292", "45323"}, D, 8, "up", 8, "44197 44228 44562 44593 44927 44958 45292 45323"},
      {"Feb 1, Jan 1 months", {"45323", "45292"}, D, 7, "down", 4, "45323 45292 45261 45231"},
      {"Jan 1, Feb 1, Mar 1 days", {"45292", "45323", "45352"}, D, 5, "down", 5, "45292 45323 45352 45383 45413"},
      {"Jan 1, 2, 4 months up", {"45292", "45293", "45295"}, D, 7, "up", 8, "45293 45295 45292 45293 45295 45292 45293 45295"},
      {"Jan 1, Feb 1, Apr 1 months", {"45292", "45323", "45383"}, D, 7, "down", 5, "45292 45323 45383 45292 45323"},
      {"Jan 1, 2, 4 days", {"45292", "45293", "45295"}, D, 5, "down", 5, "45292 45293 45295 45292 45293"},
      {"Jan 1, 3, 5 days", {"45292", "45294", "45296"}, D, 5, "down", 5, "45292 45294 45296 45298 45300"},
      {"Jan 1 2024, 2026 years", {"45292", "46023"}, D, 8, "down", 4, "45292 46023 46753 47484"},
      {"Jan 1 2024, Mar 1 2026 years", {"45292", "46082"}, D, 8, "down", 6, "45292 46082 45658 46447 46023 46813"},
      {"Jan 1, 2026, 2027 years", {"45292", "46023", "46388"}, D, 8, "down", 5, "45292 46023 46388 45292 46023"},
      {"Jan 31, Feb 29 years", {"45322", "45351"}, D, 8, "down", 6, "45322 45351 45688 45716 46053 46081"},
      // others move by as many days as the first did...
      {"Jan 15, Feb 1 months", {"45306", "45323"}, D, 7, "down", 8, "45306 45323 45337 45354 45366 45383 45397 45414"},
      {"Jan 31, Feb 1 months", {"45322", "45323"}, D, 7, "down", 8, "45322 45323 45351 45354 45382 45383 45412 45414"},
      {"Jan 31, Feb 28 months", {"45322", "45350"}, D, 7, "down", 6, "45322 45350 45351 45381 45382 45410"},
      {"Jan 1, Feb 29 months", {"45292", "45351"}, D, 7, "down", 8, "45292 45351 45323 45382 45352 45411 45383 45442"},
      {"Feb 29, Mar 15 months", {"45351", "45366"}, D, 7, "down", 8, "45351 45366 45382 45395 45412 45426 45443 45456"},
      {"Jan 1, Mar 10 months", {"45292", "45361"}, D, 7, "down", 6, "45292 45361 45323 45392 45352 45421"},
      {"Jan 1, Dec 31 months", {"45292", "45657"}, D, 7, "down", 8, "45292 45657 45323 45688 45352 45717 45383 45748"},
      {"Feb 20, Mar 10 years", {"45342", "45361"}, D, 8, "down", 4, "45342 45361 45708 45727"},
      {"Jan 1, Dec 31 years", {"45292", "45657"}, D, 8, "down", 6, "45292 45657 45658 46023 46023 46388"},
      {"Jan 1 2023, Feb 28 2023 years", {"44927", "44985"}, D, 8, "down", 6, "44927 44985 45292 45350 45658 45716"},
      // ...except a date in the first one's month, which moves as the first does
      {"Jan 1, Jan 31 months", {"45292", "45322"}, D, 7, "down", 8, "45292 45322 45323 45351 45352 45382 45383 45412"},
      {"Jan 15, Jan 31 months", {"45306", "45322"}, D, 7, "down", 4, "45306 45322 45337 45351"},
      {"Jan 1, Jan 30 months", {"45292", "45321"}, D, 7, "down", 8, "45292 45321 45323 45351 45352 45381 45383 45412"},
      {"Feb 1, Feb 29 months", {"45323", "45351"}, D, 7, "down", 8, "45323 45351 45352 45382 45383 45412 45413 45443"},
      {"Feb 10, Feb 28 2023 months", {"44967", "44985"}, D, 7, "down", 6, "44967 44985 44995 45016 45026 45046"},
      // others count as days of the first's month: Mar 1 after Feb 29 is Feb 30, and a day up to 30 the new month
      // lacks is its last day (Feb 28, 2025; Feb 29, 2028)...
      {"Feb 29, Mar 1 years", {"45351", "45352"}, D, 8, "down", 10, "45351 45352 45716 45716 46081 46081 46446 46446 46812 46812"},
      {"Feb 29, Mar 1 years up", {"45351", "45352"}, D, 8, "up", 10, "43890 43890 44255 44255 44620 44620 44985 44985 45351 45352"},
      {"Feb 28, Mar 1 years", {"45350", "45352"}, D, 8, "down", 10, "45350 45352 45716 45716 46081 46081 46446 46446 46811 46812"},
      {"Feb 28, 29, Mar 1 years", {"45350", "45351", "45352"}, D, 8, "down", 12,
       "45350 45351 45352 45716 45716 45716 46081 46081 46081 46446 46446 46446"},
      {"Feb 28, Mar 1 2023 years", {"44985", "44986"}, D, 8, "down", 10, "44985 44986 45350 45351 45716 45716 46081 46081 46446 46446"},
      {"Feb 27, Mar 1 2023 years up", {"44984", "44986"}, D, 8, "up", 10, "43523 43524 43888 43890 44254 44255 44619 44620 44984 44986"},
      {"Feb 27, Mar 2 2023 years", {"44984", "44987"}, D, 8, "down", 10, "44984 44987 45349 45351 45715 45716 46080 46081 46445 46446"},
      {"Feb 28, Mar 1 months", {"45350", "45352"}, D, 7, "down", 28,
       "45350 45352 45379 45381 45410 45412 45440 45442 45471 45473 45501 45503 45532 45534 45563 45565 45593 45595 45624 45626 "
       "45654 45656 45685 45687 45716 45716 45744 45746"},
      // ...and day 31 or later runs on into the next month (Feb 31, 2025 is Mar 3; Sep 31 is Oct 1)
      {"Feb 29, Mar 2 years", {"45351", "45353"}, D, 8, "down", 10, "45351 45353 45716 45719 46081 46084 46446 46449 46812 46814"},
      {"Feb 26, Mar 3 2023 years", {"44983", "44988"}, D, 8, "down", 10, "44983 44988 45348 45353 45714 45719 46079 46084 46444 46449"},
      {"Sep 30, Oct 1 years", {"45565", "45566"}, D, 8, "down", 10, "45565 45566 45930 45931 46295 46296 46660 46661 47026 47027"},
      {"Feb 29, Mar 2 months", {"45351", "45353"}, D, 7, "down", 10, "45351 45353 45382 45382 45412 45413 45443 45443 45473 45474"},
      // Fill Years of month ends keeps them month ends
      {"Jan 31, Feb 28 2023 years", {"44957", "44985"}, D, 8, "down", 10, "44957 44985 45322 45351 45688 45716 46053 46081 46418 46446"},
      {"Feb 28 2023, Feb 29 2024 years", {"44985", "45351"}, D, 8, "down", 10,
       "44985 45351 45716 46081 46446 46812 47177 47542 47907 48273"},
      // dates each whole months after the one before, without one step for them all, repeat whatever the fill
      {"Jan 31, Feb 29, Mar 29", {"45322", "45351", "45380"}, D, 0, "down", 12,
       "45322 45351 45380 45322 45351 45380 45322 45351 45380 45322 45351 45380"},
      {"Jan 31, Feb 29, Mar 29 days", {"45322", "45351", "45380"}, D, 5, "down", 12,
       "45322 45351 45380 45322 45351 45380 45322 45351 45380 45322 45351 45380"},
      {"Jan 31, Feb 29, Mar 29 months", {"45322", "45351", "45380"}, D, 7, "down", 12,
       "45322 45351 45380 45322 45351 45380 45322 45351 45380 45322 45351 45380"},
      {"Oct 31, Nov 30, Dec 30 years", {"45596", "45626", "45656"}, D, 8, "down", 12,
       "45596 45626 45656 45596 45626 45656 45596 45626 45656 45596 45626 45656"},
      {"Mar 31, Apr 30, May 30 weekdays", {"45382", "45412", "45442"}, D, 6, "down", 12,
       "45382 45412 45442 45382 45412 45442 45382 45412 45442 45382 45412 45442"},
      {"Jun 30, Jul 30, Sep 30 days up", {"45107", "45137", "45199"}, D, 5, "up", 12,
       "45107 45137 45199 45107 45137 45199 45107 45137 45199 45107 45137 45199"},
  });
}

TEST_CASE("Basics fill: Fill Weekdays from two dates steps days, weeks or months (measured)") {
  Check({
      {"Mon, Tue", {"45292", "45293"}, D, 6, "down", 8, "45292 45293 45294 45295 45296 45299 45300 45301"},
      {"Tue, Mon", {"45293", "45292"}, D, 6, "down", 8, "45293 45292 45289 45288 45287 45286 45285 45282"},
      {"Fri, Sat", {"45296", "45297"}, D, 6, "down", 8, "45296 45297 45300 45301 45302 45303 45306 45307"},
      {"Sat, Sun", {"45297", "45298"}, D, 6, "down", 8, "45297 45298 45300 45301 45302 45303 45306 45307"},
      {"Jan 1, 1", {"45292", "45292"}, D, 6, "down", 4, "45292 45292 45292 45292"},
      {"Mon, Wed", {"45292", "45294"}, D, 6, "down", 8, "45292 45294 45299 45301 45306 45308 45313 45315"},
      {"Mon, Wed up", {"45292", "45294"}, D, 6, "up", 8, "45271 45273 45278 45280 45285 45287 45292 45294"},
      {"Mon, Sat", {"45292", "45297"}, D, 6, "down", 8, "45292 45297 45299 45303 45306 45310 45313 45317"},
      {"Mon, Sun", {"45292", "45298"}, D, 6, "down", 6, "45292 45298 45299 45303 45306 45310"},
      {"Fri, Mon", {"45296", "45299"}, D, 6, "down", 8, "45296 45299 45303 45306 45310 45313 45317 45320"},
      {"Mon, Fri back", {"45299", "45296"}, D, 6, "down", 6, "45299 45296 45306 45303 45313 45310"},
      {"Mon, Mon", {"45292", "45299"}, D, 6, "down", 8, "45292 45299 45299 45306 45306 45313 45313 45320"},
      {"Jan 1, 3, 5", {"45292", "45294", "45296"}, D, 6, "down", 8, "45292 45294 45296 45299 45301 45303 45306 45308"},
      {"Jan 1, 2, 3", {"45292", "45293", "45294"}, D, 6, "down", 8, "45292 45293 45294 45295 45296 45299 45300 45301"},
      {"Jan 1, 9", {"45292", "45300"}, D, 6, "down", 8, "45292 45300 45323 45331 45352 45359 45383 45391"},
      {"Jan 1, 18", {"45292", "45309"}, D, 6, "down", 8, "45292 45309 45323 45338 45352 45369 45383 45400"},
      {"Jan 1, 31", {"45292", "45322"}, D, 6, "down", 10, "45292 45322 45323 45351 45352 45380 45383 45412 45413 45443"},
      {"Jan 1, Feb 10", {"45292", "45332"}, D, 6, "down", 8, "45292 45332 45323 45363 45352 45392 45383 45422"},
      {"Jan 1, Dec 23", {"45292", "45283"}, D, 6, "down", 8, "45292 45283 45323 45314 45352 45343 45383 45373"},
      {"Jan 1, Dec 31", {"45292", "45657"}, D, 6, "down", 8, "45292 45657 45323 45688 45352 45716 45383 45748"},
      {"Jan 15, Feb 1", {"45306", "45323"}, D, 6, "down", 8, "45306 45323 45337 45352 45366 45383 45397 45414"},
      {"Feb 29, Mar 15", {"45351", "45366"}, D, 6, "down", 8, "45351 45366 45380 45394 45412 45426 45443 45456"},
      {"Jan 31, Feb 15", {"45322", "45337"}, D, 6, "down", 8, "45322 45337 45351 45366 45380 45397 45412 45428"},
      // dates a month apart step by months, a weekend moved back to Friday
      {"Jan 1, Feb 1", {"45292", "45323"}, D, 6, "down", 8, "45292 45323 45352 45383 45413 45443 45474 45505"},
      {"Jan 1, Feb 1 up", {"45292", "45323"}, D, 6, "up", 8, "45107 45139 45170 45198 45231 45261 45292 45323"},
      {"Jul 1, Aug 1", {"45474", "45505"}, D, 6, "down", 10, "45474 45505 45534 45566 45597 45625 45658 45688 45716 45748"},
      {"Jan 15, Feb 15", {"45306", "45337"}, D, 6, "down", 8, "45306 45337 45366 45397 45427 45457 45488 45519"},
      {"Jan 1, Feb 1, Mar 1", {"45292", "45323", "45352"}, D, 6, "down", 8, "45292 45323 45352 45383 45413 45443 45474 45505"},
      {"Jan 1 2024, 2025, 2026", {"45292", "45658", "46023"}, D, 6, "down", 5, "45292 45658 46023 46388 46752"},
      {"Jan 1, 2, 4", {"45292", "45293", "45295"}, D, 6, "down", 5, "45292 45293 45295 45292 45293"},
      {"Jan 1,_,Jan 2", {"45292", "", "45293"}, D, 6, "down", 8, "45292 _ 45293 45294 _ 45295 45296 _"},
  });
}

TEST_CASE("AutoFill of seeds of several kinds, run by run, as Excel 16 does it (measured)") {
  struct Case {
    const char* dir;
    std::vector<std::string> seeds;  // typed into B1 down (B6 up from it)
    std::vector<std::string> want;   // the filled cells: a number, text, or "F" for a formula
    int type = 0;                    // XlAutoFillType
  };
  const Case cases[] = {
      {"down", {"Item0", "=NA()"}, {"Item1", "F", "Item2", "F", "Item3"}},
      {"down", {"5", "=A1"}, {"6", "F", "7", "F", "8"}},
      {"down", {"1", "2", "=A1"}, {"3", "4", "F", "5", "6"}},
      {"down", {"=1+1", "Mon"}, {"F", "Tue", "F", "Wed", "F"}},
      {"down", {"9:30", "a"}, {"0.4375", "a", "0.479166666666667", "a", "0.520833333333333"}},
      {"down", {"9:30", "a", "b"}, {"0.4375", "a", "b", "0.479166666666667", "a"}},
      {"down", {"1", "9:30"}, {"2", "0.4375", "3", "0.479166666666667", "4"}},
      {"down", {"9:30", "2024/1/1"}, {"0.4375", "45293", "0.479166666666667", "45294", "0.520833333333333"}},
      {"down", {"50%", "1,000"}, {"1.5", "1001", "2.5", "1002", "3.5"}},
      {"down", {"$5", "1,000"}, {"6", "1001", "7", "1002", "8"}},
      {"down", {"10%", "20.5%"}, {"0.31", "0.415", "0.52", "0.625", "0.73"}},
      {"down", {"1", "1,000"}, {"1999", "2998", "3997", "4996", "5995"}},
      {"down", {"1:00", "0:00"}, {"0.958333333333333", "0.916666666666667", "0.875", "0.833333333333334", "0.791666666666667"}},
      {"up", {"0:30", "a"}, {"0.9375", "a", "0.979166666666667", "a"}},
      {"down", {"12/2024", "a"}, {"45658", "a", "45689", "a", "45717"}},
      {"up", {"Item10", "Item2", "a99"}, {"Item18", "a98"}},
      {"down", {"Item-1", "1.5x", "1.5x"}, {"Item-2", "1.5x", "1.5x"}},
      {"down", {"1.5x", "=A1", "R2D2"}, {"1.6x", "F", "R2D3", "1.7x"}},
      // Fill Days (5), Weekdays (6), Months (7), Years (8), Linear (9), Growth (10), Values (4): texts repeat but the
      // day and month names the type steps; numbers among texts: by the unit (dates), by one (linear), or repeat
      {"down", {"Mon"}, {"Tue", "Wed", "Thu", "Fri", "Sat"}, 5},
      {"down", {"Mon"}, {"Tue", "Wed", "Thu", "Fri", "Mon"}, 6},
      {"down", {"Fri"}, {"Mon", "Tue", "Wed", "Thu", "Fri"}, 6},
      {"down", {"Mon", "Wed"}, {"Fri", "Tue", "Thu", "Mon", "Wed"}, 6},
      {"down", {"Jan"}, {"Feb", "Mar", "Apr", "May", "Jun"}, 7},
      {"down", {"Jan", "Mar"}, {"May", "Jul", "Sep", "Nov", "Jan"}, 7},
      {"down", {"Jan"}, {"Jan", "Jan", "Jan", "Jan", "Jan"}, 8},
      {"down", {"Jan"}, {"Jan", "Jan", "Jan", "Jan", "Jan"}, 5},
      {"down", {"Jan"}, {"Jan", "Jan", "Jan", "Jan", "Jan"}, 9},
      {"down", {"Monday"}, {"Monday", "Monday", "Monday", "Monday", "Monday"}, 7},
      {"down", {"Item1"}, {"Item1", "Item1", "Item1", "Item1", "Item1"}, 5},
      {"down", {"x-1"}, {"x-1", "x-1", "x-1", "x-1", "x-1"}, 5},
      {"down", {"Q4", "1a2"}, {"Q4", "1a2", "Q4", "1a2", "Q4"}, 7},
      {"down", {"Item1", "Item3"}, {"Item1", "Item3", "Item1", "Item3", "Item1"}, 9},
      {"down", {"5", "a"}, {"6", "a", "7", "a", "8"}, 9},
      {"down", {"1", "2", "a"}, {"3", "4", "a", "5", "6"}, 9},
      {"down", {"5", "a"}, {"5", "a", "5", "a", "5"}, 10},
      {"down", {"5", "a"}, {"5", "a", "5", "a", "5"}, 5},
      {"down", {"2024/1/31", "a"}, {"45351", "a", "45382", "a", "45412"}, 7},
      {"down", {"9:30", "a"}, {"0.395833333333333", "a", "0.395833333333333", "a", "0.395833333333333"}, 5},
      {"down", {"Item1", "a"}, {"Item2", "a", "Item3", "a", "Item4"}, 4},
      {"down", {"Item1", "a"}, {"Item2", "a", "Item3", "a", "Item4"}, 2},
      {"down", {"-5", "=ROW()"}, {"-4", "F", "-3", "F", "-2"}, 4},
      {"down", {"Mon", "Feb"}, {"Tue", "Feb", "Wed", "Feb", "Thu"}, 5},
      {"down", {"2023/12/31", "9:00"}, {"45292", "0.375"}, 6},
  };
  for (const Case& c : cases) {
    std::string name = std::string(c.dir) + " type " + std::to_string(c.type);
    for (const auto& sd : c.seeds) name += " " + sd;
    INFO(name);
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const bool up = std::string(c.dir) == "up";
    const uint32_t k = static_cast<uint32_t>(c.seeds.size()), n = k + static_cast<uint32_t>(c.want.size());
    const uint32_t first = up ? 10 - k : 0;  // the seeds' first row (0-based)
    for (uint32_t i = 0; i < k; ++i) t.Set(("B" + std::to_string(first + i + 1)).c_str(), c.seeds[i]);
    const Rect src{first, 1, first + k - 1, 1};
    const Rect dest = up ? Rect{10 - n, 1, 9, 1} : Rect{0, 1, n - 1, 1};
    t.d.AutoFill(sh, src, dest, c.type);
    t.d.Recalc();
    for (uint32_t i = 0; i < c.want.size(); ++i) {
      const uint32_t row = up ? 10 - n + i : k + i;
      const std::string at = "B" + std::to_string(row + 1);
      const std::string& w = c.want[i];
      INFO(at << " " << w);
      char* end = nullptr;
      const double num = std::strtod(w.c_str(), &end);
      if (w == "F") {
        CHECK(t.F(at.c_str()).rfind('=', 0) == 0);
      } else if (end && *end == 0) {
        REQUIRE(t.V(at.c_str()).IsNumber());
        CHECK(std::fabs(t.V(at.c_str()).number() - num) < 1e-9);
      } else {
        REQUIRE(t.V(at.c_str()).IsString());
        CHECK(std::string(t.V(at.c_str()).str()) == w);
      }
    }
  }
}

TEST_CASE("Fill Down, Fill Right and the fill handle carry data validation and no notes (Excel 16, measured)") {
  // measured 2026-11-19 through FillRight, FillDown and AutoFill: a list rule comes with the cell, its note doesn't;
  // a source without a rule takes the destination's away; two source cells, one with a rule, repeat that way; a
  // relative list =G1:G2 filled from H1 to H3 is =G3:G4 there. YCGrid's fills left the rules behind
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "Buy,Sell";
  auto dv = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return yc::engine::FindValidation(*t.d.S(sh), p.row, p.col);
  };
  for (const char* a : {"A6", "A10", "A14"}) {
    t.Set(a, "5");
    t.d.SetValidation(sh, yctest::Doc::Range(a), list);
    const auto p = yctest::Doc::P(a);
    t.d.SetNote(sh, p.row, p.col, "me", "n");
  }
  t.d.FillRight(sh, yctest::Doc::Range("A6:C6"));
  REQUIRE(dv("C6"));
  CHECK(dv("C6")->formula1 == "Buy,Sell");
  CHECK(t.d.S(sh)->notes->count((uint64_t{5} << 16) | 2) == 0);
  t.d.FillDown(sh, yctest::Doc::Range("A10:A12"));
  CHECK(dv("A12"));
  t.d.AutoFill(sh, yctest::Doc::Range("A14"), yctest::Doc::Range("A14:A16"));
  CHECK(dv("A16"));
  // a source without a rule takes the destination's away
  model::DvRule whole;
  whole.type = model::DvType::WholeNumber;
  whole.formula1 = "0";
  whole.formula2 = "9";
  t.d.SetValidation(sh, yctest::Doc::Range("C20"), whole);
  t.Set("C19", "1");
  t.d.FillDown(sh, yctest::Doc::Range("C19:C20"));
  CHECK_FALSE(dv("C20"));
  // a pattern of two source cells repeats
  t.d.SetValidation(sh, yctest::Doc::Range("E1"), list);
  t.Set("E1", "1");
  t.Set("E2", "2");
  t.d.AutoFill(sh, yctest::Doc::Range("E1:E2"), yctest::Doc::Range("E1:E6"));
  CHECK(dv("E3"));
  CHECK_FALSE(dv("E4"));
  CHECK(dv("E5"));
  CHECK_FALSE(dv("E6"));
  // a relative list moves with the cells: H3's items are G3:G4's
  t.Set("G1", "a");
  t.Set("G2", "b");
  t.Set("G3", "c");
  t.Set("G4", "d");
  model::DvRule rel;
  rel.type = model::DvType::List;
  rel.formula1 = "=G1:G2";
  t.d.SetValidation(sh, yctest::Doc::Range("H1"), rel);
  t.d.FillDown(sh, yctest::Doc::Range("H1:H3"));
  REQUIRE(dv("H3"));
  const auto items = yc::engine::ValidationList(t.d.wb(), sh, *dv("H3"), 2, 7);
  CHECK(items == std::vector<std::string>{"c", "d"});
  // one rule each: the rules grew, none was added (C20's went with its cell)
  CHECK(t.d.S(sh)->validations->size() == 5);
}
