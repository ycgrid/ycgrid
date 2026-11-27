// SPDX-License-Identifier: Apache-2.0
#include "formula/parser.h"
#include "core/i18n.h"

#include "formula/functions.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace yc::formula {
namespace {

using core::CellPos;
using core::Err;

enum class TT { End, Num, Str, Err, Ident, Sheet, Op, LParen, RParen, Comma, Semi, LBrace, RBrace, Bracket };

struct Tok {
  TT t = TT::End;
  std::string text;
  double num = 0;
  Err err = Err::None;
  size_t pos = 0;
  bool spill = false;  // identifier followed by # (spill range reference A1#)
};

bool IsIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '\\' || c == '$' || c >= 0x80; }
bool IsIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '$' || c == '\\' || c == '?' || c >= 0x80; }

std::string Upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  }
  return o;
}

// an R1C1 reference at s[i] (R, R2, R[-1], C, C3, C[1], RC, R1C1, R[1]C[-2]...); its end, or 0
size_t MatchR1C1(std::string_view s, size_t i) {
  size_t k = i;
  auto part = [&](char letter) {
    if (k >= s.size() || (s[k] != letter && s[k] != letter + 32)) return false;
    ++k;
    if (k < s.size() && s[k] == '[') {
      size_t m = k + 1;
      if (m < s.size() && (s[m] == '-' || s[m] == '+')) ++m;
      const size_t d0 = m;
      while (m < s.size() && std::isdigit(static_cast<unsigned char>(s[m]))) ++m;
      if (m == d0 || m >= s.size() || s[m] != ']') return false;
      k = m + 1;
    } else {
      while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
    }
    return true;
  };
  const size_t start = k;
  const bool has_r = part('R');
  if (!has_r) k = start;
  const size_t mid = k;
  if (!part('C')) k = mid;
  if (k == i) return 0;
  if (k < s.size() && (IsIdentChar(static_cast<unsigned char>(s[k])) || s[k] == '(' || s[k] == '[')) return 0;
  return k;
}

class Lexer {
 public:
  explicit Lexer(std::string_view s, bool r1c1 = false) : s_(s), r1c1_(r1c1) {}

  bool Run(std::vector<Tok>* out, std::string* err, size_t* err_pos) {
    while (true) {
      const size_t ws = i_;
      while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
      // a space between two references is Excel's intersection operator: =SUM(A1:C3 B2:D4)
      if (i_ > ws && s_[ws] == ' ' && i_ < s_.size() && !out->empty()) {
        const Tok& prev = out->back();
        const unsigned char nc = static_cast<unsigned char>(s_[i_]);
        // a row range (2:8) ends or starts a reference too: =SUM(B6:F8 2:8), =SUM(2:3 C:D)
        const size_t m = out->size();
        const bool after_rows = prev.t == TT::Num && m >= 3 && (*out)[m - 2].t == TT::Op && (*out)[m - 2].text == ":" &&
                                ((*out)[m - 3].t == TT::Num || (*out)[m - 3].t == TT::Ident);
        auto rows_ahead = [&](size_t j) {
          if (j < s_.size() && s_[j] == '$') ++j;
          const size_t d = j;
          while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
          if (j == d || j >= s_.size() || s_[j] != ':') return false;
          ++j;
          if (j < s_.size() && s_[j] == '$') ++j;
          const size_t d2 = j;
          while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
          return j > d2 && (j >= s_.size() || !IsIdentChar(static_cast<unsigned char>(s_[j])));
        };
        const bool ends_ref = prev.t == TT::Ident || prev.t == TT::RParen || prev.t == TT::Bracket || after_rows;
        const bool starts_ref = IsIdentStart(nc) || nc == '\'' || (nc == '(' && prev.t != TT::Ident) || rows_ahead(i_);  // SUM (A1) stays a call
        if (ends_ref && starts_ref) {
          Tok op;
          op.t = TT::Op;
          op.text = " ";
          op.pos = ws;
          out->push_back(std::move(op));
        }
      }
      Tok t;
      t.pos = i_;
      if (i_ >= s_.size()) {
        t.t = TT::End;
        out->push_back(t);
        return true;
      }
      const unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '[') {
        // structured reference specifier: the text between the outer brackets
        int depth = 0;
        size_t j = i_;
        for (; j < s_.size(); ++j) {
          if (s_[j] == '\'' && j + 1 < s_.size()) {
            ++j;  // escaped character
            continue;
          }
          if (s_[j] == '[') ++depth;
          else if (s_[j] == ']' && --depth == 0) break;
        }
        if (j >= s_.size()) return Fail(TR("A table reference is missing its closing bracket"), t.pos, err, err_pos);
        // [Book2.ycg]Sheet1!A1: another workbook's sheet, not a table reference
        size_t k = j + 1;
        while (k < s_.size() && IsIdentChar(static_cast<unsigned char>(s_[k]))) ++k;
        if (k > j + 1 && k < s_.size() && s_[k] == '!') {
          t.t = TT::Sheet;
          t.text = std::string(s_.substr(i_, k - i_));
          i_ = k + 1;
          out->push_back(std::move(t));
          continue;
        }
        t.t = TT::Bracket;
        t.text = std::string(s_.substr(i_ + 1, j - i_ - 1));
        i_ = j + 1;
        out->push_back(std::move(t));
        continue;
      }
      if (c == '"') {
        ++i_;
        std::string v;
        bool closed = false;
        while (i_ < s_.size()) {
          if (s_[i_] == '"') {
            if (i_ + 1 < s_.size() && s_[i_ + 1] == '"') {
              v += '"';
              i_ += 2;
              continue;
            }
            ++i_;
            closed = true;
            break;
          }
          v += s_[i_++];
        }
        if (!closed) return Fail(TR("The text is missing its closing quote"), t.pos, err, err_pos);
        t.t = TT::Str;
        t.text = v;
      } else if (c == '\'') {
        ++i_;
        std::string v;
        bool closed = false;
        while (i_ < s_.size()) {
          if (s_[i_] == '\'') {
            if (i_ + 1 < s_.size() && s_[i_ + 1] == '\'') {
              v += '\'';
              i_ += 2;
              continue;
            }
            ++i_;
            closed = true;
            break;
          }
          v += s_[i_++];
        }
        if (!closed || i_ >= s_.size() || s_[i_] != '!') return Fail(TR("The sheet name's quotes or exclamation mark are incomplete"), t.pos, err, err_pos);
        ++i_;
        t.t = TT::Sheet;
        t.text = v;
      } else if (c == '#') {
        size_t j = i_ + 1;
        while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '/' || s_[j] == '_')) ++j;
        if (j < s_.size() && (s_[j] == '!' || s_[j] == '?')) ++j;
        Err e;
        if (!core::ParseErrorText(s_.substr(i_, j - i_), &e)) return Fail(TR("Unrecognized error value"), t.pos, err, err_pos);
        t.t = TT::Err;
        t.err = e;
        i_ = j;
        // #REF!#: a spill reference whose anchor was deleted, as Excel writes it (=SUM(#REF!#))
        if (e == Err::Ref && i_ < s_.size() && s_[i_] == '#' && (i_ + 1 >= s_.size() || !std::isalnum(static_cast<unsigned char>(s_[i_ + 1])))) {
          t.spill = true;
          ++i_;
        }
      } else if (std::isdigit(c) || (c == '.' && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
        size_t j = i_;
        while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
        // a letter right after a number (e.g. "1A") isn't a valid number; but "1:1" is a row reference, left to the parser
        if (j < s_.size() && s_[j] == '.') {
          ++j;
          while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
        }
        if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
          size_t k = j + 1;
          if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
          if (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) {
            j = k;
            while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
          }
        }
        t.t = TT::Num;
        t.text = std::string(s_.substr(i_, j - i_));
        {
          // Excel keeps 15 significant digits of a number in a formula and cuts the rest to 0 (=1234567890123456&""
          // is 1234567890123450), as it does for typed numbers
          std::string digits = t.text;
          int kept = 0;
          for (char& ch : digits) {
            if (ch == 'e' || ch == 'E') break;
            if (ch < '0' || ch > '9') continue;
            if (kept >= 15) ch = '0';
            else if (kept > 0 || ch != '0') ++kept;
          }
          t.num = std::strtod(digits.c_str(), nullptr);
        }
        i_ = j;
      } else if (size_t rc_end = r1c1_ && (c == 'R' || c == 'r' || c == 'C' || c == 'c') ? MatchR1C1(s_, i_) : 0; rc_end) {
        t.t = TT::Ident;
        t.text = std::string(s_.substr(i_, rc_end - i_));
        i_ = rc_end;
        if (i_ < s_.size() && s_[i_] == '#' && (i_ + 1 >= s_.size() || !std::isalnum(static_cast<unsigned char>(s_[i_ + 1])))) {
          t.spill = true;
          ++i_;
        }
      } else if (IsIdentStart(c)) {
        size_t j = i_ + 1;
        while (j < s_.size() && IsIdentChar(static_cast<unsigned char>(s_[j]))) ++j;
        t.text = std::string(s_.substr(i_, j - i_));
        i_ = j;
        if (i_ < s_.size() && s_[i_] == '!') {
          ++i_;
          t.t = TT::Sheet;
        } else {
          t.t = TT::Ident;
          // A1# (but not the start of an error value such as A1#N/A, which isn't valid anyway)
          if (i_ < s_.size() && s_[i_] == '#' && (i_ + 1 >= s_.size() || !std::isalnum(static_cast<unsigned char>(s_[i_ + 1])))) {
            t.spill = true;
            ++i_;
          }
        }
      } else {
        ++i_;
        switch (c) {
          case '(': t.t = TT::LParen; break;
          case ')': t.t = TT::RParen; break;
          case ',': t.t = TT::Comma; break;
          case ';': t.t = TT::Semi; break;
          case '{': t.t = TT::LBrace; break;
          case '}': t.t = TT::RBrace; break;
          case '@':  // implicit intersection operator
            t.t = TT::Op;
            t.text = "@";
            break;
          case '<':
            t.t = TT::Op;
            if (i_ < s_.size() && s_[i_] == '=') {
              t.text = "<=";
              ++i_;
            } else if (i_ < s_.size() && s_[i_] == '>') {
              t.text = "<>";
              ++i_;
            } else {
              t.text = "<";
            }
            break;
          case '>':
            t.t = TT::Op;
            if (i_ < s_.size() && s_[i_] == '=') {
              t.text = ">=";
              ++i_;
            } else {
              t.text = ">";
            }
            break;
          case '+': case '-': case '*': case '/': case '^': case '&': case '=': case '%': case ':':
            t.t = TT::Op;
            t.text = std::string(1, static_cast<char>(c));
            break;
          default:
            return Fail(TR("Unrecognized character"), t.pos, err, err_pos);
        }
      }
      out->push_back(std::move(t));
    }
  }

 private:
  static bool Fail(const char* msg, size_t pos, std::string* err, size_t* err_pos) {
    *err = msg;
    *err_pos = pos;
    return false;
  }
  std::string_view s_;
  bool r1c1_ = false;
  size_t i_ = 0;
};

