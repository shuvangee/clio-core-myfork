/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 */

#ifndef BDEV_RUNTIME_H_
#define BDEV_RUNTIME_H_

#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/comutex.h>
#include "bdev_client.h"
#include "bdev_tasks.h"
#include "bdev_alloc_log.h"
#include <clio_ctp/io/async_io_factory.h>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace clio::run::bdev {

/**
 * Per-worker I/O context for parallel file access
 * Each worker has its own file descriptor and Linux AIO context
 * for efficient parallel I/O without contention
 */
struct WorkerIOContext {
  std::unique_ptr<ctp::AsyncIO> async_io_;  /**< Async I/O backend for this worker */
  bool is_initialized_ = false;              /**< Whether this context is initialized */

  WorkerIOContext() = default;

  WorkerIOContext(WorkerIOContext &&other) noexcept
      : async_io_(std::move(other.async_io_)),
        is_initialized_(other.is_initialized_) {
    other.is_initialized_ = false;
  }

  WorkerIOContext &operator=(WorkerIOContext &&other) noexcept {
    if (this != &other) {
      Cleanup();
      async_io_ = std::move(other.async_io_);
      is_initialized_ = other.is_initialized_;
      other.is_initialized_ = false;
    }
    return *this;
  }

  WorkerIOContext(const WorkerIOContext &) = delete;
  WorkerIOContext &operator=(const WorkerIOContext &) = delete;

  /**
   * Initialize the worker I/O context
   * @param file_path Path to the file to open
   * @param io_depth Maximum number of concurrent I/O operations
   * @param worker_id Worker ID for logging
   * @return true if initialization successful, false otherwise
   */
  bool Init(const std::string &file_path, clio::run::u32 io_depth, clio::run::u32 worker_id);

  /**
   * Cleanup and close all resources
   */
  void Cleanup();

  ~WorkerIOContext() {
    Cleanup();
  }
};


/**
 * Block size categories for data allocator
 * We cache the following block sizes: 256B, 1KB, 4KB, 64KB, 128KB, 1MB
 */
enum class BlockSizeCategory : clio::run::u32 {
  k256B = 0,
  k1KB = 1,
  k4KB = 2,
  k64KB = 3,
  k128KB = 4,
  k1MB = 5,
  kMaxCategories = 6
};

/**
 * Per-worker block cache
 * Maintains free lists for different block sizes without locking
 */
class WorkerBlockMap {
 public:
  WorkerBlockMap();

  /**
   * Allocate a block from the cache.
   *
   * For buckets where every freed block is exactly the nominal class size
   * the head element is always a fit, so the search is O(1). The largest
   * bucket doubles as the fallthrough sink for over-cap blocks (size >
   * kBlockSizes[max]) and therefore holds heterogeneous sizes; the
   * `min_size` filter is what keeps a caller from being handed a
   * spuriously undersized block out of that bucket.
   *
   * @param block_type Block size category index
   * @param block Output block to populate
   * @param min_size Reject any block whose size_ is smaller than this
   * @return true if allocation succeeded, false otherwise
   */
  bool AllocateBlock(int block_type, Block& block, size_t min_size = 0);

  /**
   * Free a block back to the cache
   * @param block Block to free
   */
  void FreeBlock(Block block);

 private:
  std::vector<std::list<Block>> blocks_;
};

/**
 * Global block map with per-worker caching and locking
 */
class GlobalBlockMap {
 public:
  GlobalBlockMap();

  /**
   * Initialize with number of workers
   * @param num_workers Number of worker threads
   */
  void Init(size_t num_workers);

  /**
   * Push a free range [offset, offset+size) into worker 0's free list,
   * split into the allocator's size-class buckets (largest-first, like
   * FreeBlock classifies by size). Used during recovery to make the gaps
   * between live blocks reusable.
   * @param offset Start of the free range (assumed alignment-aligned)
   * @param size Size of the free range
   */
  void SeedFreeRange(clio::run::u64 offset, clio::run::u64 size);

  /**
   * Allocate a block for a given worker
   * @param worker Worker ID
   * @param io_size Requested I/O size
   * @param block Output block to populate
   * @return true if allocation succeeded, false otherwise
   */
  bool AllocateBlock(int worker, size_t io_size, Block& block);

  /**
   * Free a block for a given worker
   * @param worker Worker ID
   * @param block Block to free
   * @return true if free succeeded
   */
  bool FreeBlock(int worker, Block& block);

 private:
  std::vector<WorkerBlockMap> worker_maps_;
  std::vector<clio::run::CoMutex> worker_locks_;

  /**
   * Find the next block size category larger than the requested size
   * @param io_size Requested I/O size
   * @return Block type index, or -1 if no suitable size
   */
  int FindBlockType(size_t io_size);
};

/**
 * Heap allocator for new blocks
 */
class Heap {
 public:
  Heap();

  /**
   * Initialize heap with total size and alignment
   * @param total_size Total size available for allocation
   * @param alignment Alignment requirement for offsets and sizes (default 4096)
   */
  void Init(clio::run::u64 total_size, clio::run::u32 alignment = 4096);

