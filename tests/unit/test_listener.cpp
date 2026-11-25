// SPDX-License-Identifier: Apache-2.0
// Every test case starts in English, whatever the machine's display language and whatever the test before it left:
// Catch2 runs the cases in random order, and a case that read UI text ("Sum of Sales") passed only when another
// case that switched to English happened to run before it (on a Chinese Windows). A case that wants Chinese switches.
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "core/i18n.h"
#include "core/parallel.h"
#include "formula/datetime.h"

namespace {

class EnglishFirst : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;
  // ... and in the 1900 date system with every calculation thread: a 1904 test's date system, or a thread limit, is
  // the process's (one workbook per YCGrid), so it doesn't reach the next case
  void testCaseStarting(const Catch::TestCaseInfo&) override {
    yc::i18n::SetCurrent(yc::i18n::Lang::En);
    yc::formula::SetDate1904(false);
    yc::core::SetWorkerLimit(0);
  }
};

}  // namespace

CATCH_REGISTER_LISTENER(EnglishFirst)
