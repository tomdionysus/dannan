//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <mutex>
#include <shared_mutex>

// Clang's thread-safety analysis, checked with -Wthread-safety and ignored by
// other compilers. Every lock in Dannan is one of the types below, so every
// lock is checked.
//
// How a class uses them:
// - A mutex any holder keeps across device or network I/O, or across a call
//   that may do it (a callback, logging), is an IoMutex; otherwise a Mutex. A
//   comment above it says what I/O it is held across.
// - State a mutex protects is DANNAN_GUARDED_BY it; a function called with it
//   held is DANNAN_REQUIRES it (the *_locked convention).
// - Where two locks nest, the comment above the inner one states the order.
// - A condition variable waits on Lock::native(); its predicate is written
//   [&]() DANNAN_REQUIRES(_mutex) { ... }.
// - A mutex that guards nothing (a wait or ordering lock) says so.
#if defined(__clang__)
#define DANNAN_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define DANNAN_THREAD_ANNOTATION(x)
#endif

#define DANNAN_CAPABILITY(x) DANNAN_THREAD_ANNOTATION(capability(x))
#define DANNAN_SCOPED_CAPABILITY DANNAN_THREAD_ANNOTATION(scoped_lockable)
#define DANNAN_ACQUIRE(...) DANNAN_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define DANNAN_RELEASE(...) DANNAN_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define DANNAN_EXCLUDES(...) DANNAN_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define DANNAN_ACQUIRE_SHARED(...) DANNAN_THREAD_ANNOTATION(acquire_shared_capability(__VA_ARGS__))
#define DANNAN_RELEASE_SHARED(...) DANNAN_THREAD_ANNOTATION(release_shared_capability(__VA_ARGS__))
#define DANNAN_TRY_ACQUIRE(...) DANNAN_THREAD_ANNOTATION(try_acquire_capability(__VA_ARGS__))
#define DANNAN_REQUIRES(...) DANNAN_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define DANNAN_REQUIRES_SHARED(...) DANNAN_THREAD_ANNOTATION(requires_shared_capability(__VA_ARGS__))
#define DANNAN_GUARDED_BY(x) DANNAN_THREAD_ANNOTATION(guarded_by(x))
#define DANNAN_PT_GUARDED_BY(x) DANNAN_THREAD_ANNOTATION(pt_guarded_by(x))
#define DANNAN_ACQUIRED_BEFORE(...) DANNAN_THREAD_ANNOTATION(acquired_before(__VA_ARGS__))
#define DANNAN_ACQUIRED_AFTER(...) DANNAN_THREAD_ANNOTATION(acquired_after(__VA_ARGS__))
#define DANNAN_RETURN_CAPABILITY(x) DANNAN_THREAD_ANNOTATION(lock_returned(x))
#define DANNAN_NO_THREAD_SAFETY_ANALYSIS DANNAN_THREAD_ANNOTATION(no_thread_safety_analysis)

namespace dannan::contract {

// A role, not a lock: "this code waits on no I/O". An operation declared to
// wait on nothing holds a NoIoRegion for its body; whatever waits on I/O is
// DANNAN_EXCLUDES(no_io), so reaching it inside the region is a compile error.
class DANNAN_CAPABILITY("no-I/O region") NoIo {};
extern NoIo no_io;

class DANNAN_SCOPED_CAPABILITY NoIoRegion {
 public:
  NoIoRegion() DANNAN_ACQUIRE(no_io) {}
  ~NoIoRegion() DANNAN_RELEASE() {}
  NoIoRegion(const NoIoRegion&) = delete;
  NoIoRegion& operator=(const NoIoRegion&) = delete;
};

// A mutex never held across I/O: holders do memory work only.
class DANNAN_CAPABILITY("mutex") Mutex {
 public:
  void lock() DANNAN_ACQUIRE() { _mutex.lock(); }
  void unlock() DANNAN_RELEASE() { _mutex.unlock(); }
  bool try_lock() DANNAN_TRY_ACQUIRE(true) { return _mutex.try_lock(); }
  std::mutex& native() noexcept { return _mutex; }

 private:
  std::mutex _mutex;
};

// A mutex some holder keeps across device or network I/O, so taking it is
// waiting on that I/O: a no-I/O region may not.
class DANNAN_CAPABILITY("I/O mutex") IoMutex {
 public:
  void lock() DANNAN_ACQUIRE() DANNAN_EXCLUDES(no_io) { _mutex.lock(); }
  void unlock() DANNAN_RELEASE() { _mutex.unlock(); }
  // Never waits, so it is allowed anywhere.
  bool try_lock() DANNAN_TRY_ACQUIRE(true) { return _mutex.try_lock(); }
  std::mutex& native() noexcept { return _mutex; }

