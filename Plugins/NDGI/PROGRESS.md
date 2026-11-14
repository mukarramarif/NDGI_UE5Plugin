# NDGI — Current Progress

**Status:** Experimental / in development (Unreal Engine 5.8)
**Updated:** November 2026

NDGI (Neural Dynamic GI) replaces baked lightmaps with per-tile neural models trained
from a multi-step time-of-day bake. At runtime an `ANDGIActor` decodes the model at the
selected time and writes RGB + directional SH directly back into the engine's uncompressed
lightmaps. Static geometry keeps baked-lighting cost while movable objects keep real-time
directional shadows.

See [README.md](README.md) for the full console-command reference.

---

## What works today

- Dockable **NDGI Pipeline** editor panel (Tools > NDGI, or `ndgi.Pipeline`) with bake,
  merge, train, and validate controls, live progress with ETA, captured logs, and a
  one-click bake → merge → train → validate workflow. The panel now includes an
  artist-facing **Project health** card (level asset, projected bytes, BC7 readiness, bake
  age/slices, merged freshness, TOD profile, next-step hint), a **per-atlas breakdown**
  (resolution, grid, models, BC7/legacy, encoding ranges, projected bytes), a **Temporal
  holdout** preview with baseline comparison, a **Rescan** action for model assets, and a
  **Reports & diagnostics** card. The card runs `ReportStorage`, dependency, benchmark-scene,
  BC7, TOD, async-cache, cadence, decode benchmark, and Lightmap Inspector checks, and a
  searchable **Run any NDGI command** browser lists every registered `ndgi.*` command with
  its help text and an optional argument field, so console-only functions are reachable in
  the panel. Diagnostics output is captured in the existing log. Training now writes
  `Saved/NDGI/Training/TileMetrics.json`, and a **Model quality** card lists the worst
  tiles by PSNR. A **Time of day** card previews the default profile at any normalized time,
  lists keyframe sun/sky values with color swatches, and provides cycle-duration,
  ping-pong, save, open, and reset helpers.
- Level-scoped bake manifests: `BakeTOD` records the current level package, slice count,
  timestamp, and atlas list; merge/train ignore stale files from other levels in the shared
  bake folder, and training rejects merged sets older than the current bake.
- Multi-step CPU Lightmass bake with RGB + directional SH export (`.ndgi` v3).
- Per-component lightmap HQ encoding ranges: bake export, training, and runtime all use
  the range of the component that owns each atlas texel. Ownership is rasterized from the
  actual lightmap-UV triangles (with a half-texel gutter), so irregular packing no longer
  misassigns padding. This fixes wrong colors/SH on atlases shared by components with
  different `Scale`/`Add` ranges.
- Per-tile 6-output MLP (RGB + SH.xyz) trained on GPU with the BC-simulated feature maps.
  Training uses a 4× SH loss weight and interleaves full-region samples with samples from
  the four tile-edge/halo bands.
- **BC7 feature export:** trained `F_uv` and every `F_uvt` time slice are BC7-encoded and
  stored in `UNDGIModelAsset`; the runtime uploads them as BC7 `Texture2DArray` layers and
  samples them with hardware decode. Only the MLP and the small `F_ut`/`F_vt` planes stay
  in a compact structured buffer. Raw RGBA8 feature arrays are editor-only, so cooked
  assets carry the BC7 representation.
- **Virtual-texture page cache:** `ndgi.ResidentModelsPerAtlas N` bounds each active
  atlas to N physical BC7 model slots. `ANDGIActor` ranks tile models by their world
  bounds, keeps the nearest pages resident, remaps the tile→slot table, and uploads
  only pages that enter/leave the pool (`ndgi.StreamingUpdateSeconds`, default 0.5 s).
  BC7 arrays and the compact parameter buffer are persistent; only the changed layers
  and slots are locked and updated.
- **Disk page streaming:** `TileModelPaths` are soft references. Model packages load
  through `FStreamableManager` only when a page becomes resident, under
  `ndgi.StreamingMaxLoadsInFlight` and `ndgi.StreamingUploadBudgetKB`, and the package
  handle is released after the BC7/compact payload is copied to the GPU. Legacy hard
  references are auto-migrated to soft paths in `UNDGILevelAsset::PostLoad`;
  `ndgi.ResaveLevelAsset` persists the migration for cook.
  `ndgi.TestVTStreaming`, `ndgi.TestDiskStreaming`, and `ndgi.TestAsyncPages` exercise
  the cache on the WP test map.
- **Lightmap Inspector:** a separate editor tab (`ndgi.Inspector`) reads the live CPU
  Lightmass atlas and merged `.ndgi` sets, decodes HQ RGB/SH with per-component ranges,
  and displays final color, channels, luminance, alpha, SH, encoding ranges, coverage,
  live-vs-merged difference, and a tile-grid overlay. It exports the current view to
  `Saved/NDGI/Inspector`. `ndgi.TestLightmapInspector` validates the live-vs-merged
  decode path against `Level_Sky_Temple_Demo_01`.
- Overlapping inference: each model is trained on its tile plus an 8-texel halo, and the
  runtime cross-fades every model whose extent covers a texel (overlap configurable in
  `TrainAtlas`/`TrainAllAtlases`).
