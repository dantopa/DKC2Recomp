#include "dkc2_kongs.h"
#include "dkc2_music.h"
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "desktop/sdl_compat.h"

#include "dkc2_game.h"
#include "dkc2_coop.h"
#include "dkc2_hd.h"
#include "dkc2_video.h"
#include "diagnostics.h"
#include "desktop_filter.h"
#include "desktop_audio_rate.h"
#include "desktop_fps.h"
#include "desktop_pacer.h"
#include "desktop_input.h"
#include "desktop_launcher.h"
#include "desktop_overlay.h"
#include "desktop_paths.h"
#include "desktop_present_sdl.h"
#include "desktop_viewport.h"
#include "desktop_rewind.h"
#include "input_playback.h"
#include "input_recording.h"
#include "verified_rom.h"

#ifdef __APPLE__
#include "macos_display_link.h"
#include "macos_host.h"
#endif

#include "common_rtl.h"
#include "host_report.h"
#include "launcher.h"
#include "snes/snes.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <SDL_syswm.h>
#include "windows_menu.h"
#include "desktop_menu_settings.h"
#define DKC2_MKDIR(path) _mkdir(path)
#else
#define DKC2_MKDIR(path) mkdir(path, 0755)
#endif

#ifndef DKC2_RELEASE_VERSION
#define DKC2_RELEASE_VERSION "dev"
#endif

enum {
  kFrameBufferWidth = kDkc2VideoMaximumWidth,
  kFrameHeight = kDkc2VideoHeight,
  kBytesPerPixel = 4,
  kAudioRate = 32040,
  kAudioChannels = 2,
  kMaximumFrameAudio = 534,
  /* Rate control stretches a frame's audio by at most half a percent, so
   * the stretched frame needs a few frames of slack. */
  kAudioStretchSlack = 16,
  kHostSpeedMultiplier = 3,
  kRewindSnapshotInterval = 3,
  kRewindSnapshotCapacity = 300,
  kMaximumControllers = 2,
  kPathCapacity = 4096,
};

static const double kVideoRate = 60.098811862;
/* Audio rate control: the queue is held near a target by stretching each
 * frame's samples within this deviation, reaching the full deviation when
 * the average fill is a quarter of the target away (gain 4). */
static const double kAudioRateDeviation = 0.005;
static const double kAudioRateGain = 4.0;
static const double kAudioFillWeight = 0.02;
/* A display lock is accepted when the display's frame cadence is within
 * this fraction of the cartridge's rate; ticks stalled this long release
 * the lock to the host clock. */
static const double kDisplayLockTolerance = 0.02;
static const double kDisplayTickTimeoutSeconds = 0.05;

typedef struct SdlHost {
  Dkc2SdlPresenter presenter;
  Dkc2DesktopColorFilter color_filter;
#if SNESRECOMP_SDL3
  SDL_AudioStream *audio_stream;
#endif
  SDL_AudioDeviceID audio_device;
  SDL_GameController *controllers[kMaximumControllers];
  bool haptic_pulse_active;
  uint8_t pixels[kFrameBufferWidth * kFrameHeight * kBytesPerPixel];
  uint8_t filtered_pixels[
      kFrameBufferWidth * kFrameHeight * kBytesPerPixel];
  int16_t scaled_audio[(kMaximumFrameAudio + kAudioStretchSlack) *
                       kAudioChannels];
  int16_t stretched_audio[(kMaximumFrameAudio + kAudioStretchSlack) *
                          kAudioChannels];
  Dkc2AudioStretch audio_stretch;
  double audio_fill_average; /* queued frames, negative until measured */
  int audio_device_frames;   /* frames the device takes per pull */
  int player_source[kDkc2DesktopPlayerCount];
  int player_deadzone[kDkc2DesktopPlayerCount];
  int player_key_bind[kDkc2DesktopPlayerCount]
                     [RECOMP_LAUNCHER_MAX_BINDINGS];
  int player_pad_bind[kDkc2DesktopPlayerCount]
                     [RECOMP_LAUNCHER_MAX_BINDINGS];
  int assist_key_bind[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
  int assist_pad_bind[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
  int audio_volume;
  Dkc2DesktopOverlay *overlay;
  bool audio_available;
  bool audio_primed;
  bool running;
  bool hidden;
  bool menu_chord_previous;
  Uint8 key_taps[SDL_NUM_SCANCODES];
  bool escaped_fullscreen;
  /* HD presentation: the compositor (NULL while off) and the configuration
   * it was built with, which DKC2_HD fixes for the run; the composed frame
   * of the last draw, or NULL to present the authentic frame in pixels. */
  Dkc2Hd *hd;
  Dkc2HdConfig hd_config;
  const char *hd_pack_dir; /* used while the HD art pack option is on */
  bool hd_environment;
  const uint8_t *rom;
  size_t rom_size;
  const uint32_t *hd_pixels;
  int hd_width;
  int hd_height;
  /* With the GPU path the last frame is a scene the presenter draws, and
   * hd_pixels stays NULL. DKC2_HD_CPU=1 keeps the CPU compositor, the
   * reference the GPU path is checked against. */
  bool hd_gpu;
  const Dkc2HdScene *hd_scene;
  /* HD tooling: the pack reloads between frames on F5 or when its folder
   * changes; F6 toggles the inspector, F7 copies the inspected name. */
  bool hd_reload;
  time_t hd_pack_time;
  unsigned hd_pack_check;
  bool hd_inspector;
  bool hd_copy_name;
  char hd_inspected[24];
#ifdef _WIN32
  Dkc2WindowsMenu *menu;
#endif
} SdlHost;

/* Renders the PPU frame, through the HD compositor when it is enabled. */
static void DrawFrame(SdlHost *host) {
  Dkc2HdBeginFrame(host->hd, g_ppu, Dkc2VideoWidth());
  Dkc2DrawPpuFrame();
  const size_t pitch = (size_t)Dkc2VideoWidth() * kBytesPerPixel;
  if (host->hd_gpu) {
    host->hd_scene =
        Dkc2HdEndFrameScene(host->hd, g_ppu, g_ram, Dkc2GetFrameLines(),
                            host->pixels, pitch);
    return;
  }
  host->hd_pixels = Dkc2HdEndFrame(host->hd, g_ppu, g_ram, Dkc2GetFrameLines(),
                                   host->pixels, pitch, &host->hd_width,
                                   &host->hd_height);
}

typedef struct SdlControls {
  uint32_t controller;
  uint32_t host_actions;
} SdlControls;

typedef enum SdlSpeedMode {
  kSdlSpeedNormal,
  kSdlSpeedRewind,
  kSdlSpeedFastForward,
} SdlSpeedMode;

#ifdef __APPLE__
static bool EnvironmentDisabled(const char *name) {
  const char *value = getenv(name);
  if (!value || !*value) return false;
  return strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
         strcmp(value, "off") == 0 || strcmp(value, "no") == 0;
}
#endif

static bool EnvironmentEnabled(const char *name) {
  const char *value = getenv(name);
  return value && *value && *value != '0';
}

static int ClampInt(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static bool EnsureSaveDirectory(void) {
  if (DKC2_MKDIR("saves") == 0) return true;
  return errno == EEXIST;
}

static void ShowError(const char *message) {
  fprintf(stderr, "%s\n", message ? message : "Unknown error");
  if (SDL_WasInit(SDL_INIT_VIDEO))
    (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                   "Unable to start DKC2",
                                   message ? message : "Unknown error", NULL);
}

static bool WriteFramePpm(const char *path, const uint8_t *pixels) {
  const int frame_width = Dkc2VideoWidth();
  FILE *stream = fopen(path, "wb");
  if (!stream) return false;
  bool ok = fprintf(stream, "P6\n%d %d\n255\n", frame_width,
                    kFrameHeight) > 0;
  for (int y = 0; ok && y < kFrameHeight; y++) {
    const uint8_t *row =
        pixels + (size_t)y * frame_width * kBytesPerPixel;
    for (int x = 0; ok && x < frame_width; x++) {
      const uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
      ok = fwrite(rgb, 1, sizeof rgb, stream) == sizeof rgb;
    }
  }
  if (fclose(stream) != 0) ok = false;
  return ok;
}

#if SNESRECOMP_SDL3
#define EVENT_SCANCODE(event) ((event).key.scancode)
#else
#define EVENT_SCANCODE(event) ((event).key.keysym.scancode)
#endif

static bool ControllerHasRumble(SDL_GameController *controller) {
#if SNESRECOMP_SDL3
  return SDL_GetBooleanProperty(SDL_GetGamepadProperties(controller),
                                SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
#else
  return SDL_GameControllerHasRumble(controller);
#endif
}

static bool RumbleController(SDL_GameController *controller, Uint16 low,
                             Uint16 high, Uint32 milliseconds) {
#if SNESRECOMP_SDL3
  return SDL_RumbleGamepad(controller, low, high, milliseconds);
#else
  return SDL_GameControllerRumble(controller, low, high, milliseconds) == 0;
#endif
}

static SDL_GameController *HapticController(SdlHost *host, int player) {
  int target = Dkc2GamepadIndexForPlayer(host->player_source, player);
  if (target < 0) return NULL;
  for (int i = 0; i < kMaximumControllers; ++i) {
    SDL_GameController *controller = host->controllers[i];
    if (!controller || !SDL_GameControllerGetAttached(controller)) continue;
    if (target-- == 0) return controller;
  }
  return NULL;
}

static void PulseHaptics(SdlHost *host, unsigned players, bool test) {
  if (!Dkc2LauncherHaptics() || !(SDL_GetWindowFlags((SDL_Window *)host->presenter.window) & SDL_WINDOW_INPUT_FOCUS)) return;
  for (int player = 0; player < 2; ++player) {
    SDL_GameController *controller = HapticController(host, player);
    if ((players & (1u << player)) && controller && ControllerHasRumble(controller)) {
      if (RumbleController(controller, test ? 0x5000 : 0x2800,
                           test ? 0x7000 : 0x5000, test ? 500 : 55))
        host->haptic_pulse_active = true;
    }
  }
}

static void UpdateHaptics(SdlHost *host) {
  bool stop = !Dkc2LauncherHaptics() || !(SDL_GetWindowFlags((SDL_Window *)host->presenter.window) & SDL_WINDOW_INPUT_FOCUS);
  for (int i = 0; stop && host->haptic_pulse_active && i < kMaximumControllers; ++i)
    if (host->controllers[i]) (void)RumbleController(host->controllers[i], 0, 0, 0);
  if (stop) host->haptic_pulse_active = false;
  for (int player = 0; player < 2; ++player) {
    SDL_GameController *controller = HapticController(host, player);
    bool supported = controller && ControllerHasRumble(controller);
    Dkc2DesktopOverlaySetHapticsDevice(host->overlay, player,
        controller ? (supported ? SDL_GameControllerName(controller) : "Gamepad has no rumble") : NULL, supported);
  }
}

static void CloseControllers(SdlHost *host) {
  for (int i = 0; i < kMaximumControllers; i++) {
    if (host->controllers[i]) {
      (void)RumbleController(host->controllers[i], 0, 0, 0);
      SDL_GameControllerClose(host->controllers[i]);
    }
    host->controllers[i] = NULL;
  }
}

static void RefreshControllers(SdlHost *host) {
  CloseControllers(host);
  int opened = 0;
#if SNESRECOMP_SDL3
  int count = 0;
  SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
  for (int i = 0; gamepads && i < count && opened < kMaximumControllers; i++) {
    SDL_Gamepad *controller = SDL_OpenGamepad(gamepads[i]);
    if (controller) host->controllers[opened++] = controller;
  }
  SDL_free(gamepads);
#else
  for (int device = 0;
       device < SDL_NumJoysticks() && opened < kMaximumControllers;
       device++) {
    if (!SDL_IsGameController(device)) continue;
    SDL_GameController *controller = SDL_GameControllerOpen(device);
    if (controller) host->controllers[opened++] = controller;
  }
#endif
}

static void PumpEvents(SdlHost *host) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        EVENT_SCANCODE(event) == SDL_SCANCODE_ESCAPE &&
        Dkc2DesktopEscapeExitsFullscreen(
            Dkc2SdlPresenterIsFullscreen(&host->presenter),
            Dkc2DesktopOverlayIsOpen(host->overlay))) {
      if (Dkc2SdlPresenterSetFullscreen(&host->presenter, false)) {
        host->escaped_fullscreen = true;
        continue;
      }
    }
#ifdef _WIN32
    if (event.type == SDL_KEYDOWN && !event.key.repeat &&
        event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT)) {
      SDL_SysWMinfo info;
      SDL_VERSION(&info.version);
      if (SDL_GetWindowWMInfo((SDL_Window *)host->presenter.window, &info))
        PostMessageW(info.info.win.window, WM_COMMAND, kDkc2MenuFullscreen, 0);
      continue;
    }
#endif
    bool consumed =
        Dkc2DesktopOverlayProcessSdlEvent(host->overlay, &event);
    if (consumed) continue;
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
      const SDL_Scancode key = EVENT_SCANCODE(event);
      if (key == SDL_SCANCODE_F5) host->hd_reload = true;
      if (key == SDL_SCANCODE_F6) host->hd_inspector = !host->hd_inspector;
      if (key == SDL_SCANCODE_F7) host->hd_copy_name = true;
      if (key == SDL_SCANCODE_F5 || key == SDL_SCANCODE_F6 ||
          key == SDL_SCANCODE_F7)
        continue;
    }
    if (event.type == SDL_KEYDOWN && !event.key.repeat &&
        EVENT_SCANCODE(event) > SDL_SCANCODE_UNKNOWN &&
        EVENT_SCANCODE(event) < SDL_NUM_SCANCODES)
      host->key_taps[EVENT_SCANCODE(event)] = 1;
#if SNESRECOMP_SDL3
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
#else
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
#endif
      memset(host->key_taps, 0, sizeof host->key_taps);
    if (event.type == SDL_QUIT) host->running = false;
    if (event.type == SDL_CONTROLLERDEVICEADDED ||
        event.type == SDL_CONTROLLERDEVICEREMOVED)
      RefreshControllers(host);
  }
}

