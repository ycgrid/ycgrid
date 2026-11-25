// SPDX-License-Identifier: Apache-2.0
// net.h - downloads for WEBSERVICE (engine NetProvider) and IMAGE (picture cache for the grid)
#pragma once

#include "engine/document.h"

#include <imgui.h>
#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace yc::engine {
class Engine;
}

namespace yc::app {

// GET a URL (http, https or file:); false with *error on failure
bool HttpGet(const std::string& url, std::string* body, std::string* error, size_t max_bytes);

// WEBSERVICE: downloads in the background and hands the text to the engine
class NetFetcher : public engine::NetProvider {
 public:
  explicit NetFetcher(engine::Engine* eng);
  ~NetFetcher() override;
  void Fetch(const std::string& key, const std::string& url) override;
  void Shutdown();  // before the engine goes away: downloads still running drop their results

 private:
  struct State {
    std::mutex mu;
    engine::Engine* eng = nullptr;
  };
  std::shared_ptr<State> state_;
};

// IMAGE: pictures by URL, downloaded once per session; textures are created on the UI thread
class ImageCache {
 public:
  explicit ImageCache(HWND hwnd) : hwnd_(hwnd) {}
  ~ImageCache();
  ImTextureID Get(const std::string& url, ImVec2* size);  // 0 while downloading or when it failed

 private:
  struct Entry;
  HWND hwnd_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<Entry>> items_;
};

}  // namespace yc::app
