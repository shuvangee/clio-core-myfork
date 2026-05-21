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

#ifndef CLIO_RUNTIME_INCLUDE_CLIO_RUNTIME_CLIO_RUNTIME_H_
#define CLIO_RUNTIME_INCLUDE_CLIO_RUNTIME_CLIO_RUNTIME_H_

/**
 * Main header file for CLIO Runtime distributed task execution framework
 *
 * This header provides the primary interface for both runtime and client
 * applications using the CLIO Runtime framework.
 */

#include "clio_runtime/pool_query.h"
#include "clio_runtime/singletons.h"
#include "clio_runtime/task.h"
#include "clio_runtime/task_archives.h"
#include "clio_runtime/types.h"
#include "clio_runtime/worker.h"

namespace chi {



/**
 * CLIO Runtime initialization mode
 */
enum class ChimaeraMode {
  kClient,   /**< Client mode - connects to existing runtime */
  kServer,   /**< Server mode - starts runtime components */
  kRuntime = kServer  /**< Alias for kServer */
};

/**
 * Global initialization functions
 */

/**
 * Initialize CLIO Runtime with specified mode
 *
 * @param mode Initialization mode (kClient or kServer/kRuntime)
 * @param default_with_runtime Default behavior if CHI_WITH_RUNTIME env var not set
 *        If true, will start runtime in addition to client initialization
 *        If false, will only initialize client components
 * @param is_restart If true, force restart_=true on compose pools and replay WAL
 *        after compose to recover address table state from before the crash
 * @return true if initialization successful, false otherwise
 *
 * Environment variable:
 *   CHI_WITH_RUNTIME=1 - Start runtime regardless of mode
 *   CHI_WITH_RUNTIME=0 - Don't start runtime (client only)
 *   If not set, uses default_with_runtime parameter
 */
bool CHIMAERA_INIT(ChimaeraMode mode, bool default_with_runtime = false,
                   bool is_restart = false);

/**
 * Finalize CLIO Runtime and release all resources
 *
 * Calls ClientFinalize on the CLIO Runtime manager to close ZMQ sockets and
 * join background threads. Must be called before process exit to avoid
 * hangs in zmq_ctx_destroy (the CLIO Runtime singleton is heap-allocated so
 * its destructor is never invoked automatically).
 */
void CHIMAERA_FINALIZE();

}  // namespace chi

//==============================================================================
// CLIO_* runtime API surface (rebranding: chimaera -> clio_runtime).
//
// All CLIO_* singleton / class-body / allocator macros are now defined at
// their canonical site in the individual subsystem headers (admin.h,
// chimaera_manager.h, config_manager.h, container.h, ipc_manager.h,
// module_manager.h, pool_manager.h, task.h, types.h, work_orchestrator.h,
// worker.h). Each of those headers also declares its `#define CHI_<X>
// CLIO_<X>` backward-compat alias, so legacy code that still uses the
// CHI_* spelling keeps working unchanged. See rebranding.md for the full
// migration table.
//
// Only two things need to live in this umbrella:
//   1. The init / finalize macros (their RHS is a function call, not a
//      macro defined elsewhere).
//   2. The CLIO_RUNTIME_MANAGER alias — the only macro whose CLIO_ form
//      is a genuinely renamed alias rather than a flipped canonical.
//==============================================================================

// --- Init / finalize ---
#define CLIO_RUNTIME_INIT      ::chi::CHIMAERA_INIT
#define CLIO_RUNTIME_FINALIZE  ::chi::CHIMAERA_FINALIZE

// --- Renamed alias for the (canonical) CLIO_CHIMAERA_MANAGER ---
#define CLIO_RUNTIME_MANAGER   CLIO_CHIMAERA_MANAGER

// --- Module namespace alias (chimaera:: -> clio_run::) ---
// Pulled in last so the alias is visible to every TU that includes this
// umbrella, regardless of which module headers come along for the ride.
#include "clio_runtime/compat/chimaera_namespace.h"

#endif  // CLIO_RUNTIME_INCLUDE_CLIO_RUNTIME_CLIO_RUNTIME_H_
