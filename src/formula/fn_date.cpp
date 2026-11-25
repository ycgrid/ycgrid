// SPDX-License-Identifier: Apache-2.0
// fn_date.cpp - date and time functions (1900 date system)
#include "formula/fn_util.h"

#include "formula/datetime.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <bit>
#include <set>

namespace yc::formula::fn {
namespace {

// date arguments: numbers are used as is; text is read as a typed number first ("45292", "0.5"), then as a date or
// time (ParseDateTime), as Excel reads it
// The functions that came from Excel's Analysis ToolPak (EDATE, EOMONTH, WEEKNUM, WORKDAY, NETWORKDAYS, YEARFRAC)
// refuse TRUE / FALSE, given or in a cell, with #VALUE! (`no_bool`); the others read them as 1 / 0 (Excel 16,
// measured). Only the reading: the range check is InDateRange, as some functions read every argument first.
bool ArgDateValue(Evaluator& ev, const Node& n, double* out, Value* err, bool no_bool = false) {
  const Value v = ev.Scalar(n);
  if (v.IsError()) {
    *err = v;
    return false;
  }
  if (no_bool && v.IsBool()) {
    *err = Value::Error(Err::Value);
    return false;
  }
  if (v.IsString()) {
    if (InputParse num; ParseNumberInput(v.str(), &num) && num.value.IsNumeric()) {
      *out = num.value.AsDouble();
    } else if ((!v.str().empty() && v.str()[0] == ' ') || !ParseDateText(v.str(), out)) {
      // a date or time after a leading space is #VALUE! here, as Excel's HOUR(" 12:00") and DAY(" 2024/1/2"); only
      // DATEVALUE, TIMEVALUE and VALUE trim it (a number takes it: DAY(" 5") is 5)
      *err = Value::Error(Err::Value);
      return false;
    }
  } else {
    Err er = Err::None;
    if (!Evaluator::ToNumber(v, out, &er)) {
      *err = Value::Error(er);
      return false;
    }
  }
  return true;
}

bool InDateRange(double d, Value* err) {
  if (d < 0 || d >= 2958466) {  // before 1900 or after 9999-12-31: #NUM! (DAYS(2958466, 1), as Excel)
    *err = Value::Error(Err::Num);
    return false;
  }
  return true;
}

bool ArgDate(Evaluator& ev, const Node& n, double* out, Value* err, bool no_bool = false) {
  return ArgDateValue(ev, n, out, err, no_bool) && InDateRange(*out, err);
}

// a number argument of an Analysis ToolPak function: TRUE / FALSE is #VALUE! (EDATE(45000,TRUE), as Excel)
bool ArgAtpNumber(Evaluator& ev, const Node& n, double* out, Value* err) {
  const Value v = ev.Scalar(n);
  Err e = v.IsBool() ? Err::Value : Err::None;
  if (e != Err::None || !Evaluator::ToNumber(v, out, &e)) {
    *err = Value::Error(e);
    return false;
  }
  return true;
}

#define YC_DATE(var, i)                                   \
  double var = 0;                                         \
  {                                                       \
    Value _e;                                             \
    if (!ArgDate(ev, call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
#define YC_ATP_DATE(var, i)                               \
  double var = 0;                                         \
  {                                                       \
    Value _e;                                             \
    if (!ArgDate(ev, call.kids[(i)], &var, &_e, true)) return EV::S(_e); \
  }
#define YC_ATP_NUM(var, i)                                \
  double var = 0;                                         \
  {                                                       \
    Value _e;                                             \
    if (!ArgAtpNumber(ev, call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }

EV FNow(Evaluator&, const Node&) { return Num(NowSerial()); }
EV FToday(Evaluator&, const Node&) { return Num(std::floor(NowSerial())); }

EV FDate(Evaluator& ev, const Node& call) {
  YC_NUM(y, 0);
  YC_NUM(m, 1);
  YC_NUM(d, 2);
  // a year past 9999 is taken when the months bring the date back (DATE(10000,-1,1) is 9999-11-01, as Excel)
  if (std::fabs(y) >= 1e8 || std::fabs(m) >= 1e9 || std::fabs(d) >= 1e12) return Er(Err::Num);
  // as Excel reads them (Excel 16, measured): each part taken down to a whole number (DATE(2024,1,-0.4) is 2023-12-30),
  // a day at most 32767 (DATE(1900,1,40000) is 32767), and months that go back before 1900 are #NUM!
  int64_t yy = static_cast<int64_t>(std::floor(y));
  if (yy < 0) return Er(Err::Num);
  if (yy < 1900) yy += 1900;
  const int64_t mm = static_cast<int64_t>(std::floor(m));
  const int64_t dd = std::min<int64_t>(static_cast<int64_t>(std::floor(d)), 32767);
  if (yy + (mm >= 1 ? (mm - 1) / 12 : -((12 - mm) / 12)) < 1900) return Er(Err::Num);
  const double s = DateSerial(yy, mm, dd);
  if (s < 0 || s > 2958465) return Er(Err::Num);
  return Num(s);
}

EV FTime(Evaluator& ev, const Node& call) {
  YC_NUM(h, 0);
  YC_NUM(mi, 1);
  YC_NUM(s, 2);
  // each part is at most 32767, as in Excel (TIME(0,0,32768) is #NUM!)
  // each part taken down to a whole number, as Excel (TIME(1,0,-0.4) is 0:59:59)
  if (std::floor(h) > 32767 || std::floor(mi) > 32767 || std::floor(s) > 32767) return Er(Err::Num);
  const double t = std::floor(h) * 3600 + std::floor(mi) * 60 + std::floor(s);
  if (t < 0) return Er(Err::Num);
  return Num(std::fmod(t, 86400.0) / 86400.0);
}

// a date and time as its day and its second of the day, rounded to the nearest second as Excel reads the parts
// (23:59:59.6 is 0:00:00 of the next day); false past 9999-12-31
bool DaySecond(double serial, double* day, int* sec) {
  double dd = std::floor(serial);
  double s = std::round((serial - dd) * 86400.0);
  if (s >= 86400) {
    dd += 1;
    s -= 86400;
  }
  if (dd >= 2958466) return false;
  *day = dd;
  *sec = static_cast<int>(s);
  return true;
}

#define YC_DAY(var, i)                                     \
  YC_DATE(var##_raw, i);                                   \
  double var = 0;                                          \
  int var##_sec = 0;                                       \
  if (!DaySecond(var##_raw, &var, &var##_sec)) return Er(Err::Num)
#define YC_ATP_DAY(var, i)                                 \
  YC_ATP_DATE(var##_raw, i);                               \
  double var = 0;                                          \
  int var##_sec = 0;                                       \
  if (!DaySecond(var##_raw, &var, &var##_sec)) return Er(Err::Num)

EV YmdPart(Evaluator& ev, const Node& call, int part) {
  YC_DAY(d, 0);
  int y, m, dd;
  if (!SerialToYmd(d, &y, &m, &dd)) return Er(Err::Num);
  return Num(part == 0 ? y : part == 1 ? m : dd);
}
EV FYear(Evaluator& ev, const Node& call) { return YmdPart(ev, call, 0); }
EV FMonth(Evaluator& ev, const Node& call) { return YmdPart(ev, call, 1); }
EV FDay(Evaluator& ev, const Node& call) { return YmdPart(ev, call, 2); }

EV HmsPart(Evaluator& ev, const Node& call, int part) {
  YC_DAY(d, 0);
  const int s = d_sec;
  return Num(part == 0 ? s / 3600 : part == 1 ? s / 60 % 60 : s % 60);
}
EV FHour(Evaluator& ev, const Node& call) { return HmsPart(ev, call, 0); }
EV FMinute(Evaluator& ev, const Node& call) { return HmsPart(ev, call, 1); }
EV FSecond(Evaluator& ev, const Node& call) { return HmsPart(ev, call, 2); }

// ISO week number of a day: the week of its Thursday; days before 1900-01-02 fall in 1899's week 52 (Excel)
double IsoWeek(double day) {
  const int wd = (Weekday(day) + 5) % 7;  // 0 = Monday
  const double thursday = day - wd + 3;
  if (thursday < 1) return 52;
  int ty, tm, td;
  SerialToYmd(thursday, &ty, &tm, &td);
  return std::floor((thursday - DateSerial(ty, 1, 1)) / 7) + 1;
}

EV FWeekday(Evaluator& ev, const Node& call) {
  YC_DAY(d, 0);
  YC_NUM_OPT_BLANK0(typef, 1, 1);  // WEEKDAY(45000,) is #NUM!: a type left empty is 0 (Excel 16, measured)
  const int w = Weekday(d);  // 1 = Sunday
  const int type = static_cast<int>(typef);
  switch (type) {
    case 1: case 17: return Num(w);
    case 2: case 11: return Num((w + 5) % 7 + 1);  // 1 = Monday
    case 3: return Num((w + 5) % 7);               // 0 = Monday
    case 12: return Num((w + 4) % 7 + 1);          // 1 = Tuesday
    case 13: return Num((w + 3) % 7 + 1);
    case 14: return Num((w + 2) % 7 + 1);
    case 15: return Num((w + 1) % 7 + 1);
    case 16: return Num(w % 7 + 1);                // 1 = Saturday
    default: return Er(Err::Num);
  }
}

EV FWeekNum(Evaluator& ev, const Node& call) {
  YC_ATP_DAY(d, 0);
  double typef = 1;
  if (Has(call, 1)) {
    Value e;
    if (!ArgAtpNumber(ev, call.kids[1], &typef, &e)) return EV::S(e);
  }
  const int type = static_cast<int>(typef);
  int y, m, dd;
  if (!SerialToYmd(d, &y, &m, &dd)) return Er(Err::Num);
  if (type == 21) return Num(IsoWeek(d));
  int start;  // first day of the week (Weekday numbering, 1 = Sunday)
  switch (type) {
    case 1: case 17: start = 1; break;
    case 2: case 11: start = 2; break;
    case 12: start = 3; break;
    case 13: start = 4; break;
    case 14: start = 5; break;
    case 15: start = 6; break;
    case 16: start = 7; break;
    default: return Er(Err::Num);
  }
  const double jan1 = DateSerial(y, 1, 1);
  const int off = (Weekday(jan1) - start + 7) % 7;
  return Num(std::floor((std::floor(d) - jan1 + off) / 7) + 1);
}

EV FIsoWeekNum(Evaluator& ev, const Node& call) {
  YC_DAY(d, 0);
  return Num(IsoWeek(d));
}

// -1 when the result falls outside 1900-01 .. 9999-12 (EDATE(1,-1) is #NUM!, as in Excel)
double AddMonths(double serial, int64_t months, bool eom) {
  int y, m, d;
  if (!SerialToYmd(serial, &y, &m, &d)) return -1;
  if (months < -200000 || months > 200000) return -1;
  int64_t total = int64_t{y} * 12 + (m - 1) + months;
  if (total < 1900 * 12 || total >= 10000 * 12) return -1;
  const int ny = static_cast<int>(total / 12), nm = static_cast<int>(total % 12) + 1;
  if (eom) return DateSerial(ny, nm, DaysInMonth(ny, nm));
  return DateSerial(ny, nm, std::min(d, DaysInMonth(ny, nm)));
}

EV FEDate(Evaluator& ev, const Node& call) {
  YC_ATP_DATE(d, 0);
  YC_ATP_NUM(m, 1);
  if (std::fabs(m) > 1e6) return Er(Err::Num);
  const double r = AddMonths(std::floor(d), static_cast<int64_t>(std::trunc(m)), false);
  if (r < 0) return Er(Err::Num);
  return Num(r);
}
EV FEOMonth(Evaluator& ev, const Node& call) {
  YC_ATP_DATE(d, 0);
  YC_ATP_NUM(m, 1);
  if (std::fabs(m) > 1e6) return Er(Err::Num);
  const double r = AddMonths(std::floor(d), static_cast<int64_t>(std::trunc(m)), true);
  if (r < 0) return Er(Err::Num);
  return Num(r);
}

EV FDateValue(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  double d;
  if (!ParseDateText(s, &d)) return Er(Err::Value);
  return Num(std::floor(d));
}
EV FTimeValue(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  double d;
  if (!ParseDateText(s, &d)) return Er(Err::Value);
  return Num(d - std::floor(d));
}

EV FDays(Evaluator& ev, const Node& call) {
  YC_DATE(e, 0);
  YC_DATE(s, 1);
  return Num(std::floor(e) - std::floor(s));
}

EV FDays360(Evaluator& ev, const Node& call) {
  YC_DATE(s, 0);
  YC_DATE(e, 1);
  YC_BOOL_OPT(eu, 2, false);
  int y1, m1, d1, y2, m2, d2;
  SerialToYmd(s, &y1, &m1, &d1);
  SerialToYmd(e, &y2, &m2, &d2);
  if (eu) {
    if (d1 == 31) d1 = 30;
    if (d2 == 31) d2 = 30;
  } else {
    // Excel's February 1900 has 29 days: its 29th (serial 60) is the month's last day, its 28th is not
    const bool last_feb1 = m1 == 2 && d1 == (y1 == 1900 ? 29 : DaysInMonth(y1, 2));
    if (d1 == 31 || last_feb1) d1 = 30;
    if (d2 == 31 && d1 >= 30) d2 = 30;
  }
  return Num((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1));
}

EV FDateDif(Evaluator& ev, const Node& call) {
  // both dates are read before either is checked: DATEDIF(-1,"x","Y") is #VALUE!, as Excel
  double s = 0, e = 0;
  Value err;
  if (!ArgDateValue(ev, call.kids[0], &s, &err) || !ArgDateValue(ev, call.kids[1], &e, &err) ||
      !InDateRange(s, &err) || !InDateRange(e, &err))
    return EV::S(err);
  YC_TEXT(unit, 2);
  if (s > e) return Er(Err::Num);
  int y1, m1, d1, y2, m2, d2;
  SerialToYmd(s, &y1, &m1, &d1);
  SerialToYmd(e, &y2, &m2, &d2);
  const std::string u = Upper(unit);
  int months = (y2 - y1) * 12 + (m2 - m1);
  if (d2 < d1) --months;
  if (u == "D") return Num(std::floor(e) - std::floor(s));
  if (u == "M") return Num(months);
  if (u == "Y") return Num(months / 12);
  if (u == "YM") return Num(months % 12);
  if (u == "MD") {
    int dd = d2 - d1;
    if (dd < 0) {
      const int pm = m2 == 1 ? 12 : m2 - 1, py = m2 == 1 ? y2 - 1 : y2;
      dd += py == 1900 && pm == 2 ? 29 : DaysInMonth(py, pm);  // Excel's February 1900 has 29 days
    }
    return Num(dd);
  }
  if (u == "YD") {
    // Excel walks the "YM" whole months from the start's month in the start's year, then adds the "MD" days: so
    // 2020-02-28 to 2021-03-10 is 10 but to 2021-03-31 is 32 (Excel 16, 428 pairs measured)
    int dd = d2 - d1;
    if (dd < 0) {
      const int pm = m2 == 1 ? 12 : m2 - 1, py = m2 == 1 ? y2 - 1 : y2;
      dd += py == 1900 && pm == 2 ? 29 : DaysInMonth(py, pm);  // Excel's February 1900 has 29 days
    }
    return Num(DateSerial(y1, m1 + months % 12, 1) - DateSerial(y1, m1, 1) + dd);
  }
  return Er(Err::Num);
}

// holiday list (numbers in a range or array); text that isn't a date and TRUE / FALSE are #VALUE!, a day before
// 1900 or after 9999 #NUM!, as in Excel (WORKDAY(45000,5,TRUE), WORKDAY(45000,5,-1), measured)
bool CollectHolidays(Evaluator& ev, const Node& call, int idx, std::set<int64_t>* out, Value* err) {
  if (!Has(call, idx)) return true;
  const EV e = ev.Eval(call.kids[static_cast<size_t>(idx)]);
  bool ok = true;
  auto take = [&](const Value& v) {
    if (v.IsError() || v.IsBool()) {
      *err = v.IsError() ? v : Value::Error(Err::Value);
      ok = false;
      return false;
    }
    double d = 0;
    if (v.IsNumeric()) {
      d = v.AsDouble();
    } else if (v.IsString()) {
      InputParse num;
      if (ParseNumberInput(v.str(), &num) && num.value.IsNumeric()) d = num.value.AsDouble();
      else if (!ParseDateText(v.str(), &d)) {
        *err = Value::Error(Err::Value);
        ok = false;
        return false;
      }
    } else {
      return true;
    }
    if (!InDateRange(d, err)) {
      ok = false;
      return false;
    }
    out->insert(static_cast<int64_t>(std::floor(d)));
    return true;
  };
  if (e.k == EV::K::Scalar) take(e.s);
  else ev.ForEachValue(e, take);
  return ok;
}

// weekend mask: bit i = whether weekday i (0 = Monday ... 6 = Sunday) is a weekend day
bool WeekendMask(Evaluator& ev, const Node& call, int idx, uint8_t* mask, Value* err) {
  *mask = (1 << 5) | (1 << 6);
  if (!Has(call, idx)) return true;
  const Value v = ev.Scalar(call.kids[static_cast<size_t>(idx)]);
  if (v.IsError()) {
    *err = v;
    return false;
  }
  if (v.IsString()) {
    const auto s = v.str();
    if (s.size() != 7) {
      *err = Value::Error(Err::Value);
      return false;
    }
    *mask = 0;
    for (int i = 0; i < 7; ++i) {
      if (s[static_cast<size_t>(i)] == '1') *mask |= static_cast<uint8_t>(1 << i);
      else if (s[static_cast<size_t>(i)] != '0') {
        *err = Value::Error(Err::Value);
        return false;
      }
    }
    return true;
  }
  // a number is taken down to a whole one (1.5 is 1), TRUE is 1 and FALSE or an empty cell 0 (#NUM!), as Excel
  const double x = v.IsBool() ? (v.boolean() ? 1.0 : 0.0) : v.AsDouble();
  const int n = x > -100 && x < 100 ? static_cast<int>(x) : 0;
  if (n >= 1 && n <= 7) {
    // 1 = Sat+Sun, 2 = Sun+Mon, 3 = Mon+Tue ... 7 = Fri+Sat
    const int a = (n + 4) % 7, b = (n + 5) % 7;
    *mask = static_cast<uint8_t>((1 << a) | (1 << b));
    return true;
  }
  if (n >= 11 && n <= 17) {
    *mask = static_cast<uint8_t>(1 << ((n - 11 + 6) % 7));
    return true;
  }
  *err = Value::Error(Err::Num);
  return false;
}

bool IsWorkday(int64_t day, uint8_t mask, const std::set<int64_t>& hol) {
  const int wd = (Weekday(static_cast<double>(day)) + 5) % 7;
  return !(mask & (1 << wd)) && !hol.count(day);
}

EV NetworkDaysImpl(Evaluator& ev, const Node& call, bool intl) {
  uint8_t mask;
  Value err;
  std::set<int64_t> hol;
  // NETWORKDAYS.INTL reads its weekend before the dates: NETWORKDAYS.INTL(-1,10,"abc") is #VALUE!, as Excel
  if (intl) {
    if (!WeekendMask(ev, call, 2, &mask, &err)) return EV::S(err);
  } else {
    mask = (1 << 5) | (1 << 6);
  }
  YC_ATP_DATE(s, 0);
  YC_ATP_DATE(e, 1);
  if (!CollectHolidays(ev, call, intl ? 3 : 2, &hol, &err)) return EV::S(err);
  int64_t a = static_cast<int64_t>(std::floor(s)), b = static_cast<int64_t>(std::floor(e));
  int sign = 1;
  if (a > b) {
    std::swap(a, b);
    sign = -1;
  }
  int64_t n = 0;
  // fast path for whole weeks
  const int64_t span = b - a + 1;
  const int64_t weeks = span / 7;
  const int per_week = 7 - std::popcount(static_cast<unsigned>(mask));
  n += weeks * per_week;
  for (int64_t d = a + weeks * 7; d <= b; ++d) {
    const int wd = (Weekday(static_cast<double>(d)) + 5) % 7;
    if (!(mask & (1 << wd))) ++n;
  }
  for (int64_t h : hol) {
    if (h >= a && h <= b) {
      const int wd = (Weekday(static_cast<double>(h)) + 5) % 7;
      if (!(mask & (1 << wd))) --n;
    }
  }
  return Num(static_cast<double>(n * sign));
}
EV FNetworkDays(Evaluator& ev, const Node& call) { return NetworkDaysImpl(ev, call, false); }
EV FNetworkDaysIntl(Evaluator& ev, const Node& call) { return NetworkDaysImpl(ev, call, true); }

EV WorkdayImpl(Evaluator& ev, const Node& call, bool intl) {
  YC_ATP_DATE(s, 0);
  YC_ATP_NUM(daysf, 1);
  // no days to move (0 to 0.99): the start, with neither the weekend nor the holidays read (WORKDAY.INTL(45000,0,99)
  // is 45000, as Excel)
  if (daysf >= 0 && daysf < 1) return Num(std::floor(s));
  uint8_t mask;
  Value err;
  std::set<int64_t> hol;
  if (intl) {
    if (!WeekendMask(ev, call, 2, &mask, &err)) return EV::S(err);
  } else {
    mask = (1 << 5) | (1 << 6);
  }
  if (mask == 0x7F) return Er(Err::Value);
  if (!CollectHolidays(ev, call, intl ? 3 : 2, &hol, &err)) return EV::S(err);
  int64_t d = static_cast<int64_t>(std::floor(s));
  if (std::fabs(daysf) > 1e8) return Er(Err::Num);
  // a fraction of a day rounds down (toward the past): -0.5 is one working day back, as in Excel
  int64_t left = static_cast<int64_t>(std::floor(daysf));
  const int step = left >= 0 ? 1 : -1;
  left = left < 0 ? -left : left;
  while (left > 0) {
    d += step;
    if (IsWorkday(d, mask, hol)) --left;
    if (d < 0 || d > 2958465) return Er(Err::Num);
  }
  return Num(static_cast<double>(d));
}
EV FWorkday(Evaluator& ev, const Node& call) { return WorkdayImpl(ev, call, false); }
EV FWorkdayIntl(Evaluator& ev, const Node& call) { return WorkdayImpl(ev, call, true); }

EV FYearFrac(Evaluator& ev, const Node& call) {
  // the basis is read and checked first: YEARFRAC("x",2,9) is #NUM!, YEARFRAC(-1,1,"x") #VALUE!, as Excel
  double basisf = 0;
  if (Has(call, 2)) {
    Value e;
    if (!ArgAtpNumber(ev, call.kids[2], &basisf, &e)) return EV::S(e);
  }
  if (!(basisf >= 0 && basisf < 5)) return Er(Err::Num);  // -0.5 is #NUM!, 4.9 is 4 (measured)
  YC_ATP_DATE(s, 0);
  YC_ATP_DATE(e, 1);
  const double f = YearFraction(s, e, static_cast<int>(basisf));
  return f < 0 ? Er(Err::Num) : Num(f);
}

}  // namespace

// YEARFRAC by day-count basis (0 US 30/360, 1 actual/actual, 2 actual/360, 3 actual/365, 4 European 30/360);
// -1 for an invalid basis
double YearFraction(double s, double e, int basis) {
  if (s > e) std::swap(s, e);
  s = std::floor(s);
  e = std::floor(e);
  int y1, m1, d1, y2, m2, d2;
  SerialToYmd(s, &y1, &m1, &d1);
  SerialToYmd(e, &y2, &m2, &d2);
  switch (basis) {
    case 0: {
      if (d1 == 31) d1 = 30;
      if (d1 == 30 && d2 == 31) d2 = 30;
      else if (m1 == 2 && d1 == DaysInMonth(y1, 2)) {
        d1 = 30;
        if (m2 == 2 && d2 == DaysInMonth(y2, 2)) d2 = 30;
      }
      return (((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360.0);
    }
    case 1: {
      double avg;
      if (y1 == y2) {
        avg = IsLeap(y1) ? 366 : 365;
      } else if (y2 == y1 + 1 && (m1 > m2 || (m1 == m2 && d1 >= d2))) {
        const bool leap = (IsLeap(y1) && (m1 < 3)) || (IsLeap(y2) && (m2 > 2 || (m2 == 2 && d2 == 29)));
        avg = leap ? 366 : 365;
      } else {
        double total = 0;
        for (int y = y1; y <= y2; ++y) total += IsLeap(y) ? 366 : 365;
        avg = total / (y2 - y1 + 1);
      }
      return ((e - s) / avg);
    }
    case 2: return ((e - s) / 360.0);
    case 3: return ((e - s) / 365.0);
    case 4: {
      if (d1 == 31) d1 = 30;
      if (d2 == 31) d2 = 30;
      return (((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360.0);
    }
    default: return -1;
  }
}

void RegisterDate(std::vector<FuncInfo>& t) {
  t.push_back({"NOW", 0, 0, FNow, true});
  t.push_back({"TODAY", 0, 0, FToday, true});
  t.push_back({"DATE", 3, 3, FDate});
  t.push_back({"TIME", 3, 3, FTime});
  t.push_back({"YEAR", 1, 1, FYear});
  t.push_back({"MONTH", 1, 1, FMonth});
  t.push_back({"DAY", 1, 1, FDay});
  t.push_back({"HOUR", 1, 1, FHour});
  t.push_back({"MINUTE", 1, 1, FMinute});
  t.push_back({"SECOND", 1, 1, FSecond});
  t.push_back({"WEEKDAY", 1, 2, FWeekday});
  t.push_back({"WEEKNUM", 1, 2, FWeekNum});
  t.push_back({"ISOWEEKNUM", 1, 1, FIsoWeekNum});
  t.push_back({"EDATE", 2, 2, FEDate});
  t.push_back({"EOMONTH", 2, 2, FEOMonth});
  t.push_back({"DATEVALUE", 1, 1, FDateValue});
  t.push_back({"TIMEVALUE", 1, 1, FTimeValue});
  t.push_back({"DAYS", 2, 2, FDays});
  t.push_back({"DAYS360", 2, 3, FDays360});
  t.push_back({"DATEDIF", 3, 3, FDateDif});
  t.push_back({"NETWORKDAYS", 2, 3, FNetworkDays});
  t.push_back({"NETWORKDAYS.INTL", 2, 4, FNetworkDaysIntl});
  t.push_back({"WORKDAY", 2, 3, FWorkday});
  t.push_back({"WORKDAY.INTL", 2, 4, FWorkdayIntl});
  t.push_back({"YEARFRAC", 2, 3, FYearFrac});
}

}  // namespace yc::formula::fn