static uint32_t ReadGamepadButtons(SDL_GameController *controller) {
  uint32_t buttons = 0;
#define MAP_SDL_BUTTON(sdl_button, dkc2_button)                            \
  do {                                                                     \
    if (SDL_GameControllerGetButton(controller, (sdl_button)))             \
      buttons |= (dkc2_button);                                            \
  } while (0)
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_UP, kDkc2GamepadDpadUp);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDkc2GamepadDpadDown);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDkc2GamepadDpadLeft);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDkc2GamepadDpadRight);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_START, kDkc2GamepadStart);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_BACK, kDkc2GamepadBack);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                 kDkc2GamepadLeftShoulder);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                 kDkc2GamepadRightShoulder);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_A, kDkc2GamepadA);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_B, kDkc2GamepadB);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_X, kDkc2GamepadX);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_Y, kDkc2GamepadY);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_GUIDE, kDkc2GamepadGuide);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSTICK, kDkc2GamepadLeftStick);
  MAP_SDL_BUTTON(SDL_CONTROLLER_BUTTON_RIGHTSTICK, kDkc2GamepadRightStick);
#undef MAP_SDL_BUTTON
  return buttons;
}

static uint8_t ReadTrigger(SDL_GameController *controller,
                           SDL_GameControllerAxis axis) {
  Sint16 value = SDL_GameControllerGetAxis(controller, axis);
  if (value <= 0) return 0;
  return (uint8_t)(((uint32_t)(uint16_t)value * 255u) / 32767u);
}

static bool IsSdlScancodePressed(int scancode, void *context) {
  const SdlHost *host = (const SdlHost *)context;
  const Uint8 *keys = snesrecomp_sdl_get_keyboard_state();
  return host && scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_NUM_SCANCODES &&
      (keys[scancode] || host->key_taps[scancode]);
}

static SdlControls ReadControlsImpl(SdlHost *host) {
  SdlControls controls = {0, 0};
  SDL_Window *window = (SDL_Window *)host->presenter.window;
  bool blocked = !host->hidden &&
      !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
#ifdef _WIN32
  blocked = blocked || (!host->hidden && Dkc2WindowsMenuBlocksInput(host->menu));
#endif
  if (blocked) {
    Dkc2DesktopOverlaySetGamepad(host->overlay, NULL);
    host->menu_chord_previous = false;
    return controls;
  }
  const Uint8 *keys = snesrecomp_sdl_get_keyboard_state();
  if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) return controls;
  uint32_t keyboard[kDkc2DesktopPlayerCount];
  for (int player = 0; player < kDkc2DesktopPlayerCount; player++)
    keyboard[player] = Dkc2MapKeyboardBindings(
        host->player_key_bind[player], IsSdlScancodePressed, host);

  Dkc2GamepadState gamepads[kMaximumControllers];
  size_t gamepad_count = 0;
  for (int i = 0; i < kMaximumControllers; i++) {
    SDL_GameController *controller = host->controllers[i];
    if (!controller || !SDL_GameControllerGetAttached(controller)) continue;
    Dkc2GamepadState *gamepad = &gamepads[gamepad_count++];
    gamepad->buttons = ReadGamepadButtons(controller);
    gamepad->left_x = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 vertical = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTY);
    gamepad->left_y = vertical == INT16_MIN ? INT16_MAX : (int16_t)-vertical;
    gamepad->right_x = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTX);
    vertical = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTY);
    gamepad->right_y =
        vertical == INT16_MIN ? INT16_MAX : (int16_t)-vertical;
    gamepad->left_trigger = ReadTrigger(
        controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    gamepad->right_trigger = ReadTrigger(
        controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  }
  controls.controller = Dkc2RoutePlayerInputsWithBindings(
      keyboard, gamepads, gamepad_count, host->player_source,
      host->player_deadzone, host->player_pad_bind);
  controls.host_actions = Dkc2MapAssistBindings(
      host->assist_key_bind, host->assist_pad_bind, IsSdlScancodePressed,
      host, gamepads, gamepad_count, 30);
  uint32_t menu_buttons = gamepad_count ? gamepads[0].buttons : 0;
  Dkc2DesktopOverlaySetGamepad(
      host->overlay, gamepad_count ? &gamepads[0] : NULL);
  uint32_t menu_input = Dkc2UpdateMenuChord(menu_buttons, &host->menu_chord_previous);
  if (menu_input & kDkc2MenuInputToggle)
    Dkc2DesktopOverlayToggle(host->overlay);
  if ((menu_input & kDkc2MenuInputBlock) || Dkc2DesktopOverlayIsOpen(host->overlay)) {
    controls.controller = 0;
    controls.host_actions = 0;
  }
  return controls;
}

static SdlControls ReadControls(SdlHost *host) {
  SdlControls controls = ReadControlsImpl(host);
  memset(host->key_taps, 0, sizeof host->key_taps);
  return controls;
}

static bool InitializeAudio(SdlHost *host) {
#if SNESRECOMP_SDL3
  const SDL_AudioSpec spec = {SDL_AUDIO_S16, kAudioChannels, kAudioRate};
  /* The same 1024-frame pull as SDL2; SDL may round it for the device. */
  (void)SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
  host->audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!host->audio_stream) return false;
  host->audio_device = SDL_GetAudioStreamDevice(host->audio_stream);
  SDL_AudioSpec device_spec;
  int device_frames = 0;
  if (!SDL_GetAudioDeviceFormat(host->audio_device, &device_spec,
                                &device_frames) ||
      device_frames <= 0 || device_spec.freq <= 0) {
    SDL_DestroyAudioStream(host->audio_stream);
    host->audio_stream = NULL;
    host->audio_device = 0;
    return false;
  }
  snesrecomp_sdl_pause_audio_device(host->audio_device, false);
  host->audio_available = true;
  /* The stream queues cartridge-rate frames; express the device pull in
   * them so the fill target keeps its SDL2 meaning. */
  host->audio_device_frames =
      (int)((int64_t)device_frames * kAudioRate / device_spec.freq);
#else
  SDL_AudioSpec desired;
  SDL_AudioSpec obtained;
  SDL_zero(desired);
  SDL_zero(obtained);
  desired.freq = kAudioRate;
  desired.format = AUDIO_S16SYS;
  desired.channels = kAudioChannels;
  /* 1024 frames is 32 ms at the cartridge's rate: a pull the queue can
   * always cover with two frames of margin, at half the latency of the
   * earlier 2048. */
  desired.samples = 1024;
  host->audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
  if (!host->audio_device) return false;
  if (obtained.freq != desired.freq || obtained.format != desired.format ||
      obtained.channels != desired.channels) {
    SDL_CloseAudioDevice(host->audio_device);
    host->audio_device = 0;
    return false;
  }
  SDL_PauseAudioDevice(host->audio_device, 0);
  host->audio_available = true;
  host->audio_device_frames = (int)obtained.samples;
#endif
  host->audio_fill_average = -1.0;
  host->audio_primed = false;
  Dkc2AudioStretchReset(&host->audio_stretch);
  return true;
}

static double AudioQueuedFrames(const SdlHost *host) {
  if (!host->audio_available) return 0.0;
#if SNESRECOMP_SDL3
  const int queued = SDL_GetAudioStreamQueued(host->audio_stream);
  if (queued <= 0) return 0.0;
  return (double)((size_t)queued / (kAudioChannels * sizeof(int16_t)));
#else
  return (double)(SDL_GetQueuedAudioSize(host->audio_device) /
                  (kAudioChannels * sizeof(int16_t)));
#endif
}

/* The queue fill rate control aims for: half a device pull, so the queue
 * can always cover the next pull, plus two frames of margin against host
 * stalls. The average fill sits here; the low point before a pull is the
 * two frames. */
static double AudioTargetFrames(const SdlHost *host) {
  return (double)host->audio_device_frames / 2.0 + 2.0 * kMaximumFrameAudio;
}

