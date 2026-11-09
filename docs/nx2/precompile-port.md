# Precompiled shaders from recipes (port from Fritchii)

Branch `oot-precompile`: `local` + latest `nx2` + zortos293/Lichii#896 (stacked on #895) + the
player-facing layer of [Frizti/Fritchii](https://github.com/Frizti/Fritchii) v5.x.

## Where each part comes from

| Part | Source |
|---|---|
| Recipe format, shader extraction, `nx2-precompile` tool, canonical native keys | #895 (merged as is) |
| Preparation at boot, kept infos and SPIR-V, fast path (`EDEN_NX2_PRECOMPILED_FAST`, on) | #896 (merged as is) |
| Recipe found and assembled automatically (`video_core/nx2_precompile.*`, core's `SetGameFiles`) | Fritchii `223819b388`, `3d216bc066` |
| A new build keeps the preparation when a sample translates the same (`CheckKeptSample`) | Fritchii `00b7a78e30`, adapted to #896's `NX2INFO3` file |
| "Preparing shaders N / M" loading stage with Skip | Plain-UI stand-in for Fritchii's `precompile_gate` |
| `dist/recipes/OoT v131072 (cache v54).nx2recipe`, copied to `<exe>/recipes/` | Fritchii v5.3 release |

Not ported: Fritchii's own `vk_precompiled_kept` re-implementation of #896 (it depends on its
translate queue, GPL warm-ups and attribute keying, none of which this tree has), the Fritchii UI,
idle-priority background preparation while playing.

## Flow at boot (NVIDIA / AMD proprietary drivers only)

1. `LoadDiskResources` → `PreparePrecompiled(title_id, ...)`.
2. `Nx2Precompile::EnsurePrecompiledFile`: uses `<shader>/<title>/precompiled.fork-v54.nx2prep`,
   or assembles one from the first matching recipe (`EDEN_NX2_PRECOMPILE_RECIPE`, the title's
   shader folder, `<exe>/recipes/`). A `.recipe.txt` note beside it names the recipe by size and
   hash; a file without the note (game list menu, tools) is the player's and wins.
3. Stamp `precompiled_prepared.fork-v54.txt` = file size, time, driver, build, GPU.
   - Same stamp: load kept infos, done.
   - Only the build differs: `CheckKeptSample` translates 64 graphics + 16 compute kept pipelines
     again; equal → stamps rewritten, done; else prepare again.
4. Preparation: loading screen shows "Preparing shaders N / M" (about 120k pipelines for OoT).
   Skip starts the game; the checkpoint resumes the rest on a later boot.

Switches: `EDEN_NX2_PRECOMPILED=0` off, `=1` also on drivers without their own cache;
`EDEN_NX2_PRECOMPILED_FORCE=1` prepare again; `EDEN_NX2_PRECOMPILE_THREADS` extraction threads.

## Known limits

- Skip (or closing) mid-preparation drops the kept infos of the records already built (#896
  writes the kept file only at the end); the resumed run keeps only the rest.
- The build in the stamp is the commit: uncommitted translator changes do not trigger the check.

## Status

- 2026-11-09: code ported, uncommitted. Not compiled yet (the owner builds it in Visual Studio),
  not run in game yet.
