// SPDX-License-Identifier: Apache-2.0
// sparkline.h - what a sparkline draws: its values with the group's Hidden & Empty Cells settings, its vertical axis
// (each sparkline's own, the same for the group, or custom) and its dates (Date Axis Type); shared by the screen and
// the printout
#pragma once

#include "model/objects.h"
#include "model/workbook.h"

#include <vector>

namespace yc::engine {

// the values of one sparkline (NaN = a gap): hidden rows and columns left out unless the group shows them, empty
// cells as gaps, zero or joined (a line), in reverse with Plot Data Right-to-Left. xfrac (optional): each point's
// place across the cell (0..1) on a date axis, left empty otherwise
std::vector<double> SparklineValues(const model::Workbook& wb, const model::SparklineGroup& g, const model::Sparkline& it, std::vector<double>* xfrac = nullptr);

// the group's vertical axis bounds: *lo / *hi NaN when each sparkline uses its own minimum / maximum
void SparklineAxis(const model::Workbook& wb, const model::SparklineGroup& g, double* lo, double* hi);

// Group: the sparklines of the groups touching areas become one group with the look of the group at (r, c) (or the
// first one); returns its id (0: nothing to group)
// Ungroup: every sparkline of those groups becomes a group of its own
// (implemented in document_shapes.cpp as Document members)

}  // namespace yc::engine
