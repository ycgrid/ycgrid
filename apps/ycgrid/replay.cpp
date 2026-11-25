// SPDX-License-Identifier: Apache-2.0
// replay.cpp - [YC] record bound-table data to a tape and replay it into the same tables (engine/tape.h)
//
// Recording writes every frame that reaches a bound table, after a snapshot of the tables at the start. Replaying feeds
// the tape's frames through the same inbox as live frames, at the recorded pace times a speed; live frames for the
// tables on the tape are dropped meanwhile, so the screen and the Python code reading it don't change. A seek replays
// from the start without waiting (the snapshot rebuilds the tables); going back to live restores the tables as they
// were before the replay.
#include "python_bridge.h"

#include "app_state.h"
#include "core/i18n.h"
#include "engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>

namespace yc::app {

namespace {

int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Listed(const std::vector<std::string>& names, const std::string& table) {
  if (names.empty()) return true;
  const std::string t = engine::Lower(table);
  return std::find(names.begin(), names.end(), t) != names.end();
}

// a figure request on the tape: u32 method length, method, u32 params length, params (msgpack), then the data columns:
// u32 columns, u32 rows, per column u32 name length, name, u8 type (0 numbers, 1 text), values
void PutU32(std::vector<uint8_t>& b, uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
void PutStr(std::vector<uint8_t>& b, const std::string& s) {
  PutU32(b, static_cast<uint32_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}

std::vector<uint8_t> EncodePlot(const std::string& m, const io::MV& p, const engine::RawFrame* data) {
  std::vector<uint8_t> b;
  PutStr(b, m);
  io::MsgWriter w;
  io::Encode(w, p);
  PutStr(b, std::string(reinterpret_cast<const char*>(w.buf().data()), w.buf().size()));
  if (!data) return b;
  PutU32(b, static_cast<uint32_t>(data->names.size()));
  PutU32(b, data->nrows);
  for (size_t c = 0; c < data->names.size(); ++c) {
    PutStr(b, data->names[c]);
    const bool text = data->num[c].empty() && !data->text[c].empty();
    b.push_back(text ? 1 : 0);
    if (text) {
      PutU32(b, static_cast<uint32_t>(data->text[c].size()));
      for (const auto& t : data->text[c]) PutStr(b, t);
    } else {
      PutU32(b, static_cast<uint32_t>(data->num[c].size()));
      const auto* raw = reinterpret_cast<const uint8_t*>(data->num[c].data());
      b.insert(b.end(), raw, raw + data->num[c].size() * sizeof(double));
    }
  }
  return b;
}

bool DecodePlot(const std::vector<uint8_t>& b, std::string* m, io::MV* p, std::shared_ptr<engine::RawFrame>* data) {
  size_t o = 0;
  auto u32 = [&](uint32_t* v) {
    if (o + 4 > b.size()) return false;
    std::memcpy(v, b.data() + o, 4);
    o += 4;
    return true;
  };
  auto str = [&](std::string* s) {
    uint32_t n = 0;
    if (!u32(&n) || o + n > b.size()) return false;
    s->assign(reinterpret_cast<const char*>(b.data() + o), n);
    o += n;
    return true;
  };
  std::string params;
  if (!str(m) || !str(&params)) return false;
  io::MsgReader r(params);
  if (!io::Decode(r, p)) return false;
  if (o >= b.size()) return true;  // no data
  auto f = std::make_shared<engine::RawFrame>();
  uint32_t nc = 0;
  if (!u32(&nc) || !u32(&f->nrows)) return false;
  for (uint32_t c = 0; c < nc; ++c) {
    std::string name;
    if (!str(&name) || o >= b.size()) return false;
    const uint8_t type = b[o++];
    uint32_t n = 0;
    if (!u32(&n)) return false;
    f->names.push_back(name);
    f->num.emplace_back();
    f->text.emplace_back();
    if (type == 1) {
      for (uint32_t k = 0; k < n; ++k) {
        std::string t;
        if (!str(&t)) return false;
        f->text.back().push_back(std::move(t));
      }
    } else {
      if (o + static_cast<size_t>(n) * sizeof(double) > b.size()) return false;
      f->num.back().resize(n);
      std::memcpy(f->num.back().data(), b.data() + o, static_cast<size_t>(n) * sizeof(double));
      o += static_cast<size_t>(n) * sizeof(double);
    }
  }
  *data = std::move(f);
  return true;
}

}  // namespace

void PythonBridge::TapePlot(const std::string& method, const io::MV& params, const std::shared_ptr<engine::RawFrame>& data) {
  if (!tape_.writer.IsOpen()) return;  // (a racy peek; checked again below)
  const std::vector<uint8_t> bytes = EncodePlot(method, params, data.get());
  std::lock_guard<std::mutex> lk(tape_.mu);
  if (tape_.writer.IsOpen() && tape_.rec_everything)
    tape_.writer.Write(engine::PerfCounterNs() - tape_.rec_start_ns, bytes.data(), bytes.size(), engine::kTapePlot);
}

void PythonBridge::TapeMark(const std::string& kind, const std::string& text) {
  std::lock_guard<std::mutex> lk(tape_.mu);
  if (!tape_.writer.IsOpen()) return;
  const std::string rec = (kind.empty() ? std::string("note") : kind) + "\t" + text;
  tape_.writer.Write(engine::PerfCounterNs() - tape_.rec_start_ns, reinterpret_cast<const uint8_t*>(rec.data()), rec.size(), engine::kTapeMark);
}

bool PythonBridge::PlotOnTape(int64_t fig) {
  if (!tape_.replaying.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lk(tape_.mu);
  return tape_.plot_figs.count(fig) > 0;
}

bool PythonBridge::PinTables() {
  std::vector<std::string> tables;
  double at = 0;
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    tables = tape_.replaying ? tape_.tables : tape_.rec_tables;
    at = tape_.replaying ? static_cast<double>(tape_.pos_ns) / 1e9 : 0;
  }
  std::vector<std::shared_ptr<engine::ColumnFrame>> pin;
  s_.engine->Call([&](engine::Document& d) {
    for (const auto& b : *d.wb().bound_tables) {
      if (!tables.empty() && !Listed(tables, b.name)) continue;
      auto f = std::make_shared<engine::ColumnFrame>();
      if (d.TableFrame(b.name, true, f.get(), nullptr)) pin.push_back(std::move(f));
    }
    return true;
  });
  std::lock_guard<std::mutex> lk(tape_.mu);
  tape_.pin = std::move(pin);
  tape_.pin_s = at;
  tape_.pinned = true;
  return !tape_.pin.empty();
}

io::MV PythonBridge::CompareWithPin(size_t limit) {
  std::vector<std::shared_ptr<engine::ColumnFrame>> pin;
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    pin = tape_.pin;
  }
  io::MV out = io::MV::Array();
  // per table: rows matched by the key column (the first column of the frame when the table has no key)
  for (const auto& then : pin) {
    engine::ColumnFrame now;
    std::string key;
    bool ok = s_.engine->Call([&](engine::Document& d) {
      if (const auto* b = d.FindBoundTable(then->table)) key = b->key;
      return d.TableFrame(then->table, true, &now, nullptr);
    });
    if (!ok || then->cols.empty() || now.cols.empty()) continue;
    auto col_of = [](const engine::ColumnFrame& f, const std::string& name) -> int {
      for (size_t c = 0; c < f.names.size(); ++c) {
        if (engine::Lower(f.names[c]) == engine::Lower(name)) return static_cast<int>(c);
      }
      return -1;
    };
    auto text = [](const core::Value& v) { return engine::DisplayTextOf(v, "General"); };
    auto at = [](const engine::ColumnFrame& f, size_t c, size_t r) { return r < f.cols[c].size() ? f.cols[c][r] : core::Value(); };
    const int kt = key.empty() ? 0 : col_of(*then, key), kn = key.empty() ? 0 : col_of(now, key);
    if (kt < 0 || kn < 0) continue;
    std::map<std::string, size_t> rows_now;
    for (size_t r = 0; r < now.nrows; ++r) rows_now[text(at(now, static_cast<size_t>(kn), r))] = r;
    for (size_t r = 0; r < then->nrows && out.a.size() < limit; ++r) {
      const std::string k = text(at(*then, static_cast<size_t>(kt), r));
      auto it = rows_now.find(k);
      for (size_t c = 0; c < then->cols.size() && out.a.size() < limit; ++c) {
        if (static_cast<int>(c) == kt) continue;
        const int cn = col_of(now, then->names[c]);
        const core::Value a = at(*then, c, r);
        const core::Value b = it == rows_now.end() || cn < 0 ? core::Value() : at(now, static_cast<size_t>(cn), it->second);
        if (a.SameAs(b)) continue;
        io::MV x = io::MV::Map();
        x.Set("table", io::MV::Str(then->table));
        x.Set("key", io::MV::Str(k));
        x.Set("column", io::MV::Str(then->names[c]));
        x.Set("then", io::MV::Str(text(a)));
        x.Set("now", io::MV::Str(it == rows_now.end() ? std::string("(gone)") : text(b)));
        if (a.IsNumeric() && b.IsNumeric()) x.Set("change", io::MV::Double(b.AsDouble() - a.AsDouble()));
        out.Push(std::move(x));
      }
    }
  }
  return out;
}

bool PythonBridge::StartRecording(const std::wstring& path, const std::vector<std::string>& tables, std::string* error, const PlotRecords* figures) {
  std::vector<std::string> want;
  bool everything = false;  // "*": every bound table (the app's own too), topics and figures - the time machine
  for (const auto& t : tables) {
    if (t == "*") everything = true;
    else if (!t.empty()) want.push_back(engine::Lower(t));
  }
  if (everything) want.clear();
  // the snapshot: every bound table as it is now
  std::vector<engine::ColumnFrame> snap;
  size_t topics = 0;
  auto snapshot = [&] {
    snap.clear();
    s_.engine->Call([&](engine::Document& d) {
      topics = d.GetTopicStats().topics;
      for (const auto& b : *d.wb().bound_tables) {
        if (!everything && (want.empty() ? !b.record : !Listed(want, b.name))) continue;
        engine::ColumnFrame f;
        if (d.TableFrame(b.name, true, &f, nullptr)) snap.push_back(std::move(f));
      }
      return true;
    });
  };
  snapshot();
  bool fallback = false;
  if (!everything && want.empty() && snap.empty()) {
    // no table is marked as market data (bind(record=False)): Record takes the whole screen, the time machine, rather
    // than writing a file with nothing in it
    everything = fallback = true;
    snapshot();
  }
  if (everything && snap.empty() && topics == 0 && (!figures || figures->empty())) {
    if (error)
      *error = TR("Nothing here can be recorded. Record captures live data: bound tables (yc.data.bind), =QUOTE() cells and "
                  "yc.plot charts; typed cells aren't recorded.");
    return false;
  }
  std::lock_guard<std::mutex> lk(tape_.mu);
  tape_.writer.Close();
  if (!tape_.writer.Open(path, UnixMs(), error)) return false;
  tape_.rec_path = ToUtf8(path);
  // topic pushes (=QUOTE() cells) are recorded with all tables, or when "@topics" is listed
  tape_.rec_topics = want.empty() || std::find(want.begin(), want.end(), "@topics") != want.end();
  if (want.empty()) {
    for (const auto& f : snap) want.push_back(engine::Lower(f.table));
    if (want.empty()) want.push_back(std::string(1, static_cast<char>(1)));  // nothing marked for recording: record nothing (empty = all)
  }
  tape_.rec_tables = std::move(want);
  tape_.rec_start_ns = engine::PerfCounterNs();
  tape_.rec_everything = everything;
  for (const auto& f : snap) {
    const std::vector<uint8_t> bytes = engine::EncodeColumnFrame(f);
    tape_.writer.Write(0, bytes.data(), bytes.size());
  }
  if (everything && figures) {
    // the figures as they are now (rebuilt when a replay goes back to the start)
    for (const auto& [m, p, data] : *figures) {
      const std::vector<uint8_t> bytes = EncodePlot(m, p, data.get());
      tape_.writer.Write(0, bytes.data(), bytes.size(), engine::kTapePlot);
    }
  }
  if (fallback) Log("[replay] " + std::string(TR("No table is marked as market data: recording everything on the screen")), 2);
  Log("[replay] " + std::string(TR("Recording to ")) + tape_.rec_path, 2);
  return true;
}

int64_t PythonBridge::StopRecording() {
  std::lock_guard<std::mutex> lk(tape_.mu);
  if (!tape_.writer.IsOpen()) return -1;
  tape_.writer.Close();
  const int64_t frames = static_cast<int64_t>(tape_.writer.frames());
  if (frames == 0) {
    // nothing arrived (=QUOTE() cells that never updated): no empty file that a replay would refuse later
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(FromUtf8(tape_.rec_path)), ec);
    Log("[replay] " + std::string(TR("Nothing was recorded: no live data arrived")), 2);
    return 0;
  }
  Log("[replay] " + std::string(TR("Recording saved: ")) + tape_.rec_path + " (" + std::to_string(frames) + " " + TR("frames") + ")", 2);
  return frames;
}

bool PythonBridge::TapeFrame(const engine::ColumnFrame& f, const uint8_t* raw, size_t len) {
  if (!tape_.writer.IsOpen() && !tape_.replaying.load(std::memory_order_acquire)) return false;  // (a racy peek; checked again below)
  std::lock_guard<std::mutex> lk(tape_.mu);
  if (tape_.replaying.load() && Listed(tape_.tables, f.table)) {
    tape_.dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (tape_.writer.IsOpen() && Listed(tape_.rec_tables, f.table)) tape_.writer.Write(engine::PerfCounterNs() - tape_.rec_start_ns, raw, len);
  return false;
}

void PythonBridge::TapeTopics(std::vector<std::pair<std::string, core::Value>>* items) {
  if (!tape_.writer.IsOpen() && !tape_.replaying.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(tape_.mu);
  if (tape_.writer.IsOpen() && tape_.rec_topics && !items->empty()) {
    const std::vector<uint8_t> bytes = engine::EncodeTopicBatch(*items);
    tape_.writer.Write(engine::PerfCounterNs() - tape_.rec_start_ns, bytes.data(), bytes.size(), engine::kTapeTopics);
  }
  if (tape_.replaying.load() && !tape_.topics.empty()) {
    const size_t before = items->size();
    std::erase_if(*items, [&](const auto& it) { return tape_.topics.count(it.first) > 0; });
    if (items->size() != before) tape_.dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

bool PythonBridge::StartReplay(const std::wstring& path, double speed, double from_s, bool loop, std::string* error) {
  EndReplay(true);
  std::vector<std::string> tables;
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    if (!tape_.reader.Open(path, error)) return false;
    if (tape_.reader.index().t.empty()) {
      tape_.reader.Close();
      if (error) *error = TR("The recording has no data");
      return false;
    }
    for (const auto& t : tape_.reader.index().tables) tables.push_back(engine::Lower(t));
    tape_.topics = std::unordered_set<std::string>(tape_.reader.index().topics.begin(), tape_.reader.index().topics.end());
    // the figures on the tape: their live updates are dropped while it plays, and they come back afterwards
    tape_.plot_figs.clear();
    const auto& ix = tape_.reader.index();
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < ix.t.size(); ++i) {
      if (ix.table[i] != engine::TapeIndex::kPlotRecord || !tape_.reader.Frame(i, &buf)) continue;
      std::string m;
      io::MV p;
      std::shared_ptr<engine::RawFrame> data;
      if (DecodePlot(buf, &m, &p, &data) && p.GetInt("fig", 0)) tape_.plot_figs.insert(p.GetInt("fig", 0));
    }
  }
  if (!tape_.plot_figs.empty()) {
    // back up the figures the replay takes over; queued before any of the replay's figure requests (the UI queue keeps
    // the order), and without waiting: a replay can be started from the UI thread
    const std::unordered_set<int64_t> figs = tape_.plot_figs;
    std::lock_guard<std::mutex> ul(ui_mu_);
    ui_queue_.push_back([figs](AppState&) { FigureBackup(figs, false); });
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
  }
  // the tables as they are now, restored when the replay ends
  std::vector<std::shared_ptr<engine::ColumnFrame>> live;
  std::vector<std::string> missing;
  s_.engine->Call([&](engine::Document& d) {
    for (const auto& t : tables) {
      if (!d.FindBoundTable(t)) {
        missing.push_back(t);
        continue;
      }
      auto f = std::make_shared<engine::ColumnFrame>();
      if (d.TableFrame(t, true, f.get(), nullptr)) live.push_back(std::move(f));
    }
    return true;
  });
  if (!missing.empty()) {
    std::lock_guard<std::mutex> lk(tape_.mu);
    tape_.reader.Close();
    if (error) *error = TR("The recording is for bound tables this workbook doesn't have: ") + missing.front();
    return false;
  }
  std::lock_guard<std::mutex> lk(tape_.mu);
  tape_.path = ToUtf8(path);
  tape_.tables = std::move(tables);
  tape_.live_state = std::move(live);
  tape_.quit = tape_.paused = tape_.ended = false;
  tape_.loop = loop;
  tape_.speed = std::max(0.0, speed);
  tape_.next = 0;
  tape_.pos_ns = 0;
  tape_.seek_ns = from_s > 0 ? static_cast<int64_t>(from_s * 1e9) : 0;
  tape_.dropped = 0;
  tape_.replaying.store(true, std::memory_order_release);
  tape_.th = std::thread([this] { ReplayLoop(); });
  Log("[replay] " + std::string(TR("Replaying ")) + tape_.path, 2);
  return true;
}

void PythonBridge::EndReplay(bool restore) {
  std::thread th;
  std::vector<std::shared_ptr<engine::ColumnFrame>> live;
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    if (!tape_.th.joinable()) return;
    tape_.quit = true;
    ++tape_.control;
    th = std::move(tape_.th);
  }
  tape_.cv.notify_all();
  {
    // the replay thread may wait for the engine (backpressure): wake it
    std::lock_guard<std::mutex> lk(bind_inbox_.mu);
  }
  bind_inbox_.cv.notify_all();
  th.join();
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    tape_.replaying.store(false, std::memory_order_release);
    tape_.reader.Close();
    live = std::move(tape_.live_state);
    tape_.tables.clear();
    tape_.topics.clear();
  }
  std::unordered_set<int64_t> figs;
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    figs.swap(tape_.plot_figs);
  }
  if (restore && !figs.empty()) {
    std::lock_guard<std::mutex> lk(ui_mu_);
    ui_queue_.push_back([figs](AppState&) { FigureBackup(figs, true); });
    ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
  }
  if (restore) {
    for (auto& f : live) EnqueueFrame(std::move(f), false);
    Log("[replay] " + std::string(TR("Back to live data")), 2);
  }
}

