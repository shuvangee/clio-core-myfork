/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

#ifndef WRP_CTE_GPU_VECTOR_KERNELS_H_
#define WRP_CTE_GPU_VECTOR_KERNELS_H_

#include <chimaera/gpu/gpu_ipc_manager.h>
#include <chimaera/types.h>
#include <wrp_cte/gpu_vector/gpu_vector_page.h>
#include <wrp_cte/gpu_vector/gpu_vector_view.h>

#if HSHM_IS_GPU_COMPILER

namespace wrp_cte::gpu_vector {

namespace detail {

/** Per-warp last-page cache. Lane 0 reads / writes; other lanes read after
 *  __syncwarp. The user kernel must provide a __shared__ array via
 *  WRP_GPU_VECTOR_KERNEL_INIT — this function just resolves the lane slot. */
HSHM_GPU_FUN Page *&LaneLastPage(Page **last_page_array) {
  return last_page_array[threadIdx.x & 31];
}

/** Atomically swap a 32-bit field to `new_val` and return the old value. */
HSHM_GPU_FUN int32_t AtomicExchI32(int32_t *p, int32_t new_val) {
  return atomicExch(reinterpret_cast<int *>(p), static_cast<int>(new_val));
}

/** Atomic CAS for u32. */
HSHM_GPU_FUN chi::u32 AtomicCasU32(chi::u32 *p, chi::u32 expected,
                                          chi::u32 desired) {
  return atomicCAS(reinterpret_cast<unsigned int *>(p),
                   static_cast<unsigned int>(expected),
                   static_cast<unsigned int>(desired));
}

/** Atomic OR on u32 — returns the old value. */
HSHM_GPU_FUN chi::u32 AtomicOrU32(chi::u32 *p, chi::u32 mask) {
  return atomicOr(reinterpret_cast<unsigned int *>(p),
                  static_cast<unsigned int>(mask));
}

/** Atomic AND-NOT on u32 (clears bits). */
HSHM_GPU_FUN chi::u32 AtomicClearBitsU32(chi::u32 *p, chi::u32 mask) {
  return atomicAnd(reinterpret_cast<unsigned int *>(p),
                   static_cast<unsigned int>(~mask));
}

/** Atomic min for i32 (CAS loop — atomicMin's i32 overload exists but
 *  using a CAS keeps the path uniform with the max variant below). */
HSHM_GPU_FUN void AtomicMinI32(int32_t *p, int32_t v) {
  atomicMin(reinterpret_cast<int *>(p), static_cast<int>(v));
}

/** Atomic max for i32. */
HSHM_GPU_FUN void AtomicMaxI32(int32_t *p, int32_t v) {
  atomicMax(reinterpret_cast<int *>(p), static_cast<int>(v));
}

/** Atomic increment for u32. */
HSHM_GPU_FUN chi::u32 AtomicIncU32(chi::u32 *p) {
  return atomicAdd(reinterpret_cast<unsigned int *>(p), 1u);
}

/** Atomic decrement for u32. */
HSHM_GPU_FUN chi::u32 AtomicDecU32(chi::u32 *p) {
  return atomicSub(reinterpret_cast<unsigned int *>(p), 1u);
}

/**
 * Build a blob_data_ ShmPtr that ToFullPtr resolves directly to a raw
 * pointer via its null-alloc_id branch (the canonical pattern used by
 * the kernel-side CTE benchmarks: see workload_cte_client_overhead.cc).
 *
 * For kDeviceMem-backed pages, off_ holds a CUDA/HIP device address.
 * The bdev runtime calls DeviceAwareMemcpy with that address;
 * DeviceAwareMemcpy's registered hook (cudaMemcpyDefault /
 * hipMemcpyDefault) auto-detects the memory kind via pointer attributes
 * and copies device→host without staging.
 *
 * `alloc_id` is unused but kept in the signature so callers can pass
 * the page backend's id without special-casing.
 */
HSHM_GPU_FUN hipc::ShmPtr<> MakeBlobShmPtr(void *device_addr,
                                                  hipc::AllocatorId alloc_id) {
  (void)alloc_id;
  hipc::ShmPtr<> p;
  p.alloc_id_ = hipc::AllocatorId::GetNull();
  p.off_ = reinterpret_cast<chi::u64>(device_addr);
  return p;
}

/**
 * Construct the blob name into a heap-free char buffer. Names are stable
 * for the lifetime of the Vector and only depend on (block, slot) so we
 * cache them in the Pool by writing the name into the pre-allocated task
 * once (host side, at ctor). The kernel just leaves blob_name_ alone.
 */

}  // namespace detail

/** T-agnostic FlushPage: takes element size in bytes explicitly so the
 *  cache-management / drain kernels can compile as non-template
 *  __global__ functions (template __global__'s aren't reliably
 *  registered by nvcc's launch glue).
 *
 *  Caller must pass the kernel-scope `g_ipc_manager_ptr` from
 *  CHIMAERA_GPU_INIT — going through CHI_IPC in this device function
 *  trips the host-pass typing check (CHI_IPC expands to chi::IpcManager*
 *  on host pass, which returns chi::Future, not gpu::Future). */
HSHM_GPU_FUN void FlushPageBase(::chi::gpu::IpcManager *ipc,
                                 const DeviceViewBase &v, chi::u32 block_idx,
                                 Page *page, chi::u32 slot) {
  if (page->page_idx < 0) return;

  int32_t mn = detail::AtomicExchI32(&page->modify_min, -1);
  int32_t mx = detail::AtomicExchI32(&page->modify_max, -1);
  if (mn < 0 || mx < 0 || mx < mn) return;

  detail::AtomicDecU32(&GetBlock(v, block_idx)->num_modified);

  auto *task = GetPutTask(v, block_idx, slot);
  // For the T-agnostic path the management kernel doesn't know T, so it
  // flushes the entire page (page_size_bytes from page_byte_off). The
  // T-aware FlushPage<T>() in the eviction path uses tighter
  // [modify_min, modify_max] ranges.
  chi::u64 page_byte_off =
      static_cast<chi::u64>(page->page_idx) * v.page_size_bytes;
  task->offset_ = page_byte_off;
  task->size_ = v.page_size_bytes;
  task->blob_data_ = detail::MakeBlobShmPtr(page->device_ptr,
                                             v.pages_alloc_id);

  hipc::FullPtr<wrp_cte::core::PutBlobTask> fp;
  fp.shm_.alloc_id_ = v.put_pool_alloc_id;
  fp.shm_.off_ = reinterpret_cast<chi::u64>(task);
  fp.ptr_ = task;
  page->active_put = ipc->Send(fp);
}

/**
 * Submit a PutBlob for `page` covering its current modify range. Caller
 * already CAS-holds the kPagePutInFlight bit. Returns once the queue
 * push completes; does not wait for the runtime to ack. `ipc` is the
 * kernel-scope `g_ipc_manager_ptr` from CHIMAERA_GPU_INIT.
 *
 * `slot` is the index in `Block::pages[]`, used to pick which task slot
 * in the put pool we use (one slot per page, so no cross-page aliasing).
 */
template <typename T>
HSHM_GPU_FUN void FlushPage(::chi::gpu::IpcManager *ipc,
                            const DeviceView<T> &v, chi::u32 block_idx,
                            Page *page, chi::u32 slot) {
  if (page->page_idx < 0) return;

  // Atomically reset the dirty range so the next concurrent writer
  // observes a fresh window. Whatever range we capture here is what we
  // promise the runtime; later writes form a new range that the next
  // tick picks up.
  int32_t mn = detail::AtomicExchI32(&page->modify_min, -1);
  int32_t mx = detail::AtomicExchI32(&page->modify_max, -1);
  if (mn < 0 || mx < 0 || mx < mn) return;

  // Bookkeeping: this page is no longer in the dirty count.
  detail::AtomicDecU32(&GetBlock(v.base, block_idx)->num_modified);

  auto *task = GetPutTask(v.base, block_idx, slot);
  // The host already populated tag_id_, blob_name_, score_, context_,
  // pool_query_, pool_id_, method_ at construction time. We only stamp
  // the per-flush mutable fields.
  chi::u64 t_size = sizeof(T);
  chi::u64 page_byte_off = static_cast<chi::u64>(page->page_idx) *
                            v.base.page_size_bytes;
  chi::u64 mn_b = static_cast<chi::u64>(mn) * t_size;
  chi::u64 mx_b = (static_cast<chi::u64>(mx) + 1) * t_size;
  task->offset_ = page_byte_off + mn_b;
  task->size_ = mx_b - mn_b;
  task->blob_data_ = detail::MakeBlobShmPtr(
      reinterpret_cast<char *>(page->device_ptr) + mn_b,
      v.base.pages_alloc_id);

  // Wrap into a FullPtr the producer-only Send understands: alloc_id is
  // the put-pool backend; off_ is the raw pinned-host address (worker
  // dereferences directly).
  hipc::FullPtr<wrp_cte::core::PutBlobTask> fp;
  fp.shm_.alloc_id_ = v.base.put_pool_alloc_id;
  fp.shm_.off_ = reinterpret_cast<chi::u64>(task);
  fp.ptr_ = task;

  page->active_put = ipc->Send(fp);
}

/** Submit a GetBlob to fault `target_page_idx` into `page->device_ptr`. */
template <typename T>
HSHM_GPU_FUN void FaultPage(::chi::gpu::IpcManager *ipc,
                            const DeviceView<T> &v, chi::u32 block_idx,
                            Page *page, chi::u32 slot,
                            int32_t target_page_idx) {
  auto *task = GetGetTask(v.base, block_idx, slot);
  chi::u64 page_byte_off = static_cast<chi::u64>(target_page_idx) *
                            v.base.page_size_bytes;
  task->offset_ = page_byte_off;
  task->size_ = v.base.page_size_bytes;
  task->blob_data_ = detail::MakeBlobShmPtr(page->device_ptr,
                                             v.base.pages_alloc_id);
  hipc::FullPtr<wrp_cte::core::GetBlobTask> fp;
  fp.shm_.alloc_id_ = v.base.get_pool_alloc_id;
  fp.shm_.off_ = reinterpret_cast<chi::u64>(task);
  fp.ptr_ = task;
  page->active_get = ipc->Send(fp);
}

/** Wait on any in-flight put for this page, then clear the slot. */
HSHM_GPU_FUN void DrainPut(Page *page) {
  if (!page->active_put.IsNull()) {
    page->active_put.Wait();
    page->active_put = chi::gpu::Future<wrp_cte::core::PutBlobTask>();
    detail::AtomicClearBitsU32(&page->flags, kPagePutInFlight);
  }
}

/** Wait on any in-flight get for this page, then clear the slot. */
HSHM_GPU_FUN void DrainGet(Page *page) {
  if (!page->active_get.IsNull()) {
    page->active_get.Wait();
    page->active_get = chi::gpu::Future<wrp_cte::core::GetBlobTask>();
  }
}

/** Flush every dirty page in the calling block. */
template <typename T>
HSHM_GPU_FUN void FlushAllInBlock(::chi::gpu::IpcManager *ipc,
                                  const DeviceView<T> &v,
                                  chi::u32 block_idx) {
  Block *b = GetBlock(v.base, block_idx);
  for (chi::u32 s = 0; s < v.base.pages_per_block; ++s) {
    Page *p = &b->pages[s];
    if (p->modify_min < 0) continue;
    if (detail::AtomicOrU32(&p->flags, kPagePutInFlight) & kPagePutInFlight) {
      continue;
    }
    FlushPage(ipc, v, block_idx, p, s);
  }
}

/**
 * Pick a victim slot (free first, else LRU). Drains any in-flight ops
 * on it before returning so the caller can reuse the slot freely.
 */
template <typename T>
HSHM_GPU_FUN chi::u32 EvictSlot(::chi::gpu::IpcManager *ipc,
                                const DeviceView<T> &v,
                                chi::u32 block_idx) {
  Block *b = GetBlock(v.base, block_idx);
  // Flush every dirty page first (kicks Sends in flight; we will Wait
  // on the chosen victim's active_put below if needed).
  FlushAllInBlock(ipc, v, block_idx);
  // Free slot?
  for (chi::u32 s = 0; s < v.base.pages_per_block; ++s) {
    if (b->pages[s].page_idx < 0) {
      DrainPut(&b->pages[s]);
      DrainGet(&b->pages[s]);
      return s;
    }
  }
  // LRU.
  chi::u32 lru = 0;
  chi::u64 lru_clock = b->pages[0].lru_clock;
  for (chi::u32 s = 1; s < v.base.pages_per_block; ++s) {
    if (b->pages[s].lru_clock < lru_clock) {
      lru_clock = b->pages[s].lru_clock;
      lru = s;
    }
  }
  DrainPut(&b->pages[lru]);
  DrainGet(&b->pages[lru]);
  return lru;
}

/** Resolve `i` to (page,offset) and return a pointer to the byte slot.
 *  Used by both read and write paths. `is_write` controls FaultPage vs
 *  not — writes don't bother faulting because they overwrite. */
template <typename T>
HSHM_GPU_FUN T *Resolve(::chi::gpu::IpcManager *ipc, DeviceView<T> v,
                        Page **last_page_array, chi::u64 i, bool is_write) {
  chi::u32 block_idx = blockIdx.x;
  Block *b = GetBlock(v.base, block_idx);
  int32_t target_page = static_cast<int32_t>(i / v.page_capacity_t);
  chi::u64 off_t = i - static_cast<chi::u64>(target_page) * v.page_capacity_t;

  // (1) Per-lane fast path.
  Page *&last = detail::LaneLastPage(last_page_array);
  Page *hit = nullptr;
  if (last && last->page_idx == target_page) {
    hit = last;
  } else {
    // (2) Block-local linear scan.
    for (chi::u32 s = 0; s < v.base.pages_per_block; ++s) {
      if (b->pages[s].page_idx == target_page) {
        hit = &b->pages[s];
        last = hit;
        break;
      }
    }
  }
  if (!hit) {
    // (3) Evict, bind, and (read path only) fault.
    chi::u32 slot = EvictSlot(ipc, v, block_idx);
    Page *p = &b->pages[slot];
    p->page_idx = target_page;
    p->modify_min = -1;
    p->modify_max = -1;
    p->flags = 0;
    if (!is_write) FaultPage(ipc, v, block_idx, p, slot, target_page);
    hit = p;
    last = hit;
  }

  // Wait on outstanding fault before returning the byte (read path).
  if (!is_write) DrainGet(hit);

  // LRU bookkeeping intentionally elided — clock64() is several hundred
  // cycles and only useful on miss for victim selection. Eviction
  // currently falls back to the lowest-numbered free slot or slot 0
  // when all are bound, which is fine while pages_per_block stays
  // small. Re-introduce a coarser clock (e.g. monotonic counter
  // incremented on miss) when LRU quality starts to matter.

  if (is_write) {
    int32_t off_i = static_cast<int32_t>(off_t);
    // Plain stores everywhere on the per-page dirty range.
    //
    // Concurrency assumption: ONE thread mutates a given (block,page) at
    // a time AND no concurrent CacheMgmtKernel atomicExch'es modify_min
    // /max to -1 mid-update. The first invariant holds today because
    // only threadIdx.x == 0 of each block calls Resolve. The second
    // invariant requires cache_period_ms = 0 (no periodic flush thread).
    // If you need concurrent flushing, revert this branch to atomicCAS
    // for the first-write seed and AtomicMinI32 / AtomicMaxI32 for the
    // range extension — see git history for the prior implementation.
    if (hit->modify_min == -1) {
      hit->modify_min = off_i;
      hit->modify_max = off_i;
      ++b->num_modified;
    } else {
      if (off_i < hit->modify_min) hit->modify_min = off_i;
      if (off_i > hit->modify_max) hit->modify_max = off_i;
    }
  }
  return reinterpret_cast<T *>(hit->device_ptr) + off_t;
}

}  // namespace wrp_cte::gpu_vector