// ---- Classifying reference text ----
struct CellText {
  bool abs_col = false, abs_row = false;
  uint32_t col = 0, row = 0;
};

bool ParseCellText(std::string_view s, CellText* out) {
  size_t i = 0;
  CellText c;
  if (i < s.size() && s[i] == '$') {
    c.abs_col = true;
    ++i;
  }
  const size_t cs = i;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
  if (i == cs || i - cs > 3) return false;
  if (!core::ParseColumnName(s.substr(cs, i - cs), &c.col) || c.col >= core::kMaxCols) return false;
  if (i < s.size() && s[i] == '$') {
    c.abs_row = true;
    ++i;
  }
  const size_t rs = i;
  uint64_t r = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    r = r * 10 + static_cast<uint64_t>(s[i] - '0');
    if (r > core::kMaxRows) return false;
    ++i;
  }
  if (i == rs || i != s.size() || r == 0) return false;
  c.row = static_cast<uint32_t>(r - 1);
  *out = c;
  return true;
}

bool ParseColText(std::string_view s, bool* abs, uint32_t* col) {
  *abs = false;
  if (!s.empty() && s[0] == '$') {
    *abs = true;
    s.remove_prefix(1);
  }
  if (s.empty() || s.size() > 3) return false;
  for (char ch : s) {
    if (!std::isalpha(static_cast<unsigned char>(ch))) return false;
  }
  return core::ParseColumnName(s, col) && *col < core::kMaxCols;
}

bool ParseRowText(std::string_view s, bool* abs, uint32_t* row) {
  *abs = false;
  if (!s.empty() && s[0] == '$') {
    *abs = true;
    s.remove_prefix(1);
  }
  if (s.empty()) return false;
  uint64_t r = 0;
  for (char ch : s) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    r = r * 10 + static_cast<uint64_t>(ch - '0');
    if (r > core::kMaxRows) return false;
  }
  if (r == 0) return false;
  *row = static_cast<uint32_t>(r - 1);
  return true;
}

// a name Excel's A1 formulas refuse because it reads as the start of an R1C1 reference (measured): R, C or RC alone,
// or R, C or RC followed by a row or column number on the sheet (R2D2, C3PO, R1x, R1C1, RC1x); R0x, C16385x, Rx
// and RCx are names
bool R1C1LikeName(std::string_view s) {
  auto up = [&](size_t i) { return i < s.size() ? static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))) : '\0'; };
  const char first = up(0);
  if (first != 'R' && first != 'C') return false;
  if (s.size() == 1 || (s.size() == 2 && first == 'R' && up(1) == 'C')) return true;
  size_t i = 1;
  uint64_t limit = first == 'R' ? uint64_t{core::kMaxRows} : uint64_t{core::kMaxCols};
  if (first == 'R' && up(1) == 'C') {
    i = 2;
    limit = core::kMaxCols;
  }
  uint64_t n = 0;
  size_t j = i;
  for (; j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])); ++j) {
    if (n <= limit) n = n * 10 + static_cast<uint64_t>(s[j] - '0');
  }
  return j > i && n >= 1 && n <= limit;
}

RefCoord Coord(uint32_t v, bool abs, uint32_t origin) {
  RefCoord c;
  c.abs = abs;
  c.v = abs ? int64_t{v} : int64_t{v} - int64_t{origin};
  return c;
}

class Parser {
 public:
  Parser(std::vector<Tok> toks, CellPos origin, const SheetResolver* sheets, bool r1c1 = false, bool keep_order = false)
      : t_(std::move(toks)), origin_(origin), sheets_(sheets), r1c1_(r1c1), keep_order_(keep_order) {}

  bool Run(Node* out, std::string* err, size_t* err_pos) {
    try {
      *out = Expr(0);
      if (Peek().t != TT::End) Throw(TR("There's extra content after the formula"));
      return true;
    } catch (const Failure& f) {
      *err = f.msg;
      *err_pos = f.pos;
      return false;
    }
  }

 private:
  struct Failure {
    std::string msg;
    size_t pos;
  };
  [[noreturn]] void Throw(const char* msg) { throw Failure{msg, Peek().pos}; }

  static constexpr int kMaxDepth = 128;  // Excel allows 64 nested functions; each level here is a few KB of stack
  int depth_ = 0;
  const Tok& Peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
  Tok Next() { return t_[std::min(p_++, t_.size() - 1)]; }
  bool IsOp(const char* op, size_t k = 0) const { return Peek(k).t == TT::Op && Peek(k).text == op; }

  static int InfixBp(const Tok& t) {
    if (t.t != TT::Op) return -1;
    const std::string& o = t.text;
    if (o == ":") return 80;
    if (o == " ") return 70;  // intersection (Excel: range, then intersection, then union)
    if (o == "%") return 60;
    if (o == "^") return 50;
    if (o == "*" || o == "/") return 40;
    if (o == "+" || o == "-") return 30;
    if (o == "&") return 20;
    if (o == "=" || o == "<>" || o == "<" || o == ">" || o == "<=" || o == ">=") return 10;
    return -1;
  }
  static char OpCode(const std::string& o) {
    if (o == "<>") return 'N';
    if (o == "<=") return 'L';
    if (o == ">=") return 'G';
    return o[0];
  }

  Node Expr(int min_bp) {
    // every nesting level (parentheses, unary signs, function arguments) passes through here: a cap keeps a formula
    // like =((((...)))) ten thousand levels deep from overflowing the stack (Excel's limit is 64)
    struct Depth {
      int& d;
      explicit Depth(int& v) : d(v) { ++d; }
      ~Depth() { --d; }
    } depth_guard(depth_);
    if (depth_ > kMaxDepth) Throw(TR("The formula is nested too deeply"));
    Node lhs = Prefix();
    while (true) {
      const Tok& t = Peek();
      const int bp = InfixBp(t);
      if (bp < 0 || bp <= min_bp) break;  // all left-associative (Excel's ^ is left-associative too: 2^3^2 = 64)
      Next();
      if (t.text == "%") {
        Node n;
        n.kind = NK::Percent;
        n.kids.push_back(std::move(lhs));
        lhs = std::move(n);
        continue;
      }
      const std::string op = t.text;
      Node rhs = Expr(bp);  // left-associative: the right side uses the same precedence (continue only when strictly greater)
      if (op == ":" && r1c1_ && lhs.kind == NK::Ref && rhs.kind == NK::Ref && (rhs.ref.sheet == -1 || rhs.ref.sheet == lhs.ref.sheet) &&
          ((lhs.ref.whole_rows && rhs.ref.whole_rows) || (lhs.ref.whole_cols && rhs.ref.whole_cols))) {
        // R1:R3, C[-1]:C[1]
        Node n = std::move(lhs);
        if (n.ref.whole_rows) n.ref.r1 = rhs.ref.r1;
        else n.ref.c1 = rhs.ref.c1;
        lhs = std::move(n);
        continue;
      }
      if (op == ":" && lhs.kind == NK::Ref && rhs.kind == NK::Ref && !lhs.ref.is_range && !rhs.ref.is_range &&
          (rhs.ref.sheet == -1 || rhs.ref.sheet == lhs.ref.sheet)) {
        // A1:B2 -> one range reference
        Node n = std::move(lhs);
        n.ref.is_range = true;
        n.ref.r1 = rhs.ref.r0;
        n.ref.c1 = rhs.ref.c0;
        if (!r1c1_) Normalize(n.ref);
        lhs = std::move(n);
        continue;
      }
      if (op == ":" && lhs.kind == NK::Paren && lhs.kids.size() == 1) {
        // (A):Data!E13: the writer puts a bare name or cell before a sheet's reference in parentheses (see Write), so
        // they are its, not the formula's: kept, the formula read back from its text was another formula than the one
        // written, one text two formulas until a save and an open (doc_fuzz --xlsx --more seed 35490977)
        const Node& in = lhs.kids[0];
        const bool bare = (in.kind == NK::Name && in.ref.sheet < 0 && !in.b) ||
                          (in.kind == NK::Ref && in.ref.sheet < 0 && in.ref.sheet2 < 0 && !in.ref.bad && !in.ref.lost_sheet);
        if (bare) {
          const std::string right = ToA1(rhs, origin_, sheets_);
          if (!right.empty() && right[0] != '(' && right.find('!') != std::string::npos) {
            Node inner = std::move(lhs.kids[0]);
            lhs = std::move(inner);
          }
        }
      }
      Node n;
      n.kind = NK::Binary;
      n.op = OpCode(op);
      n.kids.push_back(std::move(lhs));
      n.kids.push_back(std::move(rhs));
      lhs = std::move(n);
    }
    return lhs;
  }