- Atlas padding that no lightmap triangle owns is preserved by the decoder instead of
  being overwritten with decoded noise; the same coverage mask keeps padding out of
  training and seam/SH validation.
- Degenerate RGB ranges keep the baked chroma and drive luminance through the HQ alpha
  channel, avoiding the old saturated green/black tile-edge artifacts.
- Runtime decode cache: packed parameters, per-texel range/index buffers, and persistent
  GPU buffers, built incrementally (one atlas per tick) so the game thread never blocks for
  the whole scene. A 2 GB CPU+GPU budget guard is enforced by `ndgi.MaxCacheMemoryMB`.
- Batched single-graph decode for every loaded atlas.
- Direct **BGRA8 UAV + `CopyTexture`** write into the engine lightmaps (no readback/upload,
  no engine-internal format swap).
- Tiles without a model, and atlas padding, keep their original baked lightmap.
- Bake and training jobs can be canceled between slices/tiles (`Cancel Job` in the panel,
  or `ndgi.CancelPipeline`); the full-pipeline chain stops with them.
- Interpolated per-step RGB/SH means between the 8 baked time slices.
- Latest-wins async decoding with a 10–20 Hz cadence; the sun rotates every frame.
- World Partition / streaming-level ownership: `BakeTOD` records the streaming
  level packages that own each atlas, the level asset stores them, and `ANDGIActor`
  activates only the atlases referenced by loaded cells. Unloaded atlases release
  their GPU buffers and target-texture references; reloading a cell repacks and
  uploads them. Only active atlases are decoded.
- Stylized time-of-day profile (`UNDGITimeOfDayProfile`) with shared keyframes for sun
  pitch/yaw/color/intensity and sky color/intensity. `ndgi.BakeTOD` and `ANDGIActor`
  evaluate the same profile, so baked and dynamic lighting match.
- `ANDGIActor` auto-cycle (server-driven, loop or ping-pong), replicated `TimeOfDay`,
  `bAutoCycle`, and `Profile`; clients follow the server's time.
- Validation/debug commands for SH, directionality, seams in raw and decoded space,
  encoding ranges, empty tiles, cadence, GPU kernels, streaming activation, TOD profiles,
  and auto-cycle.
- **Held-out-time validation:** `ndgi.ValidateTemporalHoldout` trains a model with one
  fewer temporal feature slice and evaluates the excluded baked slice at its true time,
  writing `Saved/NDGI/TemporalHoldout.json`. The `baseline` option trains all slices and
  evaluates the same time so the interpolation gap is directly measurable.
- **Stale-asset guards:** `BenchmarkDecodeWarm`, `TestAsyncCache`, and `ReportStorage`
  now fail with an explicit `stale-asset check FAILED` line when models lack BC7 features
  or the cache contains zero models. `RunNDGICI.ps1` gates on the skipped-model count.
- The benchmark harness (`RunNDGIBenchmark.ps1 -Mode bc7|pool`) runs the BC7 decode,
  cadence, memory, storage, and streaming tests and sweeps
  `ndgi.ResidentModelsPerAtlas`, writing `Saved/NDGI/Benchmarks/summary.json`.
- Cooked DX12 runtime path: the decode compute shader is compiled for PC game targets, and
  cooked lightmaps use streaming residency instead of `NeverStream`/`UpdateResource`
  (which asserts on partially inlined cooked mips).
- `ndgi.TestRuntimeSmoke` validates the cooked write path end to end: shader present, cache
  built, two times decoded, modeled texels changed, empty tiles preserved, values in range,
  and temporal change observed.
- `Tools/RunNDGIPackage.ps1` performs the BuildCookRun/package/archive flow and runs the
  packaged smoke test. `Tools/RunNDGICI.ps1` runs quality, performance, memory, and
  packaged gates and writes `Saved/NDGI/CI/<timestamp>/summary.json`.

---

## Measured results

All numbers below were measured on an **AMD RX 9070 XT** unless noted.

