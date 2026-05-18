/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HSHM_DATA_STRUCTURES_PRIV_UNORDERED_MAP_LL_H_
#define HSHM_DATA_STRUCTURES_PRIV_UNORDERED_MAP_LL_H_

// `unordered_map_ll` -- linked-list (chaining) unordered map.
//
// Bucket array of singly-linked lists. Each bucket owns its own RwLock
// when `EnableLocking` is true (the default), so concurrent inserts /
// erases / lookups against *different* buckets never contend, and
// concurrent lookups against the *same* bucket share a reader. Set
// `EnableLocking = false` for a single-threaded variant with zero lock
// overhead.
//
// THREAD-SAFETY CONTRACT (important):
//   * Single-key operations (insert, insert_or_assign, operator[],
//     find, contains, count, erase) ARE self-locking and safe to call
//     concurrently from many threads when EnableLocking is true.
//   * Rehash is NOT thread-safe and is intentionally not made so. A
//     rehash reallocates BOTH the bucket array and the per-bucket lock
//     array; the lock array cannot protect its own reallocation, so a
//     rehash that runs concurrently with ANY other map operation is a
//     data race / use-after-free. Rehash happens (a) automatically
//     inside maybe_rehash() when size exceeds bucket_count() after an
//     insert, and (b) explicitly via rehash().
//   * Therefore, for concurrent use the caller MUST guarantee the map
//     never rehashes while other threads touch it. The simple, intended
//     way is to construct the map with bucket_count() > the maximum
//     number of distinct keys ever inserted, so the load factor stays
//     <= 1 and maybe_rehash() always early-returns. Otherwise the
//     caller must quiesce all other threads around any rehash.
//
// Counterpart to `unordered_map_lhash` (linear-probing open-addressing,
// previously named `unordered_map_ll`). Chaining trades the predictable
// cache behavior that made linear probing nice on GPU for an unbounded
// load factor and finer-grained locking that scales better on the CPU
// under high contention.

#include "hermes_shm/data_structures/priv/vector.h"
#include "hermes_shm/types/hash.h"
#include "hermes_shm/memory/allocator/malloc_allocator.h"
#include "hermes_shm/thread/lock/rwlock.h"

namespace hshm::priv {

#ifndef HSHM_PRIV_INSERT_RESULT_DEFINED_
#define HSHM_PRIV_INSERT_RESULT_DEFINED_
/** Result of insert / insert_or_assign operations. Guard-shared with
 *  `unordered_map_lhash.h` so callers can include either header (or
 *  both, in any order) without a redefinition. */
template <typename T>
struct InsertResult {
  bool inserted;
  T *value;
};
#endif  // HSHM_PRIV_INSERT_RESULT_DEFINED_

/**
 * Chaining unordered map with optional per-bucket fine-grained RwLock.
 *
 * @tparam Key            Key type (copy/move + operator==)
 * @tparam T              Mapped value type
 * @tparam AllocT         Allocator (defaults to MallocAllocator)
 * @tparam Hash           Hash functor (defaults to hshm::hash<Key>)
 * @tparam KeyEqual       Equality functor (defaults to hshm::equal_to<Key>)
 * @tparam EnableLocking  Compile-time toggle for per-bucket RwLock
 *                        (defaults to true). When false the lock array is
 *                        elided and every operation is unsynchronized --
 *                        only use that mode when the map is owned by one
 *                        thread, or when the caller guarantees external
 *                        synchronization.
 */
template <typename Key, typename T,
          typename AllocT = hshm::ipc::MallocAllocator,
          typename Hash = hshm::hash<Key>,
          typename KeyEqual = hshm::equal_to<Key>,
          bool EnableLocking = true>
class unordered_map_ll {
 public:
  using key_type = Key;
  using mapped_type = T;
  using size_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  static constexpr bool kEnableLocking = EnableLocking;

