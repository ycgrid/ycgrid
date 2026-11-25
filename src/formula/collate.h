// SPDX-License-Identifier: Apache-2.0
// collate.h - the order Excel gives texts (Windows' word sort, case ignored)
#pragma once

#include <string_view>

namespace yc::formula {

// -1 / 0 / 1 for two texts already lowered (fn::Lower). Excel orders text with Windows' word sort: hyphens and
// apostrophes count only to break ties (co-op after coop, both before cop), other punctuation comes before digits and
// digits before letters, and accented letters sit beside their base letter (e acute before f). 0 only when the texts
// are the same.
int CollateLowered(std::string_view a, std::string_view b);
// the same with case (Sort's Case sensitive): the letters decide first, then case (lowercase first), and hyphens and
// apostrophes only after that (a'b, Ab, AB; Excel 16, measured)
int CollateCased(std::string_view a, std::string_view b);

}  // namespace yc::formula
