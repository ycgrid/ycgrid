// SPDX-License-Identifier: Apache-2.0
// cow.h - copy-on-write container (simplified PLAN §4.5, see docs/adr/0001-snapshot-shared-ptr.md)
//
// The engine (the only writer) holds the working version; publishing a snapshot only copies the shared_ptr. If a snapshot
// shares the data before a write (use_count > 1), copy first, then write. use_count can only be decreased by other threads
// (readers releasing snapshots), so modifying in place at use_count == 1 is safe; seeing a larger value costs at most one extra copy.
#pragma once

#include <memory>

namespace yc::core {

template <class T>
class Cow {
 public:
  Cow() : p_(std::make_shared<T>()) {}
  explicit Cow(std::shared_ptr<T> p) : p_(std::move(p)) {}

  const T& get() const { return *p_; }
  const T* operator->() const { return p_.get(); }
  const T& operator*() const { return *p_; }

  T& mut() {
    if (p_.use_count() > 1) p_ = std::make_shared<T>(*p_);
    return *p_;
  }
  void reset(T v) { p_ = std::make_shared<T>(std::move(v)); }
  std::shared_ptr<const T> share() const { return p_; }

 private:
  std::shared_ptr<T> p_;
};

}  // namespace yc::core
