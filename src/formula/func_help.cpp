// SPDX-License-Identifier: Apache-2.0
#include "formula/func_help.h"
#include "core/i18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace yc::formula {

namespace {

const char* const kMath = TR("Math & Trig");
const char* const kStat = TR("Statistical");
const char* const kLogic = TR("Logical");
const char* const kLookup = TR("Lookup & Reference");
const char* const kText = TR("Text");
const char* const kDate = TR("Date & Time");
const char* const kInfo = TR("Information");
const char* const kFin = TR("Financial");
const char* const kCompat = TR("Compatibility");
const char* const kEng = TR("Engineering");
const char* const kDb = TR("Database");
const char* const kWeb = TR("Web");
constexpr const char* kYc = "YCGrid";

const std::vector<FuncHelp> kHelp = {
    // ---- Financial ----
    {"PV", "rate, nper, pmt, [fv], [type]", TR("Returns the present value of an investment."), kFin},
    {"FV", "rate, nper, pmt, [pv], [type]", TR("Returns the future value of an investment."), kFin},
    {"PMT", "rate, nper, pv, [fv], [type]", TR("Returns the periodic payment of a loan or annuity."), kFin},
    {"NPER", "rate, pmt, pv, [fv], [type]", TR("Returns the number of periods of an investment."), kFin},
    {"RATE", "nper, pmt, pv, [fv], [type], [guess]", TR("Returns the interest rate per period of an annuity."), kFin},
    {"IPMT", "rate, per, nper, pv, [fv], [type]", TR("Returns the interest part of a payment for a given period."), kFin},
    {"PPMT", "rate, per, nper, pv, [fv], [type]", TR("Returns the principal part of a payment for a given period."), kFin},
    {"NPV", "rate, value1, [value2], ...", TR("Returns the net present value of periodic cash flows at a discount rate."), kFin},
    {"IRR", "values, [guess]", TR("Returns the internal rate of return of periodic cash flows."), kFin},
    {"XNPV", "rate, values, dates", TR("Returns the net present value of cash flows on given dates."), kFin},
    {"XIRR", "values, dates, [guess]", TR("Returns the internal rate of return of cash flows on given dates."), kFin},
    // ---- Date & Time ----
    {"NOW", "", TR("Returns the current date and time."), kDate},
    {"TODAY", "", TR("Returns today's date."), kDate},
    {"DATE", "year, month, day", TR("Returns the serial number of a particular date."), kDate},
    {"TIME", "hour, minute, second", TR("Returns the decimal number of a particular time."), kDate},
    {"YEAR", "serial_number", TR("Returns the year of a date."), kDate},
    {"MONTH", "serial_number", TR("Returns the month of a date (1 to 12)."), kDate},
    {"DAY", "serial_number", TR("Returns the day of the month (1 to 31)."), kDate},
    {"HOUR", "serial_number", TR("Returns the hour of a time (0 to 23)."), kDate},
    {"MINUTE", "serial_number", TR("Returns the minute of a time (0 to 59)."), kDate},
    {"SECOND", "serial_number", TR("Returns the second of a time (0 to 59)."), kDate},
    {"WEEKDAY", "serial_number, [return_type]", TR("Returns the day of the week of a date."), kDate},
    {"WEEKNUM", "serial_number, [return_type]", TR("Returns the week number of a date."), kDate},
    {"ISOWEEKNUM", "date", TR("Returns the ISO week number."), kDate},
    {"EDATE", "start_date, months", TR("Returns the same day a number of months before or after a start date."), kDate},
    {"EOMONTH", "start_date, months", TR("Returns the last day of the month a number of months before or after a start date."), kDate},
    {"DATEVALUE", "date_text", TR("Converts a date in text form to a serial number."), kDate},
    {"TIMEVALUE", "time_text", TR("Converts a time in text form to a decimal number."), kDate},
    {"DAYS", "end_date, start_date", TR("Returns the number of days between two dates."), kDate},
    {"DAYS360", "start_date, end_date, [method]", TR("Returns the number of days between two dates based on a 360-day year."), kDate},
    {"DATEDIF", "start_date, end_date, unit", TR("Calculates the difference between two dates in a unit (\"Y\", \"M\", \"D\"…)."), kDate},
    {"NETWORKDAYS", "start_date, end_date, [holidays]", TR("Returns the number of workdays between two dates."), kDate},
    {"NETWORKDAYS.INTL", "start_date, end_date, [weekend], [holidays]", TR("Returns the number of workdays with custom weekend days."), kDate},
    {"WORKDAY", "start_date, days, [holidays]", TR("Returns the date a number of workdays before or after a start date."), kDate},
    {"WORKDAY.INTL", "start_date, days, [weekend], [holidays]", TR("Returns a workday date with custom weekend days."), kDate},
    {"YEARFRAC", "start_date, end_date, [basis]", TR("Returns the fraction of a year between two dates."), kDate},
    // ---- Information ----
    {"ISNUMBER", "value", TR("Returns TRUE if the value is a number."), kInfo},
    {"ISTEXT", "value", TR("Returns TRUE if the value is text."), kInfo},
    {"ISNONTEXT", "value", TR("Returns TRUE if the value is not text."), kInfo},
    {"ISBLANK", "value", TR("Returns TRUE if the value is blank."), kInfo},
    {"ISLOGICAL", "value", TR("Returns TRUE if the value is a logical value."), kInfo},
    {"ISERROR", "value", TR("Returns TRUE if the value is any error value."), kInfo},
    {"ISERR", "value", TR("Returns TRUE if the value is any error other than #N/A."), kInfo},
    {"ISNA", "value", TR("Returns TRUE if the value is #N/A."), kInfo},
    {"ISEVEN", "number", TR("Returns TRUE if the number is even."), kInfo},
    {"ISODD", "number", TR("Returns TRUE if the number is odd."), kInfo},
    {"ISREF", "value", TR("Returns TRUE if the value is a reference."), kInfo},
    {"ISFORMULA", "reference", TR("Returns TRUE if the cell contains a formula."), kInfo},
    {"FORMULATEXT", "reference", TR("Returns the formula of a cell as text."), kInfo},
    {"NA", "", TR("Returns the error value #N/A."), kInfo},
    {"TYPE", "value", TR("Returns the type of a value (1 number, 2 text, 4 logical, 16 error, 64 array)."), kInfo},
    {"ERROR.TYPE", "error_val", TR("Returns the number that corresponds to an error value."), kInfo},
    {"SHEET", "[value]", TR("Returns the sheet number of a reference."), kInfo},
    {"GETPIVOTDATA", "data_field, pivot_table, [field1, item1], ...", TR("Returns data stored in a PivotTable report."), kLookup},
    {"SHEETS", "[reference]", TR("Returns the number of sheets in a reference."), kInfo},
    {"CELL", "info_type, [reference]", TR("Returns information about the formatting, location or contents of a cell."), kInfo},
    {"N", "value", TR("Converts a value to a number."), kInfo},
    // ---- Logical ----
    {"IF", "logical_test, [value_if_true], [value_if_false]", TR("Returns one value if a condition is true and another if it is false."), kLogic},
    {"IFS", "logical_test1, value_if_true1, ...", TR("Checks conditions in order and returns the value of the first one that is true."), kLogic},
    {"IFERROR", "value, value_if_error", TR("Returns the given value if the value is an error; otherwise returns the value."), kLogic},
    {"IFNA", "value, value_if_na", TR("Returns the given value if the value is #N/A; otherwise returns the value."), kLogic},
    {"AND", "logical1, [logical2], ...", TR("Returns TRUE if all arguments are TRUE."), kLogic},
    {"OR", "logical1, [logical2], ...", TR("Returns TRUE if any argument is TRUE."), kLogic},
    {"XOR", "logical1, [logical2], ...", TR("Returns the exclusive OR of all arguments."), kLogic},
    {"NOT", "logical", TR("Reverses a logical value."), kLogic},
    {"TRUE", "", TR("Returns the logical value TRUE."), kLogic},
    {"FALSE", "", TR("Returns the logical value FALSE."), kLogic},
    {"SWITCH", "expression, value1, result1, [default_or_value2], ...", TR("Compares an expression against a list of values and returns the first match."), kLogic},
    {"LET", "name1, value1, [name2, value2], ..., calculation", TR("Assigns names to calculation results and uses them in a final calculation."), kLogic},
    {"LAMBDA", "[parameter1, ...], calculation", TR("Creates a custom function that can be called with arguments or defined as a name."), kLogic},
    {"MAP", "array1, [array2], ..., lambda", TR("Applies a LAMBDA to each value of the arrays and returns the results."), kLogic},
    {"REDUCE", "[initial_value], array, lambda", TR("Reduces an array to one value by applying a LAMBDA to each value and an accumulator."), kLogic},
    {"SCAN", "[initial_value], array, lambda", TR("Applies a LAMBDA to each value and an accumulator and returns every intermediate value."), kLogic},
    {"BYROW", "array, lambda", TR("Applies a LAMBDA to each row and returns one value per row."), kLogic},
    {"BYCOL", "array, lambda", TR("Applies a LAMBDA to each column and returns one value per column."), kLogic},
    {"MAKEARRAY", "rows, columns, lambda", TR("Builds an array of the given size by calling a LAMBDA with each row and column number."), kLogic},
    {"ISOMITTED", "argument", TR("Checks whether a LAMBDA parameter was left out."), kInfo},
    // ---- Lookup & Reference ----
    {"CHOOSE", "index_num, value1, [value2], ...", TR("Chooses a value from a list by index."), kLookup},
    {"VLOOKUP", "lookup_value, table_array, col_index_num, [range_lookup]", TR("Looks up a value in the first column of a table and returns a value in the same row."), kLookup},
    {"HLOOKUP", "lookup_value, table_array, row_index_num, [range_lookup]", TR("Looks up a value in the first row of a table and returns a value in the same column."), kLookup},
    {"LOOKUP", "lookup_value, lookup_vector, [result_vector]", TR("Looks up a value in a single row or column."), kLookup},
    {"MATCH", "lookup_value, lookup_array, [match_type]", TR("Returns the relative position of a value in a range."), kLookup},
    {"XMATCH", "lookup_value, lookup_array, [match_mode], [search_mode]", TR("Returns the relative position of a value in an array."), kLookup},
    {"XLOOKUP", "lookup_value, lookup_array, return_array, [if_not_found], [match_mode], [search_mode]",
     TR("Searches a range and returns the matching item from another range."), kLookup},
    {"INDEX", "array, row_num, [column_num]", TR("Returns the value or reference at the intersection of a row and column in a range."), kLookup},
    {"OFFSET", "reference, rows, cols, [height], [width]", TR("Returns a range offset from a reference by a number of rows and columns."), kLookup},
    {"INDIRECT", "ref_text, [a1]", TR("Returns the reference specified by text."), kLookup},
    {"ROW", "[reference]", TR("Returns the row number of a reference."), kLookup},
    {"COLUMN", "[reference]", TR("Returns the column number of a reference."), kLookup},
    {"ROWS", "array", TR("Returns the number of rows in a reference or array."), kLookup},
    {"COLUMNS", "array", TR("Returns the number of columns in a reference or array."), kLookup},
    {"ADDRESS", "row_num, column_num, [abs_num], [a1], [sheet_text]", TR("Returns a cell address as text."), kLookup},
    {"TRANSPOSE", "array", TR("Returns the transposed array (rows and columns swapped)."), kLookup},
    {"FILTER", "array, include, [if_empty]", TR("Filters a range or array by a condition."), kLookup},
    {"SORT", "array, [sort_index], [sort_order], [by_col]", TR("Sorts the contents of a range or array."), kLookup},
    {"SORTBY", "array, by_array1, [sort_order1], ...", TR("Sorts a range or array by the values of other ranges or arrays."), kLookup},
    {"UNIQUE", "array, [by_col], [exactly_once]", TR("Returns the unique values of a range or array."), kLookup},
    {"VSTACK", "array1, [array2], ...", TR("Stacks arrays vertically into one array."), kLookup},
    {"HSTACK", "array1, [array2], ...", TR("Stacks arrays horizontally into one array."), kLookup},
    {"TAKE", "array, rows, [columns]", TR("Returns rows or columns from the start or end of an array."), kLookup},
    {"DROP", "array, rows, [columns]", TR("Removes rows or columns from the start or end of an array."), kLookup},
    {"CHOOSEROWS", "array, row_num1, [row_num2], ...", TR("Returns the given rows of an array."), kLookup},
    {"CHOOSECOLS", "array, col_num1, [col_num2], ...", TR("Returns the given columns of an array."), kLookup},
    {"TOCOL", "array, [ignore], [scan_by_column]", TR("Returns an array as one column."), kLookup},
    {"TOROW", "array, [ignore], [scan_by_column]", TR("Returns an array as one row."), kLookup},
    {"EXPAND", "array, rows, [columns], [pad_with]", TR("Expands an array to the given size."), kLookup},
    {"WRAPROWS", "vector, wrap_count, [pad_with]", TR("Wraps a row or column of values into rows."), kLookup},
    {"WRAPCOLS", "vector, wrap_count, [pad_with]", TR("Wraps a row or column of values into columns."), kLookup},
    {"HYPERLINK", "link_location, [friendly_name]", TR("Creates a link that opens a web address or file."), kLookup},
    // ---- Math & Trig ----
    {"SUM", "number1, [number2], ...", TR("Returns the sum of all arguments."), kMath},
    {"SUMIF", "range, criteria, [sum_range]", TR("Adds the cells that meet a criterion."), kMath},
    {"SUMIFS", "sum_range, criteria_range1, criteria1, ...", TR("Adds the cells that meet multiple criteria."), kMath},
    {"SUMPRODUCT", "array1, [array2], ...", TR("Returns the sum of the products of corresponding elements."), kMath},
    {"SUMSQ", "number1, [number2], ...", TR("Returns the sum of the squares of the arguments."), kMath},
    {"PRODUCT", "number1, [number2], ...", TR("Returns the product of all arguments."), kMath},
    {"ABS", "number", TR("Returns the absolute value."), kMath},
    {"SIGN", "number", TR("Returns the sign of a number (1, 0, -1)."), kMath},
    {"INT", "number", TR("Rounds down to the nearest integer."), kMath},
    {"TRUNC", "number, [num_digits]", TR("Truncates a number by removing its fractional part."), kMath},
    {"ROUND", "number, num_digits", TR("Rounds to a specified number of digits."), kMath},
    {"ROUNDUP", "number, num_digits", TR("Rounds up to a specified number of digits."), kMath},
    {"ROUNDDOWN", "number, num_digits", TR("Rounds down to a specified number of digits."), kMath},
    {"MROUND", "number, multiple", TR("Rounds to a specified multiple."), kMath},
    {"MOD", "number, divisor", TR("Returns the remainder of a division."), kMath},
    {"QUOTIENT", "numerator, denominator", TR("Returns the integer portion of a division."), kMath},
    {"SQRT", "number", TR("Returns the square root."), kMath},
    {"POWER", "number, power", TR("Returns a number raised to a power."), kMath},
    {"EXP", "number", TR("Returns e raised to a power."), kMath},
    {"LN", "number", TR("Returns the natural logarithm."), kMath},
    {"LOG", "number, [base]", TR("Returns the logarithm to a given base."), kMath},
    {"LOG10", "number", TR("Returns the base-10 logarithm."), kMath},
    {"PI", "", TR("Returns the value of pi."), kMath},
    {"RAND", "", TR("Returns a random number between 0 and 1."), kMath},
    {"RANDBETWEEN", "bottom, top", TR("Returns a random integer between two integers."), kMath},
    {"SEQUENCE", "rows, [columns], [start], [step]", TR("Returns an array of sequential numbers."), kMath},
    {"RANDARRAY", "[rows], [columns], [min], [max], [whole_number]", TR("Returns an array of random numbers."), kMath},
    {"CEILING", "number, significance", TR("Rounds up to a specified multiple."), kMath},
    {"FLOOR", "number, significance", TR("Rounds down to a specified multiple."), kMath},
    {"CEILING.MATH", "number, [significance], [mode]", TR("Rounds up to the nearest integer or multiple."), kMath},
    {"FLOOR.MATH", "number, [significance], [mode]", TR("Rounds down to the nearest integer or multiple."), kMath},
    {"EVEN", "number", TR("Rounds up to the nearest even integer."), kMath},
    {"ODD", "number", TR("Rounds up to the nearest odd integer."), kMath},
    {"GCD", "number1, [number2], ...", TR("Returns the greatest common divisor."), kMath},
    {"LCM", "number1, [number2], ...", TR("Returns the least common multiple."), kMath},
    {"FACT", "number", TR("Returns the factorial."), kMath},
    {"COMBIN", "number, number_chosen", TR("Returns the number of combinations."), kMath},
    {"SIN", "number", TR("Returns the sine (radians)."), kMath},
    {"COS", "number", TR("Returns the cosine (radians)."), kMath},
    {"TAN", "number", TR("Returns the tangent (radians)."), kMath},
    {"ASIN", "number", TR("Returns the arcsine."), kMath},
    {"ACOS", "number", TR("Returns the arccosine."), kMath},
    {"ATAN", "number", TR("Returns the arctangent."), kMath},
    {"ATAN2", "x_num, y_num", TR("Returns the arctangent of x and y coordinates."), kMath},
    {"DEGREES", "angle", TR("Converts radians to degrees."), kMath},
    {"RADIANS", "angle", TR("Converts degrees to radians."), kMath},
    {"SUBTOTAL", "function_num, ref1, [ref2], ...", TR("Returns a subtotal (can ignore hidden rows)."), kMath},
    {"AGGREGATE", "function_num, options, ref1, [ref2], ...", TR("Returns an aggregate, optionally ignoring hidden rows and errors."), kMath},
    // ---- Statistical ----
    {"AVERAGE", "number1, [number2], ...", TR("Returns the average of the arguments."), kStat},
    {"AVERAGEA", "value1, [value2], ...", TR("Returns the average of the arguments (text and logical values included)."), kStat},
    {"AVERAGEIF", "range, criteria, [average_range]", TR("Returns the average of cells that meet a criterion."), kStat},
    {"AVERAGEIFS", "average_range, criteria_range1, criteria1, ...", TR("Returns the average of cells that meet multiple criteria."), kStat},
    {"COUNT", "value1, [value2], ...", TR("Counts the cells that contain numbers."), kStat},
    {"COUNTA", "value1, [value2], ...", TR("Counts the cells that are not empty."), kStat},
    {"COUNTBLANK", "range", TR("Counts the blank cells."), kStat},
    {"COUNTIF", "range, criteria", TR("Counts the cells that meet a criterion."), kStat},
    {"COUNTIFS", "criteria_range1, criteria1, ...", TR("Counts the cells that meet multiple criteria."), kStat},
    {"MAX", "number1, [number2], ...", TR("Returns the maximum value."), kStat},
    {"MIN", "number1, [number2], ...", TR("Returns the minimum value."), kStat},
    {"MAXA", "value1, [value2], ...", TR("Returns the maximum value (text and logical values included)."), kStat},
    {"MINA", "value1, [value2], ...", TR("Returns the minimum value (text and logical values included)."), kStat},
    {"MAXIFS", "max_range, criteria_range1, criteria1, ...", TR("Returns the maximum among cells that meet criteria."), kStat},
    {"MINIFS", "min_range, criteria_range1, criteria1, ...", TR("Returns the minimum among cells that meet criteria."), kStat},
    {"MEDIAN", "number1, [number2], ...", TR("Returns the median."), kStat},
    {"MODE", "number1, [number2], ...", TR("Returns the most frequent value."), kStat},
    {"MODE.SNGL", "number1, [number2], ...", TR("Returns the most frequent value."), kStat},
    {"LARGE", "array, k", TR("Returns the k-th largest value."), kStat},
    {"SMALL", "array, k", TR("Returns the k-th smallest value."), kStat},
    {"RANK", "number, ref, [order]", TR("Returns the rank of a number in a list."), kStat},
    {"RANK.EQ", "number, ref, [order]", TR("Returns the rank of a number in a list (ties get the top rank)."), kStat},
    {"RANK.AVG", "number, ref, [order]", TR("Returns the rank of a number in a list (ties get the average rank)."), kStat},
    {"STDEV", "number1, [number2], ...", TR("Estimates standard deviation based on a sample."), kStat},
    {"STDEV.S", "number1, [number2], ...", TR("Estimates standard deviation based on a sample."), kStat},
    {"STDEVP", "number1, [number2], ...", TR("Calculates standard deviation based on the entire population."), kStat},
    {"STDEV.P", "number1, [number2], ...", TR("Calculates standard deviation based on the entire population."), kStat},
    {"VAR", "number1, [number2], ...", TR("Estimates variance based on a sample."), kStat},
    {"VAR.S", "number1, [number2], ...", TR("Estimates variance based on a sample."), kStat},
    {"VARP", "number1, [number2], ...", TR("Calculates variance based on the entire population."), kStat},
    {"VAR.P", "number1, [number2], ...", TR("Calculates variance based on the entire population."), kStat},
    {"NORM.DIST", "x, mean, standard_dev, cumulative", TR("Returns the normal distribution."), kStat},
    {"NORMDIST", "x, mean, standard_dev, cumulative", TR("Returns the normal distribution."), kStat},
    {"NORM.S.DIST", "z, cumulative", TR("Returns the standard normal distribution."), kStat},
    {"NORMSDIST", "z", TR("Returns the standard normal cumulative distribution."), kStat},
    {"NORM.INV", "probability, mean, standard_dev", TR("Returns the inverse of the normal cumulative distribution."), kStat},
    {"NORMINV", "probability, mean, standard_dev", TR("Returns the inverse of the normal cumulative distribution."), kStat},
    {"NORM.S.INV", "probability", TR("Returns the inverse of the standard normal cumulative distribution."), kStat},
    {"NORMSINV", "probability", TR("Returns the inverse of the standard normal cumulative distribution."), kStat},
    {"STANDARDIZE", "x, mean, standard_dev", TR("Returns a normalized value."), kStat},
    {"PERCENTILE", "array, k", TR("Returns the k-th percentile."), kStat},
    {"PERCENTILE.INC", "array, k", TR("Returns the k-th percentile (inclusive of 0 and 1)."), kStat},
    {"PERCENTILE.EXC", "array, k", TR("Returns the k-th percentile (exclusive of 0 and 1)."), kStat},
    {"QUARTILE", "array, quart", TR("Returns the quartile."), kStat},
    {"QUARTILE.INC", "array, quart", TR("Returns the quartile (inclusive of 0 and 1)."), kStat},
    {"CORREL", "array1, array2", TR("Returns the correlation coefficient of two data sets."), kStat},
    {"COVARIANCE.S", "array1, array2", TR("Returns the sample covariance."), kStat},
    {"SLOPE", "known_y's, known_x's", TR("Returns the slope of the linear regression line."), kStat},
    {"INTERCEPT", "known_y's, known_x's", TR("Returns the intercept of the linear regression line."), kStat},
    {"FORECAST", "x, known_y's, known_x's", TR("Predicts a value along a linear trend."), kStat},
    {"FORECAST.LINEAR", "x, known_y's, known_x's", TR("Predicts a value along a linear trend."), kStat},
    {"GEOMEAN", "number1, [number2], ...", TR("Returns the geometric mean."), kStat},
    // ---- Text ----
    {"LEN", "text", TR("Returns the number of characters in text."), kText},
    {"LEFT", "text, [num_chars]", TR("Returns characters from the start of text."), kText},
    {"RIGHT", "text, [num_chars]", TR("Returns characters from the end of text."), kText},
    {"MID", "text, start_num, num_chars", TR("Returns a number of characters from a given position."), kText},
    {"FIND", "find_text, within_text, [start_num]", TR("Finds the position of text (case-sensitive)."), kText},
    {"SEARCH", "find_text, within_text, [start_num]", TR("Finds the position of text (not case-sensitive, wildcards allowed)."), kText},
    {"SUBSTITUTE", "text, old_text, new_text, [instance_num]", TR("Substitutes new text for old text."), kText},
    {"REPLACE", "old_text, start_num, num_chars, new_text", TR("Replaces characters within text by position."), kText},
    {"UPPER", "text", TR("Converts text to uppercase."), kText},
    {"LOWER", "text", TR("Converts text to lowercase."), kText},
    {"PROPER", "text", TR("Capitalizes the first letter of each word."), kText},
    {"TRIM", "text", TR("Removes extra spaces."), kText},
    {"CLEAN", "text", TR("Removes nonprintable characters."), kText},
    {"TEXT", "value, format_text", TR("Converts a value to text using a number format."), kText},
    {"VALUE", "text", TR("Converts text to a number."), kText},
    {"NUMBERVALUE", "text, [decimal_separator], [group_separator]", TR("Converts text to a number using the given separators."), kText},
    {"CONCAT", "text1, [text2], ...", TR("Joins text."), kText},
    {"CONCATENATE", "text1, [text2], ...", TR("Joins text."), kText},
    {"TEXTJOIN", "delimiter, ignore_empty, text1, [text2], ...", TR("Joins text with a delimiter."), kText},
    {"TEXTSPLIT", "text, col_delimiter, [row_delimiter], [ignore_empty], [match_mode], [pad_with]", TR("Splits text into rows and columns at delimiters."), kText},
    {"REPT", "text, number_times", TR("Repeats text a given number of times."), kText},
    {"EXACT", "text1, text2", TR("Returns TRUE if two text values are exactly the same."), kText},
    {"CHAR", "number", TR("Returns the character for a code."), kText},
    {"CODE", "text", TR("Returns the code of the first character."), kText},
    {"UNICHAR", "number", TR("Returns the character for a Unicode code point."), kText},
    {"UNICODE", "text", TR("Returns the Unicode code point of the first character."), kText},
    {"T", "value", TR("Returns the text if the value is text; otherwise an empty string."), kText},
    {"FIXED", "number, [decimals], [no_commas]", TR("Rounds a number and converts it to text."), kText},
    {"DOLLAR", "number, [decimals]", TR("Converts a number to text in currency format."), kText},
    {"TEXTBEFORE", "text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]", TR("Returns the text before a delimiter."), kText},
    {"TEXTAFTER", "text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]", TR("Returns the text after a delimiter."), kText},
    // ---- YCGrid ----
    {"DECIMAL", "text, radix", TR("Converts a number written in a given base into a decimal number; [YC] with only text: an exact decimal number (up to 28 decimal places)."), kMath},
    {"DECIMAL.SCALE", "value, [places]", TR("[YC] Creates an exact decimal number with the given number of decimal places."), kYc},
    {"ISDECIMAL", "value", TR("[YC] Returns TRUE if the value is an exact decimal."), kYc},
    {"QUOTE", "symbol, field", TR("[YC] Real-time quotes (pushed by a data feed process)."), kYc},
    {"STREAM", "name, [topic1], [topic2], ...", TR("[YC] Live values from the Python @stream of that name, with the topics it is given."), kYc},
    // ---- statistical distributions, hyperbolic, DBCS text, compatibility names ----
    {"T.DIST", "x, deg_freedom, cumulative", TR("Returns the Student's t-distribution (left-tailed)."), kStat},
    {"T.DIST.2T", "x, deg_freedom", TR("Returns the two-tailed Student's t-distribution."), kStat},
    {"T.DIST.RT", "x, deg_freedom", TR("Returns the right-tailed Student's t-distribution."), kStat},
    {"T.INV", "probability, deg_freedom", TR("Returns the left-tailed inverse of the Student's t-distribution."), kStat},
    {"T.INV.2T", "probability, deg_freedom", TR("Returns the two-tailed inverse of the Student's t-distribution."), kStat},
    {"CHISQ.DIST", "x, deg_freedom, cumulative", TR("Returns the chi-squared distribution (left-tailed)."), kStat},
    {"CHISQ.DIST.RT", "x, deg_freedom", TR("Returns the right-tailed probability of the chi-squared distribution."), kStat},
    {"CHISQ.INV", "probability, deg_freedom", TR("Returns the inverse of the left-tailed chi-squared distribution."), kStat},
    {"CHISQ.INV.RT", "probability, deg_freedom", TR("Returns the inverse of the right-tailed chi-squared distribution."), kStat},
    {"F.DIST", "x, deg_freedom1, deg_freedom2, cumulative", TR("Returns the F probability distribution (left-tailed)."), kStat},
    {"F.DIST.RT", "x, deg_freedom1, deg_freedom2", TR("Returns the right-tailed F probability distribution."), kStat},
    {"F.INV", "probability, deg_freedom1, deg_freedom2", TR("Returns the inverse of the left-tailed F distribution."), kStat},
    {"F.INV.RT", "probability, deg_freedom1, deg_freedom2", TR("Returns the inverse of the right-tailed F distribution."), kStat},
    {"GAMMA.DIST", "x, alpha, beta, cumulative", TR("Returns the gamma distribution."), kStat},
    {"GAMMA.INV", "probability, alpha, beta", TR("Returns the inverse of the gamma cumulative distribution."), kStat},
    {"GAMMALN", "x", TR("Returns the natural logarithm of the gamma function."), kStat},
    {"GAMMALN.PRECISE", "x", TR("Returns the natural logarithm of the gamma function."), kStat},
    {"GAMMA", "number", TR("Returns the gamma function value."), kStat},
    {"BINOM.DIST", "number_s, trials, probability_s, cumulative", TR("Returns the binomial distribution probability."), kStat},
    {"POISSON.DIST", "x, mean, cumulative", TR("Returns the Poisson distribution."), kStat},
    {"EXPON.DIST", "x, lambda, cumulative", TR("Returns the exponential distribution."), kStat},
    {"WEIBULL.DIST", "x, alpha, beta, cumulative", TR("Returns the Weibull distribution."), kStat},
    {"LOGNORM.DIST", "x, mean, standard_dev, cumulative", TR("Returns the lognormal distribution."), kStat},
    {"LOGNORM.INV", "probability, mean, standard_dev", TR("Returns the inverse of the lognormal cumulative distribution."), kStat},
    {"CONFIDENCE.NORM", "alpha, standard_dev, size", TR("Returns the confidence interval for a population mean (normal distribution)."), kStat},
    {"CONFIDENCE.T", "alpha, standard_dev, size", TR("Returns the confidence interval for a population mean (Student's t-distribution)."), kStat},
    {"DEVSQ", "number1, [number2], ...", TR("Returns the sum of squares of deviations from the mean."), kStat},
    {"AVEDEV", "number1, [number2], ...", TR("Returns the average of the absolute deviations from the mean."), kStat},
    {"SKEW", "number1, [number2], ...", TR("Returns the skewness of a distribution."), kStat},
    {"KURT", "number1, [number2], ...", TR("Returns the kurtosis of a data set."), kStat},
    {"HARMEAN", "number1, [number2], ...", TR("Returns the harmonic mean."), kStat},
    {"TRIMMEAN", "array, percent", TR("Returns the mean of the interior of a data set."), kStat},
    {"PERCENTRANK.INC", "array, x, [significance]", TR("Returns the rank of a value as a percentage (0..1, inclusive)."), kStat},
    {"PERCENTRANK.EXC", "array, x, [significance]", TR("Returns the rank of a value as a percentage (0..1, exclusive)."), kStat},
    {"COVARIANCE.P", "array1, array2", TR("Returns the population covariance."), kStat},
    {"PEARSON", "array1, array2", TR("Returns the Pearson product moment correlation coefficient."), kStat},
    {"RSQ", "known_y's, known_x's", TR("Returns the square of the Pearson correlation coefficient."), kStat},
    {"STEYX", "known_y's, known_x's", TR("Returns the standard error of the predicted y-values of a regression."), kStat},
    {"SINH", "number", TR("Returns the hyperbolic sine."), kMath},
    {"COSH", "number", TR("Returns the hyperbolic cosine."), kMath},
    {"TANH", "number", TR("Returns the hyperbolic tangent."), kMath},
    {"ASINH", "number", TR("Returns the inverse hyperbolic sine."), kMath},
    {"ACOSH", "number", TR("Returns the inverse hyperbolic cosine."), kMath},
    {"ATANH", "number", TR("Returns the inverse hyperbolic tangent."), kMath},
    {"LENB", "text", TR("Returns the number of bytes of a text (double-byte characters count 2)."), kText},
    {"LEFTB", "text, [num_bytes]", TR("Returns the leftmost characters by number of bytes."), kText},
    {"RIGHTB", "text, [num_bytes]", TR("Returns the rightmost characters by number of bytes."), kText},
    {"MIDB", "text, start_num, num_bytes", TR("Returns characters from the middle of a text by bytes."), kText},
    {"FINDB", "find_text, within_text, [start_num]", TR("Finds a text in another (case-sensitive) and returns the byte position."), kText},
    {"SEARCHB", "find_text, within_text, [start_num]", TR("Finds a text in another (not case-sensitive) and returns the byte position."), kText},
    {"REPLACEB", "old_text, start_num, num_bytes, new_text", TR("Replaces part of a text by byte positions."), kText},
    {"ASC", "text", TR("Changes full-width letters, digits and symbols to half-width."), kText},
    {"DBCS", "text", TR("Changes half-width letters, digits and symbols to full-width."), kText},
    {"INFO", "type_text", TR("Returns information about the operating environment."), kInfo},
    {"TDIST", "x, deg_freedom, tails", TR("Returns the Student's t-distribution (older name)."), kCompat},
    {"TINV", "probability, deg_freedom", TR("Returns the two-tailed inverse of the Student's t-distribution (older name)."), kCompat},
    {"CHIDIST", "x, deg_freedom", TR("Returns the right-tailed chi-squared probability (older name)."), kCompat},
    {"CHIINV", "probability, deg_freedom", TR("Returns the inverse of the right-tailed chi-squared distribution (older name)."), kCompat},
    {"FDIST", "x, deg_freedom1, deg_freedom2", TR("Returns the right-tailed F distribution (older name)."), kCompat},
    {"FINV", "probability, deg_freedom1, deg_freedom2", TR("Returns the inverse of the right-tailed F distribution (older name)."), kCompat},
    {"GAMMADIST", "x, alpha, beta, cumulative", TR("Returns the gamma distribution (older name)."), kCompat},
    {"GAMMAINV", "probability, alpha, beta", TR("Returns the inverse of the gamma distribution (older name)."), kCompat},
    {"BINOMDIST", "number_s, trials, probability_s, cumulative", TR("Returns the binomial distribution probability (older name)."), kCompat},
    {"POISSON", "x, mean, cumulative", TR("Returns the Poisson distribution (older name)."), kCompat},
    {"EXPONDIST", "x, lambda, cumulative", TR("Returns the exponential distribution (older name)."), kCompat},
    {"WEIBULL", "x, alpha, beta, cumulative", TR("Returns the Weibull distribution (older name)."), kCompat},
    {"LOGNORMDIST", "x, mean, standard_dev", TR("Returns the cumulative lognormal distribution (older name)."), kCompat},
    {"LOGINV", "probability, mean, standard_dev", TR("Returns the inverse of the lognormal distribution (older name)."), kCompat},
    {"CONFIDENCE", "alpha, standard_dev, size", TR("Returns the confidence interval for a population mean (older name)."), kCompat},
    {"COVAR", "array1, array2", TR("Returns the population covariance (older name)."), kCompat},
    {"PERCENTRANK", "array, x, [significance]", TR("Returns the percentage rank of a value (older name)."), kCompat},
    // ---- engineering, database, more financial / math / statistics, text, web, summaries ----
    {"BIN2DEC", "number", TR("Converts a binary number to decimal."), kEng},
    {"BIN2HEX", "number, [places]", TR("Converts a binary number to hexadecimal."), kEng},
    {"BIN2OCT", "number, [places]", TR("Converts a binary number to octal."), kEng},
    {"DEC2BIN", "number, [places]", TR("Converts a decimal number to binary."), kEng},
    {"DEC2HEX", "number, [places]", TR("Converts a decimal number to hexadecimal."), kEng},
    {"DEC2OCT", "number, [places]", TR("Converts a decimal number to octal."), kEng},
    {"HEX2BIN", "number, [places]", TR("Converts a hexadecimal number to binary."), kEng},
    {"HEX2DEC", "number", TR("Converts a hexadecimal number to decimal."), kEng},
    {"HEX2OCT", "number, [places]", TR("Converts a hexadecimal number to octal."), kEng},
    {"OCT2BIN", "number, [places]", TR("Converts an octal number to binary."), kEng},
    {"OCT2DEC", "number", TR("Converts an octal number to decimal."), kEng},
    {"OCT2HEX", "number, [places]", TR("Converts an octal number to hexadecimal."), kEng},
    {"BITAND", "number1, number2", TR("Returns a bitwise AND of two numbers."), kEng},
    {"BITOR", "number1, number2", TR("Returns a bitwise OR of two numbers."), kEng},
    {"BITXOR", "number1, number2", TR("Returns a bitwise exclusive OR of two numbers."), kEng},
    {"BITLSHIFT", "number, shift_amount", TR("Returns a number shifted left by a number of bits."), kEng},
    {"BITRSHIFT", "number, shift_amount", TR("Returns a number shifted right by a number of bits."), kEng},
    {"DELTA", "number1, [number2]", TR("Tests whether two values are equal (1 or 0)."), kEng},
    {"GESTEP", "number, [step]", TR("Tests whether a number is greater than or equal to a threshold (1 or 0)."), kEng},
    {"ERF", "lower_limit, [upper_limit]", TR("Returns the error function."), kEng},
    {"ERF.PRECISE", "x", TR("Returns the error function."), kEng},
    {"ERFC", "x", TR("Returns the complementary error function."), kEng},
    {"ERFC.PRECISE", "x", TR("Returns the complementary error function."), kEng},
    {"BESSELI", "x, n", TR("Returns the modified Bessel function In(x)."), kEng},
    {"BESSELJ", "x, n", TR("Returns the Bessel function Jn(x)."), kEng},
    {"BESSELK", "x, n", TR("Returns the modified Bessel function Kn(x)."), kEng},
    {"BESSELY", "x, n", TR("Returns the Bessel function Yn(x)."), kEng},
    {"CONVERT", "number, from_unit, to_unit", TR("Converts a number from one measurement unit to another."), kEng},
    {"COMPLEX", "real_num, i_num, [suffix]", TR("Converts real and imaginary parts into a complex number."), kEng},
    {"IMREAL", "inumber", TR("Returns the real part of a complex number."), kEng},
    {"IMAGINARY", "inumber", TR("Returns the imaginary part of a complex number."), kEng},
    {"IMABS", "inumber", TR("Returns the absolute value (modulus) of a complex number."), kEng},
    {"IMARGUMENT", "inumber", TR("Returns the argument (angle in radians) of a complex number."), kEng},
    {"IMCONJUGATE", "inumber", TR("Returns the complex conjugate of a complex number."), kEng},
    {"IMSQRT", "inumber", TR("Returns the square root of a complex number."), kEng},
    {"IMEXP", "inumber", TR("Returns the exponential of a complex number."), kEng},
    {"IMLN", "inumber", TR("Returns the natural logarithm of a complex number."), kEng},
    {"IMLOG10", "inumber", TR("Returns the base-10 logarithm of a complex number."), kEng},
    {"IMLOG2", "inumber", TR("Returns the base-2 logarithm of a complex number."), kEng},
    {"IMSIN", "inumber", TR("Returns the sine of a complex number."), kEng},
    {"IMCOS", "inumber", TR("Returns the cosine of a complex number."), kEng},
    {"IMTAN", "inumber", TR("Returns the tangent of a complex number."), kEng},
    {"IMSINH", "inumber", TR("Returns the hyperbolic sine of a complex number."), kEng},
    {"IMCOSH", "inumber", TR("Returns the hyperbolic cosine of a complex number."), kEng},
    {"IMCOT", "inumber", TR("Returns the cotangent of a complex number."), kEng},
    {"IMCSC", "inumber", TR("Returns the cosecant of a complex number."), kEng},
    {"IMCSCH", "inumber", TR("Returns the hyperbolic cosecant of a complex number."), kEng},
    {"IMSEC", "inumber", TR("Returns the secant of a complex number."), kEng},
    {"IMSECH", "inumber", TR("Returns the hyperbolic secant of a complex number."), kEng},
    {"IMPOWER", "inumber, number", TR("Returns a complex number raised to a power."), kEng},
    {"IMDIV", "inumber1, inumber2", TR("Returns the quotient of two complex numbers."), kEng},
    {"IMSUB", "inumber1, inumber2", TR("Returns the difference of two complex numbers."), kEng},
    {"IMSUM", "inumber1, [inumber2], ...", TR("Returns the sum of complex numbers."), kEng},
    {"IMPRODUCT", "inumber1, [inumber2], ...", TR("Returns the product of complex numbers."), kEng},
    {"DAVERAGE", "database, field, criteria", TR("Averages the values of a field in the records that match the criteria."), kDb},
    {"DCOUNT", "database, [field], criteria", TR("Counts the numbers in a field of the records that match the criteria."), kDb},
    {"DCOUNTA", "database, [field], criteria", TR("Counts the non-blank cells in a field of the records that match the criteria."), kDb},
    {"DGET", "database, field, criteria", TR("Returns the single value of a field that matches the criteria."), kDb},
    {"DMAX", "database, field, criteria", TR("Returns the largest value of a field in the matching records."), kDb},
    {"DMIN", "database, field, criteria", TR("Returns the smallest value of a field in the matching records."), kDb},
    {"DPRODUCT", "database, field, criteria", TR("Multiplies the values of a field in the matching records."), kDb},
    {"DSTDEV", "database, field, criteria", TR("Estimates the standard deviation from a sample of matching records."), kDb},
    {"DSTDEVP", "database, field, criteria", TR("Calculates the standard deviation of the whole population of matching records."), kDb},
    {"DSUM", "database, field, criteria", TR("Adds the values of a field in the records that match the criteria."), kDb},
    {"DVAR", "database, field, criteria", TR("Estimates the variance from a sample of matching records."), kDb},
    {"DVARP", "database, field, criteria", TR("Calculates the variance of the whole population of matching records."), kDb},
    {"COUPDAYBS", "settlement, maturity, frequency, [basis]", TR("Returns the days from the start of the coupon period to the settlement date."), kFin},
    {"COUPDAYS", "settlement, maturity, frequency, [basis]", TR("Returns the days in the coupon period that contains the settlement date."), kFin},
    {"COUPDAYSNC", "settlement, maturity, frequency, [basis]", TR("Returns the days from the settlement date to the next coupon date."), kFin},
    {"COUPNCD", "settlement, maturity, frequency, [basis]", TR("Returns the next coupon date after the settlement date."), kFin},
    {"COUPNUM", "settlement, maturity, frequency, [basis]", TR("Returns the number of coupons payable between settlement and maturity."), kFin},
    {"COUPPCD", "settlement, maturity, frequency, [basis]", TR("Returns the previous coupon date before the settlement date."), kFin},
    {"PRICE", "settlement, maturity, rate, yld, redemption, frequency, [basis]", TR("Returns the price per 100 face value of a security that pays periodic interest."), kFin},
    {"YIELD", "settlement, maturity, rate, pr, redemption, frequency, [basis]", TR("Returns the yield of a security that pays periodic interest."), kFin},
    {"DURATION", "settlement, maturity, coupon, yld, frequency, [basis]", TR("Returns the Macaulay duration of a security with periodic interest."), kFin},
    {"MDURATION", "settlement, maturity, coupon, yld, frequency, [basis]", TR("Returns the modified Macaulay duration of a security."), kFin},
    {"ODDFPRICE", "settlement, maturity, issue, first_coupon, rate, yld, redemption, frequency, [basis]", TR("Returns the price of a security with an odd first period."), kFin},
    {"ODDFYIELD", "settlement, maturity, issue, first_coupon, rate, pr, redemption, frequency, [basis]", TR("Returns the yield of a security with an odd first period."), kFin},
    {"ODDLPRICE", "settlement, maturity, last_interest, rate, yld, redemption, frequency, [basis]", TR("Returns the price of a security with an odd last period."), kFin},
    {"ODDLYIELD", "settlement, maturity, last_interest, rate, pr, redemption, frequency, [basis]", TR("Returns the yield of a security with an odd last period."), kFin},
    {"ACCRINT", "issue, first_interest, settlement, rate, par, frequency, [basis], [calc_method]", TR("Returns the accrued interest of a security that pays periodic interest."), kFin},
    {"ACCRINTM", "issue, settlement, rate, [par], [basis]", TR("Returns the accrued interest of a security that pays interest at maturity."), kFin},
    {"PRICEDISC", "settlement, maturity, discount, redemption, [basis]", TR("Returns the price per 100 face value of a discounted security."), kFin},
    {"YIELDDISC", "settlement, maturity, pr, redemption, [basis]", TR("Returns the annual yield of a discounted security."), kFin},
    {"DISC", "settlement, maturity, pr, redemption, [basis]", TR("Returns the discount rate of a security."), kFin},
    {"INTRATE", "settlement, maturity, investment, redemption, [basis]", TR("Returns the interest rate of a fully invested security."), kFin},
    {"RECEIVED", "settlement, maturity, investment, discount, [basis]", TR("Returns the amount received at maturity for a fully invested security."), kFin},
    {"PRICEMAT", "settlement, maturity, issue, rate, yld, [basis]", TR("Returns the price per 100 face value of a security that pays interest at maturity."), kFin},
    {"YIELDMAT", "settlement, maturity, issue, rate, pr, [basis]", TR("Returns the annual yield of a security that pays interest at maturity."), kFin},
    {"TBILLPRICE", "settlement, maturity, discount", TR("Returns the price per 100 face value of a Treasury bill."), kFin},
    {"TBILLYIELD", "settlement, maturity, pr", TR("Returns the yield of a Treasury bill."), kFin},
    {"TBILLEQ", "settlement, maturity, discount", TR("Returns the bond-equivalent yield of a Treasury bill."), kFin},
    {"SLN", "cost, salvage, life", TR("Returns the straight-line depreciation of an asset for one period."), kFin},
    {"SYD", "cost, salvage, life, per", TR("Returns the sum-of-years' digits depreciation of an asset for a period."), kFin},
    {"DB", "cost, salvage, life, period, [month]", TR("Returns the fixed-declining balance depreciation of an asset for a period."), kFin},
    {"DDB", "cost, salvage, life, period, [factor]", TR("Returns the double-declining balance depreciation of an asset for a period."), kFin},
    {"VDB", "cost, salvage, life, start_period, end_period, [factor], [no_switch]", TR("Returns the depreciation of an asset for any period (declining balance)."), kFin},
    {"AMORLINC", "cost, date_purchased, first_period, salvage, period, rate, [basis]", TR("Returns the depreciation for each accounting period (French system, linear)."), kFin},
    {"AMORDEGRC", "cost, date_purchased, first_period, salvage, period, rate, [basis]", TR("Returns the depreciation for each accounting period (French system, degressive)."), kFin},
    {"EFFECT", "nominal_rate, npery", TR("Returns the effective annual interest rate."), kFin},
    {"NOMINAL", "effect_rate, npery", TR("Returns the nominal annual interest rate."), kFin},
    {"FVSCHEDULE", "principal, schedule", TR("Returns the future value of a principal after a series of compound rates."), kFin},
    {"ISPMT", "rate, per, nper, pv", TR("Returns the interest paid during a period of an investment (straight principal)."), kFin},
    {"MIRR", "values, finance_rate, reinvest_rate", TR("Returns the modified internal rate of return of periodic cash flows."), kFin},
    {"PDURATION", "rate, pv, fv", TR("Returns the number of periods an investment needs to reach a value."), kFin},
    {"RRI", "nper, pv, fv", TR("Returns an equivalent interest rate for the growth of an investment."), kFin},
    {"CUMIPMT", "rate, nper, pv, start_period, end_period, type", TR("Returns the cumulative interest paid between two periods."), kFin},
    {"CUMPRINC", "rate, nper, pv, start_period, end_period, type", TR("Returns the cumulative principal paid between two periods."), kFin},
    {"DOLLARDE", "fractional_dollar, fraction", TR("Converts a price expressed as a fraction into a decimal number."), kFin},
    {"DOLLARFR", "decimal_dollar, fraction", TR("Converts a decimal price into a price expressed as a fraction."), kFin},
    {"ACOT", "number", TR("Returns the arccotangent of a number."), kMath},
    {"ACOTH", "number", TR("Returns the hyperbolic arccotangent of a number."), kMath},
    {"COT", "number", TR("Returns the cotangent of an angle."), kMath},
    {"COTH", "number", TR("Returns the hyperbolic cotangent of a number."), kMath},
    {"CSC", "number", TR("Returns the cosecant of an angle."), kMath},
    {"CSCH", "number", TR("Returns the hyperbolic cosecant of an angle."), kMath},
    {"SEC", "number", TR("Returns the secant of an angle."), kMath},
    {"SECH", "number", TR("Returns the hyperbolic secant of an angle."), kMath},
    {"ROMAN", "number, [form]", TR("Converts a number to Roman numerals, as text."), kMath},
    {"ARABIC", "text", TR("Converts a Roman numeral to a number."), kMath},
    {"BASE", "number, radix, [min_length]", TR("Converts a number into text in a given base (radix)."), kMath},
    {"CEILING.PRECISE", "number, [significance]", TR("Rounds a number up to the nearest multiple of significance."), kMath},
    {"ISO.CEILING", "number, [significance]", TR("Rounds a number up to the nearest multiple of significance."), kMath},
    {"FLOOR.PRECISE", "number, [significance]", TR("Rounds a number down to the nearest multiple of significance."), kMath},
    {"COMBINA", "number, number_chosen", TR("Returns the number of combinations with repetitions."), kMath},
    {"FACTDOUBLE", "number", TR("Returns the double factorial of a number."), kMath},
    {"MULTINOMIAL", "number1, [number2], ...", TR("Returns the multinomial of a set of numbers."), kMath},
    {"SERIESSUM", "x, n, m, coefficients", TR("Returns the sum of a power series."), kMath},
    {"SQRTPI", "number", TR("Returns the square root of (number * pi)."), kMath},
    {"SUMX2MY2", "array_x, array_y", TR("Returns the sum of the differences of squares of two arrays."), kMath},
    {"SUMX2PY2", "array_x, array_y", TR("Returns the sum of the sums of squares of two arrays."), kMath},
    {"SUMXMY2", "array_x, array_y", TR("Returns the sum of squares of the differences of two arrays."), kMath},
    {"MDETERM", "array", TR("Returns the matrix determinant of an array."), kMath},
    {"MINVERSE", "array", TR("Returns the inverse matrix of an array."), kMath},
    {"MMULT", "array1, array2", TR("Returns the matrix product of two arrays."), kMath},
    {"MUNIT", "dimension", TR("Returns the unit matrix of a dimension."), kMath},
    {"STDEVA", "value1, [value2], ...", TR("Estimates standard deviation, counting text as 0 and logicals as 1 / 0."), kStat},
    {"STDEVPA", "value1, [value2], ...", TR("Calculates population standard deviation, counting text and logicals."), kStat},
    {"VARA", "value1, [value2], ...", TR("Estimates variance, counting text as 0 and logicals as 1 / 0."), kStat},
    {"VARPA", "value1, [value2], ...", TR("Calculates population variance, counting text and logicals."), kStat},
    {"SKEW.P", "number1, [number2], ...", TR("Returns the skewness of a population."), kStat},
    {"BETA.DIST", "x, alpha, beta, cumulative, [A], [B]", TR("Returns the beta distribution."), kStat},
    {"BETADIST", "x, alpha, beta, [A], [B]", TR("Returns the cumulative beta distribution."), kCompat},
    {"BETA.INV", "probability, alpha, beta, [A], [B]", TR("Returns the inverse of the cumulative beta distribution."), kStat},
    {"BETAINV", "probability, alpha, beta, [A], [B]", TR("Returns the inverse of the cumulative beta distribution."), kCompat},
    {"BINOM.DIST.RANGE", "trials, probability_s, number_s, [number_s2]", TR("Returns the probability of a trial result range using a binomial distribution."), kStat},
    {"BINOM.INV", "trials, probability_s, alpha", TR("Returns the smallest value whose cumulative binomial distribution is at least alpha."), kStat},
    {"CRITBINOM", "trials, probability_s, alpha", TR("Returns the smallest value whose cumulative binomial distribution is at least alpha."), kCompat},
    {"CHISQ.TEST", "actual_range, expected_range", TR("Returns the chi-squared test for independence."), kStat},
    {"CHITEST", "actual_range, expected_range", TR("Returns the chi-squared test for independence."), kCompat},
    {"F.TEST", "array1, array2", TR("Returns the result of an F-test."), kStat},
    {"FTEST", "array1, array2", TR("Returns the result of an F-test."), kCompat},
    {"T.TEST", "array1, array2, tails, type", TR("Returns the probability associated with a Student's t-test."), kStat},
    {"TTEST", "array1, array2, tails, type", TR("Returns the probability associated with a Student's t-test."), kCompat},
    {"Z.TEST", "array, x, [sigma]", TR("Returns the one-tailed P-value of a z-test."), kStat},
    {"ZTEST", "array, x, [sigma]", TR("Returns the one-tailed P-value of a z-test."), kCompat},
    {"FISHER", "x", TR("Returns the Fisher transformation."), kStat},
    {"FISHERINV", "y", TR("Returns the inverse of the Fisher transformation."), kStat},
    {"GAUSS", "z", TR("Returns 0.5 less than the standard normal cumulative distribution."), kStat},
    {"PHI", "x", TR("Returns the density function of the standard normal distribution."), kStat},
    {"HYPGEOM.DIST", "sample_s, number_sample, population_s, number_pop, cumulative", TR("Returns the hypergeometric distribution."), kStat},
    {"HYPGEOMDIST", "sample_s, number_sample, population_s, number_pop", TR("Returns the hypergeometric distribution."), kCompat},
    {"NEGBINOM.DIST", "number_f, number_s, probability_s, cumulative", TR("Returns the negative binomial distribution."), kStat},
    {"NEGBINOMDIST", "number_f, number_s, probability_s", TR("Returns the negative binomial distribution."), kCompat},
    {"PERMUT", "number, number_chosen", TR("Returns the number of permutations."), kStat},
    {"PERMUTATIONA", "number, number_chosen", TR("Returns the number of permutations with repetitions."), kStat},
    {"PROB", "x_range, prob_range, lower_limit, [upper_limit]", TR("Returns the probability that values are between two limits."), kStat},
    {"QUARTILE.EXC", "array, quart", TR("Returns the quartile of a data set (0 and 1 exclusive)."), kStat},
    {"FREQUENCY", "data_array, bins_array", TR("Returns a frequency distribution as a vertical array."), kStat},
    {"MODE.MULT", "number1, [number2], ...", TR("Returns a vertical array of the most frequent values."), kStat},
    {"LINEST", "known_y's, [known_x's], [const], [stats]", TR("Returns the statistics of a least-squares linear fit."), kStat},
    {"LOGEST", "known_y's, [known_x's], [const], [stats]", TR("Returns the statistics of an exponential curve fit."), kStat},
    {"TREND", "known_y's, [known_x's], [new_x's], [const]", TR("Returns values along a linear trend."), kStat},
    {"GROWTH", "known_y's, [known_x's], [new_x's], [const]", TR("Returns values along an exponential trend."), kStat},
    {"FORECAST.ETS", "target_date, values, timeline, [seasonality], [data_completion], [aggregation]", TR("Forecasts a future value with exponential triple smoothing."), kStat},
    {"FORECAST.ETS.CONFINT", "target_date, values, timeline, [confidence_level], [seasonality], [data_completion], [aggregation]", TR("Returns the confidence interval of a forecast value."), kStat},
    {"FORECAST.ETS.SEASONALITY", "values, timeline, [data_completion], [aggregation]", TR("Returns the length of the repetitive pattern Excel detects in a time series."), kStat},
    {"FORECAST.ETS.STAT", "values, timeline, statistic_type, [seasonality], [data_completion], [aggregation]", TR("Returns a statistic of the exponential smoothing forecast."), kStat},
    {"VALUETOTEXT", "value, [format]", TR("Returns text from any value."), kText},
    {"ARRAYTOTEXT", "array, [format]", TR("Returns an array of values as text."), kText},
    {"BAHTTEXT", "number", TR("Converts a number to Thai text and adds a suffix of Baht."), kText},
    {"PHONETIC", "reference", TR("Extracts the phonetic (furigana) characters from a text string."), kText},
    {"REGEXTEST", "text, pattern, [case_sensitivity]", TR("Checks whether any part of text matches a regular expression."), kText},
    {"REGEXEXTRACT", "text, pattern, [return_mode], [case_sensitivity]", TR("Extracts text that matches a regular expression."), kText},
    {"REGEXREPLACE", "text, pattern, replacement, [occurrence], [case_sensitivity]", TR("Replaces text that matches a regular expression."), kText},
    {"ENCODEURL", "text", TR("Returns a URL-encoded string."), kWeb},
    {"FILTERXML", "xml, xpath", TR("Returns specific data from XML content by using an XPath."), kWeb},
    {"WEBSERVICE", "url", TR("Returns data from a web service (downloaded in the background)."), kWeb},
    {"STOCKHISTORY", "stock, start_date, [end_date], [interval], [headers], [property0], ...", TR("Returns historical prices of a stock (downloaded in the background)."), kWeb},
    {"TRANSLATE", "text, [source_language], [target_language]", TR("Translates text from one language to another (downloaded in the background)."), kWeb},
    {"NUMBERSTRING", "number, type", TR("Writes a number in Chinese numerals: 1 with units, 2 financial numerals, 3 digit by digit."), kText},
    {"DATESTRING", "date", TR("Writes a date in the Republic of China calendar."), kText},
    {"USDOLLAR", "number, [decimals]", TR("Converts a number to text in US dollar currency format."), kText},
    {"DETECTLANGUAGE", "text", TR("Returns the language code of a text, such as \"en\" or \"zh-Hant\"."), kWeb},
    {"PY", "code, [range1], [range2], ...",
     TR("Runs Python in YCGrid's Python host and spills the result (a DataFrame with its header, a Series, an array or a value). x is the "
        "first range (a table with a header row becomes a DataFrame), _1, _2... each range; what the VS Code kernel defined is there."),
     kLookup},
    {"IMAGE", "source, [alt_text], [sizing], [height], [width]", TR("Shows a picture from a URL in the cell."), kLookup},
    {"TRIMRANGE", "range, [trim_rows], [trim_cols]", TR("Trims the empty rows and columns from the edges of a range."), kLookup},
    {"AREAS", "reference", TR("Returns the number of areas in a reference."), kLookup},
    {"GROUPBY", "row_fields, values, function, [field_headers], [total_depth], [sort_order], [filter_array], [field_relationship]", TR("Groups rows by fields and aggregates the values (a summary like a PivotTable)."), kLookup},
    {"PIVOTBY", "row_fields, col_fields, values, function, [field_headers], [row_total_depth], [row_sort_order], [col_total_depth], [col_sort_order], [filter_array], [relative_to]", TR("Groups rows and columns by fields and aggregates the values."), kLookup},
    {"PERCENTOF", "data_subset, data_all", TR("Returns the percentage that a subset makes up of all the data."), kMath},
};

std::string Upper(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}

}  // namespace

