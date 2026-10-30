# Cross-platform desktop foundation

## Current status

The game/runtime code and the new `dkc2_snesrecomp_sdl` gameplay host are
written for Windows, Linux, and macOS. The SDL2 host provides the same minimum
playable surface on each platform:

- accelerated, resizable 4:3 video with nearest or bilinear scaling;
- Raw, CRT, Composite, and Trinitron screen-color modes;
- keyboard and two hot-pluggable SDL game controllers;
- queued 32,040 Hz signed-16 stereo audio;
- SRAM, five selectable save/load slots, 3x fast-forward, and bounded rewind;
- the shared `recomp-ui` launcher and in-game OpenGL pause overlay; and
- the FPS window-title readout.

This is not yet a three-platform release claim. The SDL target compiles and
completes its private-ROM integration test on Windows. On Apple silicon it now
also builds as a native `.app`, completes the configured native/private CTest
suite, and has visible acceptance for launch, menus, fullscreen/aspect changes,
gameplay, audio, frame pacing, and the first three west-boundary scenes. Broad
controller coverage, notarization, and a public Mac release remain open. Linux
still requires native compiler/runtime acceptance, packaging, and visible
controller/audio testing.

The established Win32 `dkc2_snesrecomp_desktop` host remains in place during
that acceptance work. It is not silently replaced by the SDL host.

## Private source generation

The portable generator uses only Python, Rust/Cargo, and the pinned
`snesrecomp` submodule:

```sh
python3 scripts/generate_snesrecomp.py --rom /private/path/dkc2.sfc
```

The PowerShell entry point remains supported on Windows. Both verify the exact
4 MiB payload and SHA-256 before generating ignored C under
`generated/snesrecomp/`. Neither copies the ROM into the repository.

## Linux build

Install a C/C++ compiler, CMake, Python 3, Rust/Cargo, OpenGL development
headers, and SDL2 development files. For example, Debian/Ubuntu package names
include `build-essential`, `cmake`, `ninja-build`, `python3`, `cargo`,
`libsdl2-dev`, and `libgl1-mesa-dev`.

```sh
git submodule update --init --recursive
./build_macos.sh /private/path/dkc2.sfc
open build/macos/DKC2Recomp.app
```

`build_macos.sh` generates ignored private sources when necessary, creates the
native `DKC2Recomp.app`, bundles the linked SDL3 dylib, fixes its install name,
ad-hoc signs and verifies the bundle, and registers it with LaunchServices. The
Mac host stores mutable files under
`~/Library/Application Support/Flat2VR/DKC2Recomp`; nothing private is copied
into the app. Set `DKC2_MACOS_DEPLOYMENT_TARGET` to override the default 14.0
deployment floor. If the installed SDL3 dylib requires a newer system, the
script raises the effective target to that dependency's declared minimum and
reports it rather than producing a bundle with conflicting version claims.
Ad-hoc signing is suitable for local testing only and is not notarization.

## macOS build

Install the Xcode command-line tools plus CMake, Python 3, Rust/Cargo,
pkg-config and SDL3. Homebrew can provide the non-Xcode dependencies. The
macOS host builds against SDL3 by default; configure with
`-DSNESRECOMP_SDL_BACKEND=SDL2` to build it against SDL2 instead.

```sh
git submodule update --init --recursive
python3 scripts/generate_snesrecomp.py --rom /private/path/dkc2.sfc
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDKC2_BUILD_SNESRECOMP=ON
cmake --build build-release --target dkc2_snesrecomp_sdl
./build-release/DKC2Recomp /private/path/dkc2.sfc
```

CMake prefers an installed SDL2 package. If none is found,
`DKC2_FETCH_SDL2=ON` (the default) fetches the pinned SDL 2.30.9 source. Set it
to `OFF` for fully offline/system-package-only builds.

## Diagnostics portability

Both desktop hosts write the same rolling JSON report and allowlisted support
bundle format. Windows unhandled exceptions include a minidump. On Linux and
macOS, a fatal-signal handler writes only a small marker using signal-safe
operations; the next launch turns it into the complete bundle. Native Linux
and macOS acceptance must deliberately exercise this recovery path as well as
normal clean-exit reporting. Reports exclude ROM paths/data and all saves, but
include loaded-module paths and basic machine/OS information.

## Nintendo Switch homebrew build (experimental)

The SDL host also builds as a homebrew `.nro` for a console running
Atmosphere, with devkitPro's libnx, `switch-sdl2` and Mesa. The official
`devkitpro/devkita64` image has every dependency, so no host install is
needed:

```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 bash -lc '
  cmake -S . -B build/switch -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DDKC2_BUILD_SNESRECOMP=ON -DSNESRECOMP_SDL_BACKEND=SDL2 &&
  cmake --build build/switch --target dkc2_snesrecomp_sdl_nro'
```

