// SPDX-License-Identifier: Apache-2.0
// python_bridge.cpp - YCP server (protocol: see python/yc/_conn.py)
#include "python_bridge.h"
#include "error_log.h"
#include "watchdog.h"

#include "app_state.h"
#include "npy.h"
#include "screenshot.h"
#include "settings.h"
#include "trust.h"

#include "app/shell_integration.h"
#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "ribbon/ribbon.h"
#include "core/decimal.h"
#include "core/paths.h"
#include "engine/custom_lists.h"
#include "model/table.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "engine/tile_iter.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
#include "core/parallel.h"
#include "formula/datetime.h"
#include "formula/func_help.h"
#include "formula/parser.h"
#include "io/content_dump.h"
#include "io/csv.h"
#include "io/xlsx.h"
#include "io/ycg.h"
#include "net.h"
#include "titlebar/titlebar.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace yc::app {

namespace {
// E_PROTECTED: why the request was refused, and how to get past it
std::string RefusalText(int reason) {
  switch (static_cast<engine::BlockReason>(reason)) {
    case engine::BlockReason::ReadOnly:
      return "YCGrid is in read-only mode (Operator Mode is off): the UI can't change the workbook. Turn on Trading > Operator Mode "
             "(yc.safety.set_operator_mode(True)); Python's own writes are not affected";
    case engine::BlockReason::Pivot: return "That is part of a PivotTable report: change the PivotTable instead";
    case engine::BlockReason::DataTable: return "That is part of a data table: its body is computed";
    case engine::BlockReason::Parameter: return "That is a parameter cell: type the new value in it (yc.safety.parameter)";
    case engine::BlockReason::Structure: return "The workbook's structure is protected (Workbook.Unprotect)";
    case engine::BlockReason::MergeSize: return "To do this, all the merged cells need to be the same size (unmerge them first)";
    case engine::BlockReason::TableShift: return "The operation would shift cells in a table on the worksheet";
    case engine::BlockReason::Array: return "You can't change part of an array: change, clear or delete the whole array formula (CurrentArray)";
    case engine::BlockReason::TableMerge: return "Cells in a table can't be merged (Excel 16 refuses Range.Merge over a table)";
    case engine::BlockReason::TooLarge: return "The fill would write more than 20,000,000 cells: fill fewer cells";
    case engine::BlockReason::OffSheet:
      return "To prevent possible loss of data, nonblank cells can't be shifted off the worksheet: delete the data at the sheet's end first";
    default:
      return "The cell or chart you're trying to change is on a protected sheet (Worksheet.Unprotect, or Protect(UserInterfaceOnly=True))";
  }
}
}  // namespace

using engine::Document;
using io::MV;

namespace {

constexpr uint32_t kRowEnd = core::kRowEnd;

std::wstring Wide(const std::string& s) { return FromUtf8(s); }

Rect RectOf(const MV& v) {
  if (v.t != MV::T::Array || v.a.size() < 4) return Rect{};
  auto g = [&](size_t i) {
    const int64_t x = v.a[i].t == MV::T::Int ? v.a[i].i : static_cast<int64_t>(v.a[i].Num());
    return static_cast<uint32_t>(std::clamp<int64_t>(x, 0, 0xFFFFFFFFll));
  };
  return Rect{g(0), g(1), g(2), g(3)};
}

std::vector<Rect> AreasOf(const MV& p) {
  std::vector<Rect> out;
  if (const MV* a = p.Get("areas"); a && a->t == MV::T::Array) {
    for (const MV& x : a->a) out.push_back(RectOf(x));
  } else if (const MV* r = p.Get("rect")) {
    out.push_back(RectOf(*r));
  }
  return out;
}

// before a request reads what cells show (Go To Special's kinds, a sort's order): formulas just written by this
// connection have their values (with manual calculation, the entered ones, as Excel shows them)
void Settle(Document& d) { d.SettleForRead(); }

// clamp entire columns (r1 = ROW_END) to the sheet's current row count
Rect Clamp(const model::Sheet& s, Rect r) {
  if (r.r1 >= s.row_limit) r.r1 = s.row_limit - 1;
  if (r.c1 >= s.col_limit) r.c1 = s.col_limit - 1;
  if (r.r0 > r.r1) r.r0 = r.r1;
  if (r.c0 > r.c1) r.c0 = r.c1;
  return r;
}

MV ValueToMV(const core::Value& v) {
  switch (v.type()) {
    case core::VType::Number: return MV::Double(v.number());
    case core::VType::Bool: return MV::Bool(v.boolean());
    case core::VType::StrInline:
    case core::VType::StrHeap: {
      if (formula::ImageSpec img; formula::ParseImageValue(v.str(), &img)) return MV::Str(img.alt);  // IMAGE(): its alt text
      if (std::string cap; formula::ParseCubeValue(v.str(), &cap)) return MV::Str(cap);               // CUBEMEMBER(): its caption
      return MV::Str(std::string(v.str()));
    }
    case core::VType::Error: return MV::Ext(2, std::string(1, static_cast<char>(v.error())));
    case core::VType::Pending: return MV::Ext(3, std::string(1, '\0'));
    case core::VType::Decimal: {
      std::string d(17, '\0');
      d[0] = static_cast<char>(v.dec_scale());
      const core::Int128 m = v.dec_mantissa();
      for (int i = 0; i < 8; ++i) d[1 + static_cast<size_t>(i)] = static_cast<char>((m.lo >> (8 * i)) & 0xFF);
      for (int i = 0; i < 8; ++i) d[9 + static_cast<size_t>(i)] = static_cast<char>((static_cast<uint64_t>(m.hi) >> (8 * i)) & 0xFF);
      return MV::Ext(1, std::move(d));
    }
    default: return MV::Nil();
  }
}

// protocol value -> cell value; kind: 0 = anything but timestamps/dates as is, 1 = date (ext 4), 2 = time (ext 5)
core::Value MVToValue(const MV& v, int* date_kind) {
  *date_kind = 0;
  switch (v.t) {
    case MV::T::Nil: return core::Value();
    case MV::T::Bool: return core::Value::Bool(v.b);
    case MV::T::Int: return core::Value::Number(static_cast<double>(v.i));
    case MV::T::Double: return core::Value::Number(v.d);
    case MV::T::Str:
    case MV::T::Bin: return core::Value::String(v.s);
    case MV::T::Ext: {
      if (v.ext == 1 && v.s.size() >= 17) {
        core::Int128 m;
        m.lo = 0;
        uint64_t hi = 0;
        for (int i = 7; i >= 0; --i) m.lo = (m.lo << 8) | static_cast<uint8_t>(v.s[1 + static_cast<size_t>(i)]);
        for (int i = 7; i >= 0; --i) hi = (hi << 8) | static_cast<uint8_t>(v.s[9 + static_cast<size_t>(i)]);
        m.hi = static_cast<int64_t>(hi);
        const core::Dec d{m, static_cast<uint8_t>(v.s[0])};
        return core::DecToValue(d);
      }
      if (v.ext == 2 && !v.s.empty()) return core::Value::Error(static_cast<core::Err>(static_cast<uint8_t>(v.s[0])));
      if ((v.ext == 4 || v.ext == 5) && v.s.size() == 8) {
        double d;
        std::memcpy(&d, v.s.data(), 8);
        *date_kind = v.ext == 4 ? 1 : 2;
        // Python sends a datetime as a 1900-system serial: the 1904 system's is 1462 days fewer (a duration stays)
        if (v.ext == 4) d -= formula::DateOffset();
        return core::Value::Number(d);
      }
      return core::Value();
    }
    default: return core::Value::String("");
  }
}

bool SameMV(const MV& a, const MV& b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case MV::T::Nil: return true;
    case MV::T::Bool: return a.b == b.b;
    case MV::T::Int: return a.i == b.i;
    case MV::T::Double: return a.d == b.d;
    case MV::T::Str:
    case MV::T::Bin: return a.s == b.s;
    case MV::T::Ext: return a.ext == b.ext && a.s == b.s;
    case MV::T::Array:
      if (a.a.size() != b.a.size()) return false;
      for (size_t i = 0; i < a.a.size(); ++i) {
        if (!SameMV(a.a[i], b.a[i])) return false;
      }
      return true;
    case MV::T::Map:
      if (a.m.size() != b.m.size()) return false;
      for (size_t i = 0; i < a.m.size(); ++i) {
        if (a.m[i].first != b.m[i].first || !SameMV(a.m[i].second, b.m[i].second)) return false;
      }
      return true;
  }
  return false;
}

MV StyleToMV(const model::StyleTable& t, const model::Style& s, const model::Theme& th) {
  MV m = MV::Map();
  m.Set("font_name", MV::Str(t.FontName(s)));
  m.Set("font_size", MV::Double(t.FontSize(s)));
  m.Set("bold", MV::Bool(s.bold));
  m.Set("italic", MV::Bool(s.italic));
  m.Set("underline", MV::Int(static_cast<int>(s.underline)));
  m.Set("strike", MV::Bool(s.strike));
  m.Set("font_color", MV::Int(model::ResolveColor(s.font_color, th)));
  m.Set("fill_color", MV::Int(model::ResolveColor(s.fill_color, th)));
  // theme colors: [index, tint] (nil = a fixed color)
  auto theme_of = [](uint32_t c) {
    if (!model::IsThemeColor(c)) return MV();
    MV a = MV::Array();
    a.Push(MV::Int(model::ThemeColorIndex(c)));
    a.Push(MV::Double(model::ThemeColorTint(c)));
    return a;
  };
  m.Set("font_theme", theme_of(s.font_color));
  m.Set("fill_theme", theme_of(s.fill_color));
  m.Set("cell_style", MV::Str(s.cell_style ? t.Str(s.cell_style) : std::string("Normal")));
  m.Set("pattern", MV::Int(static_cast<int>(s.pattern)));
  m.Set("pattern_color", MV::Int(s.pattern_color));
  m.Set("gradient", MV::Int(static_cast<int>(s.gradient)));
  m.Set("gradient_angle", MV::Int(s.gradient_angle));
  m.Set("gradient_color2", MV::Int(s.gradient_color2));
  m.Set("halign", MV::Int(static_cast<int>(s.halign)));
  m.Set("valign", MV::Int(static_cast<int>(s.valign)));
  m.Set("wrap", MV::Bool(s.wrap));
  m.Set("shrink", MV::Bool(s.shrink));
  m.Set("indent", MV::Int(s.indent));
  m.Set("rotation", MV::Int(s.rotation));
  m.Set("script", MV::Int(s.script));
  m.Set("numfmt", MV::Str(t.NumberFormat(s)));
  m.Set("locked", MV::Bool(s.locked));
  m.Set("hidden", MV::Bool(s.hidden));
  m.Set("reading_order", MV::Int(s.reading_order));
  m.Set("justify_last_line", MV::Bool(s.justify_last_line));
  MV b = MV::Array();
  for (const auto& e : s.border) {
    MV x = MV::Array();
    x.Push(MV::Int(static_cast<int>(e.style)));
    x.Push(MV::Int(model::ResolveColor(e.color, th)));
    MV tref = MV();
    if (model::IsThemeColor(e.color)) {
      tref = MV::Array();
      tref.Push(MV::Int(model::ThemeColorIndex(e.color)));
      tref.Push(MV::Double(model::ThemeColorTint(e.color)));
    }
    x.Push(std::move(tref));
    b.Push(std::move(x));
  }
  m.Set("borders", std::move(b));
  return m;
}

// Application.FindFormat / ReplaceFormat (a CellFormat's properties) as a format spec: the fields given are set
engine::FormatSpec FormatSpecOf(const MV* m) {
  engine::FormatSpec f;
  if (!m || m->IsNil()) return f;
  using namespace engine;
  model::Style& x = f.style;
  auto has = [&](const char* k) { return m->Has(k); };
  if (has("numfmt")) f.mask |= kFmNumber, f.numfmt = m->GetStr("numfmt");
  if (has("halign")) f.mask |= kFmHAlign, x.halign = static_cast<model::HAlign>(std::clamp<int64_t>(m->GetInt("halign"), 0, 7));
  if (has("valign")) f.mask |= kFmVAlign, x.valign = static_cast<model::VAlign>(std::clamp<int64_t>(m->GetInt("valign"), 0, 4));
  if (has("indent")) f.mask |= kFmIndent, x.indent = static_cast<uint8_t>(std::clamp<int64_t>(m->GetInt("indent"), 0, 15));
  if (has("wrap")) f.mask |= kFmWrap, x.wrap = m->GetBool("wrap");
  if (has("shrink")) f.mask |= kFmShrink, x.shrink = m->GetBool("shrink");
  if (has("rotation")) f.mask |= kFmRotation, x.rotation = static_cast<int16_t>(m->GetInt("rotation"));
  if (has("font_name")) f.mask |= kFmFontName, f.font = m->GetStr("font_name");
  if (has("font_size")) f.mask |= kFmFontSize, x.font_size = static_cast<float>(m->GetDouble("font_size"));
  if (has("bold")) f.mask |= kFmBold, x.bold = m->GetBool("bold");
  if (has("italic")) f.mask |= kFmItalic, x.italic = m->GetBool("italic");
  if (has("underline")) f.mask |= kFmUnderline, x.underline = static_cast<model::Underline>(std::clamp<int64_t>(m->GetInt("underline"), 0, 4));
  if (has("strike")) f.mask |= kFmStrike, x.strike = m->GetBool("strike");
  if (has("script")) f.mask |= kFmScript, x.script = static_cast<uint8_t>(std::clamp<int64_t>(m->GetInt("script"), 0, 2));
  if (has("font_color")) f.mask |= kFmFontColor, x.font_color = static_cast<uint32_t>(m->GetInt("font_color"));
  if (has("fill_color") || has("pattern") || has("pattern_color")) {
    f.mask |= kFmFill;
    x.fill_color = static_cast<uint32_t>(m->GetInt("fill_color"));
    x.pattern = static_cast<model::FillPattern>(std::clamp<int64_t>(m->GetInt("pattern"), 0, static_cast<int64_t>(model::FillPattern::Count) - 1));
    x.pattern_color = static_cast<uint32_t>(m->GetInt("pattern_color"));
  }
  if (has("locked")) f.mask |= kFmLocked, x.locked = m->GetBool("locked");
  if (has("hidden")) f.mask |= kFmHidden, x.hidden = m->GetBool("hidden");
  if (const MV* b = m->Get("borders"); b && b->t == MV::T::Map) {
    static const std::pair<const char*, int> kEdges[] = {{"left", model::kLeft}, {"top", model::kTop}, {"right", model::kRight}, {"bottom", model::kBottom},
                                                        {"diag_down", model::kDiagDown}, {"diag_up", model::kDiagUp}};
    for (const auto& [name, e] : kEdges) {
      const MV* side = b->Get(name);
      if (!side || side->a.size() < 2) continue;
      f.mask |= kFmBorderLeft << e;
      x.border[e] = model::BorderSide{static_cast<model::LineStyle>(std::clamp<int64_t>(side->a[0].i, 0, 13)), static_cast<uint32_t>(side->a[1].i)};
    }
  }
  return f;
}

// Python style properties onto a style (font_id / numfmt_id interned by the caller)
void ApplyStyleProps(model::Style& x, const MV& pr, uint32_t font_id, bool has_numfmt, uint32_t numfmt_id) {
  if (pr.Has("font_name")) x.font_name = font_id;
  if (pr.Has("font_size")) x.font_size = static_cast<float>(pr.GetDouble("font_size"));
  if (pr.Has("bold")) x.bold = pr.GetBool("bold");
  if (pr.Has("italic")) x.italic = pr.GetBool("italic");
  if (pr.Has("underline")) x.underline = static_cast<model::Underline>(pr.GetInt("underline"));
  if (pr.Has("strike")) x.strike = pr.GetBool("strike");
  if (pr.Has("font_color")) x.font_color = static_cast<uint32_t>(pr.GetInt("font_color"));
  if (pr.Has("fill_color")) x.fill_color = static_cast<uint32_t>(pr.GetInt("fill_color"));
  // Interior.Pattern = xlSolid: a cell without a fill gets a white one (Excel: Pattern 1, Color 16777215), others keep theirs
  if (pr.Has("fill_solid") && pr.GetBool("fill_solid") && !x.fill_color) x.fill_color = 0xFFFFFFFFu;
  if (pr.Has("pattern"))
    x.pattern = static_cast<model::FillPattern>(std::clamp<int64_t>(pr.GetInt("pattern"), 0, static_cast<int64_t>(model::FillPattern::Count) - 1));
  if (pr.Has("pattern_color")) x.pattern_color = static_cast<uint32_t>(pr.GetInt("pattern_color"));
  if (pr.Has("gradient")) x.gradient = static_cast<model::FillGradient>(std::clamp<int64_t>(pr.GetInt("gradient"), 0, 3));
  if (pr.Has("gradient_angle")) x.gradient_angle = static_cast<int16_t>(pr.GetInt("gradient_angle"));
  if (pr.Has("gradient_color2")) x.gradient_color2 = static_cast<uint32_t>(pr.GetInt("gradient_color2"));
  if (pr.Has("halign")) x.halign = static_cast<model::HAlign>(pr.GetInt("halign"));
  if (pr.Has("valign")) x.valign = static_cast<model::VAlign>(pr.GetInt("valign"));
  if (pr.Has("wrap")) x.wrap = pr.GetBool("wrap");
  if (pr.Has("shrink")) x.shrink = pr.GetBool("shrink");
  if (pr.Has("indent")) {
    x.indent = static_cast<uint8_t>(std::clamp<int64_t>(pr.GetInt("indent"), 0, 15));
    // Excel: an indent on a cell aligned otherwise than Left, Right or Distributed makes it Left (General, Center);
    // given to several cells at once (indent_block) it makes every one Left, Right ones too (Excel 16, measured
    // 2026-11-28: Range("A3:A4").IndentLevel = 1, InsertIndent)
    using H = model::HAlign;
    if (x.indent && !pr.Has("halign") && x.halign != H::Left &&
        (pr.GetBool("indent_block") || (x.halign != H::Right && x.halign != H::Distributed)))
      x.halign = H::Left;
  }
  if (pr.Has("rotation")) x.rotation = static_cast<int16_t>(pr.GetInt("rotation"));
  // a named style's borders (Style.Borders(...).LineStyle / Color)
  if (const MV* b = pr.Get("borders")) {
    const model::BorderSide side{static_cast<model::LineStyle>(b->GetInt("style")), static_cast<uint32_t>(b->GetInt("color"))};
    if (const MV* edges = b->Get("edges")) {
      for (const MV& e : edges->a) {
        const std::string n = e.s;
        const int k = n == "left" || n == "inside_v" ? model::kLeft : n == "top" || n == "inside_h" ? model::kTop : n == "right" ? model::kRight
                    : n == "bottom" ? model::kBottom : n == "diag_down" ? model::kDiagDown : n == "diag_up" ? model::kDiagUp : -1;
        if (k >= 0) x.border[k] = side;
      }
    }
  }
  if (pr.Has("script")) x.script = static_cast<uint8_t>(std::clamp<int64_t>(pr.GetInt("script"), 0, 2));
  if (has_numfmt) x.numfmt = numfmt_id;
  if (pr.Has("locked")) x.locked = pr.GetBool("locked");
  if (pr.Has("hidden")) x.hidden = pr.GetBool("hidden");
  if (pr.Has("reading_order")) x.reading_order = static_cast<uint8_t>(std::clamp<int64_t>(pr.GetInt("reading_order"), 0, 2));
  if (pr.Has("justify_last_line")) x.justify_last_line = pr.GetBool("justify_last_line");
}

std::wstring LocalAppDir() {
  wchar_t* local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) dir = std::wstring(local) + L"\\ycgrid";
  if (local) ::CoTaskMemFree(local);
  return dir;
}

std::string Err(std::string* code, std::string* msg, const char* c, std::string m) {
  *code = c;
  *msg = std::move(m);
  return {};
}

// Names: a Refers To that is a union written without parentheses (=Data!$A$1,Data!$A$3, which Excel takes) is kept as
// ((...)) (document_names.cpp, WrapTopUnion), read as the union everywhere, and shown without the two pairs again
bool UnionOfRefs(const formula::Node& n) {
  using formula::NK;
  if (n.kind == NK::Binary && n.op == ',') return n.kids.size() == 2 && UnionOfRefs(n.kids[0]) && UnionOfRefs(n.kids[1]);
  if (n.kind == NK::Paren) return n.kids.size() == 1 && UnionOfRefs(n.kids[0]);
  return n.kind == NK::Ref || n.kind == NK::Name || n.kind == NK::Func || n.kind == NK::StructRef ||
         (n.kind == NK::Binary && (n.op == ':' || n.op == ' '));
}

// the union a kept ((...)) text holds (nullptr for any other formula)
const formula::Node* KeptUnion(const formula::Node& root) {
  if (root.kind != formula::NK::Paren || root.kids.size() != 1 || root.kids[0].kind != formula::NK::Paren || root.kids[0].kids.size() != 1) return nullptr;
  const formula::Node& u = root.kids[0].kids[0];
  return u.kind == formula::NK::Binary && u.op == ',' ? &u : nullptr;
}

std::string WrapUnionText(const std::string& text, const formula::SheetResolver* sheets, bool r1c1) {
  std::string_view body = text;
  if (!body.empty() && body.front() == '=') body.remove_prefix(1);
  if (body.empty() || formula::Parse(body, core::CellPos{0, 0}, sheets, r1c1).ok) return text;
  const std::string inner = "(" + std::string(body) + ")";
  formula::ParseResult pr = formula::Parse(inner, core::CellPos{0, 0}, sheets, r1c1);
  if (!pr.ok || pr.root.kind != formula::NK::Paren || pr.root.kids.size() != 1) return text;
  const formula::Node& u = pr.root.kids[0];
  if (u.kind != formula::NK::Binary || u.op != ',' || !UnionOfRefs(u)) return text;
  return "=(" + (r1c1 ? formula::ToR1C1(pr.root, core::CellPos{0, 0}, sheets) : formula::ToA1(pr.root, core::CellPos{0, 0}, sheets)) + ")";  // written as Excel writes it ('T3'!$A$1)
}

// "=((A,B))" as kept -> "=A,B" as Excel shows it
std::string ShownUnionText(const std::string& text, const formula::SheetResolver* sheets) { return engine::ShownNameUnion(text, sheets); }

void FlattenUnion(const formula::Node& n, std::vector<const formula::Node*>* out) {
  if (n.kind == formula::NK::Binary && n.op == ',' && n.kids.size() == 2) {
    FlattenUnion(n.kids[0], out);
    FlattenUnion(n.kids[1], out);
  } else {
    out->push_back(&n);
  }
}

}  // namespace

// ---------------------------------------------------------------------------

PythonBridge::PythonBridge(AppState& s) : s_(s) {}

PythonBridge::~PythonBridge() {
  EndReplay(false);
  StopRecording();
  StopHost();
  ctl_stop_ = true;
  if (!ctl_pipe_name_.empty()) {
    // wake up the listener thread waiting for a connection
    HANDLE h = ::CreateFileW(ctl_pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
  if (ctl_thread_.joinable()) ctl_thread_.join();
  {
    std::lock_guard<std::mutex> lk(ctl_mu_);
    for (auto& t : ctl_workers_) {
      if (t.joinable()) t.detach();
    }
  }
  if (!instance_file_.empty()) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(Wide(instance_file_)), ec);
  }
  if (job_) ::CloseHandle(job_);
}

std::wstring PythonBridge::FindPython(const std::wstring& app_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  // 1) ycgrid.toml [python].interpreter
  const fs::path toml = fs::path(app_dir) / L"ycgrid.toml";
  if (fs::exists(toml, ec)) {
    std::ifstream f(toml);
    std::string line;
    while (std::getline(f, line)) {
      const auto p = line.find("interpreter");
      if (p == std::string::npos || line.find('=') == std::string::npos) continue;
      const auto q1 = line.find('"', line.find('=')), q2 = line.rfind('"');
      if (q1 != std::string::npos && q2 > q1) {
        fs::path ip = Wide(line.substr(q1 + 1, q2 - q1 - 1));
        if (ip.is_relative()) ip = fs::path(app_dir) / ip;
        if (fs::exists(ip, ec)) return ip.wstring();
      }
    }
  }
  // 1b) YCGRID_PYTHON: the Python that started this YCGrid (yc.launch, python -m yc run: a pip-installed YCGrid)
  {
    wchar_t env[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"YCGRID_PYTHON", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env) && fs::exists(env, ec)) return env;
  }
  // 2) project folder, 3) folder of ycgrid.exe, 4) search upwards for runtime\python (build\runtime during development)
  std::vector<fs::path> cands;
  if (!app_dir.empty()) cands.push_back(fs::path(app_dir) / L"runtime" / L"python" / L"python.exe");
  fs::path dir = app::shell::ExeDir();
  for (int i = 0; i < 6 && !dir.empty(); ++i) {
    cands.push_back(dir / L"runtime" / L"python" / L"python.exe");
    cands.push_back(dir / L"build" / L"runtime" / L"python" / L"python.exe");
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  for (const auto& c : cands) {
    if (fs::exists(c, ec)) return c.wstring();
  }
  // 5) installed with pip (<prefix>\Lib\site-packages\yc\_bin\ycgrid.exe): the Python of that installation
  const fs::path exe_dir = app::shell::ExeDir();
  if (exe_dir.filename() == L"_bin" && fs::exists(exe_dir.parent_path() / L"__init__.py", ec)) {
    const fs::path prefix = exe_dir.parent_path().parent_path().parent_path().parent_path();
    for (const fs::path& c : {prefix / L"python.exe", prefix / L"Scripts" / L"python.exe"}) {
      if (fs::exists(c, ec)) return c.wstring();
    }
  }
  return {};  // never fall back to some other Python found on the system (PLAN §13 (d))
}

std::wstring PythonBridge::FindYcPackage() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = app::shell::ExeDir();
  for (int i = 0; i < 6 && !dir.empty(); ++i) {
    if (fs::exists(dir / L"python" / L"yc" / L"__init__.py", ec)) return (dir / L"python").wstring();
    if (i > 0 && fs::exists(dir / L"yc" / L"__init__.py", ec)) return dir.wstring();  // pip: site-packages\yc\_bin\ycgrid.exe
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

// ---- Frames ----

bool PythonBridge::ReadFrame(Conn& c, std::string* out) {
  auto read_exact = [&](void* buf, DWORD n) {
    DWORD got = 0;
    while (got < n) {
      DWORD k = 0;
      if (!::ReadFile(c.read, static_cast<char*>(buf) + got, n - got, &k, nullptr) || k == 0) return false;
      got += k;
    }
    return true;
  };
  uint32_t len = 0;
  if (!read_exact(&len, 4)) return false;
  if (len > (512u << 20)) return false;
  out->resize(len);
  return len == 0 || read_exact(out->data(), len);
}

bool PythonBridge::WriteFrame(Conn& c, const std::string& data) {
  std::lock_guard<std::mutex> lk(c.wmu);
  const uint32_t len = static_cast<uint32_t>(data.size());
  std::string buf(4 + data.size(), '\0');
  std::memcpy(buf.data(), &len, 4);
  std::memcpy(buf.data() + 4, data.data(), data.size());
  DWORD put = 0;
  while (put < buf.size()) {
    DWORD k = 0;
    if (!::WriteFile(c.write, buf.data() + put, static_cast<DWORD>(buf.size() - put), &k, nullptr)) return false;
    put += k;
  }
  return true;
}

void PythonBridge::Reply(Conn& c, int64_t id, const MV* err_code, const std::string& err_msg, const MV& result) {
  if (id == 0) return;  // notify: no reply
  io::MsgWriter w;
  w.Array(4);
  w.Int(1);
  w.Int(id);
  if (err_code) {
    w.Array(2);
    io::Encode(w, *err_code);
    w.Str(err_msg);
  } else {
    w.Nil();
  }
  io::Encode(w, result);
  WriteFrame(c, w.buf());
}

void PythonBridge::Notify(const std::string& method, const MV& params) {
  io::MsgWriter w;
  w.Array(3);
  w.Int(2);
  w.Str(method);
  io::Encode(w, params);
  std::shared_ptr<Conn> h = Host();
  if (h && host_ready_) {
    if (!WriteFrame(*h, w.buf())) host_ready_ = false;
  } else {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (pending_notes_.size() < 10000) pending_notes_.push_back(w.buf());
  }
}

void PythonBridge::Log(std::string text, int kind, bool report) {
  if (kind == 1 && report) ReportErrorStream("Python", text);  // the Errors window and the log file
  std::lock_guard<std::mutex> lk(state_mu_);
  size_t start = 0;
  while (true) {
    const size_t nl = text.find('\n', start);
    console_.push_back({text.substr(start, nl == std::string::npos ? std::string::npos : nl - start), kind});
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (console_.size() > 3000) {
    const size_t drop = console_.size() - 3000;
    console_.erase(console_.begin(), console_.begin() + static_cast<ptrdiff_t>(drop));
    console_base_ += drop;
  }
  ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
}

std::vector<ConsoleLine> PythonBridge::ConsoleSnapshot() {
  std::lock_guard<std::mutex> lk(state_mu_);
  return console_;
}
void PythonBridge::ClearConsole() {
  std::lock_guard<std::mutex> lk(state_mu_);
  console_.clear();
}
std::vector<std::string> PythonBridge::Macros() {
  std::lock_guard<std::mutex> lk(state_mu_);
  return macros_;
}
std::vector<std::string> PythonBridge::UdfNames() {
  std::lock_guard<std::mutex> lk(state_mu_);
  std::vector<std::string> out;
  if (const MV* u = host_info_.Get("udfs")) {
    for (const MV& x : u->a) out.push_back(x.s);
  }
  return out;
}
void PythonBridge::TakeUdfHelp(const io::MV& help) {
  if (help.t != io::MV::T::Map) return;
  for (const auto& [name, h] : help.m) {
    UdfHelp& u = udf_help_[name];
    u.args = h.GetStr("args");
    u.desc = h.GetStr("desc");
    u.per_cell = h.GetBool("per_cell");
  }
}

void PythonBridge::PublishUdfHelp() {
  std::vector<formula::UserFunc> list;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    std::vector<std::string> names;
    if (const io::MV* u = host_info_.Get("udfs")) {
      for (const io::MV& x : u->a) names.push_back(x.s);
    }
    for (const auto& [n, h] : udf_help_) {
      if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);  // lesson-ok: find-push the @udf names of one project (a few dozen)
    }
    for (const std::string& n : names) {
      if (n == "PY") continue;  // =PY is YCGrid's own (it has its help among the built-in functions)
      formula::UserFunc f;
      f.name = n;
      if (auto it = udf_help_.find(n); it != udf_help_.end()) f.args = it->second.args, f.desc = it->second.desc;
      if (auto it = udf_options_.find(n); it != udf_options_.end()) {
        if (it->second.has_desc) f.desc = it->second.desc;
        if (it->second.has_cat) f.category = it->second.category;
        if (it->second.has_args) f.arg_help = it->second.arg_help;
      }
      list.push_back(std::move(f));
    }
  }
  formula::SetUserFuncHelp(std::move(list));
  // the functions that read their calling cell are called for each cell (Application.Caller, ThisCell)
  std::unordered_set<std::string> per_cell;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    for (const auto& [n, h] : udf_help_) {
      if (h.per_cell) per_cell.insert(n);
    }
  }
  if (s_.engine) s_.engine->Post([per_cell = std::move(per_cell)](Document& d) { d.per_cell_udfs = per_cell; });
}

std::vector<std::string> PythonBridge::MissingPackages() {
  std::lock_guard<std::mutex> lk(state_mu_);
  return missing_dismissed_ ? std::vector<std::string>{} : missing_;
}
PythonBridge::PackageBarState PythonBridge::PackageBar() {
  std::lock_guard<std::mutex> lk(state_mu_);
  PackageBarState b;
  if (!missing_dismissed_) b.packages = missing_;
  b.installing = installing_ && !b.packages.empty();
  b.error = install_error_;
  return b;
}
void PythonBridge::DismissMissing() {
  std::lock_guard<std::mutex> lk(state_mu_);
  missing_dismissed_ = true;
  installing_ = false;
  install_error_.clear();
  retry_code_.clear();
  retry_after_restart_ = false;
}
std::string PythonBridge::StatusText() {
  std::lock_guard<std::mutex> lk(state_mu_);
  if (!host_running_) return last_error.empty() ? std::string() : "Python:" + last_error;
  if (!host_ready_) return "Python: starting…";
  return "Python " + python_version_;
}

// ---- host ----

bool PythonBridge::StartHost(const std::wstring& app_dir, std::string* error) {
  host_crashed_ = false;
  last_pong_ = 0;
  StopHost(app_dir == app_dir_ ? "restart" : "close");  // the same project again: a restart; another one: this one closes
  app_dir_ = app_dir;
  python_exe_ = FindPython(app_dir);
  const std::wstring ycdir = FindYcPackage();
  if (python_exe_.empty()) {
    last_error = "YCGrid's Python runtime (runtime\\python) was not found. Run scripts\\setup_runtime.cmd or install a build that includes Python.";
    if (error) *error = last_error;
    return false;
  }
  if (ycdir.empty()) {
    last_error = "The yc package (python\\yc) was not found.";
    if (error) *error = last_error;
    return false;
  }
  if (std::error_code dir_ec; !std::filesystem::is_directory(app_dir, dir_ec)) {
    // Python starts in the project folder: a folder moved or deleted since would fail with ERROR_DIRECTORY (267)
    last_error = std::string(TR("The workbook's folder no longer exists: ")) + ShownPath(app_dir);
    if (error) *error = last_error;
    return false;
  }
  static int counter = 0;
  const std::wstring base = L"\\\\.\\pipe\\ycgrid." + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(++counter);
  const std::wstring c2p = base + L".c2p", p2c = base + L".p2c";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  HANDLE hw = ::CreateNamedPipeW(c2p.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20, 1 << 20, 0, &sa);
  HANDLE hr = ::CreateNamedPipeW(p2c.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20, 1 << 20, 0, &sa);
  if (hw == INVALID_HANDLE_VALUE || hr == INVALID_HANDLE_VALUE) {
    last_error = "Couldn't create the named pipe.";
    if (error) *error = last_error;
    if (hw != INVALID_HANDLE_VALUE) ::CloseHandle(hw);
    if (hr != INVALID_HANDLE_VALUE) ::CloseHandle(hr);
    return false;
  }
  // command line: -E -s (ignore PYTHON* environment variables and user site-packages), -X utf8
  std::wstring script = L"import sys;sys.path.insert(0,r'" + ycdir + L"');import yc.host;sys.exit(yc.host.main())";
  std::wstring cmd = L"\"" + python_exe_ + L"\" -E -s -X utf8 -c \"" + script + L"\" --pipe-in \"" + c2p + L"\" --pipe-out \"" + p2c + L"\" --app-dir \"" + app_dir + L"\"";
  // environment: clear PYTHONHOME, PYTHONPATH, VIRTUAL_ENV, CONDA_* (child processes such as pip are not affected either)
  std::wstring envblock;
  if (LPWCH env = ::GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
      std::wstring kv = p;
      std::wstring key = kv.substr(0, kv.find(L'='));
      std::wstring up = key;
      for (auto& ch : up) ch = static_cast<wchar_t>(::towupper(ch));
      if (up.rfind(L"PYTHON", 0) == 0 || up == L"VIRTUAL_ENV" || up.rfind(L"CONDA", 0) == 0) continue;
      envblock += kv;
      envblock.push_back(L'\0');
    }
    ::FreeEnvironmentStringsW(env);
  }
  envblock += L"PYTHONIOENCODING=utf-8";
  envblock.push_back(L'\0');
  envblock.push_back(L'\0');
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
  cmdbuf.push_back(0);
  if (!::CreateProcessW(python_exe_.c_str(), cmdbuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, envblock.data(),
                        app_dir.c_str(), &si, &pi)) {
    last_error = "Couldn't start Python (" + std::to_string(::GetLastError()) + ").";
    if (error) *error = last_error;
    ::CloseHandle(hw);
    ::CloseHandle(hr);
    return false;
  }
  ::CloseHandle(pi.hThread);
  // Job object: the host ends together with YCGrid
  if (!job_) {
    job_ = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
  }
  ::AssignProcessToJobObject(job_, pi.hProcess);
  host_process_ = pi.hProcess;
  // wait until both pipes are connected (while watching whether the process has exited)
  for (HANDLE h : {hw, hr}) {
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = ::ConnectNamedPipe(h, &ov) != 0;
    if (!ok) {
      const DWORD e = ::GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        ok = true;
      } else if (e == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, pi.hProcess};
        ok = ::WaitForMultipleObjects(2, waits, FALSE, 20000) == WAIT_OBJECT_0;
      }
    }
    ::CloseHandle(ov.hEvent);
    if (!ok) {
      last_error = "The Python host didn't connect (failed to start or timed out).";
      if (error) *error = last_error;
      ::TerminateProcess(pi.hProcess, 1);
      ::CloseHandle(pi.hProcess);
      host_process_ = nullptr;
      ::CloseHandle(hw);
      ::CloseHandle(hr);
      return false;
    }
  }
  auto c = std::make_shared<Conn>();
  c->read = hr;
  c->write = hw;
  c->host = true;
  c->id = next_conn_++;
  {
    std::lock_guard<std::mutex> lk(host_mu_);
    host_ = c;
  }
  host_running_ = true;
  host_ready_ = false;
  stopping_ = false;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    missing_.clear();
    missing_dismissed_ = false;
    installing_ = false;
    install_error_.clear();
    macros_.clear();
    last_error.clear();
  }
  Log(std::string(TR("[YCGrid] Starting the Python host: ")) + ShownPath(app_dir), 2);
  host_thread_ = std::thread([this, c, proc = pi.hProcess] {
    ::SetThreadDescription(::GetCurrentThread(), L"yc-python-host");
    HostReaderLoop(c, proc);
  });
  s_.engine->SetProvider(this);
  StartFeeds();
  return true;
}