  /**
   * Reconstruct the heap bump pointer from a recovered live set so no live
   * offset is ever re-handed-out. Sets the bump to max(offset+size) over the
   * live blocks (0 if none). The gaps in [0, bump) not covered by any live
   * block are NOT placed in the heap (which is a pure bump allocator); the
   * caller (GlobalBlockMap::InitFromLive) is responsible for pushing those
   * gaps into the size-class free lists so freed gaps remain reusable.
   *
   * @param live Recovered live blocks (any order)
   * @param total_size Total size available for allocation
   * @param alignment Alignment requirement for offsets and sizes
   */
  void InitFromLive(const std::vector<LiveBlock> &live, clio::run::u64 total_size,
                    clio::run::u32 alignment = 4096);

  /**
   * Allocate a block from the heap
   * @param block_size Size of block to allocate
   * @param block_type Block type category
   * @param block Output block to populate
   * @return true if allocation succeeded, false if out of space
   */
  bool Allocate(size_t block_size, int block_type, Block& block);

  /**
   * Get remaining allocatable space
   * @return Number of bytes remaining for allocation
   */
  clio::run::u64 GetRemainingSize() const;

 private:
  std::atomic<clio::run::u64> heap_;
  clio::run::u64 total_size_;
  clio::run::u32 alignment_;
};

/**
 * Runtime container for bdev operations
 */
class Runtime : public clio::run::Container {
 public:
  // Required typedef for CLIO_TASK_CC macro
  using CreateParams = clio::run::bdev::CreateParams;
  
  Runtime() : bdev_type_(BdevType::kFile),
              total_reads_(0), total_writes_(0),
              total_bytes_read_(0), total_bytes_written_(0) {
    start_time_ = std::chrono::high_resolution_clock::now();
  }
  ~Runtime() override = default;