void PythonBridge::PauseReplay(bool pause) {
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    if (!tape_.replaying) return;
    if (!pause && tape_.ended) tape_.seek_ns = 0;  // play again from the start
    tape_.paused = pause;
    ++tape_.control;
  }
  tape_.cv.notify_all();
}

void PythonBridge::SeekReplay(double seconds) {
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    if (!tape_.replaying) return;
    tape_.seek_ns = std::max<int64_t>(0, static_cast<int64_t>(seconds * 1e9));
    ++tape_.control;
  }
  tape_.cv.notify_all();
}

void PythonBridge::SetReplaySpeed(double speed) {
  {
    std::lock_guard<std::mutex> lk(tape_.mu);
    tape_.speed = std::max(0.0, speed);
    ++tape_.control;
  }
  tape_.cv.notify_all();
}

void PythonBridge::SetReplayLoop(bool loop) {
  std::lock_guard<std::mutex> lk(tape_.mu);
  tape_.loop = loop;
}

PythonBridge::TapeStatus PythonBridge::GetTapeStatus() {
  TapeStatus st;
  std::lock_guard<std::mutex> lk(tape_.mu);
  st.recording = tape_.writer.IsOpen();
  st.rec_path = tape_.rec_path;
  if (st.recording) st.rec_seconds = static_cast<double>(engine::PerfCounterNs() - tape_.rec_start_ns) / 1e9;
  st.rec_frames = tape_.writer.frames();
  st.rec_bytes = tape_.writer.bytes();
  if (tape_.replaying) {
    const auto& ix = tape_.reader.index();
    st.mode = tape_.ended ? 3 : tape_.paused ? 2 : 1;
    st.path = tape_.path;
    st.pos_s = static_cast<double>(tape_.pos_ns) / 1e9;
    st.length_s = static_cast<double>(ix.length_ns()) / 1e9;
    st.speed = tape_.speed;
    st.loop = tape_.loop;
    st.start_unix_ms = ix.start_unix_ms;
    st.frame = tape_.next;
    st.frames = ix.t.size();
    st.dropped = tape_.dropped.load();
    st.tables = ix.tables;
    for (const auto& m : ix.marks) st.marks.push_back({static_cast<double>(m.t) / 1e9, m.kind, m.text});
  }
  st.pinned = tape_.pinned;
  st.pin_s = tape_.pin_s;
  return st;
}

