// SPDX-License-Identifier: Apache-2.0
// datetime.h - Excel date serials (1900 date system; keeps the nonexistent 1900/2/29 = serial 60, PLAN §6.4)
#pragma once

#include <cstdint>
#include <string_view>

namespace yc::formula {

inline constexpr double kEpochSerial = 25569.0;  // serial of 1970-01-01 in the 1900 date system

// The workbook's date system (Workbook.Date1904, Excel 16 measured): in the 1904 system a serial counts days from
// 1904-01-01 (serial 0), 1462 fewer than the 1900 system's. DateSerial, SerialToYmd, Weekday and NowSerial follow
// it, so every date function and number format does; a cell keeps its number when it changes, and shows another
// date. One workbook per YCGrid, so one setting for every thread: the Document sets it from its workbook.
void SetDate1904(bool on);
bool Date1904();
double DateOffset();                                                // 1462 in the 1904 system, else 0
inline double EpochSerial() { return kEpochSerial - DateOffset(); }  // serial of 1970-01-01 in this system

double DateSerial(int64_t year, int64_t month, int64_t day);  // month and day may overflow (like Excel DATE)
bool SerialToYmd(double serial, int* y, int* m, int* d);       // false when serial < 0
void SerialToHms(double serial, int* h, int* mi, int* s, int* ms);
int Weekday(double serial);  // 1 = Sunday
int DaysInMonth(int y, int m);
bool IsLeap(int y);

// how a date was typed, for the format Excel gives it: numbers (2026/10/11), or a month's name with a day and a year
// (15-Jan-2026, Jan 15, 2026), with a day only (15-Jan) or a month with a year only (Jan-2026, 1/2026, 1/0: mmm-yy)
// NumberMonthYear: a month and year in numbers (1/2026, 2/30 for February 1930), which Excel takes only typed: a
// formula that reads such text (="1/2026"+0, VALUE, DATEVALUE) is #VALUE! (Excel 16, measured)
enum class DateForm : uint8_t { Numbers, NameDayYear, NameDay, NameYear, NumberMonthYear };

// text -> date / time serial: 2026/10/11, 2026-10-11, 10/11/2026, 2026/10/11 14:30, 14:30:15, 2:30 PM (also the Chinese PM
// marker), and English month names: 15-Jan-2026, Jan 15, 2026, Monday, January 15, 2026, 15-Jan, Jan-2026. The time may
// come first (9:30 1/5). time_overflow: the minutes or the seconds passed 59 (9:60), which Excel shows in General
bool ParseDateTime(std::string_view s, double* serial, DateForm* form = nullptr, bool* time_overflow = nullptr);
// a date or time as a formula reads text (DATEVALUE, a date argument): ParseDateTime without the forms Excel takes
// only typed (NumberMonthYear)
bool ParseDateText(std::string_view s, double* serial);
// a typed time alone (9:30, 9:30:15.5 PM, 100:00): its fraction of a day, which may pass 1
bool ParseTimeOfDay(std::string_view s, double* frac);

// serial of the current time (local time zone)
double NowSerial();
int64_t NowEpochNs();

}  // namespace yc::formula