| Metric | Result | Context |
|---|---|---|
| SH reconstruction | **28.20 dB**, 0 out-of-range | 512×512 test atlas, 8 slices, 16-wide MLP, tile 64, overlap 8, 30000 steps |
| Per-tile PSNR | **45.06 dB** average | Same test atlas (normalized training/validation metric) |
| Seam quality (decoded) | **0 introduced luminance**, **8 of 2,886 introduced SH** | Real tile-spanning UV island at t=1.0; 0.28% of boundaries, max introduced step 0.0216 |
| Continuous-boundary residual | **0.00001 luminance / 0.00272 SH** | Mean decoded discontinuity on boundaries that were continuous in the bake |
| Raw RGB seam error | 0.00483 mean / 0.01961 max | Same test atlas; raw SH is range-dependent and not used for the gate |
| SH directionality | mean **0.0057**, p99 **0.0802**, p99.9 **0.1839**, max 0.4880 | 698,410 dot products over 10 normals; max outliers are sub-texel UV islands |
| Live runtime decode (legacy assets) | **p50 5.50 ms / p95 6.08 ms** | 19 atlases, 578 models, 67.3 MB params, 1920×1080 |
| FP16 runtime decode (legacy) | p95 6.19 ms | Same scene/targets |
| BGRA8 benchmark targets (legacy) | p95 6.08 ms | Same scene/targets |
| Game-thread enqueue | ~0.02 ms | Cached decode |
| CI quality gates | **PASS** | SH 28.20 dB / 0 out-of-range; directionality mean 0.0057, p99 0.0802, p99.9 0.1839, max 0.4880; seams 0 luminance, 8/2,886 SH, max 0.0216 |
| CI performance gate | **PASS** | Warm engine-lightmap p95 5.90 ms (gate <= 50 ms); cadence 16.2 Hz |
| CI memory gate | **PASS** | 67.3 MB CPU + 67.3 MB GPU = 134.6 MB (budget 2048 MB); worst async tick 49.93 ms (gate <= 100 ms) |
| Cache memory guard | **PASS** | `ndgi.MaxCacheMemoryMB 1` rejected the first benchmark atlas |
| Cooked DX12 packaged smoke | **PASS** | Development/SM6 package; 1 atlas, 20 models, 524,519 changed texels, 0 empty-tile changes, 204,498 temporal changes, 0 out-of-range |
| Decode cadence | **16.2 Hz** (81 decodes in 5 s) | `ndgi.TestDecodeCadence 5` |
| Cache build (sync) | **270 ms** | 19-atlas scene; down from 697 ms after removing per-atlas flushes |
| Cache build (async) | **61 ms worst tick**, 932 ms wall | 19 atlases, one atlas packed per tick, 578 models |
| Decode-cache memory | **67.3 MB CPU + 67.3 MB GPU** | 19-atlas scene; default guard 2 GB (`ndgi.MaxCacheMemoryMB`) |
| Empty-tile preservation | **0** changed values | Test map + 19-atlas scene |
| BC7 feature export | PASS | WP test atlas, 4 models, 10-step and 800-step training; format-provider BC7 encode |
| BC7 roundtrip (`ndgi.TestBC7`) | PASS — 42.77 dB overall, R/G/B/A 46.44/41.37/46.79/40.23, max error 10, direct/swapped MSE 3.44/13216.73 | 64×64 synthetic RGBA8; confirms channel order |
| BC7 storage projection | **2.66× vs raw RGBA8 features**, **30.30× vs raw 8-slice BGRA8 set**, **2.11 BPP** | `ndgi.ReportStorage` on WP; 4 models, tile 64, overlap 8, 8 slices |
| Stale-asset guard | **FAILED as designed** — 578/578 models skipped | `Lvl_Bench_NDGI` is pre-BC7; its old 1.15 ms p95 is now rejected |
| WP BC7 decode (current asset) | p95 **0.55 ms**, 1 atlas, 4 models, 0 skipped | `/Game/NDGI/NDGITestBakeMap_WP`, 50 iterations, 1280×720 |
| WP BC7 async cache | **107.5 ms** build, **0.03 ms** worst tick, GPU 0.1 MB | 1 atlas, 4 models, 2.11 BPP |
| WP page pool warmup | pool 0/1/2 → p95 0.51/0.47/0.47 ms; 1/2 pages warmed | Tiny 4-model scene; verifies pool warmup, not production perf |
| Temporal holdout, 800 steps | holdout **11.28 dB** (RGB 8.88, SH 6.53); baseline **13.91 dB** (RGB 9.90, SH 9.21); gap **2.63 dB** | Sky Temple Tex00, 2 edge tiles, held-out step 4 |
| Temporal holdout, 3000 steps | holdout **12.27 dB** (RGB 10.22, SH 7.34); baseline **20.59 dB** (RGB 13.69, SH 17.20); gap **8.32 dB** | Same tiles; max abs holdout 0.2185 RGB / 0.4011 SH |
| VT page cache | PASS — 4 models, pool 2, peak resident 2, 4 uploads, 6 evictions, all decodes ok, GPU 0.06 MB | `ndgi.TestVTStreaming 2` on `/Game/NDGI/NDGITestBakeMap_WP` |
| World Partition spike | PASSED — 2 atlas textures, saved build data exists | `/Game/NDGI/NDGITestBakeMap_WP` |
| Full pipeline (WP, TOD profile) | 2 atlases, 3 tiles, 3 models, **ValidateLevelAsset PASSED** | `ndgi.RunPipeline 4 128 500 8` on the WP test map |
| Per-cell activation | **PASS** — 2 atlases → 0 active (GPU 0.0 MB, 0 queued), restored 2/2 | `ndgi.TestStreamingCells none` |
| Cache memory guard | **PASS** — 1 MB limit rejected a 7.5 MB first atlas | `ndgi.MaxCacheMemoryMB 1` + `ndgi.TestAsyncCache` |
| TOD profile evaluation | **PASS** — 5 keyframes, noon pitch/intensity > night | `ndgi.TestTODProfile` |
| TOD auto-cycle | **PASS** — time 0.000 → 0.605 in 3 s, 57 decodes | `ndgi.TestAutoCycle 3` (WP, `-game`) |
| GPU MLP kernels | Forward max abs 9.5e-7, backward 9.5e-7 | 16-wide, 646-param, weighted SH loss, vs CPU reference |

