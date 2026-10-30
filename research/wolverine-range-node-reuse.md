# Reusing exact dirty-range metadata

The Add fast path builds on upstream commit
[9eef2e143475b610be48d709eb0d6e61476e1f98](https://github.com/KytyPS5/KytyPS5/commit/9eef2e143475b610be48d709eb0d6e61476e1f98)
by nmzik and Majed-allababedi. It retains a containing/left-hand map node when
an addition extends or is already covered by that interval.

Local refinements also reuse a right-hand node when extending it leftward,
retain nodes when trimming either end, allocate only one new interval for a
split, and keep up to64 extracted nodes per set for later splits/insertions.
Copies copy coverage only, never node ownership; moves transfer both containers.
Clear releases retained nodes. Empty/overflowing ranges retain the previous
validation. No GPU dirty bytes, page sizes, or ownership transitions are changed.

`KYTY_RANGE_NODE_REUSE=0/1` selects legacy/new algorithms. Default0 until
explicitly selected by a candidate launcher. Both paths can be compared in the
same process; toggling them retains the same interval semantics.

Validation: independent512-byte coverage model,4000 randomized updates at each
of three address bases, legacy/new/mixed modes, containment/intersection/order,
copy/move independence. MemoryTrackerTests pass. CPU-only production-code
benchmark (500000 iterations,0/1/0/1):

| Pattern | Legacy ns/iteration | Reuse ns/iteration |
| --- | --- | --- |
| Contained addition |29.90,29.83|5.73,4.22|
| Prefix cut/re-add |61.11,57.76|19.33,19.26|
| Split/rejoin |86.36,86.62|21.57,21.16|

These are CPU operation timings, not frame-rate gains.

Protected same-process game confirmation combines async guest write readback
and range-node reuse, compared with both off, with spans2/canonical1 and native
batch/bulk/aliases enabled throughout. A17.08/17.08/17.04 versus B17.80/17.64
FPS; frame counters give A17.071/17.203/17.247 versus B17.823/17.740.
Combined improvement is about3.5–4% in this scene. This does not isolate the
range algorithm's contribution. Earlier async-only comparison already gained
about4%, so do not attribute the combined result to node reuse alone.

A separate60-second movement/attack diagnostic survived with advancing frames
and no validation/device-loss errors. Its title FPS ranged down to5 during
streaming; it is not a matched comparison or evidence of stable30 FPS.