Copy `build/switch/DKC2Recomp.nro` and `build/switch/assets/` to
`sdmc:/switch/DKC2Recomp/` and put the ROM there as `dkc2.smc`. The host
always works in that folder, so `rom.cfg`, `launcher.cfg` and `saves/` stay
there. Launch the `.nro` through a game's title override (hold R while
starting a game): the album applet leaves too little memory.

An installable title (`.nsp`, program id `01d2c20000000000`) runs from the
same folder with full application memory. It needs hacBrewPack (the original
repository is gone; `rlaphoenix/hacBrewPack` mirrors it) and the `prod.keys`
dumped from your own console with Lockpick_RCM, at `~/.switch/prod.keys`.
Inside the same image, from a work directory:

```sh
elf2nso build/switch/DKC2Recomp.elf exefs/main
npdmtool assets/switch/DKC2Recomp.npdm.json exefs/main.npdm
cp build/switch/DKC2Recomp.nacp control/control.nacp
for language in AmericanEnglish BritishEnglish Japanese French German \
    LatinAmericanSpanish Spanish Italian Dutch CanadianFrench Portuguese \
    Russian Korean TraditionalChinese SimplifiedChinese BrazilianPortuguese; do
  cp assets/switch/icon.jpg control/icon_$language.dat
done
hacbrewpack -k ~/.switch/prod.keys --titleid 01d2c20000000000 \
  --titlename DKC2Recomp --titlepublisher Flat2VR --noromfs --nologo \
  --exefsdir exefs --controldir control --nspdir nsp
```

The HOME menu shows the icon of the console language, so every language
gets one; it must be a 256x256 baseline JPEG without EXIF data, or the menu
shows "?". `assets/switch/icon.jpg` is the Switch icon in that form (original art
generated for this port; the `.nro` uses it too).

Installing it requires sigpatches and an installer such as DBI. Never commit
or share the keys or the packed title.

Platform differences:

- the launcher and overlay use OpenGL ES 2 (as recomp-ui does on Android);
  the presenter keeps its GL 2.1 context, and the fixed-function calls the
  static Mesa library does not export are resolved from that context
  (`runner/switch_host.c`);
- the game starts without the launcher (`--launcher` still opens it); a new
  configuration defaults to 16:9 and HD at 2x, which the 720p screen (1080p
  docked) shows in full, and the in-game menu changes either;
- file dialogs report a cancel, so paths come from the ROM default above and
  the saved configuration;
- MSU-1 PCM tracks are unavailable (Horizon cannot map files and the set does
  not fit in memory); the SPC soundtrack plays;
- no POSIX crash-signal handler or machine report: Atmosphere records crashes.
- `perf.log` in the data folder records, every 300 presented frames, the
  frame rate, the mean milliseconds of the recompiled CPU, drawing (split
  into the PPU frame and the HD scene), audio and presentation, the GL
  renderer and the HD composition path; `stdout.log` and `stderr.log` beside
  it keep the console streams, unless nxlink started the app with `-s`, which
  receives them instead.

Benchmark: `--bench SECONDS [--hd 0|2-8]` starts without the launcher,
replays the route recorded in `bench.input` in the data folder, echoes the
`perf.log` lines to stdout, prints `bench: done result=N` and quits. `--hd`
sets the HD scale for that run only (0 turns HD off); saves and options are
not written. From the computer:

```sh
nxlink -a <switch-ip> -s DKC2Recomp.nro --bench 60 --hd 2
```

The build boots on hardware (firmware 23.0.1) and holds 60 fps without HD
(about 2 ms recompiled CPU and 3.6 ms drawing per frame at 1785 MHz). HD at
2x/4x drops to 43-59 fps: drawing grows to 9-13 ms.

## Native acceptance required

Before checking off either operating system, run these checks on that system:

1. Configure and compile a clean Release build with strict project warnings.
2. Run the complete public test suite.
3. Run the private hidden SDL smoke test with video, audio, fast-forward, and a
   real rewind restore.
4. Watch and listen through at least one complete attract cycle.
5. Test keyboard plus two representative controllers, hot-plugging, SRAM,
   state save/load, fullscreen, resize, and every screen-color mode.
6. Produce a source-clean package that contains no ROM, generated C, saves, or
   captures.
7. Exercise clean requested diagnostics and the platform crash path; confirm
   that the bundle allowlist contains no ROM or save artifact.

The native `.app`, Application Support migration, and local ad-hoc signing are
implemented. Developer-ID signing/notarization and a distributable Mac archive
remain separate roadmap tasks, as does AppImage/Flatpak-style packaging on
Linux.