  /**
   * Get live task statistics for this task instance.
   */
  clio::run::TaskStat GetTaskStats(const clio::run::Task *task) const override;

  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume AllocateBlocks(clio::run::shared_ptr<AllocateBlocksTask> &task);
  clio::run::TaskResume FreeBlocks(clio::run::shared_ptr<FreeBlocksTask> &task);
  clio::run::TaskResume Write(clio::run::shared_ptr<WriteTask> &task);
  clio::run::TaskResume Read(clio::run::shared_ptr<ReadTask> &task);
  clio::run::TaskResume GetStats(clio::run::shared_ptr<GetStatsTask> &task);
  clio::run::TaskResume Update(clio::run::shared_ptr<UpdateTask> &task);
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);

  /**
   * Allocate multiple blocks (Method::kAllocateBlocks)
   */
  clio::run::TaskResume AllocateBlocks(ctp::ipc::FullPtr<AllocateBlocksTask> task, clio::run::RunContext& ctx);

  /**
   * Free data blocks (Method::kFreeBlocks)
   */
  clio::run::TaskResume FreeBlocks(ctp::ipc::FullPtr<FreeBlocksTask> task, clio::run::RunContext& ctx);

  /**
   * Write data to a block (Method::kWrite)
   */
  clio::run::TaskResume Write(ctp::ipc::FullPtr<WriteTask> task, clio::run::RunContext& ctx);

  /**
   * Read data from a block (Method::kRead)
   */
  clio::run::TaskResume Read(ctp::ipc::FullPtr<ReadTask> task, clio::run::RunContext& ctx);

  /**
   * Get performance statistics (Method::kGetStats)
   */
  clio::run::TaskResume GetStats(ctp::ipc::FullPtr<GetStatsTask> task, clio::run::RunContext& ctx);

  /**
   * Update GPU container with device/pinned memory pointers (Method::kUpdate)
   * No-op on the CPU side; UpdateTask is primarily handled by GpuRuntime.
   */
  clio::run::TaskResume Update(ctp::ipc::FullPtr<UpdateTask> task, clio::run::RunContext& ctx);

  /**
   * Monitor container state (Method::kMonitor)
   */
  clio::run::TaskResume Monitor(ctp::ipc::FullPtr<MonitorTask> task, clio::run::RunContext &rctx);

  /**
   * Periodically flush the allocator WAL to disk and compact it when it has
   * grown past a threshold (Method::kFlushAllocLog). Registered as a
   * TASK_PERIODIC task from Create when an alloc_log_path is configured.
   */
  clio::run::TaskResume FlushAllocLog(ctp::ipc::FullPtr<FlushAllocLogTask> task,
                                clio::run::RunContext &ctx);

  /**
   * Destroy the container (Method::kDestroy)
   */
  clio::run::TaskResume Destroy(ctp::ipc::FullPtr<DestroyTask> task, clio::run::RunContext& ctx);

  /**
   * REQUIRED VIRTUAL METHODS FROM clio::run::Container
   */

  /**
   * Initialize container with pool information
   */
  void Init(const clio::run::PoolId &pool_id, const std::string &pool_name,
            clio::run::u32 container_id = 0) override;

  /**
   * Send UpdateTask to GPU container after GPU container is registered.
   * Called from pool_manager after RegisterGpuOrchestratorContainer().
   */
  void PostGpuContainerCreate() override;
  clio::run::TaskResume Run(clio::run::u32 method,
                      clio::run::shared_ptr<clio::run::Task> task_ptr) override;
  clio::run::u64 GetWorkRemaining() const override;

  void SaveTask(clio::run::u32 method, clio::run::SaveTaskArchive& archive,
                clio::run::shared_ptr<clio::run::Task>& task_ptr) override;
  void LoadTask(clio::run::u32 method, clio::run::LoadTaskArchive& archive,
                clio::run::shared_ptr<clio::run::Task>& task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> AllocLoadTask(clio::run::u32 method, clio::run::LoadTaskArchive& archive) override;

  void LocalLoadTask(clio::run::u32 method, clio::run::DefaultLoadArchive& archive,
                     clio::run::shared_ptr<clio::run::Task>& task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> LocalAllocLoadTask(clio::run::u32 method, clio::run::DefaultLoadArchive& archive) override;
  void LocalSaveTask(clio::run::u32 method, clio::run::DefaultSaveArchive& archive,
                     clio::run::shared_ptr<clio::run::Task>& task_ptr) override;

  clio::run::shared_ptr<clio::run::Task> NewCopyTask(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task_ptr,
                                        bool deep) override;
  clio::run::shared_ptr<clio::run::Task> NewTask(clio::run::u32 method) override;
  void AggregateOut(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task,
                 const clio::run::shared_ptr<clio::run::Task>& replica_task) override;

 private:
  Client client_;
  BdevType bdev_type_;                            

  std::unique_ptr<BdevTransport> transport_;

  // File-based storage (kFile)
  std::string file_path_;                         // Path to the file (for per-worker FD creation)
  std::vector<WorkerIOContext> worker_io_contexts_;  // Per-worker I/O contexts
  clio::run::u64 file_size_;                            // Total file size
  clio::run::u32 alignment_;                            // I/O alignment requirement
  clio::run::u32 io_depth_;                             // Max concurrent I/O operations

  // RAM-based storage (kRam) — vector of fixed-size pages, lazily allocated.
  // Pages are not pre-faulted: each page is allocated only when first written
  // to, and the OS faults the underlying physical pages on first touch. This
  // keeps memory usage proportional to data actually stored, not capacity.
  static constexpr clio::run::u64 kRamPageSize = 1ULL << 30;  // 1 GiB
  std::vector<std::unique_ptr<char[]>> ram_pages_;
  clio::run::u64 ram_capacity_;  // Soft cap; UINT64_MAX = unbounded
  mutable std::mutex ram_pages_mu_;

  // kHbm / kPinned removed — see WriteToRam comment above.

  // New allocator components
  GlobalBlockMap global_block_map_;              // Global block cache with per-worker locking
  Heap heap_;                                     // Heap allocator for new blocks

  // Persistent allocator-state log (WAL). Disabled (no file) when the
  // configured alloc_log_path is empty — preserves pre-WAL behavior.
  AllocatorLog alloc_log_;
  // Compaction threshold: when records-on-disk exceeds max(kMinCompactRecords,
  // live*kCompactGrowthFactor) the periodic task rewrites the log down to the
  // live set. Bounds the file to ~live-block count over time.
  static constexpr clio::run::u64 kCompactGrowthFactor = 2;
  static constexpr clio::run::u64 kMinCompactRecords = 256;

  // Performance tracking
  std::atomic<clio::run::u64> total_reads_;
  std::atomic<clio::run::u64> total_writes_;
  std::atomic<clio::run::u64> total_bytes_read_;
  std::atomic<clio::run::u64> total_bytes_written_;
  // True LIVE allocated bytes (incremented in AllocateBlocks regardless of
  // whether the block came from the GlobalBlockMap free list or the heap;
  // decremented in FreeBlocks). GetStats reports remaining = capacity -
  // allocated_bytes_ from this, NOT heap_'s monotonic bump high-water —
  // the bump pointer is never rolled back on free, so under concurrent
  // free-list misses it raced past the true live set and made CTE's
  // StatTargets see ~0 remaining (the 16-thread rc=12 placement bug).
  std::atomic<clio::run::u64> allocated_bytes_{0};
  std::chrono::high_resolution_clock::time_point start_time_;
  
  PerfMetrics perf_metrics_;

  /**
   * Initialize the data allocator from a recovered live set (group_id 0).
   * Reconstructs the heap bump and seeds the free list with the gaps so that
   * AllocateBlock never returns a range overlapping a live block and freed
   * gaps are reusable.
   * @param live Recovered live blocks
   */
  void InitializeAllocatorFromLive(const std::vector<LiveBlock> &live);

  /**
   * Initialize POSIX AIO control blocks
   */
  void InitializeAsyncIO();

  void UpdatePerformanceMetrics(bool is_write, clio::run::u64 bytes,
                                double duration_us);
};

} // namespace clio::run::bdev

#endif // BDEV_RUNTIME_H_