namespace {
// the @udf functions' help: each list is kept for good once handed out (they are small and change rarely), so the
// const char* of an entry a dialog holds stays valid after the next list
struct UserList {
  std::vector<UserFunc> funcs;
  std::vector<FuncHelp> help;
};
std::mutex g_user_mu;
std::vector<std::unique_ptr<UserList>> g_user_lists;
const UserList* g_user = nullptr;
}  // namespace

void SetUserFuncHelp(std::vector<UserFunc> funcs) {
  auto l = std::make_unique<UserList>();
  l->funcs = std::move(funcs);
  for (UserFunc& f : l->funcs) {
    f.name = Upper(f.name);
    // a category given in English (MacroOptions' "Financial") is the UI's own one (the table translates it)
    f.category = f.category.empty() ? std::string(TR("User Defined")) : std::string(::yc::i18n::Tr(f.category.c_str()));
    if (f.desc.empty()) f.desc = TR("No help available.");
  }
  for (const UserFunc& f : l->funcs) l->help.push_back({f.name.c_str(), f.args.c_str(), f.desc.c_str(), f.category.c_str()});
  std::lock_guard<std::mutex> lk(g_user_mu);
  g_user = l.get();
  g_user_lists.push_back(std::move(l));
}

std::vector<FuncHelp> UserFuncHelp() {
  std::lock_guard<std::mutex> lk(g_user_mu);
  return g_user ? g_user->help : std::vector<FuncHelp>{};
}

