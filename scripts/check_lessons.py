# SPDX-License-Identifier: Apache-2.0
"""The lessons of AGENTS.md that a machine can check, checked: each rule below is one lesson ("Lessons from bugs"),
written as a pattern that the mistake leaves in the code. A lesson only written down is a thing everyone must remember;
here it fails the build checks (check_all, tests.test_core) the moment the mistake comes back.

A line that does what a rule forbids on purpose says why, on the line or the line above:
    // lesson-ok: <rule id> <why>
A new lesson that a pattern can catch gets its rule here.

    python scripts/check_lessons.py          check
    python scripts/check_lessons.py --list   the rules and the lessons they come from
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELF = os.path.join("scripts", "check_lessons.py")  # its own lessons quote the patterns
CPP = ["src/**/*.cpp", "src/**/*.h", "apps/**/*.cpp", "apps/**/*.h"]


class Rule:
    def __init__(self, rid, lesson, pattern, files=CPP, unless=None, skip_files=()):
        self.id, self.lesson = rid, lesson
        self.pattern = re.compile(pattern)
        self.files = files
        self.unless = re.compile(unless) if unless else None  # the line is fine when this matches it too
        self.skip_files = tuple(f.replace("/", os.sep) for f in skip_files)


RULES = [
    Rule("ref-has-cells",
         "A walk that moves, rebases, sorts or fills references asks RefSpec::HasCells, not !bad: a reference whose sheet "
         "was deleted (#REF!A1) still names cells and moves with its formula; skipped, it named other cells than its "
         "text after a row was deleted, and a save wrote another text (doc_fuzz seeds 88001420, 10460222).",
         r"if \(\w+\.bad\) (return|continue)",
         files=["src/engine/**/*.cpp", "src/formula/**/*.cpp"]),
    Rule("pending-clear",
         "The pending set is emptied with its indexes (pending_tiles_, new_pending_, pending_spills_) through "
         "Document::ClearPending only: Undo cleared it and left an anchor in pending_spills_, and every range over that "
         "spill waited for a cell nobody calculated (SUM read 0).",
         r"\bpending_(\.clear\(\)|\s*=\s*\{\})",
         files=["src/engine/**/*.cpp", "src/engine/**/*.h"]),
    Rule("utf8-cut",
         "Shortening text for the UI cuts at a UTF-8 character boundary: core::Utf8Prefix / core::Utf8First, never "
         "substr by bytes (half a Chinese character made every ui.items call fail to decode).",
         r"\.substr\(0,\s*[^)]*\)\s*\+\s*\"\.\.\.\""),
    Rule("write-source",
         "What a write does depends on its source through engine::PolicyFor only (the .xlsx reader wrote as Python "
         "and inherited the format inference meant for typing): no `src & kSrc...` tests of their own.",
         r"\bsrc\s*(&|==)\s*\(?\s*kSrc(User|Python|Live|Remote|File)\b",
         files=["src/engine/**/*.cpp", "src/io/**/*.cpp"]),
    Rule("find-push",
         "A list built from what a selection or a report holds keeps each entry once with a set beside it, never a "
         "find over the list so far: over whole columns that is a million squared steps (a chart took 25 s to insert, "
         "a PivotTable of 100,000 items with Show Values As never ended). A list that stays small says why with "
         "lesson-ok.",
         r"std::find\((\w+)\.begin\(\), \1\.end\(\), [^;]*\) == \1\.end\(\)\)?\s*\{?\s*\1\.(push_back|emplace_back)",
         files=["apps/**/*.cpp", "src/**/*.cpp"]),
    Rule("shell-open",
         "A hidden instance (tests, agents) opens nothing on the user's desktop: files, folders and programs open "
         "through OpenOnDesktop (settings.h), which does nothing there. A click on a screenshot's picture in chaos "
         "rounds opened the picture viewer over the user's screen again and again.",
         r"\bShellExecute(W|ExW)?\s*\(",
         files=["apps/**/*.cpp", "apps/**/*.h", "src/**/*.cpp"]),
    Rule("temp-folder",
         "The temp folder comes from TempFolder (settings.h), which never throws: std::filesystem::"
         "temp_directory_path() throws when %TMP% names a folder that isn't there, and a hidden instance asked it while "
         "starting, so YCGrid ended with abort() before its first frame (chaos seed 7609 replayed with a missing TMP).",
         r"\btemp_directory_path\s*\(",
         files=["apps/**/*.cpp", "apps/**/*.h", "src/**/*.cpp"]),
    Rule("dlg-tabs",
         "A dialog's tabs go through BeginDlgTabBar (dialogs.cpp), which gives them Excel's Ctrl+Tab, Ctrl+Shift+Tab, "
         "Ctrl+PgDn and Ctrl+PgUp: each dialog called ImGui::BeginTabBar itself and none of them took the keys.",
         r"\bImGui::BeginTabBar\s*\(",
         files=["apps/**/*.cpp"]),
    Rule("py-startfile",
         "Python opens files on the desktop through excel._open_on_desktop, which opens nothing for a script driving a "
         "hidden YCGrid (tests): os.startfile directly opened them on the user's screen.",
         r"\bstartfile\s*\(",
         files=["python/yc/**/*.py"]),
    Rule("ref-kinds",
         "Code that moves, renames, breaks or copies references walks model/ref_texts.h (EditObjectRefTexts, "
         "EditRuleRefTexts, EditWorkbookRefTexts), never the kinds one by one: five such lists each missed a kind.",
         r"\bForEach(Chart|Control|Sparkline|Solver)RefText\s*\(",
         unless=r"^\s*void ForEach",
         skip_files=("src/model/ref_texts.h",)),
    Rule("cow-order",
         "A copy-on-write table is read and changed in two statements: in `wb_.formulas->Share(InternFormula(x))` the "
         "`->` is read first, InternFormula then copies the table a snapshot shares, and the lookup finds nothing in "
         "the old one (a sorted formula was pasted as its value).",
         r"\b\w+\.\w+->\w+\([^;]*\bIntern\w*\("),
    Rule("ribbon-hook",
         "The ribbon's app hooks (menu_picture, menu_entry_height, tile_menu, submenu_body) are chained, never "
         "assigned over: [prev = St().tile_menu] (Cell Styles' tile menu silently replaced Format as Table's).",
         r"St\(\)\.(menu_picture|menu_entry_height|tile_menu|submenu_body)\s*=(?!=)",
         unless=r"\bprev\b"),
    Rule("hover-popups",
         "IsWindowHovered(ImGuiHoveredFlags_ChildWindows) is also true over a popup opened from the window: add "
         "ImGuiHoveredFlags_NoPopupHierarchy where the window acts on the mouse (the sheet's wheel scrolled under a flyout).",
         r"IsWindowHovered\([^)]*ImGuiHoveredFlags_ChildWindows",
         unless=r"NoPopupHierarchy"),
    Rule("double-click",
         "A ribbon button runs on the mouse release: a double-click is IsMouseReleased(0) && "
         "io.MouseClickedLastCount[0] >= 2 (IsMouseDoubleClicked is only true on the second press).",
         r"\bIsMouseDoubleClicked\b", files=["src/ui/ribbon/**/*.cpp"]),
    Rule("tr-hash",
         "TR(\"text##id\") cuts the text at ##: no ## in UI text (number format examples like #,##0).",
         r"\bTRW?\(L?\"[^\"]*#,##"),
    Rule("view-temporary",
         "A reader that keeps a view (MsgReader) is never built from a temporary string: it read freed memory and "
         "passed by luck for months.",
         r"\bMsgReader\s+\w+\s*\(\s*std::string\s*\(", files=CPP + ["tests/**/*.cpp"]),
    Rule("view-of-temporary",
         "A string_view is never set from a function that returns a std::string (ToText, Lower, to_string...): it views "
         "a temporary freed at the end of the statement (a list validation read freed memory).",
         r"\bstd::string_view\s+\w+\s*=\s*[^;]*\b(ToText|Lower|FoldCase|to_string|DisplayTextOf|GetInputText|GetFormulaA1)\(",
         files=CPP + ["tests/**/*.cpp"]),
    Rule("fixed-list",
         "No fixed-size list for things the UI adds to (the title bar's clickable areas were RECT[8] and pushed the "
         "X off the end).",
         r"\bRECT\s+\w+\s*\[\s*\d+\s*\]"),
    Rule("cmd-errorlevel",
         "Errors in .cmd files are checked with if not \"!errorlevel!\"==\"0\" (linkers can return -1, which "
         "`if errorlevel 1` takes for success).",
         r"(?i)^\s*if\s+(not\s+)?errorlevel\b|%errorlevel%", files=["scripts/*.cmd", "*.cmd"]),
    Rule("ps-null",
         "A probe releases COM objects with if ($null -ne $o): $o -ne $null on a Range makes PowerShell walk every cell.",
         r"-ne\s+\$null", files=["scripts/**/*.py", "scripts/**/*.ps1"]),
]


def files_for(rule):
    seen = set()
    for pat in rule.files:
        for p in glob.glob(os.path.join(ROOT, pat), recursive=True):
            rel = os.path.relpath(p, ROOT)
            if rel in seen or rel.startswith(("third_party", "build", "dist_")) or rel.endswith(rule.skip_files + (SELF,)):
                continue
            seen.add(rel)
            yield rel


KEY_ONCE = ("key-once",
            "A shortcut is handled in one place: Ctrl+F1 was switched by the ribbon's key check and again by main.cpp's "
            "in the same frame, so the ribbon never collapsed. A key chord (IsKeyChordPressed of ImGuiMod / ImGuiKey "
            "constants, or a shortcut table entry {chord, \"Command\"}) is either a global key (main.cpp) or a widget's "
            "own key (src/ui), never both.")
_CHORD_CALL = re.compile(r"IsKeyChordPressed\(\s*((?:ImGuiMod_\w+\s*\|\s*)*ImGuiKey_\w+)\s*[,)]")
_CHORD_ENTRY = re.compile(r"\{\s*((?:ImGuiMod_\w+\s*\|\s*)*ImGuiKey_\w+)\s*,\s*\"")


def keys_in_two_files() -> list:
    """The KEY_ONCE lesson: a chord found in more than one file (normalized: its parts sorted)."""
    where: dict = {}
    # the global keys (main.cpp's handler and its shortcut tables) against the keys the UI's own widgets take (src/ui:
    # the ribbon, the title bar...); the context routers (ObjectShortcuts, the PivotTable's keys) are one per context
    for pat in ("apps/ycgrid/main.cpp", "src/ui/**/*.cpp"):
        for p in glob.glob(os.path.join(ROOT, pat), recursive=True):
            rel = os.path.relpath(p, ROOT)
            if rel.startswith(("third_party", "build", "dist_")):
                continue
            try:
                lines = open(p, encoding="utf-8").read().splitlines()
            except (UnicodeDecodeError, OSError):
                continue
            for i, line in enumerate(lines):
                if re.search(r"lesson-ok:\s*key-once\b", line) or (i and re.search(r"lesson-ok:\s*key-once\b", lines[i - 1])):
                    continue
                for m in list(_CHORD_CALL.finditer(line)) + list(_CHORD_ENTRY.finditer(line)):
                    chord = "|".join(sorted(x.strip() for x in m.group(1).split("|")))
                    where.setdefault(chord, {}).setdefault(rel, i + 1)
    return [f"{rel}:{ln}: [{KEY_ONCE[0]}] {chord} is also handled in "
            f"{', '.join(r for r in files if r != rel)}. {KEY_ONCE[1]}"
            for chord, files in sorted(where.items()) if len(files) > 1 for rel, ln in files.items()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for r in RULES:
            print(f"{r.id}: {r.lesson}")
        print(f"{KEY_ONCE[0]}: {KEY_ONCE[1]}")
        return 0
    problems = keys_in_two_files()
    for rule in RULES:
        ok_mark = re.compile(r"lesson-ok:\s*" + re.escape(rule.id) + r"\b")
        for rel in files_for(rule):
            try:
                with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
                    lines = f.read().splitlines()
            except (UnicodeDecodeError, OSError):
                continue
            for i, line in enumerate(lines):
                if not rule.pattern.search(line) or (rule.unless and rule.unless.search(line)):
                    continue
                if ok_mark.search(line) or (i and ok_mark.search(lines[i - 1])):
                    continue
                problems.append(f"{rel}:{i + 1}: [{rule.id}] {rule.lesson}")
    for p in problems:
        print(p)
    print(f"check_lessons: {len(problems)} problem(s)" if problems else f"check_lessons: {len(RULES)} rules, no problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