Current test-atlas run (512×512 atlas with one 256-texel floor island, tile 64, overlap 8,
SH weight 4):

| Steps | Per-tile PSNR | SH PSNR | Decoded continuous SH |
|---|---|---|---|
| 30000 | 45.06 dB | 28.20 dB | 0.00272 |

Hidden-width sweep (synthetic 64×64, 8 steps, 32×32 tile):

| Hidden | Steps | Overall | RGB | SH |
|---|---|---|---|---|
| 16 | 1500 | 18.15 dB | 19.33 dB | 16.67 dB |
| 32 | 1500 | 20.99 dB | 21.65 dB | 19.49 dB |
| 32 | 3000 | 25.63 dB | 25.68 dB | 24.21 dB |
| 48 | 3000 | 26.37 dB | 26.52 dB | 24.75 dB |

**16-wide remains the default** because it is ~2.5× cheaper to decode than 32 and keeps
the most headroom for the RTX 3060 target. 48 adds only ~0.7 dB over 32.

---

## Milestone status

### M0 — spikes: complete
- 6-output SH MLP and `.ndgi` v3 roundtrip (v3 adds the encoding-range map).
- 50 ms-class write path (persistent params, batched graph, direct BGRA8 write).
- GPU Lightmass diagnosed as producing no usable lightmaps; CPU Lightmass is the bake path.
- World Partition cell force-load/bake/save verified.
- Hidden-width 16/32/48 prototype.

### M1 — directional quality: complete
- Done: 8-key bake + interpolated means, SH encode/decode, SH.w directionality fix,
  empty-tile preservation, per-tile PSNR instrumentation, overlapping inference and
  blending, per-component encoding ranges, degenerate-chroma handling.
- Done: triangle-rasterized range ownership and coverage masks; atlas padding is
  preserved by the decoder and excluded from training/seam/SH validation.
- Done: 4× SH loss weight and interleaved edge/halo sampling. On a 512×512 atlas with a
  real tile-spanning island: SH PSNR 28.20 dB, per-tile PSNR 45.06 dB, 0 introduced
  luminance seams, 8/2,886 introduced SH boundaries (max step 0.0216).
- Done: directionality gate is distribution-based (mean 0.0057, p99 0.0802, p99.9 0.1839,
  max 0.4880). Isolated sub-texel UV islands dominate the absolute max; the percentile gate
  is the visible-quality gate, with a 0.60 sanity bound on the max.
- Not implemented: wider (32) MLP (kept 16 for runtime cost), early stopping, per-tile
  heatmap PNG, worst-tile auto-retrain.

### M2 — runtime: complete (RTX 3060 measurement deferred)
- Done: decode cache, per-texel range/index buffers, persistent GPU buffers, batched render
  graph, direct write, async latest-wins requests, 10–20 Hz cadence, cadence test.
- Done: dockable pipeline panel with bake/merge/train/validate, progress, log capture, and
  full-pipeline chaining. The chain lives in a module-level runner, so closing the tab does
  not stop it. Panel construction and the full `ndgi.RunPipeline` chain were smoke-tested in
  the editor; the WP run trained only the manifest-scoped 2 atlases and validated. The panel
  also has a **Reports & diagnostics** card and a searchable browser for every registered
  `ndgi.*` command, so report/diagnostic console functions run from the UI. Training writes
  per-tile PSNR to `Saved/NDGI/Training/TileMetrics.json`, which feeds the in-panel **Model
  quality** worst-tile list.
- Done: chunked async cache build (one atlas per tick), removing the 697 ms game-thread
  stall (now 270 ms sync / 61 ms worst async tick on the 19-atlas scene).
- Done: decode-cache memory report and a 2 GB CPU+GPU guard (`ndgi.MaxCacheMemoryMB`).
- Done: bake/training cancellation between slices/tiles (`Cancel Job`,
  `ndgi.CancelPipeline`); the auto-run chain stops with the canceled stage.
- Done: repeated benchmark on the AMD RX 9070 XT — 6.08 ms p95 engine-lightmap decode,
  16.2 Hz cadence.
- Deferred: RTX 3060 measurement (hardware not available on this machine); merge and
  validation remain short synchronous operations.
- M3 adds loaded-streaming-level atlas filtering; visibility-only filtering (loaded but not
  visible) is still not distinguished.

### M3 — World Partition streaming: complete (atlas-level ownership)
- Done: bake manifests record the streaming-level packages that reference each atlas;
  the level asset persists them as `SourceCells`.
- Done: `ANDGIActor` builds the cache with the currently loaded cell set, polls streaming
  levels every 0.5 s, activates atlases when their cells load, and releases GPU buffers
  plus target-texture references when they unload. Only active atlases are decoded.
- Done: active GPU memory is tracked separately from total cache memory; the 2 GB guard
  rejects activations/builds that would exceed `ndgi.MaxCacheMemoryMB`. The async builder
  stops as soon as the budget is exceeded.
- Done: headless `ndgi.TestStreamingCells` exercises activation, GPU release, decode with
  0 active atlases, and reactivation.
