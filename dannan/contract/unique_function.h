//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace dannan::contract {

// A callable that may own what cannot be copied (a Secret, a promise): what
// std::move_only_function is in C++23, for a C++20 tree. Moved, never copied;
// empty when default-made or moved from. Called at most by one thread at a time.
template <typename Signature>
class UniqueFunction;

template <typename R, typename... Args>
class UniqueFunction<R(Args...)> {
 public:
  UniqueFunction() noexcept = default;
  UniqueFunction(std::nullptr_t) noexcept {}
  template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, UniqueFunction> && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
  UniqueFunction(F&& f) : _call(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}
  UniqueFunction(UniqueFunction&&) noexcept = default;
  UniqueFunction& operator=(UniqueFunction&&) noexcept = default;
  UniqueFunction(const UniqueFunction&) = delete;
  UniqueFunction& operator=(const UniqueFunction&) = delete;

  explicit operator bool() const noexcept { return _call != nullptr; }
  // Throws std::bad_function_call when empty.
  R operator()(Args... args) {
    if (!_call) throw std::bad_function_call();
    return _call->call(std::forward<Args>(args)...);
  }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual R call(Args... args) = 0;
  };
  template <typename F>
  struct Model final : Concept {
    template <typename G>
    explicit Model(G&& given) : f(std::forward<G>(given)) {}
    R call(Args... args) override { return std::invoke(f, std::forward<Args>(args)...); }
    F f;
  };
  std::unique_ptr<Concept> _call;
};

}  // namespace dannan::contract