 private:
  struct Node {
    Key key_;
    T value_;
    Node *next_;
    HSHM_CROSS_FUN Node() : key_(), value_(), next_(nullptr) {}
    HSHM_CROSS_FUN Node(const Key &k, const T &v, Node *n)
        : key_(k), value_(v), next_(n) {}
  };

  vector<Node *, AllocT> buckets_;  // bucket heads (nullptr = empty)
  vector<hshm::RwLock, AllocT> locks_;  // per-bucket; size 0 when !EnableLocking
  hshm::ipc::atomic<size_type> size_;
  AllocT *alloc_;
  Hash hash_fn_;
  KeyEqual key_eq_;

  /** Map a key to a bucket index. */
  HSHM_INLINE_CROSS_FUN
  size_type bucket_of(const Key &key) const {
    return hash_fn_(key) % buckets_.size();
  }

  /** Read-lock a bucket (no-op when locking is disabled). */
  HSHM_INLINE_CROSS_FUN
  void read_lock_bucket(size_type b) {
    if constexpr (EnableLocking) locks_[b].ReadLock(0);
  }
  HSHM_INLINE_CROSS_FUN
  void read_unlock_bucket(size_type b) {
    if constexpr (EnableLocking) locks_[b].ReadUnlock();
  }
  /** Write-lock a bucket (no-op when locking is disabled). */
  HSHM_INLINE_CROSS_FUN
  void write_lock_bucket(size_type b) {
    if constexpr (EnableLocking) locks_[b].WriteLock(0);
  }
  HSHM_INLINE_CROSS_FUN
  void write_unlock_bucket(size_type b) {
    if constexpr (EnableLocking) locks_[b].WriteUnlock();
  }

  /** Acquire every bucket's write lock. Used for rehash / clear / for_each. */
  HSHM_INLINE_CROSS_FUN
  void write_lock_all() {
    if constexpr (EnableLocking) {
      for (size_type i = 0; i < buckets_.size(); ++i) locks_[i].WriteLock(0);
    }
  }
  HSHM_INLINE_CROSS_FUN
  void write_unlock_all() {
    if constexpr (EnableLocking) {
      for (size_type i = 0; i < buckets_.size(); ++i) locks_[i].WriteUnlock();
    }
  }

  /** Walk a bucket chain. Caller holds whatever lock is appropriate. */
  HSHM_INLINE_CROSS_FUN
  Node *find_in_bucket(size_type b, const Key &key) const {
    Node *cur = buckets_[b];
    while (cur != nullptr) {
      if (key_eq_(cur->key_, key)) return cur;
      cur = cur->next_;
    }
    return nullptr;
  }

  /** Allocate a node via the allocator. */
  HSHM_INLINE_CROSS_FUN
  Node *new_node(const Key &k, const T &v, Node *next) {
    auto fp = alloc_->template NewObj<Node>(k, v, next);
    return fp.ptr_;
  }

  /** Free a node via the allocator. */
  HSHM_INLINE_CROSS_FUN
  void del_node(Node *n) {
    hipc::FullPtr<Node> fp(alloc_, n);
    alloc_->template DelObj<Node>(fp);
  }

  /** Set up an empty bucket array. */
  HSHM_CROSS_FUN
  void init_buckets(size_type num_buckets) {
    if (num_buckets == 0) num_buckets = 1;
    buckets_.resize(num_buckets);
    for (size_type i = 0; i < num_buckets; ++i) buckets_[i] = nullptr;
    if constexpr (EnableLocking) {
      locks_.resize(num_buckets);
      for (size_type i = 0; i < num_buckets; ++i) locks_[i].Init();
    }
  }