void PythonBridge::StopHost(const char* reason) {
  StopFeeds();
  if (!host_running_ && !host_thread_.joinable()) return;
  stopping_ = true;
  std::shared_ptr<Conn> c;
  {
    std::lock_guard<std::mutex> lk(host_mu_);
    c = host_;
  }
  const bool was_ready = host_ready_.exchange(false);  // from here on Notify queues instead of writing to a closing pipe
  if (c && was_ready) {
    // the host runs Workbook_BeforeClose (reason "close" only) and Workbook_HostShutdown (2-second limit), then exits
    io::MsgWriter w;
    w.Array(3);
    w.Int(2);
    w.Str("shutdown");
    w.Map(2);
    w.Str("reason");
    w.Str(reason);
    w.Str("before_close_done");  // the window already ran Workbook_BeforeClose (it could cancel there)
    w.Bool(before_close_done_.exchange(false));
    WriteFrame(*c, w.buf());
  }
  if (host_process_) {
    // Workbook_HostShutdown has 2 s; a margin for the rest of the exit
    if (::WaitForSingleObject(host_process_, 3000) != WAIT_OBJECT_0) ::TerminateProcess(host_process_, 0);
  }
  {
    std::lock_guard<std::mutex> lk(host_mu_);
    host_.reset();  // nobody writes to the handles once they are closed (Windows reuses handle values at once)
  }
  if (c) {
    ::CloseHandle(c->read);
    ::CloseHandle(c->write);
  }
  if (host_thread_.joinable()) host_thread_.join();
  if (host_process_) ::CloseHandle(host_process_);
  host_process_ = nullptr;
  host_running_ = false;
  host_ready_ = false;
  s_.engine->SetProvider(nullptr);
}

void PythonBridge::RestartHost() {
  const std::wstring dir = app_dir_;
  StopHost("restart");
  std::string err;
  if (!StartHost(dir, &err)) Log("[YCGrid]" + err, 1);
}

void PythonBridge::FeedTopicSubs(const std::vector<std::pair<std::string, core::Value>>& items, const std::vector<int64_t>* times) {
  const bool subs = topic_subs_.Active(), plots = PlotLiveWants();
  if (!subs && !plots) return;
  const int64_t now = TopicSubs::NowNs();
  std::vector<TopicSubs::Update> ups;
  std::vector<PlotLiveUpdate> live;
  if (subs) ups.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string& t = items[i].first;
    const int64_t at = times && i < times->size() && (*times)[i] ? (*times)[i] : now;
    std::string name = t.rfind("QUOTE|", 0) == 0 ? t.substr(6) : t;
    if (plots && items[i].second.type() == core::VType::Number) live.push_back({name, items[i].second.number(), at});
    if (subs) ups.push_back({std::move(name), ValueToMV(items[i].second), at});
  }
  if (subs) topic_subs_.Feed(ups);
  if (!live.empty()) PlotLiveFeed(live);
}

void PythonBridge::ConnClosed(const Conn& c) {
  if (ui_closed_ || !c.id) return;
  const int64_t id = c.id;
  std::lock_guard<std::mutex> ul(ui_mu_);
  ui_queue_.push_back([id](AppState&) { FiguresOwnerGone(id); });
  ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
}

void PythonBridge::HostReaderLoop(std::shared_ptr<Conn> c, HANDLE process) {
  struct Closed {
    PythonBridge* b;
    std::shared_ptr<Conn> c;
    ~Closed() {  // the host exited, crashed or was stopped: its figures and topic subscriptions go
      b->topic_subs_.Clear();
      b->ConnClosed(*c);
    }
  } closed{this, c};
  std::string frame;
  while (ReadFrame(*c, &frame)) {
    io::MsgReader r(frame);
    MV msg;
    if (!io::Decode(r, &msg) || msg.t != MV::T::Array || msg.a.size() < 4 || msg.a[0].i != 0) continue;
    const int64_t id = msg.a[1].i;
    const std::string method = msg.a[2].s;
    std::string code, err;
    MV result;
    try {
      // this request's own refusals (the user's and other connections' don't count)
      auto blocks = std::make_shared<std::atomic<uint32_t>>(0);
      engine::Engine::BlockScope scope(blocks);
      result = Dispatch(*c, method, msg.a[3], &code, &err);
      // a value written to part of an array formula is ignored without an error, as Excel's Range.Value does (a
      // clear, an insert or a delete there still raises, as in Excel)
      const bool array_write = s_.engine->block_reason() == static_cast<int>(engine::BlockReason::Array) && std::string_view(method) == "range.set";
      if (blocks->load() != 0 && !array_write) {  // refused by protection (also when the request reported its own error)
        code = "E_PROTECTED";
        err = RefusalText(s_.engine->block_reason());
      }
    } catch (const std::exception& e) {
      code = "E_FAIL";
      err = e.what();
    }
    if (code.empty()) {
      Reply(*c, id, nullptr, {}, result);
    } else {
      const MV cv = MV::Str(code);
      Reply(*c, id, &cv, err, MV::Nil());
    }
  }
  const bool expected = stopping_.load();
  host_ready_ = false;
  host_running_ = false;
  if (!expected) {
    host_crashed_ = true;
    DWORD exit_code = 0;
    ::WaitForSingleObject(process, 2000);
    ::GetExitCodeProcess(process, &exit_code);
    Log("[YCGrid] The Python host exited (code" + std::to_string(exit_code) + "). Saving your code reloads it automatically; or use Developer -> Reload.", 1);
  }
}

// ---- controller ----

void PythonBridge::StartListener() {
  ctl_pipe_name_ = L"\\\\.\\pipe\\ycgrid." + std::to_wstring(::GetCurrentProcessId()) + L".ctl";
  ctl_thread_ = std::thread([this] {
    ::SetThreadDescription(::GetCurrentThread(), L"yc-controller");
    ControllerLoop();
  });
  WriteInstanceFile();
}

void PythonBridge::NoteOpenFailed(const std::wstring& path, const std::string& error) {
  open_failed_ = ToUtf8(path);
  open_error_.clear();
  for (char ch : error) open_error_ += (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;  // one line of JSON
  WriteInstanceFile();
}

void PythonBridge::WriteInstanceFile() {
  const std::wstring dir = LocalAppDir() + L"\\instances";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::wstring path = dir + L"\\" + std::to_wstring(::GetCurrentProcessId()) + L".json";
  std::string app_dir = ToUtf8(s_.app_dir), pipe = ToUtf8(ctl_pipe_name_), file = startup_open_ ? std::string() : ToUtf8(s_.file_path),
              source = startup_open_ ? std::string() : ToUtf8(s_.opened_from);
  auto esc = [](std::string v) {
    std::string o;
    for (char ch : v) {
      if (ch == '\\' || ch == '"') o += '\\';
      o += ch;
    }
    return o;
  };
  const std::string text = "{\"pid\": " + std::to_string(::GetCurrentProcessId()) + ", \"pipe\": \"" + esc(pipe) + "\", \"app_dir\": \"" + esc(app_dir) +
                           "\", \"file\": \"" + esc(file) + "\", \"source\": \"" + esc(source) + "\"" +
                           (open_failed_.empty() ? std::string() : ", \"open_failed\": \"" + esc(open_failed_) + "\", \"open_error\": \"" + esc(open_error_) + "\"") +
                           (startup_open_ ? ", \"opening\": true" : "") + "}";  // opening: the file it started for is being read
  // Written beside and moved over the old file: a reader (yc.connect) never sees it empty or half written. The move
  // fails while a reader has the file open, so it is retried; the last resort is writing in place.
  const std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    f << text;
  }
  bool moved = false;
  for (int i = 0; i < 40 && !moved; ++i) {
    moved = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!moved) ::Sleep(5);
  }
  if (!moved) {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f << text;
    ::DeleteFileW(tmp.c_str());
  }
  instance_file_ = ToUtf8(path);
}

void PythonBridge::ControllerLoop() {
  while (!ctl_stop_) {
    HANDLE h = ::CreateNamedPipeW(ctl_pipe_name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                  1 << 20, 1 << 20, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const bool ok = ::ConnectNamedPipe(h, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
    if (ctl_stop_) {
      ::CloseHandle(h);
      return;
    }
    if (!ok) {
      ::CloseHandle(h);
      continue;
    }
    auto c = std::make_shared<Conn>();
    c->read = c->write = h;
    c->id = next_conn_++;
    ::GetNamedPipeClientProcessId(h, &c->pid);
    std::lock_guard<std::mutex> lk(ctl_mu_);
    ctl_workers_.emplace_back([this, c] {
      ServeController(c);
      ConnClosed(*c);
      ::CloseHandle(c->read);
    });
  }
}

void PythonBridge::ServeController(std::shared_ptr<Conn> c) {
  std::string frame;
  while (!ctl_stop_ && ReadFrame(*c, &frame)) {
    io::MsgReader r(frame);
    MV msg;
    if (!io::Decode(r, &msg) || msg.t != MV::T::Array) break;  // out of step: closing it beats a client waiting for ever
    if (msg.a.size() < 4) {
      if (msg.a.size() >= 2 && msg.a[1].t == MV::T::Int) {
        const MV cv = MV::Str("E_PROTO");
        Reply(*c, msg.a[1].i, &cv, "A request needs [0, id, method, params]", MV::Nil());
      }
      continue;
    }
    std::string code, err;
    MV result;
    try {
      // this request's own refusals (the user's and other connections' don't count)
      auto blocks = std::make_shared<std::atomic<uint32_t>>(0);
      engine::Engine::BlockScope scope(blocks);
      result = Dispatch(*c, msg.a[2].s, msg.a[3], &code, &err);
      // a value written to part of an array formula is ignored without an error, as Excel's Range.Value does (a
      // clear, an insert or a delete there still raises, as in Excel)
      const bool array_write = s_.engine->block_reason() == static_cast<int>(engine::BlockReason::Array) && std::string_view(msg.a[2].s) == "range.set";
      if (blocks->load() != 0 && !array_write) {  // refused by protection (also when the request reported its own error)
        code = "E_PROTECTED";
        err = RefusalText(s_.engine->block_reason());
      }
    } catch (const std::exception& e) {
      code = "E_FAIL";
      err = e.what();
    }
    if (code.empty()) {
      Reply(*c, msg.a[1].i, nullptr, {}, result);
    } else {
      const MV cv = MV::Str(code);
      Reply(*c, msg.a[1].i, &cv, err, MV::Nil());
    }
  }
}

// ---- UI queue ----

void PythonBridge::CloseUi() {
  ui_closed_ = true;
  std::deque<std::function<void(AppState&)>> q;
  {
    std::lock_guard<std::mutex> lk(ui_mu_);
    q.swap(ui_queue_);
  }
  // dropping the queued calls breaks their promises: the threads waiting on them get an error now
}

bool PythonBridge::HostStopping() const { return stopping_ && host_thread_.joinable() && std::this_thread::get_id() == host_thread_.get_id(); }

void PythonBridge::WaitUi(std::future<MV>& fut, int timeout_s) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  while (fut.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
    if (ui_closed_) throw std::runtime_error("YCGrid is closing");
    // the UI thread is in StopHost waiting to join this thread: waiting for it back would hold both for 30 s
    if (HostStopping()) throw std::runtime_error("The Python host is stopping");
    if (timeout_s > 0 && std::chrono::steady_clock::now() > end) throw std::runtime_error("The UI is not responding");
  }
}

io::MV PythonBridge::OnUi(std::function<MV(AppState&)> fn, bool sync) {
  if (ui_closed_) throw std::runtime_error("YCGrid is closing");
  if (HostStopping()) throw std::runtime_error("The Python host is stopping");
  auto prom = std::make_shared<std::promise<MV>>();
  auto fut = prom->get_future();
  // set to false when the caller stops waiting (timeout, stopping): the job then does nothing, since most jobs
  // capture the caller's locals by reference and that stack frame is gone
  auto alive = std::make_shared<std::atomic<bool>>(true);
  // the UI must see what this connection just changed (a sheet just added, cells just written): publish
  // everything queued first, and give the UI that snapshot (live changes are otherwise held back by the
  // publish interval, and Worksheets.Add followed by Activate found no such sheet yet)
  if (sync) s_.engine->Sync();
  {
    std::lock_guard<std::mutex> lk(ui_mu_);
    ui_queue_.push_back([this, prom, alive, sink = engine::Engine::CurrentBlockSink(), fn = std::move(fn)](AppState& s) {
      if (!alive->load()) return;
      engine::Engine::BlockScope scope(sink);  // the UI's commands for this request count its refusals
      struct InRequest {  // what the request runs answers it now: nothing is put off for the engine (DeferWhileEngineBusy)
        AppState& st;
        bool was;
        explicit InRequest(AppState& x) : st(x), was(x.in_request) { st.in_request = true; }
        ~InRequest() { st.in_request = was; }
      } in_request(s);
      try {
        s.snap = s.engine->Current();
        MV v = fn(s);
        ui_focus_sheet_ = s.focus_sheet;  // before replying: a request that activated a sheet is seen at once
        NoteActiveCell(s);
        prom->set_value(std::move(v));
      } catch (...) {
        prom->set_exception(std::current_exception());
      }
    });
  }
  ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
  try {
    WaitUi(fut, 30);
  } catch (...) {
    alive->store(false);
    throw;
  }
  return fut.get();
}

void PythonBridge::ProcessUiQueue() {
  std::deque<std::function<void(AppState&)>> q;
  {
    std::lock_guard<std::mutex> lk(ui_mu_);
    q.swap(ui_queue_);
  }
  for (auto& f : q) f(s_);
  ui_focus_sheet_ = s_.focus_sheet;
  NoteActiveCell(s_);
}

void PythonBridge::NoteActiveCell(AppState& s) {
  if (!s.snap || !s.snap->Sheet(s.focus_sheet)) return;
  const auto& sel = s.FocusView().sel;
  ui_active_cell_ = (uint64_t{sel.act_r} << 32) | sel.act_c;
}

// ---- UI -> host ----

void PythonBridge::SendEvent(const std::string& name, uint32_t sheet, const std::vector<Rect>* areas, const MV* extra) {
  MV p = (extra && extra->t == MV::T::Map) ? *extra : MV::Map();
  p.Set("name", MV::Str(name));
  if (sheet) p.Set("sheet", MV::Int(sheet));
  if (areas) {
    MV a = MV::Array();
    for (const Rect& r : *areas) {
      MV x = MV::Array();
      x.Push(MV::Int(r.r0));
      x.Push(MV::Int(r.c0));
      x.Push(MV::Int(r.r1 >= core::kMaxRows ? kRowEnd : r.r1));
      x.Push(MV::Int(r.c1));
      a.Push(std::move(x));
    }
    p.Set("areas", std::move(a));
  }
  Notify("event", p);
}

bool PythonBridge::WantsEvent(const std::string& name, uint32_t sheet) {
  if (!host_ready_) return false;
  std::lock_guard<std::mutex> lk(state_mu_);
  if (wanted_events_.count(name)) {
    // a sheet module's handler runs only for its own sheet (an older host sends no list: any sheet)
    const auto it = sheet_events_.find(name);
    if (!sheet || it == sheet_events_.end() || it->second.count(sheet)) return true;
  }
  // a sheet event also runs main.py's workbook-level one: Worksheet_Calculate -> Workbook_SheetCalculate
  if (name.rfind("Worksheet_", 0) == 0) return wanted_events_.count("Workbook_Sheet" + name.substr(10)) != 0;
  return false;
}

bool PythonBridge::AskEvent(const std::string& name, uint32_t sheet, const std::vector<Rect>* areas, MV extra, std::function<void(AppState&, bool)> done,
                            int timeout_ms) {
  if (!WantsEvent(name, sheet)) return false;
  int64_t id;
  {
    std::lock_guard<std::mutex> lk(ask_mu_);
    id = next_ask_++;
    asks_[id] = PendingAsk{std::move(done), std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms), name};
  }
  if (extra.t != MV::T::Map) extra = MV::Map();
  extra.Set("ask", MV::Int(id));
  SendEvent(name, sheet, areas, &extra);
  ui::RequestRedrawIn(0.05);  // TickAsks looks at the deadline even when nothing else happens
  ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);  // (asked from another thread: wake the UI so it starts counting)
  return true;
}

bool PythonBridge::AsksPending() {
  std::lock_guard<std::mutex> lk(ask_mu_);
  return !asks_.empty();
}

void PythonBridge::TickAsks() {
  std::vector<std::pair<std::string, std::function<void(AppState&, bool)>>> late;
  {
    std::lock_guard<std::mutex> lk(ask_mu_);
    if (asks_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    const bool gone = !host_ready_;  // the host stopped or restarted: nobody will answer
    for (auto it = asks_.begin(); it != asks_.end();) {
      if (gone || now >= it->second.deadline) {
        late.emplace_back(it->second.name, std::move(it->second.done));
        it = asks_.erase(it);
      } else {
        ++it;
      }
    }
    if (!asks_.empty()) ui::RequestRedrawIn(0.05);
  }
  for (auto& [name, done] : late) {
    if (host_ready_) Log("[YCGrid] " + name + TR(" didn't answer in time; YCGrid went on as if it wasn't cancelled"), 1);
    if (done) done(s_, false);
  }
}

void PythonBridge::OnSheetEvents(const std::vector<uint32_t>& added, const std::vector<uint32_t>& calculated) {
  if (!host_ready_) return;
  if (!added.empty() && WantsEvent("Workbook_NewSheet")) {
    for (uint32_t id : added) SendEvent("Workbook_NewSheet", id);
  }
  if (!calculated.empty() && WantsEvent("Worksheet_Calculate")) {
    for (uint32_t id : calculated) {
      if (WantsEvent("Worksheet_Calculate", id)) SendEvent("Worksheet_Calculate", id);
    }
  }
}

void PythonBridge::RunMacro(const std::string& name) {
  MV p = MV::Map();
  p.Set("name", MV::Str(name));
  Notify("macro.run", p);
}

void PythonBridge::InstallPackages(bool always) {
  MV p = MV::Map();
  MV pk = MV::Array();
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    for (const auto& m : missing_) pk.Push(MV::Str(m));
    // the bar stays, saying "Installing..." until the host restarts with them (or says why it couldn't)
    installing_ = true;
    install_error_.clear();
    retry_after_restart_ = !retry_code_.empty();
  }
  p.Set("packages", std::move(pk));
  p.Set("always", MV::Bool(always));
  Notify("env.install", p);
  Log("[YCGrid] Installing packages… (your code keeps running meanwhile)", 2);
}

void PythonBridge::ConsoleEval(const std::string& code, bool shown) {
  MV p = MV::Map();
  p.Set("code", MV::Str(code));
  p.Set("shown", MV::Bool(shown));
  Notify("eval", p);
}

void PythonBridge::ControlEvent(uint32_t sheet, const std::string& name, const std::string& event, const std::string& on_action, bool panel) {
  MV p = MV::Map();
  if (panel) p.Set("panel", MV::Bool(true));
  p.Set("sheet", MV::Int(sheet));
  p.Set("name", MV::Str(name));
  p.Set("event", MV::Str(event));
  if (!on_action.empty()) p.Set("on_action", MV::Str(on_action));
  Notify("control", p);
}

// ---- ExternalProvider (engine thread) ----

void PythonBridge::RequestUdf(const std::string& key, const std::string& name, const std::vector<std::shared_ptr<formula::Array>>& args, uint32_t sheet,
                              uint32_t at_row, uint32_t at_col) {
  MV p = MV::Map();
  p.Set("key", MV::Str(key));
  p.Set("name", MV::Str(name));
  MV a = MV::Array();
  for (const auto& arr : args) {
    MV rows = MV::Array();
    for (uint32_t r = 0; r < arr->rows; ++r) {
      MV row = MV::Array();
      for (uint32_t c = 0; c < arr->cols; ++c) row.Push(ValueToMV(arr->At(r, c)));
      rows.Push(std::move(row));
    }
    a.Push(std::move(rows));
  }
  p.Set("args", std::move(a));
  MV caller = MV::Array();  // Application.Caller / ThisCell in the function
  caller.Push(MV::Int(sheet));
  caller.Push(MV::Int(at_row));
  caller.Push(MV::Int(at_col));
  p.Set("caller", std::move(caller));
  Notify("udf.call", p);
}

void PythonBridge::StartHostForFormulas() {
  std::lock_guard<std::mutex> lk(ui_mu_);
  ui_queue_.push_back([](AppState& s) {
    // in a scratch folder without a project; not while the Trust Center keeps the workbook's code disabled (its =PY
    // gives #BLOCKED!: trust::AllowProject calculates those cells again)
    if (s.python && !s.python->HostRunning() && !trust::CodeDisabled()) RunPythonInHost(s, "pass");
  });
  ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
}

void PythonBridge::Subscribe(const std::string& topic, const std::vector<core::Value>& args) {
  MV p = MV::Map();
  p.Set("topic", MV::Str(topic));
  p.Set("name", MV::Str(topic.substr(0, topic.find('|'))));
  MV a = MV::Array();
  for (const auto& v : args) a.Push(ValueToMV(v));
  p.Set("args", std::move(a));
  Notify("stream.subscribe", p);
}

void PythonBridge::OnChanges(const std::vector<engine::CellChange>& changes, uint32_t) {
  if (!host_ready_) return;
  struct Group {
    uint32_t sheet;
    const char* source;
    std::vector<Rect> areas;
  };
  std::vector<Group> groups;
  const uint8_t wanted = event_sources_;
  for (const auto& ch : changes) {
    if (ch.source & engine::kSrcNoEvents) continue;
    const char* src = (ch.source & engine::kSrcUser) ? "user" : (ch.source & engine::kSrcPython) ? "python" : (ch.source & engine::kSrcLive) ? "live" : "calc";
    const uint8_t bit = (ch.source & engine::kSrcUser) ? engine::kSrcUser : (ch.source & engine::kSrcPython) ? engine::kSrcPython : (ch.source & engine::kSrcLive) ? engine::kSrcLive : engine::kSrcCalc;
    if (!(wanted & bit)) continue;
    Group* g = nullptr;
    for (auto& x : groups) {
      if (x.sheet == ch.sheet && std::strcmp(x.source, src) == 0) g = &x;
    }
    if (!g) {
      groups.push_back({ch.sheet, src, {}});
      g = &groups.back();
    }
    if (g->areas.size() < 512) g->areas.push_back(ch.rect);
    else g->areas.back() = g->areas.back().Union(ch.rect);
  }
  for (auto& g : groups) {
    core::TidyAreas(&g.areas);  // one Target as Excel gives it (no repeats, neighbours joined)
    MV p = MV::Map();
    p.Set("sheet", MV::Int(g.sheet));
    p.Set("source", MV::Str(g.source));
    MV a = MV::Array();
    for (const Rect& r : g.areas) {
      MV x = MV::Array();
      x.Push(MV::Int(r.r0));
      x.Push(MV::Int(r.c0));
      x.Push(MV::Int(r.r1 >= core::kMaxRows ? kRowEnd : r.r1));
      x.Push(MV::Int(r.c1));
      a.Push(std::move(x));
    }
    p.Set("areas", std::move(a));
    Notify("sheet.change", p);
  }
}

