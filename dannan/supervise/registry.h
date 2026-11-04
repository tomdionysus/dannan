//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <memory>
#include <string>
#include <typeindex>

#include "dannan/contract/thread_safety.h"

namespace dannan::supervise {

// Where subsystems publish what they offer, by name, and others look it up;
// "not loaded" is a null lookup. Lookups return shared_ptr: a faulted
// subsystem is rebuilt in place, and a caller's copy keeps the old instance
// alive until it is done. Thread-safe.
class Registry {
 public:
  template <class T>
  void publish(const std::string& name, std::shared_ptr<T> offered) {
    contract::WriteLock lock(_mutex);
    _entries.insert_or_assign(name, Entry{std::type_index(typeid(T)), std::static_pointer_cast<void>(std::move(offered))});
  }

  // Withdraws only if `instance` is still the current one: a faulted
  // subsystem's stop() can run after its replacement has published.
  template <class T>
  void withdraw(const std::string& name, const T* instance) {
    contract::WriteLock lock(_mutex);
    const auto found = _entries.find(name);
    if (found != _entries.end() && found->second.pointer.get() == static_cast<const void*>(instance)) _entries.erase(found);
  }

  // Null if nothing of type T is published under the name.
  template <class T>
  std::shared_ptr<T> find(const std::string& name) const {
    contract::ReadLock lock(_mutex);
    const auto found = _entries.find(name);
    if (found == _entries.end() || found->second.type != std::type_index(typeid(T))) return nullptr;
    return std::static_pointer_cast<T>(found->second.pointer);
  }

 private:
  struct Entry {
    std::type_index type;
    std::shared_ptr<void> pointer;
  };
  mutable contract::SharedMutex _mutex;
  std::map<std::string, Entry> _entries DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::supervise