- Limitation: CPU Lightmass packs components from multiple cells into shared atlases, so
  ownership and streaming are per atlas, not per cell; training remains per atlas. True
  per-cell atlases/training need per-cell bakes. The current WP test map has one cell, so
  multi-cell activation still needs a larger streaming test scene.

### M4 — time-of-day profile: complete (network playtest pending)
- Done: `UNDGITimeOfDayProfile` with stylized keyframes and shared bake/runtime settings
  (bake slices, tile size, train steps, halo, cycle duration, ping-pong).
- Done: `ndgi.BakeTOD` evaluates the profile for every slice and applies sun pitch/yaw,
  color/intensity, and sky color/intensity. `ANDGIActor` evaluates the same profile at
  runtime.
- Done: server-driven auto-cycle (loop or ping-pong), replicated `TimeOfDay`,
  `bAutoCycle`, and `Profile`; clients apply replicated keyframes through `OnRep`.
- Done: pipeline panel **TOD Defaults** loads the profile's bake/training settings.
- Done: pipeline panel **Time of day** card lists the profile's keyframes with sun
  elevation/azimuth, sun/sky color swatches, and intensities; a preview slider evaluates
  any normalized time. Cycle duration and ping-pong can be edited in-panel and saved with
  **Save Profile**; **Open Profile** and **Create/Reset Default** helpers are provided.
- Done: `ndgi.CreateTODProfile` creates `/Game/NDGI/TOD/NDGITODProfile`;
  `ndgi.TestTODProfile` and `ndgi.TestAutoCycle` validate evaluation and cycling.
- Fixed: `SunPitch` is sun elevation above the horizon. Both the bake and the runtime now
  negate it for the directional-light actor's rotation pitch; applying it directly put the
  sun below the horizon at positive elevations and made early morning/midday floors black.
  `TestTODProfile` now also checks that noon/dawn light forward Z is not positive.
  **Existing bakes made before this fix must be re-baked and retrained.**
- Limitation: replication is implemented but has not been exercised in a networked PIE
  session on this machine. Fog/post-process keyframes are not part of the profile yet.

### M5 — shipping: complete (packaged Development build; Shipping configuration remains a release step)
- Done: runtime decode shader cooked for PC game targets instead of editor-only
  permutations.
- Done: cooked-safe lightmap residency. Cooked builds request streaming residency and
  wait for full mip 0 instead of calling `NeverStream`/`UpdateResource`, which asserts in
  `TextureDerivedData` on partially inlined cooked mips.
- Done: `ndgi.TestRuntimeSmoke` validates the cooked path: decode shader present, cache
  build, two-time decode into live engine lightmaps, changed modeled texels, empty-tile
  preservation, numeric range, and temporal change.
- Done: `ndgi.TestRuntimeSmoke` has a dedicated `-ndgiSmoke` command-line trigger plus
  `-ndgiSmokeMap`, because Shipping builds compile out `-ExecCmds`; it also writes a
  `Saved/NDGI/TestRuntimeSmoke.txt` state file (`STARTED`, `BUILDING`, `DECODED_A`,
  `DECODED_B`, `PASSED`/`FAILED`) so the gate works without Shipping logging.
- Done: `Tools/RunNDGIPackage.ps1` builds/cooks/stages/paks/archives the test map,
  quarantines the known-unloadable SoulCity maps during the cook, selects the real
  executable (not the bootstrap), forces `-DEFAULTMAP`, waits for `-ndgiSmokeMap`, and
  gates on the state file.
- Done: `Tools/RunNDGICI.ps1` quality/performance/memory gates with JSON/text summaries;
  `Tools/Restore-NDGIQuarantine.ps1` recovers content if a package run is killed.
- Verified: Development DX12/SM6 package of `NDGITestBakeMap` cooked with 0 errors and ran
  the packaged smoke test; see the measured table.
- Verified: Shipping target compiles, and the command-line smoke/state-file path is in
  place. A full Shipping package + smoke rerun remains the release step because the
  earlier package was made before `-ndgiSmoke` existed.

---

## Known issues / limitations

1. **Small residual SH seam.** On the 512×512 test atlas, 8 of 2,886 decoded SH boundaries
   still exceed the 4/255 detection floor (0.28%, max introduced step 0.0216). Luminance has
   0 introduced discontinuities and the mean continuous-boundary SH residual is 0.00272.
   The 16-wide MLP is the limiting factor; the gate allows ≤0.5% of boundaries with a
   ≤0.0625 max step so a regression still fails.
2. **Directionality tail.** Mean error is 0.0057 and p99 is 0.0802, but isolated sub-texel
   UV islands reach 0.488. The gate is mean/p99/p99.9-based with a 0.60 max sanity bound;
   a 32-wide MLP or finer feature resolution would reduce the tail but raises decode cost.
3. **Cache build is chunked, not free.** The async builder packs one atlas per tick
   (61 ms worst tick on the 19-atlas scene), with a 270 ms synchronous path for tools.
   Merge/validation are still short synchronous operations. The 2 GB guard reports and
   refuses oversized caches (`ndgi.MaxCacheMemoryMB`).