static bool QueueAudio(SdlHost *host, const int16_t *samples, int frames,
                       double ratio) {
  if (!host->audio_available || frames <= 0) return true;
  frames = Dkc2AudioStretchProcess(&host->audio_stretch, ratio, samples,
                                   frames, host->stretched_audio,
                                   kMaximumFrameAudio + kAudioStretchSlack);
  if (frames <= 0) return true;
  size_t sample_count = (size_t)frames * kAudioChannels;
  const int16_t *output = host->stretched_audio;
  if (host->audio_volume != 100) {
    for (size_t i = 0; i < sample_count; i++)
      host->scaled_audio[i] =
          (int16_t)(((int)output[i] * host->audio_volume) / 100);
    output = host->scaled_audio;
  }
#if SNESRECOMP_SDL3
  return SDL_PutAudioStreamData(host->audio_stream, output,
                                (int)(sample_count * sizeof output[0]));
#else
  return SDL_QueueAudio(host->audio_device, output,
                        (Uint32)(sample_count * sizeof output[0])) == 0;
#endif
}

static void ResetAudio(SdlHost *host) {
#if SNESRECOMP_SDL3
  if (host->audio_stream) (void)SDL_ClearAudioStream(host->audio_stream);
#else
  if (host->audio_device) SDL_ClearQueuedAudio(host->audio_device);
#endif
  host->audio_fill_average = -1.0;
  host->audio_primed = false;
  Dkc2AudioStretchReset(&host->audio_stretch);
}

static void ShutdownHost(SdlHost *host) {
#ifdef _WIN32
  Dkc2WindowsMenuDestroy(host->menu);
  host->menu = NULL;
#endif
  CloseControllers(host);
#if SNESRECOMP_SDL3
  /* Destroying a device stream also closes its device. */
  if (host->audio_stream) SDL_DestroyAudioStream(host->audio_stream);
#else
  if (host->audio_device) SDL_CloseAudioDevice(host->audio_device);
#endif
  Dkc2DesktopOverlayDestroy(host->overlay);
  host->overlay = NULL;
  Dkc2SdlPresenterDestroy(&host->presenter);
  Dkc2DesktopColorFilterDestroy(&host->color_filter);
  SDL_Quit();
}

static void PaceFrame(SdlHost *host, uint64_t *deadline,
                      double *deadline_fraction) {
  uint64_t frequency = SDL_GetPerformanceFrequency();
  double ticks = (double)frequency / kVideoRate;
  *deadline_fraction += ticks;
  uint64_t whole_ticks = (uint64_t)*deadline_fraction;
  *deadline_fraction -= (double)whole_ticks;
  *deadline += whole_ticks;
  while (host->running) {
    uint64_t now = SDL_GetPerformanceCounter();
    if (now >= *deadline) {
      /* Do not repay a visible host stall with a short catch-up frame. The
       * accepted DKC1 pacer reanchors after a two-millisecond miss; use the
       * same recovery threshold while preserving DKC2's exact 60.0988-Hz
       * fractional cadence. */
      if (now - *deadline > frequency / 500u) {
        *deadline = now;
        *deadline_fraction = 0.0;
      }
      return;
    }
    uint64_t remaining = *deadline - now;
    Uint32 milliseconds = (Uint32)(remaining * 1000 / frequency);
#ifdef __APPLE__
    if (milliseconds > 1)
      Dkc2MacWaitSeconds((double)remaining / (double)frequency);
    else
      SDL_Delay(0);
#else
    if (milliseconds > 1)
      SDL_Delay(milliseconds - 1);
    else
      SDL_Delay(0);
#endif
    PumpEvents(host);
  }
}

#ifdef __APPLE__
/* Pace on the display's refresh ticks. Until the pacer has measured a
 * refresh it can lock to, the latest tick is only observed and the caller
 * keeps the host clock; once locked, the frame waits for the tick that is
 * ticks_per_frame after the one the previous frame was presented on, so
 * every frame lands on the same refresh phase. A late loop presents on the
 * next tick without catching up, and ticks that stop arriving release the
 * lock. Returns true when this frame is display paced. */
static bool WaitForDisplayTick(bool link, Dkc2DesktopPacer *pacer,
                               uint64_t *presented, uint64_t *seen,
                               Dkc2MacDisplayTick *tick) {
  memset(tick, 0, sizeof *tick);
  if (!link) return false;
  if (!Dkc2DesktopPacerLocked(pacer)) {
    if (!Dkc2MacDisplayLinkLatest(tick)) return false;
    if (tick->sequence > *seen) {
      if (tick->interval > 0.0) (void)Dkc2DesktopPacerObserve(pacer, tick->interval);
      *seen = tick->sequence;
    }
    *presented = tick->sequence;
    return false;
  }
  const uint64_t wanted = *presented + pacer->ticks_per_frame;
  if (!Dkc2MacDisplayLinkWait(wanted, kDisplayTickTimeoutSeconds, tick)) {
    Dkc2DesktopPacerReset(pacer);
    *presented = tick->sequence;
    *seen = tick->sequence;
    return false;
  }
  if (tick->interval > 0.0) (void)Dkc2DesktopPacerObserve(pacer, tick->interval);
  *presented = tick->sequence;
  *seen = tick->sequence;
  return true;
}

static uint32_t ApplyMacCommands(SdlHost *host,
                                 RecompLauncherCSettings *settings) {
  uint32_t commands = Dkc2MacTakeCommands();
  uint32_t host_actions = 0;
  bool settings_changed = false;
  if (commands & kDkc2MacCommandQuit)
    host->running = false;
  if (commands & kDkc2MacCommandToggleOverlay)
    Dkc2DesktopOverlayToggle(host->overlay);
  if (commands & kDkc2MacCommandQuickSave)
    host_actions |= kDkc2HostSaveState;
  if (commands & kDkc2MacCommandQuickLoad)
    host_actions |= kDkc2HostLoadState;
  if (commands & kDkc2MacCommandToggleFullscreen) {
    bool fullscreen = !Dkc2SdlPresenterIsFullscreen(&host->presenter);
    if (Dkc2SdlPresenterSetFullscreen(&host->presenter, fullscreen)) {
      settings->fullscreen = fullscreen ? 1 : 0;
      settings_changed = true;
    }
  }
  if (commands & kDkc2MacCommandFilterNearest) {
    settings->texture_filter = 0;
    Dkc2LauncherSetUpscaler(kDkc2UpscalerNearest);
    settings_changed = true;
  }
  if (commands & kDkc2MacCommandFilterBilinear) {
    settings->texture_filter = 1;
    Dkc2LauncherSetUpscaler(kDkc2UpscalerBilinear);
    settings_changed = true;
  }
  if (commands & kDkc2MacCommandAspectNative) {
    settings->aspect_index = kDkc2VideoAspectNative;
    settings_changed = true;
  }
  if (commands & kDkc2MacCommandAspect16x10) {
    settings->aspect_index = kDkc2VideoAspect16x10;
    settings_changed = true;
  }
  if (commands & kDkc2MacCommandAspect21x9) {
    settings->aspect_index = kDkc2VideoAspect21x9;
    settings_changed = true;
  }
  if (commands & kDkc2MacCommandAspect16x9) {
    settings->aspect_index = kDkc2VideoAspect16x9;
    settings_changed = true;
  }
  if (settings_changed) {
    settings->widescreen =
        settings->aspect_index != kDkc2VideoAspectNative;
    Dkc2DesktopOverlaySetSettings(host->overlay, settings);
  }
  return host_actions;
}
#endif

#ifdef _WIN32
static uint32_t ApplyWindowsCommands(SdlHost *host,
                                     RecompLauncherCSettings *settings) {
  uint32_t actions = 0;
  Dkc2DesktopOverlayGetSettings(host->overlay, settings);
  Dkc2MenuState state = Dkc2MenuReadSettings(settings,
      Dkc2SdlPresenterIsFullscreen(&host->presenter),
      Dkc2DesktopOverlayIsOpen(host->overlay), true, host->presenter.program != 0);
  unsigned command;
  while ((command = Dkc2WindowsMenuTakeCommand(host->menu)) != 0) {
    switch (command) {
      case kDkc2MenuPause: Dkc2DesktopOverlayToggle(host->overlay); break;
      case kDkc2MenuSettings:
        if (!Dkc2DesktopOverlayIsOpen(host->overlay))
          Dkc2DesktopOverlayToggle(host->overlay);
        break;
      case kDkc2MenuSave: actions |= kDkc2HostSaveState; break;
      case kDkc2MenuLoad: actions |= kDkc2HostLoadState; break;
      case kDkc2MenuFullscreen: settings->fullscreen = !state.fullscreen; break;
      case kDkc2MenuQuit: host->running = false; break;
      case kDkc2MenuAbout:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "About DKC2Recomp",
            "DKC2Recomp " DKC2_RELEASE_VERSION
            "\nNative PC port foundation.\nRequires your own North American v1.0 ROM.",
            (SDL_Window *)host->presenter.window);
        break;
      default: (void)Dkc2MenuChangeSettings(settings, &state, command); break;
    }
  }
  Dkc2DesktopOverlaySetSettings(host->overlay, settings);
  return actions;
}
#endif

static void DestroyHd(SdlHost *host) {
  if (!host->hd) return;
  PpuClearOverlayBindings(g_ppu);
  Dkc2HdDestroy(host->hd);
  host->hd = NULL;
  host->hd_pixels = NULL;
  host->hd_scene = NULL;
}

static time_t PackFolderTime(const char *dir) {
  struct stat info;
  return dir && stat(dir, &info) == 0 ? info.st_mtime : 0;
}

/* Rebuilds the compositor with its configuration so edited and new pack
 * files show up without a restart. Called between frames only: the last
 * scene's quads point into the compositor. Once a second it also checks
 * the pack folder, whose time changes when files are added or removed. */
static void ReloadHdWhenAsked(SdlHost *host) {
  if (!host->hd) {
    host->hd_reload = false;
    return;
  }
  if (host->hd_config.pack_dir && ++host->hd_pack_check >= 60) {
    host->hd_pack_check = 0;
    const time_t time = PackFolderTime(host->hd_config.pack_dir);
    if (host->hd_pack_time && time != host->hd_pack_time)
      host->hd_reload = true;
    host->hd_pack_time = time;
  }
  if (!host->hd_reload) return;
  host->hd_reload = false;
  DestroyHd(host);
  host->hd = Dkc2HdCreate(&host->hd_config, host->rom, host->rom_size);
  if (host->hd)
    fprintf(stdout, "HD pack reloaded\n");
  else
    fprintf(stderr, "warning: HD pack reload failed\n");
}

