# GPU-clean CPU write fault clusters

Experimental, default off. `KYTY_WRITE_FAULT_CLUSTER=0` keeps exact tracking;
1 uses 16 KiB aligned clusters, 2 uses 64 KiB, 3 uses 256 KiB. Live switch;
no translated shader or disk cache key changes.

The post-task trace `wolverine-ingame-20261029-155454.tracy` still contains
1.29 million PageManager::Protect events (2.902 seconds aggregate exclusive)
and 1.40 million MemoryTracker::FaultLockWait events (2.373 seconds). These
are overlapping CPU thread totals, not a prediction of frame rate improvement.
The saved-game forest/burning-car scene was verified visually.

Only RenderContext's actual CPU write fault path opts into clustering.
Explicit invalidations, read faults, and GPU-owned write faults remain exact.
For a clean fault, MemoryTracker holds the same RegionManager lock while it
finds the connected GPU-clean pages containing the fault within the aligned
cluster and marks them CPU dirty. GPU-dirty neighbours terminate expansion;
there is no readback or discard of their data. Later GPU use must upload the
conservatively dirty pages through the existing protocol. No page is left
writable after that upload; no stale value is cached across writes.

Only buffer cache state is widened. Texture cache invalidates just the actual
faulting byte; other PageManager watchers remain intact and can still cause
a later real write fault. This avoids speculative texture alias invalidation.
Dirty epochs continue to advance through ChangeState. Expansion is restricted
to one tracking region and a power-of-two cluster. Non-fault invalidation and
unsupported cluster sizes use the ordinary path.

MemoryTrackerTests cover GPU-owned boundaries, host permissions, preserved
neighbour ownership, a later CPU write/upload/GPU reacquisition, GPU-owned fault
fallback, default/invalid sizes, and a second cache's independent watcher.
Release tests passed before the game comparison. General memory tests also
continue to run. The play artifact25783993 is preserved independently.

This trades fewer exceptions/protection changes for potentially more uploaded
bytes. It requires an in-game comparison; do not promote from unit tests alone.
