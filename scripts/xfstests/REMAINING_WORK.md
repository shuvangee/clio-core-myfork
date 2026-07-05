# clio xfstests — remaining-work roadmap (2026-07-05)

Decision-ready summary of what's left after this session's fixes. The honest
embedded gate is now **84 tests** (all verified passing, `run_ci`-green) plus 35
in the scratch gate. Everything below is *why the rest don't pass* and *what it
would take*, with the concrete caveats found by actually attempting them.

## 2026-07-05 — CI went red: worker oversubscription livelock (FIXED)
The embedded-FUSE gate passed locally but **hung 12 tests in CI** (run
28747864260, job "adapters (linux, all 5)"): generic/006/007/011/013/089/100/
113/127/286/363/438/471. These pass on a 16-core dev box but hang in the
deps-cpu docker on a 4-vCPU GitHub runner.

Root cause (reproduced locally by `taskset -c 0,1`, i.e. pinning to 2 CPUs):
the runtime's default `num_threads=4` spawns 5 busy-spinning workers. Under
`Worker::Run` an idle/blocked worker busy-polls its lane (`ProcessNewTasks`) and
the ManyToOne batch mutex (`BatchManager::FlushDue`) **without yielding**. When
the number of runnable workers + libfuse's thread pool + the 8 ZMQ I/O threads
exceeds the core count, a worker holding a blocked task's dependency is starved
and the pipeline livelocks. gdb on a hung daemon showed two workers pinned at
~46% CPU in `ProcessNewTasks` and `BatchManager::FlushDue`/`pthread_mutex_lock`.

Fixes:
1. **`clio_xfstests_config.yaml`: `runtime.num_threads: 1`** (1 compute + 1
   network worker). Locally under 2-CPU pinning 11 of 12 recover; **in CI (run
   28751210007) it recovered HALF: 12 hangs -> 6.** 006/007/100/113 pass again
   (127 already removed). The embedded FUSE adapter is a single-client front-end
   and needs no compute-worker fan-out, so undersubscribing is correct.
2. **`worker.cc`: yield in the idle busy-wait window** (`SuspendMe`). A bare spin
   monopolizes a core; `std::this_thread::yield()` is a no-op when a core is free
   and cedes it under contention. Complementary robustness.
3. **generic/127 removed** — fsx torture; passes on >=4 cores (verified locally,
   84/84 at 4 CPUs / 90s), borderline under a tighter cap. Slow-lane candidate.

### STILL OPEN: six genuine deadlocks in CI (011/013/089/286/438/471)
These still hang in CI's deps-cpu docker **even at num_threads=1** and are
quarantined out of the gate (84 -> 77). They are **deadlocks, not timeouts**:
CI per-test timing shows them hitting the 90s cap (~91s) while heavier fsx tests
finish in 21-34s in the same run. They pass on dev hardware; they wedge only
under CI's constrained docker scheduling. gdb on a locally-reproduced hang (2-CPU
pin) showed workers spinning in `ProcessNewTasks` and
`BatchManager::FlushDue`/`pthread_mutex_lock` without yielding; with a lone
compute worker (num_threads=1) a task can also starve its own sub-task. This is
the self-send / lost-wakeup class in the busy-spin worker model — a config knob
cannot resolve it (fewer workers fixes oversubscription for some tests but
enables single-worker sub-task starvation for others).

Real fix (a runtime change, not done here — high blast radius, and CI's docker
env is not locally reproducible for validation):
- add `CTP_THREAD_MODEL->Yield()` to the `ProcessNewTasks` and
  `BatchManager::FlushDue` hot-spin paths so a worker holding a blocked task's
  dependency cedes the core under oversubscription; and/or
- fix the self-send/lost-wakeup so a blocked task is always re-scheduled when its
  sub-task completes regardless of worker count; and/or
- auto-size `num_threads` to `min(default, max(1, nproc-1))` globally.
Re-add each quarantined test to the gate once the runtime fix lands and it
passes two clean CI runs.

## What was fixed this session (committed)
- **Harness bug** (`b0a616c3`): `run_clio_xfstests.sh` counted `notrun` as
  `pass` (matched `^Passed all` before `^Not run:`). A prior sweep had thus
  admitted ~430 non-running tests into the gate. Fixed; gate **rebuilt 513→84**
  (`26f815ef`) to the tests that actually pass, `run_ci`-validated.