/* Shows what the last HD frame draws under the mouse (F6). */
static void UpdateHdInspector(SdlHost *host) {
  if (!host->hd_inspector) {
    Dkc2DesktopOverlaySetInspector(host->overlay, NULL, NULL);
    return;
  }
  SDL_Window *window = (SDL_Window *)host->presenter.window;
  int mouse_x = 0, mouse_y = 0, window_width = 0, window_height = 0;
  int drawable_width = 0, drawable_height = 0;
#if SNESRECOMP_SDL3
  float mouse_fx = 0.0f, mouse_fy = 0.0f;
  (void)SDL_GetMouseState(&mouse_fx, &mouse_fy);
  mouse_x = (int)mouse_fx;
  mouse_y = (int)mouse_fy;
#else
  SDL_GetMouseState(&mouse_x, &mouse_y);
#endif
  SDL_GetWindowSize(window, &window_width, &window_height);
  Dkc2SdlPresenterDrawableSize(&host->presenter, &drawable_width,
                               &drawable_height);
  const int frame_width = Dkc2VideoWidth();
  Dkc2DesktopViewport viewport;
  Dkc2HdProbe probe;
  if (window_width <= 0 || window_height <= 0 ||
      !Dkc2DesktopComputeViewport(drawable_width, drawable_height,
                                  frame_width, kFrameHeight, &viewport)) {
    Dkc2DesktopOverlaySetInspector(host->overlay, "HD inspector", NULL);
    return;
  }
  /* Window points to drawable pixels to frame pixels. */
  const double points = (double)drawable_width / window_width;
  const double pixel_x = mouse_x * points - viewport.x;
  const double pixel_y = mouse_y * points - viewport.y;
  const int x = (int)(pixel_x * frame_width / viewport.width);
  const int y = (int)(pixel_y * kFrameHeight / viewport.height);
  if (pixel_x < 0 || pixel_y < 0 ||
      !Dkc2HdProbePixel(host->hd, x, y, &probe)) {
    host->hd_inspected[0] = '\0';
    Dkc2DesktopOverlaySetInspector(
        host->overlay, host->hd ? "HD inspector: outside the HD frame"
                                : "HD inspector: HD presentation is off",
        NULL);
    return;
  }
  char text[128];
  if (!probe.object) {
    host->hd_inspected[0] = '\0';
    snprintf(text, sizeof text, "(%d, %d) background", x, y);
    Dkc2DesktopOverlaySetInspector(host->overlay, text, NULL);
    return;
  }
  memcpy(host->hd_inspected, probe.name, sizeof host->hd_inspected);
  snprintf(text, sizeof text, "(%d, %d) %s.rgba  %dx%d  %s", x, y,
           probe.name, probe.width, probe.height,
           probe.art ? "pack art" : "filter");
  const double frame_to_points =
      (double)viewport.width / frame_width / points;
  const float rect[4] = {
      (float)((viewport.x + probe.x * (double)viewport.width / frame_width) /
              points),
      (float)((viewport.y + probe.y * (double)viewport.height / kFrameHeight) /
              points),
      (float)(probe.width * frame_to_points),
      (float)(probe.height * (double)viewport.height / kFrameHeight / points)};
  Dkc2DesktopOverlaySetInspector(host->overlay, text, rect);
}

/* Builds, rebuilds or drops the HD compositor to match the HD options. */
static void UpdateHd(SdlHost *host) {
  if (host->hd_environment) return;
  const bool enabled = Dkc2LauncherHdEnabled() != 0;
  Dkc2HdConfig config = host->hd_config;
  config.scale = Dkc2LauncherHdScale();
  config.filter = (Dkc2HdFilter)Dkc2LauncherHdSprites();
  config.background = (Dkc2HdFilter)Dkc2LauncherHdBackground();
  config.pack_dir = Dkc2LauncherHdArt() ? host->hd_pack_dir : NULL;
  if (enabled == (host->hd != NULL) &&
      (!enabled || (config.scale == host->hd_config.scale &&
                    config.filter == host->hd_config.filter &&
                    config.background == host->hd_config.background &&
                    config.pack_dir == host->hd_config.pack_dir)))
    return;
  DestroyHd(host);
  host->hd_config = config;
  if (!enabled) return;
  host->hd = Dkc2HdCreate(&config, host->rom, host->rom_size);
  if (host->hd) {
    fprintf(stdout, "HD presentation: %dx\n", config.scale);
  } else {
    fprintf(stderr, "warning: HD presentation could not be initialized\n");
    Dkc2LauncherSetHdEnabled(0);
  }
}

static void ApplyOverlaySettings(SdlHost *host,
                                 RecompLauncherCSettings *settings,
                                 int *screen_filter) {
  if (!host || !host->overlay || !settings || !screen_filter) return;
  RecompLauncherCSettings updated;
  Dkc2DesktopOverlayGetSettings(host->overlay, &updated);
  if ((updated.fullscreen != 0) != Dkc2SdlPresenterIsFullscreen(&host->presenter)) {
#ifdef _WIN32
    Dkc2WindowsMenuSetVisible(host->menu, updated.fullscreen == 0);
#endif
    (void)Dkc2SdlPresenterSetFullscreen(&host->presenter, updated.fullscreen != 0);
    updated.fullscreen = Dkc2SdlPresenterIsFullscreen(&host->presenter) ? 1 : 0;
  }
#ifdef _WIN32
  Dkc2WindowsMenuSetVisible(host->menu, updated.fullscreen == 0);
#endif
  updated.volume = ClampInt(updated.volume, 0, 100);
  updated.texture_filter = updated.texture_filter != 0;
  updated.aspect_index =
      ClampInt(updated.aspect_index, kDkc2VideoAspectNative,
               kDkc2VideoAspectCount - 1);
  updated.widescreen =
      updated.aspect_index != kDkc2VideoAspectNative;
  if (!Dkc2DesktopScreenFilterValid(updated.screen_kind))
    updated.screen_kind = kDkc2ScreenRaw;
  if (updated.screen_kind != *screen_filter) {
    int previous_filter = *screen_filter;
    Dkc2DesktopColorFilterDestroy(&host->color_filter);
    if (Dkc2DesktopColorFilterInit(&host->color_filter,
                                   updated.screen_kind)) {
      *screen_filter = updated.screen_kind;
    } else {
      updated.screen_kind = previous_filter;
      (void)Dkc2DesktopColorFilterInit(&host->color_filter,
                                       previous_filter);
    }
  }
  host->presenter.linear_filter = updated.texture_filter != 0;
  (void)Dkc2SdlPresenterSetUpscaler(
      &host->presenter,
      Dkc2LauncherUpscaler() == kDkc2UpscalerReconstruct
          ? kDkc2UpscalerReconstruct
          : (updated.texture_filter ? kDkc2UpscalerBilinear
                                    : kDkc2UpscalerNearest),
      Dkc2LauncherReconstructMode(),
      (float)Dkc2LauncherReconstructStrength() / 100.0f,
      (float)Dkc2LauncherReconstructSoftness() / 100.0f,
      (float)Dkc2LauncherReconstructShading() / 100.0f);
  (void)Dkc2SdlPresenterSetDisplay(&host->presenter, Dkc2LauncherDisplay(),
                                   Dkc2LauncherCrt());
  host->audio_volume = updated.volume;
  for (int player = 0; player < kDkc2DesktopPlayerCount; player++) {
    host->player_source[player] =
        ClampInt(updated.player_src[player], 0, 2);
    host->player_deadzone[player] =
        ClampInt(updated.deadzone[player], 0, 100);
    updated.player_src[player] = host->player_source[player];
    updated.deadzone[player] = host->player_deadzone[player];
  }
  memcpy(host->player_key_bind, updated.player_key_bind,
         sizeof host->player_key_bind);
  memcpy(host->player_pad_bind, updated.player_pad_bind,
         sizeof host->player_pad_bind);
  memcpy(host->assist_key_bind, updated.assist_key_bind,
         sizeof host->assist_key_bind);
  memcpy(host->assist_pad_bind, updated.assist_pad_bind,
         sizeof host->assist_pad_bind);
  UpdateHd(host);
  if (Dkc2VideoGetAspect() != (Dkc2VideoAspect)updated.aspect_index) {
    Dkc2VideoSetAspect((Dkc2VideoAspect)updated.aspect_index);
    memset(host->pixels, 0, sizeof host->pixels);
    memset(host->filtered_pixels, 0, sizeof host->filtered_pixels);
    Dkc2BeginDrawing(
        host->pixels, (size_t)Dkc2VideoWidth() * kBytesPerPixel);
  }
  *settings = updated;
}