 private:
  std::mutex _mutex;
};

// A reader-writer mutex never held across I/O.
class DANNAN_CAPABILITY("shared mutex") SharedMutex {
 public:
  void lock() DANNAN_ACQUIRE() { _mutex.lock(); }
  void unlock() DANNAN_RELEASE() { _mutex.unlock(); }
  void lock_shared() DANNAN_ACQUIRE_SHARED() { _mutex.lock_shared(); }
  void unlock_shared() DANNAN_RELEASE_SHARED() { _mutex.unlock_shared(); }
  std::shared_mutex& native() noexcept { return _mutex; }

 private:
  std::shared_mutex _mutex;
};

// A reader-writer mutex some holder keeps across I/O.
class DANNAN_CAPABILITY("I/O shared mutex") IoSharedMutex {
 public:
  void lock() DANNAN_ACQUIRE() DANNAN_EXCLUDES(no_io) { _mutex.lock(); }
  void unlock() DANNAN_RELEASE() { _mutex.unlock(); }
  void lock_shared() DANNAN_ACQUIRE_SHARED() DANNAN_EXCLUDES(no_io) { _mutex.lock_shared(); }
  void unlock_shared() DANNAN_RELEASE_SHARED() { _mutex.unlock_shared(); }
  std::shared_mutex& native() noexcept { return _mutex; }

 private:
  std::shared_mutex _mutex;
};

// Holds a Mutex or IoMutex for its scope; may be released and retaken, and
// lends its std::unique_lock to a condition variable's wait.
class DANNAN_SCOPED_CAPABILITY Lock {
 public:
  explicit Lock(Mutex& mutex) DANNAN_ACQUIRE(mutex) : _lock(mutex.native()) {}
  explicit Lock(IoMutex& mutex) DANNAN_ACQUIRE(mutex) : _lock(mutex.native()) {}
  // Adopts a mutex the caller already holds, as after a successful try_lock().
  Lock(Mutex& mutex, std::adopt_lock_t) DANNAN_REQUIRES(mutex) : _lock(mutex.native(), std::adopt_lock) {}
  Lock(IoMutex& mutex, std::adopt_lock_t) DANNAN_REQUIRES(mutex) : _lock(mutex.native(), std::adopt_lock) {}
  ~Lock() DANNAN_RELEASE() {}
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

  void lock() DANNAN_ACQUIRE() { _lock.lock(); }
  void unlock() DANNAN_RELEASE() { _lock.unlock(); }
  bool owns_lock() const noexcept { return _lock.owns_lock(); }
  std::unique_lock<std::mutex>& native() noexcept { return _lock; }

 private:
  std::unique_lock<std::mutex> _lock;
};

// Releases a held Lock for its scope and retakes it at the end: work that
// must not hold the mutex (device I/O, a callback) in the middle of a
// section that does. The analysis knows the mutex is not held inside, so
// touching what it guards there is a compile error, and held again after.
class DANNAN_SCOPED_CAPABILITY Unlocked {
 public:
  Unlocked(Lock& lock, Mutex& mutex) DANNAN_RELEASE(mutex) : _lock(lock) { _lock.unlock(); }
  Unlocked(Lock& lock, IoMutex& mutex) DANNAN_RELEASE(mutex) : _lock(lock) { _lock.unlock(); }
  ~Unlocked() DANNAN_RELEASE() { _lock.lock(); }
  Unlocked(const Unlocked&) = delete;
  Unlocked& operator=(const Unlocked&) = delete;

 private:
  Lock& _lock;
};

// Holds a SharedMutex or IoSharedMutex exclusively for its scope.
class DANNAN_SCOPED_CAPABILITY WriteLock {
 public:
  explicit WriteLock(SharedMutex& mutex) DANNAN_ACQUIRE(mutex) : _lock(mutex.native()) {}
  explicit WriteLock(IoSharedMutex& mutex) DANNAN_ACQUIRE(mutex) : _lock(mutex.native()) {}
  ~WriteLock() DANNAN_RELEASE() {}
  WriteLock(const WriteLock&) = delete;
  WriteLock& operator=(const WriteLock&) = delete;

 private:
  std::unique_lock<std::shared_mutex> _lock;
};

// Holds a SharedMutex or IoSharedMutex shared for its scope.
class DANNAN_SCOPED_CAPABILITY ReadLock {
 public:
  explicit ReadLock(SharedMutex& mutex) DANNAN_ACQUIRE_SHARED(mutex) : _lock(mutex.native()) {}
  explicit ReadLock(IoSharedMutex& mutex) DANNAN_ACQUIRE_SHARED(mutex) : _lock(mutex.native()) {}
  ~ReadLock() DANNAN_RELEASE() {}
  ReadLock(const ReadLock&) = delete;
  ReadLock& operator=(const ReadLock&) = delete;

 private:
  std::shared_lock<std::shared_mutex> _lock;
};

}  // namespace dannan::contract
