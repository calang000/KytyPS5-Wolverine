# Native eager control for masked resource plans

Previously `KYTY_LAZY_SRT_REFRESH=0` disabled native flat batches whenever a
GPU-use mask existed. A lazy/eager A/B then compared different batching paths.

Masked plans now prepare a separate immutable eager canonical code variant on
their owning thread when lazy refresh is disabled. It ignores the mask both
for individual reads and bulk groups, retaining strict/ordinary provenance,
control flow, failure handling and descriptor behavior. Explicit worker sessions
keep their existing classic prepared code. `KYTY_SRT_PREPARE_EAGER=1` optionally
prewarms the control and logs which shader plans prepared it.

Normal eager plans with an empty mask and normal lazy plans are unchanged.
This does not enable lazy metadata by default or change shader emission/cache
serialization. It makes a comparison valid and avoids a scalar fallback for
the masked canonical eager case.

Tests compare against an independent unmasked interpreter across1000 inputs
for straight-line and branched plans, including mixed strict/ordinary reads,
an eight-word plan with a six-word ordinary bulk group, null bases and range
failures, batch0 and batch1. Existing native and materialization suites pass.

Protected in-game A/B/A/B/A,25 samples/phase, same compiled metadata/process:
eager17.24/17.36/17.24, lazy17.48/17.40 FPS. Frame counters show eager
17.358/17.285/17.296, lazy17.545/17.499. This is about1%, not a material
performance improvement; do not promote lazy as a new play build on this evidence.
Both modes confirmed batch1/bulk1/aliases1;500 masked shader plans prewarmed
eager code. Shader validation enabled, visual scene intact, no whole-game claim.
