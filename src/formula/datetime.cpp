// SPDX-License-Identifier: Apache-2.0
#include "formula/datetime.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace yc::formula {
namespace {

// Gregorian -> days since 1899-12-31 (without the 1900/2/29 bug)
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;  // since 1970-01-01
}

void CivilFromDays(int64_t z, int* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y = static_cast<int>(yy + (*m <= 2));
}

}  // namespace

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysInMonth(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && IsLeap(y)) return 29;
  return kDays[(m - 1) % 12];
}

namespace {
std::atomic<bool> g_1904{false};
}  // namespace

double DateSerial1900(int64_t year, int64_t month, int64_t day);

void SetDate1904(bool on) { g_1904.store(on, std::memory_order_relaxed); }
bool Date1904() { return g_1904.load(std::memory_order_relaxed); }
double DateOffset() { return Date1904() ? 1462.0 : 0.0; }

double DateSerial(int64_t year, int64_t month, int64_t day) {
  return DateSerial1900(year, month, day) - DateOffset();
}

double DateSerial1900(int64_t year, int64_t month, int64_t day) {
  if (year >= 0 && year <= 1899) year += 1900;
  // normalize month overflow
  int64_t y = year + (month - 1) / 12;
  int64_t mo = (month - 1) % 12;
  if (mo < 0) {
    mo += 12;
    --y;
  }
  const int64_t base = DaysFromCivil(y, mo + 1, 1) - DaysFromCivil(1899, 12, 31);
  int64_t serial = base + day - 1;
  // the 1900/2/29 bug: Excel's February 1900 has 29 days, so months from March 1900 on get +1
  // (DATE(1900,2,29) and DATE(1900,1,60) are 60, DATE(1900,3,0) is 60 too)
  if (base >= 60) ++serial;
  return static_cast<double>(serial);
}

bool SerialToYmd(double serial, int* y, int* m, int* d) {
  if (serial < 0) return false;
  serial += DateOffset();  // the 1904 system's serials, read in the 1900 one's
  if (serial >= 2958466) return false;  // after 9999-12-31
  int64_t s = static_cast<int64_t>(std::floor(serial));
  if (s == 0) {
    *y = 1900;
    *m = 1;
    *d = 0;
    return true;
  }
  if (s == 60) {
    *y = 1900;
    *m = 2;
    *d = 29;
    return true;
  }
  if (s > 60) --s;
  CivilFromDays(s + DaysFromCivil(1899, 12, 31), y, m, d);
  return true;
}

void SerialToHms(double serial, int* h, int* mi, int* s, int* ms) {
  double frac = serial - std::floor(serial);
  int64_t total_ms = static_cast<int64_t>(std::llround(frac * 86400000.0));
  if (total_ms >= 86400000) total_ms = 86399999;
  *h = static_cast<int>(total_ms / 3600000);
  *mi = static_cast<int>(total_ms / 60000 % 60);
  *s = static_cast<int>(total_ms / 1000 % 60);
  *ms = static_cast<int>(total_ms % 1000);
}

int Weekday(double serial) {
  int64_t s = static_cast<int64_t>(std::floor(serial + DateOffset()));
  // serial 1 (1900/1/1) is a Sunday in Excel
  return static_cast<int>(((s - 1) % 7 + 7) % 7) + 1;
}

namespace {

bool ReadInt(std::string_view s, size_t* i, int* out, int max_digits = 4) {
  size_t j = *i;
  int v = 0, n = 0;
  while (j < s.size() && s[j] >= '0' && s[j] <= '9' && n < max_digits) {
    v = v * 10 + (s[j] - '0');
    ++j;
    ++n;
  }
  if (n == 0) return false;
  *out = v;
  *i = j;
  return true;
}

// a typed time's text as Excel reads it (Excel 16, measured): spaces around a colon don't count (9 : 30), one colon
// may end it (9:30:, 9:30 PM:) and so may a decimal point without digits (9:30., 1:2:3.)
std::string TidyTimeText(std::string_view s) {
  std::string t;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ' ') {
      size_t j = i;
      while (j < s.size() && s[j] == ' ') ++j;
      if ((j < s.size() && s[j] == ':') || (!t.empty() && t.back() == ':')) {
        i = j - 1;
        continue;
      }
    }
    t += s[i];
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  if (!t.empty() && t.back() == ':' && std::count(t.begin(), t.end(), ':') >= 2) {
    t.pop_back();
  } else if (!t.empty() && t.back() == '.' && std::count(t.begin(), t.end(), '.') == 1) {
    t.pop_back();
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.empty() || t.back() < '0' || t.back() > '9') return std::string(s);
  }
  return t;
}

