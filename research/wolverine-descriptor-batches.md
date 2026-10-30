# Descriptor-only native resource batches

Experimental `KYTY_SRT_CANONICAL_VIEWS=1` requires value aliases and native batches.
It selects a separate immutable native code variant, prepared on the owning
thread. Explicit worker sessions retain their original already-prepared code.
`KYTY_SRT_PREPARE_CANONICAL=1` prewarms the variant for live comparisons.

Two changes are coupled in this prototype:

1. Buffer view handles may share their decoded base/size when their arguments
   are identical immutable user registers, immediate bits, or the same memo
   slot in the same strict/ordinary context. Distinct memory instructions are
   never identified just because their addresses or expression shapes match.
2. Descriptor batches group four to eight constant-offset scalar buffer words
   after resolving a direct ReadConst reference. These words may be deliberately
   absent from the GPU flat payload; descriptor evaluation still needs them.
   Group output offsets refer to descriptor dwords, not flat SRT slots. Lazy
   flat slots remain untouched. Unsupported inputs retain the scalar path.

The shared helper preserves memo state, original order, bounds, null semantics,
failure and captured read proofs. It uses the appropriate clean frame. A failed
first read is not retried; short/unavailable spans fall back without re-reading
an already memoized word. Inactive descriptor sources are filtered as before by
the materializer; no new descriptors are evaluated eagerly.

Validation covers distinct register nodes, duplicate ReadConst wrappers, distinct
memory operations at the same address, strict/ordinary pointers, cold and mixed
memos, descriptor-only lazy slots, bounds/failure and exact captured proof ranges.
Four capture sets: 499 valid records, 13 original incomplete/unstable records
rejected, zero differences. Lazy capture replay: 246.897 to 210.495 us with equal
weights; other capture sets were neutral. These are CPU replay timings, not FPS.