  Node Prefix() {
    Tok t = Next();
    switch (t.t) {
      case TT::Num: {
        // 1:3 -> entire-row range (1:$3 too: a $ row reads as a name)
        if (IsOp(":") && (Peek(1).t == TT::Num || Peek(1).t == TT::Ident)) {
          bool a1, a2;
          uint32_t r1, r2;
          if (ParseRowText(t.text, &a1, &r1) && ParseRowText(Peek(1).text, &a2, &r2)) {
            Next();
            Next();
            return RowRange(-1, a1, r1, a2, r2);
          }
        }
        Node n;
        n.kind = NK::Num;
        n.num = t.num;
        return n;
      }
      case TT::Str: {
        Node n;
        n.kind = NK::Str;
        n.str = t.text;
        return n;
      }
      case TT::Err: {
        if (t.spill) {
          // #REF!#: a spill reference whose anchor cell was deleted
          Node ref;
          ref.kind = NK::Ref;
          ref.ref.bad = true;
          ref.ref.spill = true;
          return ref;
        }
        // #REF!A1, #REF!$A$1:$B$2: a reference whose sheet was deleted, as Excel writes it (and saved files keep it)
        if (t.err == Err::Ref && Peek().pos == t.pos + 5 && (Peek().t == TT::Ident || Peek().t == TT::Num)) {
          Node ref = SheetPrefixed(-2, false, -1);
          ref.ref.lost_sheet = true;
          return ref;
        }
        Node n;
        n.kind = NK::Err;
        n.err = t.err;
        return n;
      }
      case TT::Op:
        if (t.text == "-" || t.text == "+" || t.text == "@") {
          Node n;
          n.kind = NK::Unary;
          n.op = t.text[0];
          n.kids.push_back(Expr(70));  // unary minus binds tighter than ^ (-2^2 = 4); @ takes a whole A1:B5
          return n;
        }
        throw Failure{TR("An operator can't appear here: ") + t.text, t.pos};
      case TT::LParen: {
        Node inner = Expr(0);
        // inside parentheses a comma is the union operator: =SUM((A1:B2,D4)), =AREAS((A1,B2,C3))
        while (Peek().t == TT::Comma) {
          Next();
          Node u;
          u.kind = NK::Binary;
          u.op = ',';
          u.kids.push_back(std::move(inner));
          u.kids.push_back(Expr(0));
          inner = std::move(u);
        }
        if (Next().t != TT::RParen) throw Failure{TR("Missing a right parenthesis"), t.pos};
        Node n;
        n.kind = NK::Paren;
        n.kids.push_back(std::move(inner));
        return n;
      }
      case TT::LBrace:
        return ArrayConst(t.pos);
      case TT::Sheet: {
        const int32_t sid = sheets_ ? sheets_->FindSheet(t.text) : -1;
        // 'Sheet 1:Sheet 3'!A1 (3D reference in quotes)
        const size_t colon = t.text.find(':');
        if (sid < 0 && colon != std::string::npos && sheets_) {
          return SheetPrefixed(sheets_->FindSheet(t.text.substr(0, colon)), true, sheets_->FindSheet(t.text.substr(colon + 1)));
        }
        return SheetPrefixed(sid, false, -1);
      }
      case TT::Bracket:
        return StructRef(std::string(), t);
      case TT::Ident: {
        if (Peek().t == TT::Bracket) {
          const Tok b = Next();
          return StructRef(t.text, b);
        }
        CellText probe;
        if (IsOp(":") && Peek(1).t == TT::Sheet && !ParseCellText(t.text, &probe)) {
          // Sheet1:Sheet3!A1 (3D reference)
          Next();
          const Tok last = Next();
          return SheetPrefixed(sheets_ ? sheets_->FindSheet(t.text) : -1, true, sheets_ ? sheets_->FindSheet(last.text) : -1);
        }
        if (Peek().t == TT::LParen) {
          Node f = Call(t);
          while (Peek().t == TT::LParen) f = CallOn(std::move(f));  // LAMBDA(x,x+1)(5)
          return f;
        }
        const std::string up = Upper(t.text);
        if (up == "TRUE" || up == "FALSE") {
          Node n;
          n.kind = NK::Bool;
          n.b = up == "TRUE";
          return n;
        }
        Node ref;
        if (IdentRef(t.text, -1, &ref)) {
          if (t.spill) MakeSpill(&ref, t.pos);
          return ref;
        }
        if (t.spill) throw Failure{TR("Only a single cell can be followed by # (spill range)"), t.pos};
        Node n;
        n.kind = NK::Name;
        n.str = t.text;
        return n;
      }
      default:
        throw Failure{TR("The formula is incomplete"), t.pos};
    }
  }

  // the reference after Sheet! (or Sheet1:Sheet3! when three_d)
  Node SheetPrefixed(int32_t sid, bool three_d, int32_t sid2) {
    const Tok& r = Peek();
    Node ref;
    if (r.t == TT::Err && r.err == Err::Ref) {
      // Data!#REF!: a deleted cell on another sheet, as Excel writes it (the sheet's name stays)
      Next();
      ref.kind = NK::Ref;
      ref.ref.sheet = sid < 0 ? -2 : sid;
      if (three_d) ref.ref.sheet2 = sid2 < 0 ? -2 : sid2;
      ref.ref.bad = true;
      ref.ref.spill = r.spill;  // Data!#REF!#
      return ref;
    }
    if (r.t == TT::Ident) {
      const Tok id = Next();
      // Data!Table1[Qty]: a table's name is the workbook's, so Excel takes the sheet and drops it (=Table1[Qty])
      if (!three_d && Peek().t == TT::Bracket) return StructRef(id.text, Next());
      if (!IdentRef(id.text, sid < 0 ? -2 : sid, &ref)) {
        // Sheet1!Name: a name defined for that sheet (Excel's sheet-level names)
        if (sid >= 0 && !three_d && !id.spill && Peek().t != TT::LParen) {
          Node n;
          n.kind = NK::Name;
          n.str = id.text;
          n.ref.sheet = sid;
          return n;
        }
        throw Failure{TR("A sheet name must be followed by a cell reference"), id.pos};
      }
      if (id.spill) MakeSpill(&ref, id.pos);
    } else if (r.t == TT::Num) {
      Next();
      bool a1, a2;
      uint32_t r1, r2;
      if (!(IsOp(":") && (Peek(1).t == TT::Num || Peek(1).t == TT::Ident) && ParseRowText(r.text, &a1, &r1) &&
            ParseRowText(Peek(1).text, &a2, &r2))) {
        throw Failure{TR("A sheet name must be followed by a cell reference"), r.pos};
      }
      Next();
      Next();
      ref = RowRange(sid < 0 ? -2 : sid, a1, r1, a2, r2);
    } else {
      throw Failure{TR("A sheet name must be followed by a cell reference"), r.pos};
    }
    if (three_d) ref.ref.sheet2 = sid2 < 0 ? -2 : sid2;
    if (sid < 0 || (three_d && sid2 < 0)) ref.ref.bad = true;  // the sheet doesn't exist -> #REF!
    return ref;
  }

  void MakeSpill(Node* ref, size_t pos) {
    if (ref->kind != NK::Ref || ref->ref.is_range) throw Failure{TR("Only a single cell can be followed by # (spill range)"), pos};
    ref->ref.spill = true;
  }

  // a range typed with its corners the other way round (E7:B7, $C5:F$3, G:C, 9:2) is kept from its upper-left to its
  // lower-right corner, each row and column keeping its $ (Excel 16, measured: B7:E7, $C$3:F5, C:G, 2:9)
  void Normalize(RefSpec& r) const {
    if (!r.is_range) return;
    auto at = [](const RefCoord& c, uint32_t origin) { return c.abs ? c.v : int64_t{origin} + c.v; };
    if (!keep_order_ && !r.whole_cols && at(r.r0, origin_.row) > at(r.r1, origin_.row)) std::swap(r.r0, r.r1);
    if (!keep_order_ && !r.whole_rows && at(r.c0, origin_.col) > at(r.c1, origin_.col)) std::swap(r.c0, r.c1);
    // a range across the sheet's whole width is those rows, as Excel writes it back (measured: =SUM(A1:XFD2) is
    // =SUM(1:2), A$1:XFD$2 is $1:$2, A1:XFD1048576 is 1:1048576); its columns can't move out of the sheet then. (A
    // range down 1,048,576 rows stays as typed: a YCGrid sheet may be taller than Excel's.)
    if (!r.whole_rows && !r.whole_cols && at(r.c0, origin_.col) == 0 && at(r.c1, origin_.col) == int64_t{core::kMaxCols} - 1) {
      r.whole_rows = true;
      r.c0 = RefCoord{0, true};
      r.c1 = RefCoord{int64_t{core::kMaxCols} - 1, true};
    }
  }

  Node RowRange(int32_t sheet, bool a1, uint32_t r1, bool a2, uint32_t r2) {
    Node n;
    n.kind = NK::Ref;
    n.ref.sheet = sheet;
    n.ref.is_range = true;
    n.ref.whole_rows = true;
    n.ref.r0 = Coord(r1, a1, origin_.row);
    n.ref.r1 = Coord(r2, a2, origin_.row);
    n.ref.c0 = RefCoord{0, true};
    n.ref.c1 = RefCoord{int64_t{core::kMaxCols} - 1, true};
    Normalize(n.ref);
    return n;
  }

  // R1C1 identifier -> reference (a row or a column alone means the whole row / column)
  bool R1C1Ref(const std::string& text, int32_t sheet, Node* out) {
    if (MatchR1C1(text, 0) != text.size()) return false;
    size_t k = 0;
    auto part = [&](char letter, int64_t limit, RefCoord* c) -> bool {
      if (k >= text.size() || (text[k] != letter && text[k] != letter + 32)) return false;
      ++k;
      *c = RefCoord{0, false};
      if (k < text.size() && text[k] == '[') {
        const size_t close = text.find(']', k);
        c->v = std::strtoll(text.c_str() + k + 1, nullptr, 10);
        k = close + 1;
      } else if (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) {
        const int64_t n = std::strtoll(text.c_str() + k, nullptr, 10);
        while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
        if (n < 1 || n > limit) throw Failure{TR("The reference is outside the sheet"), Peek().pos};
        *c = RefCoord{n - 1, true};
      }
      return true;
    };
    Node n;
    n.kind = NK::Ref;
    n.ref.sheet = sheet;
    RefCoord row, col;
    const bool has_r = part('R', int64_t{core::kMaxRows}, &row);
    const bool has_c = part('C', int64_t{core::kMaxCols}, &col);
    if (has_r && has_c) {
      n.ref.r0 = n.ref.r1 = row;
      n.ref.c0 = n.ref.c1 = col;
    } else if (has_r) {
      n.ref.is_range = n.ref.whole_rows = true;
      n.ref.r0 = n.ref.r1 = row;
      n.ref.c0 = RefCoord{0, true};
      n.ref.c1 = RefCoord{int64_t{core::kMaxCols} - 1, true};
    } else {
      n.ref.is_range = n.ref.whole_cols = true;
      n.ref.c0 = n.ref.c1 = col;
      n.ref.r0 = RefCoord{0, true};
      n.ref.r1 = RefCoord{int64_t{core::kRowEnd}, true};
    }
    *out = std::move(n);
    return true;
  }

