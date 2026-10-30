# Readback caller classification

`GuestGpu::SendCommandSync` runs its callable inline when already on the GPU
owner. The synchronous `BufferCache::ReadMemory` dispatch marked every callable
as `s_forwarded=true`, including owner-side shader reads. Historical
`guest threads` counters therefore cannot establish that all readbacks can be
moved off the GPU owner. Their total counts and elapsed scope times remain
usable, with their original profiling limitations.

The caller is now identified before dispatch, and the previous thread-local
marker is restored afterward. No memory transfers, barriers, ownership states,
waits, or default production options are changed.

With `KYTY_READBACK_STATS=1`, each report includes all buffers, including those
outside the displayed top eight, in four groups: owner-read, owner-write,
guest-read, guest-write. Times measure GPU-owner scopes. Asynchronous Begin
scopes time submission, not the guest's subsequent wait. Do not add these to
inclusive profiler waits or interpret them as FPS.

Next measurement: best resource flags (capture spans 2, canonical views 1),
protected copied save, one settled in-game diagnostic run. Do not benchmark
while a large extraction is consuming RAM/commit. Compare caller counts/times
before choosing asynchronous write work or a shader-read strategy. The release
14b04514 artifact and user launcher remain the recommended play build until a
separate performance change is validated.