bool ParseTimePart(std::string_view s, double* frac, bool* overflow = nullptr) {
  // [AM/PM marker (Chinese)] h:mm[:ss[.fff]] [AM|PM|A|P], as Excel takes a typed time: the English marker in any
  // case at the end after a space (1:30PM is text), a lone A / P too ("9:00 p", as Excel's help says to type it),
  // and the hour alone with a marker ("3 PM"; without one it is a number)
  bool pm = false, am = false;
  std::string t = TidyTimeText(s);
  auto strip = [&t](const char* tag) {
    const size_t p = t.find(tag);
    if (p == std::string::npos) return false;
    t.erase(p, std::string(tag).size());
    return true;
  };
  if (strip("\344\270\213\345\215\210")) pm = true;
  else if (strip("\344\270\212\345\215\210")) am = true;
  if (const size_t e = t.find_last_not_of(' '); e != std::string::npos && e >= 1) {
    const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(t[e])));
    const char before = static_cast<char>(std::tolower(static_cast<unsigned char>(t[e - 1])));
    if (last == 'm' && (before == 'a' || before == 'p') && e >= 2 && t[e - 2] == ' ') {
      (before == 'p' ? pm : am) = true;
      t.erase(e - 1);
    } else if ((last == 'a' || last == 'p') && before == ' ') {
      (last == 'p' ? pm : am) = true;
      t.erase(e);
    }
  }
  if (am && pm) return false;
  size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
  if (a == std::string::npos) return false;
  std::string_view v(t.data() + a, b - a + 1);
  if ((am || pm) && v.back() == ':') return false;  // 9: PM is text
  // a number of up to four digits; one of more than two digits doesn't start with 0 (012:30 and 9:0099 are text)
  auto read = [&v](size_t* i, int* out) {
    const size_t st = *i;
    if (!ReadInt(v, i, out, 4)) return false;
    return *i - st <= 2 || v[st] != '0';
  };
  size_t i = 0;
  int h, mi = 0, sec = 0;
  if (!read(&i, &h)) return false;  // 100:00 is 100 hours (Excel: [h]:mm:ss)
  double fsec = 0;
  bool mss = false;
  if (i == v.size()) {
    if (!(am || pm) || h > 12) return false;  // 3 PM; a lone 3 is a number
  } else {
    if (v[i] != ':') return false;
    ++i;
    auto fraction = [&] {  // a decimal point needs a digit after it here (9:30.: is text)
      const size_t st = i;
      ++i;
      while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
      if (i == st + 1) i = v.size() + 1;
      else fsec = std::strtod(std::string(v.substr(st, i - st)).c_str(), nullptr);
    };
    // as Excel reads them: "6:" is 6:00, minutes and seconds may pass 59 (0:90 is 1:30, 6:00:60 is 6:01), and
    // m:ss.0 takes a fraction after the seconds (1:30.5 is 1 minute 30.5 seconds)
    if (i < v.size()) {
      const size_t mi_at = i;
      if (!read(&i, &mi)) return false;
      if (i < v.size() && v[i] == ':') {
        ++i;
        if (!read(&i, &sec)) return false;
        if (i < v.size() && v[i] == '.') fraction();
      } else if (i < v.size() && v[i] == '.') {
        if (i - mi_at > 2) return false;
        sec = mi;
        mi = h;
        h = 0;
        mss = true;
        fraction();
      }
    }
  }
  if (i != v.size()) return false;
  if ((am || pm) && h > 12) return false;  // 13:00 PM is text, as in Excel
  // minutes or seconds past 59 (9:60, 23:120, 9:30:99): only one of them, before the 24th hour and without AM / PM
  // (Excel 16, measured: 24:60, 9:60:60, 9:60 AM and 12:60 PM are text)
  const bool over = !mss && (mi > 59 || sec > 59);
  if (over && (am || pm || h >= 24 || (mi > 59 && sec > 59))) return false;
  if (overflow) *overflow = over;
  if (pm && h < 12) h += 12;
  if (am && h == 12) h = 0;
  if (h > 9999) return false;
  *frac = (h * 3600.0 + mi * 60.0 + sec + fsec) / 86400.0;
  return true;
}

