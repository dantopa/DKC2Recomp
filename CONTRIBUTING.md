# Contributing

Thanks for helping. This fork focuses on the HD presentation layer; the
recompilation itself is maintained upstream at
[elliotttate/DKC2Recomp](https://github.com/elliotttate/DKC2Recomp). Fixes that
do not depend on HD belong upstream.

## Ground rules

- **Never commit game content.** No ROM, save, extracted graphics, generated
  HD art, terrain blocks, screenshots or audio. Use `private/`, `build/` or
  `generated/` (all ignored). See [docs/LEGAL_AND_CONTENT.md](docs/LEGAL_AND_CONTENT.md).
- Bring your own lawfully obtained ROM. Private integration tests read it from
  an external path.
- Follow the existing style of each file (C11 in `runner/`, Python 3 standard
  library only in `scripts/`). Keep changes focused; no drive-by refactors.
- Update `CHANGELOG.md` and the relevant `docs/` page when behavior changes.

## Building and testing

```bash
python3 scripts/generate_snesrecomp.py --rom /path/to/dkc2.sfc
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DDKC2_BUILD_SNESRECOMP=ON
cmake --build build-release
(cd build-release && ctest --output-on-failure)
python3 tests/test_hd_pack.py
```

Windows and full platform instructions are in [README.md](README.md#building-from-source).
HD changes should be checked with the headless runner, for example:

```bash
DKC2_HD=4 DKC2_ASPECT=16:9 DKC2_HD_PACK=private/hdpack \
  build-release/dkc2_snesrecomp_headless /path/to/dkc2.sfc 1500
```

Known: the six `supplied_rom_coop_*` integration tests currently fail on this
fork's base and are unrelated to HD.

## Finding work

Open tasks are tracked as GitHub Issues on this fork. Labels:

- `hd-engine`: compositor and pack-format work in `runner/dkc2_hd.c`.
- `hd-pack-tool`: `scripts/hd_pack.py` (grids, cutting, validation).
- `art`: pack generation workflows (no art is accepted into the repo).
- `good first issue`: small, self-contained tasks.

Comment on an issue before starting so work is not duplicated. AI agents are
welcome: read `AGENTS.md` first and follow the same rules.

## Pull requests

- One topic per PR, with a description of what changed, why, and how it was
  verified (commands and, for visual changes, a description of the frames
  checked; do not attach copyrighted screenshots).
- Unit tests for tooling changes; headless verification for engine changes.
- CI-equivalent check before opening: build, `ctest`, `tests/test_hd_pack.py`.