  /** Rehash without taking locks (caller holds every bucket lock). */
  HSHM_CROSS_FUN
  void rehash_no_lock(size_type new_bucket_count) {
    if (new_bucket_count == 0) new_bucket_count = 1;
    // Snapshot the current chains and the old bucket head array. We
    // re-thread the existing Node* values into the new buckets so we
    // don't have to re-alloc on rehash.
    size_type old_cap = buckets_.size();
    vector<Node *, AllocT> old_buckets(static_cast<vector<Node *, AllocT>&&>(buckets_));
    buckets_.resize(new_bucket_count);
    for (size_type i = 0; i < new_bucket_count; ++i) buckets_[i] = nullptr;
    if constexpr (EnableLocking) {
      // Re-init lock array to the new bucket count. Existing locks
      // are released; we'll re-acquire after the caller exits the
      // critical section.
      locks_.resize(new_bucket_count);
      for (size_type i = 0; i < new_bucket_count; ++i) locks_[i].Init();
    }
    for (size_type i = 0; i < old_cap; ++i) {
      Node *cur = old_buckets[i];
      while (cur != nullptr) {
        Node *next = cur->next_;
        size_type b = hash_fn_(cur->key_) % new_bucket_count;
        cur->next_ = buckets_[b];
        buckets_[b] = cur;
        cur = next;
      }
    }
  }

  /** Trigger rehash if load factor (size / num_buckets) > 1. Caller
   *  must NOT hold any per-bucket lock.
   *
   *  NOT thread-safe: see the THREAD-SAFETY CONTRACT at the top of this
   *  file. Although this takes write_lock_all(), the rehash reallocates
   *  the lock array itself, so it races with any concurrent operation.
   *  Concurrent users must size the map so this always early-returns
   *  (bucket_count() > max distinct keys) or otherwise quiesce all
   *  threads around growth. */
  HSHM_INLINE_CROSS_FUN
  void maybe_rehash() {
    size_type cur_size = size_.load();
    size_type cap = buckets_.size();
    if (cur_size <= cap) return;
    write_lock_all();
    cur_size = size_.load();
    cap = buckets_.size();
    if (cur_size > cap) {
      // rehash_no_lock() re-Init()s (and thereby releases) every bucket
      // lock while resizing locks_. Do NOT write_unlock_all() afterward:
      // unlocking freshly Init()'d locks underflows writers_ and bumps
      // cur_writer_, which wedges every subsequent WriteLock() forever.
      rehash_no_lock(cap * 2);
    } else {
      // No rehash happened, so release exactly the locks we acquired.
      write_unlock_all();
    }
  }

 public:
#if HSHM_IS_HOST
  /** Host-side constructor using the global MallocAllocator. */
  explicit unordered_map_ll(size_type num_buckets = 16)
      : buckets_(HSHM_MALLOC), locks_(HSHM_MALLOC), size_(0),
        alloc_(HSHM_MALLOC), hash_fn_(), key_eq_() {
    init_buckets(num_buckets);
  }
  /** Three-arg constructor kept for source compatibility with the
   *  prior open-addressing map -- num_locks is ignored (locking is now
   *  one-per-bucket and toggled by the EnableLocking template arg). */
  HSHM_CROSS_FUN
  unordered_map_ll(size_type num_buckets, size_type /*num_locks_ignored*/)
      : buckets_(HSHM_MALLOC), locks_(HSHM_MALLOC), size_(0),
        alloc_(HSHM_MALLOC), hash_fn_(), key_eq_() {
    init_buckets(num_buckets);
  }
#endif

  /** Constructor with an explicit allocator. */
  HSHM_CROSS_FUN
  explicit unordered_map_ll(AllocT *alloc, size_type num_buckets = 16)
      : buckets_(alloc), locks_(alloc), size_(0),
        alloc_(alloc), hash_fn_(), key_eq_() {
    init_buckets(num_buckets);
  }

  /** Source-compatible four-arg constructor; second size_type is ignored. */
  HSHM_CROSS_FUN
  unordered_map_ll(AllocT *alloc, size_type num_buckets,
                   size_type /*num_locks_ignored*/)
      : buckets_(alloc), locks_(alloc), size_(0),
        alloc_(alloc), hash_fn_(), key_eq_() {
    init_buckets(num_buckets);
  }