// ---------------------------------------------------------------------------
// request dispatch
// ---------------------------------------------------------------------------
MV PythonBridge::Dispatch(Conn& c, const std::string& m, const MV& p, std::string* ec, std::string* em) {
  auto& eng = *s_.engine;
  // everything Python sends is live data for the engine: its publishes are throttled by the publish interval
  engine::Engine::LiveScope live;
  auto rect4 = [](const Rect& r) {
    MV x = MV::Array();
    x.Push(MV::Int(r.r0));
    x.Push(MV::Int(r.c0));
    x.Push(MV::Int(r.r1));
    x.Push(MV::Int(r.c1));
    return x;
  };
  const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));

  // ---- Connection, notifications ----
  if (m == "hello") {
    if (p.GetStr("role") == "host") {
      std::lock_guard<std::mutex> lk(state_mu_);
      python_version_ = p.GetStr("python");
    }
    MV r = MV::Map();
    r.Set("app_version", MV::Str(YC_VERSION_STRING));
    r.Set("exe_dir", MV::Str(ToUtf8(app::shell::ExeDir())));
    r.Set("app_dir", MV::Str(ToUtf8(app_dir_)));
    r.Set("pid", MV::Int(::GetCurrentProcessId()));
    r.Set("hidden", MV::Bool(s_.headless));  // nobody can answer a dialog there: yc.connect shortens its timeout
    return r;
  }
  if (m == "log") {
    // "error_shown": an error the user sees where it was typed (the Immediate window): red in the console, not
    // repeated in the Error Log
    const std::string kind = p.GetStr("kind", "out");
    if (kind == "warning") {  // Python's warnings: in the console, and with the messages of the Error Log
      ReportError("Python", TR("Warning"), p.GetStr("text"), false);
      Log(p.GetStr("text"), 0, false);
      return {};
    }
    Log(p.GetStr("text"), kind == "error" || kind == "error_shown" ? 1 : 0, kind != "error_shown");
    return {};
  }
  if (m == "macro.run") {
    // run a macro of the Python host (scripts, data feeds, MCP); it runs asynchronously in the host's event loop
    if (!HostReady()) {
      *ec = "E_APP";
      *em = "The Python host isn't running";
      return {};
    }
    RunMacro(p.GetStr("name"));
    return MV::Bool(true);
  }
  if (m == "host.pong") {
    last_pong_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return {};
  }
  if (m == "host.opened") {
    host_opened_ = true;
    return {};
  }
  if (m == "app.functions") {  // the worksheet functions' names (Application.Sum / WorksheetFunction.Sum know them)
    MV a = MV::Array();
    for (const auto& n : formula::FunctionNames()) a.Push(MV::Str(n));
    return a;
  }
  if (m == "job.done") {
    std::shared_ptr<std::promise<MV>> prom;
    {
      std::lock_guard<std::mutex> lk(job_mu_);
      auto it = jobs_.find(p.GetInt("id"));
      if (it != jobs_.end()) {
        prom = it->second;
        jobs_.erase(it);
      }
    }
    if (prom) prom->set_value(p);
    return {};
  }
  if (m == "host.ready") {
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      macros_.clear();
      if (const MV* a = p.Get("macros")) {
        for (const MV& x : a->a) macros_.push_back(x.s);
      }
      python_version_ = p.GetStr("python", python_version_);
      host_info_ = p;
      udf_help_.clear();
      if (const MV* h = p.Get("udf_help")) TakeUdfHelp(*h);
      wanted_events_.clear();
      if (const MV* a = p.Get("events")) {
        for (const MV& x : a->a) wanted_events_.insert(x.s);
      }
      // the sheets whose module has each Worksheet_* handler: a double-click on another sheet doesn't wait for the host
      sheet_events_.clear();
      if (const MV* se = p.Get("sheet_events"); se && se->t == MV::T::Map) {
        for (const auto& [name, ids] : se->m) {
          auto& set = sheet_events_[name];
          for (const MV& x : ids.a) set.insert(static_cast<uint32_t>(x.i));
        }
      }
    }
    PublishUdfHelp();  // Insert Function lists the @udf functions
    host_opened_ = false;
    host_ready_ = true;
    std::vector<std::string> notes;
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      notes.swap(pending_notes_);
    }
    for (const auto& n : notes) WriteFrame(c, n);
    Log(std::string(TR("[YCGrid] Python ready: ")) + python_version_ + " (" + TR("macros: ") + std::to_string(Macros().size()) + ")", 2);
    // UDF formulas may have returned #NAME? before the host was ready: calculate them again (only those calling a
    // function the host has)
    eng.Post([names = UdfNames()](Document& d) { d.RecalcUdfCells(&names); });
    std::string retry;
    bool retry_shown = true;
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      if (retry_after_restart_) {
        retry.swap(retry_code_);
        retry_shown = retry_shown_;
        retry_after_restart_ = false;
      }
    }
    if (!retry.empty()) ConsoleEval(retry, retry_shown);  // the package is installed now: what needed it runs again
    return {};
  }
  if (m == "host.restart") {
    // the host exits right after asking (packages installed, files added): its exit is expected from now, before
    // RestartHost has stopped the data feeds, or a project with feeds logged "The Python host exited (code0)"
    stopping_ = true;
    std::thread([this] { RestartHost(); }).detach();
    return {};
  }
  if (m == "env.install_failed") {  // the packages couldn't be installed: the bar comes back with the reason
    std::string list;
    if (const MV* a = p.Get("packages")) {
      for (const MV& x : a->a) list += (list.empty() ? "" : ", ") + x.s;
    }
    const std::string reason = p.GetStr("reason");
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      installing_ = false;
      install_error_ = reason.empty() ? std::string(TR("the installation failed")) : reason;
      missing_dismissed_ = false;
      retry_after_restart_ = false;
    }
    Log(std::string(TR("[YCGrid] Couldn't install the Python packages ")) + list + ": " + reason, 1);
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
    return {};
  }
  if (m == "python.error") {
    const std::string msg = p.GetStr("where") + ":" + p.GetStr("message");
    OnUi([msg](AppState& s) {
      s.SetStatus(TR("Python error (") + msg + TR(") - see View > Error Log"), 10.0);
      return MV();
    });
    return {};
  }
  if (m == "env.missing") {
    std::vector<std::string> pk;
    if (const MV* a = p.Get("packages")) {
      for (const MV& x : a->a) pk.push_back(x.s);
    }
    const std::string policy = p.GetStr("policy", "ask");
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      missing_ = pk;
      missing_dismissed_ = policy == "never";
      retry_code_ = p.GetStr("retry");  // empty when the project's own imports failed (the restart loads them again)
      retry_shown_ = p.GetBool("shown", true);
      retry_after_restart_ = false;
    }
    std::string list;
    for (const auto& x : pk) list += (list.empty() ? "" : ", ") + x;
    Log("[YCGrid] Missing packages: " + list, 1, /*report=*/false);  // the bar shows it; not an error of its own
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      install_error_.clear();
    }
    if (policy == "always") InstallPackages(false);
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
    return {};
  }
  if (m == "events.sources") {
    event_sources_ = static_cast<uint8_t>(engine::kSrcUser | engine::kSrcPython | (p.GetInt("mask") & 0x0F));
    return {};
  }
  if (m == "event.answer") {  // a Before* event's handlers ran: go on with the action unless one set Cancel
    std::function<void(AppState&, bool)> done;
    {
      std::lock_guard<std::mutex> lk(ask_mu_);
      auto it = asks_.find(p.GetInt("id"));
      if (it == asks_.end()) return {};  // too late: YCGrid already went on
      done = std::move(it->second.done);
      asks_.erase(it);
    }
    const bool cancel = p.GetBool("cancel");
    if (done && !ui_closed_) {
      std::lock_guard<std::mutex> lk(ui_mu_);
      ui_queue_.push_back([done = std::move(done), cancel](AppState& s) { done(s, cancel); });
      ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
    }
    return {};
  }
  if (m == "udf.result") {
    int dk;
    const std::string key = p.GetStr("key");
    if (const MV* arr = p.Get("array"); arr && arr->t == MV::T::Array && !arr->a.empty()) {  // spills
      auto a = std::make_shared<formula::Array>();
      a->rows = static_cast<uint32_t>(arr->a.size());
      a->cols = 0;
      for (const MV& row : arr->a) a->cols = std::max<uint32_t>(a->cols, static_cast<uint32_t>(row.a.size()));
      a->v.resize(static_cast<size_t>(a->rows) * a->cols);
      for (uint32_t r = 0; r < a->rows; ++r) {
        const auto& row = arr->a[r].a;
        for (uint32_t k = 0; k < row.size(); ++k) a->At(r, k) = MVToValue(row[k], &dk);
      }
      eng.Post([key, a](Document& d) { d.SetUdfArrayResult(key, a); });
      return {};
    }
    const core::Value v = MVToValue(p.Get("value") ? *p.Get("value") : MV::Nil(), &dk);
    eng.Post([key, v](Document& d) { d.SetUdfResult(key, v); });
    return {};
  }
  if (m == "udf.register") {
    if (const MV* h = p.Get("help")) {
      {
        std::lock_guard<std::mutex> lk(state_mu_);
        TakeUdfHelp(*h);
      }
      PublishUdfHelp();
    }
    eng.Post([](Document& d) { d.RecalcUdfCells(); });
    return {};
  }
  if (m == "udf.options") {
    // Application.MacroOptions: an @udf's description, category and argument descriptions for Insert Function
    std::string name = p.GetStr("name");
    for (char& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      UdfHelp& o = udf_options_[name];
      if (const MV* d = p.Get("description"); d && d->t == MV::T::Str) o.desc = d->s, o.has_desc = true;
      if (const MV* cat = p.Get("category"); cat && cat->t == MV::T::Str) o.category = cat->s, o.has_cat = true;
      if (const MV* a = p.Get("args"); a && a->t == MV::T::Array) {
        o.arg_help.clear();
        std::vector<std::string> arg_names;
        if (auto it = udf_help_.find(name); it != udf_help_.end()) {
          // the descriptions go to the arguments in their order (Excel's ArgumentDescriptions)
          std::string list = it->second.args, cur;
          for (char ch : list + ",") {
            if (ch == ',') {
              while (!cur.empty() && (cur.front() == ' ' || cur.front() == '[')) cur.erase(cur.begin());
              while (!cur.empty() && (cur.back() == ' ' || cur.back() == ']')) cur.pop_back();
              if (!cur.empty()) arg_names.push_back(cur);
              cur.clear();
            } else {
              cur += ch;
            }
          }
        }
        for (size_t i = 0; i < a->a.size() && i < arg_names.size(); ++i) o.arg_help.emplace_back(arg_names[i], a->a[i].s);
        o.has_args = true;
      }
    }
    PublishUdfHelp();
    return MV::Bool(true);
  }
  if (m.rfind("data.", 0) == 0 || m.rfind("tablestyle.", 0) == 0 || m.rfind("bind.", 0) == 0 || m.rfind("safety.", 0) == 0 || m.rfind("alerts.", 0) == 0 || m == "ui.onkey" ||
      m.rfind("ui.click_range", 0) == 0 || m == "ui.cell_context" || m.rfind("plot.", 0) == 0 || m.rfind("replay.", 0) == 0 || m == "table.read" || m == "range.read" || m == "range.write_frame" || m == "analysis.run") {
    bool handled = false;
    MV r = DispatchData(c, m, p, ec, em, &handled);
    if (handled) return r;
  }
  if (m == "stream.push") {
    std::vector<std::pair<std::string, core::Value>> items;
    std::vector<int64_t> times;  // [YC] Topics.Push(..., TimeNs=): the source's time of each update (0 = now)
    if (const MV* a = p.Get("items")) {
      for (const MV& it : a->a) {
        if (it.a.size() < 2) continue;
        int dk;
        items.emplace_back(it.a[0].s, MVToValue(it.a[1], &dk));
        times.push_back(it.a.size() > 2 && it.a[2].IsNum() ? (it.a[2].t == MV::T::Int ? it.a[2].i : static_cast<int64_t>(it.a[2].d)) : 0);
      }
    }
    const size_t pushed = items.size();
    TapeTopics(&items);  // recorded; those a replay owns are dropped
    if (items.empty()) return {};
    FeedTopicSubs(items, items.size() == pushed ? &times : nullptr);
    eng.Post([items = std::move(items)](Document& d) {
      for (const auto& [t, v] : items) d.UpdateTopic(t, v);
    });
    return {};
  }
  // [YC] Range.BindTopics / UnbindTopics / TopicIds, Topics.Stats
  if (m == "topic.bind" || m == "topic.unbind" || m == "topic.bound") {
    const Rect r = RectOf(*p.Get("rect"));
    if (r.Count() > 4000000) {
      *ec = "E_APP";
      *em = "The range is too large";
      return {};
    }
    std::vector<std::string> topics;
    if (const MV* t = p.Get("topics")) {
      for (const MV& row : t->a) {
        if (row.t == MV::T::Array) {
          for (const MV& x : row.a) topics.push_back(x.t == MV::T::Str ? x.s : std::string());
        } else {
          topics.push_back(row.t == MV::T::Str ? row.s : std::string());
        }
      }
    }
    return eng.Call([&](Document& d) {
      if (m == "topic.bind") {
        d.BindTopics(sheet, r, topics);
        return MV();
      }
      if (m == "topic.unbind") {
        d.UnbindTopics(sheet, r);
        return MV();
      }
      MV out = MV::Array();
      for (const auto& t : d.BoundTopics(sheet, r)) out.Push(MV::Str(t));
      return out;
    });
  }
  if (m == "topic.stats") {
    MV o = eng.Call([](Document& d) {
      const auto st = d.GetTopicStats();
      MV o = MV::Map();
      o.Set("updates", MV::Int(static_cast<int64_t>(st.updates)));
      o.Set("topics", MV::Int(static_cast<int64_t>(st.topics)));
      o.Set("bound_cells", MV::Int(static_cast<int64_t>(st.bound_cells)));
      return o;
    });
    o.Set("subscriptions", topic_subs_.Stats());
    return o;
  }
  // [YC] Application.Topics.Subscribe: every update of these topics, sent to the host in order ("topic.ticks")
  if (m == "topic.subscribe") {
    if (!c.host) {
      *ec = "E_APP";
      *em = "Topics.Subscribe runs in the YCGrid Python host (a module of the project); a script reads topics with =QUOTE() cells or Range.BindTopics";
      return {};
    }
    std::vector<std::string> names;
    if (const MV* t = p.Get("topics")) {
      for (const MV& x : t->a) {
        if (x.t == MV::T::Str && !x.s.empty()) names.push_back(x.s);
      }
    }
    TopicSubs::Options o;
    o.conflate = p.GetBool("conflate", false);
    o.capacity = static_cast<size_t>(std::max<int64_t>(1, p.GetInt("capacity", 1 << 20)));
    o.max_batch = static_cast<size_t>(std::max<int64_t>(1, p.GetInt("max_batch", 1000)));
    return MV::Int(topic_subs_.Add(std::move(names), o));
  }
  if (m == "topic.unsubscribe") return MV::Bool(topic_subs_.Remove(p.GetInt("id", 0)));
  if (m == "debug.dump") {  // tests: what the workbook holds (io::ContentDump), to compare before and after an action
    eng.Sync();
    return eng.Call([&](Document& d) { return MV::Str(io::ContentDump(d.wb())); });
  }
  if (m == "debug.engine") {  // tests and triage: the sizes of the engine's indexes (Document::DebugCounts)
    return eng.Call([](Document& d) {
      MV o = MV::Map();
      for (const auto& [k, v] : d.DebugCounts()) o.Set(k, MV::Int(v));
      return o;
    });
  }
  if (m == "debug.ui_stall") {  // tests of the hang watchdog: keep the UI thread busy for `seconds`
    const int ms = static_cast<int>(std::clamp(p.GetDouble("seconds", 1), 0.0, 25.0) * 1000);
    OnUi([ms](AppState&) {
      ::Sleep(static_cast<DWORD>(ms));
      return MV();
    });
    return MV::Str(ToUtf8(DumpFolder()));
  }
  if (m == "debug.crash") {  // tests of the crash dump: a hidden instance only, its UI thread reads address 0
    if (!s_.headless) throw std::runtime_error("debug.crash runs in a hidden YCGrid only");
    {
      std::lock_guard<std::mutex> ul(ui_mu_);
      ui_queue_.push_back([](AppState&) {
        volatile int* p = nullptr;
        *p = 1;  // the crash this request exists for
      });
    }
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
    return MV::Str(ToUtf8(DumpFolder()));
  }
  if (m == "sync") {
    if (p.GetStr("wait") == "ui") OnUi([](AppState&) { return MV(); });
    else eng.Sync();
    return {};
  }

  // ---- Workbook ----
  if (m == "wb.info") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      const auto& wb = d.wb();
      MV r = MV::Map();
      r.Set("path", MV::Str(wb.path));
      r.Set("dirty", MV::Bool(wb.dirty));
      r.Set("read_only", MV::Bool(wb.read_only));
      MV sheets = MV::Array();
      for (size_t i = 0; i < wb.sheets.size(); ++i) {
        const auto& s = wb.sheets[i].get();
        MV x = MV::Map();
        x.Set("id", MV::Int(s.id));
        x.Set("name", MV::Str(s.name));
        x.Set("code_name", MV::Str(s.code_name));
        x.Set("index", MV::Int(static_cast<int64_t>(i) + 1));
        x.Set("visible", MV::Bool(s.visible));
        x.Set("very_hidden", MV::Bool(s.very_hidden));
        x.Set("row_limit", MV::Int(s.row_limit));
        x.Set("tab_color", MV::Int(model::IsThemeColor(s.tab_color) ? (0xFF000000u | model::ResolveColor(s.tab_color)) : s.tab_color));
        x.Set("standard_width", MV::Double(render::ColPxToChars(s.cols->default_size())));
        x.Set("chart", MV::Bool(s.chart_sheet));
        x.Set("used", s.has_used ? rect4(s.used) : MV::Nil());
        // Excel's Worksheet.CircularReference: one cell of a circular reference on this sheet
        for (const uint64_t k : d.circular_cells) {
          if (engine::KeySheet(k) != s.id) continue;
          MV cr = MV::Array();
          cr.Push(MV::Int(engine::KeyRow(k)));
          cr.Push(MV::Int(engine::KeyCol(k)));
          x.Set("circular", std::move(cr));
          break;
        }
        sheets.Push(std::move(x));
      }
      r.Set("sheets", std::move(sheets));
      return r;
    });
  }
  if (m == "wb.password") {
    const std::string pw = p.GetStr("password");
    eng.Call([&](Document& d) { d.SetOpenPassword(pw); });
    return MV::Bool(true);
  }
  if (m == "wb.has_password") {
    return eng.Call([](Document& d) { return MV::Bool(!d.wb().open_password.empty()); });
  }
  if (m == "wb.save") {
    const std::string path = p.GetStr("path");
    const bool copy = p.GetBool("copy");
    if (const MV* pw = p.Get("password")) {
      const std::string v = pw->s;
      eng.Call([&](Document& d) { d.SetOpenPassword(v); });
    }
    if (!copy && !c.host) {
      // a script's or an MCP tool's save runs Workbook_BeforeSave first (the host's own Save runs it in Python, since
      // the host can't answer while it waits for this reply); no answer in time counts as not cancelled
      auto prom = std::make_shared<std::promise<bool>>();
      auto fut = prom->get_future();
      MV extra = MV::Map();
      extra.Set("save_as_ui", MV::Bool(false));
      if (AskEvent("Workbook_BeforeSave", 0, nullptr, std::move(extra), [prom](AppState&, bool cancel) { prom->set_value(cancel); })) {
        if (fut.wait_for(std::chrono::seconds(4)) == std::future_status::ready && fut.get()) return MV::Bool(false);
      }
    }
    // Application.CalculateBeforeSave: a manual-calculation workbook is saved with calculated values (SaveCopyAs too)
    if (Config().calculate_before_save) {
      eng.Call([](Document& d) {
        if (!d.auto_calc) d.CalculateNow();
      });
    }
    eng.Sync();  // save what Python just wrote, not an older snapshot
    // a save that fails raises with its reason and shows no message box, as Excel's object model raises error 1004 (it
    // returned False, which no script read: SaveCopyAs to a name Windows refuses wrote nothing and reported nothing);
    // only Workbook_BeforeSave's Cancel comes back quietly, as False
    auto refused = [&path](std::string why) {
      // the message box's text names the file first ("<path>\n\n<reason>"): the reason alone goes after the name
      if (const size_t at = why.rfind("\n\n"); at != std::string::npos) why = why.substr(at + 2);
      return std::runtime_error("Can't save " + (path.empty() ? std::string("the workbook") : path) + ": " +
                                (why.empty() ? std::string("it wasn't saved") : why));
    };
    // known: the reason when it is known before the UI thread finishes the save (the file's write)
    auto on_ui_save = [&](std::function<bool(AppState&)> fn, const std::string& known = {}) {
      const MV r = OnUi([fn](AppState& s) {
        std::string said;
        s.quiet_error = &said;
        const bool ok = fn(s);
        s.quiet_error = nullptr;
        MV out = MV::Map();
        out.Set("ok", MV::Bool(ok));
        out.Set("error", MV::Str(said));
        return out;
      });
      if (!r.GetBool("ok")) throw refused(known.empty() ? r.GetStr("error") : known);
      return MV::Bool(true);
    };
    // a .ycg: the file is written here, off the UI thread, so live screens go on while a large workbook saves
    const std::wstring wpath = FromUtf8(path);
    if (!copy) {
      auto plan = std::make_shared<YcgSave>();
      // 1: prepared here; 0: the UI thread does it all (.xlsx / .csv, read-only, Save with no file yet: the dialog)
      const int64_t ready = OnUi([plan, wpath](AppState& s) {
        if (!s.engine || !s.snap) return MV::Int(0);
        FinishBackgroundSave(s, true);  // after a Save the user started
        const std::wstring target = wpath.empty() ? s.file_path : wpath;
        if (target.empty() || !IsYcgSavePath(target) || (wpath.empty() && s.snap->wb.read_only)) return MV::Int(0);
        *plan = PrepareYcgSave(s, target);
        return MV::Int(1);
      }).i;
      if (ready == 1) {
        auto err = std::make_shared<std::string>();
        const bool ok = WriteYcgSave(*plan, err.get());
        return on_ui_save([plan, ok, err](AppState& s) { return FinishYcgSave(s, *plan, ok, *err); }, ok ? std::string() : *err);
      }
    }
    if (copy) {  // SaveCopyAs: only the layout comes from the UI thread
      // an .xlsx or .csv name writes a copy in that format, as Save As does (it wrote a .ycg under that name); the file
      // is written here from the snapshot, off the UI thread (a CSV of a million rows held the screen for two seconds)
      if (!IsYcgSavePath(wpath)) {
        const std::filesystem::path fp(wpath);
        std::wstring ext = fp.extension().wstring();
        for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
        if (ext == L".csv" || ext == L".xlsx" || ext == L".xlsm") {
          const uint32_t shown = static_cast<uint32_t>(OnUi([](AppState& s) { return MV::Int(s.focus_sheet); }).i);
          std::error_code dir_ec;
          std::filesystem::create_directories(fp.parent_path(), dir_ec);
          std::string err;
          const auto snap = eng.Current();
          const bool ok = ext == L".csv" ? io::ExportCsv(*snap, shown, wpath, &err) : io::ExportXlsx(snap->wb, wpath, &err);
          if (!ok) throw refused(err);
          return MV::Bool(true);
        }
        return on_ui_save([wpath](AppState& s) { return SaveWorkbookTo(s, wpath); });
      }
      io::SaveOptions opt;
      opt.layout_json = OnUi([](AppState& s) { return MV::Str(CaptureLayoutJson(s)); }).s;
      opt.app_version = YC_VERSION_STRING;
      std::string err;
      std::error_code dir_ec;  // a folder that doesn't exist yet is made, as SaveAs does
      std::filesystem::create_directories(std::filesystem::path(wpath).parent_path(), dir_ec);
      if (!io::SaveYcg(eng.Current()->wb, wpath, opt, &err)) throw refused(err);
      return MV::Bool(true);
    }
    return on_ui_save([path](AppState& s) {
      // SaveAs: SaveWorkbookTo notices the new folder and switches to that folder's Python project
      if (!path.empty()) return SaveWorkbookTo(s, FromUtf8(path));
      return SaveWorkbook(s, false);
    });
  }
  if (m == "wb.mark_saved") {
    eng.Post([](Document& d) { d.wb_mut().dirty = false; });
    return {};
  }

  // ---- Sheets ----
  if (m == "sheet.add") {
    const std::string name = p.GetStr("name");
    const int32_t index = p.Has("index") ? static_cast<int32_t>(p.GetInt("index")) : -1;
    const uint32_t id = eng.Call([&](Document& d) {
      d.ClearUndo();
      return d.AddSheet(name, index);
    });
    MV r = MV::Map();
    r.Set("id", MV::Int(id));
    return r;
  }
  if (m == "sheet.standard_width") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    const int32_t px = render::CharsToColPx(std::clamp(p.GetDouble("chars"), 0.0, 255.0));
    eng.Call([&](Document& d) {
      d.SetStandardWidth(id, px);
      d.ForeignWrite();
    });
    return {};
  }
  if (m == "sheet.delete" || m == "sheet.rename" || m == "sheet.move" || m == "sheet.visible" || m == "sheet.tab_color" || m == "sheet.row_limit") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    std::string err;
    uint32_t next = UINT32_MAX;  // the sheet that becomes active if this one was: the next visible one, else the one before
    const bool ok = eng.Call([&](Document& d) {
      d.ClearUndo();
      if (!d.S(id)) {
        err = "The sheet no longer exists";
        return false;
      }
      if (m == "sheet.delete" || (m == "sheet.visible" && !p.GetBool("visible", true))) {
        const auto& sheets = d.wb().sheets;
        size_t at = 0;
        while (at < sheets.size() && sheets[at].get().id != id) ++at;
        for (size_t k = at + 1; k < sheets.size() && next == UINT32_MAX; ++k) {
          if (sheets[k].get().visible) next = sheets[k].get().id;
        }
        for (size_t k = at; k-- > 0 && next == UINT32_MAX;) {
          if (sheets[k].get().visible) next = sheets[k].get().id;
        }
      }
      if (m == "sheet.delete") {
        if (!d.DeleteSheet(id)) {
          err = "A workbook must contain at least one visible sheet";
          return false;
        }
        return true;
      }
      if (m == "sheet.rename") return d.RenameSheet(id, p.GetStr("name"), &err);
      if (m == "sheet.move") return d.MoveSheet(id, static_cast<int32_t>(p.GetInt("index")));
      if (m == "sheet.visible") {
        if (!d.SetSheetVisible(id, p.GetBool("visible", true), p.GetBool("very", false))) {
          err = "Unable to set the Visible property: a workbook must keep at least one visible sheet";
          return false;
        }
        return true;
      }
      if (m == "sheet.tab_color") {
        d.SetTabColor(id, static_cast<uint32_t>(p.GetInt("color")));
        return true;
      }
      d.SetRowLimit(id, static_cast<uint32_t>(p.GetInt("rows")));
      return true;
    });
    if (!ok) return Err(ec, em, "E_APP", err.empty() ? "The operation couldn't be completed" : err), MV();
    // deleting or hiding the active sheet activates its neighbour within this request, as Excel does at once (the
    // UI's next frame would pick one, and ActiveSheet read the deleted sheet until then)
    if (next != UINT32_MAX) {
      OnUi([&](AppState& s) {
        if (s.focus_sheet == id) ActivateSheet(s, next);
        else if (s.active_sheet == id && !s.Win(next).floating) s.active_sheet = next;
        return MV();
      });
    }
    return {};
  }
  if (m == "sheet.copy") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    const int32_t index = p.Has("index") ? static_cast<int32_t>(p.GetInt("index")) : -1;
    const uint32_t nid = eng.Call([&](Document& d) {
      d.ClearUndo();
      return d.CopySheet(id, index);
    });
    MV r = MV::Map();
    r.Set("id", MV::Int(nid));
    return r;
  }

  // ---- Range: read ----
  if (m == "range.get") {
    const Rect req = RectOf(*p.Get("rect"));
    const std::string what = p.GetStr("what", "value");
    MV out;
    std::string err;
    eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      const model::Sheet* s = d.S(sheet);
      if (!s) {
        err = "The sheet no longer exists";
        return;
      }
      Rect r = Clamp(*s, req);
      std::unique_ptr<engine::CondFormatEval> text_cf;  // Text: the conditional formats' number formats
      // reading entire columns / rows: only up to the used range
      if (req.r1 == kRowEnd || req.r1 >= s->row_limit) r.r1 = std::max(r.r0, s->has_used ? std::min(r.r1, s->used.r1) : r.r0);
      if (req.c1 >= s->col_limit - 1 && req.c0 == 0) r.c1 = std::max(r.c0, s->has_used ? std::min(r.c1, s->used.c1) : r.c0);
      if (r.Count() > 4000000) {
        err = "The range is too large (more than 4 million cells); read it in parts";
        return;
      }
      const auto& tbl = *d.wb().styles;
      MV rows = MV::Array();
      MV dates = MV::Array();
      rows.a.reserve(r.Rows());
      for (uint32_t rr = r.r0; rr <= r.r1; ++rr) {
        MV row = MV::Array();
        row.a.reserve(r.Cols());
        for (uint32_t cc = r.c0; cc <= r.c1; ++cc) {
          const core::Value& v = s->GetValue(rr, cc);
          if (what == "value" || what == "value2") {
            row.Push(ValueToMV(v));
            if (what == "value" && v.IsNumber()) {
              const uint32_t sid = s->GetStyle(rr, cc);
              if (sid && formula::IsDateFormat(tbl.NumberFormat(tbl.Get(sid)))) {
                MV x = MV::Array();
                x.Push(MV::Int(rr - r.r0));
                x.Push(MV::Int(cc - r.c0));
                dates.Push(std::move(x));
              }
            }
          } else if (what == "formula") {
            std::string f = d.GetFormulaA1(sheet, rr, cc);
            if (f.empty() && !s->data_tables->empty()) {
              // a data table cell: =TABLE(row_input,col_input), like Excel's Range.Formula
              if (std::string t = engine::DataTableText(*s, rr, cc); t.size() > 2) f = t.substr(1, t.size() - 2);
            }
            uint32_t ar = 0, ac = 0;
            Rect spill_rect;
            if (f.empty() && !v.IsEmpty() && d.SpillAt(sheet, rr, cc, &ar, &ac, &spill_rect) && (ar != rr || ac != cc)) {
              // a value a dynamic array spilled has no formula of its own (Excel: ""); every cell of an array entered
              // with Ctrl+Shift+Enter reads the array's formula (Excel 16, measured: cse_ cases)
              const model::SpillRange* sp = s->SpillAt(rr, cc, &ar, &ac);
              row.Push(MV::Str(sp && sp->cse ? d.GetFormulaA1(sheet, ar, ac) : std::string()));
            } else if (f.empty()) {
              if (v.IsNumber()) row.Push(MV::Double(v.number()));
              else row.Push(MV::Str(d.GetInputText(sheet, rr, cc)));
            } else {
              row.Push(MV::Str(f));
            }
          } else if (what == "text") {
            // what the cell shows: a conditional format's number format wins (Excel's Range.Text)
            std::string code = tbl.NumberFormat(tbl.Get(s->GetStyle(rr, cc)));
            if (!s->cond_formats->empty()) {
              if (!text_cf) text_cf = std::make_unique<engine::CondFormatEval>(d.wb(), sheet);
              engine::CfVisual cv;
              if (text_cf->Eval(rr, cc, &cv) && !cv.numfmt.empty()) code = cv.numfmt;
            }
            row.Push(MV::Str(engine::DisplayTextOf(v, code)));
          } else if (what == "numfmt") {
            row.Push(MV::Str(tbl.NumberFormat(tbl.Get(s->GetStyle(rr, cc)))));
          } else if (what == "has_formula") {
            row.Push(MV::Bool(s->GetFormula(rr, cc) != 0));
          } else if (what == "prefix") {  // Range.PrefixCharacter: ' when text was typed after an apostrophe
            const std::string in = d.GetInputText(sheet, rr, cc);
            row.Push(MV::Str(!in.empty() && in[0] == '\'' && v.IsString() && !s->GetFormula(rr, cc) ? "'" : ""));
          }
        }
        rows.Push(std::move(row));
      }
      out = MV::Map();
      out.Set("values", std::move(rows));
      if (!dates.a.empty()) {
        out.Set("dates", std::move(dates));
        out.Set("date_offset", MV::Double(formula::DateOffset()));  // Python reads the 1900 system's serials
      }
    });
    if (!err.empty()) return Err(ec, em, "E_APP", err), MV();
    return out;
  }
  if (m == "range.display_format") {
    // Range.DisplayFormat: the first cell's format as the sheet shows it, its conditional formats applied (the font's
    // color and style, the fill and its pattern, the number format)
    return eng.Call([&](Document& d) {
      d.SettleForRead();  // a formula written just before has its value, and the rules read it (Settle)
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      const auto& t = *d.wb().styles;
      const uint32_t r0 = static_cast<uint32_t>(p.GetInt("row")), c0 = static_cast<uint32_t>(p.GetInt("col"));
      model::Style st = t.Get(s->GetStyle(r0, c0));
      engine::CondFormatEval cf(d.wb(), sheet);
      engine::CfVisual v;
      std::string numfmt = t.NumberFormat(st);
      if (!cf.Empty() && cf.Eval(r0, c0, &v)) {
        if (v.has_font) st.font_color = v.font;
        if (v.has_fill) st.fill_color = v.fill;  // a solid fill, as the rule's Format... gives it
        if (v.bold >= 0) st.bold = v.bold != 0;
        if (v.italic >= 0) st.italic = v.italic != 0;
        if (v.strike >= 0) st.strike = v.strike != 0;
        if (v.underline >= 0) st.underline = static_cast<decltype(st.underline)>(v.underline);
        if (v.pattern >= 0) st.pattern = static_cast<decltype(st.pattern)>(v.pattern), st.pattern_color = v.pattern_color;
        if (!v.numfmt.empty()) numfmt = v.numfmt;
      }
      MV out = StyleToMV(t, st, *d.wb().theme);
      out.Set("numfmt", MV::Str(numfmt));
      return out;
    });
  }
  if (m == "range.style_get") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      const auto& t = *d.wb().styles;
      const uint32_t r0 = static_cast<uint32_t>(p.GetInt("row")), c0 = static_cast<uint32_t>(p.GetInt("col"));
      const uint32_t first = s->GetStyle(r0, c0);
      MV out = StyleToMV(t, t.Get(first), *d.wb().theme);
      std::vector<std::string> mixed;
      auto note_mixed = [&](const char* k) {
        if (std::find(mixed.begin(), mixed.end(), k) == mixed.end()) mixed.emplace_back(k); // lesson-ok: find-push the names of a style's parts
      };
      // a cell whose characters have fonts of their own (Characters.Font): a font property they don't all share is
      // Excel's Null; one they all share is read from them (the first cell's value; other cells compare below)
      auto rich_fonts = [&](uint32_t r, uint32_t c, MV* into) {
        const model::RichText* rt = d.RichTextAt(sheet, r, c);
        if (!rt || rt->runs.empty()) return false;
        const model::Style& st = t.Get(s->GetStyle(r, c));
        const size_t n = model::CharCount(rt->text);
        std::vector<model::RunFont> fonts;
        for (const model::RichRun& run : rt->runs) {
          if (run.start < n) fonts.push_back(model::ResolveRunFont(t, st, &run));
        }
        if (fonts.empty()) return false;
        const model::RunFont& f0 = fonts[0];
        bool differ[8] = {};
        for (const model::RunFont& f : fonts) {
          differ[0] |= f.name != f0.name;
          differ[1] |= f.size != f0.size;
          differ[2] |= f.color != f0.color;
          differ[3] |= f.bold != f0.bold;
          differ[4] |= f.italic != f0.italic;
          differ[5] |= f.strike != f0.strike;
          differ[6] |= f.underline != f0.underline;
          differ[7] |= f.script != f0.script;
        }
        if (differ[0]) note_mixed("font_name");
        if (differ[1]) note_mixed("font_size");
        if (differ[2]) note_mixed("font_color"), note_mixed("font_theme");
        if (differ[3]) note_mixed("bold");
        if (differ[4]) note_mixed("italic");
        if (differ[5]) note_mixed("strike");
        if (differ[6]) note_mixed("underline");
        if (differ[7]) note_mixed("script");
        into->Set("font_name", MV::Str(f0.name));
        into->Set("font_size", MV::Double(f0.size));
        if (f0.color != st.font_color) {
          into->Set("font_color", MV::Int(model::ResolveColor(f0.color, *d.wb().theme)));
          into->Set("font_theme", MV());
        }
        into->Set("bold", MV::Bool(f0.bold));
        into->Set("italic", MV::Bool(f0.italic));
        into->Set("strike", MV::Bool(f0.strike));
        into->Set("underline", MV::Int(static_cast<int>(f0.underline)));
        into->Set("script", MV::Int(f0.script));
        return true;
      };
      if (!p.GetBool("raw")) rich_fonts(r0, c0, &out);  // raw: the cell's own formats (what its characters start from)
      // the edges the cell shows: its own line, or the facing edge of the cell beside it when it has none (Excel 16,
      // measured: B2's bottom border is B3's top, D2's right one E2's left); Border reads the range's outer edges so
      {
        const int64_t dr[4] = {0, -1, 0, 1}, dc[4] = {-1, 0, 1, 0};
        const int facing[4] = {model::kRight, model::kBottom, model::kLeft, model::kTop};
        MV shown = MV::Array();
        const MV* own = out.Get("borders");
        for (int e = 0; e < 4; ++e) {
          const int64_t nr = int64_t{r0} + dr[e], nc = int64_t{c0} + dc[e];
          const model::Style& mine = t.Get(first);
          if (mine.border[e].style == model::LineStyle::None && nr >= 0 && nc >= 0 && nr < int64_t{s->row_limit} && nc < int64_t{s->col_limit}) {
            const model::Style& next = t.Get(s->GetStyle(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc)));
            if (next.border[facing[e]].style != model::LineStyle::None) {
              model::Style one;
              one.border[e] = next.border[facing[e]];
              const MV side = StyleToMV(t, one, *d.wb().theme);
              shown.Push(side.Get("borders")->a[static_cast<size_t>(e)]);
              continue;
            }
          }
          shown.Push(own ? own->a[static_cast<size_t>(e)] : MV());
        }
        out.Set("shown_borders", std::move(shown));
      }
      auto mixed_mv = [&]() {
        MV mv = MV::Array();
        for (const std::string& k : mixed) mv.Push(MV::Str(k));
        return mv;
      };
      const MV* rv = p.Get("rect");
      if (!rv) {
        out.Set("mixed", mixed_mv());
        return out;
      }
      // several cells: the properties that differ between them are listed in "mixed" (Python reads them as None, Excel's
      // Null). Past the used range cells share their row's or column's formats: one more row and column stand for them
      Rect w = Clamp(*s, RectOf(*rv));
      if (s->has_used) {
        w.r1 = std::min(w.r1, std::max(w.r0, s->used.r1 + 1));
        w.c1 = std::min(w.c1, std::max(w.c0, s->used.c1 + 1));
      } else {
        w.r1 = std::min(w.r1, w.r0 + 1);
        w.c1 = std::min(w.c1, w.c0 + 1);
      }
      std::vector<uint32_t> ids;
      uint64_t seen = 0;
      for (uint32_t r = w.r0; r <= w.r1 && seen < 4'000'000 && ids.size() < 64; ++r) {
        for (uint32_t c = w.c0; c <= w.c1; ++c, ++seen) {
          const uint32_t id = s->GetStyle(r, c);
          if (id != first && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id); // lesson-ok: find-push at most 64 styles
        }
      }
      static const char* kKeys[] = {"font_name", "font_size", "bold", "italic", "underline", "strike", "font_color", "fill_color",
                                    "font_theme", "fill_theme", "pattern", "pattern_color", "gradient", "halign", "valign", "wrap",
                                    "shrink", "indent", "rotation", "script", "numfmt", "locked", "hidden", "reading_order"};
      auto compare = [&](const MV& o) {
        for (const char* k : kKeys) {
          const MV* a = out.Get(k);
          const MV* b = o.Get(k);
          if (a && b && !SameMV(*a, *b)) note_mixed(k);
        }
      };
      for (const uint32_t id : ids) compare(StyleToMV(t, t.Get(id), *d.wb().theme));
      // the other cells whose characters have fonts of their own compare by what their characters show
      for (const auto& [k, rt] : *s->rich) {
        const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        if ((r == r0 && c == c0) || !w.Contains(r, c)) continue;
        MV o = StyleToMV(t, t.Get(s->GetStyle(r, c)), *d.wb().theme);
        if (rich_fonts(r, c, &o)) compare(o);
      }
      out.Set("mixed", mixed_mv());
      return out;
    });
  }
  if (m == "range.end") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      const model::Sheet* s = d.S(sheet);
      const int64_t row = p.GetInt("row"), col = p.GetInt("col");
      uint32_t r = static_cast<uint32_t>(std::clamp<int64_t>(row, 0, core::kMaxRows - 1));
      uint32_t c2 = static_cast<uint32_t>(std::clamp<int64_t>(col, 0, core::kMaxCols - 1));
      if (s && row >= 0 && col >= 0 && row < s->row_limit && col < core::kMaxCols) {
        switch (p.GetInt("dir")) {
          case -4162: r = engine::CtrlJump(*s, r, c2, true, -1); break;
          case -4121: r = engine::CtrlJump(*s, r, c2, true, +1); break;
          case -4159: c2 = engine::CtrlJump(*s, r, c2, false, -1); break;
          case -4161: c2 = engine::CtrlJump(*s, r, c2, false, +1); break;
          default: break;
        }
      }
      MV x = MV::Array();
      x.Push(MV::Int(r));
      x.Push(MV::Int(c2));
      return x;
    });
  }
  // xlGuess (Range.Sort, RemoveDuplicates): whether the range's first row is its header, Excel's one guess
  if (m == "range.guess_header") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();
      else d.CalcEntered();
      const model::Sheet* s = d.S(sheet);
      const MV* rect = p.Get("rect");
      return MV::Bool(s && rect && engine::GuessHeaderRow(*d.wb().styles, *s, RectOf(*rect)));
    });
  }
  if (m == "range.current_region") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV::Array();
      return rect4(engine::CurrentRegion(*s, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col"))));
    });
  }
  if (m == "range.merge_area") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c2 = static_cast<uint32_t>(p.GetInt("col"));
      Rect out = Rect::Cell(r, c2);
      if (s) {
        if (const Rect* mr = s->MergeAt(r, c2)) out = *mr;
      }
      return rect4(out);
    });
  }
  // Range.MergeCells of a range: 0 no cell merged, 1 every cell in a merged area, 2 some (Excel's Null)
  if (m == "range.merge_state") {
    const MV* rv = p.Get("rect");
    if (!rv) return MV::Int(0);
    const Rect want = RectOf(*rv);
    return MV::Int(eng.Call([&](Document& d) -> int64_t {
      const model::Sheet* s = d.S(sheet);
      if (!s) return 0;
      uint64_t covered = 0;  // merged areas never overlap: their parts inside the range add up
      for (const Rect& mr : *s->merges) {
        if (!mr.Intersects(want)) continue;
        const Rect in{std::max(mr.r0, want.r0), std::max(mr.c0, want.c0), std::min(mr.r1, want.r1), std::min(mr.c1, want.c1)};
        covered += in.Count();
      }
      return covered == 0 ? 0 : covered >= want.Count() ? 1 : 2;
    }));
  }
  // dynamic array / array formula covering a cell: {anchor: [r, c], rect: [r0, c0, r1, c1], cse} or nil
  if (m == "range.spill") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      uint32_t ar = 0, ac = 0;
      Rect rc;
      const auto row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
      if (!d.SpillAt(sheet, row, col, &ar, &ac, &rc)) {
        // a formula whose result is one value (=DROP({1;2;3},2)): Range("A1#") is the cell itself, as =A1# reads it
        // (Excel 16, measured; a cell without a formula has no spill range)
        const model::Sheet* sh = d.S(sheet);
        if (!sh || sh->GetFormula(row, col) == 0) return MV();
        ar = row;
        ac = col;
        rc = Rect::Cell(row, col);
      }
      const model::SpillRange* sp = d.S(sheet)->SpillOf(ar, ac);
      MV x = MV::Map();
      MV anchor = MV::Array();
      anchor.Push(MV::Int(ar));
      anchor.Push(MV::Int(ac));
      x.Set("anchor", std::move(anchor));
      x.Set("rect", rect4(rc));
      x.Set("cse", MV::Bool(sp && sp->cse));
      return x;
    });
  }
  // Range.RemoveDuplicates: cols are 1-based within the range
  if (m == "range.remove_duplicates") {
    const auto areas = AreasOf(p);
    if (areas.empty()) return {};
    // Excel 16 refuses several areas (measured 2026-11-20); the first one alone was done
    if (areas.size() > 1) return Err(ec, em, "E_APP", "This can't be done on a multiple range selection. Select a single range and try again."), MV();
    std::vector<uint32_t> cols;
    if (const MV* cv = p.Get("cols"); cv && cv->t == MV::T::Array) {
      for (const MV& x : cv->a) cols.push_back(areas[0].c0 + static_cast<uint32_t>(std::max<int64_t>(1, (x.t == MV::T::Double ? static_cast<int64_t>(x.d) : x.i)) - 1));
    }
    const bool header = p.GetBool("header", false);
    return MV::Int(eng.Call([&](Document& d) {
      Settle(d);
      d.Begin("RemoveDuplicates");
      const uint32_t n = d.RemoveDuplicates(sheet, areas[0], cols, header);
      d.Commit();
      return n;
    }));
  }
  // Range.TextToColumns
  if (m == "range.text_to_columns") {
    const auto areas = AreasOf(p);
    if (areas.empty()) return {};
    if (areas.size() > 1) return Err(ec, em, "E_APP", "This can't be done on a multiple range selection. Select a single range and try again."), MV();  // Excel 16 refuses them (measured)
    engine::TextToColumnsOptions o;
    o.fixed = p.GetBool("fixed", false);
    o.tab = p.GetBool("tab", false);
    o.semicolon = p.GetBool("semicolon", false);
    o.comma = p.GetBool("comma", false);
    o.space = p.GetBool("space", false);
    o.other = p.GetStr("other");
    o.consecutive = p.GetBool("consecutive", false);
    const std::string q = p.GetStr("qualifier", "\"");
    o.qualifier = q.empty() ? 0 : q[0];
    const std::string dec = p.GetStr("decimal"), th = p.GetStr("thousands");
    o.decimal = dec.empty() ? 0 : dec[0];
    o.thousands = th.empty() ? 0 : th[0];
    o.trailing_minus = p.GetBool("trailing_minus", true);
    if (const MV* b = p.Get("breaks"); b && b->t == MV::T::Array) {
      for (const MV& x : b->a) o.breaks.push_back(static_cast<uint32_t>(std::max<int64_t>(0, (x.t == MV::T::Double ? static_cast<int64_t>(x.d) : x.i))));
    }
    if (const MV* ty = p.Get("types"); ty && ty->t == MV::T::Array) {
      for (const MV& x : ty->a) o.types.push_back(static_cast<int>((x.t == MV::T::Double ? static_cast<int64_t>(x.d) : x.i)));
    }
    const uint32_t dr = static_cast<uint32_t>(p.GetInt("dest_row", areas[0].r0)), dc = static_cast<uint32_t>(p.GetInt("dest_col", areas[0].c0));
    return MV::Int(eng.Call([&](Document& d) {
      d.Begin("TextToColumns");
      const uint32_t n = d.TextToColumns(sheet, areas[0], dr, dc, o);
      d.Commit();
      return n;
    }));
  }
  // outline: Range.Group / Ungroup / ShowDetail, Outline.ShowLevels, Range.OutlineLevel
  if (m == "outline.group") {
    const bool rows = p.GetBool("rows", true), group = p.GetBool("group", true);
    const uint32_t a = static_cast<uint32_t>(p.GetInt("a"));
    std::string err;
    eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return;
      const uint32_t b = std::min<uint32_t>(static_cast<uint32_t>(p.GetInt("b")), rows ? s->row_limit - 1 : core::kMaxCols - 1);
      if (a > b) return;
      // Excel: rows all at the deepest level (8) can't be grouped again, rows in no group can't be ungrouped
      uint64_t grouped = 0, at_max = 0;
      const auto& lv = (rows ? *s->rows : *s->cols).Levels();
      for (auto it = lv.lower_bound(a); it != lv.end() && it->first <= b; ++it) {
        ++grouped;
        if (it->second >= model::DimIndex::kMaxOutline) ++at_max;
      }
      if (group && at_max == uint64_t{b} - a + 1) {
        err = "Unable to get the Group property of the Range class: an outline has at most 8 levels";
        return;
      }
      if (!group && grouped == 0) {
        err = "Unable to get the Ungroup property of the Range class: the rows or columns are in no group";
        return;
      }
      d.Begin(group ? "Group" : "Ungroup");
      d.GroupOutline(sheet, rows, a, b, group);  // hidden rows that leave their groups stay hidden, as Excel's
      d.Commit();
    });
    if (!err.empty()) return Err(ec, em, "E_APP", err), MV();
    return {};
  }
  // Outline.SummaryRow / SummaryColumn / AutomaticStyles (Data > Outline settings), Range.AutoOutline
  if (m == "outline.settings") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      bool below = s->outline_below, right = s->outline_right, styles = s->outline_styles;
      if (p.Has("below") || p.Has("right") || p.Has("styles")) {
        below = p.GetBool("below", below);
        right = p.GetBool("right", right);
        styles = p.GetBool("styles", styles);
        d.Begin("Outline Settings");
        d.SetOutlineSettings(sheet, below, right, styles);
        d.Commit();
      }
      MV x = MV::Map();
      x.Set("below", MV::Bool(below));
      x.Set("right", MV::Bool(right));
      x.Set("styles", MV::Bool(styles));
      return x;
    });
  }
  if (m == "outline.auto") {
    const Rect r = RectOf(*p.Get("rect"));
    return MV::Int(eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      Rect area = r;
      if (s && s->has_used && r.IsCell()) area = s->used;
      d.Begin("Auto Outline");
      const int n = d.AutoOutline(sheet, area);
      d.Commit();
      return n;
    }));
  }
  if (m == "outline.show_levels") {
    const int rl = static_cast<int>(p.GetInt("rows", 0)), cl = static_cast<int>(p.GetInt("cols", 0));
    eng.Call([&](Document& d) {
      if (rl > 0) d.ShowOutlineLevel(sheet, true, rl);
      if (cl > 0) d.ShowOutlineLevel(sheet, false, cl);
    });
    return {};
  }
  if (m == "outline.detail") {
    const bool rows = p.GetBool("rows", true), show = p.GetBool("show", true);
    const uint32_t index = static_cast<uint32_t>(p.GetInt("index"));
    return MV::Bool(eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return false;
      const auto& dim = rows ? *s->rows : *s->cols;
      // the summary row of the group next to it: the group above it (Outline.SummaryRow = xlSummaryBelow, the
      // default) or below it (xlSummaryAbove); columns follow SummaryColumn. Otherwise the group the row is in.
      const bool after = rows ? s->outline_below : s->outline_right;
      const uint32_t limit = rows ? s->row_limit : s->col_limit;
      uint32_t i = index;
      int level = dim.Level(i);
      if (after && i > 0 && dim.Level(i - 1) > level) {
        i = i - 1;
        level += 1;
      } else if (!after && i + 1 < limit && dim.Level(i + 1) > level) {
        i = i + 1;
        level += 1;
      }
      if (level <= 0) return false;
      d.SetGroupCollapsed(sheet, rows, i, level, !show);
      return true;
    }));
  }
  if (m == "outline.level") {
    const bool rows = p.GetBool("rows", true);
    const uint32_t index = static_cast<uint32_t>(p.GetInt("index"));
    if (const MV* set = p.Get("set")) {
      const int lv = static_cast<int>((set->t == MV::T::Double ? static_cast<int64_t>(set->d) : set->i));
      const uint32_t b_in = static_cast<uint32_t>(p.GetInt("b", index));
      const bool clear = p.GetBool("clear", false);  // Clear Outline rather than OutlineLevel = 1
      if (lv < 1 || lv > model::DimIndex::kMaxOutline + 1) {
        return Err(ec, em, "E_APP", "Unable to set the OutlineLevel property of the Range class: the level goes from 1 to 8"), MV();
      }
      eng.Call([&](Document& d) {
        const model::Sheet* s0 = d.S(sheet);
        if (!s0) return;
        // a whole column's end marker (0xFFFFFFFF) walked four billion lines
        const uint32_t b = std::min<uint32_t>(b_in, rows ? s0->row_limit - 1 : core::kMaxCols - 1);
        if (index > b) return;
        d.Begin(clear ? "Clear Outline" : "OutlineLevel");
        if (clear) {
          // Range.ClearOutline: the lines leave every group and collapsed ones are shown (Excel 16, measured)
          d.ClearOutline(sheet, rows, index, b);
        } else if (lv == 1) {
          // Clear Outline: only the grouped rows change (a whole sheet's rows are not walked one by one)
          for (int k = 0; k <= model::DimIndex::kMaxOutline; ++k) {
            const model::Sheet* s = d.S(sheet);
            if (!s) break;
            const auto& levels = (rows ? *s->rows : *s->cols).Levels();
            auto lo = levels.lower_bound(index);
            auto hi = levels.upper_bound(b);
            if (lo == levels.end() || lo->first > b) break;
            --hi;
            d.GroupOutline(sheet, rows, static_cast<uint32_t>(lo->first), static_cast<uint32_t>(hi->first), false);
          }
        } else {
          // runs of lines at one level, each grouped as a whole: a line at a time, a million rows were a million
          // steps of each change (the lines in no group are the gaps between the levels kept)
          struct Run {
            uint32_t a, b;
            int level;
          };
          std::vector<Run> runs;
          const auto& levels = (rows ? *s0->rows : *s0->cols).Levels();
          auto it = levels.lower_bound(index);
          for (uint32_t i = index;;) {
            if (it == levels.end() || it->first > b) {
              runs.push_back(Run{i, b, 0});
              break;
            }
            if (it->first > i) {
              runs.push_back(Run{i, static_cast<uint32_t>(it->first - 1), 0});
              i = static_cast<uint32_t>(it->first);
              continue;
            }
            const int l = it->second;
            uint32_t j = i;
            auto nx = std::next(it);
            while (nx != levels.end() && nx->first == int64_t{j} + 1 && nx->first <= b && nx->second == l) {
              ++j;
              ++nx;
            }
            runs.push_back(Run{i, j, l});
            if (j >= b) break;
            i = j + 1;
            it = nx;
          }
          for (const Run& run : runs) {
            for (int k = run.level; k < lv - 1; ++k) d.GroupOutline(sheet, rows, run.a, run.b, true);
            for (int k = run.level; k > lv - 1; --k) d.GroupOutline(sheet, rows, run.a, run.b, false);
          }
        }
        d.Commit();
      });
      return {};
    }
    return MV::Int(eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      return s ? (rows ? *s->rows : *s->cols).Level(index) + 1 : 1;
    }));
  }
  // Range.FormulaArray = ...: one array formula for the rect
  if (m == "range.set_array") {
    const auto areas = AreasOf(p);
    if (areas.empty()) return {};
    std::string f = p.GetStr("formula");
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    std::string error;
    const bool ok = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return false;
      d.Begin("FormulaArray");
      // whole columns end at the sheet's last row: their end marker read as past the sheet, and B:B's array (which
      // Excel 16 enters, measured) was refused as an invalid formula
      const bool r_ok = d.SetArrayFormula(sheet, Clamp(*s, areas[0]), f, &error, engine::kSrcPython);
      d.Commit();
      return r_ok;
    });
    if (!ok) return Err(ec, em, "E_FORMULA", error.empty() ? "Invalid array formula" : error), MV();
    return {};
  }
  if (m == "range.find") {
    return eng.Call([&](Document& d) {
      if (d.auto_calc) d.Recalc();  // manual calculation: read what the cells show (Excel), new formulas computed
      else d.CalcEntered();
      const auto areas = AreasOf(p);
      const model::Sheet* s = d.S(sheet);
      if (!s || areas.empty()) return MV();
      // Excel 16 (measured 2026-11-20): the areas are one sequence, each area by rows (columns) in the areas' order,
      // searched from the cell after the start round to the start itself (FindPrevious backwards). Only the first
      // area was searched: Range("A1:B2,XFD1048576").Find found nothing in XFD1048576
      std::vector<Rect> ar;
      for (const Rect& a : areas) ar.push_back(Clamp(*s, a));
      const MV* st = p.Get("start");
      const uint32_t sr = st ? static_cast<uint32_t>(st->a[0].i) : ar[0].r0, sc = st ? static_cast<uint32_t>(st->a[1].i) : ar[0].c0;
      size_t k = 0;
      for (size_t i = 0; i < ar.size(); ++i) {
        if (ar[i].Contains(sr, sc)) {
          k = i;
          break;
        }
      }
      const bool by_rows = p.GetBool("by_rows", true);
      auto key = [&](uint32_t r, uint32_t c) { return by_rows ? (uint64_t{r} << 32 | c) : (uint64_t{c} << 32 | r); };
      const uint64_t sk = key(sr, sc);
      const std::string what = p.GetStr("what");
      const bool match_case = p.GetBool("match_case"), whole = p.GetBool("whole"), formulas = p.GetBool("formulas");
      const engine::FormatSpec fmt = FormatSpecOf(p.Get("fmt"));
      const engine::FormatSpec* fp = fmt.Any() ? &fmt : nullptr;
      auto cell = [](uint32_t r, uint32_t c) {
        MV x = MV::Array();
        x.Push(MV::Int(r));
        x.Push(MV::Int(c));
        return x;
      };
      // the areas after the start's, then those before it
      std::vector<size_t> others;
      for (size_t i = k + 1; i < ar.size(); ++i) others.push_back(i);
      for (size_t i = 0; i < k; ++i) others.push_back(i);
      if (p.GetBool("previous")) {
        // search backwards (FindPrevious, SearchDirection:=xlPrevious): the last match before the start, wrapping round
        auto all = [&](size_t i) { return d.FindAll(sheet, ar[i], what, by_rows, match_case, whole, formulas, 1000000, fp); };
        const auto own = all(k);
        for (auto it = own.rbegin(); it != own.rend(); ++it) {
          if (key(it->row, it->col) < sk) return cell(it->row, it->col);
        }
        for (auto it = others.rbegin(); it != others.rend(); ++it) {
          const auto m = all(*it);
          if (!m.empty()) return cell(m.back().row, m.back().col);
        }
        if (!own.empty() && key(own.back().row, own.back().col) >= sk) return cell(own.back().row, own.back().col);
        return MV();
      }
      uint32_t r = 0, c2 = 0;
      auto first_in = [&](const Rect& a) { return d.Find(sheet, a, what, a.r1, a.c1, by_rows, match_case, whole, formulas, &r, &c2, fp); };
      if (d.Find(sheet, ar[k], what, sr, sc, by_rows, match_case, whole, formulas, &r, &c2, fp) && key(r, c2) > sk) return cell(r, c2);
      for (const size_t i : others) {
        if (first_in(ar[i])) return cell(r, c2);
      }
      if (first_in(ar[k]) && key(r, c2) <= sk) return cell(r, c2);
      return MV();
    });
  }
  if (m == "range.geometry") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      MV x = MV::Map();
      if (!s) return x;
      const Rect r = Clamp(*s, RectOf(*p.Get("rect")));
      x.Set("left", MV::Double(static_cast<double>(s->cols->Offset(r.c0)) * 0.75));
      x.Set("top", MV::Double(static_cast<double>(s->rows->Offset(r.r0)) * 0.75));
      x.Set("width", MV::Double(static_cast<double>(s->cols->Offset(int64_t{r.c1} + 1) - s->cols->Offset(r.c0)) * 0.75));
      x.Set("height", MV::Double(static_cast<double>(s->rows->Offset(int64_t{r.r1} + 1) - s->rows->Offset(r.r0)) * 0.75));
      return x;
    });
  }
  if (m == "dims.get") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      MV x = MV::Map();
      if (!s) return x;
      const bool col = p.GetStr("kind") == "col";
      const int64_t i = p.GetInt("index");
      const auto& dim = col ? *s->cols : *s->rows;
      const int32_t px = dim.Size(i);
      x.Set("px", MV::Int(px));
      x.Set("hidden", MV::Bool(px == 0));
      x.Set("points", MV::Double(px * 0.75));
      x.Set("chars", MV::Double(render::ColPxToChars(px)));
      if (p.Has("last")) {
        // several rows / columns: whether their sizes and hidden states differ (Python reads them as None, Excel's Null)
        const int64_t last = std::max<int64_t>(i, p.GetInt("last"));
        const int64_t count = last - i + 1;
        int64_t customs = 0, hidden = 0;
        bool same = true;
        for (const auto& e : dim.Entries()) {
          if (e.index < i || e.index > last) continue;
          ++customs;
          if (e.size == 0) ++hidden;
          if (e.size != px) same = false;
        }
        if (customs < count && dim.default_size() != px) same = false;
        x.Set("mixed", MV::Bool(!same));
        x.Set("hidden_mixed", MV::Bool(hidden > 0 && hidden < count));
      }
      return x;
    });
  }

  // ---- Range: write (Python writes don't go on the undo stack, like Excel clears undo after running a macro) ----
  if (m == "range.set") {
    const Rect req = RectOf(*p.Get("rect"));
    // one value written over rows of a filtered list goes into the rows the filter shows, as Excel 16's Range.Value
    // does (measured: "z" into E2:E9 under Side = Buy left E3, E6, E7); an array of values, or a formula, as it is
    // (a formula too: each run of rows gets it as the cells there would, its relative references moved to them:
    // =C2*2 over E2:E9 gives E4 =C4*2, measured)
    // whole columns too (Excel 16, measured 2026-11-20: Range("B:B").Value = 5 leaves the hidden B2): the runs are as
    // many as the filter's gaps, not the column's rows
    if (!p.GetBool("all_rows") && req.r1 > req.r0) {
      const MV* v0 = p.Get("values");
      const bool formula = v0 && v0->t == MV::T::Str && !v0->s.empty() && v0->s[0] == '=';
      const bool one = !v0 || v0->t != MV::T::Array;
      std::vector<Rect> runs;
      std::vector<std::string> texts;  // per run: the formula written for its first cell
      if (one) {
        eng.Call([&](Document& d) {
          const model::Sheet* s = d.S(sheet);
          if (!s) return;
          runs = engine::FilterShownRuns(*s, {req});
          if (!formula || (runs.size() == 1 && runs[0] == req)) return;
          formula::ParseResult pr = formula::Parse(std::string_view(v0->s).substr(1), core::CellPos{req.r0, req.c0}, &d);
          if (!pr.ok) {
            runs = {req};  // the write itself reports the error
            return;
          }
          for (const Rect& run : runs) texts.push_back("=" + formula::ToA1(pr.root, core::CellPos{run.r0, run.c0}, &d));
        });
      }
      if (one && !(runs.size() == 1 && runs[0] == req)) {
        for (size_t i = 0; i < runs.size(); ++i) {
          MV q = p;
          q.Set("rect", rect4(runs[i]));
          q.Set("all_rows", MV::Bool(true));
          if (formula) q.Set("values", MV::Str(texts[i]));
          Dispatch(c, m, q, ec, em);
          if (ec && !ec->empty()) return MV();
        }
        return {};
      }
    }
    const std::string kind = p.GetStr("kind", "input");
    const bool events = p.GetBool("events", true);
    const MV* vals = p.Get("values");
    std::string err_code, err;
    // rows that size themselves fit what is written, as Excel 16's Range.Value does (measured 2026-11-28: two lines
    // replaced by "c" put the row back at the standard height). Asked of the UI only when the write may change a
    // height: a text with a line break, or a row of the range that isn't at the standard height; a script streaming
    // numbers into standard rows waits for nothing
    std::vector<std::pair<uint32_t, int32_t>> fit;
    if (kind != "formula" && req.r1 != kRowEnd && req.r1 >= req.r0 && req.c1 >= req.c0 && req.Rows() <= 2000 && req.Count() <= 20000) {
      const bool scalar0 = !vals || vals->t != MV::T::Array;
      auto at = [&](uint32_t i, uint32_t j) -> const MV* {
        if (scalar0) return vals;
        if (i >= vals->a.size()) return nullptr;
        const MV& row = vals->a[i];
        if (row.t != MV::T::Array) return j == 0 ? &row : nullptr;
        return j < row.a.size() ? &row.a[j] : nullptr;
      };
      bool may = false;
      for (uint32_t i = 0; i < req.Rows() && !may; ++i) {
        for (uint32_t j = 0; j < req.Cols() && !may; ++j) {
          const MV* v = at(i, j);
          may = v && v->t == MV::T::Str && v->s.find('\n') != std::string::npos;
        }
      }
      if (!may) {
        // a taller row may shrink: when a cell written over holds lines or wraps (the snapshot is enough to tell)
        eng.Sync();
        const auto snap = eng.Current();
        if (const model::Sheet* s0 = snap ? snap->Sheet(sheet) : nullptr) {
          for (uint32_t r = req.r0; r <= req.r1 && !may; ++r) {
            if (s0->rows->Size(r) == s0->rows->default_size()) continue;
            for (uint32_t cc = req.c0; cc <= req.c1 && !may; ++cc) {
              const core::Value& old = s0->GetValue(r, cc);
              may = (old.IsString() && old.str().find('\n') != std::string_view::npos) || snap->wb.styles->Get(s0->GetStyle(r, cc)).wrap;
            }
          }
        }
      }
      if (may) {
        std::map<uint64_t, std::string> texts;
        for (uint32_t i = 0; i < req.Rows(); ++i) {
          for (uint32_t j = 0; j < req.Cols(); ++j) {
            const MV* v = at(i, j);
            texts[(uint64_t{req.r0 + i} << 16) | (req.c0 + j)] = v && v->t == MV::T::Str ? v->s : (v && v->t != MV::T::Nil ? std::string("0") : std::string());
          }
        }
        OnUi([&](AppState& s) {
          fit = FitRowsForWrite(s, sheet, req, texts);
          return MV();
        });
      }
    }
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      const model::Sheet* s = d.S(sheet);
      if (!s) {
        err_code = "E_APP";
        err = "The sheet no longer exists";
        return;
      }
      Rect r = req;
      if (r.r1 == kRowEnd) r.r1 = s->row_limit - 1;
      if (r.c1 >= core::kMaxCols) r.c1 = core::kMaxCols - 1;
      if (r.r0 >= core::kMaxRows || r.c0 >= core::kMaxCols || r.r0 > r.r1 || r.c0 > r.c1) {
        err_code = "E_ARG";
        err = "The range is outside the sheet";
        return;
      }
      if (r.Count() > 20000000) {
        err_code = "E_APP";
        err = "The range is too large";
        return;
      }
      struct SrcBits {  // restored even when the write throws: later changes must raise their events again
        Document& doc;
        ~SrcBits() { doc.extra_src_bits = 0; }
      } src_bits_guard{d};
      d.extra_src_bits = events ? 0 : engine::kSrcNoEvents;
      const bool scalar = !vals || vals->t != MV::T::Array;
      auto cell = [&](uint32_t i, uint32_t j) -> const MV* {
        if (scalar) return vals;
        if (i >= vals->a.size()) return nullptr;
        const MV& row = vals->a[i];
        if (row.t != MV::T::Array) return j == 0 ? &row : nullptr;
        return j < row.a.size() ? &row.a[j] : nullptr;
      };
      static const MV kNA = MV::Ext(2, std::string(1, static_cast<char>(core::Err::NA)));
      // a cell hidden under a merged cell takes nothing: Range("A1:B2").Value = 7 on the merged A1:B2 writes A1 only,
      // and a write to B2 alone is ignored (Excel); SUM(A1:B2) stays 7
      std::vector<Rect> merges_in;
      for (const Rect& mg : *s->merges) {
        if (mg.Intersects(r)) merges_in.push_back(mg);
      }
      auto covered = [&](uint32_t rr, uint32_t cc) {
        for (const Rect& mg : merges_in) {
          if (mg.Contains(rr, cc) && (rr != mg.r0 || cc != mg.c0)) return true;
        }
        return false;
      };
      // a formula written over arrays entered with Ctrl+Shift+Enter does what a value does (Excel 16, measured
      // 2026-11-20): a range that cuts one changes nothing and raises nothing (Range("2:3").Formula through G2:G4, or G3
      // alone), and an array inside the range is replaced (G2:G4, G:G). The cells beside a cut array were written, an
      // array inside the range stayed, and G3 raised
      if (kind == "formula") {
        std::vector<Rect> inside;
        for (const auto& [key, sp] : *s->spills) {
          if (!sp.cse || !sp.rect.Intersects(r)) continue;
          if (!r.Contains(sp.rect)) return;
          inside.push_back(sp.rect);
        }
        for (const Rect& a : inside) d.ClearContents(sheet, a, engine::kSrcPython);
      }
      // fast path: all numbers / booleans / blanks (large numpy writes)
      bool fast = kind != "formula";
      for (uint32_t i = 0; fast && i < r.Rows(); ++i) {
        for (uint32_t j = 0; fast && j < r.Cols(); ++j) {
          const MV* v = cell(i, j);
          fast = !v || v->t == MV::T::Int || v->t == MV::T::Double || v->t == MV::T::Bool || v->t == MV::T::Nil ||
                 (v->t == MV::T::Ext && v->ext != 4 && v->ext != 5);
          if (scalar) break;
        }
        if (scalar) break;
      }
      if (fast) {
        std::vector<core::Value> buf;
        buf.reserve(static_cast<size_t>(r.Count()));
        for (uint32_t i = 0; i < r.Rows(); ++i) {
          for (uint32_t j = 0; j < r.Cols(); ++j) {
            const MV* v = cell(i, j);
            int dk;
            if (!merges_in.empty() && covered(r.r0 + i, r.c0 + j)) buf.emplace_back();
            else buf.push_back(v ? MVToValue(*v, &dk) : MVToValue(kNA, &dk));
          }
        }
        d.SetValues(sheet, r, buf, engine::kSrcPython);
      } else if (kind == "formula" && scalar && vals && vals->t == MV::T::Str && !vals->s.empty() && vals->s[0] == '=' && !r.IsCell() &&
                 !d.IsTextFormatted(sheet, r.r0, r.c0)) {
        // one formula for many cells: parse at the upper-left cell, offset relative references for the others (like Excel's Range.Formula)
        const model::Sheet* sb = d.S(sheet);
        const uint32_t fmt_before = sb ? d.wb().styles->Get(sb->GetStyle(r.r0, r.c0)).numfmt : 0;
        // the cell the others are pasted from is not a table's header, which makes a formula text at once:
        // Range("B:B").Formula through a table pasted the header's "0" into every cell
        uint32_t sr = r.r0;
        auto header = [&](uint32_t rr) {
          for (const auto& tb : *sb->tables) {
            if (tb.header && rr == tb.range.r0 && r.c0 >= tb.range.c0 && r.c0 <= tb.range.c1) return true;
          }
          return false;
        };
        while (sb && sr < r.r1 && header(sr)) ++sr;
        std::string text(std::string_view(vals->s).substr(1));
        if (sr != r.r0) {
          formula::ParseResult pr = formula::Parse(text, core::CellPos{r.r0, r.c0}, &d);
          if (pr.ok) text = formula::ToA1(pr.root, core::CellPos{sr, r.c0}, &d);
        }
        const std::string inferred = d.InferredFormat(sheet, r.r0, r.c0, std::string_view(vals->s).substr(1));
        if (!d.SetFormula(sheet, sr, r.c0, text, &err, engine::kSrcPython)) {
          err_code = "E_FORMULA";
        } else {
          const auto clip = d.Copy(sheet, Rect::Cell(sr, r.c0));
          for (uint32_t i = r.r0; i <= r.r1; ++i) {
            for (uint32_t j = r.c0; j <= r.c1; ++j) {
              if ((i != sr || j != r.c0) && !covered(i, j)) d.Paste(sheet, i, j, clip, engine::PasteMode::Formulas);
            }
          }
          // the format the first cell's formula gives its result goes to every cell without a format, whether or
          // not the first cell has one (Excel 16, measured 2026-11-21: =$B$2+C3 over F3:G3 with F3 in 0.00% and C3 in
          // 0.00 gives G3 0.00, though G3's own =$B$2+D3 gives none); a date function's date format the same way
          (void)fmt_before;
          if (!inferred.empty()) {
            const uint32_t fmt = d.wb_mut().styles.mut().InternString(inferred);
            d.ModifyStyle(sheet, r, [fmt](model::Style& st) {
              if (st.numfmt == 0) st.numfmt = fmt;
            });
          }
        }
      } else {
        for (uint32_t i = 0; i < r.Rows() && err_code.empty(); ++i) {
          for (uint32_t j = 0; j < r.Cols(); ++j) {
            const MV* v = cell(i, j);
            const uint32_t rr = r.r0 + i, cc = r.c0 + j;
            if (!merges_in.empty() && covered(rr, cc)) continue;
            if (!v) {
              d.SetValue(sheet, rr, cc, core::Value::Error(core::Err::NA), engine::kSrcPython);
              continue;
            }
            if (v->t == MV::T::Str && (kind == "input" || kind == "formula")) {
              std::string e2;
              if (!d.SetInput(sheet, rr, cc, v->s, &e2, engine::kSrcPython)) {
                err_code = "E_FORMULA";
                err = core::CellA1(rr, cc) + ":" + e2;
                break;
              }
              continue;
            }
            int dk;
            core::Value val = MVToValue(*v, &dk);
            d.SetValue(sheet, rr, cc, std::move(val), engine::kSrcPython);
            if (dk) {
              const model::Sheet* s2 = d.S(sheet);
              if (d.wb().styles->Get(s2->GetStyle(rr, cc)).numfmt == 0) {  // read it fresh: a mut() above may have copied the table
                const double x = v->s.size() == 8 ? [&] { double t; std::memcpy(&t, v->s.data(), 8); return t; }() : 0.0;
                const char* code = dk == 2 ? "h:mm:ss" : (x == std::floor(x) ? "yyyy/m/d" : "yyyy/m/d h:mm");
                const uint32_t id = d.wb_mut().styles.mut().InternString(code);
                d.ModifyStyle(sheet, Rect::Cell(rr, cc), [id](model::Style& st) { st.numfmt = id; });
              }
            }
          }
        }
      }
      if (err_code.empty()) {
        for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
      }
      // Application.EnableEvents = False: the recalculation this write causes fires no Calculate either (Excel
      // calculates during the write, while events are off)
      if (!events && err_code.empty()) d.RecalcWithoutEvents();
      d.extra_src_bits = 0;
    });
    if (!err_code.empty()) return Err(ec, em, err_code.c_str(), err), MV();
    return {};
  }
  if (m == "range.clear") {
    auto areas = AreasOf(p);
    // Clear Contents and Clear over part of a merged cell are refused, as Excel 16's object model does (measured: C3 of
    // B2:C3; Clear of B3 through B3:C4, 2026-11-20)
    if (p.GetInt("what") != 1) {
      bool part = false;
      eng.Call([&](Document& d) {
        if (const model::Sheet* s = d.S(sheet)) {
          for (const Rect& a : areas) part = part || engine::CutsMerge(*s, a);
        }
      });
      if (part) return Err(ec, em, "E_APP", "We can't do that to a merged cell."), MV();
    }
    // Clear Contents / Clear over a filtered list: the rows it shows, as Excel 16's object model does (measured)
    if (p.GetInt("what") != 1) {
      eng.Call([&](Document& d) {
        if (const model::Sheet* s = d.S(sheet)) {
          std::vector<Rect> clamped;
          for (const Rect& a : areas) clamped.push_back(Clamp(*s, a));
          areas = engine::FilterShownRuns(*s, clamped);
        }
      });
    }
    const int what = static_cast<int>(p.GetInt("what"));
    const bool events = p.GetBool("events", true);
    // rows that size themselves fit what is left, as after Delete (Excel 16's ClearContents, measured): measured on
    // the UI thread, which has the fonts
    std::vector<std::pair<uint32_t, int32_t>> fit;
    if (what != 1) {
      OnUi([&](AppState& s) {
        fit = FitRowsAfterClear(s, sheet, areas, what == 2);
        return MV();
      });
    }
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      struct SrcBits {  // restored even when the write throws: later changes must raise their events again
        Document& doc;
        ~SrcBits() { doc.extra_src_bits = 0; }
      } src_bits_guard{d};
      d.extra_src_bits = events ? 0 : engine::kSrcNoEvents;
      for (const Rect& r0 : areas) {
        const model::Sheet* s = d.S(sheet);
        if (!s) break;
        const Rect r = Clamp(*s, r0);
        if (what != 1) d.ClearTablesIn(sheet, r);  // a whole table cleared goes away (Excel)
        if (what == 0) d.ClearContents(sheet, r, engine::kSrcPython);
        else if (what == 1) d.ClearFormats(sheet, r);
        else d.ClearAll(sheet, r, engine::kSrcPython, /*everything=*/true);
      }
      for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
      if (!events) d.RecalcWithoutEvents();  // (as range.set)
      d.extra_src_bits = 0;
    });
    return {};
  }
  // ---- Themes (Workbook.Theme, ApplyTheme) ----
  if (m == "theme.get") {
    return eng.Call([&](Document& d) {
      const model::Theme& t = *d.wb().theme;
      MV x = MV::Map();
      x.Set("name", MV::Str(t.name));
      MV c = MV::Array();
      for (uint32_t v : t.colors) c.Push(MV::Int(v));
      x.Set("colors", std::move(c));
      x.Set("colors_name", MV::Str(t.colors_name));
      x.Set("major", MV::Str(t.major_font));
      x.Set("minor", MV::Str(t.minor_font));
      x.Set("fonts_name", MV::Str(t.fonts_name));
      MV names = MV::Array();
      for (const auto& b : model::BuiltinThemes()) names.Push(MV::Str(b.name));
      x.Set("builtin", std::move(names));
      return x;
    });
  }
  if (m == "theme.set") {
    std::string err;
    eng.Call([&](Document& d) {
      model::Theme t = *d.wb().theme;
      if (p.Has("theme")) {
        const std::string want = engine::Lower(p.GetStr("theme"));
        bool found = false;
        for (const auto& b : model::BuiltinThemes()) {
          if (engine::Lower(b.name) == want) {
            t = b;
            found = true;
          }
        }
        if (!found) {
          err = "Unknown theme: " + p.GetStr("theme");
          return;
        }
      }
      if (const MV* c = p.Get("colors")) {
        for (size_t i = 0; i < c->a.size() && i < t.colors.size(); ++i) t.colors[i] = static_cast<uint32_t>(c->a[i].i) & 0xFFFFFF;
        t.colors_name = "Custom";
        t.name = "Custom";
      }
      if (p.Has("major")) t.major_font = p.GetStr("major");
      if (p.Has("minor")) t.minor_font = p.GetStr("minor");
      if (p.Has("major") || p.Has("minor")) t.fonts_name = "Custom";
      // Page Layout > Colors / Fonts: a built-in color set or font pair by name
      if (p.Has("colors_name")) {
        const std::string want = engine::Lower(p.GetStr("colors_name"));
        bool found = false;
        for (const auto& b : model::BuiltinThemeColors()) {
          if (engine::Lower(b.name) == want) t.colors = b.colors, t.colors_name = b.name, found = true;
        }
        if (!found) err = "Unknown theme colors: " + p.GetStr("colors_name");
      }
      if (p.Has("fonts_name")) {
        const std::string want = engine::Lower(p.GetStr("fonts_name"));
        bool found = false;
        for (const auto& b : model::BuiltinThemeFonts()) {
          if (engine::Lower(b.name) == want) t.major_font = b.major, t.minor_font = b.minor, t.fonts_name = b.name, found = true;
        }
        if (!found) err = "Unknown theme fonts: " + p.GetStr("fonts_name");
      }
      if (!err.empty()) return;
      d.ForeignWrite();
      d.SetTheme(t);
    });
    if (!err.empty()) {
      *ec = "E_ARG";
      *em = err;
    }
    return {};
  }
  // ---- Named cell styles (Workbook.Styles, Range.Style) ----
  if (m == "cellstyle.list") {
    return eng.Call([&](Document& d) {
      const auto& t = *d.wb().styles;
      MV out = MV::Array();
      auto push = [&](const model::NamedStyle& ns) {
        MV x = MV::Map();
        x.Set("name", MV::Str(ns.name));
        x.Set("builtin", MV::Bool(ns.builtin));
        x.Set("includes", MV::Int(ns.includes));
        MV st = StyleToMV(t, ns.style, *d.wb().theme);
        if (!ns.number_format.empty()) st.Set("numfmt", MV::Str(ns.number_format));
        x.Set("style", std::move(st));
        out.Push(std::move(x));
      };
      for (const auto& ns : *d.wb().cell_styles) {
        if (!ns.builtin) push(ns);
      }
      for (const auto& b : model::BuiltinCellStyles()) push(*d.CellStyleDef(b.name));
      return out;
    });
  }
  if (m == "cellstyle.export") {
    // this workbook's cell styles for another YCGrid's Merge Styles: a small workbook of its styles (.ycg bytes)
    return eng.Call([&](Document& d) {
      engine::Document tmp;
      tmp.wb_mut().styles = d.wb().styles;
      tmp.wb_mut().cell_styles = d.wb().cell_styles;
      tmp.wb_mut().theme = d.wb().theme;
      return MV::Bin(io::SaveYcgToBytes(tmp.wb(), {}));
    });
  }
  if (m == "cellstyle.merge") {
    const std::wstring path = Wide(p.GetStr("path"));
    return OnUi([path](AppState& s) {
      // a workbook open in another YCGrid: its styles as they are there now
      std::string pipe;
      for (const auto& b : OtherWorkbooks(true)) {
        std::error_code ec;
        if (!b.file.empty() && std::filesystem::equivalent(b.file, path, ec)) pipe = b.pipe;
      }
      MergeStylesFrom(s, path, false, pipe);
      s.engine->Sync();
      return MV::Bool(true);
    });
  }
  if (m == "cellstyle.save") {
    // a new style (from a cell's formats, or Normal) or a change to one: props as for range.style
    if (const MV* pr = p.Get("props"); pr && pr->Has("numfmt") && !formula::ValidFormatCode(pr->GetStr("numfmt")))
      return Err(ec, em, "E_ARG", "Unable to set the NumberFormat property of the Style class"), MV();
    std::string err;
    MV out = eng.Call([&](Document& d) -> MV {
      const std::string name = p.GetStr("name");
      if (name.empty()) {
        err = "A style needs a name";
        return MV();
      }
      auto def = d.CellStyleDef(name);
      if (p.GetBool("create") && def) {
        err = "A style named " + name + " already exists";
        return MV();
      }
      if (!p.GetBool("create") && !def) {
        err = "No style named " + name;
        return MV();
      }
      model::NamedStyle ns = def ? *def : model::NamedStyle{};
      if (!def) ns.name = name;
      auto& tbl = d.wb_mut().styles.mut();
      if (!ns.number_format.empty()) {
        ns.style.numfmt = tbl.InternString(ns.number_format);
        ns.number_format.clear();
      }
      if (p.Has("based_sheet")) {
        const model::Sheet* bs = d.S(static_cast<uint32_t>(p.GetInt("based_sheet")));
        if (bs) ns.style = tbl.Get(bs->GetStyle(static_cast<uint32_t>(p.GetInt("based_row")), static_cast<uint32_t>(p.GetInt("based_col"))));
        ns.style.cell_style = 0;
      }
      if (p.Has("includes")) ns.includes = static_cast<uint8_t>(p.GetInt("includes") & model::kIncAll);
      if (const MV* pr = p.Get("props")) {
        const uint32_t font_id = pr->Has("font_name") ? tbl.InternString(pr->GetStr("font_name")) : 0;
        const bool has_numfmt = pr->Has("numfmt");
        const std::string nf = pr->GetStr("numfmt");
        const uint32_t numfmt_id = has_numfmt ? (nf == "General" || nf.empty() ? 0 : tbl.InternString(nf)) : 0;
        ApplyStyleProps(ns.style, *pr, font_id, has_numfmt, numfmt_id);
      }
      d.ForeignWrite();
      d.SaveCellStyle(ns);
      return MV::Str(ns.name);
    });
    if (!err.empty()) {
      *ec = "E_ARG";
      *em = err;
    }
    return out;
  }
  if (m == "cellstyle.delete") {
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.DeleteCellStyle(p.GetStr("name"));
    });
    if (!ok) {
      *ec = "E_ARG";
      *em = "The style can't be deleted: " + p.GetStr("name");
    }
    return {};
  }
  if (m == "range.cellstyle") {
    const auto areas = AreasOf(p);
    const std::string name = p.GetStr("name");
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      bool all = true;
      for (const Rect& r : areas) all = d.ApplyCellStyle(sheet, r, name) && all;
      return all;
    });
    if (!ok) {
      *ec = "E_ARG";
      *em = "No style named " + name;
    }
    return {};
  }
  // ---- Characters(Start, Length): fonts inside a cell's text ----
  if (m == "rich.get" || m == "rich.apply" || m == "rich.replace") {
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    auto runs_mv = [](const model::RichText* rt) {
      MV a = MV::Array();
      if (!rt) return a;
      for (const auto& r : rt->runs) {
        MV x = MV::Map();
        x.Set("start", MV::Int(r.start));
        x.Set("font", MV::Str(r.font));
        x.Set("size", MV::Double(r.size));
        x.Set("color", MV::Int(r.color));
        x.Set("bold", MV::Int(r.bold));
        x.Set("italic", MV::Int(r.italic));
        x.Set("strike", MV::Int(r.strike));
        x.Set("underline", MV::Int(r.underline));
        x.Set("script", MV::Int(r.script));
        a.Push(std::move(x));
      }
      return a;
    };
    std::string err;
    MV out = eng.Call([&](Document& d) -> MV {
      const core::Value& v = d.GetValue(sheet, row, col);
      std::string text;
      if (v.IsString()) text = std::string(v.str());
      else if (!v.IsEmpty()) text = engine::DisplayTextOf(v, "General");
      const model::RichText* cur = d.RichTextAt(sheet, row, col);
      model::RichText rt = cur ? *cur : model::RichText{text, {}};
      const size_t start = static_cast<size_t>(std::max<int64_t>(0, p.GetInt("start"))), n = model::CharCount(text);
      const size_t len = p.Has("length") ? static_cast<size_t>(std::max<int64_t>(0, p.GetInt("length"))) : n;
      if (m == "rich.apply") {
        if (d.S(sheet) && d.S(sheet)->GetFormula(row, col)) {
          err = "Characters can't be formatted in a formula cell";
          return MV();
        }
        const MV& pr = *p.Get("props");
        d.ForeignWrite();
        if (!v.IsString() && !v.IsEmpty()) d.SetValue(sheet, row, col, core::Value::String(text), engine::kSrcPython);  // like Excel: the number becomes text
        model::ApplyToChars(&rt, start, len, [&](model::RichRun& r) {
          if (pr.Has("font")) r.font = pr.GetStr("font");
          if (pr.Has("size")) r.size = static_cast<float>(pr.GetDouble("size"));
          if (pr.Has("color")) r.color = static_cast<uint32_t>(pr.GetInt("color"));
          if (pr.Has("bold")) r.bold = pr.GetBool("bold") ? 1 : 0;
          if (pr.Has("italic")) r.italic = pr.GetBool("italic") ? 1 : 0;
          if (pr.Has("strike")) r.strike = pr.GetBool("strike") ? 1 : 0;
          if (pr.Has("underline")) r.underline = static_cast<int8_t>(pr.GetInt("underline"));
          if (pr.Has("script")) r.script = static_cast<int8_t>(pr.GetInt("script"));
        });
        d.SetRichText(sheet, row, col, rt);
      } else if (m == "rich.replace") {
        // Characters(Start, Length).Text = ... / Insert / Delete
        const std::string ins = p.GetStr("text");
        const size_t a = model::ByteOfChar(text, std::min(start, n)), b = model::ByteOfChar(text, std::min(n, start + len));
        const std::string nt = text.substr(0, a) + ins + text.substr(b);
        model::RichText remapped = model::ReplaceChars(rt, std::min(start, n), std::min(len, n - std::min(start, n)), ins);
        d.ForeignWrite();
        d.SetValue(sheet, row, col, core::Value::String(nt), engine::kSrcPython);
        d.SetRichText(sheet, row, col, remapped);
        text = nt;
        rt = remapped;
      }
      MV x = MV::Map();
      x.Set("text", MV::Str(text));
      const model::RichText* now = d.RichTextAt(sheet, row, col);
      x.Set("runs", runs_mv(now));
      return x;
    });
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return out;
  }
  if (m == "range.style") {
    auto areas = AreasOf(p);
    const MV* props = p.Get("props");
    if (!props) return {};
    // the formats of a filtered list go to the rows it shows, as Excel 16's object model does (measured with
    // NumberFormat; rows hidden by hand take them)
    eng.Call([&](Document& d) {
      if (const model::Sheet* s = d.S(sheet)) areas = engine::FilterShownRuns(*s, areas);
    });
    if (areas.empty()) return {};
    // a code Excel refuses ("0;@;0", "[Blue", "0_") raises and changes nothing, as Excel's (measured)
    if (props->Has("numfmt") && !formula::ValidFormatCode(props->GetStr("numfmt")))
      return Err(ec, em, "E_ARG", "Unable to set the NumberFormat property of the Range class"), MV();
    MV pr = *props;
    // rows that size themselves fit a new size, font or Wrap Text, as from the ribbon (Excel's Range.Font.Size and
    // WrapText do): measured on the UI thread, which has the fonts, and set with the formats
    std::vector<std::pair<uint32_t, int32_t>> fit;
    if (pr.Has("font_size") || pr.Has("font_name") || pr.Has("wrap")) {
      OnUi([&](AppState& s) {
        const std::string font = pr.GetStr("font_name");
        fit = FitRowsForStyle(s, sheet, areas, [&](model::Style& x) { ApplyStyleProps(x, pr, 0, false, 0); },
                              pr.Has("font_name") ? &font : nullptr);
        return MV();
      });
    }
    // a number format that shows numbers wider than their column widens a column nobody sized (Excel 16's
    // Range.NumberFormat, measured), as the ribbon's formats do
    std::vector<std::pair<uint32_t, int32_t>> widen;
    if (pr.Has("numfmt") && pr.GetStr("numfmt") != "General" && !pr.GetStr("numfmt").empty()) {
      OnUi([&](AppState& s) {
        widen = ColumnsToWidenForFormat(s, sheet, areas, pr.GetStr("numfmt"));
        for (const auto& [c, px] : widen) s.auto_widened[{sheet, c}] = px;
        return MV();
      });
    }
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      auto& tbl = d.wb_mut().styles.mut();
      const uint32_t font_id = pr.Has("font_name") ? tbl.InternString(pr.GetStr("font_name")) : 0;
      const bool has_numfmt = pr.Has("numfmt");
      const std::string nf = pr.GetStr("numfmt");
      const uint32_t numfmt_id = has_numfmt ? (nf == "General" || nf.empty() ? 0 : tbl.InternString(nf)) : 0;
      for (const Rect& r : areas) {
        d.ModifyStyle(sheet, r, [&](model::Style& x) {
          ApplyStyleProps(x, pr, font_id, has_numfmt, numfmt_id);
        });
      }
      for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
      for (const auto& [c, px] : widen) d.SetColWidth(sheet, c, c, px);
      // a bigger font grows rows without a height of their own, empty cells too (SetFontSize, the ribbon's)
      const model::Sheet* sh = d.S(sheet);
      if (!pr.Has("font_size") || !sh) return;
      const float pt = static_cast<float>(pr.GetDouble("font_size"));
      const int32_t h = static_cast<int32_t>(std::ceil(pt * 96.0f / 72.0f * 1.36f));
      for (const Rect& a0 : areas) {
        const Rect a = Clamp(*sh, a0);
        if (a.Rows() > 100000) continue;
        for (uint32_t r = a.r0; r <= a.r1; ++r) {
          if (std::any_of(fit.begin(), fit.end(), [&](const auto& f) { return f.first == r; })) continue;
          const model::Sheet* cur = d.S(sheet);  // SetRowHeight may give the document a sheet of its own
          if (!cur->rows->IsCustom(r) && h > cur->rows->default_size() && cur->rows->Size(r) < h) d.SetRowHeight(sheet, r, r, h);
        }
      }
    });
    return {};
  }
  if (m == "range.border") {
    const auto areas = AreasOf(p);
    std::vector<std::string> edges;
    if (const MV* e = p.Get("edges")) {
      for (const MV& x : e->a) edges.push_back(x.s);
    }
    const model::BorderSide side{static_cast<model::LineStyle>(p.GetInt("style")), static_cast<uint32_t>(p.GetInt("color"))};
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      for (const Rect& r0 : areas) {
        const model::Sheet* s = d.S(sheet);
        if (!s) break;
        const Rect r = Clamp(*s, r0);
        const uint32_t row_lim = s->row_limit, col_lim = s->col_limit;  // s may be replaced by the first write
        auto set = [&](const Rect& sub, int edge) { d.ModifyStyle(sheet, sub, [&](model::Style& x) { x.border[edge] = side; }); };
        for (const auto& e : edges) {
          if (e == "left") {
            set(Rect{r.r0, r.c0, r.r1, r.c0}, model::kLeft);
            if (side.style == model::LineStyle::None && r.c0 > 0) set(Rect{r.r0, r.c0 - 1, r.r1, r.c0 - 1}, model::kRight);
          } else if (e == "right") {
            set(Rect{r.r0, r.c1, r.r1, r.c1}, model::kRight);
            if (side.style == model::LineStyle::None && r.c1 + 1 < col_lim) set(Rect{r.r0, r.c1 + 1, r.r1, r.c1 + 1}, model::kLeft);
          } else if (e == "top") {
            set(Rect{r.r0, r.c0, r.r0, r.c1}, model::kTop);
            if (side.style == model::LineStyle::None && r.r0 > 0) set(Rect{r.r0 - 1, r.c0, r.r0 - 1, r.c1}, model::kBottom);
          } else if (e == "bottom") {
            set(Rect{r.r1, r.c0, r.r1, r.c1}, model::kBottom);
            if (side.style == model::LineStyle::None && r.r1 + 1 < row_lim) set(Rect{r.r1 + 1, r.c0, r.r1 + 1, r.c1}, model::kTop);
          } else if (e == "inside_v" && r.c0 == 0 && r.c1 == col_lim - 1) {
            // whole rows: every cell's left and right (Excel 16, measured), as whole columns' horizontal lines below
            set(r, model::kLeft);
            set(r, model::kRight);
          } else if (e == "inside_v" && r.c1 > r.c0) {
            set(Rect{r.r0, r.c0, r.r1, r.c1 - 1}, model::kRight);
          } else if (e == "inside_h" && r.r0 == 0 && r.r1 == row_lim - 1) {
            set(r, model::kTop);
            set(r, model::kBottom);
          } else if (e == "inside_h" && r.r1 > r.r0) {
            set(Rect{r.r0, r.c0, r.r1 - 1, r.c1}, model::kBottom);
          } else if (e == "diag_down") {
            set(r, model::kDiagDown);
          } else if (e == "diag_up") {
            set(r, model::kDiagUp);
          }
        }
      }
    });
    return {};
  }
  if (m == "range.merge" || m == "range.unmerge") {
    const Rect r = RectOf(*p.Get("rect"));
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      if (m == "range.merge") d.Merge(sheet, r, p.GetBool("across"), false, p.GetBool("property"));
      else d.Unmerge(sheet, r);
    });
    return {};
  }
  if (m == "range.insert" || m == "range.delete") {
    Rect r = RectOf(*p.Get("rect"));
    std::string mode = p.GetStr("mode");
    bool table_rows = false;  // Insert without Shift inside a table's data: its rows (columns), as Excel 16 does
    if (mode == "down_auto" || mode == "right_auto") {
      const bool down = mode == "down_auto";
      mode = down ? "down" : "right";
      eng.Call([&](Document& d) {
        const model::Sheet* s = d.S(sheet);
        if (!s) return;
        for (const auto& tb : *s->tables) {
          const Rect body = tb.Data();
          if (!body.Contains(r)) continue;
          r = down ? Rect{r.r0, tb.range.c0, r.r1, tb.range.c1} : Rect{tb.range.r0, r.c0, tb.range.r1, r.c1};
          table_rows = down;
          break;
        }
      });
    }
    const int origin = static_cast<int>(p.GetInt("origin", 0));  // CopyOrigin: 0 above / left, 1 below / right
    eng.Call([&](Document& d) {
      d.ClearUndo();
      const bool ins = m == "range.insert";
      if (mode == "left_auto") {  // Delete without Shift: up where a filter hides rows in it (Excel 16, measured)
        const model::Sheet* s = d.S(sheet);
        const std::vector<Rect> runs = s ? engine::FilterShownRuns(*s, {r}) : std::vector<Rect>{r};
        mode = runs.size() == 1 && runs[0] == r ? "left" : "up";
      }
      if (mode == "rows") {
        // whole rows of a filtered list deleted: the rows it shows, bottom up (Excel 16's object model, measured)
        std::vector<Rect> runs{r};
        // every row (EntireRow of a whole column) too: its end marker kept the filter out and the hidden rows went
        if (const model::Sheet* s = d.S(sheet); s && !ins) runs = engine::FilterShownRuns(*s, {Rect{r.r0, r.c0, std::min(r.r1, s->row_limit - 1), r.c1}});
        if (ins) d.InsertRows(sheet, r.r0, static_cast<uint32_t>(r.Rows()), origin);
        else
          for (size_t i = runs.size(); i-- > 0;) d.DeleteRows(sheet, runs[i].r0, static_cast<uint32_t>(runs[i].Rows()));
      } else if (mode == "cols") {
        if (ins) d.InsertCols(sheet, r.c0, static_cast<uint32_t>(r.Cols()), origin);
        else d.DeleteCols(sheet, r.c0, static_cast<uint32_t>(r.Cols()));
      } else if (mode == "down" || mode == "up") {
        if (!ins) d.DeleteCellsShown(sheet, r, true);
        else if (d.InsertCells(sheet, r, true, origin) && table_rows) d.FillCalculatedColumns(sheet, r.r0, static_cast<uint32_t>(r.Rows()));
      } else {
        if (ins) d.InsertCells(sheet, r, false, origin);
        else d.DeleteCellsShown(sheet, r, false);
      }
    });
    // Excel 16's object model, measured: Rows / Columns / Range.Delete end cut / copy mode; an Insert keeps it (it
    // inserts the copied cells)
    if (m == "range.delete") OnUi([](AppState& s) {
      EndCopyMode(s);
      return MV();
    });
    return {};
  }
  if (m == "range.sort") {
    const Rect r = RectOf(*p.Get("rect"));
    std::vector<engine::SortKey> keys;
    if (const MV* k = p.Get("keys")) {
      for (const MV& x : k->a) {
        if (x.a.size() < 2) continue;
        engine::SortKey key{static_cast<uint32_t>(x.a[0].i), x.a[1].b};
        if (x.a.size() >= 3 && x.a[2].t == MV::T::Int) key.custom_list = static_cast<int>(x.a[2].i);
        if (x.a.size() >= 4) {
          for (const MV& it : x.a[3].a) key.custom_items.push_back(it.s);
        }
        if (x.a.size() >= 6) {  // Sort On a color: [col, desc, list, items, 1 cell / 2 font color, ARGB]
          key.sort_on = static_cast<int>(x.a[4].Num());
          key.color = static_cast<uint32_t>(static_cast<int64_t>(x.a[5].Num()));
        }
        if (x.a.size() >= 8) {  // Sort On a conditional formatting icon: [..., 3, 0, icon set, icon (0 = the lowest)]
          key.icon_set = static_cast<int>(x.a[6].Num());
          key.icon = static_cast<int>(x.a[7].Num());
        }
        keys.push_back(std::move(key));
      }
    }
    const bool header = p.GetBool("header");
    const bool match_case = p.GetBool("match_case", false), left_to_right = p.GetBool("left_to_right", false);
    eng.Call([&](Document& d) {
      Settle(d);
      d.ClearUndo();
      d.Sort(sheet, r, keys, header, true, match_case, left_to_right);
    });
    return {};
  }
  if (m == "range.series") {  // Range.DataSeries
    const Rect r = RectOf(*p.Get("rect"));
    const bool by_cols = p.GetBool("by_cols", true), has_stop = p.Has("stop");
    const int type = static_cast<int>(p.GetInt("type")), unit = static_cast<int>(p.GetInt("unit"));
    const double step = p.GetDouble("step", 1.0), stop = p.GetDouble("stop", 0.0);
    const bool trend = p.GetBool("trend", false) && type <= 1;
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      Settle(d);  // the start values a script just wrote as formulas have their values (D2:D6 of =B2*C2 filled nothing)
      if (!trend || !d.FillTrend(sheet, r, by_cols, type == 1)) d.FillSeries(sheet, r, by_cols, type, unit, step, has_stop, stop);
    });
    return {};
  }
  if (m == "range.autofill") {
    const Rect src = RectOf(*p.Get("src")), dst = RectOf(*p.Get("dest"));
    const int type = static_cast<int>(p.GetInt("type"));
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      d.AutoFill(sheet, src, dst, type);
    });
    return {};
  }
  // ---- several workbooks: each YCGrid window holds one (Workbooks.Open / Add, Workbook.Close / Activate) ----
  if (m == "app.pid") return MV::Int(static_cast<int64_t>(::GetCurrentProcessId()));
  if (m == "wb.open_window" || m == "wb.new_window") {
    const std::wstring path = FromUtf8(p.GetStr("path"));
    const std::string password = p.GetStr("password");
    std::string err;
    eng.Sync();
    MV r = OnUi([&err, path, password, open = m == "wb.open_window"](AppState& s) {
      trust::AutomationScope automation;  // a script opens it: Application.AutomationSecurity decides whether its code runs
      MV x = MV::Map();
      std::error_code ec;
      if (open && !s.file_path.empty() && std::filesystem::equivalent(s.file_path, path, ec)) {
        x.Set("here", MV::Bool(true));
      } else if (!WorkbookInUse(s)) {
        // an empty Book1 is replaced, like Excel
        const bool ok = open ? OpenWorkbook(s, path, password, false, &err) : NewWorkbook(s);
        if (!ok && err.empty()) err = "The workbook couldn't be opened";
        x.Set("here", MV::Bool(true));
      } else {
        if (open && !OpenPasswordOk(path, password)) {
          err = password.empty() ? "The workbook is password-protected: pass Password" : "The password you supplied is not correct";
        } else {
          uint32_t pid = 0;
          if (!OpenInNewWindow(open ? path : std::wstring(), open ? password : std::string(), false, &pid)) err = "A new YCGrid window couldn't be started";
          // the window's process: Workbooks.Add waits for that one, not for any YCGrid that starts meanwhile
          x.Set("pid", MV::Int(pid));
        }
        x.Set("here", MV::Bool(false));
      }
      return x;
    });
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return r;
  }
  if (m == "ui.activate") {
    return OnUi([](AppState& s) {
      if (::IsIconic(s.hwnd)) ::ShowWindow(s.hwnd, SW_RESTORE);
      ::SetForegroundWindow(s.hwnd);
      return MV();
    });
  }
  if (m == "wb.close") {
    // SaveChanges: True saves first, False discards, nothing asks (like Excel)
    const MV* save = p.Get("save");
    const int mode = !save || save->IsNil() ? 0 : (save->b ? 1 : 2);
    // keep: the window stays with a new blank workbook, like Excel after its last workbook closes (Application.Quit
    // closes the window); otherwise the window closes (another window's workbook)
    const bool keep = p.GetBool("keep", false);
    eng.Sync();
    std::string why;
    MV r = OnUi([mode, keep, &why](AppState& s) {
      s.snap = s.engine->Current();
      const bool dirty = s.snap && s.snap->wb.dirty;
      // a dialog inside this call would block the caller (and a window nobody sees would never answer)
      if (s.headless && ((mode == 0 && dirty) || (mode == 1 && s.file_path.empty()))) {
        why = mode == 0 ? "The workbook has unsaved changes: pass SaveChanges=True or False"
                        : "The workbook has no file name yet: use SaveAs first";
        return MV();
      }
      if (mode == 1) {  // a save that fails raises (Excel), and the workbook stays open
        std::string said;
        s.quiet_error = &said;
        const bool saved = SaveWorkbook(s, false);
        s.quiet_error = nullptr;
        if (!saved) {
          if (const size_t at = said.rfind("\n\n"); at != std::string::npos) said = said.substr(at + 2);  // the reason
          why = "Can't save the workbook" + (said.empty() ? std::string(": it wasn't saved, so it stays open") : ": " + said);
          return MV();
        }
      }
      if (keep) {
        if (mode == 0 && dirty) {
          s.new_workbook_later = true;  // asks "Save changes?" on the next frame, after this call returns (like Excel)
          ui::RequestRedrawIn(0.0);
          return MV::Bool(true);
        }
        if (mode != 0) {
          s.engine->Call([](Document& d) { d.wb_mut().dirty = false; });
          s.engine->Sync();
          s.snap = s.engine->Current();
        }
        return MV::Bool(NewWorkbook(s));
      }
      if (mode == 2) s.close_without_asking = true;
      ::PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
      return MV::Bool(true);
    });
    if (!why.empty()) {
      *ec = "E_APP";
      *em = why;
      return MV();
    }
    return r;
  }
  // ---- links to other workbooks (Workbook.LinkSources, UpdateLink, ChangeLink, BreakLink) ----
  if (m.rfind("links.", 0) == 0) {
    const std::string name = p.GetStr("name");
    auto find = [&](Document& d) -> int {
      const auto& links = *d.wb().external_links;
      for (size_t i = 0; i < links.size(); ++i) {
        if (links[i].path.empty()) continue;
        const std::string fn = ToUtf8(std::filesystem::path(FromUtf8(links[i].path)).filename().wstring());
        if (engine::Lower(links[i].path) == engine::Lower(name) || engine::Lower(fn) == engine::Lower(name)) return static_cast<int>(i);
      }
      return -1;
    };
    if (m == "links.list") {
      return eng.Call([&](Document& d) {
        MV a = MV::Array();
        for (const auto& l : *d.wb().external_links) {
          if (l.path.empty()) continue;
          MV x = MV::Map();
          x.Set("path", MV::Str(l.path));
          x.Set("updated", MV::Double(l.updated));
          x.Set("error", MV::Str(l.error));
          a.Push(std::move(x));
        }
        return a;
      });
    }
    if (m == "links.update") {
      eng.Sync();
      std::string err;
      const int idx = name.empty() ? -2 : eng.Call(find);
      if (idx == -1) {
        *ec = "E_ARG";
        *em = "No link to " + name;
        return {};
      }
      MV r = OnUi([&err, idx](AppState& s) {
        s.snap = s.engine->Current();
        const bool ok = idx == -2 ? UpdateAllLinks(s, &err) : UpdateLink(s, static_cast<size_t>(idx), &err);
        return MV::Bool(ok);
      });
      eng.Sync();
      if (!err.empty()) {
        *ec = "E_APP";
        *em = err;
      }
      return r;
    }
    if (m == "links.change" || m == "links.break") {
      const std::string to = p.GetStr("new_name");
      const bool ok = eng.Call([&](Document& d) {
        const int idx = find(d);
        if (idx < 0) return false;
        d.ForeignWrite();
        if (m == "links.break") d.BreakLink(static_cast<size_t>(idx));
        else d.ChangeLinkSource(static_cast<size_t>(idx), to);
        return true;
      });
      if (!ok) {
        *ec = "E_ARG";
        *em = "No link to " + name;
        return {};
      }
      if (m == "links.change") {
        eng.Sync();  // the new source is in the snapshot the update reads
        OnUi([](AppState& s) {
          s.snap = s.engine->Current();
          UpdateAllLinks(s, nullptr);
          return MV();
        });
        eng.Sync();
      }
      return {};
    }
  }
  // ---- Data View: columnar data in a window (yc.data.view) ----
  if (m.rfind("dataview.", 0) == 0) {
    std::string err;
    const MV pc = p;
    NpyPreload files;  // twenty million rows are read here, not on the UI thread
    if (const MV* cols = m == "dataview.open" ? p.Get("columns") : nullptr) {
      for (const MV& col : cols->a) files.Add(FromUtf8(col.GetStr("path")), static_cast<int>(col.GetInt("column", -1)));
    }
    MV r = OnUi([&err, m, pc](AppState& s) { return DataViewRequest(s, m, pc, &err); });
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return r;
  }
  // ---- grouped sheets, Fill Across Worksheets, Justify, Insert Copied Cells, objects / outline display (cells_ui.cpp)
  if (m.rfind("cells.", 0) == 0) {
    eng.Sync();
    std::string err;
    const MV pc = p;
    MV r = OnUi([&err, m, pc](AppState& s) { return CellsRequest(s, m, pc, &err); });
    eng.Sync();
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return r;
  }
  // ---- Screenshot: Range / Worksheet / Workbook.ExportImage, yc.ui.windows ----
  if (m.rfind("image.", 0) == 0) {
    eng.Sync();
    std::string err;
    const MV pc = p;
    MV r = OnUi([&err, m, pc](AppState& s) { return ScreenshotRequest(s, m, pc, &err); });
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return r;
  }
  // ---- CommandBars("Cell").Controls (context menus) and Application.Dialogs ----
  if (m.rfind("cmdbar.", 0) == 0) {
    std::string err;
    const MV pc = p;
    MV r = OnUi([&err, m, pc](AppState& s) { return CommandBarRequest(s, m, pc, &err); });
    if (!err.empty()) {
      *ec = "E_ARG";
      *em = err;
    }
    return r;
  }
  if (m == "ui.dialog_show") {
    std::string err;
    const int number = static_cast<int>(p.GetInt("dialog"));
    eng.Sync();
    MV r = OnUi([&err, number](AppState& s) { return MV::Bool(ShowBuiltinDialog(s, number, &err)); });
    if (!err.empty()) {
      *ec = "E_ARG";
      *em = err;
    }
    return r;
  }
  if (m == "ui.dialog_open") {
    return OnUi([](AppState& s) { return MV::Bool(DialogOpen(s)); });
  }
  // ---- custom lists (Application.AddCustomList ...) and AutoRecover: global settings ----
  if (m == "lists.get") {
    MV x = MV::Map(), lists = MV::Array();
    for (const auto& l : engine::AllFillLists()) {
      MV a = MV::Array();
      for (const auto& e : l) a.Push(MV::Str(e));
      lists.Push(std::move(a));
    }
    x.Set("lists", std::move(lists));
    x.Set("builtin", MV::Int(static_cast<int64_t>(engine::BuiltinFillLists().size())));
    return x;
  }
  if (m == "lists.set") {
    std::vector<std::vector<std::string>> user;
    if (const MV* u = p.Get("user")) {
      for (const MV& l : u->a) {
        std::vector<std::string> items;
        for (const MV& e : l.a) items.push_back(e.s);
        if (!items.empty()) user.push_back(std::move(items));
      }
    }
    return OnUi([user](AppState&) {
      Config().custom_lists = user;
      ApplyCustomListSettings();
      return MV::Bool(SaveSettings());
    });
  }
  if (m == "autorecover.get" || m == "autorecover.set") {
    const MV pc = p;
    return OnUi([pc, set = m == "autorecover.set"](AppState&) {
      auto& c = Config();
      if (set) {
        if (pc.Has("enabled")) c.autorecover = pc.GetBool("enabled");
        if (pc.Has("minutes")) c.autorecover_minutes = std::clamp(static_cast<int>(pc.GetInt("minutes")), 1, 120);
        if (pc.Has("path")) c.autorecover_folder = FromUtf8(pc.GetStr("path"));
        SaveSettings();
      }
      MV x = MV::Map();
      x.Set("enabled", MV::Bool(c.autorecover));
      x.Set("minutes", MV::Int(c.autorecover_minutes));
      x.Set("path", MV::Str(ToUtf8(AutoRecoverDir())));
      return x;
    });
  }
  if (m == "range.fill") {
    const Rect r = RectOf(*p.Get("rect"));
    const int dir = static_cast<int>(p.GetInt("dir"));
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      // a fill over a merged cell does nothing, as Excel 16's FillDown (measured: B2:B5 through B2:C3), and so does one
      // that holds a whole merged cell (measured 2026-11-20: FillUp over B3:C4 merged, or rows 3:4 through it, changes
      // nothing; it emptied B3)
      if (const model::Sheet* s = d.S(sheet)) {
        const Rect in{r.r0, r.c0, std::min(r.r1, s->row_limit - 1), std::min(r.c1, core::kMaxCols - 1)};
        for (const Rect& mg : *s->merges) {
          if (mg.Intersects(in)) return;
        }
      }
      // a filtered list: the rows it shows (Excel 16, measured: FillDown and FillUp)
      if ((dir == 0 || dir == 2) && d.FillDownShown(sheet, r, dir == 2)) return;
      if (dir == 1 || dir == 3) {  // right / left: each run of rows it shows (Excel 16's FillRight, measured)
        const model::Sheet* s = d.S(sheet);
        std::vector<Rect> runs = s && r.r1 < s->row_limit ? engine::FilterShownRuns(*s, {r}) : std::vector<Rect>{r};
        if (runs.empty()) runs = {r};  // rows the filter all hides: the cells themselves (Excel 16 fills A2:C2 left, measured)
        for (const Rect& run : runs) d.FillEdge(sheet, run, dir);
        return;
      }
      d.FillEdge(sheet, r, dir);  // down, right, up, left: stops at the sheet's end (a whole column's end marker)
    });
    return {};
  }
  if (m == "range.copy") {
    const Rect r = RectOf(*p.Get("rect"));
    const uint32_t dsheet = static_cast<uint32_t>(p.GetInt("dest_sheet", sheet));
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    const bool cut = p.GetBool("cut");
    // the whole destination (older clients send only its top-left cell)
    const Rect dest = p.Has("dest") ? RectOf(*p.Get("dest")) : Rect::Cell(row, col);
    bool off_sheet = false, cuts_merge = false, size_mismatch = false;
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      // the copy area would run past the sheet's end from the destination: Excel raises (measured), nothing changes
      if (const model::Sheet* s = d.S(sheet); s && engine::AreaRunsOffSheet(engine::CopyAreaOf(*s, r), dest.r0, dest.c0)) {
        off_sheet = true;
        return;
      }
      if (cut) {
        // part of a merged cell cut: Excel 16 refuses it (measured 2026-11-20: B3, B2:B6, A3:E3 or B:B through B3:C4)
        if (const model::Sheet* ss = d.S(sheet); ss && engine::CutsMerge(*ss, Clamp(*ss, r))) {
          cuts_merge = true;
          return;
        }
        // like Excel: the cells move, and formulas that refer to them follow
        d.MoveCells(sheet, r, dsheet, dest.r0, dest.c0);
        return;
      }
      auto clip = d.CopyShown(sheet, r);  // a filtered list: the rows it shows (Excel)
      clip.carry_notes = true;  // notes and validation come along, as with Ctrl+C / Ctrl+V
      // a destination that is a whole multiple of the source gets the source repeated (Excel behavior)
      uint32_t tile_r = 1, tile_c = 1;
      if (clip.rows && clip.cols && dest.Rows() % clip.rows == 0 && dest.Cols() % clip.cols == 0 && dest.Count() < 1000000) {
        tile_r = static_cast<uint32_t>(dest.Rows() / clip.rows);
        tile_c = static_cast<uint32_t>(dest.Cols() / clip.cols);
      }
      // a merged area the pasted cells cover part of: Excel 16 refuses the copy (measured: J2:K3 merged copied to E4
      // over F3:G5 merged, "We can't do that to a merged cell"), as YCGrid's own paste does; a cut goes through
      if (const model::Sheet* ds = d.S(dsheet)) {
        const Rect out{dest.r0, dest.c0, dest.r0 + std::max<uint32_t>(clip.rows, 1) * tile_r - 1, dest.c0 + std::max<uint32_t>(clip.cols, 1) * tile_c - 1};
        if (engine::CutsMerge(*ds, out)) {
          cuts_merge = true;
          return;
        }
        // a destination of the copy's own size over rows a filter hides: Excel 16 pastes into the rows shown and
        // refuses, their count not the copy's (measured 2026-11-21: D6:D8 copied to D2:D4 with row 3 hidden; a single
        // cell, a multiple of the copy or a destination of one cell fill every row)
        if (!dest.IsCell() && tile_r == 1 && tile_c == 1 && dest.Rows() == clip.rows && dest.Cols() == clip.cols) {
          const std::vector<Rect> shown = engine::FilterShownRuns(*ds, {dest});
          if (!(shown.size() == 1 && shown[0] == dest)) {
            size_mismatch = true;
            return;
          }
        }
      }
      d.PasteRepeated(dsheet, dest.r0, dest.c0, tile_r, tile_c, clip);
    });
    if (cuts_merge) return Err(ec, em, "E_APP", "We can't do that to a merged cell."), MV();
    if (size_mismatch) {
      return Err(ec, em, "E_APP", "We can't paste here because the Copy area and paste area aren't the same size: the rows the filter shows are fewer"), MV();
    }
    if (off_sheet) {
      *ec = "E_APP";
      *em = "We can't paste here because the Copy area and paste area aren't the same size: the copied cells would run past the "
            "sheet's last row or column from the destination";
    }
    return {};
  }
  if (m == "range.replace") {
    auto areas = AreasOf(p);
    return eng.Call([&](Document& d) {
      d.ForeignWrite();
      // a filtered list: the cells it shows, as Excel 16's Range.Replace does (measured)
      if (const model::Sheet* s = d.S(sheet)) areas = engine::FilterShownRuns(*s, areas);
      uint32_t n = 0;
      const engine::FormatSpec fmt = FormatSpecOf(p.Get("fmt")), repl_fmt = FormatSpecOf(p.Get("repl_fmt"));
      for (const Rect& r : areas) n += d.Replace(sheet, r, p.GetStr("what"), p.GetStr("repl"), p.GetBool("match_case"), p.GetBool("whole"), &fmt, &repl_fmt);
      return MV::Int(n);
    });
  }
  if (m == "range.note") {
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c2 = static_cast<uint32_t>(p.GetInt("col"));
    const std::string text = p.GetStr("text");
    eng.Call([&](Document& d) {
      if (model::Sheet* s = d.M(sheet)) {
        s->notes.mut()[(uint64_t{r} << 16) | c2] = model::CellNote{"Python", text};
        d.wb_mut().dirty = true;
      }
    });
    return {};
  }
  if (m == "dims.set") {
    const bool col = p.GetStr("kind") == "col";
    const uint32_t first = static_cast<uint32_t>(p.GetInt("first")), last = static_cast<uint32_t>(p.GetInt("last"));
    eng.Call([&](Document& d) {
      d.ForeignWrite();
      if (p.Has("hidden")) {
        if (col) d.HideCols(sheet, first, last, p.GetBool("hidden"));
        else d.HideRows(sheet, first, last, p.GetBool("hidden"));
      } else if (col) {
        d.SetColWidth(sheet, first, last, render::CharsToColPx(p.GetDouble("chars")));
      } else {
        d.SetRowHeight(sheet, first, std::min<uint32_t>(last, d.S(sheet) ? d.S(sheet)->row_limit - 1 : last), static_cast<int32_t>(std::lround(p.GetDouble("points") / 0.75)));
      }
    });
    return {};
  }
  if (m == "dims.autofit") {
    const bool col = p.GetStr("kind") == "col";
    const uint32_t first = static_cast<uint32_t>(p.GetInt("first")), last = static_cast<uint32_t>(p.GetInt("last"));
    // Range("A2:A3").Columns.AutoFit: only those rows' cells (Excel); without rows the whole column
    std::vector<Rect> rows;
    if (col && p.Has("row0")) rows.push_back(Rect{static_cast<uint32_t>(p.GetInt("row0")), first, static_cast<uint32_t>(p.GetInt("row1")), last});
    // a protected sheet that doesn't allow formatting columns (rows) refuses it here, as Excel 16's AutoFit raises
    // (measured 2026-11-20); the fit the UI posted was refused later, where the request no longer saw it
    if (eng.Call([&](Document& d) { return d.RefuseFormatDims(sheet, col); })) return MV();
    // measure what Python just wrote: wait for the engine, then use its latest snapshot
    // (the UI thread's snapshot can still be from before those writes)
    eng.Sync();
    return OnUi([=](AppState& s) {
      s.snap = s.engine->Current();
      AutoFitDims(s, sheet, col, {{first, last}}, rows.empty() ? nullptr : &rows);  // every row / column of the range, one undo step
      return MV();
    });
  }

  // ---- Names, KV, document properties ----
  if (m == "names.list") {
    // name list: name, refers_to (with =), scope (-1 workbook / sheet id), scope_name, comment, visible
    const core::CellPos at = ActiveCellNow();
    return eng.Call([&](Document& d) {
      MV out = MV::Array();
      for (const auto& n : *d.wb().names) {
        MV x = MV::Map();
        x.Set("name", MV::Str(n.name));
        // relative references shown relative to the active cell, as Excel's RefersTo gives them (kept relative to A1)
        const std::string shown = formula::MoveRelativeText("=" + n.refers_to, core::CellPos{0, 0}, at, &d, false, true);
        x.Set("refers_to", MV::Str(ShownUnionText(shown, &d)));
        formula::ParseResult pr = formula::Parse(std::string_view(shown).substr(1), at, &d);
        const std::string r1c1 = pr.ok ? "=" + formula::ToR1C1(pr.root, at, &d) : shown;
        x.Set("refers_to_r1c1", MV::Str(pr.ok && KeptUnion(pr.root) && r1c1.size() > 5 ? "=" + r1c1.substr(3, r1c1.size() - 5) : r1c1));
        x.Set("scope", MV::Int(n.sheet_scope));
        const model::Sheet* sc = n.sheet_scope >= 0 ? d.S(static_cast<uint32_t>(n.sheet_scope)) : nullptr;
        x.Set("scope_name", MV::Str(sc ? sc->name : ""));
        x.Set("comment", MV::Str(n.comment));
        x.Set("visible", MV::Bool(!n.hidden));
        out.Push(std::move(x));
      }
      return out;
    });
  }
  if (m == "names.add" || m == "names.update") {
    // add: replaces a name with the same name and scope (like Excel's Names.Add); update: rename, change refers-to, scope or comment (renaming updates formulas)
    std::string err;
    const bool update = m == "names.update";
    const core::CellPos at = ActiveCellNow();
    const bool ok = eng.Call([&](Document& d) {
      model::NameDef def;
      const model::NameDef* old = nullptr;
      if (update) {
        const std::string on = engine::Lower(p.GetStr("old_name"));
        const int32_t os = static_cast<int32_t>(p.GetInt("old_scope", -1));
        for (const auto& n : *d.wb().names) {
          if (n.sheet_scope == os && engine::Lower(n.name) == on) old = &n;
        }
        if (!old) {
          err = "Name not found: \"" + p.GetStr("old_name") + "\".";
          return false;
        }
        def = *old;
      }
      if (p.Has("name")) def.name = p.GetStr("name");
      // typed relative to the active cell (Excel's Names.Add), kept relative to A1; r1c1: RefersToR1C1
      if (p.Has("refers_to")) {
        // a union written without parentheses (=Data!$A$1,Data!$A$3) is taken, as in Excel
        const std::string typed = WrapUnionText(p.GetStr("refers_to"), &d, p.GetBool("r1c1"));
        def.refers_to = formula::MoveRelativeText(typed, at, core::CellPos{0, 0}, &d, p.GetBool("r1c1"));
      }
      if (p.Has("scope")) def.sheet_scope = static_cast<int32_t>(p.GetInt("scope", -1));
      if (p.Has("comment")) def.comment = p.GetStr("comment");
      if (p.Has("visible")) def.hidden = !p.GetBool("visible");
      if (def.sheet_scope >= 0 && !d.S(static_cast<uint32_t>(def.sheet_scope))) {
        err = "The sheet doesn't exist.";
        return false;
      }
      // the refers-to formula resolves relative references against the name's scope (or the caller's current sheet)
      d.Begin(update ? "Edit Name" : "Define Name");
      const bool k = update ? d.UpdateName(old->name, old->sheet_scope, def, &err) : d.DefineName(def, &err);
      d.Commit();
      return k;
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return {};
  }
  if (m == "names.delete") {
    eng.Call([&](Document& d) {
      d.Begin("Delete Name");
      d.DeleteName(p.GetStr("name"), static_cast<int32_t>(p.GetInt("scope", -1)));
      d.Commit();
    });
    return {};
  }
  if (m == "names.resolve") {
    // name -> range: sheet-level names first, supports "Sheet!Name" and dynamic names (OFFSET, INDEX...)
    const core::CellPos at = ActiveCellNow();
    return eng.Call([&](Document& d) {
      uint32_t sid = 0;
      Rect r;
      // a union (=Data!$A$1,Data!$A$3): its areas, as Excel's RefersToRange gives them
      if (const model::NameDef* n = d.FindName(p.GetStr("name"), sheet)) {
        const uint32_t base = n->sheet_scope >= 0 ? static_cast<uint32_t>(n->sheet_scope) : sheet;
        const std::string text = formula::MoveRelativeText("=" + n->refers_to, core::CellPos{0, 0}, at, &d, false, true);
        formula::ParseResult pr = formula::Parse(std::string_view(text).substr(1), at, &d);
        if (const formula::Node* u = pr.ok ? KeptUnion(pr.root) : nullptr) {
          std::vector<const formula::Node*> parts;
          FlattenUnion(*u, &parts);
          MV areas = MV::Array();
          int64_t first_sheet = -1;
          for (const formula::Node* part : parts) {
            uint32_t ps = 0;
            Rect pr2;
            if (!d.EvaluateRef(base, formula::ToA1(*part, at, &d), &ps, &pr2)) return MV();
            if (first_sheet < 0) {
              first_sheet = ps;
              r = pr2;
            } else if (first_sheet != static_cast<int64_t>(ps)) {
              return MV();  // areas on different sheets make no range
            }
            areas.Push(rect4(pr2));
          }
          MV x = MV::Map();
          x.Set("sheet", MV::Int(first_sheet));
          x.Set("rect", rect4(r));
          x.Set("areas", std::move(areas));
          return x;
        }
      }
      if (!d.ResolveNameRef(p.GetStr("name"), sheet, &sid, &r, nullptr, at)) {
        // a table and its structured references (Range("Trades[Qty]"), Goto "Trades"): Excel 16's object model takes
        // them, the table's name alone being its data (measured: Goto "Trades" selects $B$3:$D$6)
        const std::string text = p.GetStr("name");
        const size_t br = text.find('[');
        if (!model::FindTable(d.wb(), std::string_view(text).substr(0, br))) return MV();
        if (!d.EvaluateRef(sheet, text, &sid, &r)) return MV();
      }
      MV x = MV::Map();
      x.Set("sheet", MV::Int(sid));
      x.Set("rect", rect4(r));
      return x;
    });
  }
  if (m == "names.value") {
    // current value of a name (for constant or formula names; range names return None and Python reads the range)
    const core::CellPos at = ActiveCellNow();
    return eng.Call([&](Document& d) {
      const model::NameDef* n = d.FindName(p.GetStr("name"), sheet);
      if (!n) return MV();
      uint32_t sid = 0;
      Rect r;
      const std::string text = formula::MoveRelativeText(n->refers_to, core::CellPos{0, 0}, at, &d, false, true);
      if (d.EvaluateRef(n->sheet_scope >= 0 ? static_cast<uint32_t>(n->sheet_scope) : sheet, text, &sid, &r)) return MV();
      std::string err;
      const core::Value v = d.EvaluateFormula(n->sheet_scope >= 0 ? static_cast<uint32_t>(n->sheet_scope) : sheet, text, &err);
      return ValueToMV(v);
    });
  }
  if (m == "names.create_from_selection") {
    const Rect r = RectOf(*p.Get("rect"));
    const bool top = p.GetBool("top"), left = p.GetBool("left"), bottom = p.GetBool("bottom"), right = p.GetBool("right");
    return eng.Call([&](Document& d) {
      d.Begin("Create Names from Selection");
      int64_t created = 0;
      for (auto& def : d.NamesFromSelection(sheet, r, top, left, bottom, right)) {
        if (d.DefineName(def)) ++created;
      }
      d.Commit();
      return MV::Int(created);
    });
  }
  if (m == "sheet.circles") {  // Worksheet.CircleInvalid / ClearCircles
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    const bool circle = p.GetBool("circle", true);
    return OnUi([id, circle](AppState& s) {
      if (!circle) {
        ClearValidationCircles(s, id);
        return MV();
      }
      return MV::Int(CircleInvalidData(s, id));
    });
  }
  if (m == "range.differences") {
    // Range.RowDifferences / ColumnDifferences(Comparison)
    const Rect req = RectOf(*p.Get("rect"));
    const uint32_t cr = static_cast<uint32_t>(p.GetInt("row")), cc = static_cast<uint32_t>(p.GetInt("col"));
    const bool rows = p.GetBool("rows", true);
    return eng.Call([&](Document& d) {
      Settle(d);
      MV out = MV::Array();
      const model::Sheet* s = d.S(sheet);
      if (!s) return out;
      Rect w = req;
      if (w.IsCell()) w = engine::CurrentRegion(*s, w.r0, w.c0);
      for (const Rect& r : engine::DifferenceCells(*s, w, cr, cc, rows)) {
        MV x = MV::Array();
        x.Push(MV::Int(r.r0));
        x.Push(MV::Int(r.c0));
        x.Push(MV::Int(r.r1));
        x.Push(MV::Int(r.c1));
        out.Push(std::move(x));
      }
      return out;
    });
  }
  if (m == "range.special") {
    // Range.SpecialCells: kind is an XlCellType, values an XlSpecialCellsValue
    const Rect req = RectOf(*p.Get("rect"));
    const int64_t kind = p.GetInt("kind", 4);
    const int values = static_cast<int>(p.GetInt("values", 1 | 2 | 4 | 16));
    return eng.Call([&](Document& d) {
      Settle(d);
      const model::Sheet* s = d.S(sheet);
      MV out = MV::Array();
      if (!s) return out;
      Rect w = Clamp(*s, req);
      const bool one = w.IsCell();
      // a single cell = the whole sheet (like Excel); notes, rules and validation can sit on empty cells outside the used
      // range, so for them the whole grid, as Go To Special does
      const bool on_empty = kind == -4144 || kind == -4172 || kind == -4173 || kind == -4174 || kind == -4175;
      // visible cells of one cell: that cell when nothing is hidden, else the visible cells of the whole sheet (Excel 16,
      // measured: Range("C5").SpecialCells(xlCellTypeVisible) is $C$5, or 1:3,5:1048576 with row 4 hidden)
      const Rect all{0, 0, s->row_limit - 1, s->col_limit - 1};
      if (one && on_empty) {
        w = all;
      } else if (one && kind == 12) {
        const std::vector<Rect> vis = engine::SpecialCells(*s, all, engine::Special::Visible, values);
        if (vis.size() != 1 || !(vis[0] == all)) w = all;
      } else if (one && s->has_used) {
        w = Rect{0, 0, s->used.r1, s->used.c1};
      }
      if (kind == -4173 || kind == -4175) {
        // Same: the cells that share a rule with the anchor cell (Go To Special > Conditional formats / Data validation > Same)
        const uint32_t ar = static_cast<uint32_t>(p.GetInt("anchor_row", req.r0)), ac = static_cast<uint32_t>(p.GetInt("anchor_col", req.c0));
        std::vector<Rect> areas;
        auto take = [&](const std::vector<Rect>& ranges) {
          bool has = false;
          for (const Rect& q : ranges) has = has || q.Contains(ar, ac);
          if (!has) return;
          for (const Rect& q : ranges) {
            if (!q.Intersects(w)) continue;
            areas.push_back(Rect{std::max(q.r0, w.r0), std::max(q.c0, w.c0), std::min(q.r1, w.r1), std::min(q.c1, w.c1)});
          }
        };
        if (kind == -4173) {
          for (const auto& rule : *s->cond_formats) take(rule.ranges);
        } else {
          take(engine::SameValidationAreas(*s, ar, ac, &d));
        }
        std::sort(areas.begin(), areas.end(), [](const Rect& x, const Rect& y) { return x.r0 != y.r0 ? x.r0 < y.r0 : x.c0 < y.c0; });
        areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
        for (const Rect& r : areas) out.Push(rect4(r));
        return out;
      }
      engine::Special k = engine::Special::Blanks;
      switch (kind) {
        case 2: k = engine::Special::Constants; break;
        case -4123: k = engine::Special::Formulas; break;
        case -4144: k = engine::Special::Comments; break;
        case 11: k = engine::Special::LastCell; break;
        case 12: k = engine::Special::Visible; break;
        case -4172: k = engine::Special::AllFormatConditions; break;
        case -4174: k = engine::Special::AllValidation; break;
        default: k = engine::Special::Blanks; break;
      }
      for (const Rect& r : engine::SpecialCells(*s, w, k, values)) out.Push(rect4(r));
      return out;
    });
  }
  if (m == "range.precedents" || m == "range.dependents") {
    const Rect req = RectOf(*p.Get("rect"));
    const bool direct = p.GetBool("direct");
    const bool prec = m == "range.precedents";
    return eng.Call([&](Document& d) {
      MV out = MV::Array();
      const model::Sheet* s = d.S(sheet);
      if (!s) return out;
      const Rect r = Clamp(*s, req);
      for (const Rect& x : prec ? d.Precedents(sheet, r, direct) : d.Dependents(sheet, r, direct)) out.Push(rect4(x));
      return out;
    });
  }
  if (m == "store.get" || m == "store.set" || m == "store.delete" || m == "store.keys") {
    const bool ws = p.Has("sheet");
    return eng.Call([&](Document& d) {
      std::map<std::string, std::string>* mp = nullptr;
      const std::map<std::string, std::string>* cmp = nullptr;
      if (ws) {
        const model::Sheet* s = d.S(sheet);
        if (!s) return MV();
        cmp = &*s->store;
        if (m == "store.set" || m == "store.delete") mp = &d.M(sheet)->store.mut();
      } else {
        cmp = &*d.wb().store;
        if (m == "store.set" || m == "store.delete") mp = &d.wb_mut().store.mut();
      }
      const std::string key = p.GetStr("key");
      if (m == "store.get") {
        auto it = cmp->find(key);
        return it == cmp->end() ? MV() : MV::Bin(it->second);
      }
      if (m == "store.keys") {
        MV out = MV::Array();
        for (const auto& [k, v] : *cmp) out.Push(MV::Str(k));
        return out;
      }
      d.wb_mut().dirty = true;  // KV writes mark the workbook unsaved but don't go on the undo stack
      if (m == "store.set") {
        const MV* v = p.Get("value");
        (*mp)[key] = v ? v->s : std::string("\xc0", 1);
        return MV::Bool(true);
      }
      return MV::Bool(mp->erase(key) > 0);
    });
  }
  if (m == "props.list" || m == "props.get" || m == "props.set" || m == "props.delete") {
    return eng.Call([&](Document& d) {
      const std::string name = p.GetStr("name");
      if (m == "props.list") {
        MV out = MV::Array();
        for (const auto& [k, v] : *d.wb().doc_props) out.Push(MV::Str(k));
        return out;
      }
      if (m == "props.get") {
        auto it = d.wb().doc_props->find(name);
        return it == d.wb().doc_props->end() ? MV() : ValueToMV(it->second.value);
      }
      d.wb_mut().dirty = true;
      if (m == "props.set") {
        int dk;
        d.wb_mut().doc_props.mut()[name].value = MVToValue(p.Get("value") ? *p.Get("value") : MV::Nil(), &dk);
        return MV::Bool(true);
      }
      return MV::Bool(d.wb_mut().doc_props.mut().erase(name) > 0);
    });
  }

  // ---- Calculation ----
  if (m == "eval") {
    std::string err;
    const std::string text = p.GetStr("formula");
    const MV v = eng.Call([&](Document& d) {
      uint32_t sid = sheet;
      if (!sid || !d.S(sid)) sid = s_.focus_sheet;
      // an array result (Evaluate("{1,2,3}*2"), WorksheetFunction.Transpose) comes back whole, as rows
      std::shared_ptr<formula::Array> arr;
      const core::Value one = d.EvaluateFormula(sid, text, &err, &arr);
      if (!arr) return ValueToMV(one);
      MV rows = MV::Array();
      for (uint32_t r = 0; r < arr->rows; ++r) {
        MV row = MV::Array();
        for (uint32_t c = 0; c < arr->cols; ++c) row.Push(ValueToMV(arr->At(r, c)));
        rows.Push(std::move(row));
      }
      return rows;
    });
    if (!err.empty()) return Err(ec, em, "E_FORMULA", err), MV();
    return v;
  }
  if (m == "format") {
    int dk;
    const core::Value v = MVToValue(p.Get("value") ? *p.Get("value") : MV::Nil(), &dk);
    return MV::Str(formula::FormatValue(v, p.GetStr("format")).text);
  }
  // Range.ApplyNames: one undo step
  if (m == "names.apply") {
    std::vector<std::string> names;
    if (const MV* a = p.Get("names")) {
      for (const MV& x : a->a) names.push_back(x.s);
    }
    std::vector<Rect> rects;
    if (const MV* a = p.Get("rects")) {
      for (const MV& x : a->a) {
        if (x.a.size() == 4)
          rects.push_back(Rect{static_cast<uint32_t>(x.a[0].i), static_cast<uint32_t>(x.a[1].i), static_cast<uint32_t>(x.a[2].i), static_cast<uint32_t>(x.a[3].i)});
      }
    }
    Document::ApplyNamesOptions opt;
    opt.ignore_relative = p.GetBool("ignore_relative", true);
    opt.use_row_column = p.GetBool("use_row_column", true);
    opt.omit_column = p.GetBool("omit_column", true);
    opt.omit_row = p.GetBool("omit_row", true);
    opt.row_first = p.GetInt("order", 1) != 2;  // xlRowThenColumn 1, xlColumnThenRow 2
    return MV::Int(eng.Call([&](Document& d) {
      const bool whole = rects.size() == 1 && rects[0].IsCell();
      d.Begin("Apply Names");
      int k = 0;
      if (whole) k = d.ApplyNames(sheet, Rect{0, 0, core::kRowEnd, core::kMaxCols - 1}, names, opt);
      else
        for (const Rect& r : rects) k += d.ApplyNames(sheet, r, names, opt);
      d.Commit();
      return k;
    }));
  }
  // formula auditing: Range.ShowPrecedents / ShowDependents, Worksheet.ClearArrows (drawn by the UI)
  if (m == "audit.trace") {
    const bool prec = p.GetBool("precedents", true), remove = p.GetBool("remove", false), errors = p.GetBool("errors", false);
    const uint32_t ar = static_cast<uint32_t>(p.GetInt("row")), ac = static_cast<uint32_t>(p.GetInt("col"));
    return OnUi([sheet, ar, ac, prec, remove, errors](AppState& s) {
      if (errors) {  // Range.ShowErrors: Trace Error from the cell
        s.focus_sheet = sheet;
        render::SelectRange(s.Win(sheet).view, Rect::Cell(ar, ac));
        ui::Commands().Execute("TraceError");
        return MV::Int(static_cast<int64_t>(s.audit_arrows.size()));
      }
      if (remove) {
        auto& arr = s.audit_arrows;
        arr.erase(std::remove_if(arr.begin(), arr.end(), [&](const AppState::AuditArrow& a) {
                    return a.sheet == sheet && (prec ? (a.to_r == ar && a.to_c == ac) : (a.from == Rect::Cell(ar, ac)));
                  }), arr.end());
        return MV();
      }
      s.focus_sheet = sheet;
      auto& sel = s.Win(sheet).view.sel;
      render::SelectRange(s.Win(sheet).view, Rect::Cell(ar, ac));
      (void)sel;
      ui::Commands().Execute(prec ? "TracePrecedents" : "TraceDependents");
      return MV::Int(static_cast<int64_t>(s.audit_arrows.size()));
    });
  }
  if (m == "audit.clear") {
    return OnUi([sheet](AppState& s) {
      auto& arr = s.audit_arrows;
      arr.erase(std::remove_if(arr.begin(), arr.end(), [&](const AppState::AuditArrow& a) { return a.sheet == sheet; }), arr.end());
      s.audit_prec_frontier.clear();
      s.audit_dep_frontier.clear();
      return MV();
    });
  }
  if (m == "audit.arrows") {
    return OnUi([](AppState& s) {
      MV out = MV::Array();
      for (const auto& a : s.audit_arrows) {
        MV x = MV::Array();
        x.Push(MV::Int(a.sheet));
        x.Push(MV::Int(a.from.r0));
        x.Push(MV::Int(a.from.c0));
        x.Push(MV::Int(a.from.r1));
        x.Push(MV::Int(a.from.c1));
        x.Push(MV::Int(a.to_r));
        x.Push(MV::Int(a.to_c));
        // [7]: the references on other sheets of a dashed arrow to a sheet icon (empty for an arrow on the sheet)
        MV refs = MV::Array();
        for (const std::string& r : a.other_refs) refs.Push(MV::Str(r));
        x.Push(std::move(refs));
        out.Push(std::move(x));
      }
      return out;
    });
  }
  // Application.Watches
  if (m == "watch.list" || m == "watch.add" || m == "watch.delete") {
    const uint32_t ar = static_cast<uint32_t>(p.GetInt("row")), ac = static_cast<uint32_t>(p.GetInt("col"));
    const int64_t index = p.GetInt("index", -1);
    return OnUi([m, sheet, ar, ac, index](AppState& s) {
      if (s.engine) FollowStructureChanges(s, *s.engine->Current());  // rows inserted just now (Excel's watch is a reference)
      if (m == "watch.add") {
        bool dup = false;
        for (const auto& w : s.watches) dup = dup || (w.sheet == sheet && w.r == ar && w.c == ac);
        if (!dup) s.watches.push_back({sheet, ar, ac});
        s.show_watch = true;
      } else if (m == "watch.delete") {
        if (index < 0) s.watches.clear();
        else if (index < static_cast<int64_t>(s.watches.size())) s.watches.erase(s.watches.begin() + index);
      }
      MV out = MV::Array();
      for (const auto& w : s.watches) {
        MV x = MV::Array();
        x.Push(MV::Int(w.sheet));
        x.Push(MV::Int(w.r));
        x.Push(MV::Int(w.c));
        out.Push(std::move(x));
      }
      return out;
    });
  }
  // ---- tables: Worksheet.ListObjects ----
  if (m.rfind("table.", 0) == 0) {
    std::string error;
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id", 0));
    auto table_mv = [&](const model::Table& t) {
      MV x = MV::Map();
      x.Set("id", MV::Int(t.id));
      x.Set("name", MV::Str(t.name));
      x.Set("range", rect4(t.range));
      x.Set("header", MV::Bool(t.header));
      x.Set("totals", MV::Bool(t.totals));
      x.Set("style", MV::Str(t.style));
      x.Set("banded_rows", MV::Bool(t.banded_rows));
      x.Set("banded_cols", MV::Bool(t.banded_cols));
      x.Set("first_col", MV::Bool(t.first_col));
      x.Set("last_col", MV::Bool(t.last_col));
      x.Set("filter_button", MV::Bool(t.filter_button));
      MV cols = MV::Array();
      for (const auto& c : t.columns) {
        MV e = MV::Array();
        e.Push(MV::Str(c.name));
        e.Push(MV::Int(c.totals_func));
        e.Push(MV::Str(c.totals_label));
        cols.Push(std::move(e));
      }
      x.Set("columns", std::move(cols));
      return x;
    };
    MV out = eng.Call([&](Document& d) -> MV {
      if (m == "table.list") {
        MV a = MV::Array();
        if (const model::Sheet* s = d.S(sheet)) {
          for (const auto& t : *s->tables) a.Push(table_mv(t));
        }
        return a;
      }
      d.Begin(m);
      MV r;
      if (m == "table.add") {
        const auto areas = AreasOf(p);
        if (areas.size() > 1) error = "This can't be done on a multiple range selection. Select a single range and try again.";  // Excel 16 refuses several areas (measured)
        // the headers become the text their cells show: formulas this connection just wrote have their values first
        // (=SUM(B2:B5) became "0" and the next formulas "02", "03"; Excel 16 makes them "70", "2.69", measured)
        if (areas.size() == 1) Settle(d);
        // xlGuess: Excel's guess of a header row, the Create Table dialog's (engine::GuessHeaderRow)
        const bool headers = p.GetBool("guess", false) && areas.size() == 1 && d.S(sheet) ? engine::GuessHeaderRow(*d.wb().styles, *d.S(sheet), areas[0])
                                                                                            : p.GetBool("headers", true);
        const uint32_t nid = areas.size() != 1 ? 0 : d.AddTable(sheet, areas[0], headers, p.Has("style") ? p.GetStr("style") : model::DefaultTableStyle(*d.wb().store), &error);
        if (nid && p.Get("name") && !p.GetStr("name").empty()) d.RenameTable(sheet, nid, p.GetStr("name"), &error);
        r = nid ? table_mv(*d.TableById(sheet, nid)) : MV();
      } else if (m == "table.update") {
        const MV* pr = p.Get("props");
        if (pr && pr->Get("name")) {
          if (!d.RenameTable(sheet, id, pr->GetStr("name"), &error) && error.empty()) error = "That table name isn't valid";
        }
        if (error.empty() && pr) {
          d.UpdateTable(sheet, id, [&](model::Table& t) {
            if (pr->Get("style")) t.style = pr->GetStr("style");
            if (pr->Get("header")) t.header = pr->GetBool("header");
            if (pr->Get("totals")) t.totals = pr->GetBool("totals");
            if (pr->Get("banded_rows")) t.banded_rows = pr->GetBool("banded_rows");
            if (pr->Get("banded_cols")) t.banded_cols = pr->GetBool("banded_cols");
            if (pr->Get("first_col")) t.first_col = pr->GetBool("first_col");
            if (pr->Get("last_col")) t.last_col = pr->GetBool("last_col");
            if (pr->Get("filter_button")) t.filter_button = pr->GetBool("filter_button");
          });
          // the filter buttons are the sheet's AutoFilter on the table (as Table Design > Filter Button does)
          if (pr->Get("filter_button")) {
            if (const model::Table* t = d.TableById(sheet, id)) d.SetTableFilterButton(sheet, id, t->filter_button);  // or the table's own buttons
          }
        }
      } else if (m == "table.resize") {
        const auto areas = AreasOf(p);
        if (areas.size() > 1) error = "This can't be done on a multiple range selection. Select a single range and try again.";  // Excel 16 refuses several areas (measured)
        else if (areas.empty() || !d.ResizeTable(sheet, id, areas[0], &error)) {
          if (error.empty()) error = "The table can't be resized to that range";
        }
      } else if (m == "table.delete") {
        const model::Table* t = d.TableById(sheet, id);
        const Rect range = t ? t->range : Rect{};
        d.DeleteTable(sheet, id, p.GetBool("keep_look", true));
        if (t && p.GetBool("clear", false)) d.ClearAll(sheet, range);
      } else if (m == "table.total") {
        d.SetTableTotal(sheet, id, static_cast<uint32_t>(p.GetInt("col")), static_cast<int>(p.GetInt("func")), p.GetStr("label"));
      } else if (m == "table.row") {
        const bool add = p.GetBool("add", true);
        const bool ok = add ? d.TableInsertRow(sheet, id, p.GetInt("pos", -1)) : d.TableDeleteRow(sheet, id, static_cast<uint32_t>(p.GetInt("pos", 0)));
        if (!ok) error = "The table row can't be changed";
      } else if (m == "table.column") {
        const bool add = p.GetBool("add", true);
        const bool ok = add ? d.TableInsertColumn(sheet, id, p.GetInt("pos", -1)) : d.TableDeleteColumn(sheet, id, static_cast<uint32_t>(p.GetInt("pos", 0)));
        if (!ok) error = "The table column can't be changed";
      }
      d.Commit();
      return r;
    });
    if (!error.empty()) return Err(ec, em, "E_APP", error), MV();
    return out;
  }
  // ---- protection: Worksheet.Protect / Unprotect / Protection, Workbook.Protect / Unprotect ----
  if (m == "protect.sheet" || m == "protect.unsheet" || m == "protect.get" || m == "protect.ranges" || m == "protect.selection") {
    std::string error;
    MV out = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      if (m == "protect.selection") {
        // Worksheet.EnableSelection: 0 xlNoRestrictions, 1 xlUnlockedCells, -4142 xlNoSelection (applies while protected)
        const int64_t v = p.GetInt("value");
        auto& pr = d.wb_mut().sheets[static_cast<size_t>(d.wb().IndexOfId(sheet))].mut().protection.mut();
        pr.select_locked = v == 0;
        pr.select_unlocked = v != -4142;
        d.wb_mut().dirty = true;
        return MV::Bool(true);
      }
      if (m == "protect.sheet") {
        model::SheetProtection o = *s->protection;
        const MV* f = p.Get("flags");
        auto flag = [&](const char* k, bool* v) {
          if (f) {
            if (const MV* x = f->Get(k)) *v = x->b;
          }
        };
        flag("format_cells", &o.format_cells);
        flag("format_columns", &o.format_columns);
        flag("format_rows", &o.format_rows);
        flag("insert_columns", &o.insert_columns);
        flag("insert_rows", &o.insert_rows);
        flag("insert_hyperlinks", &o.insert_hyperlinks);
        flag("delete_columns", &o.delete_columns);
        flag("delete_rows", &o.delete_rows);
        flag("sort", &o.sort);
        flag("autofilter", &o.autofilter);
        flag("pivot", &o.pivot);
        flag("objects", &o.objects);
        flag("scenarios", &o.scenarios);
        flag("select_locked", &o.select_locked);
        flag("select_unlocked", &o.select_unlocked);
        o.ui_only = p.GetBool("ui_only", false);
        // protecting again without a password keeps the old one (like Excel's Protect UserInterfaceOnly:=True)
        const bool has_pw = p.Get("password") && p.Get("password")->t == MV::T::Str;
        const uint16_t keep = s->protection->hash;
        d.ProtectSheet(sheet, o, has_pw ? p.GetStr("password") : std::string());
        if (!has_pw && s->protection->on) d.wb_mut().sheets[static_cast<size_t>(d.wb().IndexOfId(sheet))].mut().protection.mut().hash = keep;
        return MV::Bool(true);
      }
      if (m == "protect.unsheet") {
        if (!d.UnprotectSheet(sheet, p.GetStr("password"))) error = "The password you supplied is not correct";
        return MV::Bool(error.empty());
      }
      if (m == "protect.ranges") {
        std::vector<model::AllowEditRange> ranges;
        if (const MV* rv = p.Get("ranges"); rv && rv->t == MV::T::Array) {
          for (const MV& e : rv->a) {
            if (e.a.size() < 6) continue;
            model::AllowEditRange er;
            er.title = e.a[0].s;
            er.rect = Rect{static_cast<uint32_t>(e.a[1].i), static_cast<uint32_t>(e.a[2].i), static_cast<uint32_t>(e.a[3].i), static_cast<uint32_t>(e.a[4].i)};
            er.hash = e.a[5].t == MV::T::Str ? model::ProtectionHash(e.a[5].s) : static_cast<uint16_t>(e.a[5].i);
            if (e.a.size() >= 7 && e.a[6].t == MV::T::Array) {
              for (const MV& u : e.a[6].a) {
                if (u.a.size() >= 2) er.users.push_back({u.a[0].s, u.a[1].b});
              }
            }
            ranges.push_back(std::move(er));
          }
        }
        d.SetAllowEditRanges(sheet, std::move(ranges));
        return MV::Bool(true);
      }
      const auto& pr = *s->protection;
      MV x = MV::Map();
      x.Set("on", MV::Bool(pr.on));
      x.Set("has_password", MV::Bool(pr.hash != 0));
      x.Set("ui_only", MV::Bool(pr.ui_only));
      MV f = MV::Map();
      f.Set("format_cells", MV::Bool(pr.format_cells));
      f.Set("format_columns", MV::Bool(pr.format_columns));
      f.Set("format_rows", MV::Bool(pr.format_rows));
      f.Set("insert_columns", MV::Bool(pr.insert_columns));
      f.Set("insert_rows", MV::Bool(pr.insert_rows));
      f.Set("insert_hyperlinks", MV::Bool(pr.insert_hyperlinks));
      f.Set("delete_columns", MV::Bool(pr.delete_columns));
      f.Set("delete_rows", MV::Bool(pr.delete_rows));
      f.Set("sort", MV::Bool(pr.sort));
      f.Set("autofilter", MV::Bool(pr.autofilter));
      f.Set("pivot", MV::Bool(pr.pivot));
      f.Set("objects", MV::Bool(pr.objects));
      f.Set("scenarios", MV::Bool(pr.scenarios));
      f.Set("select_locked", MV::Bool(pr.select_locked));
      f.Set("select_unlocked", MV::Bool(pr.select_unlocked));
      x.Set("flags", std::move(f));
      MV rs = MV::Array();
      for (const auto& er : pr.ranges) {
        MV e = MV::Array();
        e.Push(MV::Str(er.title));
        e.Push(MV::Int(er.rect.r0));
        e.Push(MV::Int(er.rect.c0));
        e.Push(MV::Int(er.rect.r1));
        e.Push(MV::Int(er.rect.c1));
        e.Push(MV::Int(er.hash));
        MV us = MV::Array();
        for (const auto& u : er.users) {
          MV one = MV::Array();
          one.Push(MV::Str(u.name));
          one.Push(MV::Bool(u.allow));
          us.Push(std::move(one));
        }
        e.Push(std::move(us));
        rs.Push(std::move(e));
      }
      x.Set("ranges", std::move(rs));
      return x;
    });
    if (!error.empty()) return Err(ec, em, "E_PASSWORD", error), MV();
    return out;
  }
  if (m == "protect.workbook" || m == "protect.unworkbook" || m == "protect.wbget") {
    std::string error;
    MV out = eng.Call([&](Document& d) {
      if (m == "protect.workbook") {
        d.ProtectWorkbook(p.GetStr("password"), p.GetBool("structure", true), p.GetBool("windows", false));
        return MV::Bool(true);
      }
      if (m == "protect.unworkbook") {
        if (!d.UnprotectWorkbook(p.GetStr("password"))) error = "The password you supplied is not correct";
        return MV::Bool(error.empty());
      }
      MV x = MV::Map();
      x.Set("structure", MV::Bool(d.wb().protect_structure));
      x.Set("windows", MV::Bool(d.wb().protect_windows));
      return x;
    });
    if (!error.empty()) return Err(ec, em, "E_PASSWORD", error), MV();
    return out;
  }
  // Application.AutoCorrect: {"add": [what, with]} / {"delete": what}; returns the list
  if (m == "app.autocorrect") {
    MV params = p;
    return OnUi([params](AppState&) {
      if (const MV* a = params.Get("add"); a && a->t == MV::T::Array && a->a.size() == 2) AutoCorrectAdd(a->a[0].s, a->a[1].s);
      if (params.Has("delete")) AutoCorrectDelete(params.GetStr("delete"));
      if (params.Has("replace_text")) {
        Config().autocorrect_replace = params.GetBool("replace_text");
        SaveSettings();
      }
      if (params.Has("get_replace_text")) return MV::Bool(Config().autocorrect_replace);
      const auto ac = AutoCorrectList();
      MV out = MV::Array();
      for (const auto& [a, b] : ac) {
        MV e = MV::Array();
        e.Push(MV::Str(a));
        e.Push(MV::Str(b));
        out.Push(std::move(e));
      }
      return out;
    });
  }
  // Application.ReferenceStyle (File -> Options, saved to settings.json): {"r1c1": bool} sets it; returns the current one
  if (m == "app.reference_style") {
    const bool set = p.Has("r1c1"), v = p.GetBool("r1c1", false);
    return OnUi([set, v](AppState&) {
      if (set && Config().r1c1 != v) {
        Config().r1c1 = v;
        SaveSettings();
      }
      return MV::Bool(Config().r1c1);
    });
  }
  if (m == "app.edit_options") {  // Application.MoveAfterReturn(Direction), EditDirectlyInCell, EnableAutoComplete
    MV params = p;
    return OnUi([params](AppState&) {
      Settings& c = Config();
      bool changed = false;
      auto flag = [&](const char* key, bool* v) {
        if (params.Has(key) && *v != params.GetBool(key)) *v = params.GetBool(key), changed = true;
      };
      flag("move_after_return", &c.move_after_return);
      flag("edit_directly_in_cell", &c.edit_directly_in_cell);
      flag("enable_autocomplete", &c.enable_autocomplete);
      flag("extend_list", &c.extend_list);
      flag("auto_flash_fill", &c.auto_flash_fill);
      flag("show_quick_analysis", &c.show_quick_analysis);
      flag("alert_before_overwriting", &c.alert_before_overwriting);
      flag("large_operation_alert", &c.large_operation_alert);
      flag("show_chart_tip_names", &c.show_chart_tip_names);
      flag("show_chart_tip_values", &c.show_chart_tip_values);
      flag("calculate_before_save", &c.calculate_before_save);
      flag("format_stale_values", &c.format_stale_values);
      flag("generate_getpivotdata", &c.generate_getpivotdata);
      flag("generate_table_refs", &c.generate_table_refs);
      flag("autoformat_hyperlinks", &c.autoformat_hyperlinks);
      const bool threads_were = c.multi_threaded_calc;
      const int count_was = c.calc_thread_count;
      flag("multi_threaded_calc", &c.multi_threaded_calc);
      if (params.Has("calc_thread_count")) c.calc_thread_count = std::clamp(static_cast<int>(params.GetInt("calc_thread_count")), 0, 1024), changed = true;
      if (threads_were != c.multi_threaded_calc || count_was != c.calc_thread_count) ApplyCalcThreads();
      if (params.Has("large_operation_thousands")) {
        c.large_operation_thousands = std::clamp(static_cast<int>(params.GetInt("large_operation_thousands")), 1, 1000000000);
        changed = true;
      }
      if (params.Has("standard_font") && params.GetStr("standard_font") != c.standard_font) c.standard_font = params.GetStr("standard_font"), changed = true;
      if (params.Has("standard_font_size")) c.standard_font_size = std::clamp(params.GetDouble("standard_font_size"), 1.0, 409.0), changed = true;
      if (params.Has("sheets_in_new_workbook")) c.sheets_in_new_workbook = std::clamp(static_cast<int>(params.GetInt("sheets_in_new_workbook")), 1, 255), changed = true;
      if (params.Has("direction")) {
        const int d = std::clamp(static_cast<int>(params.GetInt("direction")), 0, 3);
        if (d != c.move_after_return_dir) c.move_after_return_dir = d, changed = true;
      }
      if (changed) SaveSettings();
      MV x = MV::Map();
      x.Set("move_after_return", MV::Bool(c.move_after_return));
      x.Set("direction", MV::Int(c.move_after_return_dir));
      x.Set("edit_directly_in_cell", MV::Bool(c.edit_directly_in_cell));
      x.Set("enable_autocomplete", MV::Bool(c.enable_autocomplete));
      x.Set("extend_list", MV::Bool(c.extend_list));
      x.Set("auto_flash_fill", MV::Bool(c.auto_flash_fill));
      x.Set("show_quick_analysis", MV::Bool(c.show_quick_analysis));
      x.Set("alert_before_overwriting", MV::Bool(c.alert_before_overwriting));
      x.Set("large_operation_alert", MV::Bool(c.large_operation_alert));
      x.Set("large_operation_thousands", MV::Int(c.large_operation_thousands));
      x.Set("show_chart_tip_names", MV::Bool(c.show_chart_tip_names));
      x.Set("show_chart_tip_values", MV::Bool(c.show_chart_tip_values));
      x.Set("calculate_before_save", MV::Bool(c.calculate_before_save));
      x.Set("format_stale_values", MV::Bool(c.format_stale_values));
      x.Set("generate_getpivotdata", MV::Bool(c.generate_getpivotdata));
      x.Set("generate_table_refs", MV::Bool(c.generate_table_refs));
      x.Set("autoformat_hyperlinks", MV::Bool(c.autoformat_hyperlinks));
      x.Set("multi_threaded_calc", MV::Bool(c.multi_threaded_calc));
      x.Set("calc_thread_count", MV::Int(c.calc_thread_count));
      x.Set("pool_threads", MV::Int(static_cast<int64_t>(core::PoolSize())));
      x.Set("calc_threads", MV::Int(static_cast<int64_t>(core::WorkerCount())));
      x.Set("standard_font", MV::Str(c.standard_font.empty() ? std::string("Calibri") : c.standard_font));
      x.Set("standard_font_size", MV::Double(c.standard_font_size > 0 ? c.standard_font_size : 11.0));
      x.Set("sheets_in_new_workbook", MV::Int(c.sheets_in_new_workbook));
      return x;
    });
  }
  if (m == "app.cut_copy_mode") {  // Application.CutCopyMode: 1 copy, 2 cut, 0 off; setting False ends it
    const bool end = p.Has("off") && p.GetBool("off");
    return OnUi([end](AppState& s) {
      if (end) EndCopyMode(s);
      return MV::Int(CopyModeOn(s) ? (s.clip->cut ? 2 : 1) : 0);
    });
  }
  // Range.Errors(i): the error checking rule the cell breaks (whatever the options) and whether it is ignored
  if (m == "cell.error_check") {
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    return OnUi([sheet, row, col](AppState& s) {
      MV x = MV::Map();
      const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
      x.Set("rule", MV::Int(sh ? engine::ErrorCheckRule(s.snap->wb, *sh, row, col, engine::ErrorCheckOptions{}, false) : 0));
      // every rule the cell breaks (Range.Errors(i).Value), each checked on its own whatever the options
      MV rules = MV::Array();
      for (int k = 1; sh && k < engine::kErrRuleCount; ++k) {
        engine::ErrorCheckOptions one;
        one.evaluate_to_error = one.text_date = one.number_as_text = one.inconsistent_formula = one.omitted_cells = one.unlocked_formula =
            one.empty_cell_reference = one.list_data_validation = one.inconsistent_list_formula = one.misleading_format = false;
        switch (k) {
          case engine::kErrEvaluateToError: one.evaluate_to_error = true; break;
          case engine::kErrTextDate: one.text_date = true; break;
          case engine::kErrNumberAsText: one.number_as_text = true; break;
          case engine::kErrInconsistentFormula: one.inconsistent_formula = true; break;
          case engine::kErrOmittedCells: one.omitted_cells = true; break;
          case engine::kErrUnlockedFormula: one.unlocked_formula = true; break;
          case engine::kErrEmptyCellReference: one.empty_cell_reference = true; break;
          case engine::kErrListDataValidation: one.list_data_validation = true; break;
          case engine::kErrMisleadingFormat: one.misleading_format = true; break;
          default: one.inconsistent_list_formula = true; break;
        }
        if (engine::ErrorCheckRule(s.snap->wb, *sh, row, col, one, false) == k) rules.Push(MV::Int(k));
      }
      x.Set("rules", std::move(rules));
      x.Set("ignored", MV::Bool(sh && engine::ErrorIgnored(*sh, row, col)));
      return x;
    });
  }
  if (m == "cell.ignore_errors") {  // Range.Errors(i).Ignore = True / False
    Rect rc{};
    if (const MV* a = p.Get("rect"); a && a->a.size() == 4) {
      rc = Rect{static_cast<uint32_t>(a->a[0].i), static_cast<uint32_t>(a->a[1].i), static_cast<uint32_t>(a->a[2].i), static_cast<uint32_t>(a->a[3].i)};
    }
    const bool on = p.GetBool("on", true);
    eng.Call([&](Document& d) {
      d.Begin("Ignore Error");
      d.SetIgnoreErrors(sheet, rc, on);
      d.Commit();
      return true;
    });
    return MV();
  }
  // Application.ErrorCheckingOptions (File > Options > Formulas; saved)
  if (m == "app.error_checking") {
    MV params = p;
    return OnUi([params](AppState&) {
      auto set = [&](const char* k, bool* v) {
        if (params.Has(k)) *v = params.GetBool(k);
        return params.Has(k);
      };
      bool any = set("background", &Config().error_checking);
      any = set("evaluate_to_error", &Config().err_evaluate_to_error) || any;
      any = set("number_as_text", &Config().err_number_as_text) || any;
      any = set("inconsistent_formula", &Config().err_inconsistent_formula) || any;
      any = set("text_date", &Config().err_text_date) || any;
      any = set("omitted_cells", &Config().err_omitted_cells) || any;
      any = set("unlocked_formula", &Config().err_unlocked_formula) || any;
      any = set("empty_cell_reference", &Config().err_empty_cell_reference) || any;
      any = set("list_data_validation", &Config().err_list_data_validation) || any;
      any = set("inconsistent_list_formula", &Config().err_inconsistent_list_formula) || any;
      any = set("misleading_format", &Config().err_misleading_format) || any;
      if (any) SaveSettings();
      MV x = MV::Map();
      x.Set("background", MV::Bool(Config().error_checking));
      x.Set("evaluate_to_error", MV::Bool(Config().err_evaluate_to_error));
      x.Set("number_as_text", MV::Bool(Config().err_number_as_text));
      x.Set("inconsistent_formula", MV::Bool(Config().err_inconsistent_formula));
      x.Set("text_date", MV::Bool(Config().err_text_date));
      x.Set("omitted_cells", MV::Bool(Config().err_omitted_cells));
      x.Set("unlocked_formula", MV::Bool(Config().err_unlocked_formula));
      x.Set("empty_cell_reference", MV::Bool(Config().err_empty_cell_reference));
      x.Set("list_data_validation", MV::Bool(Config().err_list_data_validation));
      x.Set("inconsistent_list_formula", MV::Bool(Config().err_inconsistent_list_formula));
      x.Set("misleading_format", MV::Bool(Config().err_misleading_format));
      return x;
    });
  }
  // Application.DisplayFormulaAutoComplete, DisplayFunctionToolTips, FormulaBarHeight (File > Options; saved)
  if (m == "app.formula_options") {
    const bool set_ac = p.Has("autocomplete"), set_tips = p.Has("tooltips"), set_lines = p.Has("bar_lines");
    const bool ac = p.GetBool("autocomplete", true), tips = p.GetBool("tooltips", true);
    const int lines = static_cast<int>(std::clamp<int64_t>(p.GetInt("bar_lines", 1), 1, 20));
    return OnUi([=](AppState&) {
      if (set_ac) Config().formula_autocomplete = ac;
      if (set_tips) Config().function_tooltips = tips;
      if (set_lines) Config().formula_bar_lines = lines;
      if (set_ac || set_tips || set_lines) SaveSettings();
      MV x = MV::Map();
      x.Set("autocomplete", MV::Bool(Config().formula_autocomplete));
      x.Set("tooltips", MV::Bool(Config().function_tooltips));
      x.Set("bar_lines", MV::Int(Config().formula_bar_lines));
      return x;
    });
  }
  // [YC] Application.TruncateTo15Digits: File > Options > Calculation > "Truncate numbers longer than 15 significant digits"
  if (m == "app.truncate_15_digits") {
    const bool set = p.Has("on"), v = p.GetBool("on", false);
    return OnUi([set, v](AppState& s) {
      if (set && Config().excel_15_digit_input != v) {
        Config().excel_15_digit_input = v;
        s.engine->Post([v](Document& d) { d.excel_15_digit_input = v; });
        SaveSettings();
      }
      return MV::Bool(Config().excel_15_digit_input);
    });
  }
  // Application.AutomationSecurity: how workbooks opened by scripts treat their Python code (not saved, as in Excel)
  if (m == "app.automation_security") {
    const bool set = p.Has("value");
    const int64_t v = p.GetInt("value", 1);
    if (set && (v < 1 || v > 3)) return Err(ec, em, "E_ARG", "AutomationSecurity: use msoAutomationSecurityLow (1), msoAutomationSecurityByUI (2) or msoAutomationSecurityForceDisable (3)"), MV();
    return OnUi([set, v](AppState&) {
      if (set) trust::SetAutomationSecurity(static_cast<int>(v));
      return MV::Int(trust::AutomationSecurity());
    });
  }
  // [YC] Application.TrustCenter: the Trust Center Settings (File > Options > Trust Center; saved for this computer)
  if (m == "app.trust_center") {
    std::string err;
    MV r = OnUi([&p, &err](AppState& s) { return trust::CenterRequest(s, p, &err); });
    if (!err.empty()) return Err(ec, em, "E_ARG", err), MV();
    return r;
  }
  if (m == "app.trust_state") return OnUi([](AppState&) { return trust::StateMV(); });
  // Application.ProtectedViewWindows: this window when its workbook is in Protected View; edit = Enable Editing
  if (m == "app.protected_view") {
    const bool edit = p.GetBool("edit");
    return OnUi([edit](AppState& s) {
      if (edit) trust::EnableEditing(s);
      MV x = MV::Map();
      x.Set("active", MV::Bool(trust::InProtectedView()));
      x.Set("path", MV::Str(ToUtf8(s.file_path)));
      x.Set("reason", MV::Str(trust::StateMV().GetStr("protected_view_reason")));
      return x;
    });
  }
  if (m == "app.cell_drag_drop") {  // Application.CellDragAndDrop (File > Options; saved for this computer)
    const bool set = p.Has("on"), v = p.GetBool("on", true);
    return OnUi([set, v](AppState&) {
      if (set && Config().cell_drag_and_drop != v) {
        Config().cell_drag_and_drop = v;
        SaveSettings();
      }
      return MV::Bool(Config().cell_drag_and_drop);
    });
  }
  if (m == "calc.get") {
    return eng.Call([&](Document& d) {
      MV x = MV::Map();
      x.Set("auto", MV::Bool(d.auto_calc));
      x.Set("data_tables", MV::Bool(d.auto_data_tables));
      x.Set("iterate", MV::Bool(d.iterate));
      x.Set("max_iterations", MV::Int(d.max_iterations));
      x.Set("max_change", MV::Double(d.max_change));
      // Application.CalculationState: formulas waiting (manual calculation; with automatic calculation they are done by
      // the time any request reads them)
      x.Set("pending", MV::Bool(!d.auto_calc && d.has_pending()));
      return x;
    });
  }
  // the focused sheet (ActiveSheet of unqualified Range / Cells): answered here, no UI round trip
  if (m == "view.focus") {
    const uint32_t f = ui_focus_sheet_.load();
    if (f != UINT32_MAX && eng.Current()->Sheet(f)) return MV::Int(f);  // (a sheet just deleted: ask the UI)
    return OnUi([](AppState& s) { return MV::Int(s.focus_sheet); });
  }
  // Application.PublishInterval [YC]: get / set the publish interval (ms) of live changes for this session
  // (the saved default is in settings.json, changed from File -> Options)
  if (m == "engine.publish_interval") {
    if (p.Has("ms")) eng.SetPublishInterval(static_cast<int>(p.GetInt("ms", 100)));
    return MV::Int(eng.PublishInterval());
  }
  // only the given keys change (Application.Calculation / Iteration / MaxIterations / MaxChange)
  if (m == "calc.set") {
    eng.Call([&](Document& d) {
      // switching to manual: what was entered before is calculated first (Excel leaves the workbook calculated)
      if (p.Has("auto") && !p.GetBool("auto", true) && d.auto_calc) d.Recalc();
      if (p.Has("auto")) d.auto_calc = p.GetBool("auto", true);
      if (p.Has("data_tables")) d.auto_data_tables = p.GetBool("data_tables", true);
      if (p.Has("iterate")) d.iterate = p.GetBool("iterate", false);
      if (p.Has("max_iterations")) d.max_iterations = std::clamp(static_cast<int>(p.GetInt("max_iterations", 100)), 1, 32767);
      if (const MV* v = p.Get("max_change")) d.max_change = v->Num() > 0 ? v->Num() : 0.001;
      // with iteration on, Excel calculates the workbook again (the cycles go on with the new settings); turning it
      // off calculates nothing: the cycles keep the values iteration gave them
      if ((p.Has("iterate") || p.Has("max_iterations") || p.Has("max_change")) && d.iterate) d.RecalcAll();
    });
    // File > Options shows the session's state, as Excel's does after code changed it (not saved: a script's
    // temporary Manual doesn't become the user's default)
    MV params = p;
    OnUi([params](AppState&) {
      Settings& c = Config();
      if (params.Has("auto")) c.auto_calc = params.GetBool("auto", true);
      if (params.Has("data_tables")) c.auto_data_tables = params.GetBool("data_tables", true);
      if (params.Has("iterate")) c.iterative = params.GetBool("iterate", false);
      if (params.Has("max_iterations")) c.max_iterations = std::clamp(static_cast<int>(params.GetInt("max_iterations", 100)), 1, 32767);
      if (const MV* v = params.Get("max_change")) c.max_change = v->Num() > 0 ? v->Num() : 0.001;
      return MV();
    });
    return {};
  }
  if (m == "calc.dirty") {  // Range.Dirty with manual calculation: the range's formulas wait for the next Calculate
    const Rect r = RectOf(*p.Get("rect"));
    eng.Call([&](Document& d) { d.MarkDirty(sheet, r); });
    return {};
  }
  if (m == "calc.run") {
    const bool full = p.GetBool("full");
    const bool one_sheet = p.Has("sheet");  // Worksheet.Calculate: only that sheet, as Excel
    const MV* rect = p.Get("rect");         // Range.Calculate: only the formulas of the range
    eng.Call([&](Document& d) {
      if (full) d.RecalcAll();
      else if (one_sheet && rect) d.RecalcRange(sheet, RectOf(*rect));
      else if (one_sheet) d.RecalcSheet(sheet);
      else d.CalculateNow();
    });
    return {};
  }

  // ---- View (UI thread) ----
  if (m == "view.get") {
    const bool has_sheet = p.Has("sheet");
    return OnUi([sheet, has_sheet, rect4](AppState& s) {
      const uint32_t sid = has_sheet && s.snap && s.snap->Sheet(sheet) ? sheet : s.focus_sheet;
      auto& w = s.Win(sid);
      const auto& v = s.ViewFor(sid);  // the active window's, as Excel's Selection
      MV x = MV::Map();
      x.Set("sheet", MV::Int(sid));
      x.Set("focus_sheet", MV::Int(s.focus_sheet));
      x.Set("active_sheet", MV::Int(s.active_sheet));
      MV areas = MV::Array();
      // whole columns (Ctrl+Space, a column header) are full-height areas: Python's whole-column form, so Selection reads
      // "$C:$C" as in Excel (it read "$C$1:$C$1048576")
      const model::Sheet* vs = s.snap ? s.snap->Sheet(sid) : nullptr;
      for (Rect a : render::AreasInExcelOrder(v.sel)) {  // as Excel lists them (the active one first after a Select)
        if (vs && a.r0 == 0 && vs->row_limit > 0 && a.r1 >= vs->row_limit - 1) a.r1 = 0xFFFFFFFFu;
        areas.Push(rect4(a));
      }
      x.Set("areas", std::move(areas));
      MV act = MV::Array();
      act.Push(MV::Int(v.sel.act_r));
      act.Push(MV::Int(v.sel.act_c));
      x.Set("active", std::move(act));
      // a split window gives its top-left pane's first row and column, a frozen one its scrolling pane's (Excel 16, measured)
      x.Set("scroll_row", MV::Int(v.split && v.freeze_rows ? int64_t{v.frozen_top} : v.v.first));
      x.Set("scroll_col", MV::Int(v.split && v.freeze_cols ? int64_t{v.frozen_left} : v.h.first));
      x.Set("zoom", MV::Double(v.zoom));
      x.Set("page_view", MV::Int(v.page_view));
      MV fr = MV::Array();
      fr.Push(MV::Int(v.freeze_rows));
      fr.Push(MV::Int(v.freeze_cols));
      x.Set("freeze", std::move(fr));
      x.Set("split", MV::Bool(v.split));
      // the panes (Window.Panes): the top / left part starts at frozen_top / frozen_left, the lower / right at first_*
      MV pn = MV::Array();
      pn.Push(MV::Int(v.frozen_top));
      pn.Push(MV::Int(v.frozen_left));
      pn.Push(MV::Int(v.v.first));
      pn.Push(MV::Int(v.h.first));
      x.Set("panes", std::move(pn));
      if (v.TLayout()) {  // [YC] Window.CenterColumns
        MV cc = MV::Array();
        cc.Push(MV::Int(v.frozen_left));
        cc.Push(MV::Int(v.freeze_cols));
        x.Set("center", std::move(cc));
      }
      x.Set("gridlines", MV::Bool(w.gridlines));
      x.Set("headings", MV::Bool(w.headings));
      x.Set("formulas", MV::Bool(w.formulas));
      x.Set("zeros", MV::Bool(w.zeros));
      x.Set("page_breaks", MV::Bool(w.page_breaks));
      x.Set("gridline_color", MV::Int(w.gridline_color));
      x.Set("sheet_tabs", MV::Bool(s.show_sheet_tabs));
      x.Set("hscroll", MV::Bool(s.show_hscroll));
      x.Set("vscroll", MV::Bool(s.show_vscroll));
      x.Set("floating", MV::Bool(w.floating));
      x.Set("visible", rect4(Rect{static_cast<uint32_t>(v.v.first), static_cast<uint32_t>(v.h.first), static_cast<uint32_t>(v.last_bottom_row),
                                  static_cast<uint32_t>(v.last_right_col)}));
      return x;
    });
  }
  if (m == "view.set") {
    MV params = p;
    return OnUi([params](AppState& s) {
      if (params.Has("activate_sheet")) ActivateSheet(s, static_cast<uint32_t>(params.GetInt("activate_sheet")));
      const uint32_t sid = params.Has("sheet") && s.snap->Sheet(static_cast<uint32_t>(params.GetInt("sheet"))) ? static_cast<uint32_t>(params.GetInt("sheet")) : s.focus_sheet;
      auto& v = s.ViewFor(sid);  // the active window's, as Excel's Select
      const model::Sheet* sh = s.snap->Sheet(sid);
      if (!sh) return MV();
      if (const MV* sel = params.Get("select")) {
        // the first area's first cell is active, as Excel's Range("B2,D5").Select makes B2 active (measured)
        std::vector<Rect> given;
        for (const MV& a : sel->a) {
          Rect r = RectOf(a);
          if (r.r1 >= sh->row_limit) r.r1 = sh->row_limit - 1;
          given.push_back(r);
        }
        if (given.empty()) given.push_back(Rect{});
        render::SetAreasActiveFirst(v.sel, given);
        render::NormalizeWholeSelection(v, *sh);  // Range("C:C").Select(): whole columns, as in Excel
        if (v.sel.areas.size() > 1) {  // Excel's order again, over the areas as normalized
          v.sel.excel_order.assign(1, v.sel.areas.back());
          v.sel.excel_order.insert(v.sel.excel_order.end(), v.sel.areas.begin(), v.sel.areas.end() - 1);
        }
        const Rect f = v.sel.areas.back();
        render::EnsureVisible(*sh, v, f.r0, f.c0);
      }
      if (const MV* a = params.Get("active")) {
        const uint32_t r = static_cast<uint32_t>(a->a[0].i), c = static_cast<uint32_t>(a->a[1].i);
        bool inside = false;
        for (const Rect& ar : v.sel.areas) inside = inside || ar.Contains(r, c);
        if (inside) {
          v.sel.act_r = r;
          v.sel.act_c = c;
        } else {
          render::SelectCell(v, r, c);
        }
        render::EnsureVisible(*sh, v, r, c);
      }
      if (const MV* e = params.Get("ensure_visible")) render::EnsureVisible(*sh, v, static_cast<uint32_t>(e->a[0].i), static_cast<uint32_t>(e->a[1].i));
      if (const MV* iv = params.Get("into_view"); iv && iv->a.size() >= 5) {
        // Window.ScrollIntoView(Left, Top, Width, Height, Start): a rectangle of the sheet in points; Start keeps its
        // top-left corner in view first, else its bottom-right
        auto num = [&](size_t i) { return iv->a[i].IsNum() ? (iv->a[i].t == MV::T::Int ? static_cast<double>(iv->a[i].i) : iv->a[i].d) : 0.0; };
        const double l = std::max(0.0, num(0)), t = std::max(0.0, num(1)), w = std::max(0.0, num(2)), h = std::max(0.0, num(3));
        auto row = [&](double pt) { return static_cast<uint32_t>(std::clamp<int64_t>(sh->rows->IndexAt(static_cast<int64_t>(pt / 0.75)), 0, sh->row_limit - 1)); };
        auto col = [&](double pt) { return static_cast<uint32_t>(std::clamp<int64_t>(sh->cols->IndexAt(static_cast<int64_t>(pt / 0.75)), 0, sh->col_limit - 1)); };
        const bool start = iv->a[4].t == MV::T::Bool ? iv->a[4].b : true;
        render::EnsureVisible(*sh, v, start ? row(t + h) : row(t), start ? col(l + w) : col(l));
        render::EnsureVisible(*sh, v, start ? row(t) : row(t + h), start ? col(l) : col(l + w));
      }
      // (a split window's ScrollRow / ScrollColumn are its top-left pane's, as they read)
      if (params.Has("scroll_row")) {
        const int64_t r = std::clamp<int64_t>(params.GetInt("scroll_row"), 0, sh->row_limit - 1);
        if (v.split && v.freeze_rows) v.frozen_top = static_cast<uint32_t>(r);
        else v.v.first = r;
      }
      if (params.Has("scroll_col")) {
        const int64_t c = std::clamp<int64_t>(params.GetInt("scroll_col"), 0, sh->col_limit - 1);
        if (v.split && v.freeze_cols) v.frozen_left = static_cast<uint32_t>(c);
        else v.h.first = c;
      }
      // one pane's scrolling (Pane.ScrollRow / ScrollColumn): the top or left part, or the lower or right one
      if (params.Has("top_row")) v.frozen_top = static_cast<uint32_t>(std::clamp<int64_t>(params.GetInt("top_row"), 0, sh->row_limit - 1));
      if (params.Has("left_col")) v.frozen_left = static_cast<uint32_t>(std::clamp<int64_t>(params.GetInt("left_col"), 0, sh->col_limit - 1));
      if (params.Has("lower_row")) v.v.first = std::clamp<int64_t>(params.GetInt("lower_row"), 0, sh->row_limit - 1);
      if (params.Has("right_col")) v.h.first = std::clamp<int64_t>(params.GetInt("right_col"), 0, sh->col_limit - 1);
      if (params.Has("zoom")) v.zoom = static_cast<float>(std::clamp(params.GetDouble("zoom"), 0.1, 4.0));
      if (params.Has("page_view")) v.page_view = static_cast<int>(std::clamp<int64_t>(params.GetInt("page_view"), 0, 2));
      if (params.Has("split")) {
        // Window.Split: on at the active cell (like View -> Split), off removes it
        const bool want = params.GetBool("split");
        if (want != v.split) {
          const uint32_t saved = s.focus_sheet;
          s.focus_sheet = sid;
          if (want && (v.freeze_rows || v.freeze_cols)) v.split = true;  // frozen panes become split ones
          else SplitWindow(s);
          s.focus_sheet = saved;
        }
      }
      if (const MV* f = params.Get("freeze")) {
        const bool was_split = v.split;
        v.split = false;
        v.mirror_left = false;  // a plain freeze replaces a T layout
        if (f->t == MV::T::Str) {
          if (f->s == "none") UnfreezeView(v);
          else if (was_split && (v.freeze_rows || v.freeze_cols)) {
            // like Excel: freezing a split window freezes where the split was
          } else {
            v.freeze_rows = v.freeze_cols = 0;
            const uint32_t saved = s.focus_sheet;
            s.focus_sheet = sid;
            FreezePanes(s, 0);
            s.focus_sheet = saved;
          }
        } else if (f->t == MV::T::Array && f->a.size() >= 2) {
          // the frozen (or split) part starts where it already starts, a new one where the window's top left is:
          // SplitRow then SplitColumn took the lower pane's first row for the second, and rows 1:3 went out of
          // sight (Excel 16 splits at rows 1:3 and columns A:B, the window still showing A1, measured)
          if (!v.freeze_rows) v.frozen_top = static_cast<uint32_t>(v.v.first);
          if (!v.freeze_cols) v.frozen_left = static_cast<uint32_t>(v.h.first);
          v.freeze_rows = static_cast<uint32_t>(f->a[0].i);
          v.freeze_cols = static_cast<uint32_t>(f->a[1].i);
        }
      }
      if (const MV* c = params.Get("center")) {
        // [YC] Window.CenterColumns: [first column, count] keeps them in the middle (T layout), nil ends it
        if (c->t == MV::T::Array && c->a.size() >= 2 && c->a[0].i > 0 && c->a[1].i > 0 && c->a[0].i + c->a[1].i < sh->col_limit) {
          v.split = false;
          v.frozen_left = static_cast<uint32_t>(c->a[0].i);
          v.freeze_cols = static_cast<uint32_t>(c->a[1].i);
          v.h.first = v.frozen_left + v.freeze_cols;
          v.mirror_left = true;
        } else if (v.mirror_left) {
          v.mirror_left = false;
          v.freeze_cols = 0;
        }
      }
      // per sheet, like Excel; the ribbon's check boxes show the focused sheet's
      SheetWin& sw = s.Win(sid);
      if (params.Has("gridlines")) sw.gridlines = params.GetBool("gridlines");
      if (params.Has("headings")) sw.headings = params.GetBool("headings");
      if (sid == s.focus_sheet) s.show_gridlines = sw.gridlines, s.show_headings = sw.headings;
      if (params.Has("formulas")) sw.formulas = params.GetBool("formulas");
      if (params.Has("zeros")) sw.zeros = params.GetBool("zeros");
      if (params.Has("page_breaks")) sw.page_breaks = params.GetBool("page_breaks");
      if (params.Has("gridline_color")) sw.gridline_color = static_cast<uint32_t>(params.GetInt("gridline_color")) & 0x00FFFFFFu;
      if (params.Has("sheet_tabs")) s.show_sheet_tabs = params.GetBool("sheet_tabs");
      if (params.Has("hscroll")) s.show_hscroll = params.GetBool("hscroll");
      if (params.Has("vscroll")) s.show_vscroll = params.GetBool("vscroll");
      if (sid == s.focus_sheet) s.show_formulas = sw.formulas;
      return MV();
    });
  }
  if (m == "view.float_sheet") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    const bool fl = p.GetBool("floating");
    return OnUi([id, fl](AppState& s) {
      if (!s.snap->Sheet(id)) return MV();
      s.Win(id).floating = fl;
      if (fl) s.just_torn.clear();
      else ActivateSheet(s, id);
      return MV();
    });
  }
  if (m.rfind("errors.", 0) == 0) {
    std::string e;
    MV r = ErrorRequest(m, p, &e);
    if (!e.empty()) {
      *ec = "E_ARG";
      *em = e;
    }
    return r;
  }
  // Page Layout, View tab and window members (print_ui.cpp); the window layout itself (reset / capture / apply) is
  // python_bridge_objects.cpp's, so those go on down
  if (m.rfind("layout.", 0) == 0 && m != "layout.reset" && m != "layout.capture" && m != "layout.apply") {
    std::string e;
    MV r = OnUi([&e, m, p](AppState& s) { return LayoutRequest(s, m, p, &e); });
    if (!e.empty()) {
      *ec = "E_ARG";
      *em = e;
    }
    return r;
  }
  if (m.rfind("notebook.", 0) == 0) {
    std::string e;
    MV r = OnUi([&e, m, p](AppState& s) { return NotebookRequest(s, m, p, &e); });
    if (!e.empty()) {
      *ec = "E_ARG";
      *em = e;
    }
    return r;
  }
  if (m == "ui.command") {
    const std::string id = p.GetStr("id");
    return OnUi([id](AppState&) { return MV::Bool(ui::Commands().Execute(id)); });
  }
  // ---- RibbonX (custom_ribbon.cpp) ----
  if (m == "ribbon.value") {
    const std::string key = p.GetStr("key"), value = p.GetStr("value");
    return OnUi([key, value](AppState& s) {
      SetRibbonValue(s, key, value);
      return MV::Bool(true);
    });
  }
  if (m == "ribbon.invalidate") {
    const std::string control = p.GetStr("control");
    return OnUi([control](AppState& s) {
      InvalidateRibbon(s, control);
      return MV::Bool(true);
    });
  }
  if (m == "ribbon.info") {
    return OnUi([](AppState&) { return RibbonInfo(); });
  }
  if (m == "ribbon.activate") {
    // IRibbonUI.ActivateTab (a custom tab's id) / ActivateTabMso ("TabHome"...)
    const std::string id = p.GetStr("id");
    return OnUi([id](AppState&) { return MV::Bool(ActivateRibbonTab(id)); });
  }
  if (m == "ui.command_state") {
    const std::string id = p.GetStr("id");
    return OnUi([id](AppState&) {
      MV x = MV::Map();
      if (ui::Command* c2 = ui::Commands().Find(id)) {
        x.Set("enabled", MV::Bool(c2->enabled));
        x.Set("pressed", MV::Bool(c2->pressed));
        x.Set("label", MV::Str(c2->label));
      }
      return x;
    });
  }
  if (m == "ui.interactive") {
    const bool set = p.Has("on"), on = p.GetBool("on", true);
    return OnUi([set, on](AppState& s) {
      if (set) s.interactive = on;
      ui::RequestRedrawIn(0.0);
      return MV::Bool(s.interactive);
    });
  }
  if (m == "wb.options") {
    // Workbook.Date1904, PrecisionAsDisplayed, ForceFullCalculation (saved with the workbook)
    MV params = p;
    return eng.Call([params](Document& d) {
      if (params.Has("date1904")) d.SetDate1904(params.GetBool("date1904"));
      if (params.Has("precision_as_displayed")) d.SetPrecisionAsDisplayed(params.GetBool("precision_as_displayed"));
      if (params.Has("force_full_calc")) d.SetForceFullCalculation(params.GetBool("force_full_calc"));
      d.ForeignWrite();
      MV x = MV::Map();
      x.Set("date1904", MV::Bool(d.wb().date1904));
      x.Set("precision_as_displayed", MV::Bool(d.wb().precision_as_displayed));
      x.Set("force_full_calc", MV::Bool(d.wb().force_full_calc));
      return x;
    });
  }
  if (m == "sheet.ui_enable") {
    // Worksheet.EnableAutoFilter / EnableOutlining / EnablePivotTable (which: 0, 1, 2)
    const int which = static_cast<int>(std::clamp<int64_t>(p.GetInt("which"), 0, 2));
    const bool set = p.Has("on"), on = p.GetBool("on");
    return eng.Call([&](Document& d) {
      if (set) d.SetProtectionEnable(sheet, which, on);
      return MV::Bool(d.ProtectionEnable(sheet, which));
    });
  }
  if (m == "sheet.cf_calc") {
    // Worksheet.EnableFormatConditionsCalculation: the formats keep what they show while it is False
    const bool set = p.Has("on"), on = p.GetBool("on", true);
    return OnUi([sheet, set, on](AppState&) {
      if (set && on) render::CfFrozen().erase(sheet);
      if (set && !on) render::CfFrozen().insert(sheet);
      ui::RequestRedrawIn(0.0);
      return MV::Bool(!render::CfFrozen().count(sheet));
    });
  }
  if (m == "sheet.consolidation") {
    // Worksheet.ConsolidationFunction / Options / Sources: the last consolidation onto the sheet (nil: none)
    return eng.Call([&](Document& d) {
      auto it = d.last_consolidation.find(sheet);
      if (it == d.last_consolidation.end()) return MV();
      MV x = MV::Map();
      x.Set("func", MV::Int(it->second.func));
      x.Set("top", MV::Bool(it->second.top));
      x.Set("left", MV::Bool(it->second.left));
      x.Set("links", MV::Bool(it->second.links));
      MV src = MV::Array();
      for (const auto& [sid, r] : it->second.sources) {
        const model::Sheet* ss = d.S(sid);
        auto rc = [](uint32_t row, uint32_t col) { return "R" + std::to_string(row + 1) + "C" + std::to_string(col + 1); };
        const std::string ref = r.r0 == r.r1 && r.c0 == r.c1 ? rc(r.r0, r.c0) : rc(r.r0, r.c0) + ":" + rc(r.r1, r.c1);
        std::string sn = ss ? ss->name : std::string();
        if (!sn.empty() && sn.find_first_of(" -+()&,;'") != std::string::npos) sn = "'" + sn + "'";  // a name to quote
        src.Push(MV::Str((sn.empty() ? std::string() : sn + "!") + ref));
      }
      x.Set("sources", std::move(src));
      return x;
    });
  }
  if (m == "debug.func_help") {
    // what Insert Function and the argument hints show for a function (tests: an @udf's MacroOptions)
    const formula::FuncHelp* h = formula::FindFuncHelp(p.GetStr("name"));
    if (!h) return MV();
    MV x = MV::Map();
    x.Set("args", MV::Str(h->args));
    x.Set("desc", MV::Str(h->desc));
    x.Set("category", MV::Str(h->category));
    MV a = MV::Map();
    if (const MV* names = p.Get("arg_names")) {
      for (const MV& n : names->a) a.Set(n.s, MV::Str(formula::FuncArgHelp(p.GetStr("name"), n.s)));
    }
    x.Set("arg_help", std::move(a));
    return x;
  }
  if (m == "sheet.cell_at") {
    // the cell under a point of the sheet, in points from its top left (TopLeftCell / BottomRightCell of an object)
    const double left = p.GetDouble("left"), top = p.GetDouble("top");
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      MV a = MV::Array();
      a.Push(MV::Int(std::clamp<int64_t>(s->rows->IndexAt(static_cast<int64_t>(std::max(0.0, top) / 0.75)), 0, s->row_limit - 1)));
      a.Push(MV::Int(std::clamp<int64_t>(s->cols->IndexAt(static_cast<int64_t>(std::max(0.0, left) / 0.75)), 0, s->col_limit - 1)));
      return a;
    });
  }
  if (m == "sheet.calc") {
    // Worksheet.EnableCalculation (not saved)
    const bool set = p.Has("on"), on = p.GetBool("on", true);
    return eng.Call([&](Document& d) {
      if (set) d.SetSheetCalculation(sheet, on);
      d.ForeignWrite();
      return MV::Bool(d.SheetCalculation(sheet));
    });
  }
  if (m == "app.calc_interrupt") {
    // Application.CalculationInterruptKey (this session), CheckAbort (stop the calculation in progress)
    MV params = p;
    return OnUi([params](AppState& s) {
      if (params.Has("key")) s.calc_interrupt_key = static_cast<int>(std::clamp<int64_t>(params.GetInt("key"), 0, 2));
      if (params.GetBool("abort") && s.engine) s.engine->InterruptCalc();
      MV x = MV::Map();
      x.Set("key", MV::Int(s.calc_interrupt_key));
      return x;
    });
  }
  if (m == "app.window") {
    // Application / Window: Caption, WindowState, DisplayFullScreen, DisplayStatusBar, Cursor, Ready, Left / Top /
    // Width / Height and UsableWidth / UsableHeight (points, the YCGrid window's), as Excel 16 reads them (measured)
    MV params = p;
    return OnUi([params](AppState& s) {
      const UINT dpi = s.hwnd ? ::GetDpiForWindow(s.hwnd) : 96;
      const double pt = 72.0 / static_cast<double>(dpi ? dpi : 96);  // points per pixel
      bool title = false;
      if (params.Has("app_caption")) s.app_caption = params.GetStr("app_caption"), title = true;
      if (params.Has("win_caption")) {
        const MV* c = params.Get("win_caption");
        s.win_caption_set = c && c->t == MV::T::Str;  // None: the file's name again
        s.win_caption = s.win_caption_set ? c->s : std::string();
        title = true;
      }
      if (params.GetBool("win_caption_reset")) s.win_caption_set = false, s.win_caption.clear(), title = true;  // the file's name again
      if (title) UpdateWindowTitle(s);
      if (params.Has("status_bar")) s.show_status_bar = params.GetBool("status_bar");
      if (params.Has("cursor")) {
        const int64_t c = params.GetInt("cursor");
        s.cursor = (c == 1 || c == 2 || c == 3) ? static_cast<int>(c) : -4143;
      }
      auto show = [&s](int state) {  // xlMaximized -4137, xlMinimized -4140, xlNormal -4143
        if (s.headless || !s.hwnd) {
          s.headless_window_state = state;  // a hidden instance never shows its window
          return;
        }
        ::ShowWindow(s.hwnd, state == -4137 ? SW_MAXIMIZE : state == -4140 ? SW_MINIMIZE : SW_RESTORE);
      };
      if (params.Has("full_screen") && params.GetBool("full_screen") != s.full_screen) {
        s.full_screen = params.GetBool("full_screen");
        if (s.full_screen) show(-4137);
      }
      if (params.Has("state")) {
        const int64_t st = params.GetInt("state");
        if (st == -4137 || st == -4140 || st == -4143) show(static_cast<int>(st));
      }
      RECT wr{};
      if (s.hwnd) ::GetWindowRect(s.hwnd, &wr);
      if (params.Has("left") || params.Has("top") || params.Has("width") || params.Has("height")) {
        // Excel sets them on a normal window only: a maximized one becomes normal first
        if (!s.headless && s.hwnd && (::IsZoomed(s.hwnd) || ::IsIconic(s.hwnd))) {
          ::ShowWindow(s.hwnd, SW_RESTORE);
          ::GetWindowRect(s.hwnd, &wr);
        }
        if (s.headless) s.headless_window_state = -4143;
        auto px = [&](const char* k, LONG cur) { return params.Has(k) ? static_cast<LONG>(std::lround(params.GetDouble(k) / pt)) : cur; };
        const LONG l = px("left", wr.left), t = px("top", wr.top);
        const LONG w = std::max<LONG>(px("width", wr.right - wr.left), 100), h = std::max<LONG>(px("height", wr.bottom - wr.top), 100);
        if (s.hwnd) ::SetWindowPos(s.hwnd, nullptr, l, t, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (s.hwnd) ::GetWindowRect(s.hwnd, &wr);
      }
      ui::RequestRedrawIn(0.0);
      MV x = MV::Map();
      const std::string app = s.app_caption.empty() ? std::string("YCGrid") : s.app_caption;
      x.Set("title", MV::Str(ui::titlebar::St().title));
      x.Set("app_caption", MV::Str(app));
      std::string win = s.win_caption;
      if (!s.win_caption_set) win = s.file_path.empty() ? TR("Book1") : ToUtf8(std::filesystem::path(s.file_path).filename().wstring());
      x.Set("win_caption", MV::Str(win));
      int state = s.headless_window_state;
      if (!s.headless && s.hwnd) state = ::IsZoomed(s.hwnd) ? -4137 : ::IsIconic(s.hwnd) ? -4140 : -4143;
      x.Set("state", MV::Int(state));
      x.Set("full_screen", MV::Bool(s.full_screen));
      x.Set("status_bar", MV::Bool(s.show_status_bar && !s.full_screen));
      x.Set("cursor", MV::Int(s.cursor));
      // Ready: not in the middle of a cell entry or a modal dialog (Excel's False while a cell is edited)
      x.Set("ready", MV::Bool(!s.edit.active && s.dialog == Dialog::None));
      x.Set("left", MV::Double(wr.left * pt));
      x.Set("top", MV::Double(wr.top * pt));
      x.Set("width", MV::Double((wr.right - wr.left) * pt));
      x.Set("height", MV::Double((wr.bottom - wr.top) * pt));
      x.Set("usable_width", MV::Double(s.work_rect.GetWidth() * pt));
      x.Set("usable_height", MV::Double(s.work_rect.GetHeight() * pt));
      const render::GridView& v = s.ViewFor(s.focus_sheet);
      x.Set("cells_width", MV::Double(v.cells_rect.GetWidth() * pt));
      x.Set("cells_height", MV::Double(v.cells_rect.GetHeight() * pt));
      x.Set("dpi", MV::Int(dpi));
      return x;
    });
  }
  if (m == "sheet.scroll_area") {
    // Worksheet.ScrollArea: "" removes it
    const uint32_t sa_sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    const bool set = p.Has("area");
    const std::string area = p.GetStr("area");
    std::string err;
    MV r = OnUi([&err, sheet = sa_sheet, set, area](AppState& s) {
      if (set) {
        if (area.empty()) {
          s.scroll_areas.erase(sheet);
        } else {
          uint32_t sid = sheet;
          Rect rr;
          if (!ResolveRangeText(s.snap->wb, sheet, area, &sid, &rr) || sid != sheet) {
            err = "Not a range of this sheet: " + area;
            return MV();
          }
          s.scroll_areas[sheet] = rr;
        }
        ui::RequestRedrawIn(0.0);
      }
      auto it = s.scroll_areas.find(sheet);
      return MV::Str(it == s.scroll_areas.end() ? std::string() : core::CellA1(it->second.r0, it->second.c0, true, true) + ":" +
                                                                     core::CellA1(it->second.r1, it->second.c1, true, true));
    });
    if (!err.empty()) {
      *ec = "E_APP";
      *em = err;
    }
    return r;
  }
  if (m == "ui.status") {
    const std::string text = p.GetStr("text");
    return OnUi([text](AppState& s) {
      s.python_status = text;
      return MV();
    });
  }
  // ---- measurements (stress tests) ----
  if (m == "perf.stats") {
    const size_t last = static_cast<size_t>(std::max<int64_t>(1, p.GetInt("last", 120)));
    const bool reset_latency = p.GetBool("reset_latency");
    return OnUi([this, &eng, last, reset_latency](AppState& s) {
      MV o = MV::Map();
      o.Set("frames", MV::Int(static_cast<int64_t>(s.frames_presented)));
      std::vector<double> v(s.frame_ms.end() - static_cast<std::ptrdiff_t>(std::min(last, s.frame_ms.size())), s.frame_ms.end());
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (double x : v) sum += x;
      o.Set("frame_cpu_avg_ms", MV::Double(v.empty() ? 0 : sum / static_cast<double>(v.size())));
      o.Set("frame_cpu_p50_ms", MV::Double(v.empty() ? 0 : v[v.size() / 2]));
      o.Set("frame_cpu_p99_ms", MV::Double(v.empty() ? 0 : v[std::min(v.size() - 1, v.size() * 99 / 100)]));
      o.Set("frame_cpu_max_ms", MV::Double(v.empty() ? 0 : v.back()));
      o.Set("grid_ms", MV::Double(s.grid_ms));
      o.Set("visible_cells", MV::Int(s.visible_cells));
      o.Set("publishes", MV::Int(static_cast<int64_t>(eng.version())));
      o.Set("recalc_ms", MV::Double(s.snap ? s.snap->stats.ms : 0.0));
      o.Set("recalc_cells", MV::Int(s.snap ? static_cast<int64_t>(s.snap->stats.evaluated) : 0));
      o.Set("vsync", MV::Bool(s.vsync));
      o.Set("continuous", MV::Bool(s.perf_continuous));
      o.Set("hz", MV::Double(s.perf_hz));
      o.Set("snapshots_shown", MV::Int(static_cast<int64_t>(s.snapshots_shown)));
      {
        // tick-to-screen latency of bound-table data (send -> after Present), over the recent samples
        std::vector<double> lat(s.input_latency_ms.end() - static_cast<std::ptrdiff_t>(std::min(last * 8, s.input_latency_ms.size())), s.input_latency_ms.end());
        std::sort(lat.begin(), lat.end());
        o.Set("latency_samples", MV::Int(static_cast<int64_t>(lat.size())));
        o.Set("latency_p50_ms", MV::Double(lat.empty() ? 0 : lat[lat.size() / 2]));
        o.Set("latency_p99_ms", MV::Double(lat.empty() ? 0 : lat[std::min(lat.size() - 1, lat.size() * 99 / 100)]));
        o.Set("latency_max_ms", MV::Double(lat.empty() ? 0 : lat.back()));
        if (reset_latency) s.input_latency_ms.clear();
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof(pm);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) {
          o.Set("working_set_mb", MV::Double(static_cast<double>(pm.WorkingSetSize) / 1048576.0));
          o.Set("private_mb", MV::Double(static_cast<double>(pm.PrivateUsage) / 1048576.0));
        }
        o.Set("bind_cells", MV::Int(s.snap ? static_cast<int64_t>(s.snap->bind_cells) : 0));
        o.Set("engine_us_tasks", MV::Int(eng.us_tasks.load()));
        o.Set("engine_us_recalc", MV::Int(eng.us_recalc.load()));
        o.Set("engine_us_publish", MV::Int(eng.us_publish.load()));
        o.Set("engine_tasks", MV::Int(eng.tasks_run.load()));
        o.Set("bind_drains", MV::Int(static_cast<int64_t>(bind_inbox_.drains.load())));
        o.Set("bind_breaks", MV::Int(static_cast<int64_t>(bind_inbox_.breaks.load())));
        o.Set("bind_frames", MV::Int(static_cast<int64_t>(bind_inbox_.applied.load())));
      }
      DEVMODEW dm{};
      dm.dmSize = sizeof(dm);
      o.Set("refresh_hz", MV::Int(::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? static_cast<int64_t>(dm.dmDisplayFrequency) : 0));
      return o;
    });
  }
  if (m == "perf.set") {
    const bool has_vsync = p.Has("vsync");
    const bool vsync = p.GetBool("vsync", true);
    const bool has_cont = p.Has("continuous");
    const bool cont = p.GetBool("continuous");
    const bool has_hz = p.Has("hz");
    const double hz = std::clamp(p.GetDouble("hz", 0), 0.0, 1000.0);
    return OnUi([has_vsync, vsync, has_cont, cont, has_hz, hz](AppState& s) {
      if (has_vsync) s.vsync = vsync;
      if (has_cont) s.perf_continuous = cont;
      if (has_hz) s.perf_hz = hz;
      ui::RequestRedrawIn(0.0);
      return MV::Bool(s.vsync);
    });
  }
  if (m == "ui.status_get") {
    return OnUi([](AppState& s) {
      MV x = MV::Map();
      x.Set("text", MV::Str(s.python_status));
      return x;
    });
  }
  if (m == "ui.msgbox" && (auto_dialogs_.load() > 0 || s_.headless)) {
    // while MCP runs, or hidden (nobody could answer): no dialogs, log to the console and the Errors window
    // (ui.errors() shows the test what was asked) and answer with the default button
    Log("[MsgBox]" + p.GetStr("prompt"), 2);
    ReportError("Python", p.GetStr("title", "YCGrid"), p.GetStr("prompt") + "\n(" + TR("answered automatically: nobody can see this window") + ")", false);
    const int64_t b = p.GetInt("buttons") & 0x0F;
    return MV::Int(b == 2 ? 3 : (b == 3 || b == 4) ? 6 : b == 5 ? 4 : 1);
  }
  if (m == "ui.inputbox" && (auto_dialogs_.load() > 0 || s_.headless)) {
    Log("[InputBox]" + p.GetStr("prompt") + " ->" + p.GetStr("default"), 2);
    ReportError("Python", p.GetStr("title", "YCGrid"), p.GetStr("prompt") + "\n(" + TR("answered automatically: nobody can see this window") + ")", false);
    return MV::Str(p.GetStr("default"));
  }
  if (m == "ui.msgbox") {
    const UINT flags = static_cast<UINT>(p.GetInt("buttons")) | MB_SETFOREGROUND;
    const int r = ::MessageBoxW(s_.hwnd, Wide(p.GetStr("prompt")).c_str(), Wide(p.GetStr("title", "YCGrid")).c_str(), flags);
    return MV::Int(r);
  }
  if (m == "app.file_dialog") {  // Application.GetOpenFilename / GetSaveAsFilename
    if (auto_dialogs_.load() > 0 || s_.headless) {  // tests: the answer they queued, or the user cancels
      std::wstring a;
      NoFileDialog(nullptr, p.GetBool("save") ? "GetSaveAsFilename" : p.GetBool("folder") ? "FileDialog" : "GetOpenFilename", &a);
      if (a.empty()) return MV();
      if (p.GetBool("multi")) {
        MV arr = MV::Array();
        arr.Push(MV::Str(ToUtf8(a)));
        return arr;
      }
      return MV::Str(ToUtf8(a));
    }
    // folder: Application.FileDialog(msoFileDialogFolderPicker), a folder instead of files
    const bool save = p.GetBool("save"), multi = p.GetBool("multi") && !p.GetBool("folder"), folder = p.GetBool("folder") && !save;
    const std::wstring title = Wide(p.GetStr("title")), initial = Wide(p.GetStr("initial")), button = Wide(p.GetStr("button"));
    // Excel's FileFilter: "Text Files (*.txt),*.txt,All Files (*.*),*.*"
    std::vector<std::wstring> parts;
    {
      const std::wstring f = Wide(p.GetStr("filter", "All Files (*.*),*.*"));
      size_t a = 0;
      for (size_t i = 0; i <= f.size(); ++i) {
        if (i == f.size() || f[i] == L',') {
          parts.push_back(f.substr(a, i - a));
          a = i + 1;
        }
      }
    }
    std::vector<COMDLG_FILTERSPEC> spec;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) spec.push_back({parts[i].c_str(), parts[i + 1].c_str()});
    const int index = static_cast<int>(p.GetInt("index", 1));
    MV out;
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(::CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
      if (!spec.empty() && !folder) {
        dlg->SetFileTypes(static_cast<UINT>(spec.size()), spec.data());
        dlg->SetFileTypeIndex(static_cast<UINT>(std::clamp<int>(index, 1, static_cast<int>(spec.size()))));
      }
      if (!title.empty()) dlg->SetTitle(title.c_str());
      if (!button.empty()) dlg->SetOkButtonLabel(button.c_str());
      if (!initial.empty() && folder) {
        // a folder picker opens in the folder it is given
        IShellItem* at = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&at))) && at) {
          dlg->SetFolder(at);
          at->Release();
        }
      } else if (!initial.empty()) {
        dlg->SetFileName(initial.c_str());
      }
      DWORD opts = 0;
      dlg->GetOptions(&opts);
      opts |= FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
      if (multi && !save) opts |= FOS_ALLOWMULTISELECT;
      dlg->SetOptions(opts);
      if (SUCCEEDED(dlg->Show(s_.hwnd))) {
        auto path_of = [](IShellItem* item) {
          std::string r;
          PWSTR w = nullptr;
          if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
            r = ToUtf8(w);
            ::CoTaskMemFree(w);
          }
          return r;
        };
        if (multi && !save) {
          IShellItemArray* items = nullptr;
          if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg)->GetResults(&items)) && items) {
            out = MV::Array();
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
              IShellItem* it = nullptr;
              if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                out.Push(MV::Str(path_of(it)));
                it->Release();
              }
            }
            items->Release();
          }
        } else {
          IShellItem* item = nullptr;
          if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            out = MV::Str(path_of(item));
            item->Release();
          }
        }
      }
      dlg->Release();
    }
    if (SUCCEEDED(init)) ::CoUninitialize();
    return out;
  }
  if (m == "ui.inputbox") {
    auto prom = std::make_shared<std::promise<MV>>();
    auto fut = prom->get_future();
    const std::string prompt = p.GetStr("prompt"), title = p.GetStr("title", "YCGrid"), def = p.GetStr("default");
    {
      std::lock_guard<std::mutex> lk(ui_mu_);
      ui_queue_.push_back([prom, prompt, title, def](AppState& s) {
        s.inputbox_prompt = prompt;
        s.inputbox_title = title;
        s.inputbox_text = def;
        s.inputbox_done = [prom](bool ok, const std::string& text) { prom->set_value(ok ? MV::Str(text) : MV()); };
        s.OpenDialog(Dialog::InputBox);
      });
    }
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
    WaitUi(fut, 0);  // the user takes their time; only closing YCGrid ends the wait
    return fut.get();
  }
  if (m == "ui.copy" || m == "ui.paste") {
    MV params = p;
    return OnUi([params, m, sheet](AppState& s) {
      ActivateSheet(s, sheet);
      auto& v = s.Win(sheet).view;
      if (m == "ui.copy") {
        v.sel.areas.assign(1, RectOf(*params.Get("rect")));
        v.sel.mode = render::SelectMode::Cells;
        CopySelection(s, params.GetBool("cut"), true);  // Python reads the copy as soon as this returns
      } else {
        // the whole destination is selected: a copy that fits it a whole number of times fills it, as in Excel
        // (Range("G1:G9").PasteSpecial of one copied cell divides all nine)
        if (const MV* rr = params.Get("rect"); rr && rr->t == MV::T::Array && rr->a.size() == 4) render::SelectRange(v, RectOf(*rr));
        else render::SelectCell(v, static_cast<uint32_t>(params.GetInt("row")), static_cast<uint32_t>(params.GetInt("col")));
        const std::string mode = params.GetStr("mode", "all");
        const engine::PasteMode pm = mode == "values" ? engine::PasteMode::Values : mode == "formats" ? engine::PasteMode::Formats
                                     : mode == "formulas" ? engine::PasteMode::Formulas : mode == "noborders" ? engine::PasteMode::NoBorders
                                     : mode == "valuesformats" ? engine::PasteMode::ValuesAndFormats : mode == "widths" ? engine::PasteMode::ColumnWidths
                                     : mode == "formulasformats" ? engine::PasteMode::FormulasAndFormats : mode == "comments" ? engine::PasteMode::Comments
                                     : mode == "validation" ? engine::PasteMode::Validation : mode == "sourcetheme" ? engine::PasteMode::AllSourceTheme
                                     : mode == "mergecf" ? engine::PasteMode::AllMergingCf : engine::PasteMode::All;
        const int op = static_cast<int>(params.GetInt("op", 0));
        WaitForPendingCopy(s);  // a copy the UI handed to a busy engine: Python's paste waits for it
        if (params.GetBool("link")) PasteLink(s);
        else PasteClipboard(s, pm, params.GetBool("transpose"), params.GetBool("skip_blanks"), op >= 1 && op <= 4 ? static_cast<engine::PasteOp>(op) : engine::PasteOp::None);
      }
      return MV();
    });
  }
  if (m == "ui.design_mode_get") {
    return OnUi([](AppState& s) {
      MV x = MV::Map();
      x.Set("on", MV::Bool(s.design_mode));
      return x;
    });
  }
  // ---- What-If Analysis and data tools ----
  if (m == "whatif.goalseek") {
    return eng.Call([&](Document& d) {
      d.ForeignWrite();
      const auto r = d.GoalSeek(sheet, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col")), p.GetDouble("goal"),
                                static_cast<uint32_t>(p.GetInt("ch_sheet", sheet)), static_cast<uint32_t>(p.GetInt("ch_row")),
                                static_cast<uint32_t>(p.GetInt("ch_col")));
      MV o = MV::Map();
      o.Set("found", MV::Bool(r.found));
      o.Set("value", MV::Double(r.value));
      o.Set("result", MV::Double(r.result));
      o.Set("error", MV::Str(r.error));
      return o;
    });
  }
  if (m == "whatif.table") {
    const Rect r = RectOf(*p.Get("rect"));
    const MV* row = p.Get("row");
    const MV* col = p.Get("col");
    core::CellPos rp{}, cp{};
    const bool has_row = row && row->t == MV::T::Array && row->a.size() == 2, has_col = col && col->t == MV::T::Array && col->a.size() == 2;
    if (has_row) rp = core::CellPos{static_cast<uint32_t>(row->a[0].i), static_cast<uint32_t>(row->a[1].i)};
    if (has_col) cp = core::CellPos{static_cast<uint32_t>(col->a[0].i), static_cast<uint32_t>(col->a[1].i)};
    std::string err;
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      const model::Sheet* s = d.S(sheet);
      return s && d.AddDataTable(sheet, Clamp(*s, r), has_row ? &rp : nullptr, has_col ? &cp : nullptr, &err);
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return MV::Bool(true);
  }
  if (m == "scenario.list") {
    return eng.Call([&](Document& d) {
      MV out = MV::Array();
      if (const model::Sheet* sh = d.S(sheet)) {
        for (const auto& sc : *sh->scenarios) {
          MV x = MV::Map();
          x.Set("name", MV::Str(sc.name));
          x.Set("comment", MV::Str(sc.comment));
          MV cells = MV::Array();
          for (const auto& c : sc.cells) {
            MV rc = MV::Array();
            rc.Push(MV::Int(c.row));
            rc.Push(MV::Int(c.col));
            cells.Push(std::move(rc));
          }
          x.Set("cells", std::move(cells));
          MV vals = MV::Array();
          for (const auto& v : sc.values) vals.Push(ValueToMV(v));
          x.Set("values", std::move(vals));
          x.Set("locked", MV::Bool(sc.locked));
          x.Set("hidden", MV::Bool(sc.hidden));
          out.Push(std::move(x));
        }
      }
      return out;
    });
  }
  if (m == "scenario.add") {
    model::Scenario sc;
    sc.name = p.GetStr("name");
    sc.comment = p.GetStr("comment");
    sc.locked = p.GetBool("locked", true);
    sc.hidden = p.GetBool("hidden", false);
    if (const MV* cl = p.Get("cells")) {
      for (const MV& rc : cl->a) {
        if (rc.a.size() == 2) sc.cells.push_back(core::CellPos{static_cast<uint32_t>(rc.a[0].i), static_cast<uint32_t>(rc.a[1].i)});
      }
    }
    if (const MV* v = p.Get("values")) {
      for (const MV& x : v->a) {
        int dk;
        sc.values.push_back(MVToValue(x, &dk));
      }
    }
    std::string err;
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      // values not given: the cells' current values
      for (size_t i = sc.values.size(); i < sc.cells.size(); ++i) sc.values.push_back(d.GetValue(sheet, sc.cells[i].row, sc.cells[i].col));
      return d.AddScenario(sheet, sc, &err);
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return MV::Bool(true);
  }
  if (m == "scenario.delete" || m == "scenario.show") {
    const std::string name = p.GetStr("name");
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return m == "scenario.delete" ? d.DeleteScenario(sheet, name) : d.ShowScenario(sheet, name);
    });
    if (!ok) return Err(ec, em, "E_BAD_REF", "No such scenario: " + name), MV();
    return MV::Bool(true);
  }
  if (m == "scenario.summary") {
    std::vector<core::CellPos> res;
    if (const MV* cl = p.Get("results")) {
      for (const MV& rc : cl->a) {
        if (rc.a.size() == 2) res.push_back(core::CellPos{static_cast<uint32_t>(rc.a[0].i), static_cast<uint32_t>(rc.a[1].i)});
      }
    }
    std::string err;
    const bool pivot = p.GetInt("type", 1) == 2;
    const uint32_t id = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return pivot ? d.ScenarioPivotReport(sheet, res, &err) : d.ScenarioSummary(sheet, res, &err);
    });
    if (!id) return Err(ec, em, "E_APP", err), MV();
    return MV::Int(id);
  }
  if (m == "range.importrows") {  // QueryTable.Refresh: a text file's fields from (row, col); types 0 detected, 1 text
    std::vector<std::vector<std::string>> rows;
    if (const MV* rr = p.Get("rows")) {
      for (const MV& row : rr->a) {
        std::vector<std::string> f;
        for (const MV& x : row.a) f.push_back(x.s);
        rows.push_back(std::move(f));
      }
    }
    std::vector<int> types;
    if (const MV* t = p.Get("types")) {
      for (const MV& x : t->a) types.push_back(static_cast<int>(x.Num()));
    }
    const Rect at = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.ImportRows(sheet, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col")), rows, types);
    });
    MV out = MV::Array();
    for (uint32_t v : {at.r0, at.c0, at.r1, at.c1}) out.Push(MV::Int(v));
    return out;
  }
  if (m == "scenario.merge") {  // Scenarios.Merge(Source)
    const uint32_t src = static_cast<uint32_t>(p.GetInt("source"));
    const int n = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.MergeScenarios(sheet, src);
    });
    return MV::Int(n);
  }
  if (m == "range.subtotal") {  // Range.Subtotal
    const Rect r = RectOf(*p.Get("rect"));
    std::vector<uint32_t> cols;
    if (const MV* tl = p.Get("totals")) {
      for (const MV& x : tl->a) cols.push_back(static_cast<uint32_t>(x.Num()));
    }
    std::string err;
    const bool ok = eng.Call([&](Document& d) {
      Settle(d);
      d.ForeignWrite();
      return d.Subtotal(sheet, r, static_cast<uint32_t>(p.GetInt("group")), static_cast<int>(p.GetInt("func", 9)), cols, p.GetBool("replace", true),
                        p.GetBool("page_breaks", false), p.GetBool("below", true), &err);
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return MV::Bool(true);
  }
  if (m == "range.removesubtotal") {  // Range.RemoveSubtotal
    const Rect r = RectOf(*p.Get("rect"));
    const uint32_t n = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.RemoveSubtotal(sheet, r);
    });
    return MV::Int(n);
  }
  if (m == "range.consolidate") {
    std::vector<std::pair<uint32_t, Rect>> sources;
    std::string err;
    bool refs_ok = true;
    eng.Call([&](Document& d) {
      if (const MV* src = p.Get("sources")) {
        for (const MV& x : src->a) {
          std::vector<std::pair<uint32_t, Rect>> part;
          if (!d.ParseRefList(x.s, sheet, &part)) {
            refs_ok = false;
            err = "Invalid reference: " + x.s;
            return;
          }
          sources.insert(sources.end(), part.begin(), part.end());
        }
      }
    });
    if (!refs_ok) return Err(ec, em, "E_BAD_REF", err), MV();
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.Consolidate(sheet, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col")), sources, static_cast<int>(p.GetInt("func")),
                           p.GetBool("top"), p.GetBool("left"), &err, p.GetBool("links", false));
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return MV::Bool(true);
  }
  if (m == "range.advfilter") {
    const Rect r = RectOf(*p.Get("rect"));
    const MV* cr = p.Get("crit");
    const MV* dr = p.Get("dest");
    std::string err;
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.AdvancedFilter(sheet, r, static_cast<uint32_t>(p.GetInt("crit_sheet", sheet)), cr && cr->t == MV::T::Array ? RectOf(*cr) : Rect{}, p.GetBool("copy"),
                              static_cast<uint32_t>(p.GetInt("dest_sheet", sheet)), dr && dr->t == MV::T::Array ? RectOf(*dr) : Rect{}, p.GetBool("unique"), &err);
    });
    if (!ok) return Err(ec, em, "E_APP", err.empty() ? "AdvancedFilter failed" : err), MV();
    return MV::Bool(true);
  }
  if (m == "range.flashfill") {
    std::string err;
    const uint32_t n = eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.FlashFill(sheet, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col")), &err);
    });
    if (!n) return Err(ec, em, "E_APP", err), MV();
    return MV::Int(n);
  }
  if (m == "solver.reports") {  // SolverFinish(ReportArray): 1 Answer, 2 Sensitivity, 3 Limits
    model::SolverModel sm;
    Document::SolverResult res;
    res.status = static_cast<int>(p.GetInt("status"));
    res.message = p.GetStr("message");
    if (const MV* o = p.Get("original")) {
      for (const MV& x : o->a) res.original.push_back(x.Num());
    }
    std::string err;
    std::vector<uint32_t> made = eng.Call([&](Document& d) {
      d.ForeignWrite();
      if (const model::Sheet* sh = d.S(sheet)) sm = *sh->solver;
      return d.SolverReports(sheet, sm, res, p.GetBool("answer"), p.GetBool("sensitivity"), p.GetBool("limits"), &err);
    });
    if (made.empty() && !err.empty()) return Err(ec, em, "E_APP", err), MV();
    MV out = MV::Array();
    for (uint32_t id : made) out.Push(MV::Int(id));
    return out;
  }
  if (m == "solver.save") {  // SolverSave(SaveArea)
    std::string err;
    const Rect at = eng.Call([&](Document& d) {
      d.ForeignWrite();
      const model::Sheet* sh = d.S(static_cast<uint32_t>(p.GetInt("model_sheet", sheet)));
      return d.SaveSolverModel(sheet, sh ? *sh->solver : model::SolverModel{}, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col")), &err);
    });
    if (!err.empty()) return Err(ec, em, "E_APP", err), MV();
    MV out = MV::Array();
    for (uint32_t v : {at.r0, at.c0, at.r1, at.c1}) out.Push(MV::Int(v));
    return out;
  }
  if (m == "solver.load") {  // SolverLoad(LoadArea)
    const Rect r = RectOf(*p.Get("rect"));
    const uint32_t model_sheet = static_cast<uint32_t>(p.GetInt("model_sheet", sheet));
    std::string err;
    const bool ok = eng.Call([&](Document& d) {
      model::SolverModel sm;
      if (const model::Sheet* sh = d.S(model_sheet)) sm = *sh->solver;
      if (!d.LoadSolverModel(sheet, r, &sm, &err)) return false;
      d.SetSolverModel(model_sheet, sm);
      return true;
    });
    if (!ok) return Err(ec, em, "E_APP", err), MV();
    return MV::Bool(true);
  }
  if (m == "solver.get") {
    return eng.Call([&](Document& d) {
      MV o = MV::Map();
      const model::Sheet* sh = d.S(sheet);
      if (!sh) return o;
      const auto& sm = *sh->solver;
      o.Set("defined", MV::Bool(sm.defined));
      o.Set("objective", MV::Str(sm.objective));
      o.Set("goal", MV::Int(sm.goal));
      o.Set("value", MV::Double(sm.value));
      o.Set("changing", MV::Str(sm.changing));
      MV cons = MV::Array();
      for (const auto& c : sm.constraints) {
        MV x = MV::Array();
        x.Push(MV::Str(c.ref));
        x.Push(MV::Int(c.rel));
        x.Push(MV::Str(c.rhs));
        cons.Push(std::move(x));
      }
      o.Set("constraints", std::move(cons));
      o.Set("non_negative", MV::Bool(sm.non_negative));
      o.Set("method", MV::Int(sm.method));
      o.Set("max_iterations", MV::Int(sm.max_iterations));
      o.Set("precision", MV::Double(sm.precision));
      o.Set("max_time", MV::Double(sm.max_time));
      o.Set("auto_scaling", MV::Bool(sm.auto_scaling));
      o.Set("show_iterations", MV::Bool(sm.show_iterations));
      o.Set("ignore_integer", MV::Bool(sm.ignore_integer));
      o.Set("integer_optimality", MV::Double(sm.integer_optimality));
      o.Set("max_subproblems", MV::Int(sm.max_subproblems));
      o.Set("max_feasible", MV::Int(sm.max_feasible));
      o.Set("convergence", MV::Double(sm.convergence));
      o.Set("derivatives", MV::Int(sm.derivatives));
      o.Set("multistart", MV::Bool(sm.multistart));
      o.Set("require_bounds", MV::Bool(sm.require_bounds));
      o.Set("population", MV::Int(sm.population));
      o.Set("random_seed", MV::Int(sm.random_seed));
      o.Set("mutation_rate", MV::Double(sm.mutation_rate));
      o.Set("max_time_no_improve", MV::Double(sm.max_time_no_improve));
      return o;
    });
  }
  if (m == "solver.set" || m == "solver.solve") {
    model::SolverModel sm;
    const MV* mm = p.Get("model");
    if (mm) {
      sm.defined = true;
      sm.objective = mm->GetStr("objective");
      sm.goal = static_cast<int>(mm->GetInt("goal", 1));
      sm.value = mm->GetDouble("value");
      sm.changing = mm->GetStr("changing");
      if (const MV* cons = mm->Get("constraints")) {
        for (const MV& x : cons->a) {
          if (x.a.size() >= 3) sm.constraints.push_back(model::SolverConstraint{x.a[0].s, static_cast<int>(x.a[1].i), x.a[2].s});
        }
      }
      sm.non_negative = mm->GetBool("non_negative", true);
      sm.method = static_cast<int>(mm->GetInt("method", 1));
      sm.max_iterations = static_cast<int>(mm->GetInt("max_iterations", 1000));
      sm.precision = mm->GetDouble("precision", 1e-6);
      sm.max_time = mm->GetDouble("max_time", 100);
      sm.auto_scaling = mm->GetBool("auto_scaling", true);
      sm.show_iterations = mm->GetBool("show_iterations", false);
      sm.ignore_integer = mm->GetBool("ignore_integer", false);
      sm.integer_optimality = mm->GetDouble("integer_optimality", 1);
      sm.max_subproblems = static_cast<int>(mm->GetInt("max_subproblems", 0));
      sm.max_feasible = static_cast<int>(mm->GetInt("max_feasible", 0));
      sm.convergence = mm->GetDouble("convergence", 0.0001);
      sm.derivatives = static_cast<int>(mm->GetInt("derivatives", 1));
      sm.multistart = mm->GetBool("multistart", false);
      sm.require_bounds = mm->GetBool("require_bounds", true);
      sm.population = static_cast<int>(mm->GetInt("population", 100));
      sm.random_seed = static_cast<int>(mm->GetInt("random_seed", 0));
      sm.mutation_rate = mm->GetDouble("mutation_rate", 0.075);
      sm.max_time_no_improve = mm->GetDouble("max_time_no_improve", 30);
    }
    if (m == "solver.set") {
      eng.Call([&](Document& d) { d.SetSolverModel(sheet, sm); });
      return MV::Bool(true);
    }
    return eng.Call([&](Document& d) {
      d.ForeignWrite();
      if (mm) d.SetSolverModel(sheet, sm);
      else if (const model::Sheet* sh = d.S(sheet)) sm = *sh->solver;
      const auto r = d.Solve(sheet, sm);
      MV o = MV::Map();
      o.Set("status", MV::Int(r.status));
      o.Set("message", MV::Str(r.message));
      o.Set("objective", MV::Double(r.objective));
      MV orig = MV::Array();
      for (double v : r.original) orig.Push(MV::Double(v));
      o.Set("original", std::move(orig));
      return o;
    });
  }
  {
    bool handled = false;
    MV r = DispatchObjects(m, p, ec, em, &handled);
    if (handled) return r;
    r = DispatchMcp(m, p, ec, em, &handled);
    if (handled) return r;
  }
  return Err(ec, em, "E_UNSUPPORTED", "YCGrid doesn't support this request:" + m), MV();
}

}  // namespace yc::app
