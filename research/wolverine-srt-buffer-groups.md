# Readonly buffer descriptor groups

Research, default off: `KYTY_SRT_BUFFER_GROUP=1` (live, CPU evaluation only).
Base25783993. Reuses the existing native descriptor routines and their memo, read
order, lazy branches and clean-frame rules. Enters the host C ABI once for a
group of at least four readonly four-word buffer descriptors, instead of once
per descriptor. It does not reuse guest values across materializations.

The native group initializes each descriptor immediately before evaluating it
and returns the successful prefix on failure. The C++ loop decodes that prefix,
handles the next failure without re-reading guest memory, and uses the existing
path afterward when a planning-only failure is recoverable. Future outputs stay
untouched on failure. A fourth C ABI argument carries a per-buffer activation
mask; inactive descriptors are initialized empty without reading guest memory.
Writable buffers, unsupported widths, more than64buffers and native verification
mode retain the original evaluation path.

The group calls the same private routines rather than duplicating their bodies.
It preserves the output pointer, frame/memo/generation registers and callee-saved
registers, including the activation mask across helper calls. A separate entry
thunk returns a prefix count. Output layout is checked
at compile time. No serialized plan or GPU shader layout changes.

Focused tests cover changing guest values, all16 failing DWORD positions, exact
read order, partial outputs, output guards, inactive sources and writable fallback.
ResourceMaterializationTests passed. Replay v2 uses ABBA measurement order;
117valid/11unstable rejected,0mismatches, four independent sessions with25walks per
worker. Equal-weight CPU sum129.703us versus127.462us (~1.8%); not game FPS.
Final masked replay:117valid/11rejected/0mismatches,130.216us versus127.047us
(~2.5%). Second capture set:128valid/0rejected/0mismatches,189.026us versus188.976us
(no aggregate gain). Both include four sessions with25evaluations per worker.
Some individual plans improved more; do not extrapolate that to the whole game.

Broader SrtNativeTests and ProgramDiskCacheTests passed. Keep disabled pending a
justified in-game comparison; the aggregate replay gain does not justify another
game launch by itself.