  HSHM_CROSS_FUN ~unordered_map_ll() {
    // Free every node. We don't bother locking -- destruction is
    // expected to be single-threaded.
    for (size_type i = 0; i < buckets_.size(); ++i) {
      Node *cur = buckets_[i];
      while (cur != nullptr) {
        Node *next = cur->next_;
        del_node(cur);
        cur = next;
      }
      buckets_[i] = nullptr;
    }
  }

  // -- size / capacity -------------------------------------------------

  HSHM_INLINE_CROSS_FUN size_type size() const { return size_.load(); }
  HSHM_INLINE_CROSS_FUN bool empty() const { return size() == 0; }
  HSHM_INLINE_CROSS_FUN size_type bucket_count() const {
    return buckets_.size();
  }

  /** Force a rehash to a new bucket count.
   *
   *  NOT thread-safe (by design): see the THREAD-SAFETY CONTRACT at the
   *  top of this file. The caller must ensure no other thread is
   *  touching the map for the duration of this call -- the bucket and
   *  lock arrays are both reallocated, so concurrent inserts/finds/
   *  erases would race and use freed memory. */
  HSHM_CROSS_FUN
  bool rehash(size_type new_bucket_count) {
    write_lock_all();
    // rehash_no_lock() resizes locks_ and re-Init()s every bucket lock,
    // which both releases the locks we just took and discards their held
    // state. Calling write_unlock_all() afterward would run WriteUnlock()
    // on freshly Init()'d (unlocked) locks -- underflowing writers_ and
    // advancing cur_writer_ -- permanently wedging all future WriteLock()
    // calls in an infinite spin. The rehash itself is the release.
    rehash_no_lock(new_bucket_count);
    return true;
  }

  // -- locked variants (caller holds the bucket write lock) -----------

  /** Write-lock the bucket that owns this key. */
  HSHM_CROSS_FUN
  void lock_key(const Key &key) {
    write_lock_bucket(bucket_of(key));
  }
  HSHM_CROSS_FUN
  void unlock_key(const Key &key) {
    write_unlock_bucket(bucket_of(key));
  }

  /** Find while holding the bucket's write lock. */
  HSHM_CROSS_FUN
  T *find_locked(const Key &key) {
    Node *n = find_in_bucket(bucket_of(key), key);
    return n != nullptr ? &n->value_ : nullptr;
  }

  /** Insert while holding the bucket's write lock; returns existing
   *  entry untouched if the key is already present. */
  HSHM_CROSS_FUN
  InsertResult<T> insert_locked(const Key &key, const T &value) {
    size_type b = bucket_of(key);
    Node *n = find_in_bucket(b, key);
    if (n != nullptr) return {false, &n->value_};
    Node *fresh = new_node(key, value, buckets_[b]);
    if (fresh == nullptr) return {false, nullptr};
    buckets_[b] = fresh;
    size_.fetch_add(1);
    return {true, &fresh->value_};
  }

  /** Erase while holding the bucket's write lock. */
  HSHM_CROSS_FUN
  size_type erase_locked(const Key &key) {
    size_type b = bucket_of(key);
    Node **prev = &buckets_[b];
    Node *cur = *prev;
    while (cur != nullptr) {
      if (key_eq_(cur->key_, key)) {
        *prev = cur->next_;
        del_node(cur);
        size_.fetch_sub(1);
        return 1;
      }
      prev = &cur->next_;
      cur = cur->next_;
    }
    return 0;
  }

  // -- self-locking primary API ---------------------------------------

  /** Insert if absent; thread-safe. */
  HSHM_CROSS_FUN
  InsertResult<T> insert(const Key &key, const T &value) {
    size_type b = bucket_of(key);
    write_lock_bucket(b);
    InsertResult<T> r = insert_locked(key, value);
    write_unlock_bucket(b);
    if (r.inserted) maybe_rehash();
    return r;
  }

