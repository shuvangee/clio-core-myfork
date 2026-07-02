/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 */

#include <clio_runtime/bdev/bdev_runtime.h>
#include <clio_runtime/comutex.h>
#include <clio_ctp/util/gpu_api.h>
#include <clio_runtime/work_orchestrator.h>
#include <clio_runtime/worker.h>
#include <clio_ctp/introspect/system_info.h>
#include <clio_ctp/io/io_error.h>
#include <clio_ctp/serialize/msgpack_wrapper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include "clio_ctp/util/timer.h"

namespace clio::run::bdev {

//===========================================================================
// WorkerIOContext Implementation
//===========================================================================

bool WorkerIOContext::Init(const std::string &file_path, clio::run::u32 io_depth,
                           clio::run::u32 worker_id) {
  if (is_initialized_) {
    return true;  // Already initialized
  }

  // Create async I/O backend via factory (io_depth passed at construction)
#if CTP_ENABLE_NIXL
  async_io_ = ctp::AsyncIoFactory::Get(io_depth, ctp::AsyncIoBackend::kNixl);
#else
  async_io_ = ctp::AsyncIoFactory::Get(io_depth);
#endif
  if (!async_io_) {
    HLOG(kError, "Worker {} failed to create async I/O backend", worker_id);
    return false;
  }

  // AsyncIO owns the file descriptors internally
  if (!async_io_->Open(file_path, O_RDWR | O_CREAT, 0644)) {
    HLOG(kError, "Worker {} failed to open file: {}", worker_id, file_path);
    async_io_.reset();
    return false;
  }

  is_initialized_ = true;
  HLOG(kDebug,
       "Worker {} I/O context initialized: event_fd={}",
       worker_id, async_io_->GetEventFd());
  return true;
}

void WorkerIOContext::Cleanup() {
  if (!is_initialized_) {
    return;
  }

  if (async_io_) {
    async_io_->Close();
    async_io_.reset();
  }

  is_initialized_ = false;
}

// Block size constants (in bytes) - 4KB, 16KB, 32KB, 64KB, 128KB, 1MB
static const size_t kBlockSizes[] = {
    4096,     // 4KB
    16384,    // 16KB
    32768,    // 32KB
    65536,    // 64KB
    131072,   // 128KB
    1048576   // 1MB
};

//===========================================================================
// Helper Functions
//===========================================================================

/**
 * Find the block type for a given I/O size (rounds to next largest)
 * @param io_size Requested I/O size
 * @param out_block_size Output parameter for the actual block size
 * @return Block type index, or -1 if larger than all cached sizes
 */
static int FindBlockTypeForSize(size_t io_size, size_t &out_block_size) {
  // Find the next block size that is larger than or equal to io_size
  for (int i = 0; i < static_cast<int>(BlockSizeCategory::kMaxCategories);
       ++i) {
    if (kBlockSizes[i] >= io_size) {
      out_block_size = kBlockSizes[i];
      return i;
    }
  }
  // If io_size is larger than all cached sizes, return -1
  out_block_size = io_size;  // Use exact size
  return -1;
}

//===========================================================================
// WorkerBlockMap Implementation
//===========================================================================

WorkerBlockMap::WorkerBlockMap() {
  // Initialize vector with 5 empty lists (one for each block size category)
  blocks_.resize(static_cast<size_t>(BlockSizeCategory::kMaxCategories));
}

bool WorkerBlockMap::AllocateBlock(int block_type, Block &block,
                                   size_t min_size) {
  if (block_type < 0 ||
      block_type >= static_cast<int>(BlockSizeCategory::kMaxCategories)) {
    return false;
  }

  auto &list = blocks_[block_type];
  if (list.empty()) {
    return false;
  }

  // For block_types where every freed block is exactly the bucket's nominal
  // size, the head element is always a fit and we exit on the first pop.
  // The largest bucket (kMaxCategories-1) is the fallthrough sink for any
  // freed block whose actual size exceeds the largest cached class — those
  // blocks can be any size >= kBlockSizes[max], so an explicit min_size
  // check is needed to avoid returning an undersized block to a caller
  // that asked for, say, 2 MiB.
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (static_cast<size_t>(it->size_) >= min_size) {
      block = *it;
      list.erase(it);
      return true;
    }
  }
  return false;
}

void WorkerBlockMap::FreeBlock(Block block) {
  int block_type = static_cast<int>(block.block_type_);
  if (block_type >= 0 &&
      block_type < static_cast<int>(BlockSizeCategory::kMaxCategories)) {
    // Append to the block list
    blocks_[block_type].push_back(block);
  }
}

//===========================================================================
// GlobalBlockMap Implementation
//===========================================================================

GlobalBlockMap::GlobalBlockMap() {}

void GlobalBlockMap::Init(size_t num_workers) {
  // Pre-allocate vectors for specified number of workers
  worker_maps_.resize(num_workers);
  worker_locks_.resize(num_workers);
}