const FuncHelp* FindFuncHelp(std::string_view upper_name) {
  const std::string u = Upper(upper_name);
  for (const auto& h : kHelp) {
    if (u == h.name) return &h;
  }
  std::lock_guard<std::mutex> lk(g_user_mu);
  if (g_user) {
    for (const FuncHelp& h : g_user->help) {
      if (u == h.name) return &h;
    }
  }
  return nullptr;
}

const std::vector<FuncHelp>& AllFuncHelp() { return kHelp; }

namespace {

// the arguments' descriptions, by the argument's name without its number (number1, number2 -> number); a function
// can describe its own (fn != nullptr) before the shared ones
struct ArgHelp {
  const char* fn;
  const char* arg;
  const char* text;
};
const ArgHelp kArgHelp[] = {
    {"SUM", "number", TR("number1,number2,... are 1 to 255 numbers to sum. Logical values and text are ignored in cells, included if typed as arguments.")},
    {"AVERAGE", "number", TR("number1,number2,... are 1 to 255 numeric arguments for which you want the average.")},
    {"COUNT", "value", TR("value1,value2,... are 1 to 255 arguments that can contain or refer to a variety of different types of data, but only numbers are counted.")},
    {"COUNTA", "value", TR("value1,value2,... are 1 to 255 arguments representing the values you want to count. Values can be any type of information.")},
    {"MAX", "number", TR("number1,number2,... are 1 to 255 numbers, empty cells, logical values, or text numbers for which you want the maximum.")},
    {"MIN", "number", TR("number1,number2,... are 1 to 255 numbers, empty cells, logical values, or text numbers for which you want the minimum.")},
    {"ROUND", "number", TR("is the number you want to round.")},
    {"ROUND", "num_digits", TR("is the number of digits to which you want to round. Negative rounds to the left of the decimal point; zero to the nearest integer.")},
    {"IFERROR", "value", TR("is any value or expression or reference.")},
    {nullptr, "lookup_value", TR("is the value to be found. It can be a value, a reference or a text string.")},
    {nullptr, "table_array", TR("is a table of text, numbers, or logical values, in which data is retrieved. It can be a reference to a range or a range name.")},
    {nullptr, "col_index_num", TR("is the column number in table_array from which the matching value should be returned. The first column of values in the table is column 1.")},
    {nullptr, "row_index_num", TR("is the row number in table_array from which the matching value should be returned. The first row of values in the table is row 1.")},
    {nullptr, "range_lookup", TR("is a logical value: to find the closest match in the first column (sorted in ascending order) = TRUE or omitted; find an exact match = FALSE.")},
    {nullptr, "lookup_array", TR("is the array or range to search.")},
    {nullptr, "return_array", TR("is the array or range to return.")},
    {nullptr, "if_not_found", TR("is returned if no match is found.")},
    {nullptr, "match_mode", TR("specifies how to match lookup_value against the values in lookup_array: 0 exact match (default), -1 exact or next smaller, 1 exact or next larger, 2 wildcard match.")},
    {nullptr, "search_mode", TR("specifies the search mode: 1 search from first to last (default), -1 from last to first, 2 binary search ascending, -2 binary search descending.")},
    {nullptr, "match_type", TR("is a number 1, 0, or -1 indicating which value to return.")},
    {nullptr, "logical_test", TR("is any value or expression that can be evaluated to TRUE or FALSE.")},
    {nullptr, "value_if_true", TR("is the value that is returned if logical_test is TRUE. If omitted, TRUE is returned.")},
    {nullptr, "value_if_false", TR("is the value that is returned if logical_test is FALSE. If omitted, FALSE is returned.")},
    {nullptr, "value_if_error", TR("is any value or expression or reference.")},
    {nullptr, "logical", TR("logical1,logical2,... are 1 to 255 conditions you want to test that can be either TRUE or FALSE.")},
    {nullptr, "range", TR("is the range of cells you want evaluated.")},
    {nullptr, "criteria", TR("is the condition in the form of a number, expression, or text that defines which cells will be counted or added.")},
    {nullptr, "criteria_range", TR("is the range of cells you want evaluated for the particular condition.")},
    {nullptr, "sum_range", TR("are the actual cells to sum. If omitted, the cells in range are used.")},
    {nullptr, "average_range", TR("are the actual cells to be used to find the average. If omitted, the cells in range are used.")},
    {nullptr, "array", TR("is a range of cells or an array constant.")},
    {nullptr, "reference", TR("is a reference to one or more cells.")},
    {nullptr, "row_num", TR("selects the row in array or reference from which to return a value.")},
    {nullptr, "column_num", TR("selects the column in array or reference from which to return a value.")},
    {nullptr, "number", TR("is the number (or a reference to a cell holding it).")},
    {nullptr, "num_digits", TR("is the number of digits.")},
    {nullptr, "text", TR("is the text (or a reference to a cell holding text).")},
    {nullptr, "start_num", TR("is the position of the first character you want to extract. The first character in text is 1.")},
    {nullptr, "num_chars", TR("specifies how many characters to return.")},
    {nullptr, "value", TR("is the value to test or use.")},
    {nullptr, "rate", TR("is the interest rate per period.")},
    {nullptr, "nper", TR("is the total number of payment periods in an investment.")},
    {nullptr, "pmt", TR("is the payment made each period; it cannot change over the life of the investment.")},
    {nullptr, "pv", TR("is the present value: the total amount that a series of future payments is worth now.")},
    {nullptr, "fv", TR("is the future value, or a cash balance you want to attain after the last payment is made (0 if omitted).")},
    {nullptr, "type", TR("is a logical value: payment at the beginning of the period = 1; payment at the end of the period = 0 or omitted.")},
    {nullptr, "date", TR("is a date serial number (or text that can be read as a date).")},
    {nullptr, "start_date", TR("is a serial date number that represents the start date.")},
    {nullptr, "end_date", TR("is a serial date number that represents the end date.")},
    {nullptr, "rows", TR("is the number of rows to fill.")},
    {nullptr, "columns", TR("is the number of columns to fill.")},
    {nullptr, "start", TR("is the first number in the sequence.")},
    {nullptr, "step", TR("is the amount to increment each subsequent value in the sequence.")},
    {nullptr, "include", TR("is a Boolean array whose height or width is the same as the array.")},
    {nullptr, "if_empty", TR("is the value returned if all values in the included array are empty (filter returns nothing).")},
    {nullptr, "by_col", TR("is a logical value: compare columns (TRUE) or rows (FALSE or omitted).")},
    {nullptr, "sort_index", TR("is a number indicating the row or column to sort by.")},
    {nullptr, "sort_order", TR("is a number indicating the sort order: 1 for ascending (default), -1 for descending.")},
};

}  // namespace