static int RunGame(const char *rom_path,
                   RecompLauncherCSettings *settings) {
  SdlHost host;
  memset(&host, 0, sizeof host);
  host.running = true;
  host.hidden = EnvironmentEnabled("DKC2_DESKTOP_TEST_HIDDEN");
  host.audio_volume = ClampInt(settings->volume, 0, 100);
  for (int player = 0; player < kDkc2DesktopPlayerCount; player++) {
    host.player_source[player] = ClampInt(settings->player_src[player], 0, 2);
    host.player_deadzone[player] = ClampInt(settings->deadzone[player], 0, 100);
  }
  memcpy(host.player_key_bind, settings->player_key_bind,
         sizeof host.player_key_bind);
  memcpy(host.player_pad_bind, settings->player_pad_bind,
         sizeof host.player_pad_bind);
  memcpy(host.assist_key_bind, settings->assist_key_bind,
         sizeof host.assist_key_bind);
  memcpy(host.assist_pad_bind, settings->assist_pad_bind,
         sizeof host.assist_pad_bind);

  unsigned long long test_frame_limit = 0;
  const char *test_frames = getenv("DKC2_DESKTOP_TEST_FRAMES");
  if (test_frames && *test_frames) {
    char *end = NULL;
    test_frame_limit = strtoull(test_frames, &end, 10);
    if (!end || *end != '\0' || test_frame_limit == 0 ||
        test_frame_limit > 1000000) {
      ShowError("DKC2_DESKTOP_TEST_FRAMES must be between 1 and 1000000");
      return 2;
    }
  }
  bool test_rewind_requested = EnvironmentEnabled("DKC2_DESKTOP_TEST_REWIND");
  bool test_fast_forward_requested =
      EnvironmentEnabled("DKC2_DESKTOP_TEST_FASTFORWARD");
  bool test_overlay_requested =
      EnvironmentEnabled("DKC2_DESKTOP_TEST_OVERLAY");
  bool test_save_load_requested =
      EnvironmentEnabled("DKC2_DESKTOP_TEST_SAVELOAD");
  bool test_save_injected = false;
  bool test_save_completed = false;
  bool test_load_injected = false;
  bool test_load_completed = false;
  bool sram_enabled = !EnvironmentEnabled("DKC2_DESKTOP_DISABLE_SRAM");
  int persisted_aspect =
      ClampInt(settings->aspect_index, kDkc2VideoAspectNative,
               kDkc2VideoAspectCount - 1);
  Dkc2VideoAspect aspect = (Dkc2VideoAspect)persisted_aspect;
  const char *aspect_override = getenv("DKC2_ASPECT");
  bool aspect_override_active = aspect_override && *aspect_override;
  if (aspect_override_active &&
      !Dkc2VideoAspectFromName(aspect_override, &aspect)) {
    ShowError("DKC2_ASPECT must be 4:3, 16:10, 16:9, or 21:9");
    return 2;
  }
  const char *widescreen_override = getenv("DKC2_WIDESCREEN");
  bool widescreen_override_active = !aspect_override_active &&
      widescreen_override && *widescreen_override;
  if (widescreen_override_active)
    aspect = *widescreen_override != '0'
        ? kDkc2VideoAspect16x9 : kDkc2VideoAspectNative;
  /* HD follows the options unless DKC2_HD fixes it for the run. Art comes
   * from the "hdpack" folder beside launcher.cfg, or DKC2_HD_PACK. */
  const char *hd_error = NULL;
  host.hd_environment =
      Dkc2HdConfigFromEnvironment(&host.hd_config, &hd_error);
  if (hd_error) {
    ShowError(hd_error);
    return 2;
  }
  if (!host.hd_environment) {
    const char *pack_dir = getenv("DKC2_HD_PACK");
    host.hd_pack_dir = pack_dir && *pack_dir ? pack_dir : "hdpack";
    host.hd_config.pack_dir = host.hd_pack_dir;
    host.hd_config.dump_dir = getenv("DKC2_HD_DUMP");
    const char *filter_table = getenv("DKC2_HD_FILTER_TABLE");
    host.hd_config.filter_table =
        filter_table && *filter_table ? filter_table : NULL;
  }
  settings->aspect_index = (int)aspect;
  settings->widescreen = aspect != kDkc2VideoAspectNative;
  Dkc2VideoSetAspect(aspect);
  Dkc2VideoSetEdgePolicy((Dkc2VideoEdgePolicy)Dkc2LauncherWidescreenEdge());
  {
    const char *edge_text = getenv("DKC2_WIDESCREEN_EDGE");
    Dkc2VideoEdgePolicy edge_policy = kDkc2VideoEdgeGlide;
    if (edge_text && *edge_text) {
      if (!Dkc2VideoEdgePolicyFromName(edge_text, &edge_policy)) {
        ShowError("DKC2_WIDESCREEN_EDGE must be reflect, bars, shift, or glide");
        return 2;
      }
      Dkc2VideoSetEdgePolicy(edge_policy);
    }
  }
  Dkc2CoopSetMode((Dkc2CoopMode)Dkc2LauncherCoopMode());
  {
    const char *coop_text = getenv("DKC2_COOP");
    Dkc2CoopMode coop_mode = kDkc2CoopSimultaneous;
    if (coop_text && *coop_text) {
      if (!Dkc2CoopModeFromName(coop_text, &coop_mode)) {
        ShowError("DKC2_COOP must be simultaneous or classic");
        return 2;
      }
      Dkc2CoopSetMode(coop_mode);
    }
  }
  int screen_filter = ClampInt(settings->screen_kind, 0, 3);
  const char *screen_override = getenv("DKC2_SCREEN");
  if (screen_override && *screen_override &&
      !Dkc2DesktopScreenFilterFromName(screen_override, &screen_filter)) {
    ShowError("DKC2_SCREEN must be raw, crt, composite, or trinitron");
    return 2;
  }

  size_t rom_size = 0;
  char rom_error[160];
  uint8_t *rom =
      Dkc2ReadVerifiedRom(rom_path, &rom_size, rom_error, sizeof rom_error);
  if (!rom) {
    ShowError(rom_error);
    return 2;
  }
  Dkc2KongsInitialize();
  Dkc2MusicInitialize();
  RtlRegisterGame(Dkc2GameInfo());
  if (!SnesInit(rom, (int)rom_size)) {
    free(rom);
    ShowError("snesrecomp rejected the verified ROM");
    return 3;
  }
  if (sram_enabled) {
    if (EnsureSaveDirectory()) RtlReadSram();
    else sram_enabled = false;
  }
  if (!Dkc2DesktopColorFilterInit(&host.color_filter, screen_filter)) {
    free(rom);
    ShowError("Unable to initialize the selected screen-color filter");
    return 4;
  }
#if SNESRECOMP_SDL3
  const Uint32 sdl_subsystems =
      SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD;
#else
  const Uint32 sdl_subsystems = SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                                SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER;
#endif
  if (!snesrecomp_sdl_init(sdl_subsystems)) {
    free(rom);
    Dkc2DesktopColorFilterDestroy(&host.color_filter);
    ShowError(SDL_GetError());
    return 4;
  }
  char video_error[256] = {0};
  if (!Dkc2SdlPresenterInit(
          &host.presenter, ClampInt(settings->window_scale, 1, 4),
          ClampInt(settings->fullscreen, 0, 2), host.hidden,
          settings->texture_filter != 0, Dkc2VideoWidth(), kFrameHeight,
          video_error, sizeof video_error)) {
    free(rom);
    ShutdownHost(&host);
    ShowError(video_error);
    return 4;
  }
  host.hd_gpu = Dkc2SdlPresenterHdAvailable(&host.presenter) &&
                !EnvironmentEnabled("DKC2_HD_CPU");
  fprintf(stdout, "HD composition: %s\n", host.hd_gpu ? "gpu" : "cpu");
  {
    /* DKC2_DESKTOP_TEST_WINDOW=WxH: size the window in points so a hidden
     * capture can be taken at a chosen drawable size (the panel's full
     * resolution on a Retina display is half its pixel size in points). */
    const char *window_text = getenv("DKC2_DESKTOP_TEST_WINDOW");
    if (window_text && *window_text) {
      int width = 0, height = 0;
      if (sscanf(window_text, "%dx%d", &width, &height) != 2 || width < 64 ||
          height < 64 || width > 16384 || height > 16384) {
        free(rom);
        ShutdownHost(&host);
        ShowError("DKC2_DESKTOP_TEST_WINDOW must be WIDTHxHEIGHT in points");
        return 2;
      }
      Dkc2SdlPresenterSetWindowSize(&host.presenter, width, height);
    }
  }
  {
    /* Upscaler: the launcher's remembered choice, overridable for one run
     * with DKC2_UPSCALER=nearest|bilinear|reconstruct; DKC2_RECONSTRUCT_MODE
     * (0..3) and DKC2_RECONSTRUCT_STRENGTH (0..100) tune the experiment. */
    int upscaler = Dkc2LauncherUpscaler();
    const char *upscaler_text = getenv("DKC2_UPSCALER");
    if (upscaler_text && *upscaler_text &&
        !Dkc2SdlPresenterUpscalerFromName(upscaler_text, &upscaler)) {
      free(rom);
      ShutdownHost(&host);
      ShowError("DKC2_UPSCALER must be nearest, bilinear, or reconstruct");
      return 2;
    }
    const char *mode_text = getenv("DKC2_RECONSTRUCT_MODE");
    if (mode_text && *mode_text)
      Dkc2LauncherSetReconstructMode(atoi(mode_text));
    const char *strength_text = getenv("DKC2_RECONSTRUCT_STRENGTH");
    if (strength_text && *strength_text)
      Dkc2LauncherSetReconstructStrength(atoi(strength_text));
    const char *softness_text = getenv("DKC2_RECONSTRUCT_SOFTNESS");
    if (softness_text && *softness_text)
      Dkc2LauncherSetReconstructSoftness(atoi(softness_text));
    const char *shading_text = getenv("DKC2_RECONSTRUCT_SHADING");
    if (shading_text && *shading_text)
      Dkc2LauncherSetReconstructShading(atoi(shading_text));
    if (upscaler == kDkc2UpscalerReconstruct) {
      Dkc2LauncherSetUpscaler(kDkc2UpscalerReconstruct);
    } else if (upscaler_text && *upscaler_text) {
      Dkc2LauncherSetUpscaler(upscaler);
      settings->texture_filter = upscaler == kDkc2UpscalerBilinear;
    } else {
      upscaler = settings->texture_filter ? kDkc2UpscalerBilinear
                                          : kDkc2UpscalerNearest;
    }
    const int effective = Dkc2SdlPresenterSetUpscaler(
        &host.presenter, upscaler, Dkc2LauncherReconstructMode(),
        (float)Dkc2LauncherReconstructStrength() / 100.0f,
        (float)Dkc2LauncherReconstructSoftness() / 100.0f,
        (float)Dkc2LauncherReconstructShading() / 100.0f);
    if (effective != upscaler && host.presenter.shader_error[0])
      fprintf(stderr, "warning: %s; using %s\n", host.presenter.shader_error,
              Dkc2SdlPresenterUpscalerName(effective));
  }
  {
    char crt_error[160];
    if (!Dkc2LauncherApplyCrtEnvironment(crt_error, sizeof crt_error)) {
      free(rom);
      ShutdownHost(&host);
      ShowError(crt_error);
      return 2;
    }
    const int display = Dkc2LauncherDisplay();
    const int effective_display = Dkc2SdlPresenterSetDisplay(
        &host.presenter, display, Dkc2LauncherCrt());
    if (effective_display != display && host.presenter.crt_error[0])
      fprintf(stderr, "warning: %s; using flat\n", host.presenter.crt_error);
  }
  host.overlay = Dkc2DesktopOverlayCreate(settings);
  if (!host.overlay ||
      !Dkc2DesktopOverlayInitSdl(
          host.overlay, host.presenter.window, host.presenter.gl_context)) {
    free(rom);
    ShutdownHost(&host);
    ShowError("Unable to initialize the in-game overlay");
    return 4;
  }
#ifdef __APPLE__
  if (!host.hidden) {
    Dkc2MacInstallMenu();
    Dkc2MacUpdateMenu(
        Dkc2SdlPresenterIsFullscreen(&host.presenter),
        settings->texture_filter != 0, settings->aspect_index);
  }
#endif
#ifdef _WIN32
  int menu_client_width, menu_client_height;
  SDL_GetWindowSize((SDL_Window *)host.presenter.window,
      &menu_client_width, &menu_client_height);
  SDL_SysWMinfo menu_info;
  SDL_VERSION(&menu_info.version);
  if (!SDL_GetWindowWMInfo((SDL_Window *)host.presenter.window, &menu_info) ||
      !(host.menu = Dkc2WindowsMenuCreate(menu_info.info.win.window))) {
    free(rom);
    ShutdownHost(&host);
    ShowError("Unable to create the native menu");
    return 4;
  }
  Dkc2WindowsMenuSetVisible(host.menu,
      !Dkc2SdlPresenterIsFullscreen(&host.presenter));
  if (!Dkc2SdlPresenterIsFullscreen(&host.presenter))
    SDL_SetWindowSize((SDL_Window *)host.presenter.window,
        menu_client_width, menu_client_height);
#endif
  RefreshControllers(&host);
  Dkc2BeginDrawing(
      host.pixels, (size_t)Dkc2VideoWidth() * kBytesPerPixel);
  if (settings->enable_audio && !InitializeAudio(&host)) {
    if (test_frame_limit) {
      free(rom);
      ShutdownHost(&host);
      ShowError("SDL audio could not be opened in the desktop test");
      return 6;
    }
    fprintf(stderr, "warning: SDL audio unavailable; continuing silent\n");
  }
  Dkc2DiagnosticsSetPresentation(
      Dkc2SdlPresenterBackend(&host.presenter),
      Dkc2DesktopScreenFilterName(screen_filter), host.audio_available);
  fprintf(stdout,
          "Video: %s, %s, %s sampling, display=%s, aspect=%s (%dx%d)\n",
          Dkc2SdlPresenterBackend(&host.presenter),
          Dkc2DesktopScreenFilterName(screen_filter),
          Dkc2SdlPresenterUpscalerName(host.presenter.upscaler),
          Dkc2CrtDisplayName(host.presenter.display),
          Dkc2VideoAspectName(Dkc2VideoGetAspect()), Dkc2VideoWidth(),
          kFrameHeight);
  fprintf(stdout,
          "Controls: gameplay and Assist bindings are configurable in the "
          "pre-boot launcher. Escape=Exit Fullscreen/Overlay. "
          "SDL game controllers are "
          "detected automatically.\n");

#ifdef __APPLE__
  Dkc2DesktopPacer pacer;
  Dkc2DesktopPacerInit(&pacer, kVideoRate, kDisplayLockTolerance);
  bool display_link = false;
  uint64_t presented_tick = 0;
  uint64_t seen_tick = 0;
  /* The link runs whenever the window has one, so a pacing log always has
   * the display's ticks to measure against; DKC2_DISPLAY_LOCK=0 keeps the
   * frames on the host clock while still recording them. */
  const bool display_lock = !EnvironmentDisabled("DKC2_DISPLAY_LOCK");
  if (Dkc2SdlPresenterUsesSoftwarePacing(&host.presenter)) {
    char link_error[128] = {0};
    display_link = Dkc2MacDisplayLinkStart(
        Dkc2SdlPresenterNativeWindow(&host.presenter), 60.0, link_error,
        sizeof link_error);
    if (!display_link)
      fprintf(stdout, "Display link unavailable (%s); pacing on the host "
              "clock.\n", link_error);
  }
  fprintf(stdout, "Frame pacing: %s\n",
          display_link && display_lock
              ? "display link (DKC2_DISPLAY_LOCK=0 keeps the host clock)"
              : "host clock");
  /* DKC2_PACING_LOG: one line per presented frame with the pacing mode,
   * the present time, the display tick it followed, the tick's target
   * refresh time and interval, the emulation time, the audio queue's
   * average fill and the stretch ratio, for measuring cadence offline. */
  FILE *pacing_log = NULL;
  {
    const char *pacing_log_path = getenv("DKC2_PACING_LOG");
    if (pacing_log_path && *pacing_log_path) {
      pacing_log = fopen(pacing_log_path, "w");
      if (pacing_log)
        fprintf(pacing_log, "frame mode present_s tick target_s interval_s "
                            "emulate_s fill_frames ratio pump_s work_s "
                            "wait_s present_call_s\n");
    }
  }
#endif
  Dkc2DesktopFpsCounter fps_counter;
  uint64_t deadline = SDL_GetPerformanceCounter();
  uint64_t frequency = SDL_GetPerformanceFrequency();
  Dkc2DesktopFpsInit(&fps_counter, deadline);
  double deadline_fraction = 0.0;
  double audio_fraction = 0.0;
  unsigned long long host_frame = 0;
  const char *screenshot_path = getenv("DKC2_DESKTOP_SCREENSHOT");
  if (screenshot_path && !*screenshot_path) screenshot_path = NULL;
  /* DKC2_DESKTOP_SCREENSHOT_FRAME: one frame, or up to eight separated by
   * commas; with several, each file is the path with "-<frame>" before its
   * extension. */
  enum { kMaxScreenshots = 8 };
  unsigned long long screenshot_frames[kMaxScreenshots] = {60};
  int screenshot_count = 1;
  {
    const char *frame_text = getenv("DKC2_DESKTOP_SCREENSHOT_FRAME");
    if (frame_text && *frame_text) {
      screenshot_count = 0;
      char *end = NULL;
      while (screenshot_count < kMaxScreenshots) {
        screenshot_frames[screenshot_count++] = strtoull(frame_text, &end, 10);
        if (*end != ',') break;
        frame_text = end + 1;
      }
    }
  }
  int screenshot_index = 0;
  uint8_t *screenshot_rgb = NULL;
  const char *test_load_state_path = getenv("DKC2_DESKTOP_TEST_LOADSTATE");
  if (test_load_state_path && !*test_load_state_path)
    test_load_state_path = NULL;
  bool test_load_state_done = false;
  unsigned rewind_capture_counter = 0;
  int16_t frame_audio[kMaximumFrameAudio * kAudioChannels];
  Dkc2RewindHistory rewind_history;
  memset(&rewind_history, 0, sizeof rewind_history);
  Dkc2DesktopOverlaySetReconstructAvailable(host.overlay, host.presenter.program != 0);
  Dkc2DesktopOverlaySetCrtAvailable(host.overlay,
      Dkc2GlPipelineCrtAvailable(&host.presenter));
  size_t rewind_snapshot_size = RtlSaveSnapshotToMemory(NULL, 0);
  uint8_t *rewind_scratch = rewind_snapshot_size
      ? (uint8_t *)malloc(rewind_snapshot_size) : NULL;
  bool rewind_available =
      rewind_scratch &&
      Dkc2RewindHistoryInit(&rewind_history, rewind_snapshot_size,
                            kRewindSnapshotCapacity) &&
      RtlSaveSnapshotToMemory(rewind_scratch, rewind_snapshot_size) ==
          rewind_snapshot_size &&
      Dkc2RewindHistoryPush(&rewind_history, rewind_scratch);
  bool test_rewind_completed = false;
  bool test_fast_forward_completed = false;
  bool test_overlay_completed = false;
  unsigned test_overlay_ticks = 0;
  bool runtime_failure = false;
  uint32_t previous_state_actions = 0;
  SdlSpeedMode previous_mode = kSdlSpeedNormal;
  bool previous_overlay_open = false;
  Dkc2InputRecorder input_recorder = {0};
  char input_recording_error[512] = {0};
  const char *input_recording_path = getenv("SNESRECOMP_INPUT_REC");

  if (input_recording_path && *input_recording_path) {
    if (!Dkc2InputRecorderOpen(
            &input_recorder, input_recording_path,
            input_recording_error, sizeof input_recording_error)) {
      fprintf(stderr, "Input recording failed: %s\n", input_recording_error);
      ShowError(input_recording_error);
      runtime_failure = true;
      host.running = false;
    } else {
      fprintf(stdout, "Input recording enabled: %s\n", input_recording_path);
      Dkc2SdlPresenterSetTitle(
          &host.presenter, DKC2_PRODUCT_TITLE " (Recording Input)");
    }
  }
  /* SNESRECOMP_INPUT_PLAY replays a recorded route in place of the
   * controllers (one word per emulated frame), as the headless runner does,
   * so GPU presentation can be checked in any scene. */
  Dkc2InputPlayback input_playback = {0};
  const char *input_playback_path = getenv("SNESRECOMP_INPUT_PLAY");
  if (input_playback_path && *input_playback_path) {
    char error[192];
    if (!Dkc2InputPlaybackLoad(input_playback_path, &input_playback, error,
                               sizeof error)) {
      fprintf(stderr, "Input playback failed: %s: %s\n", input_playback_path,
              error);
      ShowError(error);
      runtime_failure = true;
      host.running = false;
    }
  }

  host.rom = rom;
  host.rom_size = rom_size;
  if (host.hd_environment) {
    host.hd = Dkc2HdCreate(&host.hd_config, rom, rom_size);
    if (host.hd) {
      fprintf(stdout, "HD presentation: %dx\n", host.hd_config.scale);
    } else {
      ShowError("HD presentation could not be initialized");
      runtime_failure = true;
      host.running = false;
    }
  }

  while (host.running) {
#ifdef __APPLE__
    const double stage_top = Dkc2MacHostSeconds();
#endif
    PumpEvents(&host);
    ReloadHdWhenAsked(&host);
    UpdateHdInspector(&host);
    if (host.hd_copy_name) {
      host.hd_copy_name = false;
      if (host.hd_inspected[0]) (void)SDL_SetClipboardText(host.hd_inspected);
    }
#ifdef __APPLE__
    const double stage_pumped = Dkc2MacHostSeconds();
    double stage_paced = stage_pumped;
    double stage_before_pace = stage_pumped;
#endif
    if (host.escaped_fullscreen) {
      settings->fullscreen = 0;
      Dkc2DesktopOverlaySetSettings(host.overlay, settings);
      host.escaped_fullscreen = false;
    }
    Dkc2DiagnosticsHeartbeat(host_frame, Dkc2ResumePc());
    host_report_crash_test_tick();
    uint32_t platform_host_actions = 0;
#ifdef _WIN32
    platform_host_actions = ApplyWindowsCommands(&host, settings);
    if (!host.running) break;
#endif
#ifdef __APPLE__
    platform_host_actions = ApplyMacCommands(&host, settings);
    if (test_save_load_requested && !test_save_injected &&
        host_frame >= 30) {
      platform_host_actions |= kDkc2HostSaveState;
      test_save_injected = true;
    }
    if (test_save_load_requested && test_save_completed &&
        !test_load_injected && host_frame >= 60) {
      platform_host_actions |= kDkc2HostLoadState;
      test_load_injected = true;
    }
#endif
    /* DKC2_DESKTOP_TEST_LOADSTATE: start a hidden or visible run from a
     * preserved snapshot once the host has settled, so presentation
     * experiments can be captured on real gameplay. */
    if (test_load_state_path && !test_load_state_done && host_frame >= 2) {
      test_load_state_done = true;
      if (RtlLoadSnapshot(test_load_state_path)) {
        ResetAudio(&host);
        audio_fraction = 0.0;
        deadline = SDL_GetPerformanceCounter();
        deadline_fraction = 0.0;
        DrawFrame(&host);
      } else {
        fprintf(stderr, "warning: DKC2_DESKTOP_TEST_LOADSTATE failed: %s\n",
                test_load_state_path);
      }
    }
    UpdateHaptics(&host);
    SdlControls controls = ReadControls(&host);
    if (test_overlay_requested && !test_overlay_completed &&
        host_frame >= 30) {
      if (test_overlay_ticks == 0) {
        Dkc2DesktopOverlayToggle(host.overlay);
        test_overlay_ticks = 1;
      } else if (Dkc2DesktopOverlayIsOpen(host.overlay)) {
        test_overlay_ticks++;
        if (test_overlay_ticks >= 30) {
          Dkc2DesktopOverlayToggle(host.overlay);
          test_overlay_completed = true;
        }
      }
    }
    uint32_t overlay_actions =
        Dkc2DesktopOverlayTakeActions(host.overlay);
    if (overlay_actions & kDkc2OverlayActionQuit) {
      host.running = false;
      break;
    }
    if (overlay_actions & kDkc2OverlayActionReloadHdPack)
      host.hd_reload = true;
    if (overlay_actions & kDkc2OverlayActionSaveState)
      controls.host_actions |= kDkc2HostSaveState;
    if (overlay_actions & kDkc2OverlayActionLoadState)
      controls.host_actions |= kDkc2HostLoadState;
    ApplyOverlaySettings(&host, settings, &screen_filter);
#ifdef _WIN32
    Dkc2MenuState menu_state = Dkc2MenuReadSettings(settings,
        Dkc2SdlPresenterIsFullscreen(&host.presenter),
        Dkc2DesktopOverlayIsOpen(host.overlay), true, host.presenter.program != 0);
    Dkc2WindowsMenuUpdate(host.menu, &menu_state);
#endif
    bool overlay_open = Dkc2DesktopOverlayIsOpen(host.overlay);
    if (overlay_open != previous_overlay_open) {
      ResetAudio(&host);
      if (host.audio_device)
        snesrecomp_sdl_pause_audio_device(host.audio_device, overlay_open);
      audio_fraction = 0.0;
      deadline = SDL_GetPerformanceCounter();
      deadline_fraction = 0.0;
      previous_overlay_open = overlay_open;
    }
    bool assist_tools = Dkc2DesktopOverlayAssistTools(host.overlay);
#ifdef __APPLE__
    if (!host.hidden)
      Dkc2MacUpdateMenu(
          Dkc2SdlPresenterIsFullscreen(&host.presenter),
          host.presenter.linear_filter, settings->aspect_index);
#endif
    controls.host_actions = Dkc2ApplyAssistGate(
        controls.host_actions, platform_host_actions, assist_tools);
    uint32_t state_actions = controls.host_actions &
        (kDkc2HostSaveState | kDkc2HostLoadState);
    uint32_t pressed_state_actions = state_actions & ~previous_state_actions;
    previous_state_actions = state_actions;
    if (pressed_state_actions & kDkc2HostSaveState) {
      int slot = Dkc2DesktopOverlaySelectedSlot(host.overlay);
      char path[128];
      RtlSaveSlotPath(slot, path, sizeof path);
      bool saved = EnsureSaveDirectory() && RtlSaveSnapshot(path);
      char status[80];
      (void)snprintf(status, sizeof status,
                     saved ? "Slot %d saved." : "Slot %d save failed.",
                     slot + 1);
      Dkc2DesktopOverlaySetStatus(
          host.overlay, status, saved);
      if (test_save_load_requested && test_save_injected)
        test_save_completed = saved;
      (void)snprintf(status, sizeof status,
                     saved ? DKC2_PRODUCT_TITLE " - Slot %d saved"
                           : DKC2_PRODUCT_TITLE " - Slot %d save failed",
                     slot + 1);
      Dkc2SdlPresenterSetTitle(&host.presenter, status);
    }
    if (pressed_state_actions & kDkc2HostLoadState) {
      int slot = Dkc2DesktopOverlaySelectedSlot(host.overlay);
      char path[128];
      RtlSaveSlotPath(slot, path, sizeof path);
      bool loaded = RtlLoadSnapshot(path);
      if (!loaded && slot == 0)
        loaded = RtlLoadSnapshot(DKC2_STATE_SLOT0_LEGACY_FILE);
      if (loaded) {
        char status[80];
        (void)snprintf(status, sizeof status, "Slot %d loaded.", slot + 1);
        Dkc2DesktopOverlaySetStatus(host.overlay, status, true);
        ResetAudio(&host);
        audio_fraction = 0.0;
        deadline = SDL_GetPerformanceCounter();
        deadline_fraction = 0.0;
        rewind_history.count = 0;
        rewind_history.write_index = 0;
        rewind_capture_counter = 0;
        if (rewind_available &&
            RtlSaveSnapshotToMemory(rewind_scratch, rewind_snapshot_size) ==
                rewind_snapshot_size)
          (void)Dkc2RewindHistoryPush(&rewind_history, rewind_scratch);
        DrawFrame(&host);
        if (test_save_load_requested && test_load_injected)
          test_load_completed = true;
      } else {
        char status[80];
        (void)snprintf(status, sizeof status,
                       "Slot %d could not be loaded.", slot + 1);
        Dkc2DesktopOverlaySetStatus(
            host.overlay, status, false);
      }
    }
    if (test_fast_forward_requested && !test_fast_forward_completed &&
        host_frame >= 60)
      controls.host_actions |= kDkc2HostFastForward;
    if (test_rewind_requested && !test_rewind_completed && host_frame >= 120)
      controls.host_actions |= kDkc2HostRewind;

    SdlSpeedMode mode = kSdlSpeedNormal;
    if (controls.host_actions & kDkc2HostRewind) mode = kSdlSpeedRewind;
    else if (controls.host_actions & kDkc2HostFastForward)
      mode = kSdlSpeedFastForward;
    if (mode != previous_mode) {
      ResetAudio(&host);
      audio_fraction = 0.0;
      deadline = SDL_GetPerformanceCounter();
      deadline_fraction = 0.0;
      previous_mode = mode;
    }

    bool frame_ready = overlay_open;
    double audio_ratio = 1.0;
#ifdef __APPLE__
    double emulate_seconds = 0.0;
#endif
    if (overlay_open) {
      mode = kSdlSpeedNormal;
    } else if (mode == kSdlSpeedRewind) {
      if (rewind_available &&
          Dkc2RewindHistoryPop(&rewind_history, rewind_scratch)) {
        if (!RtlLoadSnapshotFromMemory(rewind_scratch, rewind_snapshot_size)) {
          runtime_failure = true;
          break;
        }
        DrawFrame(&host);
        frame_ready = true;
        if (test_rewind_requested) test_rewind_completed = true;
      }
    } else {
      int frames_to_run = mode == kSdlSpeedFastForward
          ? kHostSpeedMultiplier : 1;
      unsigned long long iteration_start = host_frame;
      for (int run = 0; run < frames_to_run && host.running; run++) {
        if (input_playback.count)
          controls.controller =
              Dkc2InputPlaybackFrame(&input_playback, (size_t)host_frame);
        if (Dkc2InputRecorderIsOpen(&input_recorder) &&
            !Dkc2InputRecorderWrite(
                &input_recorder, controls.controller,
                input_recording_error, sizeof input_recording_error)) {
          fprintf(stderr, "Input recording failed: %s\n",
                  input_recording_error);
          Dkc2DiagnosticsFatal(input_recording_error);
          ShowError(input_recording_error);
          runtime_failure = true;
          host.running = false;
          break;
        }
        (void)Dkc2CoopTakeStompEvents();
#ifdef __APPLE__
        const uint64_t emulate_start = SDL_GetPerformanceCounter();
#endif
        (void)RtlRunFrame(controls.controller);
        PulseHaptics(&host, Dkc2CoopTakeStompEvents(), false);
        if (g_fail || !Dkc2LastLleResult()) {
          fprintf(stderr, "Runtime stopped at frame %llu (resume PC $%06x).\n",
                  host_frame + 1, (unsigned)Dkc2ResumePc());
          Dkc2DiagnosticsFatal("native runtime stopped unexpectedly");
          runtime_failure = true;
          break;
        }
        host_frame++;
        rewind_capture_counter++;
        if (rewind_available &&
            rewind_capture_counter >= kRewindSnapshotInterval) {
          rewind_capture_counter = 0;
          if (RtlSaveSnapshotToMemory(rewind_scratch, rewind_snapshot_size) !=
                  rewind_snapshot_size ||
              !Dkc2RewindHistoryPush(&rewind_history, rewind_scratch))
            rewind_available = false;
        }
        DrawFrame(&host);
        frame_ready = true;
#ifdef __APPLE__
        emulate_seconds += (double)(SDL_GetPerformanceCounter() - emulate_start) /
                           (double)frequency;
#endif
        audio_fraction += (double)kAudioRate / kVideoRate;
        int audio_frames = (int)audio_fraction;
        audio_fraction -= audio_frames;
        RtlRenderAudio(frame_audio, audio_frames, kAudioChannels);
        Dkc2MusicMix(frame_audio, audio_frames, 32040);
        if (mode == kSdlSpeedNormal) {
          host.audio_fill_average = Dkc2AudioFillAverage(
              host.audio_fill_average, AudioQueuedFrames(&host),
              kAudioFillWeight);
          audio_ratio = host.audio_primed
              ? Dkc2AudioRateRatio(host.audio_fill_average,
                                   AudioTargetFrames(&host),
                                   kAudioRateDeviation, kAudioRateGain)
              : 1.0;
          if (!QueueAudio(&host, frame_audio, audio_frames, audio_ratio)) {
            fprintf(stderr, "warning: SDL audio queue stopped\n");
            host.audio_available = false;
          }
        }
        if (test_frame_limit && host_frame >= test_frame_limit) {
          host.running = false;
          break;
        }
      }
      if (runtime_failure) break;
      if (test_fast_forward_requested && mode == kSdlSpeedFastForward &&
          host_frame - iteration_start == kHostSpeedMultiplier)
        test_fast_forward_completed = true;
    }

    /* Until the queue is primed the loop runs unpaced to fill it to the
     * rate-control target; once primed, rate control holds the fill there
     * and only a queue that has all but drained after a host stall runs
     * unpaced again. */
    const double queued_frames = AudioQueuedFrames(&host);
    if (!host.audio_primed && queued_frames >= AudioTargetFrames(&host))
      host.audio_primed = true;
    const bool should_pace =
        overlay_open || mode != kSdlSpeedNormal || !host.audio_available ||
        queued_frames >= (host.audio_primed ? (double)kMaximumFrameAudio
                                            : AudioTargetFrames(&host));
#ifdef __APPLE__
    /* With OpenGL's second vsync gate disabled, the presentation lands
     * either on the display's own refresh tick, when the display link has
     * locked, or on the exact DKC2 deadline of the host clock. Startup may
     * still run a few unpaced frames to establish the audio queue. */
    bool display_paced = false;
    Dkc2MacDisplayTick tick;
    memset(&tick, 0, sizeof tick);
    stage_before_pace = Dkc2MacHostSeconds();
    if (frame_ready && should_pace &&
        Dkc2SdlPresenterUsesSoftwarePacing(&host.presenter)) {
      display_paced = WaitForDisplayTick(display_link && display_lock, &pacer,
                                         &presented_tick, &seen_tick, &tick);
      if (display_paced) {
        deadline = SDL_GetPerformanceCounter();
        deadline_fraction = 0.0;
      } else {
        PaceFrame(&host, &deadline, &deadline_fraction);
      }
    }
    stage_paced = Dkc2MacHostSeconds();
#endif
    if (frame_ready) {
      /* The color filter works on the authentic frame only; an HD scene
       * is drawn by the presenter. */
      const uint8_t *present_pixels =
          host.hd_scene ? NULL
          : host.hd_pixels
              ? (const uint8_t *)host.hd_pixels
              : Dkc2DesktopColorFilterApply(&host.color_filter, host.pixels,
                                            host.filtered_pixels,
                                            Dkc2VideoPixelCount());
      const int present_width =
          host.hd_pixels ? host.hd_width : Dkc2VideoWidth();
      const int present_height =
          host.hd_pixels ? host.hd_height : kFrameHeight;
      /* DKC2_DESKTOP_SCREENSHOT: capture the presented drawable (after the
       * upscaler and screen model, before the overlay is composited over it
       * is not possible, so the overlay should be closed) at frame
       * DKC2_DESKTOP_SCREENSHOT_FRAME as a binary PPM, for verifying what
       * the GPU path draws without a visible window. */
      if (screenshot_path && !screenshot_rgb &&
          screenshot_index < screenshot_count &&
          host_frame >= screenshot_frames[screenshot_index]) {
        int dw = 0, dh = 0;
        Dkc2SdlPresenterDrawableSize(&host.presenter, &dw, &dh);
        if (dw > 0 && dh > 0) {
          screenshot_rgb = (uint8_t *)malloc((size_t)dw * (size_t)dh * 3u);
          if (screenshot_rgb)
            Dkc2SdlPresenterArmCapture(&host.presenter, screenshot_rgb, dw,
                                       dh);
        }
      }
      bool presented = false;
      if (host.hd_scene) {
        presented = Dkc2SdlPresenterPresentHd(&host.presenter, host.hd_scene,
                                              Dkc2DesktopOverlayRenderOpenGl,
                                              host.overlay);
        if (!presented) {
          /* Show the authentic frame now and compose on the CPU from the
           * next frame on. */
          fprintf(stderr, "warning: GPU HD composition failed; using the "
                          "CPU compositor\n");
          host.hd_gpu = false;
          host.hd_scene = NULL;
          present_pixels = Dkc2DesktopColorFilterApply(
              &host.color_filter, host.pixels, host.filtered_pixels,
              Dkc2VideoPixelCount());
        }
      }
      if (!host.hd_scene)
        presented = present_pixels &&
                    Dkc2SdlPresenterPresent(&host.presenter, present_pixels,
                                            present_width, present_height,
                                            Dkc2DesktopOverlayRenderOpenGl,
                                            host.overlay);
      if (!presented) {
        fprintf(stderr, "SDL video presentation failed: %s\n", SDL_GetError());
        Dkc2DiagnosticsFatal("SDL video presentation failed");
        runtime_failure = true;
        break;
      }
#ifdef __APPLE__
      if (pacing_log) {
        if (!display_paced) (void)Dkc2MacDisplayLinkLatest(&tick);
        const double stage_presented = Dkc2MacHostSeconds();
        fprintf(pacing_log,
                "%llu %s %.6f %llu %.6f %.6f %.6f %.1f %.5f %.6f %.6f %.6f %.6f\n",
                host_frame, display_paced ? "display" : "clock",
                stage_presented, (unsigned long long)tick.sequence,
                tick.target, tick.interval, emulate_seconds,
                host.audio_fill_average, audio_ratio,
                stage_pumped - stage_top, stage_before_pace - stage_pumped,
                stage_paced - stage_before_pace, stage_presented - stage_paced);
      }
#endif
      if (screenshot_rgb && host.presenter.capture_done) {
        char shot_path[1024];
        if (screenshot_count == 1) {
          snprintf(shot_path, sizeof shot_path, "%s", screenshot_path);
        } else {
          const char *dot = strrchr(screenshot_path, '.');
          const char *slash = strrchr(screenshot_path, '/');
          const int stem = dot && (!slash || dot > slash)
                               ? (int)(dot - screenshot_path)
                               : (int)strlen(screenshot_path);
          snprintf(shot_path, sizeof shot_path, "%.*s-%llu%s", stem,
                   screenshot_path, screenshot_frames[screenshot_index],
                   screenshot_path + stem);
        }
        FILE *shot = fopen(shot_path, "wb");
        if (shot) {
          fprintf(shot, "P6\n%d %d\n255\n", host.presenter.capture_width,
                  host.presenter.capture_height);
          fwrite(screenshot_rgb, 1,
                 (size_t)host.presenter.capture_width *
                     (size_t)host.presenter.capture_height * 3u,
                 shot);
          fclose(shot);
          fprintf(stdout, "screenshot: %s (%dx%d, %s, display=%s)\n",
                  shot_path, host.presenter.capture_width,
                  host.presenter.capture_height,
                  Dkc2SdlPresenterUpscalerName(host.presenter.upscaler),
                  Dkc2CrtDisplayName(host.presenter.display));
        }
        free(screenshot_rgb);
        screenshot_rgb = NULL;
        screenshot_index++;
        Dkc2SdlPresenterArmCapture(&host.presenter, NULL, 0, 0);
      }
    }
    if (should_pace &&
        (!Dkc2SdlPresenterUsesSoftwarePacing(&host.presenter) ||
         !frame_ready))
      PaceFrame(&host, &deadline, &deadline_fraction);
    else if (!should_pace) {
      deadline = SDL_GetPerformanceCounter();
      deadline_fraction = 0.0;
    }
    unsigned fps = 0;
    uint64_t now = SDL_GetPerformanceCounter();
    if (Dkc2DesktopFpsUpdate(&fps_counter, frame_ready, now, frequency, &fps)) {
      char title[112];
      (void)snprintf(title, sizeof title,
                     DKC2_PRODUCT_TITLE " (FPS: %u)", fps);
      if (assist_tools)
        (void)snprintf(title, sizeof title,
                       DKC2_PRODUCT_TITLE
                       " (FPS: %u) (Assist Tools: On)",
                       fps);
      if (Dkc2InputRecorderIsOpen(&input_recorder)) {
        size_t used = strlen(title);
        (void)snprintf(
            title + used, sizeof title - used, " (Recording Input)");
      }
      Dkc2SdlPresenterSetTitle(&host.presenter, title);
    }
  }

#ifdef __APPLE__
  Dkc2MacDisplayLinkStop();
  if (pacing_log) fclose(pacing_log);
#endif
  if (test_rewind_requested && !test_rewind_completed) runtime_failure = true;
  if (test_fast_forward_requested && !test_fast_forward_completed)
    runtime_failure = true;
  if (test_overlay_requested && !test_overlay_completed)
    runtime_failure = true;
  if (test_save_load_requested &&
      (!test_save_completed || !test_load_completed))
    runtime_failure = true;
  unsigned long long recorded_frames = input_recorder.frames;
  if (!Dkc2InputRecorderClose(
          &input_recorder, input_recording_error,
          sizeof input_recording_error)) {
    fprintf(stderr, "Input recording failed: %s\n", input_recording_error);
    runtime_failure = true;
  } else if (input_recording_path && *input_recording_path) {
    fprintf(stdout, "Input recording completed: %llu frames at %s\n",
            recorded_frames, input_recording_path);
  }
  bool completed = !runtime_failure && !g_fail && Dkc2LastLleResult();
  const char *frame_output = getenv("DKC2_FRAME_PPM");
  if (frame_output && *frame_output &&
      !WriteFramePpm(frame_output, host.pixels))
    completed = false;
  if (sram_enabled && completed) RtlWriteSram();
  Dkc2DesktopOverlayGetSettings(host.overlay, settings);
  if (aspect_override_active || widescreen_override_active) {
    settings->aspect_index = persisted_aspect;
    settings->widescreen =
        persisted_aspect != kDkc2VideoAspectNative;
  }
  Dkc2DiagnosticsShutdown(completed ? "clean_exit" : "runtime_failure");
  Dkc2RewindHistoryDestroy(&rewind_history);
  Dkc2InputPlaybackFree(&input_playback);
  free(rewind_scratch);
  DestroyHd(&host);
  free(rom);
  ShutdownHost(&host);
  free(screenshot_rgb);
  if (test_frame_limit) {
    fprintf(stdout,
            "result=desktop_completed frames=%llu rewind_restore=%s "
            "fast_forward=%s save_load=%s host=sdl2\n",
            host_frame,
            test_rewind_requested
                ? (test_rewind_completed ? "passed" : "failed")
                : "not_requested",
            test_fast_forward_requested
                ? (test_fast_forward_completed ? "passed" : "failed")
                : "not_requested",
            test_save_load_requested
                ? (test_save_completed && test_load_completed
                       ? "passed" : "failed")
                : "not_requested");
  }
  return completed ? 0 : 5;
}

