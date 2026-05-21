/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

#ifndef CHIMAERA_DEVICE_MEMCPY_H_
#define CHIMAERA_DEVICE_MEMCPY_H_

/**
 * Device-aware memcpy hook.
 *
 * Why this exists:
 *
 *   The bdev runtime's WriteToRam / ReadFromRam / WriteToFile /
 *   ReadFromFile handlers receive a `data` ShmPtr that was filled in by
 *   the original PutBlob / GetBlob caller. With the SYCL build, that
 *   caller is sometimes a GPU kernel that allocated its data buffer in
 *   device USM (sycl::malloc_device) — i.e. an HBM-resident pointer
 *   the CPU cannot dereference with a plain memcpy. The bdev needs to
 *   route those copies through the SYCL queue (sycl::queue::memcpy)
 *   which dispatches based on USM pointer kind.
 *
 *   The bdev runtime is built without -fsycl (precompiled-headers
 *   conflict with the offload bundler), so it cannot call SYCL APIs
 *   directly. Instead, the SYCL init code (gpu/gpu2cpu_init_sycl.cc,
 *   which IS built with -fsycl) installs a function pointer here at
 *   ServerInit time. Bdev calls `chi::DeviceAwareMemcpy(...)`, which
 *   uses the hook when set and falls back to std::memcpy otherwise.
 *
 *   Setting the hook is single-threaded (server init), reading is
 *   single-threaded per task (workers don't race the hook itself).
 *   The atomic load is for visibility, not contention.
 */

#include <atomic>
#include <cstddef>
#include <cstring>

namespace clio::run {

/** Device-aware memcpy signature. Same shape as std::memcpy. */
using DeviceAwareMemcpyFn = void (*)(void *dst, const void *src, std::size_t n);

/** Predicate: returns true if ptr is device USM (not host-accessible). */
using IsDevicePointerFn = bool (*)(const void *ptr);

/**
 * Installable hook. Set by ServerInitGpuQueuesSycl (or its CUDA/ROCm
 * equivalent) so the bdev runtime can route memcpys involving device
 * USM through the GPU runtime instead of failing on a plain memcpy.
 *
 * Default value is nullptr; the inline DeviceAwareMemcpy() helper
 * falls back to std::memcpy when nullptr.
 */
extern std::atomic<DeviceAwareMemcpyFn> g_device_aware_memcpy;

/**
 * Companion hook: identifies whether a pointer is device USM. Used by
 * the bdev's file path to decide whether to stage through a host
 * buffer (when the data lives on device, libaio/POSIX-AIO can't
 * dereference it). When unset, IsDevicePointer() returns false —
 * matching the pre-GPU semantics.
 */
extern std::atomic<IsDevicePointerFn> g_is_device_pointer;

/**
 * Memcpy that dispatches to the registered device-aware hook if any.
 * On a non-GPU build (or before init), this is exactly std::memcpy.
 *
 * Both pointers may be host or device USM; the hook is responsible
 * for handling all four src/dst combinations. SYCL's
 * sycl::queue::memcpy already does that.
 */
inline void DeviceAwareMemcpy(void *dst, const void *src, std::size_t n) {
  DeviceAwareMemcpyFn fn = g_device_aware_memcpy.load(std::memory_order_acquire);
  if (fn != nullptr) {
    fn(dst, src, n);
  } else {
    std::memcpy(dst, src, n);
  }
}

/**
 * Returns true if ptr is device USM (host load/store would segfault).
 * Used by the file-path bdev to stage through a host buffer only when
 * needed.
 */
inline bool IsDevicePointer(const void *ptr) {
  IsDevicePointerFn fn = g_is_device_pointer.load(std::memory_order_acquire);
  return fn ? fn(ptr) : false;
}

}  // namespace clio::run

#endif  // CHIMAERA_DEVICE_MEMCPY_H_