const char* FuncArgHelp(std::string_view fn, std::string_view arg) {
  std::string a(arg);
  a.erase(std::remove(a.begin(), a.end(), '['), a.end());
  a.erase(std::remove(a.begin(), a.end(), ']'), a.end());
  while (!a.empty() && (a.back() == ' ' || std::isdigit(static_cast<unsigned char>(a.back())))) a.pop_back();
  for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const std::string f = Upper(fn);
  {
    // an @udf's own descriptions (Application.MacroOptions' ArgumentDescriptions), by its argument's name
    std::lock_guard<std::mutex> lk(g_user_mu);
    if (g_user) {
      for (const UserFunc& u : g_user->funcs) {
        if (u.name != f) continue;
        for (const auto& [n, text] : u.arg_help) {
          std::string ln = n;
          for (char& ch : ln) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          if (ln == a) return text.c_str();
        }
        return "";
      }
    }
  }
  for (const ArgHelp& h : kArgHelp) {
    if (h.fn && f == h.fn && a == h.arg) return h.text;
  }
  for (const ArgHelp& h : kArgHelp) {
    if (!h.fn && a == h.arg) return h.text;
  }
  return "";
}

std::vector<const char*> FuncCategories() {
  std::vector<const char*> out;
  auto add = [&](const char* cat) {
    if (std::find_if(out.begin(), out.end(), [&](const char* c) { return std::strcmp(c, cat) == 0; }) == out.end()) out.push_back(cat);
  };
  for (const auto& h : kHelp) add(h.category);
  for (const FuncHelp& h : UserFuncHelp()) add(h.category);  // "User Defined", after Excel's own
  return out;
}

}  // namespace yc::formula