  // whether an identifier is a reference: A1, $A$1, A:C (including a following :)
  bool IdentRef(const std::string& text, int32_t sheet, Node* out) {
    if (r1c1_) return R1C1Ref(text, sheet, out);
    CellText c;
    if (ParseCellText(text, &c)) {
      Node n;
      n.kind = NK::Ref;
      n.ref.sheet = sheet;
      n.ref.r0 = n.ref.r1 = Coord(c.row, c.abs_row, origin_.row);
      n.ref.c0 = n.ref.c1 = Coord(c.col, c.abs_col, origin_.col);
      // A1:B2 after a sheet prefix: consume the :B2 here so both ends are on the same sheet
      if (sheet != -1 && IsOp(":") && Peek(1).t == TT::Ident) {
        CellText c2;
        if (ParseCellText(Peek(1).text, &c2)) {
          Next();
          Next();
          n.ref.is_range = true;
          n.ref.r1 = Coord(c2.row, c2.abs_row, origin_.row);
          n.ref.c1 = Coord(c2.col, c2.abs_col, origin_.col);
          Normalize(n.ref);
        }
      }
      *out = std::move(n);
      return true;
    }
    bool a1, a2;
    uint32_t col1, col2;
    if (ParseColText(text, &a1, &col1) && IsOp(":") && Peek(1).t == TT::Ident && ParseColText(Peek(1).text, &a2, &col2)) {
      Next();
      Next();
      Node n;
      n.kind = NK::Ref;
      n.ref.sheet = sheet;
      n.ref.is_range = true;
      n.ref.whole_cols = true;
      n.ref.c0 = Coord(col1, a1, origin_.col);
      n.ref.c1 = Coord(col2, a2, origin_.col);
      n.ref.r0 = RefCoord{0, true};
      n.ref.r1 = RefCoord{int64_t{core::kRowEnd}, true};
      Normalize(n.ref);
      *out = std::move(n);
      return true;
    }
    uint32_t r1, r2;
    if (ParseRowText(text, &a1, &r1) && IsOp(":") && (Peek(1).t == TT::Ident || Peek(1).t == TT::Num) &&
        ParseRowText(Peek(1).text, &a2, &r2)) {  // $9:$2, and $9:2 (the second row is a number)
      Next();
      Next();
      *out = RowRange(sheet, a1, r1, a2, r2);
      return true;
    }
    return false;
  }

  // the text between the brackets of an optional LAMBDA parameter: a plain name
  static bool IsParamName(std::string_view s) {
    if (s.empty() || !IsIdentStart(static_cast<unsigned char>(s[0])) || s[0] == '$' || s[0] == '\\') return false;
    for (const char ch : s) {
      if (!IsIdentChar(static_cast<unsigned char>(ch)) || ch == '$') return false;
    }
    return true;
  }

  Node Call(const Tok& name) {
    Next();  // (
    Node n;
    n.kind = NK::Func;
    std::string up = Upper(name.text);
    if (up.rfind("_XLFN.", 0) == 0) up = up.substr(6);
    if (up.rfind("_XLWS.", 0) == 0) up = up.substr(6);
    n.str = up;
    n.func_id = FindFunction(up);
    if (Peek().t == TT::RParen) {
      Next();
    } else {
      while (true) {
        if (Peek().t == TT::Comma || Peek().t == TT::RParen) {
          Node m;
          m.kind = NK::Missing;
          n.kids.push_back(m);
        } else if (up == "LAMBDA" && Peek().t == TT::Bracket && Peek(1).t == TT::Comma && IsParamName(Peek().text)) {
          // LAMBDA(x,[y],...): a name in brackets before the calculation is an optional parameter, not a column of
          // the table the formula is in; the name node carries b, and the writer puts the brackets back
          Node p;
          p.kind = NK::Name;
          p.str = Next().text;
          p.b = true;
          n.kids.push_back(std::move(p));
        } else {
          n.kids.push_back(Expr(0));
        }
        const Tok sep = Next();
        if (sep.t == TT::RParen) break;
        if (sep.t != TT::Comma) throw Failure{TR("Missing a comma or right parenthesis between function arguments"), sep.pos};
      }
    }
    if (up == "DECIMAL" && n.kids.size() == 2 && sheets_ && sheets_->LegacyDecimal()) {
      n.str = "DECIMAL.SCALE";
      n.func_id = FindFunction(n.str);
    }
    if (n.func_id >= 0) {
      const FuncInfo& fi = GetFunction(n.func_id);
      const int argc = static_cast<int>(n.kids.size());
      if (argc < fi.min_args || (fi.max_args >= 0 && argc > fi.max_args)) {
        throw Failure{std::string(TR("Function ")) + fi.name + TR(" has the wrong number of arguments"), name.pos};
      }
      // LET(x,1,x,2,x) and LAMBDA(x,X,x) name one variable twice: Excel refuses the formula, and a LAMBDA whose
      // optional parameter comes before a required one, LAMBDA([x],y,x+y), too (Excel 16, measured)
      const bool let = up == "LET", lambda = up == "LAMBDA";
      if (let || lambda) {
        std::vector<std::string> seen;
        bool optional = false;
        for (size_t i = 0; i + 1 < n.kids.size(); i += let ? 2 : 1) {
          // a name goes there: LET(1,2,3) and LAMBDA(1,2) are refused as they are typed (Excel 16, measured)
          if (n.kids[i].kind != NK::Name) throw Failure{TR("That name isn't valid."), name.pos};
          const std::string v = Upper(n.kids[i].str);
          if (std::find(seen.begin(), seen.end(), v) != seen.end()) throw Failure{TR("That name isn't valid."), name.pos};
          seen.push_back(v);
          if (optional && !n.kids[i].b) throw Failure{TR("An optional parameter can't come before a required one"), name.pos};
          optional = optional || n.kids[i].b;
        }
      }
      // an argument that takes only a reference refuses what can't be one as it is typed: a constant, an array
      // constant or an operator's result (Excel 16, measured: ROW("A1"), COUNTIF({1,2},1), SUMIF(A1:A2+0,1)). A
      // reference, a name, a function (OFFSET, INDEX, IF... may give a reference) and an error (#REF! after a
      // deletion) pass.
      for (size_t i = 0; i < n.kids.size(); ++i) {
        if (!RefOnlyArg(up, i) || CanBeReference(n.kids[i])) continue;
        throw Failure{TR("There's a problem with this formula."), name.pos};
      }
    }
    return n;
  }

  // the arguments that take only a reference (Excel 16, measured 2026-11-15)
  static bool RefOnlyArg(std::string_view f, size_t i) {
    if (f == "ROW" || f == "COLUMN" || f == "OFFSET" || f == "AREAS" || f == "PHONETIC" || f == "COUNTBLANK" ||
        f == "FORMULATEXT" || f == "ISFORMULA" || f == "COUNTIF")
      return i == 0;
    if (f == "CELL") return i == 1;
    if (f == "SUMIF" || f == "AVERAGEIF") return i == 0 || i == 2;
    if (f == "SUMIFS" || f == "AVERAGEIFS" || f == "MAXIFS" || f == "MINIFS") return i == 0 || i % 2 == 1;
    if (f == "COUNTIFS") return i % 2 == 0;
    if (f == "SUBTOTAL") return i >= 1;
    if (f == "RANK" || f == "RANK.EQ" || f == "RANK.AVG") return i == 1;
    if (f.size() > 1 && f[0] == 'D' &&
        (f == "DAVERAGE" || f == "DCOUNT" || f == "DCOUNTA" || f == "DGET" || f == "DMAX" || f == "DMIN" || f == "DPRODUCT" ||
         f == "DSTDEV" || f == "DSTDEVP" || f == "DSUM" || f == "DVAR" || f == "DVARP"))
      return i == 0 || i == 2;
    return false;
  }
  static bool CanBeReference(const Node& k) {
    switch (k.kind) {
      case NK::Num: case NK::Str: case NK::Bool: case NK::Array: case NK::Percent:
        return false;
      case NK::Binary:
        return k.op == ':' || k.op == ',' || k.op == ' ';  // range, union, intersection
      case NK::Unary:
        return k.op == '@';
      case NK::Paren:
        return k.kids.empty() || CanBeReference(k.kids[0]);
      default:
        return true;  // a reference, a name, a function, a table, an error, an argument left out
    }
  }

