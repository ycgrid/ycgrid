// SPDX-License-Identifier: Apache-2.0
#include "frame/frame_context.h"

#include <atomic>

namespace yc::ui {
namespace {
std::atomic<double> g_deadline{-1.0};  // bridge threads (live plot data) ask for redraws too
}

void RequestRedrawIn(double seconds) {
  const double t = ImGui::GetTime() + (seconds < 0.0 ? 0.0 : seconds);
  double cur = g_deadline.load();
  while ((cur < 0.0 || t < cur) && !g_deadline.compare_exchange_weak(cur, t)) {
  }
}

double ConsumeRedrawDeadline() { return g_deadline.exchange(-1.0); }

}  // namespace yc::ui