void GlobalBlockMap::SeedFreeRange(clio::run::u64 offset, clio::run::u64 size) {
  if (size == 0 || worker_maps_.empty()) {
    return;
  }
  // Carve [offset, offset+size) into the allocator's nominal size classes,
  // largest-first, and file each carved block into worker 0's free list under
  // the size class that AllocateBlock would consult for that size. A freed
  // gap thus becomes reusable by a subsequent AllocateBlock of the same (or
  // smaller) class. The tail that is smaller than the smallest class is
  // filed into the smallest bucket so it is still reachable for sub-class
  // requests (AllocateBlock validates min_size).
  const size_t kMaxIdx =
      static_cast<size_t>(BlockSizeCategory::kMaxCategories) - 1;
  clio::run::ScopedCoMutex lock(worker_locks_[0]);
  clio::run::u64 cur = offset;
  clio::run::u64 left = size;
  while (left > 0) {
    // Pick the largest nominal class <= left (clamp to smallest class if the
    // remaining gap is below the smallest class).
    size_t idx = 0;
    for (size_t i = kMaxIdx + 1; i-- > 0;) {
      if (kBlockSizes[i] <= left) {
        idx = i;
        break;
      }
    }
    clio::run::u64 chunk = std::min<clio::run::u64>(left, kBlockSizes[idx]);
    Block block(cur, chunk, static_cast<clio::run::u32>(idx));
    worker_maps_[0].FreeBlock(block);
    cur += chunk;
    left -= chunk;
  }
}

int GlobalBlockMap::FindBlockType(size_t io_size) {
  // Use the shared helper function to find block type
  size_t block_size;  // Not needed here, but required by the function signature
  return FindBlockTypeForSize(io_size, block_size);
}

bool GlobalBlockMap::AllocateBlock(int worker, size_t io_size, Block &block) {
  if (worker < 0 || static_cast<size_t>(worker) >= worker_maps_.size()) {
    return false;
  }

  size_t worker_idx = static_cast<size_t>(worker);

  // Find the next block size that is larger than this. When io_size exceeds
  // every cached class FindBlockType returns -1; mirror FreeBlocks' fallthrough
  // (which files such oversized blocks into the largest bucket via
  // FindBlockTypeForSize) so they stay reachable here too.  Without this
  // fallthrough, every freed 2 MiB block lands in bucket-max but AllocateBlock
  // never consults that bucket — so 2 MiB AllocateBlocks always falls through
  // to heap_.Allocate, growing the bdev's footprint monotonically with op
  // count.  WorkerBlockMap::AllocateBlock validates min_size for the
  // largest bucket so we don't accidentally return an undersized block.
  int block_type = FindBlockType(io_size);
  if (block_type == -1) {
    block_type = static_cast<int>(BlockSizeCategory::kMaxCategories) - 1;
  }

  // Acquire this worker's mutex using ScopedCoMutex
  {
    clio::run::ScopedCoMutex lock(worker_locks_[worker_idx]);
    // First attempt to allocate from this worker's map
    if (worker_maps_[worker_idx].AllocateBlock(block_type, block, io_size)) {
      return true;
    }
  }

  // If we fail, try up to 4 other workers (iterate linearly)
  size_t num_workers = worker_maps_.size();
  for (size_t i = 1; i <= 4 && i < num_workers; ++i) {
    size_t other_worker = (worker_idx + i) % num_workers;
    clio::run::ScopedCoMutex lock(worker_locks_[other_worker]);
    if (worker_maps_[other_worker].AllocateBlock(block_type, block, io_size)) {
      return true;
    }
  }

  return false;
}

bool GlobalBlockMap::FreeBlock(int worker, Block &block) {
  if (worker < 0 || static_cast<size_t>(worker) >= worker_maps_.size()) {
    return false;
  }

  size_t worker_idx = static_cast<size_t>(worker);

  // Free on this worker's map (with lock for thread safety)
  clio::run::ScopedCoMutex lock(worker_locks_[worker_idx]);
  worker_maps_[worker_idx].FreeBlock(block);
  return true;
}

//===========================================================================
// Heap Implementation
//===========================================================================

Heap::Heap() : heap_(0), total_size_(0), alignment_(4096) {}

void Heap::Init(clio::run::u64 total_size, clio::run::u32 alignment) {
  total_size_ = total_size;
  alignment_ = (alignment == 0) ? 4096 : alignment;
  heap_.store(0);
}

void Heap::InitFromLive(const std::vector<LiveBlock> &live, clio::run::u64 total_size,
                        clio::run::u32 alignment) {
  total_size_ = total_size;
  alignment_ = (alignment == 0) ? 4096 : alignment;
  // Bump high-water = max(offset+size) over all live blocks (0 if none). Any
  // new heap allocation starts past every recovered block, so AllocateBlock
  // via the heap can never overlap a live offset. The gaps below the bump are
  // handed to the free list by GlobalBlockMap::SeedFreeRange (see
  // InitializeAllocatorFromLive).
  clio::run::u64 bump = 0;
  for (const auto &b : live) {
    clio::run::u64 end = b.offset + b.size;
    if (end > bump) {
      bump = end;
    }
  }
  heap_.store(bump);
}

bool Heap::Allocate(size_t block_size, int block_type, Block &block) {
  // Align the requested block size to alignment boundary for O_DIRECT I/O
  // Formula: aligned_size = ((block_size + alignment_ - 1) / alignment_) *
  // alignment_
  clio::run::u32 alignment = (alignment_ == 0) ? 4096 : alignment_;

  // Align the requested size
  clio::run::u64 aligned_size =
      ((block_size + alignment - 1) / alignment) * alignment;
  HLOG(kDebug,
       "Allocating block: block_size = {}, alignment = {}, aligned_size = {}",
       block_size, alignment, aligned_size);

  // Atomic fetch-and-add to allocate from heap using aligned size
  clio::run::u64 old_heap = heap_.fetch_add(aligned_size);

  if (old_heap + aligned_size > total_size_) {
    // Out of space - rollback
    return false;
  }

  // Allocation successful - both offset and size are aligned
  block.offset_ = old_heap;
  block.size_ = aligned_size;
  block.block_type_ = static_cast<clio::run::u32>(block_type);
  return true;
}