  // Table1[spec] / [spec]: items (#All, #Data, #Headers, #Totals, #This Row, @) and columns ([A] or [A]:[B]).
  // A column name keeps its spaces, as Excel's: Table1[[  lead]] is the column "  lead"
  static std::string Unescape(std::string_view s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\'' && i + 1 < s.size()) ++i;
      o += s[i];
    }
    return o;
  }
  // a '[' that isn't escaped ('[ is part of a column name: Table1['[x']] is the column "[x]")
  static bool HasOpenBracket(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\'' && i + 1 < s.size()) {
        ++i;
        continue;
      }
      if (s[i] == '[') return true;
    }
    return false;
  }
  Node StructRef(const std::string& table, const Tok& b) {
    Node n;
    n.kind = NK::StructRef;
    n.str = table;
    uint8_t items = 0;
    std::string c1, c2;
    auto item = [&](std::string s) -> bool {
      std::string u = Upper(s);
      if (u == "#ALL") items |= 1;
      else if (u == "#DATA") items |= 2;
      else if (u == "#HEADERS") items |= 4;
      else if (u == "#TOTALS") items |= 8;
      else if (u == "#THIS ROW") items |= 16;
      else return false;
      return true;
    };
    std::string body = b.text;
    size_t at = body.find_first_not_of(' ');
    if (at != std::string::npos && body[at] == '@') {
      items |= 16;
      body = body.substr(at + 1);
    }
    if (!HasOpenBracket(body)) {
      // one item or one column: [Col], [#All], [@Col]
      const std::string x = Unescape(body);
      if (!x.empty() && !(x[0] == '#' && item(x))) c1 = x;
      if (!x.empty() && x[0] == '#' && c1.empty() && items == 0) throw Failure{TR("Unknown table item"), b.pos};
    } else {
      // [[#Headers],[A]:[B]] / [@[A]:[B]]: parts separated by commas at the top level
      std::vector<std::string> parts;
      int depth = 0;
      std::string cur;
      for (size_t i = 0; i < body.size(); ++i) {
        const char ch = body[i];
        if (ch == '\'' && i + 1 < body.size()) {
          cur += ch;
          cur += body[++i];
          continue;
        }
        if (ch == '[') ++depth;
        if (ch == ']') --depth;
        if (ch == ',' && depth == 0) {
          parts.push_back(cur);
          cur.clear();
          continue;
        }
        cur += ch;
      }
      if (!cur.empty()) parts.push_back(cur);
      for (std::string p : parts) {
        // [A] or [A]:[B]
        auto inner = [&](const std::string& s, size_t from, size_t* end) -> std::string {
          size_t i = s.find('[', from);
          if (i == std::string::npos) throw Failure{TR("A table reference isn't valid"), b.pos};
          std::string v;
          size_t j = i + 1;
          for (; j < s.size() && s[j] != ']'; ++j) {
            if (s[j] == '\'' && j + 1 < s.size()) v += s[j++];
            v += s[j];
          }
          *end = j + 1;
          return Unescape(v);
        };
        size_t e1 = 0;
        const std::string a = inner(p, 0, &e1);
        if (!a.empty() && a[0] == '#') {
          if (!item(a)) throw Failure{TR("Unknown table item"), b.pos};
          continue;
        }
        c1 = a;
        const size_t colon = p.find(':', e1);
        if (colon != std::string::npos) {
          size_t e2 = 0;
          c2 = inner(p, colon, &e2);
        }
      }
    }
    Node k1, k2;
    k1.kind = k2.kind = NK::Str;
    k1.str = c1;
    k2.str = c2;
    n.kids.push_back(std::move(k1));
    n.kids.push_back(std::move(k2));
    n.op = static_cast<char>(items);
    return n;
  }

  // calling the value of an expression: callee(args)
  Node CallOn(Node callee) {
    Next();  // (
    Node n;
    n.kind = NK::Call;
    n.kids.push_back(std::move(callee));
    if (Peek().t == TT::RParen) {
      Next();
      return n;
    }
    while (true) {
      if (Peek().t == TT::Comma || Peek().t == TT::RParen) {
        Node m;
        m.kind = NK::Missing;
        n.kids.push_back(m);
      } else {
        n.kids.push_back(Expr(0));
      }
      const Tok sep = Next();
      if (sep.t == TT::RParen) break;
      if (sep.t != TT::Comma) throw Failure{TR("Missing a comma or right parenthesis between function arguments"), sep.pos};
    }
    return n;
  }

  Node ArrayConst(size_t pos) {
    Node n;
    n.kind = NK::Array;
    uint32_t cols = 0, cur = 0, rows = 1;
    while (true) {
      Node e;
      Tok t = Next();
      bool neg = false;
      if (t.t == TT::Op && (t.text == "-" || t.text == "+")) {
        neg = t.text == "-";
        t = Next();
      }
      if (t.t == TT::Num) {
        e.kind = NK::Num;
        e.num = neg ? -t.num : t.num;
      } else if (t.t == TT::Str) {
        e.kind = NK::Str;
        e.str = t.text;
      } else if (t.t == TT::Err) {
        e.kind = NK::Err;
        e.err = t.err;
      } else if (t.t == TT::Ident && (Upper(t.text) == "TRUE" || Upper(t.text) == "FALSE")) {
        e.kind = NK::Bool;
        e.b = Upper(t.text) == "TRUE";
      } else {
        throw Failure{TR("Array constants can only contain numbers, text, logical values or errors"), t.pos};
      }
      n.kids.push_back(std::move(e));
      ++cur;
      const Tok sep = Next();
      if (sep.t == TT::Comma) continue;
      if (sep.t == TT::Semi) {
        if (cols && cur != cols) throw Failure{TR("Each row of an array constant must have the same number of items"), sep.pos};
        cols = cur;
        cur = 0;
        ++rows;
        continue;
      }
      if (sep.t == TT::RBrace) break;
      throw Failure{TR("The array constant is incomplete"), pos};
    }
    if (cols && cur != cols) throw Failure{TR("Each row of an array constant must have the same number of items"), pos};
    n.cols = cols ? cols : cur;
    n.rows = rows;
    return n;
  }

  std::vector<Tok> t_;
  size_t p_ = 0;
  CellPos origin_;
  const SheetResolver* sheets_;
  bool r1c1_ = false;
  bool keep_order_ = false;
};

// ---- Text output ----
std::string NumText(double v) {
  char buf[40];
  if (std::floor(v) == v && std::fabs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.15G", v);
    // avoid %.15G losing precision: use 17 digits when it doesn't round-trip
    if (std::strtod(buf, nullptr) != v) std::snprintf(buf, sizeof(buf), "%.17G", v);
  }
  return buf;
}

std::string QuoteStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    o += c;
    if (c == '"') o += '"';
  }
  return o + "\"";
}

std::string OpText(char op) {
  switch (op) {
    case 'N': return "<>";
    case 'L': return "<=";
    case 'G': return ">=";
    default: return std::string(1, op);
  }
}

void KeyRef(const RefSpec& r, std::string& o) {
  if (r.bad) {
    o += "#REF!";
    if (!r.lost_sheet) {
      if (r.sheet >= 0) o += "@S" + std::to_string(r.sheet);  // Data!#REF! and #REF! are two texts
      if (r.spill) o += '#';
      return;
    }
  }
  // a deleted sheet's reference has one key whatever its sheet field holds (-2 parsed from #REF!A1, the old id or -1
  // after a move or a copy): its text is the same, and two keys made two formulas of one text that a save and an open
  // made one (doc_fuzz seed 11682258)
  if (r.lost_sheet || r.sheet == -2) o += "S?!";
  else if (r.sheet >= 0) o += "S" + std::to_string(r.sheet) + (r.sheet2 >= 0 ? ":S" + std::to_string(r.sheet2) : std::string()) + "!";
  auto rc = [&o](char p, const RefCoord& c) {
    o += p;
    if (c.abs) o += std::to_string(c.v + 1);
    else o += "[" + std::to_string(c.v) + "]";
  };
  if (r.whole_cols) {
    rc('C', r.c0);
    o += ':';
    rc('C', r.c1);
    return;
  }
  if (r.whole_rows) {
    rc('R', r.r0);
    o += ':';
    rc('R', r.r1);
    return;
  }
  rc('R', r.r0);
  rc('C', r.c0);
  if (r.is_range) {
    o += ':';
    rc('R', r.r1);
    rc('C', r.c1);
  }
  if (r.spill) o += '#';
}

void Key(const Node& n, std::string& o) {
  switch (n.kind) {
    case NK::Num: o += NumText(n.num); break;
    case NK::Str: o += QuoteStr(n.str); break;
    case NK::Bool: o += n.b ? "TRUE" : "FALSE"; break;
    case NK::Err: o += core::ErrorText(n.err); break;
    case NK::Ref: KeyRef(n.ref, o); break;
    case NK::Name:
      if (n.ref.sheet >= 0) o += "S" + std::to_string(n.ref.sheet);
      o += (n.b ? "O'" : "N'") + Upper(n.str) + "'";  // O: an optional LAMBDA parameter, [y]
      break;
    case NK::Missing: break;
    case NK::Paren:
      o += '(';
      Key(n.kids[0], o);
      o += ')';
      break;
    case NK::Unary:
      o += n.op;
      Key(n.kids[0], o);
      break;
    case NK::Percent:
      Key(n.kids[0], o);
      o += '%';
      break;
    case NK::Binary:
      Key(n.kids[0], o);
      o += OpText(n.op);
      Key(n.kids[1], o);
      break;
    case NK::Func:
      o += n.str + "(";
      for (size_t i = 0; i < n.kids.size(); ++i) {
        if (i) o += ',';
        Key(n.kids[i], o);
      }
      o += ')';
      break;
    case NK::Array:
      o += '{';
      for (size_t i = 0; i < n.kids.size(); ++i) {
        if (i) o += (n.cols && i % n.cols == 0) ? ';' : ',';
        Key(n.kids[i], o);
      }
      o += '}';
      break;
    case NK::Call:
      Key(n.kids[0], o);
      o += '(';
      for (size_t i = 1; i < n.kids.size(); ++i) {
        if (i > 1) o += ',';
        Key(n.kids[i], o);
      }
      o += ')';
      break;
    case NK::StructRef:
      o += "T'" + Upper(n.str) + "'[" + std::to_string(static_cast<uint8_t>(n.op)) + "|" + Upper(n.kids[0].str) + "|" + Upper(n.kids[1].str) + "]";
      break;
  }
}

// Table1[...] as Excel writes it
std::string StructText(const Node& n) {
  auto esc = [](const std::string& s) {
    std::string o;
    for (char ch : s) {
      if (ch == '[' || ch == ']' || ch == '#' || ch == '\'') o += '\'';
      o += ch;
    }
    return o;
  };
  auto simple = [](const std::string& s) {
    for (char ch : s) {
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80)) return false;
    }
    return !s.empty();
  };
  // Table1[Unit Price], Table1[p&l]: one column needs no inner brackets unless its name starts or ends with a space
  // (Table1[[  lead]]), as Excel writes it
  auto bare = [](const std::string& s) { return !s.empty() && s.front() != ' ' && s.front() != '@' && s.back() != ' '; };
  const uint8_t items = static_cast<uint8_t>(n.op);
  const std::string& c1 = n.kids[0].str;
  const std::string& c2 = n.kids[1].str;
  std::string cols;
  if (!c1.empty()) cols = c2.empty() ? "[" + esc(c1) + "]" : "[" + esc(c1) + "]:[" + esc(c2) + "]";
  std::string body;
  if (items & 16) {
    // [@Col], [@[Col 1]], [@[A]:[B]], and the whole row Table1[@]
    if (c1.empty()) body = "@";
    else if (c2.empty() && simple(c1)) body = "@" + esc(c1);
    else body = "@" + cols;
  } else {
    // in Excel's order: Table1[[#Headers],[#Data],[Qty]], Table1[[#Data],[#Totals]]; #Data stays when it was written
    std::vector<std::string> parts;
    if (items & 1) parts.push_back("#All");
    if (items & 4) parts.push_back("#Headers");
    if (items & 2) parts.push_back("#Data");
    if (items & 8) parts.push_back("#Totals");
    if (parts.empty()) {
      body = c1.empty() ? "" : (c2.empty() && bare(c1) ? esc(c1) : cols);
      if (c1.empty()) return n.str.empty() ? "[#Data]" : n.str;  // Table1 = its data body
    } else {
      for (size_t i = 0; i < parts.size(); ++i) body += (i ? "," : "") + ("[" + parts[i] + "]");
      if (!cols.empty()) body += "," + cols;
      if (parts.size() == 1 && cols.empty()) body = parts[0];
    }
  }
  return n.str + "[" + body + "]";
}

