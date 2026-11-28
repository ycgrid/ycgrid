// SPDX-License-Identifier: Apache-2.0
// python_bridge_data.cpp - YCP: the shared-memory data channel and bound tables (ws.bind, ADR 0007)
//
// A client asks for a channel (data.channel): a named file mapping that holds a ring of column frames (engine/bind.h).
// It writes a frame at a position of the ring and sends bind.frame {chan, pos, len, end} through the pipe; YCGrid
// decodes the frame (copying it out), stores `end` in the ring's header so the writer can reuse the space, and hands the
// rows to the engine. Channels belong to their connection and go away with it.
#include "app_state.h"
#include "formula/datetime.h"
#include "npy.h"
#include "python_bridge.h"

#include "engine/bind.h"
#include "engine/engine.h"
#include "formula/numfmt.h"
#include "model/table.h"
#include "model/theme.h"
#include "io/msgvalue.h"

#include <windows.h>

#include <atomic>
#include <chrono>

namespace yc::app {

using engine::Document;
using io::MV;

namespace {

constexpr uint64_t kChanMagic = 0x314E414843584359ull;  // "YCXCHAN1"
constexpr size_t kChanHeader = 64;
constexpr uint64_t kBindBacklog = 1u << 20;  // cells of frames waiting for the engine before a sender has to wait (latency stays ~1 cycle)

uint64_t FrameCells(const engine::ColumnFrame& f) { return uint64_t{f.nrows} * std::max<size_t>(f.cols.size(), 1); }

// header of a channel: [0] magic, [1] total size, [2] consumed position (monotonic; written by YCGrid)
struct ChanHeader {
  uint64_t magic;
  uint64_t size;
  std::atomic<uint64_t> tail;
  uint64_t reserved[5];
};
static_assert(sizeof(ChanHeader) == kChanHeader);

MV DefToMV(const model::BoundTable& b) {
  MV o = MV::Map();
  o.Set("name", MV::Str(b.name));
  o.Set("sheet", MV::Int(b.sheet));
  o.Set("key", MV::Str(b.key));
  o.Set("stale_after", MV::Double(b.stale_after));
  o.Set("show_age", MV::Bool(b.show_age));
  o.Set("record", MV::Bool(b.record));
  MV cols = MV::Array();
  for (const auto& c : b.columns) {
    MV x = MV::Map();
    x.Set("name", MV::Str(c.name));
    x.Set("number_format", MV::Str(c.number_format));
    x.Set("flash", MV::Int(c.flash));
    x.Set("up_color", MV::Int(c.up_color));
    x.Set("down_color", MV::Int(c.down_color));
    x.Set("flash_ms", MV::Int(c.flash_ms));
    x.Set("max_hz", MV::Int(c.max_hz));
    if (c.render) {
      x.Set("render", MV::Int(c.render));
      x.Set("render_color", MV::Int(c.render_color));
      x.Set("render_neg", MV::Int(c.render_neg));
      x.Set("render_max", MV::Double(c.render_max));
      x.Set("trend_len", MV::Int(c.trend_len));
    }
    cols.Push(std::move(x));
  }
  o.Set("columns", std::move(cols));
  return o;
}

// the declared columns of a definition from Python ({"columns": [{name, number_format, flash, ...}]})
void ColumnsFromMV(const MV& p, model::BoundTable* b) {
  const MV* cols = p.Get("columns");
  if (!cols) return;
  b->columns.clear();
  for (const MV& x : cols->a) {
    model::BoundColumn c;
    c.name = x.GetStr("name");
    c.number_format = x.GetStr("number_format");
    c.flash = static_cast<int>(x.GetInt("flash", 0));
    c.up_color = static_cast<uint32_t>(x.GetInt("up_color", c.up_color));
    c.down_color = static_cast<uint32_t>(x.GetInt("down_color", c.down_color));
    c.flash_ms = static_cast<int>(x.GetInt("flash_ms", c.flash_ms));
    c.max_hz = static_cast<int>(x.GetInt("max_hz", 0));
    c.render = static_cast<int>(x.GetInt("render", 0));
    c.render_color = static_cast<uint32_t>(x.GetInt("render_color", 0));
    c.render_neg = static_cast<uint32_t>(x.GetInt("render_neg", 0));
    c.render_max = x.GetDouble("render_max", 0);
    c.trend_len = static_cast<int>(std::clamp<int64_t>(x.GetInt("trend_len", 60), 2, 2000));
    if (!c.name.empty()) b->columns.push_back(std::move(c));
  }
}

// b only changes what a already changes, with newer values: a can be dropped (same table, upserts, same columns,
// same keys in the same order)
bool Supersedes(const engine::ColumnFrame& b, const engine::ColumnFrame& a) {
  if (a.op != engine::FrameOp::Upsert || b.op != engine::FrameOp::Upsert || a.table != b.table || a.names != b.names || a.key_col != b.key_col ||
      a.nrows != b.nrows || a.key_col < 0)
    return false;
  const auto& ka = a.cols[static_cast<size_t>(a.key_col)];
  const auto& kb = b.cols[static_cast<size_t>(b.key_col)];
  for (size_t i = 0; i < ka.size(); ++i) {
    const auto& x = ka[i];
    const auto& y = kb[i];
    if (x.IsNumeric() && y.IsNumeric()) {
      if (x.AsDouble() != y.AsDouble()) return false;
    } else if (x.IsString() && y.IsString()) {
      if (x.str() != y.str()) return false;
    } else if (x.IsEmpty() != y.IsEmpty() || !x.IsEmpty()) {
      return false;
    }
  }
  return true;
}

}  // namespace

// applies the waiting frames; when the publish interval is due it stops and posts the rest, so the screen updates
// while data keep arriving
void PythonBridge::PostBindDrain() {
  s_.engine->Post([this](engine::Document& d) {
    d.python_call = true;  // Python's bound tables, from whichever thread posted the drain (UserInterfaceOnly lets them in)
    ++bind_inbox_.drains;
    while (true) {
      std::shared_ptr<engine::ColumnFrame> f;
      {
        std::lock_guard<std::mutex> lk(bind_inbox_.mu);
        if (bind_inbox_.frames.empty()) {
          bind_inbox_.posted = false;
          return;
        }
        f = bind_inbox_.frames.front();
        bind_inbox_.frames.pop_front();
        bind_inbox_.cells -= std::min(bind_inbox_.cells, FrameCells(*f));
      }
      bind_inbox_.cv.notify_all();
      std::string e;
      if (!d.ApplyColumnFrame(*f, &e)) {
        std::lock_guard<std::mutex> lk(state_mu_);
        bind_errors_[engine::Lower(f->table)] = e;
      }
      ++bind_inbox_.applied;
      if (s_.engine->PublishDue()) {
        ++bind_inbox_.breaks;
        std::lock_guard<std::mutex> lk(bind_inbox_.mu);
        if (bind_inbox_.frames.empty()) {
          bind_inbox_.posted = false;
          return;
        }
        break;  // the rest after the publish (posted stays true)
      }
    }
    PostBindDrain();
  });
}

void PythonBridge::EnqueueFrame(std::shared_ptr<engine::ColumnFrame> frame, bool wait) {
  bool post = false;
  {
    std::unique_lock<std::mutex> lk(bind_inbox_.mu);
    if (wait) bind_inbox_.cv.wait_for(lk, std::chrono::seconds(10), [&] { return bind_inbox_.cells < kBindBacklog; });
    auto& q = bind_inbox_.frames;
    const uint64_t cells = FrameCells(*frame);
    if (!q.empty() && Supersedes(*frame, *q.back())) {
      bind_inbox_.cells -= FrameCells(*q.back());
      q.back() = std::move(frame);
      ++bind_inbox_.merged;
    } else {
      q.push_back(std::move(frame));
    }
    bind_inbox_.cells += cells;
    if (!bind_inbox_.posted) bind_inbox_.posted = post = true;
  }
  if (post) PostBindDrain();
}

PythonBridge::DataChannel::~DataChannel() {
  if (view) ::UnmapViewOfFile(view);
  if (mapping) ::CloseHandle(mapping);
}

MV PythonBridge::DispatchData(Conn& c, const std::string& m, const MV& p, std::string* ec, std::string* em, bool* handled) {
  *handled = true;
  auto& eng = *s_.engine;
  auto err = [&](const std::string& code, const std::string& msg) {
    *ec = code;
    *em = msg;
    return MV();
  };
  // ---- Workbook.TableStyles: Excel's built-in styles and the workbook's own (New Table Style) ----
  if (m.rfind("tablestyle.", 0) == 0) {
    auto side_mv = [](const model::BorderSide& b) {
      MV x = MV::Array();
      x.Push(MV::Int(static_cast<int>(b.style)));
      x.Push(MV::Int(b.color ? model::ResolveColor(b.color) : 0));
      return x;
    };
    auto side_of = [](const MV* v) {
      if (!v || v->a.size() < 2) return model::BorderSide{};
      return model::BorderSide{static_cast<model::LineStyle>(std::clamp<int64_t>(v->a[0].i, 0, 13)), static_cast<uint32_t>(v->a[1].i)};
    };
    return eng.Call([&](Document& d) -> MV {
      const auto& store = *d.wb().store;
      auto it = store.find(model::kTableStylesKey);
      std::vector<model::TableStyleDef> own = it == store.end() ? std::vector<model::TableStyleDef>{} : model::DecodeTableStyles(it->second);
      const std::string name = p.GetStr("name");
      if (m == "tablestyle.default_get") return MV::Str(model::DefaultTableStyle(store));
      if (m == "tablestyle.default_set") {
        d.ForeignWrite();
        d.Begin("Table Style");
        auto& st = d.wb_mut().store.mut();
        if (name.empty() || name == "TableStyleMedium2") st.erase(model::kDefaultTableStyleKey);
        else st[model::kDefaultTableStyleKey] = model::EncodeStoreString(name);
        d.wb_mut().dirty = true;
        d.Commit();
        return MV::Bool(true);
      }
      if (m == "tablestyle.list") {
        MV out = MV::Array();
        for (const auto& x : model::BuiltinTableStyles()) {
          MV e = MV::Map();
          e.Set("name", MV::Str(x.name));
          e.Set("builtin", MV::Bool(true));
          out.Push(std::move(e));
        }
        for (const auto& x : own) {
          MV e = MV::Map();
          e.Set("name", MV::Str(x.name));
          e.Set("builtin", MV::Bool(false));
          out.Push(std::move(e));
        }
        return out;
      }
      if (m == "tablestyle.get") {
        const model::TableStyleDef* x = nullptr;
        for (const auto& o : own) {
          if (o.name == name) x = &o;
        }
        if (!x) x = model::BuiltinTableStyle(name);
        if (!x) return MV();
        MV out = MV::Map();
        out.Set("name", MV::Str(x->name));
        out.Set("builtin", MV::Bool(x->builtin));
        out.Set("row_stripe", MV::Int(x->row_stripe));
        out.Set("col_stripe", MV::Int(x->col_stripe));
        MV els = MV::Array();
        for (const auto& e : x->el) {
          MV y = MV::Map();
          y.Set("fill", MV::Int(e.fill ? model::ResolveColor(e.fill) : 0));
          y.Set("font_color", MV::Int(e.font_color ? model::ResolveColor(e.font_color) : 0));
          y.Set("bold", MV::Int(e.bold));
          y.Set("italic", MV::Int(e.italic));
          y.Set("top", side_mv(e.top));
          y.Set("bottom", side_mv(e.bottom));
          y.Set("left", side_mv(e.left));
          y.Set("right", side_mv(e.right));
          y.Set("inner_h", side_mv(e.inner_h));
          y.Set("inner_v", side_mv(e.inner_v));
          els.Push(std::move(y));
        }
        out.Set("elements", std::move(els));
        return out;
      }
      if (m == "tablestyle.put") {
        if (name.empty() || name.find('|') != std::string::npos || model::BuiltinTableStyle(name)) return err("E_ARG", "A table style with that name can't be made");
        model::TableStyleDef x;
        x.name = name;
        x.row_stripe = static_cast<int>(std::clamp<int64_t>(p.GetInt("row_stripe", 1), 1, 9));
        x.col_stripe = static_cast<int>(std::clamp<int64_t>(p.GetInt("col_stripe", 1), 1, 9));
        if (const MV* els = p.Get("elements")) {
          for (size_t k = 0; k < els->a.size() && k < model::kTeCount; ++k) {
            const MV& y = els->a[k];
            model::TableStyleElement& e = x.el[k];
            e.fill = static_cast<uint32_t>(y.GetInt("fill"));
            e.font_color = static_cast<uint32_t>(y.GetInt("font_color"));
            e.bold = static_cast<int8_t>(std::clamp<int64_t>(y.GetInt("bold", -1), -1, 1));
            e.italic = static_cast<int8_t>(std::clamp<int64_t>(y.GetInt("italic", -1), -1, 1));
            e.top = side_of(y.Get("top"));
            e.bottom = side_of(y.Get("bottom"));
            e.left = side_of(y.Get("left"));
            e.right = side_of(y.Get("right"));
            e.inner_h = side_of(y.Get("inner_h"));
            e.inner_v = side_of(y.Get("inner_v"));
          }
        }
        bool replaced = false;
        for (auto& o : own) {
          if (o.name == name) o = x, replaced = true;
        }
        if (!replaced) own.push_back(x);
      } else if (m == "tablestyle.delete") {
        own.erase(std::remove_if(own.begin(), own.end(), [&](const model::TableStyleDef& o) { return o.name == name; }), own.end());
      } else {
        *handled = false;
        return MV();
      }
      d.ForeignWrite();
      d.Begin("Table Style");
      auto& st = d.wb_mut().store.mut();
      if (own.empty()) st.erase(model::kTableStylesKey);
      else st[model::kTableStylesKey] = model::EncodeTableStyles(own);
      d.wb_mut().dirty = true;
      d.Commit();
      return MV::Bool(true);
    });
  }
  if (m == "data.channel") {
    // one channel per connection is enough; asking again returns it
    std::lock_guard<std::mutex> lk(c.chan_mu);
    if (c.channels.empty()) {
      const int64_t mb = std::clamp<int64_t>(p.GetInt("size_mb", 64), 1, 2048);
      const uint64_t size = static_cast<uint64_t>(mb) << 20;
      static std::atomic<int> seq{0};
      const std::wstring name = L"Local\\ycgrid." + std::to_wstring(::GetCurrentProcessId()) + L".data." + std::to_wstring(++seq);
      auto ch = std::make_shared<DataChannel>();
      ch->mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
      if (!ch->mapping) return err("E_APP", "The shared memory channel couldn't be created");
      ch->view = static_cast<uint8_t*>(::MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size)));
      if (!ch->view) return err("E_APP", "The shared memory channel couldn't be mapped");
      ch->size = size;
      ch->name = ToUtf8(name);
      auto* h = reinterpret_cast<ChanHeader*>(ch->view);
      h->magic = kChanMagic;
      h->size = size;
      h->tail.store(0, std::memory_order_release);
      c.channels.push_back(std::move(ch));
    }
    const auto& ch = c.channels.front();
    MV r = MV::Map();
    r.Set("chan", MV::Int(0));
    r.Set("name", MV::Str(ch->name));
    r.Set("size", MV::Int(static_cast<int64_t>(ch->size)));
    r.Set("header", MV::Int(static_cast<int64_t>(kChanHeader)));
    return r;
  }
  if (m == "bind.frame") {
    // a frame in the channel: decode it, free its space, apply it (sync: wait and report)
    std::shared_ptr<DataChannel> ch;
    {
      std::lock_guard<std::mutex> lk(c.chan_mu);
      const int64_t i = p.GetInt("chan", 0);
      if (i >= 0 && static_cast<size_t>(i) < c.channels.size()) ch = c.channels[static_cast<size_t>(i)];
    }
    if (!ch) return err("E_APP", "No data channel: ask for one with data.channel first");
    const uint64_t pos = static_cast<uint64_t>(p.GetInt("pos", 0)), len = static_cast<uint64_t>(p.GetInt("len", 0));
    const uint64_t cap = ch->size - kChanHeader;
    auto* h = reinterpret_cast<ChanHeader*>(ch->view);
    if (pos > cap || len > cap - pos) {
      h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
      return err("E_APP", "The frame is outside the data channel");
    }
    if (!p.GetBool("sync")) {
      // backpressure: wait while the engine is behind (the ring isn't freed meanwhile, so the sender waits too)
      std::unique_lock<std::mutex> lk(bind_inbox_.mu);
      bind_inbox_.cv.wait_for(lk, std::chrono::seconds(10), [&] { return bind_inbox_.cells < kBindBacklog; });
    }
    auto frame = std::make_shared<engine::ColumnFrame>();
    std::string why;
    const uint8_t* raw = ch->view + kChanHeader + pos;
    const bool ok = engine::DecodeColumnFrame(raw, static_cast<size_t>(len), frame.get(), &why);
    // recorded before its ring space is freed; dropped while a replay owns its table
    const bool replayed = ok && TapeFrame(*frame, raw, static_cast<size_t>(len));
    h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
    if (!ok) {
      Log("[bind] " + why, 1);
      return err("E_APP", why);
    }
    if (replayed) {
      MV r = MV::Map();
      r.Set("rows", MV::Int(0));
      r.Set("replay", MV::Bool(true));
      return p.GetBool("sync") ? r : MV();
    }
    if (p.GetBool("sync")) {
      engine::Document::BindResult res;
      std::string e;
      const bool done = eng.Call([&](engine::Document& d) { return d.ApplyColumnFrame(*frame, &e, &res); });
      if (!done) return err("E_APP", e);
      MV r = MV::Map();
      r.Set("rows", MV::Int(res.rows));
      r.Set("added", MV::Int(res.added));
      r.Set("removed", MV::Int(res.removed));
      r.Set("cells", MV::Int(res.cells));
      return r;
    }
    EnqueueFrame(std::move(frame), false);  // the backpressure wait was before the decode
    return {};
  }
  if (m == "bind.create") {
    model::BoundTable def;
    def.name = p.GetStr("name");
    def.key = p.GetStr("key");
    def.stale_after = p.GetDouble("stale_after", 0);
    def.show_age = p.GetBool("show_age");
    def.record = p.GetBool("record", true);
    ColumnsFromMV(p, &def);
    std::vector<std::string> names;
    if (const MV* a = p.Get("names")) {
      for (const MV& x : a->a) names.push_back(x.s);
    }
    const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    const uint32_t r0 = static_cast<uint32_t>(p.GetInt("row", 0)), c0 = static_cast<uint32_t>(p.GetInt("col", 0));
    std::string e;
    MV r = eng.Call([&](engine::Document& d) {
      def.sheet = sheet ? sheet : d.wb().sheets[static_cast<size_t>(std::clamp<int32_t>(d.wb().active_sheet, 0, static_cast<int32_t>(d.wb().sheets.size()) - 1))]->id;
      if (p.GetBool("replace") && d.FindBoundTable(def.name)) d.RemoveBoundTable(def.name, true);
      if (!d.CreateBoundTable(def, r0, c0, names, &e)) return MV();
      d.ClearUndo();
      return DefToMV(*d.FindBoundTable(def.name.empty() ? d.wb().bound_tables->back().name : def.name));
    });
    if (!e.empty()) return err("E_APP", e);
    return r;
  }
  if (m == "bind.set") {
    std::string e;
    MV r = eng.Call([&](engine::Document& d) {
      if (!d.UpdateBoundTable(p.GetStr("name"), [&](model::BoundTable& b) {
            if (p.Has("stale_after")) b.stale_after = p.GetDouble("stale_after");
            if (p.Has("show_age")) b.show_age = p.GetBool("show_age");
            if (p.Has("record")) b.record = p.GetBool("record", true);
            if (p.Has("columns")) ColumnsFromMV(p, &b);
          }, &e))
        return MV();
      return DefToMV(*d.FindBoundTable(p.GetStr("name")));
    });
    if (!e.empty()) return err("E_APP", e);
    return r;
  }
  if (m == "bind.remove") {
    const bool ok = eng.Call([&](engine::Document& d) {
      const bool removed = d.RemoveBoundTable(p.GetStr("name"), p.GetBool("delete"));
      if (removed) d.ClearUndo();
      return removed;
    });
    return MV::Bool(ok);
  }
  if (m == "bind.list") {
    return eng.Call([](engine::Document& d) {
      MV a = MV::Array();
      const int64_t now = engine::PerfCounterNs();
      for (const auto& b : *d.wb().bound_tables) {
        MV m = DefToMV(b);
        // the newest row update: freshness checks (BoundTable.age)
        int64_t newest = 0;
        if (auto t = d.BoundRowTimes(b.name)) {
          for (const auto& x : t->t) newest = std::max(newest, x.load(std::memory_order_relaxed));
        }
        m.Set("age", newest ? MV::Double(static_cast<double>(now - newest) / 1e9) : MV());
        a.Push(std::move(m));
      }
      return a;
    });
  }
  // ---- [YC] safety (safety_ui.cpp, model/safety.h) ----
  if (m == "safety.beat") {
    const std::string name = p.GetStr("name"), detail = p.GetStr("detail");
    const double timeout = p.GetDouble("timeout", 2.0);
    return OnUi([name, detail, timeout](AppState& s) {
      SafetyBeat(s, name, timeout, detail);
      return MV();
    });
  }
  if (m == "safety.alarm") {
    const std::string name = p.GetStr("name"), text = p.GetStr("text");
    return OnUi([name, text](AppState& s) {
      SafetySetAlarm(s, name, text);
      return MV();
    });
  }
  if (m == "safety.unbeat") {
    const std::string name = p.GetStr("name");
    return OnUi([name](AppState& s) {
      SafetyRemoveBeat(s, name);
      return MV();
    });
  }
  if (m == "safety.param_set" || m == "safety.param_remove") {
    model::ParameterDef d;
    d.sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    d.row = static_cast<uint32_t>(p.GetInt("row", 0));
    d.col = static_cast<uint32_t>(p.GetInt("col", 0));
    d.name = p.GetStr("name");
    d.type = static_cast<int>(std::clamp<int64_t>(p.GetInt("type", 0), 0, 5));
    if (const MV* v = p.Get("min"); v && !v->IsNil()) {
      d.has_min = true;
      d.min = v->Num();
    }
    if (const MV* v = p.Get("max"); v && !v->IsNil()) {
      d.has_max = true;
      d.max = v->Num();
    }
    if (const MV* a = p.Get("choices")) {
      for (const MV& x : a->a) d.choices.push_back(x.s);
    }
    d.confirm = p.GetBool("confirm", true);
    d.max_step = p.GetDouble("max_step", 0);
    std::string e;
    const bool ok = eng.Call([&](engine::Document& doc) {
      if (m == "safety.param_remove") return doc.RemoveParameter(d.sheet, d.row, d.col);
      return doc.SetParameter(d, &e);
    });
    if (!ok && !e.empty()) return err("E_APP", e);
    return MV::Bool(ok);
  }
  if (m == "safety.params") {
    return eng.Call([](engine::Document& doc) {
      MV a = MV::Array();
      for (const auto& d : *doc.wb().parameters) {
        MV x = MV::Map();
        x.Set("sheet", MV::Int(d.sheet));
        x.Set("row", MV::Int(d.row));
        x.Set("col", MV::Int(d.col));
        x.Set("name", MV::Str(d.name));
        x.Set("type", MV::Int(d.type));
        x.Set("min", d.has_min ? MV::Double(d.min) : MV());
        x.Set("max", d.has_max ? MV::Double(d.max) : MV());
        MV ch = MV::Array();
        for (const auto& c2 : d.choices) ch.Push(MV::Str(c2));
        x.Set("choices", std::move(ch));
        x.Set("confirm", MV::Bool(d.confirm));
        x.Set("max_step", MV::Double(d.max_step));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  if (m == "safety.log") {
    model::AuditEntry e;
    e.kind = p.GetStr("kind", "action");
    e.target = p.GetStr("target");
    e.before = p.GetStr("before");
    e.after = p.GetStr("after");
    e.source = p.GetStr("source", "python");
    e.user = p.GetStr("user", e.source);
    eng.Post([e = std::move(e)](engine::Document& doc) mutable { doc.AddAudit(std::move(e)); });
    return {};
  }
  if (m == "safety.log_get") {
    const int64_t limit = p.GetInt("limit", 0);
    return eng.Call([limit](engine::Document& doc) {
      const auto& log = *doc.wb().audit_log;
      const size_t from = limit > 0 && static_cast<size_t>(limit) < log.size() ? log.size() - static_cast<size_t>(limit) : 0;
      MV a = MV::Array();
      for (size_t i = from; i < log.size(); ++i) {
        const auto& e = log[i];
        MV x = MV::Map();
        x.Set("time", MV::Double(e.time));
        x.Set("user", MV::Str(e.user));
        x.Set("kind", MV::Str(e.kind));
        x.Set("target", MV::Str(e.target));
        x.Set("before", MV::Str(e.before));
        x.Set("after", MV::Str(e.after));
        x.Set("source", MV::Str(e.source));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  if (m == "safety.settings") {
    // reads, and changes the keys that are given (a PIN is stored as its hash; "" removes it)
    return eng.Call([&](engine::Document& doc) {
      model::SafetySettings st = *doc.wb().safety;
      bool changed = false;
      if (p.Has("start_read_only")) st.start_read_only = p.GetBool("start_read_only"), changed = true;
      if (p.Has("pin")) {
        const std::string pin = p.GetStr("pin");
        st.has_pin = !pin.empty();
        st.pin_hash = pin.empty() ? 0 : model::PinHash(pin);
        changed = true;
      }
      if (p.Has("operator_idle_s")) st.operator_idle_s = static_cast<int>(std::max<int64_t>(0, p.GetInt("operator_idle_s"))), changed = true;
      if (p.Has("kill_macro")) st.kill_macro = p.GetStr("kill_macro"), changed = true;
      if (p.Has("host_timeout_s")) st.host_timeout_s = std::max(0.5, p.GetDouble("host_timeout_s", 3)), changed = true;
      if (changed) doc.SetSafetySettings(st);
      MV o = MV::Map();
      o.Set("start_read_only", MV::Bool(st.start_read_only));
      o.Set("has_pin", MV::Bool(st.has_pin));
      o.Set("operator_idle_s", MV::Int(st.operator_idle_s));
      o.Set("kill_macro", MV::Str(st.kill_macro));
      o.Set("host_timeout_s", MV::Double(st.host_timeout_s));
      return o;
    });
  }
  if (m == "safety.mode") {
    const bool set = p.Has("operator");
    const bool on = p.GetBool("operator");
    const std::string pin = p.GetStr("pin");
    std::string e;
    MV r = OnUi([&e, set, on, pin](AppState& s) {
      if (set) SetOperatorMode(s, on, pin, &e);
      MV o = MV::Map();
      o.Set("operator", MV::Bool(s.operator_mode));
      MV al = MV::Array();
      for (const auto& a : SafetyAlarms(s)) al.Push(MV::Str(a));
      o.Set("alarms", std::move(al));
      MV hb = MV::Array();
      const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      for (const auto& h : s.heartbeats) {
        MV x = MV::Map();
        x.Set("name", MV::Str(h.name));
        x.Set("age", MV::Double(now - h.last));
        x.Set("timeout", MV::Double(h.timeout));
        x.Set("detail", MV::Str(h.detail));
        hb.Push(std::move(x));
      }
      o.Set("heartbeats", std::move(hb));
      return o;
    });
    if (!e.empty()) return err("E_APP", e);
    return r;
  }
  if (m == "safety.kill") {
    const bool ask = p.GetBool("ask");
    std::string e;
    MV r = OnUi([&e, ask](AppState& s) { return MV::Bool(RunKillSwitch(s, ask, "python", &e)); });
    if (!e.empty()) return err("E_APP", e);
    return r;
  }
  // ---- [YC] trading screens (trading_ui.cpp) ----
  if (m == "ui.onkey") {
    // Application.OnKey(Key, Procedure): procedure None = the key works normally again; "" = the key does nothing
    const std::string key = p.GetStr("key");
    const MV* proc = p.Get("procedure");
    int chord = 0;
    if (!ParseOnKey(key, &chord)) return err("E_ARG", "Unknown key: " + key + " (use ^ Ctrl, + Shift, % Alt and a letter, a digit or {F5}, {ENTER}...)");
    const bool remove = !proc || proc->IsNil();
    const std::string procedure = remove ? std::string() : proc->s;
    return OnUi([key, procedure, chord, remove](AppState& s) {
      s.on_keys.erase(std::remove_if(s.on_keys.begin(), s.on_keys.end(), [&](const AppState::OnKey& k) { return k.chord == chord; }), s.on_keys.end());
      if (!remove) s.on_keys.push_back(AppState::OnKey{key, procedure, chord});
      return MV();
    });
  }
  if (m == "ui.click_range" || m == "ui.click_range_remove") {
    AppState::ClickRange cr;
    cr.id = p.GetInt("id", 0);
    cr.sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    if (const MV* r = p.Get("rect"); r && r->a.size() == 4)
      cr.rect = Rect{static_cast<uint32_t>(r->a[0].i), static_cast<uint32_t>(r->a[1].i), static_cast<uint32_t>(r->a[2].i), static_cast<uint32_t>(r->a[3].i)};
    cr.left = p.GetBool("left", true);
    cr.right = p.GetBool("right", false);
    const bool add = m == "ui.click_range";
    return OnUi([cr, add](AppState& s) {
      s.click_ranges.erase(std::remove_if(s.click_ranges.begin(), s.click_ranges.end(), [&](const AppState::ClickRange& x) { return x.id == cr.id; }),
                           s.click_ranges.end());
      if (add) s.click_ranges.push_back(cr);
      return MV();
    });
  }
  if (m == "ui.cell_context") {
    const bool given = p.Has("row");
    const uint32_t sh = static_cast<uint32_t>(p.GetInt("sheet", 0)), row = static_cast<uint32_t>(p.GetInt("row", 0)), col = static_cast<uint32_t>(p.GetInt("col", 0));
    return OnUi([given, sh, row, col](AppState& s) {
      if (given) return CellContextMV(s, sh ? sh : s.focus_sheet, row, col);
      const auto& sel = s.FocusView().sel;
      return CellContextMV(s, s.focus_sheet, sel.act_r, sel.act_c);
    });
  }
  if (m == "alerts.set") {
    model::AlertRule a;
    a.name = p.GetStr("name");
    if (a.name.empty()) return err("E_ARG", "An alert needs a name");
    a.sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    if (const MV* r = p.Get("rect"); r && r->a.size() == 4)
      a.range = Rect{static_cast<uint32_t>(r->a[0].i), static_cast<uint32_t>(r->a[1].i), static_cast<uint32_t>(r->a[2].i), static_cast<uint32_t>(r->a[3].i)};
    a.op = static_cast<int>(std::clamp<int64_t>(p.GetInt("op", 1), 1, 9));
    a.a = p.GetDouble("a", 0);
    a.b = p.GetDouble("b", 0);
    a.sound = p.GetBool("sound", true);
    a.popup = p.GetBool("popup", true);
    a.flash_window = p.GetBool("flash_window", false);
    a.notify = p.GetBool("notify", false);
    a.enabled = p.GetBool("enabled", true);
    a.color = static_cast<uint32_t>(p.GetInt("color", 0xFFFFC107));
    a.cooldown_s = p.GetDouble("cooldown", 5);
    eng.Call([&](engine::Document& d) { d.SetAlert(a); });
    return MV::Bool(true);
  }
  if (m == "alerts.remove") {
    const std::string name = p.GetStr("name");
    return MV::Bool(eng.Call([&](engine::Document& d) { return d.RemoveAlert(name); }));
  }
  if (m == "alerts.list") {
    return eng.Call([](engine::Document& d) {
      MV a = MV::Array();
      for (const auto& r : *d.wb().alerts) {
        MV x = MV::Map();
        x.Set("name", MV::Str(r.name));
        x.Set("sheet", MV::Int(r.sheet));
        MV rc = MV::Array();
        for (uint32_t v : {r.range.r0, r.range.c0, r.range.r1, r.range.c1}) rc.Push(MV::Int(v));
        x.Set("rect", std::move(rc));
        x.Set("op", MV::Int(r.op));
        x.Set("a", MV::Double(r.a));
        x.Set("b", MV::Double(r.b));
        x.Set("sound", MV::Bool(r.sound));
        x.Set("popup", MV::Bool(r.popup));
        x.Set("flash_window", MV::Bool(r.flash_window));
        x.Set("notify", MV::Bool(r.notify));
        x.Set("enabled", MV::Bool(r.enabled));
        x.Set("color", MV::Int(r.color));
        x.Set("cooldown", MV::Double(r.cooldown_s));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  // ---- [YC] yc.plot (figures_ui.cpp): an optional frame of arrays in the channel, then the request on the UI thread ----
  if (m.rfind("plot.", 0) == 0) {
    std::shared_ptr<engine::RawFrame> frame;
    if (p.Has("pos")) {
      std::shared_ptr<DataChannel> ch;
      {
        std::lock_guard<std::mutex> lk(c.chan_mu);
        if (!c.channels.empty()) ch = c.channels.front();
      }
      if (!ch) return err("E_APP", "No data channel");
      const uint64_t pos = static_cast<uint64_t>(p.GetInt("pos", 0)), len = static_cast<uint64_t>(p.GetInt("len", 0));
      const uint64_t cap = ch->size - kChanHeader;
      auto* h = reinterpret_cast<ChanHeader*>(ch->view);
      if (pos > cap || len > cap - pos) {
        h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
        return err("E_APP", "The frame is outside the data channel");
      }
      frame = std::make_shared<engine::RawFrame>();
      std::string why;
      const bool ok = engine::DecodeRawFrame(ch->view + kChanHeader + pos, static_cast<size_t>(len), frame.get(), &why);
      h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
      if (!ok) return err("E_APP", why);
    }
    NpyPreload files;  // column files (Axes.plot_files) are read here, not on the UI thread
    if (const MV* fl = p.Get("files"); fl && fl->t == MV::T::Map) {
      for (const auto& [name, path] : fl->m) files.Add(FromUtf8(path.s));
    }
    std::string e;
    const int64_t owner = c.id;
    const bool host = c.host;
    const std::string owner_name = c.host ? std::string("host") : "script " + std::to_string(c.pid);
    MV r = OnUi([&e, m, p, frame, owner, host, owner_name](AppState& s) {
      SetFigureOwner(owner, owner_name, host);
      MV v = FigureRequest(s, m, p, frame, &e);
      SetFigureOwner(0, {}, false);
      return v;
    });
    if (!e.empty()) return err("E_APP", e);
    return r;
  }
  if (m == "analysis.run") {
    // Analysis ToolPak: {tool (Excel's macro name), in / in2 / bins: {sheet, rect}, out: {sheet, rect} or new_sheet, options}
    Document::AnalysisRequest req;
    if (!engine::analysis::ToolFromMacro(p.GetStr("tool"), &req.p.tool)) return err("E_ARG", "Unknown analysis tool: " + p.GetStr("tool"));
    auto range = [&](const char* key, uint32_t* sheet, core::Rect* r) {
      const MV* v = p.Get(key);
      if (!v || v->t != MV::T::Map) return false;
      const MV* rc = v->Get("rect");
      if (!rc || rc->a.size() != 4) return false;
      *sheet = static_cast<uint32_t>(v->GetInt("sheet", 0));
      *r = core::Rect{static_cast<uint32_t>(rc->a[0].Num()), static_cast<uint32_t>(rc->a[1].Num()), static_cast<uint32_t>(rc->a[2].Num()),
                      static_cast<uint32_t>(rc->a[3].Num())};
      return true;
    };
    req.has_in = range("in", &req.in_sheet, &req.in);
    req.has_in2 = range("in2", &req.in2_sheet, &req.in2);
    req.has_bins = range("bins", &req.bins_sheet, &req.bins);
    core::Rect out{};
    if (range("out", &req.out_sheet, &out)) {
      req.out_r = out.r0;
      req.out_c = out.c0;
    } else {
      req.new_sheet = true;
      req.sheet_name = p.GetStr("new_sheet");
      if (!req.has_in) req.in_sheet = static_cast<uint32_t>(p.GetInt("active_sheet", 0));
    }
    auto& o = req.p;
    o.by_rows = p.GetBool("by_rows");
    o.labels = p.GetBool("labels");
    o.alpha = p.GetDouble("alpha", 0.05);
    o.summary = p.GetBool("summary", true);
    o.conf = p.GetDouble("conf", 0);
    o.kth_largest = static_cast<int>(p.GetInt("k1", 0));
    o.kth_smallest = static_cast<int>(p.GetInt("k2", 0));
    o.rows_per_sample = static_cast<int>(p.GetInt("rows_per_sample", 1));
    o.damping = p.GetDouble("damping", 0.3);
    o.interval = static_cast<int>(p.GetInt("interval", 3));
    o.std_errors = p.GetBool("std_errors");
    o.hyp_diff = p.GetDouble("difference", 0);
    o.var1 = p.GetDouble("var1", 0);
    o.var2 = p.GetDouble("var2", 0);
    o.inverse = p.GetBool("inverse");
    o.pareto = p.GetBool("pareto");
    o.cumulative = p.GetBool("cumulative");
    o.variables = static_cast<int>(p.GetInt("variables", 1));
    o.points = static_cast<int>(p.GetInt("points", 0));
    o.distribution = static_cast<int>(p.GetInt("distribution", 1));
    o.p1 = p.GetDouble("p1", 0);
    o.p2 = p.GetDouble("p2", 1);
    o.p3 = p.GetDouble("p3", 1);
    o.p4 = p.GetDouble("p4", 1);
    o.p5 = p.GetDouble("p5", 1);
    o.seed = static_cast<uint64_t>(std::max<int64_t>(0, p.GetInt("seed", 0)));
    o.zero_constant = p.GetBool("zero_constant");
    o.confidence = p.GetDouble("confidence", 0);
    o.residuals = p.GetBool("residuals");
    o.std_residuals = p.GetBool("std_residuals");
    o.normal_probability = p.GetBool("normal_probability");
    o.periodic = p.GetBool("periodic", true);
    o.period = static_cast<int>(p.GetInt("period", 1));
    o.count = static_cast<int>(p.GetInt("count", 1));
    std::string e;
    uint32_t osheet = 0;
    core::Rect orect{};
    const bool ok = eng.Call([&](Document& d) {
      d.SettleForRead();  // the input a script just wrote as formulas has its values
      return d.RunAnalysis(req, &osheet, &orect, &e);
    });
    if (!ok) return err("E_ARG", e);
    MV r = MV::Map();
    r.Set("sheet", MV::Int(osheet));
    MV rc = MV::Array();
    for (uint32_t x : {orect.r0, orect.c0, orect.r1, orect.c1}) rc.Push(MV::Int(x));
    r.Set("rect", std::move(rc));
    return r;
  }
  if (m == "range.write_frame") {
    // DataFrame columns into a block of cells (Worksheet.write_frame): the frame comes through the data channel
    std::shared_ptr<DataChannel> ch;
    {
      std::lock_guard<std::mutex> lk(c.chan_mu);
      if (!c.channels.empty()) ch = c.channels.front();
    }
    if (!ch) return err("E_APP", "No data channel");
    const uint64_t pos = static_cast<uint64_t>(p.GetInt("pos", 0)), len = static_cast<uint64_t>(p.GetInt("len", 0));
    const uint64_t cap = ch->size - kChanHeader;
    auto* h = reinterpret_cast<ChanHeader*>(ch->view);
    if (pos > cap || len > cap - pos) {
      h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
      return err("E_APP", "The frame is outside the data channel");
    }
    engine::ColumnFrame f;
    std::string why;
    const bool ok = engine::DecodeColumnFrame(ch->view + kChanHeader + pos, static_cast<size_t>(len), &f, &why);
    h->tail.store(static_cast<uint64_t>(p.GetInt("end", 0)), std::memory_order_release);
    if (!ok) return err("E_APP", why);
    const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0)), r0 = static_cast<uint32_t>(p.GetInt("row", 0)),
                   c0 = static_cast<uint32_t>(p.GetInt("col", 0));
    const bool header = p.GetBool("header", true);
    std::vector<std::string> formats;
    if (const MV* fm = p.Get("formats")) {
      for (const MV& x : fm->a) formats.push_back(x.t == MV::T::Str ? x.s : std::string());
    }
    const uint32_t ncols = static_cast<uint32_t>(f.cols.size()), nrows = f.nrows + (header ? 1u : 0u);
    if (!ncols) return MV();
    if (c0 + ncols > core::kMaxCols) return err("E_ARG", "The data don't fit on the sheet");
    std::string e;
    const bool done = eng.Call([&](engine::Document& d) {
      const Rect rect{r0, c0, r0 + std::max(nrows, 1u) - 1, c0 + ncols - 1};
      if (!d.S(sheet)) {
        e = "The sheet doesn't exist";
        return false;
      }
      d.Begin("Write DataFrame");
      std::vector<core::Value> vals;
      vals.reserve(static_cast<size_t>(nrows) * ncols);
      if (header) {
        for (const auto& n : f.names) vals.push_back(core::Value::String(n));
      }
      for (uint32_t r = 0; r < f.nrows; ++r) {
        for (uint32_t k = 0; k < ncols; ++k) vals.push_back(f.cols[k][r]);
      }
      if (nrows) d.SetValues(sheet, rect, vals, engine::kSrcPython);
      for (uint32_t k = 0; k < ncols && k < formats.size(); ++k) {
        if (formats[k].empty() || !f.nrows) continue;
        const uint32_t fmt = d.wb_mut().styles.mut().InternString(formats[k]);
        d.ModifyStyle(sheet, Rect{r0 + (header ? 1u : 0u), c0 + k, r0 + nrows - 1, c0 + k}, [fmt](model::Style& st) { st.numfmt = fmt; });
      }
      d.Commit();
      return true;
    });
    if (!done) return err("E_ARG", e);
    MV r = MV::Array();
    for (uint32_t x : {r0, c0, r0 + std::max(nrows, 1u) - 1, c0 + ncols - 1}) r.Push(MV::Int(x));
    return r;
  }
  if (m == "table.read" || m == "range.read") {
    // a table's body (or a range, with its first row as the column names) as a column frame in shared memory: a
    // DataFrame without a Python object per cell
    engine::ColumnFrame f;
    std::string e;
    std::vector<bool> dates;
    const bool ok = eng.Call([&](engine::Document& d) {
      d.SettleForRead();  // a formula written just before has its value (a frame read it as empty)
      if (m == "range.read") {
        const uint32_t sid = static_cast<uint32_t>(p.GetInt("sheet", 0));
        const model::Sheet* sh = d.S(sid);
        const MV* rc = p.Get("rect");
        if (!sh || !rc || rc->a.size() != 4) {
          e = "The range isn't valid";
          return false;
        }
        Rect r{static_cast<uint32_t>(rc->a[0].Num()), static_cast<uint32_t>(rc->a[1].Num()), static_cast<uint32_t>(rc->a[2].Num()),
               static_cast<uint32_t>(rc->a[3].Num())};
        if (sh->has_used) {  // whole columns or rows: up to the used range
          r.r1 = std::min(r.r1, std::max(r.r0, sh->used.r1));
          r.c1 = std::min(r.c1, std::max(r.c0, sh->used.c1));
        }
        if (static_cast<uint64_t>(r.Rows()) * r.Cols() > 50'000'000) {
          e = "The range is too large to read at once";
          return false;
        }
        const bool header = p.GetBool("header", true);
        const uint32_t body = r.r0 + (header ? 1u : 0u);
        f = engine::ColumnFrame{};
        f.op = engine::FrameOp::Replace;
        f.nrows = body <= r.r1 ? r.r1 - body + 1 : 0;
        const auto& styles = *d.wb().styles;
        for (uint32_t c = r.c0; c <= r.c1; ++c) {
          std::string name;
          if (header) name = engine::DisplayTextOf(sh->GetValue(r.r0, c), "General");
          if (name.empty()) name = "Column" + std::to_string(c - r.c0 + 1);
          f.names.push_back(name);
          std::vector<core::Value> v(f.nrows);
          for (uint32_t k = 0; k < f.nrows; ++k) v[k] = sh->GetValue(body + k, c);
          f.cols.push_back(std::move(v));
          const uint32_t st = f.nrows ? sh->GetStyle(body, c) : 0;
          dates.push_back(st && formula::IsDateFormat(styles.NumberFormat(styles.Get(st))));
        }
        return true;
      }
      if (!d.TableFrame(p.GetStr("name"), false, &f, &e)) return false;
      uint32_t sid = 0;
      const model::Table* t = model::FindTable(d.wb(), p.GetStr("name"), &sid);
      const model::Sheet* sh = t ? d.S(sid) : nullptr;
      const auto& styles = *d.wb().styles;
      for (size_t i = 0; i < f.names.size(); ++i) {
        bool date = false;
        if (sh && f.nrows) {
          const int c = t->ColumnIndex(f.names[i]);
          const uint32_t st = c >= 0 ? sh->GetStyle(t->Data().r0, t->range.c0 + static_cast<uint32_t>(c)) : 0;
          date = st && formula::IsDateFormat(styles.NumberFormat(styles.Get(st)));
        }
        dates.push_back(date);
      }
      return true;
    });
    if (!ok) return err("E_ARG", e);
    const std::vector<uint8_t> bytes = engine::EncodeColumnFrame(f);
    std::lock_guard<std::mutex> lk(c.chan_mu);
    if (!c.out || c.out->size < bytes.size()) {
      uint64_t size = 1u << 20;
      while (size < bytes.size()) size <<= 1;
      static std::atomic<int> seq{0};
      const std::wstring name = L"Local\\ycgrid." + std::to_wstring(::GetCurrentProcessId()) + L".read." + std::to_wstring(++seq);
      auto ch = std::make_shared<DataChannel>();
      ch->mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
      if (!ch->mapping) return err("E_APP", "The shared memory for the read couldn't be created");
      ch->view = static_cast<uint8_t*>(::MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size)));
      if (!ch->view) return err("E_APP", "The shared memory for the read couldn't be mapped");
      ch->size = size;
      ch->name = ToUtf8(name);
      c.out = std::move(ch);
    }
    std::memcpy(c.out->view, bytes.data(), bytes.size());
    MV r = MV::Map();
    r.Set("name", MV::Str(c.out->name));
    r.Set("size", MV::Int(static_cast<int64_t>(c.out->size)));
    r.Set("len", MV::Int(static_cast<int64_t>(bytes.size())));
    MV dm = MV::Array();
    for (bool b : dates) dm.Push(MV::Bool(b));
    r.Set("dates", std::move(dm));
    r.Set("date_offset", MV::Double(formula::DateOffset()));  // Python reads the 1900 system's serials
    return r;
  }
  if (m.rfind("replay.", 0) == 0) {
    const std::wstring path = FromUtf8(p.GetStr("path"));
    std::string e;
    if (m == "replay.record") {
      std::vector<std::string> tables;
      if (const MV* t = p.Get("tables")) {
        for (const auto& x : t->a) tables.push_back(x.s);
      }
      std::wstring target = path;
      if (target.empty()) target = FromUtf8(OnUi([](AppState& s) { return MV::Str(ToUtf8(DefaultRecordingPath(s))); }).s);
      PlotRecords figs;
      // the figures for "*", and for the default (with no table marked as market data it records the whole screen)
      if (tables.empty() || std::find(tables.begin(), tables.end(), "*") != tables.end()) {
        OnUi([&figs](AppState&) {
          figs = FigureSnapshot();
          return MV();
        });
      }
      if (!StartRecording(target, tables, &e, &figs)) return err("E_APP", e + ": " + ToUtf8(target));
      return MV::Str(ToUtf8(target));
    }
    if (m == "replay.stop_record") {
      StopRecording();
      return {};
    }
    if (m == "replay.mark") {
      TapeMark(p.GetStr("kind", "note"), p.GetStr("text"));
      return {};
    }
    if (m == "replay.pin") return MV::Bool(PinTables());
    if (m == "replay.compare") return CompareWithPin(static_cast<size_t>(std::clamp<int64_t>(p.GetInt("limit", 500), 1, 100000)));
    if (m == "replay.play") {
      if (!StartReplay(path, p.GetDouble("speed", 1), p.GetDouble("start", 0), p.GetBool("loop"), &e)) return err("E_APP", e + ": " + p.GetStr("path"));
      return {};
    }
    if (m == "replay.pause") {
      PauseReplay(p.GetBool("pause", true));
      return {};
    }
    if (m == "replay.seek") {
      SeekReplay(p.GetDouble("seconds", 0));
      return {};
    }
    if (m == "replay.speed") {
      SetReplaySpeed(p.GetDouble("speed", 1));
      return {};
    }
    if (m == "replay.loop") {
      SetReplayLoop(p.GetBool("loop"));
      return {};
    }
    if (m == "replay.live") {
      StopReplay();
      return {};
    }
    if (m == "replay.status") {
      const TapeStatus st = GetTapeStatus();
      static const char* modes[] = {"live", "replay", "paused", "ended"};
      MV r = MV::Map();
      r.Set("mode", MV::Str(modes[std::clamp(st.mode, 0, 3)]));
      r.Set("recording", MV::Bool(st.recording));
      r.Set("record_path", MV::Str(st.rec_path));
      r.Set("record_seconds", MV::Double(st.rec_seconds));
      r.Set("record_frames", MV::Int(static_cast<int64_t>(st.rec_frames)));
      r.Set("record_bytes", MV::Int(static_cast<int64_t>(st.rec_bytes)));
      r.Set("path", MV::Str(st.path));
      r.Set("position", MV::Double(st.pos_s));
      r.Set("length", MV::Double(st.length_s));
      r.Set("speed", MV::Double(st.speed));
      r.Set("loop", MV::Bool(st.loop));
      r.Set("started_ms", MV::Int(st.start_unix_ms));
      r.Set("frame", MV::Int(static_cast<int64_t>(st.frame)));
      r.Set("frames", MV::Int(static_cast<int64_t>(st.frames)));
      r.Set("dropped", MV::Int(static_cast<int64_t>(st.dropped)));
      MV t = MV::Array();
      for (const auto& x : st.tables) t.Push(MV::Str(x));
      r.Set("tables", std::move(t));
      MV mk = MV::Array();
      for (const auto& x : st.marks) {
        MV mark = MV::Map();
        mark.Set("t", MV::Double(x.t));
        mark.Set("kind", MV::Str(x.kind));
        mark.Set("text", MV::Str(x.text));
        mk.Push(std::move(mark));
      }
      r.Set("marks", std::move(mk));
      r.Set("pinned", MV::Bool(st.pinned));
      return r;
    }
  }
  if (m == "bind.error") {
    // the last error of asynchronous frames for a table ("" = none); reading it clears it
    std::lock_guard<std::mutex> lk(state_mu_);
    auto it = bind_errors_.find(engine::Lower(p.GetStr("name")));
    if (it == bind_errors_.end()) return MV::Str("");
    std::string e = it->second;
    bind_errors_.erase(it);
    return MV::Str(e);
  }
  *handled = false;
  return {};
}

}  // namespace yc::app