4. **No RTX 3060 measurement.** Current performance numbers are from an AMD RX 9070 XT.
5. **Legacy assets.** v1/v2 `.ndgi` files and old models load, but have no per-texel
   range map and use the atlas-wide fallback; colors/SH can be wrong when components sharing
   an atlas have different ranges. In addition, models exported before the BC7 change have
   no `FeatureUVBC7`/`FeatureUVTBC7` and are skipped by the runtime. **Retrain and re-cook
   every atlas** for correct output.
6. **Degenerate-color regions.** When a component's RGB range is almost zero, NDGI keeps the
   baked chroma and updates only luminance. Hue changes over time are lost there, but the
   region is nearly uniform by construction and no longer saturates.
7. **Streaming is atlas-level.** CPU Lightmass packs components from multiple cells into
   shared atlases, so one atlas activates when any of its cells is loaded. Per-cell
   atlases/training would require per-cell bakes. The current WP test map has one cell;
   multi-cell activation needs a larger streaming test scene.
8. **TOD replication is not playtested.** `TimeOfDay`, `bAutoCycle`, and `Profile` are
   replicated and clients apply `OnRep_TimeOfDay`, but the path has not been exercised in a
   networked PIE session. Fog/post-process keyframes are not part of the profile yet.
9. **Bake constraints:** CPU Lightmass only; compressed lightmaps cannot be updated; GPU
   Lightmass currently produces no usable lightmaps.
10. **Level assets reference build-data textures by soft path.** Re-baking can rename them, so
    retrain all atlases after a bake.
11. **The source `LV_Soul_Slum` map is currently unloadable** (`InvalidTag`); this is a content
    issue unrelated to NDGI. Several generated test maps have shown transient
    `InvalidTag` warnings while the editor is saving; re-opening after the save settles
    loads them correctly. `SLV_Soul_Slum_Collision.umap` is also corrupt. Because the
    asset registry aborts a cook on these packages, `RunNDGIPackage.ps1` quarantines both
    for the duration of the cook and restores them in `finally`. If a run is killed,
    `Tools/Restore-NDGIQuarantine.ps1` restores them.
12. **Shipping-config package is not yet run.** M5 verified a Development DX12/SM6 cooked
    package and packaged smoke test. The same scripts accept `-ClientConfig Shipping`;
    run that on the release machine before shipping.
13. **CI performance numbers are hardware-specific.** The 6.37 ms p95 perf gate and
    cadence numbers were measured on the AMD RX 9070 XT. The CI script exposes gate
    thresholds so a slower runner can use a larger `-MaxP95Ms` while still catching
    regressions.
14. **Pre-fix bakes have the sun below the horizon.** The earlier bake/runtime applied
    `SunPitch` directly as the actor pitch; positive elevations therefore lit surfaces
    from below and caused black floors at dawn/midday. The code now negates the elevation.
    Re-bake, merge, and train every atlas after updating from an older build; the old
    lightmaps and models cannot be corrected at decode time. The WP test map was re-baked
    and retrained with the fix (`ndgi.RunPipeline 8 128 800 8`).
15. **Disk streaming is package-granular.** Each page loads one `UNDGIModelAsset` package
    and releases its handle after GPU upload. There is no custom chunk store yet, so a page
    re-entry reads the package again and the streamable manager/GC decides when the
    package memory actually returns. Resident pages still cost GPU memory for the full
    physical pool, not just the uploaded pages.
16. **Page updates are incremental, but buffer rewrites are whole-slot.** BC7 uploads lock
    only the changed texture-array layers. The parameter and tile-index buffers are small
    and are rewritten in full under `LockBuffer`; they still use one render command per
    atlas per streaming update. There is no per-frame streaming work when the camera and
    residency are stable.
17. **Tile bounds are approximate.** `BuildTileBounds` assigns a component's world bounds
    to every tile overlapped by its lightmap-UV bounding rectangle. It is only a ranking
    heuristic for page residency; it is never used for shading. Components with no resolved
    bounds are treated as distance zero in the page cache.
18. **The engine VT system is not used.** NDGI's page cache is a custom slot table over
    BC7 `Texture2DArray` layers. Unreal's virtual-texture system is built around readable
    material/lightmap virtual textures and cannot be used for the writable lightmap output
    path, so NDGI manages its own physical slots and page table.
19. **The physical pool is sized at activation.** Changing `ndgi.ResidentModelsPerAtlas`
    after a cache is built does not resize the BC7 arrays; reload the level (or rebuild
    the cache) to apply a new pool size. Very small pools intentionally leave distant
    tiles on their baked lightmap until they stream in.
20. **The Lightmap Inspector currently reads live and merged sources only.** Individual
    `StepNN_TexNN.ndgi` browsing, UV-chart outlines, and baked-vs-decoded comparison are
    follow-up work. The live atlas can already contain an NDGI decode; reload the level
    before comparing it with the merged bake.
21. **Held-out-time validation is conservative.** The command removes one baked slice from
    an 8-slice set, doubling the temporal gap the model must interpolate compared with the
    runtime's adjacent-slice spacing. The 11-12 dB holdout numbers are therefore a lower
    bound; a 16-slice bake with every other slice held out would measure the production
    interpolation distance directly. The current 2-tile run is also an edge-of-atlas
    sample, not a full-map average.