clio::run::u64 Heap::GetRemainingSize() const {
  clio::run::u64 current_heap = heap_.load();
  if (current_heap >= total_size_) {
    return 0;
  }
  return total_size_ - current_heap;
}

Runtime::~Runtime() {
  // Clean up libaio (only for file-based storage)
  if (bdev_type_ == BdevType::kFile) {
    CleanupAsyncIO();
    CleanupWorkerIOContexts();
  }

  // Clean up RAM backend (vector of unique_ptr<char[]> handles itself)

  // kHbm / kPinned bdev tiers were removed — kRam/kFile handle device
  // USM source/dest pointers directly via ctp::DeviceAwareMemcpy.

  // Note: GlobalBlockMap and Heap destructors will clean up automatically
}

bool Runtime::InitializeWorkerIOContexts() {
  // Pre-allocate vector based on actual number of workers
  clio::run::WorkOrchestrator *work_orchestrator = CLIO_WORK_ORCHESTRATOR;
  size_t num_workers =
      work_orchestrator ? work_orchestrator->GetWorkerCount() : 16;
  worker_io_contexts_.resize(num_workers);
  // Contexts are lazily initialized when first accessed
  return true;
}

void Runtime::CleanupWorkerIOContexts() {
  for (auto &ctx : worker_io_contexts_) {
    ctx.Cleanup();
  }
  worker_io_contexts_.clear();
}

WorkerIOContext *Runtime::GetWorkerIOContext(size_t worker_id) {
  // Check bounds - vector is pre-allocated in InitializeWorkerIOContexts
  if (worker_id >= worker_io_contexts_.size()) {
    HLOG(kWarning, "Worker ID {} exceeds pre-allocated size {}", worker_id,
         worker_io_contexts_.size());
    return nullptr;
  }

  WorkerIOContext *ctx = &worker_io_contexts_[worker_id];

  // Lazy initialization: initialize on first access
  if (!ctx->is_initialized_) {
    if (!ctx->Init(file_path_, io_depth_, static_cast<clio::run::u32>(worker_id))) {
      HLOG(kError, "Failed to initialize I/O context for worker {}", worker_id);
      return nullptr;
    }

    // Register the eventfd with the worker's EventManager for completion notification
    int event_fd = ctx->async_io_ ? ctx->async_io_->GetEventFd() : -1;
    clio::run::Worker *worker = CLIO_CUR_WORKER;
    if (worker != nullptr && event_fd >= 0) {
      auto &em = worker->GetEventManager();
      if (em.AddEvent(event_fd) < 0) {
        HLOG(kWarning, "Failed to register eventfd with worker {} EventManager",
             worker_id);
      } else {
        HLOG(kDebug, "Registered eventfd {} with worker {} EventManager",
             event_fd, worker_id);
      }
    }
  }

  return ctx;
}

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  if (!task) return clio::run::TaskStat();
  switch (task->method_) {
    case Method::kWrite: {
      auto *wt = static_cast<const WriteTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = wt->length_;
      size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0f;
      return stat;
    }
    case Method::kRead: {
      auto *rt = static_cast<const ReadTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = rt->length_;
      size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0f;
      return stat;
    }
    default: return clio::run::TaskStat();
  }
}