namespace cte::gpu::dev {

template <typename T>
class vector;

/**
 * Proxy returned by `vector<T>::operator[]`. Defers the read-vs-write
 * decision until the proxy is consumed:
 *   - Implicit conversion to `T` triggers a read (Resolve with
 *     is_write=false → may FaultPage and Wait).
 *   - `operator=`, `operator+=`, etc. trigger a write (Resolve with
 *     is_write=true → records the dirty range, no fault).
 *
 * Stack-only RAII handle; never store across kernel boundaries.
 */
template <typename T>
class ElementRef {
 public:
  HSHM_GPU_FUN ElementRef(vector<T> *v, chi::u64 i) : v_(v), i_(i) {}

  /** Read. */
  HSHM_GPU_FUN operator T() const;

  /** Write. */
  HSHM_GPU_FUN ElementRef &operator=(const T &val);

  /** Read-modify-write. */
  HSHM_GPU_FUN ElementRef &operator+=(const T &val);
  HSHM_GPU_FUN ElementRef &operator-=(const T &val);

  /** Chained assignment from another ElementRef. */
  HSHM_GPU_FUN ElementRef &operator=(const ElementRef &other) {
    return (*this = static_cast<T>(other));
  }

 private:
  vector<T> *v_;
  chi::u64 i_;
};

/**
 * Per-block in-kernel handle to a host-side Vector<T>. Constructed once
 * at the top of a user kernel after CHIMAERA_GPU_INIT. The ctor allocates
 * the per-warp last-page cache in __shared__ memory and zero-initializes
 * it via the first warp.
 *
 * Usage:
 *   __global__ void K(chi::IpcManagerGpuInfo info,
 *                     wrp_cte::gpu_vector::DeviceView<int> view) {
 *     CHIMAERA_GPU_INIT(info, nullptr);
 *     cte::gpu::dev::vector<int> v(view, g_ipc_manager_ptr);
 *     v[i] = 42;            // write
 *     int x = v[j];         // read
 *     v[k] += 1;             // read-modify-write
 *   }
 *
 * Only thread 0 of each warp should call operator[] (matches the
 * IpcGpu2Cpu::ClientSend threadIdx.x==0 contract for the underlying
 * Send). All threads in the block must construct the handle so the
 * ctor's __syncthreads is balanced.
 */
template <typename T>
class vector {
 public:
  using DeviceView = ::wrp_cte::gpu_vector::DeviceView<T>;

