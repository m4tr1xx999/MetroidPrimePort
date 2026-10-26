# Conservative sun-cascade filtering evidence

Measured 2026-10-26 on Apple M1 Max/Metal. Code commit 524b538b951566c09044f4947a77ddb8ba0d90e8, upstream base 7ce9dd0cd07d0a684286ba66212685bb9ed69db1. Both binaries use the same fixed Dawn v20261025.182847 prebuilt override; download stamp and library SHA verified. Build identity is in build-identity.json.

## Applicability

Current upstream deliberately builds sun maps only for directional lights with the cast bit. Its header documents that no shipped light has it. A scan of 286rooms found four relevant directional LightDynamic entries, all without that flag. This patch does not enable them. There is no demonstrated default-game FPS improvement.

The game measurements use a **synthetic sun-shadow stress fixture**, enabling only sun3 casting in an isolated copy of the old D1241219 environment. Everything else remains as imported. The version22-to24 migration adds the new cast fields and zero worldShift, preserving the old reader default. Original-file hashes and byte restoration were verified privately. No game assets or fixture are published here.

## Fresh game-renderer measurements

Fixed EFB 1704x960, scale 2, MSAA 4, dynamic resolution off, vsync off, uncapped. Every frame is rendered to the EFB, but AURORA_PRESENT_EVERY=60 presents only one in 60. This avoids a 120Hz presentation limit observed in one ordinary baseline run, which was excluded. Values below are **render throughput, not normal presented-game FPS**.

Two three-second frame-delta samples per view/mode after warm-up. The patched renderer alternated filtering off/on/off/on; the unchanged base ran separately. Timed samples had no pending pipelines or skipped draws.

| View | All-layer reference | Filtered | Fewer submissions |
|---|---|---|---:|
| Distant frigate | 256/256/256/256 | 72/72/72/130 | 66.2% |
| Close exterior | 351/351/351/351 | 184/219/337/351 | 22.3% |
| Platform | 400/400/400/400 | 225/295/358/400 | 20.1% |

| View | Unchanged upstream | Patched all-layer reference | Patched filtered |
|---|---:|---:|---:|
| Distant frigate | 3.316 ms | 3.319 ms | 3.259 ms |
| Close exterior | 3.540 ms | 3.686 ms | 3.611 ms |
| Platform | 3.916 ms | 3.854 ms | 3.892 ms |

The frame-time differences are small, include a slight regression in one paired view, and are within run-to-run noise. They do not support a strong total-frame gain. Fewer submissions alone are not a claim of lower GPU time.

Within the patched process, all nine ordinary off/on/repeated-off comparisons and all three sun-visibility comparisons changed zero pixels. Included close-view before/after PNGs were converted independently and have identical bytes. Independently loaded upstream scenes were not time-matched and differ; cross-process pixel equality is not claimed.

The moving intro resumed and finished, followed by an interior transition into MREA85578E54 without an active sun map. No validation/fatal errors occurred. Original saves/settings and live imported environment stayed unchanged; no installed app was replaced.

## GPU depth comparison

The standalone source uses production cascade-fit/membership maths with an independent WGSL rasterizer on Metal. It does not exercise the production caster queue or generated shaders; FIFO tests and game captures cover those separately.

8 views x 4 layers x 1024x1024: zero changed depth texels, 29,440 -> 10,887 submissions, 19,346,865 covered depth texels. Includes opaque/cutout draws, GPU-applied transforms/reflections, side/far-plane boundaries, sunward near-plane pancaking, and unknown-All fallback. Exact output is in gpu-result.log.

To reproduce on a Mac with Dawn installed:

    cmake -S . -B /tmp/shadow-culling-evidence -DMP_SOURCE_ROOT=/path/to/MetroidPrimePort -DDawn_DIR=/path/to/Dawn/lib/cmake/Dawn
    cmake --build /tmp/shadow-culling-evidence
    /tmp/shadow-culling-evidence/shadow_culling_evidence

## Other validation

Native build passed; all 256 FIFO tests passed; all 8 cascade tests passed with ASan/UBSan and strict warnings. Full serial CTest on 7ce9dd0c: 638 passed, 1 skipped, 1 failed of 640. The remaining SceneHdrTest.GxWriterVariantsCompileInBothModes abort reproduces on the unchanged same upstream base; no HDR test changes are included.