---

## Validation status

| Command | Status |
|---|---|
| `ndgi.TestGPU` | PASS — weighted forward/backward match the CPU reference |
| `ndgi.TestRuntimeDecode` | PASS |
| `ndgi.TestLightmapSet 1500` | PASS (v3 serialization exercised by the bake/merge/train cycle) |
| `ndgi.TestCachedDecodeMatch` | PASS (test map + 19-atlas scene; 0 empty-tile changes) |
| `ndgi.ValidateSH 1.0` | PASS, 28.20 dB, 0 out-of-range |
| `ndgi.ValidateDirectionality 1.0` | PASS — mean 0.0057, p99 0.0802, p99.9 0.1839, max 0.4880 |
| `ndgi.ValidateSeams 1.0` | PASS — 0 introduced luminance, 8/2,886 introduced SH (max step 0.0216), continuous-boundary mean 0.00001 luminance / 0.00272 SH |
| `ndgi.ValidateTemporalHoldout 128 800 8 4 2` | PASS — 2 tiles, holdout 11.28 dB / baseline 13.91 dB |
| `ndgi.ValidateTemporalHoldout 128 3000 8 4 2` | PASS — 2 tiles, holdout 12.27 dB / baseline 20.59 dB |
| `ndgi.BenchmarkDecodeWarm 5` on stale bench map | FAILED as designed — 578/578 models skipped, pre-BC7 |
| `ndgi.BenchmarkDecodeWarm 50` on WP | PASS — p95 0.55 ms, 1 atlas, 4 models, 0 skipped |
| `ndgi.TestDecodeCadence 5` | PASS, 16.2 Hz |
| `ndgi.BenchmarkDecodeWarm 200` | PASS, engine-lightmap p95 6.08 ms; cold build 270 ms |
| `ndgi.TestAsyncCache` | PASS, 19 atlases, 578 models, 61 ms worst tick, 932 ms wall, 134.6 MB total |
| `ndgi.TestStreamingCells none` | PASS — 2 atlases → 0 active (GPU 0.0 MB, 0 queued), restored 2/2 |
| `ndgi.TestVTStreaming 2` on WP | PASS — 4 models, pool 2, peak resident 2, 4 page uploads, 2 evictions, BC7 texture reused across swaps, all decodes ok, GPU 0.06 MB |
| `ndgi.TestDiskStreaming 2` on WP | PASS — build loaded 0 model handles; 2 resident/2 loaded after the first update; 0 handles held after upload; 1 page uploaded after the camera move; texture reused; decode ok |
| `ndgi.TestAsyncPages 2` on WP | PASS — 2 load requests, 2 completions, 0 failures, 2 resident pages, 0 handles held, decode ok |
| `ndgi.VerifyStreamingDeps` on WP (after `ResaveLevelAsset`) | PASS — 4 model paths, 0 missing packages, 4 soft dependency edges |
| `ndgi.ReportStorage` on WP | PASS — raw BGRA8 TOD set 2.00 MB, raw feature assets 0.18 MB, BC7 assets 0.07 MB, 2.66×/30.30× ratios, 2.11 BPP |
| `ndgi.TestBC7` | PASS — 42.77 dB overall, max error 10, R/B order verified |
| `ndgi.TestLightmapInspector 0` on Sky_Temple | PASS — 1024×512 live atlas, 289,752 covered texels, mean |RGB| 0.00000, mean |SH| 0.00000 vs merged step 7; Tex05 passes at 0.00524/0.05007 |
| NDGI Lightmap Inspector tab | PASS — constructs on Sky_Temple, loads the live 1024×512 coefficient-0 atlas, and switches to merged step 7 without render-thread assertions |
| `ndgi.InspectorExport` on Sky_Temple | PASS — exported 1024×512 live and merged step-7 PNGs to `Saved/NDGI/Inspector`; both contain rendered atlas content |
| BC7 model export | PASS — WP atlas retrained at 10 and 800 steps; BC7 format provider encoded every layer |
| `ndgi.MaxCacheMemoryMB 1` + `TestAsyncCache` | PASS — rejected at 7.5 MB after one atlas |
| `ndgi.TestCancelAfter 2` during training | PASS — stopped after the current tile |
| `ndgi.ValidateLevelAsset 1.0` | PASS (includes luminance alpha) |
| NDGI Pipeline panel (`ndgi.Pipeline`) | PASS — dockable tab constructs in the editor; validation command runs; no Slate errors |
| Reports & diagnostics card | PASS — panel with the card and all-command browser constructs on `Lvl_ThirdPerson1`; `ndgi.ReportStorage` reports 19 atlases, 469 models, 19.13 MB BC7, 2.1246 BPP |
| TOD + model-quality cards | PASS — panel constructs on `Lvl_ThirdPerson1` with the default TOD profile; a synthetic 6-tile `TileMetrics.json` was parsed and the card loaded all 6 metrics; no Slate errors |
| `ndgi.RunPipeline 4 128 500 8` on WP | PASS — profile-driven bake, 2 atlases, 3 tiles, 3 models, `ValidateLevelAsset` PASSED |
| `ndgi.RunPipeline 8 128 800 8` on WP (sun-pitch fix) | PASS — re-baked/retrained 2 atlases, 3 models, 15,509/65,536 texels changed, decode OK |
| `ndgi.BakeTOD 2` on WP test map | PASS (`WP spike: PASSED`; level-scoped manifest with cell ownership) |
| `ndgi.CreateTODProfile` | PASS — 5 keyframes saved to `/Game/NDGI/TOD/NDGITODProfile` |
| `ndgi.TestTODProfile` | PASS — keyframes/interpolation, noon > night, noon/dawn light forward Z <= 0 |
| `ndgi.TestAutoCycle 3` (WP, `-game`) | PASS — time 0.000 → 0.605, 57 decodes |
| `ndgi.TestTrainGPU` | PASS — CPU/GPU training equivalence, max parameter diff 2.4e-7 |
| `ndgi.TestRuntimeSmoke` (editor `-game`) | PASS — 1 atlas, 20 models, 524,519 changed, 0 empty, 204,498 temporal |
| `RunNDGIPackage.ps1` (Development, DX12/SM6) | PASS — clean cook (0 errors), archive + packaged smoke |
| `RunNDGICI.ps1 -Mode packaged` | PASS — wrapper cook/archive/packaged-smoke path |
| `RunNDGICI.ps1 -Mode quality` | PASS — all kernel, cache-match, SH, directionality, seam, level, and runtime-smoke gates |
| `RunNDGICI.ps1 -Mode perf` | PASS — engine-lightmap p95 5.90 ms; decode cadence 16.2 Hz |
| `RunNDGICI.ps1 -Mode memory` | PASS — 134.6 MB total, worst async tick 49.93 ms; 1 MB guard rejects the first atlas |