  /** Insert if absent, else overwrite. */
  HSHM_CROSS_FUN
  InsertResult<T> insert_or_assign(const Key &key, const T &value) {
    size_type b = bucket_of(key);
    write_lock_bucket(b);
    Node *n = find_in_bucket(b, key);
    InsertResult<T> r;
    if (n != nullptr) {
      n->value_ = value;
      r = {false, &n->value_};
    } else {
      Node *fresh = new_node(key, value, buckets_[b]);
      if (fresh == nullptr) {
        r = {false, nullptr};
      } else {
        buckets_[b] = fresh;
        size_.fetch_add(1);
        r = {true, &fresh->value_};
      }
    }
    write_unlock_bucket(b);
    if (r.inserted) maybe_rehash();
    return r;
  }

  /** operator[] -- creates a default-valued entry if absent. */
  HSHM_CROSS_FUN
  T &operator[](const Key &key) {
    size_type b = bucket_of(key);
    write_lock_bucket(b);
    Node *n = find_in_bucket(b, key);
    if (n == nullptr) {
      Node *fresh = new_node(key, T(), buckets_[b]);
      buckets_[b] = fresh;
      size_.fetch_add(1);
      n = fresh;
    }
    T &ref = n->value_;
    write_unlock_bucket(b);
    return ref;
  }

  /** Lookup (mutable) returning a pointer or nullptr. */
  HSHM_CROSS_FUN
  T *find(const Key &key) {
    size_type b = bucket_of(key);
    read_lock_bucket(b);
    Node *n = find_in_bucket(b, key);
    T *res = n != nullptr ? &n->value_ : nullptr;
    read_unlock_bucket(b);
    return res;
  }

  /** Lookup (const). */
  HSHM_CROSS_FUN
  const T *find(const Key &key) const {
    size_type b = bucket_of(key);
    const_cast<unordered_map_ll *>(this)->read_lock_bucket(b);
    Node *n = find_in_bucket(b, key);
    const T *res = n != nullptr ? &n->value_ : nullptr;
    const_cast<unordered_map_ll *>(this)->read_unlock_bucket(b);
    return res;
  }

  HSHM_CROSS_FUN bool contains(const Key &key) { return find(key) != nullptr; }
  HSHM_CROSS_FUN size_type count(const Key &key) { return contains(key) ? 1 : 0; }

  /** Erase by key. */
  HSHM_CROSS_FUN
  size_type erase(const Key &key) {
    size_type b = bucket_of(key);
    write_lock_bucket(b);
    size_type r = erase_locked(key);
    write_unlock_bucket(b);
    return r;
  }

  /** Drop all entries. */
  HSHM_CROSS_FUN
  void clear() {
    write_lock_all();
    for (size_type i = 0; i < buckets_.size(); ++i) {
      Node *cur = buckets_[i];
      while (cur != nullptr) {
        Node *next = cur->next_;
        del_node(cur);
        cur = next;
      }
      buckets_[i] = nullptr;
    }
    size_.store(0);
    write_unlock_all();
  }

  /** Apply `fn(key, value)` to every entry. Takes every write lock so
   *  the callback sees a consistent snapshot. */
  template <typename Func>
  HSHM_CROSS_FUN void for_each(Func fn) {
    write_lock_all();
    for (size_type i = 0; i < buckets_.size(); ++i) {
      Node *cur = buckets_[i];
      while (cur != nullptr) {
        fn(cur->key_, cur->value_);
        cur = cur->next_;
      }
    }
    write_unlock_all();
  }

  template <typename Func>
  HSHM_CROSS_FUN void for_each(Func fn) const {
    auto *self = const_cast<unordered_map_ll *>(this);
    self->write_lock_all();
    for (size_type i = 0; i < buckets_.size(); ++i) {
      Node *cur = buckets_[i];
      while (cur != nullptr) {
        fn(cur->key_, cur->value_);
        cur = cur->next_;
      }
    }
    self->write_unlock_all();
  }
};

}  // namespace hshm::priv

#endif  // HSHM_DATA_STRUCTURES_PRIV_UNORDERED_MAP_LL_H_