// the replay thread: frames at their recorded pace (divided by the speed); seeks replay from the start at full speed
void PythonBridge::ReplayLoop() {
  using Clock = std::chrono::steady_clock;
  std::vector<uint8_t> buf;
  Clock::time_point base_wall = Clock::now();
  int64_t base_t = 0;
  int64_t target = -1;  // fast-forward up to this time (a seek)
  std::unique_lock<std::mutex> lk(tape_.mu);
  const auto& ix = tape_.reader.index();
  auto rebase = [&] {
    base_wall = Clock::now();
    base_t = tape_.pos_ns;
  };
  uint64_t seen = tape_.control;
  while (!tape_.quit) {
    if (tape_.seek_ns >= 0) {
      target = std::min(tape_.seek_ns, ix.length_ns());
      tape_.seek_ns = -1;
      tape_.ended = false;
      if (target < tape_.pos_ns || tape_.next == 0) {
        tape_.next = 0;  // back: from the snapshot at the start
        tape_.pos_ns = 0;
      }
    }
    if (tape_.control != seen) {
      seen = tape_.control;
      rebase();
    }
    if (tape_.next >= ix.t.size()) {
      if (tape_.loop && !ix.t.empty()) {
        tape_.seek_ns = 0;
        continue;
      }
      tape_.ended = true;
      target = -1;
      tape_.cv.wait(lk, [&] { return tape_.quit || tape_.seek_ns >= 0 || tape_.control != seen; });
      continue;
    }
    const bool fast = target >= 0 && ix.t[tape_.next] <= target;
    if (!fast) {
      if (target >= 0) {
        // the seek is done: continue from its time
        tape_.pos_ns = target;
        target = -1;
        rebase();
      }
      if (tape_.paused) {
        tape_.cv.wait(lk, [&] { return tape_.quit || !tape_.paused || tape_.seek_ns >= 0; });
        seen = ~tape_.control;  // rebase after a pause
        continue;
      }
      if (tape_.speed > 0) {
        const auto due = base_wall + std::chrono::nanoseconds(static_cast<int64_t>(static_cast<double>(ix.t[tape_.next] - base_t) / tape_.speed));
        if (Clock::now() < due) {
          // wait for the frame's time; the replay position moves on meanwhile (status)
          const auto step = std::min(due, Clock::now() + std::chrono::milliseconds(50));
          tape_.cv.wait_until(lk, step, [&] { return tape_.quit || tape_.control != seen || tape_.seek_ns >= 0; });
          if (tape_.control == seen && tape_.seek_ns < 0 && !tape_.quit) {
            const int64_t now_t = base_t + static_cast<int64_t>(static_cast<double>((Clock::now() - base_wall).count()) * tape_.speed);
            tape_.pos_ns = std::clamp(now_t, tape_.pos_ns, ix.t[tape_.next]);
          }
          continue;
        }
      }
    }
    const size_t i = tape_.next++;
    tape_.pos_ns = std::max(tape_.pos_ns, ix.t[i]);
    if (!tape_.reader.Frame(i, &buf)) continue;
    if (ix.table[i] == engine::TapeIndex::kPlotRecord) {
      // a figure request: applied on the UI thread like Python's
      std::string m;
      io::MV p;
      std::shared_ptr<engine::RawFrame> data;
      if (DecodePlot(buf, &m, &p, &data)) {
        std::lock_guard<std::mutex> ul(ui_mu_);
        ui_queue_.push_back([m, p, data](AppState& s) {
          std::string e;
          ReplayFigureRequest(s, m, p, data, &e);
        });
        ::PostMessageW(s_.hwnd, WM_APP + 1, 0, 0);
      }
      continue;
    }
    if (ix.table[i] == engine::TapeIndex::kTopicRecord) {
      // topic updates: the same path as Topics.Push
      std::vector<std::pair<std::string, core::Value>> items;
      lk.unlock();
      if (engine::DecodeTopicBatch(buf.data(), buf.size(), &items)) {
        FeedTopicSubs(items, nullptr);  // Topics.Subscribe sees the recorded updates
        s_.engine->Post([items = std::move(items)](engine::Document& d) {
          for (const auto& [t, v] : items) d.UpdateTopic(t, v);
        });
      }
      lk.lock();
      continue;
    }
    lk.unlock();
    // the frame arrives now: its send time is now (latency and freshness of the bound tables)
    if (buf.size() >= sizeof(engine::FrameHeader)) {
      const int64_t now = engine::PerfCounterNs();
      std::memcpy(buf.data() + offsetof(engine::FrameHeader, sent_ns), &now, sizeof(now));
    }
    auto f = std::make_shared<engine::ColumnFrame>();
    std::string why;
    if (engine::DecodeColumnFrame(buf.data(), buf.size(), f.get(), &why)) {
      EnqueueFrame(std::move(f), true);
    } else {
      Log("[replay] " + why, 1);
    }
    lk.lock();
  }
}

}  // namespace yc::app
