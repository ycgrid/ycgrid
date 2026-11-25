// SPDX-License-Identifier: Apache-2.0
// criteria.h - criteria ranges of the database functions (DSUM...) and Advanced Filter
#pragma once

#include "formula/eval.h"

#include <vector>

namespace yc::formula {

// which data rows match a criteria range; both have their labels in row 0. Conditions in one criteria row must all
// hold, separate rows are alternatives; text without an operator matches values that begin with it.
// Result: one entry per data row after the labels (index 0 = data row 1).
std::vector<char> MatchCriteriaRows(const Array& data, const Array& criteria);

}  // namespace yc::formula