struct A1Writer {
  CellPos origin;
  const SheetResolver* sheets;
  bool r1c1 = false;
  bool keep_order = false;

  static std::string RcPart(char letter, const RefCoord& c) {
    if (c.abs) return letter + std::to_string(c.v + 1);
    if (c.v == 0) return std::string(1, letter);
    return letter + ("[" + std::to_string(c.v) + "]");
  }

  void Ref(const RefSpec& ref_in, std::string& o) const {
    // a range whose corners pass each other in this cell (a copy moves its relative parts, not its $ ones) is written
    // from its upper-left corner, each part keeping its $, as Excel writes it after a copy (measured: what read
    // =SUM(F$5:$E10) is =SUM($E$5:F10))
    RefSpec r = ref_in;
    if (r.is_range && !r.bad && !keep_order) {
      if (!r.whole_cols && r.r0.Resolve(origin.row) > r.r1.Resolve(origin.row)) std::swap(r.r0, r.r1);
      if (!r.whole_rows && r.c0.Resolve(origin.col) > r.c1.Resolve(origin.col)) std::swap(r.c0, r.c1);
    }
    // a reference to a deleted sheet keeps its cell part after #REF!, as Excel writes it (=#REF!A1+1), so the text
    // can be put right again; a deleted cell is #REF! alone
    bool lost = false;
    if (r.bad) {
      if (!r.lost_sheet) {
        // a deleted cell on another sheet keeps that sheet's name: =Data!#REF!+1 (Excel)
        const std::string nm = r.sheet >= 0 && sheets ? sheets->SheetName(r.sheet) : std::string();
        if (!nm.empty()) o += core::QuoteSheetName(nm, r1c1) + "!";
        o += r.spill ? "#REF!#" : "#REF!";  // a spill range whose anchor was deleted: =SUM(#REF!#) (Excel)
        return;
      }
      lost = true;
    } else if (r.sheet >= 0) {
      const std::string nm = sheets ? sheets->SheetName(r.sheet) : std::string();
      const std::string nm2 = r.sheet2 >= 0 && sheets ? sheets->SheetName(r.sheet2) : std::string();
      if (nm.empty() || (r.sheet2 >= 0 && nm2.empty())) {
        lost = true;
      } else if (r.sheet2 >= 0) {
        // Sheet1:Sheet3! or 'Sheet 1:Sheet 3'! (one pair of quotes around both names)
        const std::string q1 = core::QuoteSheetName(nm, r1c1), q2 = core::QuoteSheetName(nm2, r1c1);
        if (q1[0] == '\'' || q2[0] == '\'') {
          std::string both = nm + ":" + nm2, esc;
          for (char ch : both) {
            esc += ch;
            if (ch == '\'') esc += '\'';
          }
          o += "'" + esc + "'!";
        } else {
          o += nm + ":" + nm2 + "!";
        }
      } else {
        o += core::QuoteSheetName(nm, r1c1) + "!";
      }
    }
    if (lost) o += "#REF!";
    auto col = [&](const RefCoord& c, std::string& s) -> bool {
      const int64_t v = c.Resolve(origin.col);
      if (v < 0 || v >= core::kMaxCols) return false;
      if (c.abs) s += '$';
      s += core::ColumnName(static_cast<uint32_t>(v));
      return true;
    };
    auto row = [&](const RefCoord& c, std::string& s) -> bool {
      const int64_t v = c.Resolve(origin.row);
      if (v < 0 || v >= int64_t{core::kMaxRows}) return false;
      if (c.abs) s += '$';
      s += std::to_string(v + 1);
      return true;
    };
    std::string s;
    bool ok = true;
    if (r1c1) {
      if (r.whole_cols) {
        s = RcPart('C', r.c0);
        if (!(r.c1 == r.c0)) s += ":" + RcPart('C', r.c1);
      } else if (r.whole_rows) {
        s = RcPart('R', r.r0);
        if (!(r.r1 == r.r0)) s += ":" + RcPart('R', r.r1);
      } else {
        s = RcPart('R', r.r0) + RcPart('C', r.c0);
        if (r.is_range) s += ":" + RcPart('R', r.r1) + RcPart('C', r.c1);
      }
      if (r.spill) s += '#';
      o += s;
      return;
    }
    if (r.whole_cols) {
      ok = col(r.c0, s);
      s += ':';
      ok = ok && col(r.c1, s);
    } else if (r.whole_rows) {
      ok = row(r.r0, s);
      s += ':';
      ok = ok && row(r.r1, s);
    } else {
      ok = col(r.c0, s) && row(r.r0, s);
      if (ok && r.is_range) {
        s += ':';
        ok = col(r.c1, s) && row(r.r1, s);
      }
    }
    if (ok && r.spill) s += '#';
    o += ok ? s : std::string(lost ? "" : r.spill ? "#REF!#" : "#REF!");
  }

  void Write(const Node& n, std::string& o) const {
    switch (n.kind) {
      case NK::Ref: Ref(n.ref, o); break;
      case NK::Name:
        if (n.ref.sheet >= 0) {
          const std::string nm = sheets ? sheets->SheetName(n.ref.sheet) : std::string();
          if (nm.empty()) {  // the sheet was deleted
            o += "#REF!";
            break;
          }
          o += core::QuoteSheetName(nm, r1c1) + "!";
        }
        if (n.b) o += '[';  // an optional LAMBDA parameter
        o += n.str;
        if (n.b) o += ']';
        break;
      case NK::Paren:
        o += '(';
        Write(n.kids[0], o);
        o += ')';
        break;
      case NK::Unary:
        o += n.op;
        Write(n.kids[0], o);
        break;
      case NK::Percent:
        Write(n.kids[0], o);
        o += '%';
        break;
      case NK::Binary: {
        const size_t at = o.size();
        Write(n.kids[0], o);
        if (n.op == ':') {
          // a bare name or cell before a sheet's reference reads back as a range of sheets: Ab:Sheet1!A3 is the sheets
          // Ab to Sheet1 (#REF!), where it was the name Ab to Sheet1!A3 (a formula cut to another sheet after Replace
          // made =A1:A3*2 =Ab:A3*2, doc_fuzz --more seed 24071853). The left side goes in parentheses then
          std::string right;
          Write(n.kids[1], right);
          const std::string_view left = std::string_view(o).substr(at);
          const bool right_sheet = !right.empty() && right[0] != '(' && right.find('!') != std::string::npos;
          const bool bare_left = !left.empty() && left.find('!') == std::string_view::npos && left.back() != ')';
          if (right_sheet && bare_left) {
            o.insert(at, "(");
            o += ')';
          }
          o += ':';
          o += right;
          break;
        }
        o += OpText(n.op);
        Write(n.kids[1], o);
        break;
      }
      case NK::Func:
        o += n.str + "(";
        for (size_t i = 0; i < n.kids.size(); ++i) {
          if (i) o += ',';
          Write(n.kids[i], o);
        }
        o += ')';
        break;
      case NK::StructRef: o += StructText(n); break;
      case NK::Call:
        Write(n.kids[0], o);
        o += '(';
        for (size_t i = 1; i < n.kids.size(); ++i) {
          if (i > 1) o += ',';
          Write(n.kids[i], o);
        }
        o += ')';
        break;
      default:
        Key(n, o);
        break;
    }
  }
};

void Flags(const Node& n, uint32_t* f, std::vector<std::string>& scope) {
  auto bound = [&](const std::string& s) { return std::find(scope.begin(), scope.end(), Upper(s)) != scope.end(); };
  if (n.kind == NK::Func) {
    if (n.func_id < 0) {
      if (!bound(n.str)) *f |= model::FormulaDef::kUdf;
    } else {
      const FuncInfo& fi = GetFunction(n.func_id);
      if (fi.is_volatile) *f |= model::FormulaDef::kVolatile;
      if (fi.is_volatile && !fi.dynamic_ref) *f |= model::FormulaDef::kChanging;
      if (fi.dynamic_ref) *f |= model::FormulaDef::kDynamicRef | model::FormulaDef::kVolatile;
      if (std::string_view(fi.name) == "QUOTE" || std::string_view(fi.name) == "STREAM") *f |= model::FormulaDef::kStream;
      const bool let = std::string_view(fi.name) == "LET", lambda = std::string_view(fi.name) == "LAMBDA";
      if (let || lambda) {
        // LET(name, value, ..., calc) / LAMBDA(param, ..., body): the names are variables from here on
        const size_t base = scope.size();
        for (size_t i = 0; i < n.kids.size(); ++i) {
          const Node& k = n.kids[i];
          const bool name_slot = i + 1 < n.kids.size() && (lambda || i % 2 == 0);
          if (name_slot && k.kind == NK::Name) {
            scope.push_back(Upper(k.str));
            continue;
          }
          Flags(k, f, scope);
        }
        scope.resize(base);
        return;
      }
    }
  }
  // 3D references depend on which sheets lie between the two: recalculated every time, like other dynamic references
  if (n.kind == NK::Ref && n.ref.sheet2 >= 0) *f |= model::FormulaDef::kDynamicRef | model::FormulaDef::kVolatile;
  if (n.kind == NK::Name && !bound(n.str)) *f |= model::FormulaDef::kDynamicRef;  // the target of a name may change: conservatively treat it as dynamic
  for (const Node& k : n.kids) Flags(k, f, scope);
}

// a name Excel's A1 formulas refuse (R1C1LikeName: =R2D2, =C3PO, =Other!R1x) outside LET and LAMBDA, whose
// variables may be named so (measured: LET(r,1,r), LAMBDA(r,c,r*c), LET(R2D2,1,R2D2))
bool HasUnboundR1C1Name(const Node& n, std::vector<std::string>& scope) {
  if (n.kind == NK::Name) {
    if (n.ref.sheet < 0 && std::find(scope.begin(), scope.end(), Upper(n.str)) != scope.end()) return false;
    return R1C1LikeName(n.str);
  }
  if (n.kind == NK::Func && n.func_id >= 0) {
    const std::string_view name = GetFunction(n.func_id).name;
    const bool let = name == "LET", lambda = name == "LAMBDA";
    if (let || lambda) {
      const size_t base = scope.size();
      bool bad = false;
      for (size_t i = 0; i < n.kids.size() && !bad; ++i) {
        const Node& k = n.kids[i];
        if (i + 1 < n.kids.size() && (lambda || i % 2 == 0) && k.kind == NK::Name) {
          scope.push_back(Upper(k.str));
          continue;
        }
        bad = HasUnboundR1C1Name(k, scope);
      }
      scope.resize(base);
      return bad;
    }
  }
  for (const Node& k : n.kids) {
    if (HasUnboundR1C1Name(k, scope)) return true;
  }
  return false;
}

