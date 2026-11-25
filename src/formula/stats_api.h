// SPDX-License-Identifier: Apache-2.0
// stats_api.h - [YC] distribution functions of the formula library for other code (Analysis ToolPak, engine/analysis.cpp)
#pragma once

namespace yc::formula::stats {

double TCdf(double t, double df);           // Student's t, left tail
double TInv(double p, double df);           // left-tailed inverse
double FCdf(double x, double d1, double d2);
double FInv(double p, double d1, double d2);  // left-tailed inverse
double TTail(double t, double df);           // P(T > t), accurate for tiny tails
double FTail(double x, double d1, double d2);  // P(F > x)
double NormSCdf(double z);
double NormSTail(double z);                  // P(Z > z)
double NormSInv(double p);

}  // namespace yc::formula::stats