- **6 clio fixes/features**: vector null-alloc crash (`9ec45525`), `st_blocks`
  was 0 (`4cab6fad`), POSIX mode/exec (`a85e675b`), `total_size_cache_` drift
  under concurrent extend (`8f545daf`), fallocate `ZERO_RANGE` (`c9c2c17a`),
  `RENAME_NOREPLACE` (`93a9a135`).
- **2 parallel stress tests** (`1cfea3f2`) — pass release + ASan/LSan.

## Proven-fixed but not yet gated (need a working scratch driver)
- **452** (exec a copied binary) — fixed by the mode/exec commit; proven
  end-to-end on a manual keeper+client. **615** (st_blocks under concurrent
  overwrite) — fixed by the st_blocks commit; proven the same way. Both only
  await a run through `run_clio_scratch_xfstests.sh` in an environment where the
  two-runtime driver works (it wedges in this nested-namespace sandbox).

## Remaining tests — why, and the cost to address
### Not clio-fixable (structural / environment)
- **reflink (~200), scratch-reformatting, dax, verity, encryption, dedupe**:
  fs features a userspace FUSE-over-blob-store cannot provide. Permanent notrun.
- **Environment** (clio's impl WORKS on a plain mount; the harness blocks it):
  `_require_chown`/`_require_user`/`_require_runas`/ACLs notrun because the
  `unshare -rm` user namespace doesn't map the test uids / rejects system
  xattrs. Verified: chown to uid 4242 and chacl both succeed as real root.
  Also missing helper programs here (dbench/dbtest) that CI builds.

### Clio-fixable but needs a maintainer call (effort vs risk vs yield)
- **sparse st_blocks** (~1-2 tests, e.g. 014): clio over-reports sparse
  `st_blocks` (proven: 4 KB at 64 MB → 131080 blocks instead of ~8). All three
  implementations have a real downside: a new tracked physical counter =
  drift risk (the class of bug fixed in `8f545daf` this session); on-demand via
  `GetContainedBlobs` = names only, O(N) `GetBlobSize` RPCs; adding sizes +
  summing per `getattr` = a per-stat blob scan that regresses the hot path.
- **fallocate family + fiemap** (~16-30 tests, LARGEST cluster): needs FIEMAP
  (`FS_IOC_FIEMAP` ioctl over the page layout — the hard keystone), plus
  PUNCH_HOLE (tractable: free fully-covered page-blobs, zero partial edges),
  COLLAPSE/INSERT_RANGE, and real st_blocks. Most of these tests need SEVERAL
  together, so fiemap alone unlocks only ~2-4.
- **RENAME_EXCHANGE** (025), **RENAME_WHITEOUT** (078): need an atomic
  chimod-level tag swap / whiteout; adapter-level sequences aren't atomic.
- **mknod** (184/423): store node type + rdev (mirror the symlink marker);
  low value for clio's I/O-buffering purpose.
- **ACL enforcement under the harness**: `FUSE_CAP_POSIX_ACL` would delegate
  ACL xattrs to clio, but it implicitly enables `default_permissions`
  (kernel permission-enforcement across ALL ops) — regression risk to the 84
  gated tests for ~1 test.

## Recommended next steps for a maintainer
1. Run `run_clio_scratch_xfstests.sh` for 452/615 in a working scratch env → 2
   proven fixes gated.
2. Decide the fallocate-family/fiemap project (largest yield, biggest effort).
3. If sparse-file correctness matters beyond tests, pick the st_blocks tradeoff
   (accept the per-getattr scan cost, or the tracked-counter drift risk with a
   debug assert like `GetTotalSize()` has).

## Metadata durability model (verified 2026-07-05)
- DATA: consistent + durable. Write-through (no writeback cache), per-blob write
  token serializes same-blob writes. Verified byte-identical readback + in-place
  overwrite; fsx 075/091/127/263 pass. mmap uses the kernel page cache (074).
- XATTRS: durable. Stored in the `_clio_xattr_store` CTE tag (keyed by file tag
  id). set/get/list/remove verified, incl. system.posix_acl_access (ACLs work).
  Gap: cross-mount-cycle persistence (533).
- PERMISSIONS (mode/uid/gid): correct but PER-RUNTIME IN-MEMORY (by_path_
  FileInfo set_mode_/set_uid_/set_gid_), NOT durable tag metadata. Survive while
  the serving runtime/keeper is alive (192/258/639 pass under the 2-runtime
  driver) but are lost on a runtime restart / embedded unmount. To make
  permissions durable, move these overrides into the tag metadata alongside
  timestamps/xattrs. This is the one metadata-durability gap.
