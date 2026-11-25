// SPDX-License-Identifier: Apache-2.0
// net.cpp - downloads for WEBSERVICE and IMAGE (WinHTTP, in background threads; at most 4 at a time)
#include "net.h"

#include "app_state.h"
#include "engine/engine.h"

#include <windows.h>
#include <winhttp.h>

#include <condition_variable>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

namespace yc::app {
namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// at most 4 downloads at a time
std::mutex g_slots_mu;
std::condition_variable g_slots_cv;
int g_slots = 4;
struct Slot {
  Slot() {
    std::unique_lock<std::mutex> lk(g_slots_mu);
    g_slots_cv.wait(lk, [] { return g_slots > 0; });
    --g_slots;
  }
  ~Slot() {
    {
      std::lock_guard<std::mutex> lk(g_slots_mu);
      ++g_slots;
    }
    g_slots_cv.notify_one();
  }
};

}  // namespace

bool HttpGet(const std::string& url, std::string* body, std::string* error, size_t max_bytes) {
  body->clear();
  // file: URLs (IMAGE of a local picture)
  if (url.rfind("file:", 0) == 0) {
    std::string path = url.substr(5);
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    std::ifstream f(Widen(path), std::ios::binary);
    if (!f) {
      *error = "The file can't be read";
      return false;
    }
    body->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
  }
  const std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {}, path[4096] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    *error = "Invalid URL";
    return false;
  }
  HINTERNET ses = ::WinHttpOpen(L"YCGrid", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    *error = "Network unavailable";
    return false;
  }
  ::WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
  bool ok = false;
  HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
  HINTERNET req = con ? ::WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
  if (req && ::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && ::WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status >= 200 && status < 300) {
      ok = true;
      for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        body->append(chunk.data(), read);
        if (body->size() > max_bytes) {
          ok = false;
          *error = "The response is too large";
          break;
        }
      }
    } else {
      *error = "HTTP " + std::to_string(status);
    }
  } else {
    *error = "The request failed";
  }
  if (req) ::WinHttpCloseHandle(req);
  if (con) ::WinHttpCloseHandle(con);
  ::WinHttpCloseHandle(ses);
  return ok;
}

// ---- WEBSERVICE ----

NetFetcher::NetFetcher(engine::Engine* eng) : state_(std::make_shared<State>()) { state_->eng = eng; }
NetFetcher::~NetFetcher() { Shutdown(); }

void NetFetcher::Shutdown() {
  std::lock_guard<std::mutex> lk(state_->mu);
  state_->eng = nullptr;
}

void NetFetcher::Fetch(const std::string& key, const std::string& url) {
  std::shared_ptr<State> st = state_;
  std::thread([st, key, url] {
    Slot slot;
    std::string body, err;
    core::Value v;
    if (HttpGet(url, &body, &err, 4 << 20)) v = core::Value::String(body);
    else v = core::Value::Error(core::Err::Value);
    std::lock_guard<std::mutex> lk(st->mu);
    if (st->eng) st->eng->Post([key, v](engine::Document& d) { d.SetUdfResult(key, v); });
  }).detach();
}

// ---- IMAGE ----

struct ImageCache::Entry {
  bool done = false, failed = false;
  std::string bytes;
  ImTextureID tex{};
  int w = 0, h = 0;
};

ImageCache::~ImageCache() {
  for (auto& [url, e] : items_) {
    if (e->tex) ReleaseTexture(e->tex);
  }
}

ImTextureID ImageCache::Get(const std::string& url, ImVec2* size) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto& slot = items_[url];
    if (!slot) {
      slot = std::make_shared<Entry>();
      std::shared_ptr<Entry> job = slot;
      HWND hwnd = hwnd_;
      std::mutex* mu = &mu_;
      std::thread([job, url, hwnd, mu] {
        Slot s;
        std::string body, err;
        const bool ok = HttpGet(url, &body, &err, 32 << 20);
        {
          std::lock_guard<std::mutex> lk(*mu);
          job->bytes = std::move(body);
          job->failed = !ok;
          job->done = true;
        }
        if (hwnd) ::PostMessageW(hwnd, WM_NULL, 0, 0);  // wake the UI to draw it
      }).detach();
    }
    e = slot;
    if (!e->done || e->failed) return ImTextureID{};
  }
  if (!e->tex && !e->bytes.empty()) {
    e->tex = TextureFromImage(e->bytes, &e->w, &e->h);
    e->bytes.clear();
    if (!e->tex) e->failed = true;
  }
  if (size) *size = ImVec2(static_cast<float>(e->w), static_cast<float>(e->h));
  return e->tex;
}

}  // namespace yc::app