int main(int argc, char **argv) {
  SDL_SetMainReady();
  bool force_launcher = false;
  int rom_argument = 1;
  if (argc >= 2 && strcmp(argv[1], "--launcher") == 0) {
    force_launcher = true;
    rom_argument++;
  }
  if (argc > rom_argument + 1) {
    fprintf(stderr, "Usage: %s [--launcher] [ROM.smc]\n", argv[0]);
    return 2;
  }
  char rom_path[kPathCapacity] = {0};
  if (argc == rom_argument + 1) {
    if (!snesrecomp_abspath(argv[rom_argument], rom_path, sizeof rom_path))
      (void)snprintf(rom_path, sizeof rom_path, "%s", argv[rom_argument]);
  }
#ifdef __APPLE__
  char assets_path[kPathCapacity] = {0};
  char macos_error[256] = {0};
  if (!Dkc2MacPrepareRuntimeDirectory(
          assets_path, sizeof assets_path, macos_error,
          sizeof macos_error)) {
    fprintf(stderr, "Unable to prepare DKC2 user data: %s\n", macos_error);
    return 2;
  }
  Dkc2LauncherSetAssetsPath(assets_path);
#else
  (void)snesrecomp_anchor_to_exe_dir();
#endif
  if (!Dkc2DiagnosticsInit("sdl2", DKC2_RELEASE_VERSION))
    fprintf(stderr, "warning: diagnostics could not be initialized\n");

  RecompLauncherCSettings settings;
  Dkc2LauncherSettingsDefault(&settings);
  Dkc2LauncherSettingsLoad(&settings);
  if (!rom_path[0])
    (void)Dkc2LauncherReadRomCache(rom_path, sizeof rom_path);
#ifdef __SWITCH__
  /* The console has no file picker: the ROM sits beside the .nro, which is
   * the working directory under the homebrew loader. */
  if (!rom_path[0])
    (void)snprintf(rom_path, sizeof rom_path, "%s", "dkc2.smc");
#endif
  bool suppress_launcher = EnvironmentEnabled("SNESRECOMP_NO_LAUNCHER") ||
                           EnvironmentEnabled("DKC2_DESKTOP_TEST_HIDDEN");
  bool show_launcher = !suppress_launcher &&
      (force_launcher || !settings.skip_launcher || !rom_path[0]);
  if (show_launcher) {
    char selected_rom[kPathCapacity] = {0};
    int action = Dkc2LauncherRun(&settings, rom_path, selected_rom,
                                 sizeof selected_rom, NULL, 0);
    if (action == 1) return 0;
    if (action == 0 && selected_rom[0])
      (void)snprintf(rom_path, sizeof rom_path, "%s", selected_rom);
    (void)Dkc2LauncherSettingsSave(&settings);
  }
  if (!rom_path[0]) {
    fprintf(stderr, "No ROM was selected. Run with --launcher to choose one.\n");
    return 0;
  }
  (void)Dkc2LauncherWriteRomCache(rom_path);
  int result = RunGame(rom_path, &settings);
  (void)Dkc2LauncherSettingsSave(&settings);
  return result;
}
