//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/secure_memory.h"

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <mach/vm_inherit.h>
#endif

#include "dannan/identity/crypto.h"

namespace dannan::identity {
namespace {

size_t page_size() noexcept {
  const auto page = ::sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<size_t>(page) : 4096;
}

}  // namespace

SecureBytes::SecureBytes(size_t size) : _size(size) {
  if (!size) return;
  const auto page = page_size();
  _mapped = (size + page - 1) / page * page;
  void* pages = ::mmap(nullptr, _mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (pages == MAP_FAILED) throw std::runtime_error(std::string("cannot map secure memory: ") + std::strerror(errno));
  _data = static_cast<uint8_t*>(pages);
  _locked = ::mlock(pages, _mapped) == 0;
#if defined(__linux__)
  (void)::madvise(pages, _mapped, MADV_DONTDUMP);
  (void)::madvise(pages, _mapped, MADV_DONTFORK);
#elif defined(__APPLE__)
  (void)::minherit(pages, _mapped, VM_INHERIT_NONE);
#endif
}

SecureBytes::~SecureBytes() { release(); }

SecureBytes::SecureBytes(SecureBytes&& other) noexcept
    : _data(std::exchange(other._data, nullptr)),
      _size(std::exchange(other._size, 0)),
      _mapped(std::exchange(other._mapped, 0)),
      _locked(std::exchange(other._locked, false)) {}

SecureBytes& SecureBytes::operator=(SecureBytes&& other) noexcept {
  if (this != &other) {
    release();
    _data = std::exchange(other._data, nullptr);
    _size = std::exchange(other._size, 0);
    _mapped = std::exchange(other._mapped, 0);
    _locked = std::exchange(other._locked, false);
  }
  return *this;
}

void SecureBytes::release() noexcept {
  if (!_data) return;
  cleanse({_data, _mapped});
  if (_locked) (void)::munlock(_data, _mapped);
  (void)::munmap(_data, _mapped);
  _data = nullptr;
  _size = 0;
  _mapped = 0;
  _locked = false;
}

Secret::Secret(std::span<const uint8_t> bytes) : _bytes(bytes.size()) { std::copy(bytes.begin(), bytes.end(), _bytes.data()); }

Secret Secret::random(size_t size) {
  SecureBytes bytes(size);
  auto fresh = random_bytes(size);
  std::copy(fresh.begin(), fresh.end(), bytes.data());
  cleanse(fresh);
  return Secret(std::move(bytes));
}

bool operator==(const Secret& a, const Secret& b) noexcept {
  if (a.size() != b.size()) return false;
  return constant_time_equal(a.reveal(), b.reveal());
}

Hardening harden_process() noexcept {
  Hardening out;
  const rlimit none{0, 0};
  out.core_dumps_disabled = ::setrlimit(RLIMIT_CORE, &none) == 0;
  if (!out.core_dumps_disabled) out.detail = std::string("RLIMIT_CORE: ") + std::strerror(errno);
#if defined(__linux__)
  out.attach_refused = ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0;
  if (!out.attach_refused) out.detail += std::string(out.detail.empty() ? "" : "; ") + "PR_SET_DUMPABLE: " + std::strerror(errno);
#elif defined(__APPLE__)
  // PT_DENY_ATTACH would also stop the test runner's own debugger; a node
  // that wants it asks for it in its own main.
  out.attach_refused = false;
#endif
  return out;
}

}  // namespace dannan::identity
