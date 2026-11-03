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

size_t mapped_size(size_t bytes) noexcept {
  const auto page = page_size();
  return (bytes + page - 1) / page * page;
}

// Pages of their own, kept out of dumps and forks; whether they could be
// locked.
std::pair<void*, bool> map_secure(size_t mapped) {
  void* pages = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (pages == MAP_FAILED) throw std::runtime_error(std::string("cannot map secure memory: ") + std::strerror(errno));
  const bool locked = ::mlock(pages, mapped) == 0;
#if defined(__linux__)
  (void)::madvise(pages, mapped, MADV_DONTDUMP);
  (void)::madvise(pages, mapped, MADV_DONTFORK);
#elif defined(__APPLE__)
  (void)::minherit(pages, mapped, VM_INHERIT_NONE);
#endif
  return {pages, locked};
}

void unmap_secure(void* pages, size_t mapped, bool locked) noexcept {
  cleanse({static_cast<uint8_t*>(pages), mapped});
  if (locked) (void)::munlock(pages, mapped);
  (void)::munmap(pages, mapped);
}

}  // namespace

void* secure_allocate(size_t bytes) {
  if (!bytes) bytes = 1;
  // Whether it locked is not kept: munlock of an unlocked page is harmless.
  return map_secure(mapped_size(bytes)).first;
}

void secure_deallocate(void* pointer, size_t bytes) noexcept {
  if (!pointer) return;
  unmap_secure(pointer, mapped_size(bytes ? bytes : 1), true);
}

SecureBytes::SecureBytes(size_t size) : _size(size) {
  if (!size) return;
  _mapped = mapped_size(size);
  const auto [pages, locked] = map_secure(_mapped);
  _data = static_cast<uint8_t*>(pages);
  _locked = locked;
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
  unmap_secure(_data, _mapped, _locked);
  _data = nullptr;
  _size = 0;
  _mapped = 0;
  _locked = false;
}

Secret::Secret(std::span<const uint8_t> bytes) : _bytes(bytes.size()) { std::copy(bytes.begin(), bytes.end(), _bytes.data()); }

Secret::Secret(SecureBuffer&& buffer) : _bytes(buffer.size()) {
  std::copy(buffer.begin(), buffer.end(), _bytes.data());
  cleanse(buffer);
  buffer.clear();
}

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
