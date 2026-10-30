# Recorded SRT reads from authorized backing spans

`KYTY_SRT_CAPTURE_SPANS` is experimental and defaults to 0:

- 0: the existing capturing readers, every uncached logical read uses the reader.
- 1: capturing readers may copy from their own already-authorized span.
- 2: also permit grouped native reads, recording each uncached word in order.

Only a reader that explicitly supplies a valid backing span for the lifetime of the
evaluation opts in. The renderer publishes spans only for clean backing pages and
invalidates them when a read log needs the actual callback stream. Capture still
disables ordinary direct JIT loads: those would omit specialization proof ranges.
Grouped reads use a separate span and append the exact successful logical reads,
including their original addresses and sizes; they never substitute whole pages
in the alias proof. Ordinary and strict readers keep separate authorization.

Span bounds use subtraction, short spans fall back to the reader, failed reads
still fail, and a missing strict reader cannot be supplied by a span. Memoized
words do not add duplicate reads. No data is retained across evaluations.

Validation: native and tracking suites, materialization cases with distinct
ordinary/strict data, short spans, failures, direct loads and grouped buffer reads.
All four existing replay capture directories passed differential comparison
(499 valid records, 13 pre-existing incomplete/unstable records rejected).
Replay timings model CPU evaluation with a map-backed reader, not game FPS.

Initial FPS runs without explicit benchmark configuration are invalid. The
benchmark script now sets the override directory itself, validates its flags,
and requires the emulator's confirmation that commandline.txt was overridden.

With this correction, a protected stationary jungle scene gave 14.6 / 16.5 /
14.3 / 16.0 / 14.45 FPS for modes 0 / 2 / 0 / 2 / 0, twenty one-second samples
per phase after sixty seconds settling. Candidate mean 16.25 versus 14.45
for controls is a scene-specific ~12.5% difference inside that executable.
This is not a claim of 30 FPS or validation of the whole game.

An attempted increase from eight to thirty-two words per native group passed
differential tests but showed no aggregate replay gain (247.024 versus
247.226 us, 128 plans). It was removed before another game test.
