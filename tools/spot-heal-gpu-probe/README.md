# Spot Heal dominant-score GPU feasibility probe

This is an original, explicitly built developer executable. It is not linked
into the editor, does not install a shader, and does not claim to be a working
GPU repair backend. It measures the expensive weighted patch-distance stage,
not the inexpensive voting stage.

```sh
cmake --build build/release --target imageeditor_spot_heal_gpu_probe -j2
build/release/tools/spot-heal-gpu-probe/imageeditor_spot_heal_gpu_probe
build/release/tools/spot-heal-gpu-probe/imageeditor_spot_heal_gpu_probe --validation
```

The probe caches unassociated double-precision RGB, original float alpha and
texture descriptors, target support/weight, and exact CPU-computed locality.
It evaluates the production weighted and normalized objective in sequential
patch-pixel order, including partial alpha and provisional weights. CPU FMA is
disabled and the shader uses `precise`/FP64 (including double-precision decimal
literals) to avoid introducing a hidden FP32 objective or a parallel reduction.
It checks all returned scores against the CPU implementation. It does **not**
weaken whole-donor validity: all generated candidate support is valid, so this
constant-time host check is simply outside the timed objective.

The device, command pool, staging/readback buffers, descriptor sets and pipeline
are reused. The report separates cold device/pipeline creation, input-buffer
allocation and upload, first dispatch, warm device timestamps, and complete
record/submit/fence/readback latency. Fence waits are 1 ms bounded polls; there
are no device-wide or queue-wide idle calls. Validation runs must be recorded
separately from performance runs.

The batch sizes include the approximately 128 diverse candidates used during
serial initialization and larger batches representing several independent
targets. These are **optimistic lower bounds**, not end-to-end repair speedups:

- Targets, candidates and descriptors are already resident and unchanged.
- No dispatch uploads a newly filled boundary pixel or new target descriptors.
- All candidate scores are requested; the optimized CPU's incumbent-aware early
  rejection can be faster than this full-score CPU reference.
- The probe does not implement directional propagation, its per-pixel serial
  random-search dependency, boundary-first initialization or transactional
  publication.

A real exact-schedule backend cannot batch successive boundary initialization
pixels: each accepted pixel changes nearby target descriptors. A useful GPU
backend would therefore need persistent reconstruction/correspondence state,
bounded synchronized directional wavefront work and an explicit strategy for
the remaining serial initialization floor. The shipping Vulkan renderer owns
its graphics/compute queue on the UI side and does not enable `shaderFloat64`;
worker submission must not simply race that queue. This isolated probe uses its
own compute device with explicitly requested FP64 support and avoids changing
the application's Vulkan feature requirements.

## Measured gate, 2026-09-13

Fedora 44, Release `-O3 -DNDEBUG -ffp-contract=off`, RTX 4080 driver
610.57.04, dedicated compute family 2. A 527×527 cached context, radius 7
(225 support pixels), mixed opaque/partial-alpha samples and provisional
weights were identical on CPU/GPU: **zero differing scores**, max absolute
error zero. The separately executed validation run reported zero messages.

| Candidates | Approximate target batches | Cached CPU full scores | Warm GPU full round trip |
| ---: | ---: | ---: | ---: |
| 64 | 1 | 0.0283 ms | 0.1774 ms |
| 128 | 1 | 0.0628 ms | 0.1796 ms |
| 256 | 1 | 0.1190 ms | 0.2042 ms |
| 512 | 2 | 0.2407 ms | 0.2114 ms |
| 2,048 | 8 | 0.9345 ms | 0.2322 ms |
| 4,096 | 16 | 1.8534 ms | 0.2750 ms |

GPU values are medians of 40 warm dispatches. The device timestamp span was
approximately 0.163–0.166 ms; this is not just a host-submit overhead problem.
Process-cold device/pipeline creation took 151.64 ms; buffer allocation and
upload of 18,136,128 bytes took 4.42 ms. Raw logs live in
`build/test-artifacts/spot-heal-performance-pass/gpu-probe.log` and
`gpu-probe-validation.log`.

The relevant serial-initialization batch is around 128 candidates: the GPU is
already **2.86× slower than the cached CPU full scorer**, before new-target
uploads, correspondence updates, cancellation scheduling or publication. It is
not justified as a per-target drop-in accelerator. The large-batch throughput
is useful evidence for a future persistent GPU algorithm, but not permission
to batch dependent initialization pixels or change the accepted directional
schedule unnoticed. Nor does it establish an end-to-end speedup: the optimized
shipping CPU can also reject candidates early and use independent target
wavefronts across several cores, neither of which this CPU comparison includes.