---

## Next steps (priority order)

1. **Re-bake and retrain the benchmark map** with BC7, then measure CPU/GPU cache size,
   BPP, decode p50/p95, and the `ndgi.ResidentModelsPerAtlas` memory/quality trade-off.
   The stale-asset guard now fails the perf/memory gates on the old map (578/578 models
   skipped), so this is required before those gates can pass again. The current WP asset
   is BC7 and has been measured (see the table), but it is a tiny 4-model scene.
2. **Quality check at production steps** (800/3000): the leave-one-slice-out command and
   baseline comparison are implemented and measured on two Sky Temple tiles. Next run the
   full command at 3000 steps on a representative set of tiles, then compare with the
   production 8-slice model. Note that leave-one-out doubles the temporal gap relative to
   runtime adjacent-slice interpolation, so it is a conservative lower bound.
3. **Custom chunk store**: replace per-page `.uasset` loads with a single atlas page file
   and offset table if package load overhead becomes the streaming bottleneck.
4. **RTX 3060 measurement** of `BenchmarkDecodeWarm` / `TestDecodeCadence` (hardware item).
5. **Multi-cell World Partition scene**: validate several cells loading/unloading concurrently
   and measure active-atlas and page churn. Per-cell atlases/training would require per-cell bakes.
7. **Network playtest**: verify replicated `TimeOfDay`/`Profile` and auto-cycle on a dedicated
   server + 2 clients.
8. **Shipping-config package**: run `RunNDGIPackage.ps1 -ClientConfig Shipping` and the
   packaged smoke test on the release machine (Development package already verified).
9. **UI polish**: per-tile ETA and the per-atlas memory/BC7 breakdown are done, along with
   the project-health card, atlas table, holdout preview, report/diagnostic quick actions,
   the searchable all-commands browser, the TOD keyframe/preview card with quick edits, and
   the worst-tile list. Remaining: async merge/validation.

---

## Key files

- `Source/NDGIRuntime/` — runtime cache, atlas decoder, level actor, TOD profile,
  benchmarks (`TestStreamingCells`, `TestAsyncCache`, `TestAutoCycle`).
- `Source/NDGI/` — GPU MLP/training kernels and shader bindings.
- `Source/NDGIEditor/` — bake/export, training orchestration, validation, preview,
  `NDGIEncodingMap.*` (per-component HQ range map + cell ownership), `NDGIBakeManifest.*`
  (level-scoped atlas/cell list), `NDGIPipelinePanel.cpp` (editor UI),
  `NDGITrainAtlas.cpp` (training; writes per-tile PSNR metrics),
  `NDGIPipelineStatus.*` (progress/log capture and full-pipeline runner).
- `Shaders/Private/` — `NDGIMLP.ush`, `NDGITile.ush`, `NDGIDecodeAtlas.usf`, training shaders.
- `Source/NDGIRuntime/Private/NDGIRuntimeSmoke.cpp` — cooked/packaged
  `ndgi.TestRuntimeSmoke` validation.
- `Tools/RunNDGIBenchmark.ps1` — benchmark suite launcher.
- `Tools/RunNDGIPackage.ps1`, `Tools/RunNDGICI.ps1`, `Tools/NDGITools.ps1` — packaged
  build/smoke and CI gates.
- `Tools/Restore-NDGIQuarantine.ps1` — restores content after an interrupted package run.
- `README.md` — command reference and shipping/CI guide.
