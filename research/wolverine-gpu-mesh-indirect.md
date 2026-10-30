# GPU conversion of non-indexed mesh draw arguments

The final14b trace contains1276 GPU-owner readbacks (1.720s of inclusive
scope time in a20s capture) inside ExecutePreparedDraw. These intervals
are distinct from resource materialization. The renderer's mesh/legacy-quad
fallback dereferences indirect arguments on the CPU; GPU-written argument
pages therefore force readback. This is a candidate bottleneck, not an FPS
prediction or evidence that all1276 requests are non-indexed mesh arguments.

The experimental path uses a one-invocation compute shader to snapshot
vertexCount, instanceCount, firstVertex and firstInstance, derive the same
primitive/group counts as the host fallback, and emit an indirect mesh draw.
The mesh shader reads the snapshotted count/offset parameters by physical
address. A structured branch prevents physical loads on direct draws.

- Only non-indexed mesh draws are eligible. Indexed, quad and normal draws
  keep their previous paths. The option is off by default.
- Zero-count/zero-instance draws and workgroup-limit violations produce no
  work, matching the fallback. Product validation uses division to avoid
  overflow. No frame, draw or shader is deliberately skipped for speed.
- The converter snapshots offsets as well as group counts: later writes to
  guest arguments cannot alter a draw already recorded.
- The ring is shared with the existing dispatch converter. All-commands to
  compute ordering covers previous consumers before an entry is overwritten;
  compute to indirect/mesh ordering makes both counts and parameters visible.
- Acquiring argument storage/conversion precedes final resource rebinding,
  because the cache can merge buffers while acquiring the argument range.
- `KYTY_MESH_INDIRECT_GPU=1` changes shader emission and is included in the
  program-cache translation signature. Old shader cache files are preserved.
- `KYTY_MESH_INDIRECT_RUNTIME=0/1` selects fallback/conversion within the same
  process and ABI. Use this for an A/B/A/B/A comparison after warmup, keeping
  the previously winning resource options enabled in every phase.

Validation: Release build, resource tracking tests, converter SPIR-V
validated with spirv-val for Vulkan1.3. The automated in-game comparison enables
the emulator's shader validation, uses the protected copied save, and checks
runtime acknowledgements for every changed switch. No performance conclusion
before comparison and visual verification.

Results (protected stationary scene;25 samples/phase, A/B/A/B/A):

- V1, unconditional conversion: A16.92/17.16/16.68, B15.76/15.80 FPS.
- V2, only GPU-dirty arguments: A17.08/17.16/17.36, B15.96/16.00 FPS.
  Frame-counter progress confirms A17.16/17.16/17.27 versus B15.92/15.84.
- Both regress. The candidate is NOT recommended or enabled by the play
  launcher. Preserve it as research, not a production optimization.
- No SPIR-V validation failures or crashes; visual checks of the same scene
  showed no obvious rendering break. This is not whole-game validation.
- The extra compute dispatch/ordering and processing zero draws that the
  CPU fallback can discard early are plausible costs, not measured attribution.
- Extending conversion to multi-draw was considered but rejected for this
  scene: the existing capture contains zero readbacks inside
  CpOpDrawIndirectMulti. Do not optimize that packet based only on its code.

References: [indirect mesh command](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMeshTasksIndirectEXT.html),
[mesh workgroup limits](https://docs.vulkan.org/refpages/latest/refpages/source/VkDrawMeshTasksIndirectCommandEXT.html),
[separate push-descriptor bindings by pipeline type](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html).
