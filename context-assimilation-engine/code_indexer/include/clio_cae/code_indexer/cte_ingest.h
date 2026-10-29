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


#ifndef CLIO_CAE_CODE_INDEXER_CTE_INGEST_H_
#define CLIO_CAE_CODE_INDEXER_CTE_INGEST_H_

/**
 * Put source files into the CTE through the Clio FS client.
 *
 * The Code Indexer reads only from the CTE. In production Clio FS (a mount or
 * the clio:: interceptors) fills the CTE transparently as programs read and
 * write files; this helper is the explicit stand-in for that, used by tests,
 * benchmarks and the `ingest` command. It is the ONE place the Code Indexer
 * module reads local disk: it copies each file to the same path under
 * `cte_root` and the indexer never looks at the disk afterwards.
 */

#include <clio_cae/code_indexer/code_scanner.h>

#include <cstddef>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

struct IngestStats {
  size_t files = 0;
  size_t bytes = 0;
  size_t failures = 0;
};

/**
 * Copy the source files under `local_root` into Clio FS under `cte_root`
 * (e.g. "/code"). Missing parent directories are created. Files that already
 * exist are overwritten. Returns what was written and what failed.
 */
IngestStats IngestTree(const std::string &local_root,
                       const std::string &cte_root,
                       const ScanOptions &opts = {});

/**
 * Read the source files stored under `cte_root` back out of Clio FS (client
 * side, synchronous). Same extension / size / ignored-directory rules as a
 * disk scan, files sorted by path and relative to `cte_root`. Never reads the
 * disk. Empty when the CTE holds nothing there or Clio FS is unavailable.
 */
std::vector<SourceFile> ReadTreeFromCte(const std::string &cte_root,
                                        const ScanOptions &opts = {});

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CTE_INGEST_H_