size_t Runtime::GetWorkerID(clio::run::RunContext &rctx) {
  clio::run::Worker *worker = CLIO_CUR_WORKER;
  if (worker == nullptr) {
    return 0;
  }
  return worker->GetId();
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  CLIO_TASK_BODY_BEGIN

  CreateParams params = task->GetParams();
  bdev_type_ = params.bdev_type_;
  
  transport_ = BdevTransportFactory::Create(bdev_type_);
  if (!transport_) {
    if (bdev_type_ == BdevType::kNoop) {
      task->return_code_ = 0;
      CLIO_CO_RETURN;
    }
    HLOG(kError, "Failed to create bdev transport for type {}", static_cast<int>(bdev_type_));
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // pool_name doubles as the file path (kFile) / S3 bucket (kS3); it lives on
  // the create task, not in CreateParams.
  if (!transport_->Init(params, task->pool_name_.str(), this)) {
    HLOG(kError, "Failed to initialize bdev transport");
    task->return_code_ = 2;
    CLIO_CO_RETURN;
  }

  // Open the persistent allocator-state log (WAL). Empty path => disabled
  // (no file created). On recover, replay the log so we can reconstruct the
  // allocator without re-handing-out any still-live offset.
  bool recovered = false;
  if (!params.alloc_log_path_.empty()) {
    if (!alloc_log_.Open(params.alloc_log_path_, /*recover=*/true)) {
      HLOG(kWarning, "bdev: failed to open alloc log at {}, logging disabled",
           params.alloc_log_path_);
    } else {
      const std::vector<LiveBlock> &live = alloc_log_.live(/*group_id=*/0);
      if (!live.empty()) {
        InitializeAllocatorFromLive(live);
        recovered = true;
      }
    }
  }

  // Initialize the data allocator (skip if we already rebuilt from a
  // recovered live set).
  if (!recovered) {
    InitializeAllocator();
  }

  // Register a periodic task that flushes (and compacts) the WAL. Only when
  // logging is enabled. Mirrors admin/CTE's SetPeriod + TASK_PERIODIC pattern
  // via client_ (initialized in Init()).
  if (alloc_log_.enabled()) {
    constexpr double kFlushPeriodUs = 50000.0;  // 50 ms
    client_.AsyncFlushAllocLog(clio::run::PoolQuery::Local(), kFlushPeriodUs);
  }

  // UpdateTask is sent in PostGpuContainerCreate(), called after the GPU
  // container is registered so it arrives when the container is ready.

  // Initialize performance tracking
  start_time_ = std::chrono::high_resolution_clock::now();
  total_reads_ = 0;
  total_writes_ = 0;
  total_bytes_read_ = 0;
  total_bytes_written_ = 0;

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::AllocateBlocks(clio::run::shared_ptr<AllocateBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  clio::run::Worker *worker = CLIO_CUR_WORKER;
  int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId()) : 0;
  std::vector<Block> local_blocks;

  if (!transport_->AllocateBlocks(task->size_, worker_id, local_blocks)) {
    task->blocks_.clear();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Copy the local vector to the task's shared memory vector using assignment
  // operator
  // task->blocks_ = local_blocks;
  clio::run::u64 alloc_bytes = 0;
  for (size_t i = 0; i < local_blocks.size(); i++) {
    task->blocks_.push_back(local_blocks[i]);
    alloc_bytes += local_blocks[i].size_;
    // Persist the allocation in the WAL (group_id 0 = plain bdev). No-op
    // when logging is disabled.
    alloc_log_.LogAlloc(/*group_id=*/0, local_blocks[i].offset_,
                        local_blocks[i].size_, local_blocks[i].block_type_);
  }

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::FreeBlocks(clio::run::shared_ptr<FreeBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  clio::run::Worker *worker = CLIO_CUR_WORKER;
  int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId()) : 0;
  std::vector<Block> local_blocks;
  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    Block block_copy = task->blocks_[i];  // Make a copy since FreeBlock takes
                                          // non-const reference
    size_t cat_size = 0;
    int bt = FindBlockTypeForSize(static_cast<size_t>(block_copy.size_),
                                  cat_size);
    if (bt < 0) {
      // Larger than every cached class — mirror AllocateBlocks, which
      // uses the largest category for such sizes.
      bt = static_cast<int>(BlockSizeCategory::kMaxCategories) - 1;
    }
    block_copy.block_type_ = static_cast<clio::run::u32>(bt);
    freed_bytes += block_copy.size_;
    global_block_map_.FreeBlock(worker_id, block_copy);
    // Persist the free in the WAL (removes the matching live alloc by
    // (group_id, offset) on replay). No-op when logging is disabled.
    alloc_log_.LogFree(/*group_id=*/0, block_copy.offset_, block_copy.size_,
                       block_copy.block_type_);
  }
  // Reclaim live-byte accounting so GetStats' remaining recovers as
  // blocks are reused (pairs with AllocateBlocks' fetch_add). Guard the
  // subtraction so a double-free / mismatched free can't underflow the
  // unsigned counter.
  {
    clio::run::u64 cur = allocated_bytes_.load(std::memory_order_relaxed);
    clio::run::u64 dec = std::min(cur, freed_bytes);
    allocated_bytes_.fetch_sub(dec, std::memory_order_relaxed);
  }

  transport_->FreeBlocks(worker_id, local_blocks);

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Write(clio::run::shared_ptr<WriteTask> &task) {
  CLIO_TASK_BODY_BEGIN
  switch (bdev_type_) {
    case BdevType::kFile:
      CLIO_CO_AWAIT(WriteToFile(task, rctx));
      break;
    case BdevType::kRam:
      WriteToRam(task);
      break;
    case BdevType::kHbm:
    case BdevType::kPinned:
      // Removed tiers; reject as unsupported.
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_written_ = 0;
      break;
    case BdevType::kNoop:
      task->return_code_ = 0;
      task->bytes_written_ = task->length_;
      break;
    default:
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_written_ = 0;
      break;
  }

  if (transport_) {
    CLIO_CO_AWAIT(transport_->WriteBlocks(ctp::ipc::FullPtr<WriteTask>(task.get())));
    total_writes_.fetch_add(1);
    total_bytes_written_.fetch_add(task->bytes_written_);
  } else {
    task->return_code_ = 1;
    task->bytes_written_ = 0;
  }

  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Read(clio::run::shared_ptr<ReadTask> &task) {
  CLIO_TASK_BODY_BEGIN
  switch (bdev_type_) {
    case BdevType::kFile:
      CLIO_CO_AWAIT(ReadFromFile(task, rctx));
      break;
    case BdevType::kRam:
      ReadFromRam(task);
      break;
    case BdevType::kHbm:
    case BdevType::kPinned:
      // Removed tiers; reject as unsupported.
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_read_ = 0;
      break;
    case BdevType::kNoop:
      task->return_code_ = 0;
      task->bytes_read_ = task->length_;
      break;
    default:
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_read_ = 0;
      break;
  }

  if (transport_) {
    CLIO_CO_AWAIT(transport_->ReadBlocks(ctp::ipc::FullPtr<ReadTask>(task.get())));
    total_reads_.fetch_add(1);
    total_bytes_read_.fetch_add(task->bytes_read_);
  } else {
    task->return_code_ = 1;
    task->bytes_read_ = 0;
  }

  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Update(clio::run::shared_ptr<UpdateTask> &task) {
  CLIO_TASK_BODY_BEGIN
  size_t worker_id = GetWorkerID(rctx);
  WorkerIOContext *io_ctx = GetWorkerIOContext(worker_id);

  auto *ipc_mgr = CLIO_IPC;
  ctp::ipc::FullPtr<char> data_ptr = ipc_mgr->ToFullPtr(task->data_).Cast<char>();

  // libaio / POSIX-AIO can't dereference device USM, so stage through
  // a host buffer when the data lives on device. The host staging
  // buffer is sized to the largest single-block write — typical CTE
  // PutBlob is one block, but worst-case loop bound is task->length_.
  bool data_on_device = ctp::IsDevicePointer(data_ptr.ptr_);
  std::vector<char> staging;
  if (data_on_device) {
    staging.resize(task->length_);
    ctp::DeviceAwareMemcpy(staging.data(), data_ptr.ptr_, task->length_);
  }

  clio::run::u64 total_bytes_written = 0;
  clio::run::u64 data_offset = 0;

  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    const Block &block = task->blocks_[i];

    clio::run::u64 remaining = task->length_ - total_bytes_written;
    if (remaining == 0) break;
    clio::run::u64 block_write_size = std::min(remaining, block.size_);

    void *block_data = data_on_device
                           ? static_cast<void *>(staging.data() + data_offset)
                           : static_cast<void *>(data_ptr.ptr_ + data_offset);

    if (io_ctx == nullptr || !io_ctx->is_initialized_ || !io_ctx->async_io_) {
      HLOG(kError, "WriteToFile called with invalid I/O context");
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_written_ = total_bytes_written;
      CLIO_CO_RETURN;
    }

    ctp::IoToken token = io_ctx->async_io_->Write(
        block_data, static_cast<size_t>(block_write_size),
        static_cast<off_t>(block.offset_));
    if (token == ctp::kInvalidIoToken) {
      HLOG(kError, "Failed to submit async write: offset={}, size={}",
           block.offset_, block_write_size);
      task->return_code_ = 2;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_written_ = total_bytes_written;
      CLIO_CO_RETURN;
    }

    ctp::IoResult result;
    while (!io_ctx->async_io_->IsComplete(token, result)) {
      CLIO_CO_AWAIT(clio::run::yield(10.0));
    }

    if (result.error_code != 0) {
      HLOG(kError, "Async write failed: error_code={}", result.error_code);
      task->return_code_ = 4;
      task->io_error_ =
          static_cast<clio::run::u32>(ctp::ClassifyErrno(result.error_code));
      task->bytes_written_ = total_bytes_written;
      CLIO_CO_RETURN;
    }

    clio::run::u64 actual_bytes = std::min(
        static_cast<clio::run::u64>(result.bytes_transferred), block_write_size);
    total_bytes_written += actual_bytes;
    data_offset += actual_bytes;
  }

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::GetStats(clio::run::shared_ptr<GetStatsTask> &task) {
  CLIO_TASK_BODY_BEGIN
  size_t worker_id = GetWorkerID(rctx);
  WorkerIOContext *io_ctx = GetWorkerIOContext(worker_id);

  auto *ipc_mgr = CLIO_IPC;
  ctp::ipc::FullPtr<char> data_ptr = ipc_mgr->ToFullPtr(task->data_).Cast<char>();

  // libaio / POSIX-AIO can't write into device USM. When the dest is
  // device, allocate a host staging buffer, AIO into it, then
  // DeviceAwareMemcpy to the device dest at the end.
  bool data_on_device = ctp::IsDevicePointer(data_ptr.ptr_);
  std::vector<char> staging;
  if (data_on_device) {
    staging.resize(task->length_);
  }

  clio::run::u64 total_bytes_read = 0;
  clio::run::u64 data_offset = 0;

  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    const Block &block = task->blocks_[i];

    clio::run::u64 remaining = task->length_ - total_bytes_read;
    if (remaining == 0) break;
    clio::run::u64 block_read_size = std::min(remaining, block.size_);

    void *block_data = data_on_device
                           ? static_cast<void *>(staging.data() + data_offset)
                           : static_cast<void *>(data_ptr.ptr_ + data_offset);

    if (io_ctx == nullptr || !io_ctx->is_initialized_ || !io_ctx->async_io_) {
      HLOG(kError, "ReadFromFile called with invalid I/O context");
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_read_ = total_bytes_read;
      CLIO_CO_RETURN;
    }

    ctp::IoToken token = io_ctx->async_io_->Read(
        block_data, static_cast<size_t>(block_read_size),
        static_cast<off_t>(block.offset_));
    if (token == ctp::kInvalidIoToken) {
      HLOG(kError, "Failed to submit async read: offset={}, size={}",
           block.offset_, block_read_size);
      task->return_code_ = 2;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_read_ = total_bytes_read;
      CLIO_CO_RETURN;
    }

    ctp::IoResult result;
    while (!io_ctx->async_io_->IsComplete(token, result)) {
      CLIO_CO_AWAIT(clio::run::yield(10.0));
    }

    if (result.error_code != 0) {
      HLOG(kError, "Async read failed: error_code={}", result.error_code);
      task->return_code_ = 4;
      task->io_error_ =
          static_cast<clio::run::u32>(ctp::ClassifyErrno(result.error_code));
      task->bytes_read_ = total_bytes_read;
      CLIO_CO_RETURN;
    }

    clio::run::u64 actual_bytes = std::min(
        static_cast<clio::run::u64>(result.bytes_transferred), block_read_size);
    total_bytes_read += actual_bytes;
    data_offset += actual_bytes;
  }

  // If we staged through a host buffer, push the freshly-read bytes
  // out to the device-USM destination. (No-op when data is on host.)
  if (data_on_device && total_bytes_read > 0) {
    ctp::DeviceAwareMemcpy(data_ptr.ptr_, staging.data(), total_bytes_read);
  }

  task->return_code_ = 0;
  task->bytes_read_ = total_bytes_read;
  total_reads_.fetch_add(1);
  total_bytes_read_.fetch_add(total_bytes_read);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Update(ctp::ipc::FullPtr<UpdateTask> task,
                                clio::run::RunContext &ctx) {
  // UpdateTask is meant for the GPU container only.
  // The CPU runtime receives it as a no-op.
  task->return_code_ = 0;
  (void)ctx;
  co_return;
}


clio::run::TaskResume Runtime::FlushAllocLog(ctp::ipc::FullPtr<FlushAllocLogTask> task,
                                       clio::run::RunContext &ctx) {
#ifdef __NVCOMPILER
  clio::run::RunContext& rctx = ctx;
#else
  (void)ctx;
#endif
  CLIO_TASK_BODY_BEGIN
  // Append buffered records to disk (also folds them into the in-memory
  // recovered model). Idempotent when the buffer is empty.
  alloc_log_.Flush();
  // Compact when the on-disk record count has grown past the threshold:
  // max(kMinCompactRecords, live * kCompactGrowthFactor). Compaction rewrites
  // the log down to one record per live block, bounding the file size.
  clio::run::u64 live = alloc_log_.live_block_count();
  clio::run::u64 on_disk = alloc_log_.records_on_disk();
  clio::run::u64 threshold = std::max<clio::run::u64>(kMinCompactRecords,
                                          live * kCompactGrowthFactor);
  if (on_disk > threshold) {
    alloc_log_.Compact();
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::GetStats(ctp::ipc::FullPtr<GetStatsTask> task,
                                  clio::run::RunContext &ctx) {
#ifdef __NVCOMPILER
  clio::run::RunContext& rctx = ctx;
#else
  (void)ctx;
#endif
  CLIO_TASK_BODY_BEGIN
  // Predict wall time from learned model using a synthetic 1 MiB R/W
  // task as the reference size for the bandwidth/latency estimate.
  ReadTask r_synthetic;
  r_synthetic.method_ = Method::kRead;
  r_synthetic.length_ = 1024 * 1024;
  WriteTask w_synthetic;
  w_synthetic.method_ = Method::kWrite;
  w_synthetic.length_ = 1024 * 1024;
  clio::run::TaskStat read_stat = GetTaskStats(&r_synthetic);
  clio::run::TaskStat write_stat = GetTaskStats(&w_synthetic);
  float read_wall_us = InferWallClockTime(Method::kRead, read_stat);
  float write_wall_us = InferWallClockTime(Method::kWrite, write_stat);
  double read_size_mb = static_cast<double>(read_stat.io_size_) / (1024.0 * 1024.0);
  double write_size_mb = static_cast<double>(write_stat.io_size_) / (1024.0 * 1024.0);
  task->metrics_.read_bandwidth_mbps_ = (read_wall_us > 0)
      ? read_size_mb / (read_wall_us * 1e-6) : perf_metrics_.read_bandwidth_mbps_;
  task->metrics_.write_bandwidth_mbps_ = (write_wall_us > 0)
      ? write_size_mb / (write_wall_us * 1e-6) : perf_metrics_.write_bandwidth_mbps_;
  task->metrics_.read_latency_us_ = read_wall_us;
  task->metrics_.write_latency_us_ = write_wall_us;
  task->metrics_.iops_ = perf_metrics_.iops_;

  if (transport_) {
    task->remaining_size_ = transport_->GetRemainingSize();
  } else {
    task->remaining_size_ = 0;
  }

  // Persist any buffered allocator-state records before teardown so a clean
  // pool destroy leaves a recoverable log on disk.
  alloc_log_.Flush();

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

void Runtime::InitializeAllocator() {
  // Initialize global block map with actual number of workers
  clio::run::WorkOrchestrator *work_orchestrator = CLIO_WORK_ORCHESTRATOR;
  size_t num_workers =
      work_orchestrator ? work_orchestrator->GetWorkerCount() : 16;
  global_block_map_.Init(num_workers);

  // Initialize heap with total file size and alignment requirement
  heap_.Init(file_size_, alignment_);
}

void Runtime::InitializeAllocatorFromLive(const std::vector<LiveBlock> &live) {
  // Initialize global block map with actual number of workers
  clio::run::WorkOrchestrator *work_orchestrator = CLIO_WORK_ORCHESTRATOR;
  size_t num_workers =
      work_orchestrator ? work_orchestrator->GetWorkerCount() : 16;
  global_block_map_.Init(num_workers);

  // Set the heap bump past every recovered live block so the heap never
  // re-hands a live offset.
  heap_.InitFromLive(live, file_size_, alignment_);

  // Sort live by offset and push the gaps in [0, bump) not covered by any
  // live block into the free list so freed gaps are reusable. After this the
  // allocator is equivalent to one that had allocated exactly `live`.
  std::vector<LiveBlock> sorted = live;
  std::sort(sorted.begin(), sorted.end(),
            [](const LiveBlock &a, const LiveBlock &b) {
              return a.offset < b.offset;
            });
  clio::run::u64 cursor = 0;
  for (const auto &b : sorted) {
    if (b.offset > cursor) {
      global_block_map_.SeedFreeRange(cursor, b.offset - cursor);
    }
    clio::run::u64 end = b.offset + b.size;
    if (end > cursor) {
      cursor = end;
    }
  }
  HLOG(kInfo,
       "bdev: recovered allocator from {} live blocks, heap bump at {}",
       live.size(), cursor);
}

size_t Runtime::GetBlockSize(int block_type) {
  if (block_type >= 0 &&
      block_type < static_cast<int>(BlockSizeCategory::kMaxCategories)) {
    return kBlockSizes[block_type];
  }
  return 0;
}

size_t Runtime::GetWorkerID(clio::run::RunContext &ctx) {
  // Get current worker from thread-local storage using CLIO_CUR_WORKER macro
  clio::run::Worker *worker = CLIO_CUR_WORKER;
  if (worker == nullptr) {
    return 0;  // Fallback to worker 0 if not in worker context
  }
  return worker->GetId();
}


clio::run::u64 Runtime::AlignSize(clio::run::u64 size) {
  if (alignment_ == 0) {
    alignment_ = 4096;  // Set to default if somehow it's 0
  }
  return ((size + alignment_ - 1) / alignment_) * alignment_;
}

void Runtime::UpdatePerformanceMetrics(bool is_write, clio::run::u64 bytes,
                                       double duration_us) {
  // This is a simplified implementation
  // In a real implementation, you'd maintain running averages or histograms
}

void Runtime::InitializeAsyncIO() {
  // No initialization needed for POSIX AIO fallback
}

void Runtime::CleanupAsyncIO() {
  // No cleanup needed for POSIX AIO fallback
}

char* Runtime::EnsureRamPage(size_t page_idx) {
  std::lock_guard<std::mutex> lock(ram_pages_mu_);
  if (page_idx >= ram_pages_.size()) {
    ram_pages_.resize(page_idx + 1);
  }
  if (!ram_pages_[page_idx]) {
    // Lazy alloc for pages beyond the eagerly-allocated range (or for
    // unbounded-capacity bdevs). The default-init `new char[]` reserves
    // virtual address space; physical pages fault in on first touch by
    // WriteToRam's DeviceAwareMemcpy. We do NOT memset here — that would
    // double the memory traffic (one pass to zero-fault the page, a
    // second pass to memcpy the user's data), capping Put bandwidth at
    // half of memory bandwidth. The bounded-capacity path in Create()
    // pre-allocates and pre-faults so the cost lands outside any
    // benchmark loop.
    ram_pages_[page_idx].reset(new char[kRamPageSize]);
  }
  return ram_pages_[page_idx].get();
}

char* Runtime::GetRamPage(size_t page_idx) const {
  std::lock_guard<std::mutex> lock(ram_pages_mu_);
  if (page_idx >= ram_pages_.size()) return nullptr;
  return ram_pages_[page_idx].get();
}

void Runtime::WriteToRam(ctp::ipc::FullPtr<WriteTask> task) {
  auto *ipc_mgr = CLIO_IPC;
  ctp::ipc::FullPtr<char> data_ptr = ipc_mgr->ToFullPtr(task->data_).Cast<char>();

  clio::run::u64 total_bytes_written = 0;
  clio::run::u64 data_offset = 0;

  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    const Block &block = task->blocks_[i];

    clio::run::u64 remaining = task->length_ - total_bytes_written;
    if (remaining == 0) break;
    clio::run::u64 block_write_size = std::min(remaining, block.size_);

    if (ram_capacity_ != std::numeric_limits<clio::run::u64>::max() &&
        block.offset_ + block_write_size > ram_capacity_) {
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_written_ = total_bytes_written;
      HLOG(kError,
           "Write to RAM beyond capacity offset: {}, length: {}, "
           "ram_capacity: {}",
           block.offset_, block_write_size, ram_capacity_);
      return;
    }

    // Walk the (offset, size) range across 1 GiB pages, allocating a page
    // on first write only. Bench-sized writes (≤ block size, typically MBs)
    // touch one page; this loop only runs >1 iteration when a block straddles
    // a 1 GiB boundary. DeviceAwareMemcpy dispatches through
    // sycl::queue::memcpy (or the CUDA equivalent) when the data ShmPtr
    // resolves to device USM, and falls back to std::memcpy otherwise.
    clio::run::u64 cur_off = block.offset_;
    clio::run::u64 left = block_write_size;
    while (left > 0) {
      size_t page_idx = static_cast<size_t>(cur_off / kRamPageSize);
      clio::run::u64 intra = cur_off % kRamPageSize;
      clio::run::u64 chunk = std::min<clio::run::u64>(left, kRamPageSize - intra);
      char* page = EnsureRamPage(page_idx);
      ctp::DeviceAwareMemcpy(page + intra,
                             data_ptr.ptr_ + data_offset,
                             chunk);
      cur_off += chunk;
      data_offset += chunk;
      left -= chunk;
    }

    total_bytes_written += block_write_size;
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

void Runtime::ReadFromRam(ctp::ipc::FullPtr<ReadTask> task) {
  auto *ipc_mgr = CLIO_IPC;
  ctp::ipc::FullPtr<char> data_ptr = ipc_mgr->ToFullPtr(task->data_).Cast<char>();

  clio::run::u64 total_bytes_read = 0;
  clio::run::u64 data_offset = 0;

  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    const Block &block = task->blocks_[i];

    clio::run::u64 remaining = task->length_ - total_bytes_read;
    if (remaining == 0) break;
    clio::run::u64 block_read_size = std::min(remaining, block.size_);

    if (ram_capacity_ != std::numeric_limits<clio::run::u64>::max() &&
        block.offset_ + block_read_size > ram_capacity_) {
      task->return_code_ = 1;
      task->io_error_ = static_cast<clio::run::u32>(ctp::IoError::kInvalid);
      task->bytes_read_ = total_bytes_read;
      HLOG(kError,
           "Read from RAM beyond capacity offset: {}, length: {}, "
           "ram_capacity: {}",
           block.offset_, block_read_size, ram_capacity_);
      return;
    }

    // Sparse semantics: a never-written page reads back as zeros, mirroring
    // a sparse file. DeviceAwareMemcpy handles device-USM data buffers
    // (see WriteToRam comment); for the zero-fill branch we copy from a
    // static zero scratch when the dest is device-resident, since plain
    // memset on a device USM pointer would segfault on the host.
    clio::run::u64 cur_off = block.offset_;
    clio::run::u64 left = block_read_size;
    while (left > 0) {
      size_t page_idx = static_cast<size_t>(cur_off / kRamPageSize);
      clio::run::u64 intra = cur_off % kRamPageSize;
      clio::run::u64 chunk = std::min<clio::run::u64>(left, kRamPageSize - intra);
      char* page = GetRamPage(page_idx);
      char *dst = data_ptr.ptr_ + data_offset;
      if (page) {
        ctp::DeviceAwareMemcpy(dst, page + intra, chunk);
      } else if (ctp::IsDevicePointer(dst)) {
        static const char kZeroScratch[4096] = {};
        clio::run::u64 z_left = chunk;
        clio::run::u64 z_off = 0;
        while (z_left > 0) {
          clio::run::u64 z_chunk =
              std::min<clio::run::u64>(z_left, sizeof(kZeroScratch));
          ctp::DeviceAwareMemcpy(dst + z_off, kZeroScratch, z_chunk);
          z_off += z_chunk;
          z_left -= z_chunk;
        }
      } else {
        memset(dst, 0, chunk);
      }
      cur_off += chunk;
      data_offset += chunk;
      left -= chunk;
    }

    total_bytes_read += block_read_size;
  }

  task->return_code_ = 0;
  task->bytes_read_ = total_bytes_read;

  total_reads_.fetch_add(1);
  total_bytes_read_.fetch_add(total_bytes_read);
}

// VIRTUAL METHOD IMPLEMENTATIONS (now in autogen/bdev_lib_exec.cc)

clio::run::u64 Runtime::GetWorkRemaining() const { return 0; }

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (task->query_ == "stats") {
    ReadTask r_synthetic;
    r_synthetic.method_ = Method::kRead;
    r_synthetic.length_ = 1024 * 1024;
    WriteTask w_synthetic;
    w_synthetic.method_ = Method::kWrite;
    w_synthetic.length_ = 1024 * 1024;
    clio::run::TaskStat read_stat = GetTaskStats(&r_synthetic);
    clio::run::TaskStat write_stat = GetTaskStats(&w_synthetic);
    float read_wall_us = InferWallClockTime(Method::kRead, read_stat);
    float write_wall_us = InferWallClockTime(Method::kWrite, write_stat);
    double read_size_mb = static_cast<double>(read_stat.io_size_) / (1024.0 * 1024.0);
    double write_size_mb = static_cast<double>(write_stat.io_size_) / (1024.0 * 1024.0);
    double read_bw = (read_wall_us > 0)
        ? read_size_mb / (read_wall_us * 1e-6) : perf_metrics_.read_bandwidth_mbps_;
    double write_bw = (write_wall_us > 0)
        ? write_size_mb / (write_wall_us * 1e-6) : perf_metrics_.write_bandwidth_mbps_;

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    pk.pack_map(14);
    pk.pack("pool_name");              pk.pack(pool_name_);
    pk.pack("bdev_type");              pk.pack(static_cast<clio::run::u32>(bdev_type_));
    pk.pack("total_capacity");         pk.pack(transport_ ? transport_->GetCapacity() : 0);
    pk.pack("remaining_capacity");     pk.pack(transport_ ? transport_->GetRemainingSize() : 0);
    pk.pack("read_bandwidth_mbps");    pk.pack(read_bw);
    pk.pack("write_bandwidth_mbps");   pk.pack(write_bw);
    pk.pack("read_latency_us");        pk.pack(static_cast<double>(read_wall_us));
    pk.pack("write_latency_us");       pk.pack(static_cast<double>(write_wall_us));
    pk.pack("iops");                   pk.pack(perf_metrics_.iops_);
    pk.pack("total_reads");            pk.pack(total_reads_.load());
    pk.pack("total_writes");           pk.pack(total_writes_.load());
    pk.pack("total_bytes_read");       pk.pack(total_bytes_read_.load());
    pk.pack("total_bytes_written");    pk.pack(total_bytes_written_.load());
    pk.pack("device_health");          pk.pack(ctp::SystemInfo::GetDeviceHealthStats(pool_name_));

    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

void Runtime::PostGpuContainerCreate() {}

}  // namespace clio::run::bdev

// Define ChiMod entry points using CLIO_TASK_CC macro
CLIO_TASK_CC(clio::run::bdev::Runtime)