  /**
   * @param view DeviceView<T> from `Vector<T>::Device()` (POD, captured
   *             by the kernel by value).
   * @param ipc  Kernel-scope `g_ipc_manager_ptr` declared by
   *             CHIMAERA_GPU_INIT. CHI_IPC can't be used here because on
   *             the host pass it expands to chi::IpcManager* (not
   *             gpu::IpcManager*).
   */
  HSHM_GPU_FUN vector(const DeviceView &view,
                      ::chi::gpu::IpcManager *ipc) noexcept
      : view_(view), ipc_(ipc) {
    // Function-scope __shared__ has block lifetime, so the address
    // captured into last_page_array_ stays valid for the whole kernel.
    __shared__ ::wrp_cte::gpu_vector::Page *last_page_storage[32];
    last_page_array_ = last_page_storage;
    if (threadIdx.x < 32) last_page_array_[threadIdx.x] = nullptr;
    __syncthreads();
  }

  /** Return a proxy for `v[i]`. The proxy resolves the access (and
   *  records dirty state on write) lazily. */
  HSHM_GPU_FUN ElementRef<T> operator[](chi::u64 i) {
    return ElementRef<T>(this, i);
  }

  /** Read-only overload — returns a value, never an lvalue. */
  HSHM_GPU_FUN T operator[](chi::u64 i) const {
    return *::wrp_cte::gpu_vector::Resolve(
        ipc_, const_cast<DeviceView &>(view_),
        last_page_array_, i, /*is_write=*/false);
  }

