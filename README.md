# DKC2Recomp HD

High-resolution *Donkey Kong Country 2* on a native static recompilation.

DKC2Recomp HD adds an HD compositor that redraws the game at 2x-8x, with
modern art for the Kongs, objects, terrain and title screen built locally
from your own ROM. The recompilation that runs the game is
[elliotttate/DKC2Recomp](https://github.com/elliotttate/DKC2Recomp); for its
features, controls, co-op, build and ROM requirements, see the
[upstream README](https://github.com/elliotttate/DKC2Recomp#readme).

## Why this is different

An emulator, or the upstream build, hands its scaler one flat 256x224 image:
a filter cannot tell Diddy from the floor or the sky. DKC2Recomp HD keeps the
frame in pieces:

- **Layers with depth.** The PPU exports BG1-BG3 and sprites separately, and
  every pixel carries its composition priority (what is in front of what).
- **Known objects.** The game's sprite table says which object each sprite
  is, where it stands and which graphic it shows.
- **Recomposition.** The frame is rebuilt at output resolution from those
  pieces with the console's priority rules, instead of upscaling the
  finished image.

That is what makes replacing Diddy's art, matching terrain blocks and,
next, lighting and depth effects possible.

## What it adds

- **HD sprites.** Kongs, enemies and items are drawn at full output
  resolution, from redrawn art when the pack has the frame, otherwise
  filtered from the ROM frame.
- **HD terrain.** BG1 blocks are replaced by pack art when they match the
  native capture.
- **HD still screens.** Full-screen art (the title) is matched against a
  native BG1 reference and keeps sprites and fades on top.
- **Better fallback filter.** Everything else uses a premultiplied
  Catmull-Rom upscale with anti-ringing (no halos around ropes or edges), or
  square pixels.
- **GPU composition.** The desktop app draws each HD frame on the GPU
  (OpenGL, over Metal on macOS) from a scene the compositor decides at 1x;
  the CPU compositor remains for headless runs and as the reference.
- **Widescreen and fades** keep working at every scale. Scenes that draw
  terrain through the subscreen (Mainbrace Mayhem) keep it.
- **Settings tab options**, remembered between runs: HD graphics, scale
  (2x-8x), sprite and background filters, HD art pack.
- **Pack tooling.** `DKC2_HD_DUMP` captures sprite frames, terrain blocks and
  still screens. `scripts/hd_pack.py` lays them out for an image model, then
  cuts the redrawn grids back into frames: alignment, silhouette checks, and
  white-matte and sliver removal.

## Gains

| | Upstream | DKC2Recomp HD |
| --- | --- | --- |
| Output | 256x224 frame, scaled as a picture | Recomposed at 512x448-2048x1792 (wider in widescreen) |
| Kongs and objects | Original pixels | Full-resolution redraws (with a local pack) |
| Title screen | Original | Redrawn HD art (with a local pack) |
| Edges when scaled | Blocky or blurred | Smooth without ringing halos |
| CPU per frame at 4x (Apple M4, whole game, recorded Pirate Panic route) | n/a | ~4.7 ms with GPU composition (~9.5 ms on the CPU) |

## Status

| Area | State |
| --- | --- |
| Compositor, priorities, widescreen, fades | Working |
| Diddy/Dixie redrawn frames | ~900 of ~950 graphics; a few being re-cut |
| Common objects (barrels, banana bunches, explosions, balloons) | First pass |
| Terrain art | Pirate Panic |
| Title screen art | Working |
| Sky/water (BG2/BG3), loose objects (single bananas) | Filter only |
| Mounts, most enemies | Not covered yet |
| Nintendo Switch homebrew build | Builds; not yet run on hardware ([docs](docs/CROSS_PLATFORM.md#nintendo-switch-homebrew-build-experimental)) |

## Roadmap: a remaster without a pack

The layer and object data make these possible on the original graphics:

- Soft character shadows on the terrain.
- Atmospheric depth: haze and slight blur on background layers.
- Bloom and light from fire, explosions, torches and pickups.
- Sub-pixel camera smoothing at HD scales.
- Lighting and rim light on the pre-rendered sprites, per level.
- Water reflections and refraction; per-level color grading.
- GPU compositing, pixel-art base filter, BG2 and loose-object art, and
  build-time terrain capture for every level.

Each item is tracked in the [issues](https://github.com/dantopa/DKC2Recomp/issues).

## Using it

Build exactly as upstream; the HD layer is part of the SDL and desktop
targets. Enable it in the in-game **Settings** tab. For headless and
scripted runs use `DKC2_HD=<2-8>`, `DKC2_HD_FILTER=nearest|smooth|learned|learned-pixel`,
`DKC2_HD_PACK=<dir>` and `DKC2_HD_DUMP=<dir>`. The pack format is documented
in `runner/dkc2_hd.h`.

**No art ships with this repository.** Packs are derivative of Nintendo and
Rare content: build yours from your own ROM and keep it in `private/` or the
user-data `hdpack` folder. See [docs/LEGAL_AND_CONTENT.md](docs/LEGAL_AND_CONTENT.md).

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md). The license and third-party notices
are those of the upstream project: [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
