// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/py_source.h"

#include <string>

using yc::core::EnsurePythonHandler;
using yc::core::IsPythonName;

TEST_CASE("View Code on a control: a handler not written yet is appended, two blank lines after the code") {
  std::string t = "# Sheet1 (Orders): this sheet's events\n";
  const auto at = EnsurePythonHandler(t, "CommandButton1_Click");
  CHECK(at.added);
  CHECK(t == "# Sheet1 (Orders): this sheet's events\n\n\ndef CommandButton1_Click():\n    pass\n");
  CHECK(at.line == 5);  // the `pass`, where the caret goes
  CHECK(at.column == 5);
}

TEST_CASE("View Code on a control: a handler already there is found, not added again") {
  std::string t = "import yc\n\n\ndef CommandButton1_Click():\n    yc.excel.Range('A1').Value = 1\n";
  const std::string before = t;
  const auto at = EnsurePythonHandler(t, "CommandButton1_Click");
  CHECK_FALSE(at.added);
  CHECK(t == before);
  CHECK(at.line == 5);
  // the same once it was added: opening it twice makes one handler
  std::string u = "# s\n";
  EnsurePythonHandler(u, "ComboBox1_Change");
  const std::string once = u;
  const auto again = EnsurePythonHandler(u, "ComboBox1_Change");
  CHECK_FALSE(again.added);
  CHECK(u == once);
  CHECK(again.line == 5);
}

TEST_CASE("View Code on a control: only a definition of that name counts") {
  std::string t =
      "def CommandButton10_Click():\n    pass\n"
      "# def CommandButton1_Click(): in a comment\n"
      "    def CommandButton1_Click():  # nested, not the module's\n        pass\n"
      "x = CommandButton1_Click\n";
  const auto at = EnsurePythonHandler(t, "CommandButton1_Click");
  CHECK(at.added);
  CHECK(at.line == 10);
  // spaces between the parts are Python's too
  std::string u = "def  CommandButton1_Click ( ):\n    pass\n";
  CHECK_FALSE(EnsurePythonHandler(u, "CommandButton1_Click").added);
}

TEST_CASE("View Code on a control: the module's line ends, a last line without one and blank lines already there") {
  std::string crlf = "# s\r\nimport yc\r\n";
  EnsurePythonHandler(crlf, "B_Click");
  CHECK(crlf == "# s\r\nimport yc\r\n\r\n\r\ndef B_Click():\r\n    pass\r\n");
  std::string open_end = "x = 1";
  const auto at = EnsurePythonHandler(open_end, "B_Click");
  CHECK(open_end == "x = 1\n\n\ndef B_Click():\n    pass\n");
  CHECK(at.line == 5);
  std::string spaced = "x = 1\n\n";
  EnsurePythonHandler(spaced, "B_Click");
  CHECK(spaced == "x = 1\n\n\ndef B_Click():\n    pass\n");
  std::string empty;
  const auto first = EnsurePythonHandler(empty, "B_Click");
  CHECK(empty == "def B_Click():\n    pass\n");
  CHECK(first.line == 2);
  // a byte order mark stays first and doesn't hide a handler on the first line
  std::string bom = "\xEF\xBB\xBF" "def B_Click():\n    pass\n";
  const auto found = EnsurePythonHandler(bom, "B_Click");
  CHECK_FALSE(found.added);
  CHECK(found.line == 2);
  // a handler on the last line, without a body line after it
  std::string last = "def B_Click(): pass";
  CHECK(EnsurePythonHandler(last, "B_Click").line == 1);
}

TEST_CASE("View Code on a control: names that can't be a Python function aren't written") {
  CHECK(IsPythonName("CommandButton1_Click"));
  CHECK(IsPythonName("_x"));
  CHECK(IsPythonName("\xE6\x8C\x89\xE9\x88\x95" "1_Click"));  // a Chinese name: Python takes it
  CHECK_FALSE(IsPythonName(""));
  CHECK_FALSE(IsPythonName("1Button_Click"));
  CHECK_FALSE(IsPythonName("My Button_Click"));
  CHECK_FALSE(IsPythonName("a-b_Click"));
}