  /** Flush every dirty page in the calling block. */
  HSHM_GPU_FUN void FlushAll() {
    ::wrp_cte::gpu_vector::FlushAllInBlock(ipc_, view_, blockIdx.x);
  }

  HSHM_GPU_FUN const DeviceView &view() const { return view_; }

 private:
  friend class ElementRef<T>;
  DeviceView view_;
  ::chi::gpu::IpcManager *ipc_;
  ::wrp_cte::gpu_vector::Page **last_page_array_;
};

template <typename T>
HSHM_GPU_FUN ElementRef<T>::operator T() const {
  return *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                          v_->last_page_array_, i_,
                                          /*is_write=*/false);
}

template <typename T>
HSHM_GPU_FUN ElementRef<T> &ElementRef<T>::operator=(const T &val) {
  *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                   v_->last_page_array_, i_,
                                   /*is_write=*/true) = val;
  return *this;
}

template <typename T>
HSHM_GPU_FUN ElementRef<T> &ElementRef<T>::operator+=(const T &val) {
  T cur = *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                           v_->last_page_array_, i_, false);
  *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                   v_->last_page_array_, i_, true) =
      cur + val;
  return *this;
}

template <typename T>
HSHM_GPU_FUN ElementRef<T> &ElementRef<T>::operator-=(const T &val) {
  T cur = *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                           v_->last_page_array_, i_, false);
  *::wrp_cte::gpu_vector::Resolve(v_->ipc_, v_->view_,
                                   v_->last_page_array_, i_, true) =
      cur - val;
  return *this;
}

}  // namespace cte::gpu::dev

#endif  // HSHM_IS_GPU_COMPILER

#endif  // WRP_CTE_GPU_VECTOR_KERNELS_H_
