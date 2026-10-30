# Task payloads for non-indexed mesh indirect arguments

Research only, off by default. Base143adabc. Code097732d9, Release and
resource_tracking_tests with KYTY_MESH_TASK_INDIRECT=1 passed. No game FPS result.

The task stage snapshots VkDrawIndirectCommand into a three-word task payload
and emits the same mesh group/instance counts and limits as the host fallback.
The mesh stage conditionally reads that payload for indirect draws. Direct
pipelines use the same module but never execute an unproduced payload load.
Indexed/quad/restart/clean-argument cases retain host handling. No per-draw
compute converter, extra submission or copied argument ring remains.

Argument registration happens before resource preparation. BDA resolution happens
after all preparation that can submit, merge or delete buffers. Failure re-enters
the CPU fallback with a recursion guard, rather than dropping the draw. Pipeline
keys include task presence; specialization arrays and the task module remain owned
by the asynchronous graphics pipeline build. Push layout stays at6prefix dwords.

Existing compute access dependencies target AllCommands. Buffer copies have their
normal transfer dependencies. With the task feature enabled, post-draw barriers
also cover address writes. Audit producer ordering and all aliases in gameplay
before promoting. Full-game correctness and performance remain unproven.

An isolated probe builds the production task shader and checks7168draws across
direct/task pipelines, topology parameters, zero/overflow-like counts and limits.
SPIR-V validation passed Vulkan1.3; outputs matched on RTX5070. Vulkan API layer
was unavailable. Its~11us/512direct versus~16us/512task timing is synthetic GPU
overhead, not game FPS or a comparison with the old compute/readback paths.

The first in-game opening attempt was stopped gracefully at the user's request
while they edit a thumbnail. Runner exit0; helper's early-exit error is intentional.
Resume functional validation only after that interruption is lifted. Main143
launcher, game dump and main save are unchanged.

References: [mesh/task semantics](https://docs.vulkan.org/spec/latest/chapters/VK_NV_mesh_shader/mesh.html),
[pipeline creation](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html).