// January..December by their English name, its first three letters or "Sept"; 0 otherwise
int MonthOfName(std::string_view w) {
  static const char* kMonths[] = {"january", "february", "march", "april", "may", "june",
                                  "july", "august", "september", "october", "november", "december"};
  if (w.size() < 3 || w.size() > 9) return 0;
  std::string l(w);
  for (char& ch : l) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
  }
  if (l == "sept") return 9;
  for (int i = 0; i < 12; ++i) {
    const std::string_view full = kMonths[i];
    if (l == full || (l.size() == 3 && full.substr(0, 3) == l)) return i + 1;
  }
  return 0;
}

bool IsDayName(std::string_view w) {
  static const char* kDays[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
  if (w.size() < 3) return false;
  std::string l(w);
  for (char& ch : l) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
  }
  for (const char* d : kDays) {
    const std::string_view full = d;
    if (l == full || (l.size() == 3 && full.substr(0, 3) == l)) return true;
  }
  return false;
}

bool SmallNumber(std::string_view w, int* out) {
  if (w.empty() || w.size() > 4) return false;
  int v = 0;
  for (char ch : w) {
    if (ch < '0' || ch > '9') return false;
    v = v * 10 + (ch - '0');
  }
  *out = v;
  return true;
}

int FullYear(int y, size_t digits) { return digits <= 2 ? (y < 30 ? 2000 + y : 1900 + y) : y; }