void Refs(const Node& n, CellPos origin, std::vector<ResolvedRef>* out) {
  if (n.kind == NK::Ref && !n.ref.bad) {
    ResolvedRef rr;
    if (ResolveRef(n.ref, origin, &rr)) out->push_back(rr);
  }
  for (const Node& k : n.kids) Refs(k, origin, out);
}

void Specs(const Node& n, std::vector<RefSpec>* out) {
  if (n.kind == NK::Ref && n.ref.HasCells()) out->push_back(n.ref);  // #REF!A1 too: it moves with its formula
  for (const Node& k : n.kids) Specs(k, out);
}

}  // namespace

ParseResult Parse(std::string_view text, CellPos origin, const SheetResolver* sheets, bool r1c1, bool keep_order) {
  ParseResult res;
  std::vector<Tok> toks;
  Lexer lx(text, r1c1);
  if (!lx.Run(&toks, &res.error, &res.error_pos)) return res;
  Parser p(std::move(toks), origin, sheets, r1c1, keep_order);
  res.ok = p.Run(&res.root, &res.error, &res.error_pos);
  if (res.ok && !r1c1) {
    std::vector<std::string> scope;
    if (HasUnboundR1C1Name(res.root, scope)) {
      res.ok = false;
      res.error = TR("A name can't be the same as an R1C1 reference.");
      res.error_pos = 0;
    }
  }
  return res;
}

std::string SerializeKey(const Node& root) {
  std::string o;
  Key(root, o);
  return o;
}

std::string ToA1(const Node& root, CellPos origin, const SheetResolver* sheets, bool keep_order) {
  std::string o;
  A1Writer{origin, sheets, false, keep_order}.Write(root, o);
  return o;
}

std::string ToR1C1(const Node& root, CellPos origin, const SheetResolver* sheets) {
  std::string o;
  A1Writer{origin, sheets, true}.Write(root, o);
  return o;
}

std::string RangeR1C1(const core::Rect& r, const CellPos* origin) {
  auto row = [&](uint32_t v) { return origin ? RefCoord{int64_t{v} - origin->row, false} : RefCoord{v, true}; };
  auto col = [&](uint32_t v) { return origin ? RefCoord{int64_t{v} - origin->col, false} : RefCoord{v, true}; };
  std::string s = A1Writer::RcPart('R', row(r.r0)) + A1Writer::RcPart('C', col(r.c0));
  if (!r.IsCell()) s += ":" + A1Writer::RcPart('R', row(r.r1)) + A1Writer::RcPart('C', col(r.c1));
  return s;
}

uint32_t ComputeFlags(const Node& root) {
  uint32_t f = 0;
  std::vector<std::string> scope;
  Flags(root, &f, scope);
  return f;
}

void CollectRefs(const Node& root, CellPos origin, std::vector<ResolvedRef>* out) { Refs(root, origin, out); }

void CollectRefSpecs(const Node& root, std::vector<RefSpec>* out) { Specs(root, out); }

bool ResolveRef(const RefSpec& r, CellPos origin, ResolvedRef* out) {
  int64_t r0 = r.r0.Resolve(origin.row), r1 = (r.is_range ? r.r1 : r.r0).Resolve(origin.row);
  int64_t c0 = r.c0.Resolve(origin.col), c1 = (r.is_range ? r.c1 : r.c0).Resolve(origin.col);
  if (r.whole_cols) {
    r0 = 0;
    r1 = core::kRowEnd;
  }
  if (r0 > r1) std::swap(r0, r1);
  if (c0 > c1) std::swap(c0, c1);
  if (r0 < 0 || c0 < 0) return false;
  out->sheet = r.sheet;
  out->rect = core::Rect{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(std::min<int64_t>(r1, core::kRowEnd)),
                         static_cast<uint32_t>(std::min<int64_t>(c1, core::kMaxCols - 1))};
  return true;
}

void CollectNames(const Node& root, std::vector<std::string>* out) {
  if (root.kind == NK::Name) out->push_back(Upper(root.str));
  for (const Node& k : root.kids) CollectNames(k, out);
}

void ForEachRef(Node& root, const std::function<void(RefSpec&)>& fn) {
  if (root.kind == NK::Ref) fn(root.ref);
  for (Node& k : root.kids) ForEachRef(k, fn);
}

namespace {

// Excel's grid: relative names wrap around its 1,048,576 rows and 16,384 columns
constexpr int64_t kWrapRows = 1048576;

bool RefHasRelative(const RefSpec& r) {
  if (!r.HasCells()) return false;
  const bool rows = !r.whole_cols && (!r.r0.abs || (r.is_range && !r.r1.abs));
  const bool cols = !r.whole_rows && (!r.c0.abs || (r.is_range && !r.c1.abs));
  return rows || cols;
}

}  // namespace

bool HasRelativeRef(const Node& root) {
  if (root.kind == NK::Ref && RefHasRelative(root.ref)) return true;
  for (const Node& k : root.kids) {
    if (HasRelativeRef(k)) return true;
  }
  return false;
}

void RelativeTo(Node& root, CellPos to) {
  ForEachRef(root, [&](RefSpec& r) {
    if (!r.HasCells()) return;
    // the offset, taken around the sheet (XFD from A is one column left), then the cell it reaches from `to`
    auto fix = [](RefCoord& c, int64_t at, int64_t n) {
      if (c.abs) return;
      int64_t off = c.v % n;
      if (off < 0) off += n;
      if (off >= n / 2) off -= n;
      int64_t t = at + off;
      if (t < 0) t += n;
      if (n == core::kMaxCols && t >= n) t -= n;
      c.v = t - at;
    };
    if (!r.whole_cols) {
      fix(r.r0, to.row, kWrapRows);
      fix(r.r1, to.row, kWrapRows);
    }
    if (!r.whole_rows) {
      fix(r.c0, to.col, core::kMaxCols);
      fix(r.c1, to.col, core::kMaxCols);
    }
  });
}

std::string MoveRelativeText(std::string_view text, CellPos from, CellPos to, const SheetResolver* sheets, bool r1c1, bool keep_order) {
  std::string_view body = text;
  const bool eq = !body.empty() && body.front() == '=';
  if (eq) body.remove_prefix(1);
  ParseResult pr = Parse(body, from, sheets, r1c1, keep_order);
  if (!pr.ok || (!r1c1 && !HasRelativeRef(pr.root))) return std::string(text);
  RelativeTo(pr.root, to);
  return (eq ? "=" : "") + ToA1(pr.root, to, sheets, keep_order);
}

std::string AbsoluteRefText(std::string_view text, const SheetResolver* sheets) {
  std::string_view body = text;
  const bool eq = !body.empty() && body.front() == '=';
  if (eq) body.remove_prefix(1);
  ParseResult pr = Parse(body, CellPos{0, 0}, sheets);
  if (!pr.ok || !HasRelativeRef(pr.root)) return std::string(text);
  ForEachRef(pr.root, [](RefSpec& r) {
    if (!r.HasCells()) return;
    for (RefCoord* c : {&r.r0, &r.r1, &r.c0, &r.c1}) c->abs = true;  // at A1 an offset is the index itself
  });
  return (eq ? "=" : "") + ToA1(pr.root, CellPos{0, 0}, sheets);
}

namespace {

bool IsRefSpanByte(unsigned char ch) { return std::isalnum(ch) || ch == '$' || ch == ':' || ch == '!' || ch == '_' || ch == '.' || ch >= 0x80; }

}  // namespace

std::vector<RefSpan> FindRefSpans(std::string_view text, CellPos origin, const SheetResolver* res, bool r1c1) {
  std::vector<RefSpan> out;
  const size_t n = text.size();
  size_t i = !text.empty() && text[0] == '=' ? 1 : 0;
  while (i < n) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == '"') {
      for (++i; i < n; ++i) {
        if (text[i] == '"') {
          if (i + 1 < n && text[i + 1] == '"') ++i;
          else break;
        }
      }
      ++i;
      continue;
    }
    if (!(ch == '\'' || IsRefSpanByte(ch)) || ch == ':' || ch == '!') {
      ++i;
      continue;
    }
    const size_t b = i;
    if (ch == '\'') {  // 'Sheet name'!A1
      for (++i; i < n; ++i) {
        if (text[i] == '\'') {
          if (i + 1 < n && text[i + 1] == '\'') ++i;
          else break;
        }
      }
      if (i < n) ++i;  // past the closing quote; a quote never closed (=SUM(A1)'0012 being typed) ends at the text's end
    }
    while (i < n) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c == '[') {  // R[-1]C[2], Table1[Qty]
        int depth = 0;
        for (; i < n; ++i) {
          if (text[i] == '[') ++depth;
          else if (text[i] == ']' && --depth == 0) break;
        }
        if (i < n) ++i;
        continue;
      }
      if (!IsRefSpanByte(c)) break;
      ++i;
    }
    size_t e = i;
    size_t k = i;
    while (k < n && text[k] == ' ') ++k;
    if (k < n && text[k] == '(') continue;  // a function's name
    size_t pe = e;
    while (pe > b && text[pe - 1] == ':') --pe;  // "A1:" while the range is being typed
    if (pe <= b) continue;
    const ParseResult pr = Parse(text.substr(b, pe - b), origin, res, r1c1);
    if (!pr.ok) continue;
    std::vector<ResolvedRef> refs;
    CollectRefs(pr.root, origin, &refs);
    if (refs.size() != 1) continue;
    out.push_back({static_cast<int>(b), static_cast<int>(e), refs[0], 0});
  }
  // one group for each different reference (the same color in the edit box and on the sheet)
  std::vector<ResolvedRef> seen;
  for (RefSpan& sp : out) {
    size_t c = 0;
    while (c < seen.size() && !(seen[c].sheet == sp.ref.sheet && seen[c].rect == sp.ref.rect)) ++c;
    if (c == seen.size()) seen.push_back(sp.ref);
    sp.group = static_cast<int>(c);
  }
  return out;
}