// dates with a month's name, as Excel takes them: 15-Jan-2026, 15 Jan 2026, Jan 15, 2026, Monday, January 15, 2026,
// 2026-Jan-15, 15-Jan (this year), Jan-2026 (its first day); a time may follow (15-Jan-2026 6:35 PM)
bool ParseNamedDate(std::string_view s, double* serial, DateForm* form, bool* overflow) {
  auto sep = [](char ch) { return ch == ' ' || ch == '-' || ch == '/' || ch == ','; };
  std::vector<std::string_view> words;
  std::string_view time;
  for (size_t i = 0; i < s.size();) {
    if (sep(s[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && !sep(s[j])) ++j;
    std::string_view w = s.substr(i, j - i);
    if (w.find(':') != std::string_view::npos) {  // the time starts here and takes the rest
      time = s.substr(i);
      break;
    }
    if (w.size() >= 4 && w.back() == '.') w.remove_suffix(1);  // Jan. 15
    words.push_back(w);
    i = j;
  }
  if (!words.empty() && IsDayName(words.front()) && !MonthOfName(words.front())) words.erase(words.begin());
  if (words.size() < 2 || words.size() > 3) return false;
  int month_at = -1;
  for (size_t k = 0; k < words.size(); ++k) {
    if (MonthOfName(words[k])) {
      if (month_at >= 0) return false;
      month_at = static_cast<int>(k);
    }
  }
  if (month_at < 0) return false;
  const int m = MonthOfName(words[static_cast<size_t>(month_at)]);
  int num[3] = {0, 0, 0};
  size_t digits[3] = {0, 0, 0};
  int n = 0;
  for (size_t k = 0; k < words.size(); ++k) {
    if (static_cast<int>(k) == month_at) continue;
    if (!SmallNumber(words[k], &num[n])) return false;
    digits[n++] = words[k].size();
  }
  int cy, cm, cd;
  SerialToYmd(std::floor(NowSerial()), &cy, &cm, &cd);
  int y, d;
  DateForm f;
  if (n == 2) {
    if (month_at == 1 && (num[0] > 31 || digits[0] > 2)) {  // 2026-Jan-15
      y = FullYear(num[0], digits[0]);
      d = num[1];
    } else {  // 15-Jan-2026, Jan 15, 2026
      d = num[0];
      y = FullYear(num[1], digits[1]);
    }
    f = DateForm::NameDayYear;
  } else if (digits[0] <= 2 && num[0] >= 1 && num[0] <= DaysInMonth(cy, m)) {  // 15-Jan, Jan 15: this year
    y = cy;
    d = num[0];
    f = DateForm::NameDay;
  } else {  // Jan-2026, Jan 32 (1932): the first day of that month
    y = FullYear(num[0], digits[0]);
    d = 1;
    f = DateForm::NameYear;
  }
  const bool excel_leap_day = y == 1900 && m == 2 && d == 29;
  if (d < 1 || (d > DaysInMonth(y, m) && !excel_leap_day) || y < 1900 || y > 9999) return false;
  double v = DateSerial(y, m, d);
  if (!time.empty()) {
    double tf;
    if (!ParseTimePart(time, &tf, overflow)) return false;
    v += tf;
  }
  *serial = v;
  if (form) *form = f;
  return true;
}

}  // namespace

bool ParseTimeOfDay(std::string_view s, double* frac) { return ParseTimePart(s, frac); }

bool ParseDateText(std::string_view s, double* serial) {
  DateForm form = DateForm::Numbers;
  return ParseDateTime(s, serial, &form) && form != DateForm::NumberMonthYear;
}

bool ParseDateTime(std::string_view s, double* serial, DateForm* form, bool* time_overflow) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return false;
  if (form) *form = DateForm::Numbers;
  for (char ch : s) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
      if (ParseNamedDate(s, serial, form, time_overflow)) return true;
      break;
    }
  }
  // 2024\u5e741\u67087\u65e5 and 9\u664230\u5206[15\u79d2], as Excel's Traditional Chinese edition reads them
  // (\u67087\u65e5 alone and 2024\u5e741\u6708 stay text)
  {
    static const std::string kYear = "\345\271\264", kMonth = "\346\234\210", kDay = "\346\227\245";
    static const std::string kHour = "\346\231\202", kMin = "\345\210\206", kSec = "\347\247\222";
    auto number = [&](size_t* at, const std::string& unit, int* out) {
      size_t j = *at;
      if (!ReadInt(s, &j, out, 4) || s.substr(j, unit.size()) != unit) return false;
      *at = j + unit.size();
      return true;
    };
    size_t at = 0;
    int y, m, d;
    if (number(&at, kYear, &y) && number(&at, kMonth, &m) && number(&at, kDay, &d) && at == s.size()) {
      if (m < 1 || m > 12 || d < 1 || d > DaysInMonth(y, m) || y < 1900 || y > 9999) return false;
      *serial = DateSerial(y, m, d);
      return true;
    }
    at = 0;
    int h, mi, sec = 0;
    if (number(&at, kHour, &h) && number(&at, kMin, &mi) && (at == s.size() || (number(&at, kSec, &sec) && at == s.size()))) {
      if (mi > 59 || sec > 59 || h > 9999) return false;
      *serial = (h * 3600.0 + mi * 60.0 + sec) / 86400.0;
      return true;
    }
  }
  // time only
  double tf;
  if (s.find('/') == std::string_view::npos && s.find('-') == std::string_view::npos && ParseTimePart(s, &tf, time_overflow)) {
    *serial = tf;
    return true;
  }
  // the time first, then the date (9:30 1/5, 9:30 PM 2024/1/5, 9:30 Jan 5), as Excel reads it
  if (const size_t sp0 = s.find(' '); sp0 != std::string_view::npos && s.substr(0, sp0).find(':') != std::string_view::npos) {
    for (size_t p = sp0; p != std::string_view::npos; p = s.find(' ', p + 1)) {
      const std::string_view left = s.substr(0, p), right = s.substr(p + 1);
      if (right.find(':') != std::string_view::npos) continue;
      double day = 0, time_of_day = 0;
      bool over = false;
      DateForm day_form = DateForm::Numbers;
      if (ParseTimePart(left, &time_of_day, &over) && ParseDateTime(right, &day, &day_form) && day == std::floor(day)) {
        *serial = day + time_of_day;
        if (form) *form = day_form;
        if (time_overflow) *time_overflow = over;
        return true;
      }
    }
  }
  // spaces beside a date's separators don't count (1 / 2, 2024/ 1/ 5, 1 - 2)
  std::string tidy;
  if (s.find(' ') != std::string_view::npos) {
    for (size_t k = 0; k < s.size(); ++k) {
      if (s[k] == ' ') {
        size_t j = k;
        while (j < s.size() && s[j] == ' ') ++j;
        const bool next_sep = j < s.size() && (s[j] == '/' || s[j] == '-');
        const bool prev_sep = !tidy.empty() && (tidy.back() == '/' || tidy.back() == '-');
        if (next_sep || prev_sep) {
          k = j - 1;
          continue;
        }
      }
      tidy += s[k];
    }
    s = tidy;
  }
  // date [space time]
  size_t sp = s.find(' ');
  std::string_view date = sp == std::string_view::npos ? s : s.substr(0, sp);
  std::string_view time = sp == std::string_view::npos ? std::string_view() : s.substr(sp + 1);
  size_t i = 0;
  int a, b, c;
  if (!ReadInt(date, &i, &a)) return false;
  if (i >= date.size() || (date[i] != '/' && date[i] != '-')) return false;
  const char sep = date[i++];
  const size_t b_at = i;
  if (!ReadInt(date, &i, &b, 2)) return false;
  int y, m, d;
  if (i < date.size() && date[i] == sep) {
    ++i;
    if (!ReadInt(date, &i, &c)) return false;
    if (i != date.size()) return false;
    if (a > 31) {  // y/m/d
      y = a;
      m = b;
      d = c;
    } else {       // m/d/y
      m = a;
      d = b;
      y = c < 100 ? (c < 30 ? 2000 + c : 1900 + c) : c;
    }
  } else {
    // m/d (this year), or the month and year when the number can't be a day of that month: 1/0 is January 2000,
    // 1/32 January 1932, 2/30 February 1930, 12/2024 December 2024 (Excel 16, measured: mmm-yy); a year of three
    // digits (1/100) or before 1900 stays text
    size_t j = b_at;
    int whole = 0;
    if (!ReadInt(date, &j, &whole, 5) || j != date.size()) return false;
    const size_t digits = j - b_at;
    int cy, cm, cd;
    SerialToYmd(std::floor(NowSerial()), &cy, &cm, &cd);
    m = a;
    if (digits <= 2 && a >= 1 && a <= 12 && b >= 1 && b <= DaysInMonth(cy, a)) {
      y = cy;
      d = b;
    } else if (digits == 3 || digits == 5) {
      return false;
    } else {
      y = FullYear(whole, digits);
      d = 1;
      if (form) *form = DateForm::NumberMonthYear;
    }
  }
  const bool excel_leap_day = y == 1900 && m == 2 && d == 29;  // serial 60, like Excel
  if (m < 1 || m > 12 || d < 1 || (d > DaysInMonth(y, m) && !excel_leap_day) || y < 1900 || y > 9999) return false;
  double v = DateSerial(y, m, d);
  if (!time.empty()) {
    if (!ParseTimePart(time, &tf, time_overflow)) return false;
    v += tf;
  }
  *serial = v;
  return true;
}

int64_t NowEpochNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

double NowSerial() {
  const int64_t ns = NowEpochNs();
  const std::time_t t = static_cast<std::time_t>(ns / 1000000000);
  std::tm lt{};
#if defined(_WIN32)
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  const double day = DateSerial(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  const double secs = lt.tm_hour * 3600.0 + lt.tm_min * 60.0 + lt.tm_sec + static_cast<double>(ns % 1000000000) / 1e9;
  return day + secs / 86400.0;
}

}  // namespace yc::formula