std::string FoldFullWidthFormula(std::string_view text) {
  if (text.size() < 3 || text.substr(0, 3) != "\xEF\xBC\x9D") return std::string(text);  // U+FF1D
  std::string out;
  out.reserve(text.size());
  bool quoted = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char b0 = static_cast<unsigned char>(text[i]);
    if (!quoted && b0 == 0xEF && i + 2 < text.size()) {
      const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
      if (cp >= 0xFF01 && cp <= 0xFF5E) {
        const char half = static_cast<char>(cp - 0xFEE0);
        out += half;
        if (half == '"') quoted = true;
        i += 2;
        continue;
      }
    }
    if (!quoted && b0 == 0xE3 && i + 2 < text.size() && text[i + 1] == '\x80' && text[i + 2] == '\x80') {  // ideographic space
      out += ' ';
      i += 2;
      continue;
    }
    if (text[i] == '"') quoted = !quoted;
    out += text[i];
  }
  return out;
}

bool LooksLikeFormula(std::string_view text) {
  if (text.empty()) return false;
  if (text[0] == '=') return true;
  if (text[0] != '+' && text[0] != '-') return false;
  if (text.size() < 2) return false;
  // a typed amount, as Excel reads it first: -5, +.5, -$5, -NT$5 (NT$5 is also a cell), - 5, -%12.5, -1 1/2; anything
  // else after a sign is a formula, digits first too (Excel 16, measured: +1+2 is =1+2, -2^2 is =-2^2, +1/2 is =1/2)
  InputParse amount;
  return !ParseNumberInput(text, &amount);
}

namespace {

struct CellTok {
  size_t begin = 0, end = 0;  // [begin, end) in the text
  size_t col_begin = 0, col_end = 0, row_begin = 0, row_end = 0;  // a part that is missing is empty: A of A:C, 1 of 1:3
  bool col_abs = false, row_abs = false;
  bool HasCol() const { return col_end > col_begin; }
  bool HasRow() const { return row_end > row_begin; }
};

bool IsRefWordChar(char ch) {
  const unsigned char u = static_cast<unsigned char>(ch);
  return std::isalnum(u) || ch == '_' || ch == '.' || ch == '$' || u >= 0x80;  // a CJK name like 價格A1 is one word
}

// an A1 cell reference starting exactly at i ($A$1, A1, XFD1048576)
bool ScanCell(const std::string& s, size_t i, CellTok* t) {
  size_t k = i;
  t->begin = i;
  t->col_abs = k < s.size() && s[k] == '$';
  if (t->col_abs) ++k;
  t->col_begin = k;
  while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k])) && k - t->col_begin < 4) ++k;
  t->col_end = k;
  const size_t letters = t->col_end - t->col_begin;
  if (letters == 0 || letters > 3) return false;
  t->row_abs = k < s.size() && s[k] == '$';
  if (t->row_abs) ++k;
  t->row_begin = k;
  while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
  t->row_end = k;
  if (t->row_end == t->row_begin || t->row_end - t->row_begin > 7 || s[t->row_begin] == '0') return false;
  t->end = k;
  return true;
}

// one end of a whole-column (A of A:C) or whole-row (1 of 1:3) reference starting exactly at i
bool ScanColumnEnd(const std::string& s, size_t i, CellTok* t) {
  size_t k = i;
  t->begin = i;
  t->col_abs = k < s.size() && s[k] == '$';
  if (t->col_abs) ++k;
  t->col_begin = k;
  while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k])) && k - t->col_begin < 4) ++k;
  t->col_end = k;
  t->row_begin = t->row_end = k;
  t->row_abs = false;
  const size_t letters = t->col_end - t->col_begin;
  t->end = k;
  return letters >= 1 && letters <= 3 && (k >= s.size() || !std::isalnum(static_cast<unsigned char>(s[k])));
}

bool ScanRowEnd(const std::string& s, size_t i, CellTok* t) {
  size_t k = i;
  t->begin = i;
  t->col_begin = t->col_end = k;
  t->col_abs = false;
  t->row_abs = k < s.size() && s[k] == '$';
  if (t->row_abs) ++k;
  t->row_begin = k;
  while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
  t->row_end = k;
  t->end = k;
  const size_t digits = t->row_end - t->row_begin;
  return digits >= 1 && digits <= 7 && s[t->row_begin] != '0' && (k >= s.size() || !IsRefWordChar(s[k]));
}

std::string CellText(const std::string& s, const CellTok& t, bool col_abs, bool row_abs) {
  std::string out;
  if (col_abs && t.HasCol()) out += '$';
  out.append(s, t.col_begin, t.col_end - t.col_begin);
  if (row_abs && t.HasRow()) out += '$';
  out.append(s, t.row_begin, t.row_end - t.row_begin);
  return out;
}

}  // namespace

bool CycleAbsoluteRef(std::string& text, int sel_begin, int sel_end, int* out_begin, int* out_end) {
  // find the references outside string literals and quoted sheet names: one cell, or two joined by ':'
  struct Ref {
    CellTok a, b;
    bool range = false;
    size_t begin() const { return a.begin; }
    size_t end() const { return range ? b.end : a.end; }
  };
  std::vector<Ref> refs;
  const std::string& s = text;
  for (size_t i = 0; i < s.size();) {
    const char ch = s[i];
    if (ch == '"' || ch == '\'') {
      size_t k = i + 1;
      while (k < s.size() && s[k] != ch) ++k;
      i = k + 1;
      continue;
    }
    if ((ch == '$' || std::isalnum(static_cast<unsigned char>(ch))) && (i == 0 || !IsRefWordChar(s[i - 1]))) {
      Ref r;
      if (ScanCell(s, i, &r.a)) {
        size_t e = r.a.end;
        if (e < s.size() && s[e] == ':' && ScanCell(s, e + 1, &r.b)) {
          r.range = true;
          e = r.b.end;
        }
        // not a function name (LOG10() or part of a longer word
        if (e >= s.size() || (!IsRefWordChar(s[e]) && s[e] != '(' && s[e] != '!')) {
          refs.push_back(r);
          i = e;
          continue;
        }
      }
      // whole columns or rows (A:C, $1:$3): F4 toggles their $ as Excel's does
      CellTok a, b;
      if ((ScanColumnEnd(s, i, &a) && a.end < s.size() && s[a.end] == ':' && ScanColumnEnd(s, a.end + 1, &b)) ||
          (ScanRowEnd(s, i, &a) && a.end < s.size() && s[a.end] == ':' && ScanRowEnd(s, a.end + 1, &b))) {
        if (b.end >= s.size() || (!IsRefWordChar(s[b.end]) && s[b.end] != '(' && s[b.end] != '!')) {
          r.a = a;
          r.b = b;
          r.range = true;
          refs.push_back(r);
          i = b.end;
          continue;
        }
      }
      while (i < s.size() && IsRefWordChar(s[i])) ++i;
      continue;
    }
    ++i;
  }
  if (refs.empty()) return false;
  // what F4 changes, as Excel's (16, measured): with a selection, every reference it touches, each end of a range on its
  // own (=B2:C3 with C3 selected: =B2:$C$3); without one, the cell the caret is in or at either end of, past closing
  // parentheses just before it (=SUM(B2:B4) with the caret at its end: =SUM(B2:$B$4)), and nothing when the caret is at
  // no reference (=B2+5, =B2 and a space). Whole columns and rows (A:A, 1:3) stay one reference
  struct Unit {
    size_t begin, end;
    const CellTok* a;
    const CellTok* b;  // the second end of a whole-column or whole-row reference
  };
  std::vector<Unit> units;
  for (const Ref& r : refs) {
    const bool whole = r.range && (!r.a.HasRow() || !r.a.HasCol());
    if (whole) {
      units.push_back(Unit{r.a.begin, r.b.end, &r.a, &r.b});
    } else {
      units.push_back(Unit{r.a.begin, r.a.end, &r.a, nullptr});
      if (r.range) units.push_back(Unit{r.b.begin, r.b.end, &r.b, nullptr});
    }
  }
  const size_t n = s.size();
  const size_t lo = static_cast<size_t>(std::clamp(std::min(sel_begin, sel_end), 0, static_cast<int>(n)));
  const size_t hi = static_cast<size_t>(std::clamp(std::max(sel_begin, sel_end), 0, static_cast<int>(n)));
  std::vector<size_t> pick;
  if (hi > lo) {
    for (size_t k = 0; k < units.size(); ++k)
      if (units[k].begin < hi && units[k].end > lo) pick.push_back(k);
  } else {
    size_t p = lo;
    while (p > 0 && s[p - 1] == ')') --p;
    for (size_t k = 0; k < units.size() && pick.empty(); ++k)
      if (units[k].begin <= p && p <= units[k].end) pick.push_back(k);
  }
  if (pick.empty()) return false;
  // the next state, from the first picked reference: A1 -> $A$1 -> A$1 -> $A1 -> A1
  const CellTok& f = *units[pick.front()].a;
  bool ca, ra;
  if (!f.HasRow()) ca = !f.col_abs, ra = false;       // a whole column: A:A <-> $A:$A
  else if (!f.HasCol()) ca = false, ra = !f.row_abs;  // a whole row: 1:1 <-> $1:$1
  else if (!f.col_abs && !f.row_abs) ca = true, ra = true;
  else if (f.col_abs && f.row_abs) ca = false, ra = true;
  else if (!f.col_abs && f.row_abs) ca = true, ra = false;
  else ca = false, ra = false;
  // rewrite back to front so earlier offsets stay valid
  const size_t first = units[pick.front()].begin;
  size_t last_end = 0;
  const std::string src = s;  // (the tokens point into the text as it was)
  for (size_t k = pick.size(); k-- > 0;) {
    const Unit& u = units[pick[k]];
    std::string rep = CellText(src, *u.a, ca, ra);
    if (u.b) rep += ":" + CellText(src, *u.b, ca, ra);
    const size_t old_len = u.end - u.begin;
    text.replace(u.begin, old_len, rep);
    if (k + 1 == pick.size()) last_end = u.begin + rep.size();
    else last_end = last_end + rep.size() - old_len;
  }
  *out_begin = static_cast<int>(first);
  *out_end = static_cast<int>(last_end);
  return true;
}

bool IsR1C1LikeName(std::string_view s) { return R1C1LikeName(s); }

}  // namespace yc::formula
