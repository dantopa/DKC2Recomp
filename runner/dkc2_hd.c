#include "dkc2_hd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dkc2_native_render.h"

enum {
  kNativeWidth = kDkc2NativeWidth,
  kHeight = kDkc2NativeHeight,
  /* Widest frame the PPU can export: the native columns plus its full
   * widescreen margin on each side. */
  kMaxWidth = kPpuXPixels + 2 * kPpuExtraLeftRight,
  kMaxPixels = kMaxWidth * kHeight,
  kBgLayers = 3,
  /* Graphic ids are 4-byte directory offsets below $35A0. */
  kGraphicSlots = 0x35a0 / 4,
  kPalettes = 8,
  kCacheSlots = 1024,
  kCacheFlush = kCacheSlots * 3 / 4,
  /* Smooth upscale taps reach two source pixels past either edge. */
  kTapReach = 2,
  kPathMax = 1024,
  kBlock = kDkc2NativeBlockSize,
  kBlockPixels = kBlock * kBlock,
  /* Distinct terrain blocks remembered (art and dump state); a level uses a
   * few hundred. */
  kTerrainSlots = 8192,
  /* Full-screen art (title and other still screens). */
  kScreenSlots = 16,
  /* Frames BG1 must stay unchanged before the dump records it as a screen. */
  kScreenStableFrames = 60,
};

typedef struct HdImage {
  int width, height;
  uint32_t pixels[];  /* straight-alpha ARGB */
} HdImage;

typedef enum PackState {
  kPackUnknown = 0,
  kPackLoaded,
  kPackMissing,
} PackState;

typedef struct CacheEntry {
  uint64_t key;
  HdImage *image;
} CacheEntry;

typedef struct TerrainEntry {
  uint64_t key;  /* 0: free slot */
  HdImage *image;
  uint8_t state;  /* PackState */
  bool dumped;
} TerrainEntry;

typedef struct ScreenEntry {
  uint64_t key;
  HdImage *reference;  /* native BG1 the art was made for */
  HdImage *art;
} ScreenEntry;

typedef struct SpriteState {
  Dkc2NativeSprite sprite;
  bool decoded;
  bool replaced;
  uint8_t rank;
  int left, top, width, height;
} SpriteState;

struct Dkc2Hd {
  int scale;
  Dkc2HdFilter filter;
  Dkc2HdFilter background;
  /* Smooth background: first source tap and four Catmull-Rom weights for
   * each output phase of a source pixel. */
  int tap_offset[kDkc2HdMaxScale];
  float tap_weight[kDkc2HdMaxScale][4];
  char pack_dir[kPathMax];
  char dump_dir[kPathMax];
  const uint8_t *rom;
  size_t rom_size;
  bool active;
  /* Frame width, and the frame column of native screen X 0 for sprites
   * (the margin less the widescreen presentation bias). */
  int width;
  int origin;
  bool has_previous;
  uint8_t previous_wram[0x2000];
  uint32_t bg[kBgLayers][kMaxPixels];
  uint32_t obj[kMaxPixels];
  /* Top-most sprite-table record (index + 1) and its color per pixel. */
  uint8_t owner[kMaxPixels];
  uint32_t native[kMaxPixels];
  SpriteState sprites[kDkc2NativeSpriteMax];
  unsigned sprite_count;
  Dkc2NativeSpriteFrame frame;
  HdImage *pack[kGraphicSlots][kPalettes];
  uint8_t pack_state[kGraphicSlots][kPalettes];
  uint8_t dumped[kGraphicSlots];
  CacheEntry cache[kCacheSlots];
  unsigned cache_count;
  uint32_t base[kMaxPixels];
  /* BG1 art: when the pack lists terrain blocks or screens, BG1 leaves the
   * rendered frame and is composed here, art where the pack has it and the
   * background filter elsewhere. */
  bool layer_pack;
  /* This frame composes BG1 here: the pack has BG1 art and BG1 is drawn on
   * the main screen only (captures see the main screen; scenes such as
   * Mainbrace Mayhem draw BG1 on the subscreen through color math). */
  bool layer_frame;
  ScreenEntry screens[kScreenSlots];
  unsigned screen_count;
  /* Dump: hash of the native BG1, frames it has been unchanged, and the
   * last one written. */
  uint64_t screen_hash;
  unsigned screen_stable;
  uint64_t screen_dumped;
  TerrainEntry terrain[kTerrainSlots];
  unsigned terrain_count;
  uint32_t block[kBlockPixels];
  uint8_t base_rank[kMaxPixels];
  uint8_t bg1_rank[kMaxPixels];
  uint32_t *layer;  /* HD BG1, straight alpha */
  float *rows;      /* smooth upscale: premultiplied rows (see Create) */
  /* Smooth upscale anti-ringing: per-channel minimum and maximum of the 2x2
   * source pixels from (x, y) down-right; the low alpha byte flags any
   * opaque pixel among them. */
  uint32_t range_low[kMaxPixels];
  uint32_t range_high[kMaxPixels];
  /* One premultiplied source line, padded by kTapReach on each side. */
  float premultiplied[(kMaxWidth + 2 * kTapReach) * 4];
  uint32_t *output;
};

static uint32_t Rgb(uint32_t pixel) { return pixel & 0xffffffu; }

bool Dkc2HdConfigFromEnvironment(Dkc2HdConfig *config, const char **error) {
  *error = NULL;
  const char *scale_text = getenv("DKC2_HD");
  if (!scale_text || !*scale_text)
    return false;
  char *end = NULL;
  const long scale = strtol(scale_text, &end, 10);
  if (!end || *end || scale < kDkc2HdMinScale || scale > kDkc2HdMaxScale) {
    *error = "DKC2_HD must be a scale between 2 and 8";
    return false;
  }
  const char *filter = getenv("DKC2_HD_FILTER");
  if (!filter || !*filter || strcmp(filter, "smooth") == 0) {
    config->filter = kDkc2HdFilterSmooth;
  } else if (strcmp(filter, "nearest") == 0) {
    config->filter = kDkc2HdFilterNearest;
  } else {
    *error = "DKC2_HD_FILTER must be nearest or smooth";
    return false;
  }
  config->scale = (int)scale;
  config->background = kDkc2HdFilterSmooth;
  config->pack_dir = getenv("DKC2_HD_PACK");
  config->dump_dir = getenv("DKC2_HD_DUMP");
  return true;
}

static double CatmullRom(double t) {
  t = t < 0 ? -t : t;
  if (t < 1)
    return (1.5 * t - 2.5) * t * t + 1;
  if (t < 2)
    return ((-0.5 * t + 2.5) * t - 4) * t + 2;
  return 0;
}

static bool CopyPath(char *destination, const char *source) {
  destination[0] = '\0';
  if (!source || !*source)
    return true;
  const size_t length = strlen(source);
  if (length >= kPathMax)
    return false;
  memcpy(destination, source, length + 1);
  return true;
}

static HdImage *LoadImage(const char *path);

/* Reads screens.txt: one 16-digit hex key per line, each with a native BG1
 * reference "s_<key>.ref" (as the dump writes it) and art "s_<key>.rgba". */
static void LoadScreens(Dkc2Hd *hd) {
  char path[kPathMax + 64];
  snprintf(path, sizeof path, "%s/screens.txt", hd->pack_dir);
  FILE *index = fopen(path, "r");
  if (!index)
    return;
  unsigned long long key = 0;
  while (hd->screen_count < kScreenSlots &&
         fscanf(index, "%16llx", &key) == 1) {
    ScreenEntry *screen = &hd->screens[hd->screen_count];
    snprintf(path, sizeof path, "%s/s_%016llx.ref", hd->pack_dir, key);
    screen->reference = LoadImage(path);
    snprintf(path, sizeof path, "%s/s_%016llx.rgba", hd->pack_dir, key);
    screen->art = LoadImage(path);
    if (!screen->reference || !screen->art ||
        screen->reference->width != kNativeWidth ||
        screen->reference->height != kHeight) {
      fprintf(stderr, "warning: DKC2_HD_PACK screen %016llx is incomplete\n",
              key);
      free(screen->reference);
      free(screen->art);
      *screen = (ScreenEntry){0};
      continue;
    }
    screen->key = key;
    hd->screen_count++;
  }
  fclose(index);
}

Dkc2Hd *Dkc2HdCreate(const Dkc2HdConfig *config, const uint8_t *rom,
                     size_t rom_size) {
  if (!config || !rom || config->scale < kDkc2HdMinScale ||
      config->scale > kDkc2HdMaxScale)
    return NULL;
  Dkc2Hd *hd = (Dkc2Hd *)calloc(1, sizeof *hd);
  if (!hd)
    return NULL;
  hd->scale = config->scale;
  hd->filter = config->filter;
  hd->background = config->background;
  hd->rom = rom;
  hd->rom_size = rom_size;
  for (int phase = 0; phase < hd->scale; phase++) {
    const double source = (phase + 0.5) / hd->scale - 0.5;
    hd->tap_offset[phase] = (source < 0 ? -1 : 0) - 1;
    for (int k = 0; k < 4; k++)
      hd->tap_weight[phase][k] =
          (float)CatmullRom(source - (hd->tap_offset[phase] + k));
  }
  const size_t output_pixels =
      (size_t)kMaxPixels * (size_t)(config->scale * config->scale);
  hd->output = (uint32_t *)malloc(sizeof(uint32_t) * output_pixels);
  if (!hd->output || !CopyPath(hd->pack_dir, config->pack_dir) ||
      !CopyPath(hd->dump_dir, config->dump_dir)) {
    Dkc2HdDestroy(hd);
    return NULL;
  }
  if (hd->pack_dir[0]) {
    char path[kPathMax + 32];
    snprintf(path, sizeof path, "%s/terrain.txt", hd->pack_dir);
    FILE *index = fopen(path, "r");
    hd->layer_pack = index != NULL;
    if (index)
      fclose(index);
    LoadScreens(hd);
    hd->layer_pack |= hd->screen_count > 0;
  }
  if (hd->layer_pack)
    hd->layer = (uint32_t *)malloc(sizeof(uint32_t) * output_pixels);
  if (hd->background == kDkc2HdFilterSmooth)
    /* Horizontally scaled rows, then one vertically filtered row. */
    hd->rows = (float *)malloc(sizeof(float) * 4u * (size_t)kMaxWidth *
                               (size_t)config->scale * (kHeight + 1u));
  if ((hd->layer_pack && !hd->layer) ||
      (hd->background == kDkc2HdFilterSmooth && !hd->rows)) {
    Dkc2HdDestroy(hd);
    return NULL;
  }
  return hd;
}

static void FlushCache(Dkc2Hd *hd) {
  for (unsigned i = 0; i < kCacheSlots; i++) {
    free(hd->cache[i].image);
    hd->cache[i].image = NULL;
    hd->cache[i].key = 0;
  }
  hd->cache_count = 0;
}

void Dkc2HdDestroy(Dkc2Hd *hd) {
  if (!hd)
    return;
  FlushCache(hd);
  for (unsigned graphic = 0; graphic < kGraphicSlots; graphic++)
    for (unsigned palette = 0; palette < kPalettes; palette++)
      free(hd->pack[graphic][palette]);
  for (unsigned i = 0; i < kTerrainSlots; i++)
    free(hd->terrain[i].image);
  for (unsigned i = 0; i < hd->screen_count; i++) {
    free(hd->screens[i].reference);
    free(hd->screens[i].art);
  }
  free(hd->layer);
  free(hd->rows);
  free(hd->output);
  free(hd);
}

void Dkc2HdBeginFrame(Dkc2Hd *hd, Ppu *ppu, int frame_width) {
  if (!hd || !ppu)
    return;
  static const PpuOverlaySource kBgSources[kBgLayers] = {
      kPpuOverlaySource_Bg1, kPpuOverlaySource_Bg2, kPpuOverlaySource_Bg3};
  hd->active = (ppu->bgmode & 7) == 1 && frame_width >= kNativeWidth &&
               frame_width <= kMaxWidth &&
               (frame_width - kNativeWidth) % 2 == 0;
  hd->width = frame_width;
  const size_t pitch = (size_t)frame_width * 4u;
  PpuClearOverlayCaptures(ppu);
  for (unsigned layer = 0; layer < kBgLayers; layer++)
    PpuBindOverlaySurface(ppu, kBgSources[layer],
                          hd->active ? (uint8_t *)hd->bg[layer] : NULL,
                          pitch);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj,
                        hd->active ? (uint8_t *)hd->obj : NULL, pitch);
  hd->layer_frame = hd->active && hd->layer_pack &&
                      (ppu->screenEnabled[0] & 1u) &&
                      !(ppu->screenEnabled[1] & 1u);
  if (!hd->active)
    return;
  /* Captures span the margins too: screen X runs from -extra. */
  const int extra = (frame_width - kNativeWidth) / 2;
  for (unsigned layer = 0; layer < kBgLayers; layer++)
    PpuSetOverlayCapture(ppu, kBgSources[layer], -extra, 0, frame_width,
                         kHeight,
                         layer == 0 && hd->layer_frame
                             ? kPpuOverlayFlag_RemoveFromGame
                             : 0);
  /* Objects leave the rendered frame; the host composes them back. */
  PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj, -extra, 0, frame_width,
                       kHeight, kPpuOverlayFlag_RemoveFromGame);
  PpuSetOverlayOamRange(ppu, 0, 128);
}

static HdImage *NewImage(int width, int height) {
  HdImage *image = (HdImage *)malloc(
      sizeof(HdImage) + sizeof(uint32_t) * (size_t)width * (size_t)height);
  if (image) {
    image->width = width;
    image->height = height;
  }
  return image;
}

static HdImage *LoadImage(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  uint8_t header[8];
  HdImage *image = NULL;
  if (fread(header, 1, sizeof header, file) == sizeof header &&
      memcmp(header, "DKHD", 4) == 0) {
    const int width = header[4] | header[5] << 8;
    const int height = header[6] | header[7] << 8;
    if (width > 0 && height > 0)
      image = NewImage(width, height);
    for (int i = 0; image && i < width * height; i++) {
      uint8_t rgba[4];
      if (fread(rgba, 1, sizeof rgba, file) != sizeof rgba) {
        free(image);
        image = NULL;
        break;
      }
      image->pixels[i] = (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 |
                         (uint32_t)rgba[1] << 8 | rgba[2];
    }
  }
  fclose(file);
  return image;
}

static bool WriteFrameImage(const char *path,
                            const Dkc2NativeSpriteFrame *frame) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "warning: DKC2_HD_DUMP could not write %s\n", path);
    return false;
  }
  const uint8_t header[8] = {'D', 'K', 'H', 'D',
                             (uint8_t)frame->width, (uint8_t)(frame->width >> 8),
                             (uint8_t)frame->height,
                             (uint8_t)(frame->height >> 8)};
  bool ok = fwrite(header, 1, sizeof header, file) == sizeof header;
  for (int i = 0; ok && i < frame->width * frame->height; i++) {
    const uint32_t pixel = frame->pixels[i];
    const uint8_t rgba[4] = {(uint8_t)(pixel >> 16), (uint8_t)(pixel >> 8),
                             (uint8_t)pixel, pixel ? 255 : 0};
    ok = fwrite(rgba, 1, sizeof rgba, file) == sizeof rgba;
  }
  if (fclose(file) != 0 || !ok) {
    fprintf(stderr, "warning: DKC2_HD_DUMP could not write %s\n", path);
    return false;
  }
  return true;
}

static const HdImage *PackImage(Dkc2Hd *hd, unsigned slot, unsigned palette) {
  if (!hd->pack_dir[0])
    return NULL;
  if (hd->pack_state[slot][palette] == kPackUnknown) {
    char path[kPathMax + 32];
    snprintf(path, sizeof path, "%s/%04x_%x.rgba", hd->pack_dir, slot * 4u,
             palette);
    hd->pack[slot][palette] = LoadImage(path);
    hd->pack_state[slot][palette] =
        hd->pack[slot][palette] ? kPackLoaded : kPackMissing;
  }
  return hd->pack[slot][palette];
}

/* Premultiplied Catmull-Rom upscale of a decoded frame, so transparent
 * neighbours never darken or tint the edges. */
static HdImage *SmoothImage(const Dkc2NativeSpriteFrame *frame, int scale) {
  const int width = frame->width * scale, height = frame->height * scale;
  HdImage *image = NewImage(width, height);
  if (!image)
    return NULL;
  for (int y = 0; y < height; y++) {
    const double source_y = (y + 0.5) / scale - 0.5;
    const int base_y = (int)(source_y + 2) - 2;
    for (int x = 0; x < width; x++) {
      const double source_x = (x + 0.5) / scale - 0.5;
      const int base_x = (int)(source_x + 2) - 2;
      double sum[4] = {0, 0, 0, 0};
      for (int ky = -1; ky <= 2; ky++) {
        const int sy = base_y + ky;
        const double wy = CatmullRom(source_y - sy);
        if (sy < 0 || sy >= frame->height || wy == 0)
          continue;
        for (int kx = -1; kx <= 2; kx++) {
          const int sx = base_x + kx;
          const double weight = wy * CatmullRom(source_x - sx);
          if (sx < 0 || sx >= frame->width || weight == 0)
            continue;
          const uint32_t pixel = frame->pixels[sy * frame->width + sx];
          if (!pixel)
            continue;
          sum[0] += weight * 255.0;
          sum[1] += weight * (double)((pixel >> 16) & 0xff);
          sum[2] += weight * (double)((pixel >> 8) & 0xff);
          sum[3] += weight * (double)(pixel & 0xff);
        }
      }
      uint32_t result = 0;
      if (sum[0] >= 1.0) {
        const double alpha = sum[0] > 255.0 ? 255.0 : sum[0];
        result = (uint32_t)(alpha + 0.5) << 24;
        for (int channel = 1; channel < 4; channel++) {
          double value = sum[channel] * 255.0 / sum[0];
          value = value < 0 ? 0 : value > 255 ? 255 : value;
          result |= (uint32_t)(value + 0.5) << (8 * (3 - channel));
        }
      }
      image->pixels[y * width + x] = result;
    }
  }
  return image;
}

static uint64_t PaletteKey(const Ppu *ppu, const SpriteState *state) {
  const unsigned palette = (state->sprite.properties >> 9) & 7u;
  uint32_t hash = 2166136261u ^ ppu->brightnessMult[31];
  for (unsigned i = 0; i < 16; i++) {
    hash ^= ppu->cgram[0x80u + palette * 16u + i];
    hash *= 16777619u;
  }
  return (uint64_t)hash << 32 | (uint64_t)palette << 16 |
         state->sprite.graphic_id;
}

static const HdImage *SmoothCached(Dkc2Hd *hd, const Ppu *ppu,
                                   const SpriteState *state) {
  const uint64_t key = PaletteKey(ppu, state);
  unsigned slot = (unsigned)((key ^ key >> 29) * 0x9e3779b97f4a7c15ull >> 54);
  for (;; slot = (slot + 1) % kCacheSlots) {
    if (!hd->cache[slot].image)
      break;
    if (hd->cache[slot].key == key)
      return hd->cache[slot].image;
  }
  if (hd->cache_count >= kCacheFlush) {
    FlushCache(hd);
    return SmoothCached(hd, ppu, state);
  }
  HdImage *image = SmoothImage(&hd->frame, hd->scale);
  if (image) {
    hd->cache[slot].key = key;
    hd->cache[slot].image = image;
    hd->cache_count++;
  }
  return image;
}

/* Decodes each live record, builds the top-most ownership map and decides
 * which records the HD path draws: only those the PPU actually showed as
 * decoded. */
static void PrepareSprites(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram) {
  Dkc2NativeSprite sprites[kDkc2NativeSpriteMax];
  hd->sprite_count = Dkc2NativeCollectSprites(ram, hd->rom, hd->rom_size,
                                              sprites);
  memset(hd->owner, 0, sizeof hd->owner);
  for (unsigned k = 0; k < hd->sprite_count; k++) {
    SpriteState *state = &hd->sprites[k];
    memset(state, 0, sizeof *state);
    state->sprite = sprites[k];
    /* From native screen X to frame columns. */
    state->sprite.x = (int16_t)(state->sprite.x + hd->origin);
    if (!Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
      continue;
    state->decoded = true;
    state->left = hd->frame.left;
    state->top = hd->frame.top;
    state->width = hd->frame.width;
    state->height = hd->frame.height;
    state->rank = (uint8_t)SPRITE_PRIO_TO_PRIO(
        (state->sprite.properties & 0x3000u) >> 12,
        (state->sprite.properties & 0x800u) == 0);
    for (int fy = 0; fy < hd->frame.height; fy++) {
      for (int fx = 0; fx < hd->frame.width; fx++) {
        const uint32_t pixel = hd->frame.pixels[fy * hd->frame.width + fx];
        int x = 0, y = 0;
        Dkc2NativeSpritePixelScreen(&state->sprite, &hd->frame, fx, fy, &x,
                                    &y);
        if (!pixel || x < 0 || x >= hd->width || y < 0 || y >= kHeight)
          continue;
        hd->owner[y * hd->width + x] = (uint8_t)(k + 1);
        hd->native[y * hd->width + x] = pixel;
      }
    }
  }
  for (unsigned k = 0; k < hd->sprite_count; k++) {
    SpriteState *state = &hd->sprites[k];
    if (!state->decoded)
      continue;
    size_t compared = 0, wrong = 0;
    for (size_t i = 0; i < (size_t)hd->width * kHeight; i++) {
      if (hd->owner[i] != k + 1)
        continue;
      compared++;
      wrong += Rgb(hd->obj[i]) != Rgb(hd->native[i]) || !hd->obj[i];
    }
    state->replaced = compared > 0 && wrong * 8 <= compared;
  }
}

static void DumpSprite(Dkc2Hd *hd, const Ppu *ppu, const SpriteState *state) {
  const unsigned slot = state->sprite.graphic_id / 4u;
  const unsigned palette = (state->sprite.properties >> 9) & 7u;
  /* Frames seen during a screen fade would be dumped darkened; wait for
   * one at full brightness. */
  if (!hd->dump_dir[0] || (hd->dumped[slot] & (1u << palette)) ||
      ppu->brightnessMult[31] != 255u)
    return;
  hd->dumped[slot] |= (uint8_t)(1u << palette);
  if (!Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
    return;
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/%04x_%x.rgba", hd->dump_dir,
           state->sprite.graphic_id, palette);
  if (!WriteFrameImage(path, &hd->frame))
    return;
  snprintf(path, sizeof path, "%s/index.txt", hd->dump_dir);
  FILE *index = fopen(path, "a");
  bool ok = index && fprintf(index, "%04x %x %04x %d %d\n",
                             state->sprite.graphic_id, palette,
                             state->sprite.type, hd->frame.width,
                             hd->frame.height) >= 0;
  if (index && fclose(index) != 0)
    ok = false;
  if (!ok)
    fprintf(stderr, "warning: DKC2_HD_DUMP could not update %s\n", path);
}

static uint32_t Blend(uint32_t under, uint32_t over, unsigned alpha) {
  uint32_t result = 0xff000000u;
  for (int shift = 0; shift < 24; shift += 8) {
    const unsigned a = (under >> shift) & 0xff, b = (over >> shift) & 0xff;
    result |= ((a * (255u - alpha) + b * alpha + 127u) / 255u) << shift;
  }
  return result;
}

static uint8_t BackgroundRank(const Dkc2Hd *hd, size_t i) {
  uint8_t rank = 0;
  for (unsigned layer = 0; layer < kBgLayers; layer++) {
    const uint8_t layer_rank = (uint8_t)(hd->bg[layer][i] >> 24);
    if (layer_rank > rank)
      rank = layer_rank;
  }
  return rank;
}

/* Whether the authentic object pixel at i belongs to a replaced record. */
static bool ReplacedPixel(const Dkc2Hd *hd, size_t i) {
  const unsigned owner = hd->owner[i];
  return owner && hd->sprites[owner - 1].replaced &&
         Rgb(hd->obj[i]) == Rgb(hd->native[i]);
}

static void DrawSprite(Dkc2Hd *hd, const Ppu *ppu, unsigned k) {
  const SpriteState *state = &hd->sprites[k];
  const unsigned slot = state->sprite.graphic_id / 4u;
  const unsigned palette = (state->sprite.properties >> 9) & 7u;
  const HdImage *image = PackImage(hd, slot, palette);
  const unsigned brightness = image ? ppu->brightnessMult[31] : 255u;
  if (!image && hd->filter == kDkc2HdFilterSmooth) {
    if (!Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
      return;
    image = SmoothCached(hd, ppu, state);
  }
  if (!image && !Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
    return;

  const int scale = hd->scale;
  const int width = state->width * scale, height = state->height * scale;
  const bool flip_x = (state->sprite.properties & 0x4000u) != 0;
  const bool flip_y = (state->sprite.properties & 0x8000u) != 0;
  const int out_width = hd->width * scale, out_height = kHeight * scale;
  for (int hy = 0; hy < height; hy++) {
    const int y = flip_y ? (state->sprite.y - state->top) * scale - 1 - hy
                         : (state->sprite.y + state->top) * scale + hy;
    if (y < 0 || y >= out_height)
      continue;
    for (int hx = 0; hx < width; hx++) {
      const int x = flip_x ? (state->sprite.x - state->left) * scale - 1 - hx
                           : (state->sprite.x + state->left) * scale + hx;
      if (x < 0 || x >= out_width)
        continue;
      uint32_t pixel;
      if (image) {
        pixel = image->pixels[(size_t)(hy * image->height / height) *
                                  (size_t)image->width +
                              (size_t)(hx * image->width / width)];
      } else {
        pixel = hd->frame.pixels[(hy / scale) * hd->frame.width + hx / scale];
        pixel = pixel ? 0xff000000u | Rgb(pixel) : 0;
      }
      const unsigned alpha = pixel >> 24;
      if (!alpha)
        continue;
      const size_t i =
          (size_t)(y / scale) * (size_t)hd->width + (size_t)(x / scale);
      if (state->rank <= BackgroundRank(hd, i))
        continue;
      const uint32_t object = hd->obj[i];
      if (object && !ReplacedPixel(hd, i)) {
        const bool from_table = hd->owner[i] &&
                                Rgb(object) == Rgb(hd->native[i]);
        if (from_table ? hd->owner[i] - 1u > k
                       : (uint8_t)(object >> 24) >= state->rank)
          continue;
      }
      if (brightness != 255u) {
        uint32_t scaled = 0;
        for (int shift = 0; shift < 24; shift += 8)
          scaled |= (((pixel >> shift) & 0xff) * brightness / 255u) << shift;
        pixel = scaled;
      }
      uint32_t *destination = &hd->output[(size_t)y * (size_t)out_width + x];
      *destination = Blend(*destination, pixel, alpha);
    }
  }
}

/* Opacity of a frame or captured-layer pixel: captures carry the
 * composition rank in the alpha byte and 0 when transparent. */
static uint32_t Opaque(uint32_t pixel) {
  return pixel ? 0xff000000u | Rgb(pixel) : 0;
}

static void UpscaleNearest(const Dkc2Hd *hd, const uint32_t *source,
                           uint32_t *destination) {
  const int scale = hd->scale;
  const size_t out_width = (size_t)hd->width * (size_t)scale;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < hd->width; x++) {
      const uint32_t pixel =
          Opaque(source[(size_t)y * (size_t)hd->width + (size_t)x]);
      for (int sy = 0; sy < scale; sy++) {
        uint32_t *row = destination +
                        (size_t)(y * scale + sy) * out_width +
                        (size_t)(x * scale);
        for (int sx = 0; sx < scale; sx++)
          row[sx] = pixel;
      }
    }
  }
}

static int ClampIndex(int value, int count) {
  return value < 0 ? 0 : value >= count ? count - 1 : value;
}

/* Separable premultiplied Catmull-Rom upscale: smooth gradients and edges
 * instead of square pixels, with transparent neighbours never tinting. */
static void UpscaleSmooth(Dkc2Hd *hd, const uint32_t *source,
                          uint32_t *destination) {
  const int scale = hd->scale;
  const int out_width = hd->width * scale;
  uint32_t *range_low = hd->range_low;
  uint32_t *range_high = hd->range_high;
  for (int y = 0; y < kHeight; y++) {
    const uint32_t *lines[2] = {
        source + (size_t)y * (size_t)hd->width,
        source + (size_t)ClampIndex(y + 1, kHeight) * (size_t)hd->width};
    for (int x = 0; x < hd->width; x++) {
      const int next = ClampIndex(x + 1, hd->width);
      const uint32_t near[4] = {lines[0][x], lines[0][next], lines[1][x],
                                lines[1][next]};
      uint32_t low = 0x00ffffffu, high = 0;
      for (int k = 0; k < 4; k++) {
        if (!near[k])
          continue;
        high |= 0xff000000u;
        for (int shift = 0; shift < 24; shift += 8) {
          const uint32_t value = near[k] & (0xffu << shift);
          if (value < (low & (0xffu << shift)))
            low = (low & ~(0xffu << shift)) | value;
          if (value > (high & (0xffu << shift)))
            high = (high & ~(0xffu << shift)) | value;
        }
      }
      range_low[(size_t)y * (size_t)hd->width + (size_t)x] = low;
      range_high[(size_t)y * (size_t)hd->width + (size_t)x] = high;
    }
  }
  /* Premultiplied source line (transparent pixels are all zero), padded by
   * the kernel reach on both sides so the taps need no clamping. */
  float *line = hd->premultiplied;
  for (int y = 0; y < kHeight; y++) {
    const uint32_t *source_line = source + (size_t)y * (size_t)hd->width;
    for (int x = -kTapReach; x < hd->width + kTapReach; x++) {
      const uint32_t pixel = source_line[ClampIndex(x, hd->width)];
      float *texel = line + (size_t)(x + kTapReach) * 4u;
      texel[0] = pixel ? 255.0f : 0.0f;
      texel[1] = pixel ? (float)((pixel >> 16) & 0xff) : 0.0f;
      texel[2] = pixel ? (float)((pixel >> 8) & 0xff) : 0.0f;
      texel[3] = pixel ? (float)(pixel & 0xff) : 0.0f;
    }
    float *row = hd->rows + (size_t)y * (size_t)out_width * 4u;
    for (int x = 0; x < out_width; x++) {
      const int phase = x % scale;
      const float *weight = hd->tap_weight[phase];
      const float *taps =
          line + (size_t)(x / scale + hd->tap_offset[phase] + kTapReach) * 4u;
      float *out = row + (size_t)x * 4u;
      for (int c = 0; c < 4; c++)
        out[c] = weight[0] * taps[c] + weight[1] * taps[4 + c] +
                 weight[2] * taps[8 + c] + weight[3] * taps[12 + c];
    }
  }
  float *sums = hd->rows + (size_t)kHeight * (size_t)out_width * 4u;
  const size_t values = (size_t)out_width * 4u;
  for (int y = 0; y < kHeight * scale; y++) {
    const int phase = y % scale;
    const int first = y / scale + hd->tap_offset[phase];
    const float *taps[4];
    for (int k = 0; k < 4; k++)
      taps[k] = hd->rows +
                (size_t)ClampIndex(first + k, kHeight) * (size_t)out_width * 4u;
    const float *weights = hd->tap_weight[phase];
    /* One straight pass over the row so the compiler can vectorize it. */
    for (size_t j = 0; j < values; j++)
      sums[j] = weights[0] * taps[0][j] + weights[1] * taps[1][j] +
                weights[2] * taps[2][j] + weights[3] * taps[3][j];
    /* Ranges of the 2x2 source pixels nearest each output pixel (taps 1
     * and 2 of both passes). */
    const size_t range_row =
        (size_t)ClampIndex(first + 1, kHeight) * (size_t)hd->width;
    uint32_t *out = destination + (size_t)y * (size_t)out_width;
    for (int x = 0; x < out_width; x++) {
      const float *sum = sums + (size_t)x * 4u;
      if (sum[0] < 1.0f) {
        out[x] = 0;
        continue;
      }
      /* Anti-ringing: each channel stays within the nearest 2x2 source
       * pixels, so the kernel's negative lobes cannot invent colors (dark
       * rims, or off-hue fringes on isolated layer detail). */
      const size_t range =
          range_row + (size_t)ClampIndex(
                          x / scale + hd->tap_offset[x % scale] + 1,
                          hd->width);
      const uint32_t high = range_high[range];
      if (!high) {
        out[x] = 0;
        continue;
      }
      const uint32_t low = range_low[range];
      const float alpha = sum[0] > 255.0f ? 255.0f : sum[0];
      const float unpremultiply = 255.0f / sum[0];
      uint32_t pixel = (uint32_t)(alpha + 0.5f) << 24;
      for (int c = 0; c < 3; c++) {
        const int shift = 16 - 8 * c;
        const float lowest = (float)((low >> shift) & 0xffu);
        const float highest = (float)((high >> shift) & 0xffu);
        float value = sum[c + 1] * unpremultiply;
        value = value < lowest ? lowest : value > highest ? highest : value;
        pixel |= (uint32_t)(value + 0.5f) << shift;
      }
      out[x] = pixel;
    }
  }
}

static void Upscale(Dkc2Hd *hd, const uint32_t *source,
                    uint32_t *destination) {
  if (hd->background == kDkc2HdFilterSmooth)
    UpscaleSmooth(hd, source, destination);
  else
    UpscaleNearest(hd, source, destination);
}

static TerrainEntry *TerrainFind(Dkc2Hd *hd, uint64_t key) {
  unsigned slot = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 51) %
                  kTerrainSlots;
  for (unsigned probe = 0; probe < kTerrainSlots; probe++) {
    TerrainEntry *entry = &hd->terrain[(slot + probe) % kTerrainSlots];
    if (entry->key == key)
      return entry;
    if (!entry->key) {
      /* Keep a quarter free so probes stay short; past that nothing new is
       * remembered. */
      if (hd->terrain_count >= kTerrainSlots * 3u / 4u)
        return NULL;
      entry->key = key;
      hd->terrain_count++;
      return entry;
    }
  }
  return NULL;
}

static const HdImage *TerrainArt(Dkc2Hd *hd, TerrainEntry *entry) {
  if (entry->state == kPackUnknown) {
    char path[kPathMax + 32];
    snprintf(path, sizeof path, "%s/t_%016llx.rgba", hd->pack_dir,
             (unsigned long long)entry->key);
    entry->image = LoadImage(path);
    entry->state = entry->image ? kPackLoaded : kPackMissing;
  }
  return entry->image;
}

/* Writes captured pixels (zero is transparent) as a pack image. */
static bool WriteCaptureImage(const char *path, const uint32_t *pixels,
                              int width, int height, size_t pitch) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return false;
  const uint8_t header[8] = {'D',
                             'K',
                             'H',
                             'D',
                             (uint8_t)width,
                             (uint8_t)(width >> 8),
                             (uint8_t)height,
                             (uint8_t)(height >> 8)};
  bool ok = fwrite(header, 1, sizeof header, file) == sizeof header;
  for (int y = 0; ok && y < height; y++) {
    for (int x = 0; ok && x < width; x++) {
      const uint32_t pixel = pixels[(size_t)y * pitch + (size_t)x];
      const uint8_t rgba[4] = {(uint8_t)(pixel >> 16), (uint8_t)(pixel >> 8),
                               (uint8_t)pixel, pixel ? 255 : 0};
      ok = fwrite(rgba, 1, sizeof rgba, file) == sizeof rgba;
    }
  }
  return fclose(file) == 0 && ok;
}

/* Decodes the block at frame position (x, y) into hd->block and reports
 * whether it reproduces the captured BG1 there. */
static bool TerrainBlockMatches(Dkc2Hd *hd, const Ppu *ppu,
                                const Dkc2NativeTerrainView *view,
                                uint32_t tile_x, uint32_t tile_y, int x,
                                int y) {
  Dkc2NativeTerrainDecodeBlock(ppu, view, tile_x, tile_y, hd->block);
  size_t compared = 0, wrong = 0;
  for (int by = 0; by < kBlock; by++) {
    const int fy = y + by;
    if (fy < 0 || fy >= kHeight)
      continue;
    for (int bx = 0; bx < kBlock; bx++) {
      const int fx = x + bx;
      if (fx < 0 || fx >= hd->width)
        continue;
      const uint32_t decoded = hd->block[by * kBlock + bx];
      const uint32_t captured = hd->bg[0][(size_t)fy * hd->width + fx];
      if (!decoded && !captured)
        continue;
      compared++;
      wrong += decoded != captured;
    }
  }
  return compared > 0 && wrong * 8 <= compared;
}

/* Draws art over the frame area (x, y, width, height) of the BG1 layer. */
static void DrawLayerArt(Dkc2Hd *hd, const HdImage *image, int x, int y,
                         int width, int height, unsigned brightness) {
  const int scale = hd->scale;
  const int size_x = width * scale, size_y = height * scale;
  const int out_width = hd->width * scale, out_height = kHeight * scale;
  for (int hy = 0; hy < size_y; hy++) {
    const int oy = y * scale + hy;
    if (oy < 0 || oy >= out_height)
      continue;
    const uint32_t *source =
        image->pixels + (size_t)(hy * image->height / size_y) * image->width;
    uint32_t *row = hd->layer + (size_t)oy * (size_t)out_width;
    for (int hx = 0; hx < size_x; hx++) {
      const int ox = x * scale + hx;
      if (ox < 0 || ox >= out_width)
        continue;
      uint32_t pixel = source[hx * image->width / size_x];
      if (brightness != 255u) {
        uint32_t scaled = pixel & 0xff000000u;
        for (int shift = 0; shift < 24; shift += 8)
          scaled |= (((pixel >> shift) & 0xff) * brightness / 255u) << shift;
        pixel = scaled;
      }
      row[ox] = pixel;
    }
  }
}

/* Visits every terrain block overlapping the frame, margins included: art
 * replaces validated blocks in the BG1 layer, and the dump records new
 * blocks with the scene they appeared in. */
static void ProcessTerrain(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram) {
  Dkc2NativeTerrainView view;
  if (!Dkc2NativeTerrainBegin(ppu, ram, &view))
    return;
  const bool dump = hd->dump_dir[0] && ppu->brightnessMult[31] == 255u;
  const unsigned brightness = ppu->brightnessMult[31];
  const int64_t left = (int64_t)view.map_x - hd->origin;
  const uint32_t first_x = left < 0 ? 0 : (uint32_t)(left >> 3) & ~3u;
  const uint32_t first_y = view.first_tile_y & ~3u;
  char scene[4096];
  size_t scene_length = 0;
  bool scene_new = false;
  for (uint32_t by = 0; by < kHeight / kBlock + 2u; by++) {
    for (uint32_t bx = 0; bx < (uint32_t)hd->width / kBlock + 2u; bx++) {
      const uint32_t tile_x = first_x + bx * 4u;
      const uint32_t tile_y = (first_y + by * 4u) & (0x2000u - 1u);
      int x = 0, y = 0;
      Dkc2NativeTerrainTileScreen(&view, tile_x, tile_y, &x, &y);
      x += hd->origin;
      if (x >= hd->width || y >= kHeight || x + kBlock <= 0 || y + kBlock <= 0)
        continue;
      const uint64_t key =
          Dkc2NativeTerrainBlockKey(ppu, &view, tile_x, tile_y);
      if (!key || !TerrainBlockMatches(hd, ppu, &view, tile_x, tile_y, x, y))
        continue;
      TerrainEntry *entry = TerrainFind(hd, key);
      if (!entry)
        continue;
      if (hd->layer_frame) {
        const HdImage *image = TerrainArt(hd, entry);
        if (image)
          DrawLayerArt(hd, image, x, y, kBlock, kBlock, brightness);
      }
      if (!dump)
        continue;
      if (!entry->dumped) {
        char path[kPathMax + 32];
        snprintf(path, sizeof path, "%s/t_%016llx.rgba", hd->dump_dir,
                 (unsigned long long)key);
        entry->dumped =
            WriteCaptureImage(path, hd->block, kBlock, kBlock, kBlock);
        scene_new |= entry->dumped;
      }
      const int written = snprintf(
          scene + scene_length, sizeof scene - scene_length, " %u,%u,%016llx",
          (unsigned)(tile_x / 4u), (unsigned)(tile_y / 4u),
          (unsigned long long)key);
      if (written > 0 && (size_t)written < sizeof scene - scene_length)
        scene_length += (size_t)written;
    }
  }
  if (!scene_new)
    return;
  /* A scene line lists every matched block of a frame that showed new ones,
   * so generated art sees blocks beside their neighbours. */
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/scenes.txt", hd->dump_dir);
  FILE *scenes = fopen(path, "a");
  bool ok = scenes && fprintf(scenes, "%.*s\n", (int)scene_length, scene) >= 0;
  if (scenes && fclose(scenes) != 0)
    ok = false;
  if (!ok)
    fprintf(stderr, "warning: DKC2_HD_DUMP could not update %s\n", path);
}

/* Whether the native 256 columns lie inside the frame (the presentation
 * bias can shift them). */
static bool NativeAreaInFrame(const Dkc2Hd *hd) {
  return hd->origin >= 0 && hd->origin + kNativeWidth <= hd->width;
}

/* The pack screen whose reference the native BG1 reproduces, if any. */
static const ScreenEntry *MatchScreen(const Dkc2Hd *hd) {
  /* Mismatches past this rule a screen out whatever the rest compares. */
  const size_t limit = (size_t)kNativeWidth * kHeight / 8u;
  if (!NativeAreaInFrame(hd))
    return NULL;
  for (unsigned s = 0; s < hd->screen_count; s++) {
    const uint32_t *reference = hd->screens[s].reference->pixels;
    size_t compared = 0, wrong = 0;
    for (int y = 0; y < kHeight && wrong <= limit; y++) {
      const uint32_t *captured =
          hd->bg[0] + (size_t)y * (size_t)hd->width + (size_t)hd->origin;
      for (int x = 0; x < kNativeWidth; x++) {
        const uint32_t expected = reference[y * kNativeWidth + x];
        const uint32_t pixel = captured[x];
        /* References are straight RGBA: opaque or fully clear. */
        if (!(expected >> 24) && !pixel)
          continue;
        compared++;
        wrong += !(expected >> 24) || !pixel ||
                 Rgb(expected) != Rgb(pixel);
      }
    }
    if (compared > 0 && wrong * 8 <= compared)
      return &hd->screens[s];
  }
  return NULL;
}

/* Screen art replaces the whole native area of BG1: its clear pixels take
 * the lowest BG1 rank so the art also covers the backdrop, below any layer
 * or object that was in front of it. */
static void DrawScreenArt(Dkc2Hd *hd, const ScreenEntry *screen,
                          unsigned brightness) {
  DrawLayerArt(hd, screen->art, hd->origin, 0, kNativeWidth, kHeight,
               brightness);
  for (int y = 0; y < kHeight; y++) {
    uint8_t *rank = hd->bg1_rank + (size_t)y * (size_t)hd->width +
                    (size_t)hd->origin;
    for (int x = 0; x < kNativeWidth; x++)
      if (!rank[x])
        rank[x] = 1;
  }
}

/* Records the native BG1 of still screens outside levels (title, menus) as
 * references for screen art: once it has stayed unchanged for a second at
 * full brightness. */
static void DumpScreen(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram) {
  Dkc2NativeTerrainView view;
  if (!NativeAreaInFrame(hd) || ppu->brightnessMult[31] != 255u ||
      Dkc2NativeTerrainBegin(ppu, ram, &view)) {
    hd->screen_stable = 0;
    return;
  }
  uint64_t hash = 14695981039346656037ull;
  bool any = false;
  for (int y = 0; y < kHeight; y++) {
    const uint32_t *captured =
        hd->bg[0] + (size_t)y * (size_t)hd->width + (size_t)hd->origin;
    for (int x = 0; x < kNativeWidth; x++) {
      const uint32_t pixel = captured[x] ? 0xff000000u | Rgb(captured[x]) : 0;
      any |= pixel != 0;
      hash = (hash ^ pixel) * 1099511628211ull;
    }
  }
  if (!any || hash != hd->screen_hash) {
    hd->screen_hash = hash;
    hd->screen_stable = 0;
    return;
  }
  if (++hd->screen_stable != kScreenStableFrames || hash == hd->screen_dumped)
    return;
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/s_%016llx.ref", hd->dump_dir,
           (unsigned long long)hash);
  if (WriteCaptureImage(path, hd->bg[0] + hd->origin, kNativeWidth, kHeight,
                        (size_t)hd->width))
    hd->screen_dumped = hash;
  else
    fprintf(stderr, "warning: DKC2_HD_DUMP could not write %s\n", path);
}

/* Composes the HD BG1 layer over the output where BG1 was in front of what
 * the base frame shows. */
static void CompositeTerrain(Dkc2Hd *hd) {
  const int scale = hd->scale;
  const int out_width = hd->width * scale;
  for (int y = 0; y < kHeight * scale; y++) {
    const size_t line = (size_t)(y / scale) * (size_t)hd->width;
    uint32_t *out = hd->output + (size_t)y * (size_t)out_width;
    const uint32_t *layer = hd->layer + (size_t)y * (size_t)out_width;
    for (int x = 0; x < out_width; x++) {
      const unsigned alpha = layer[x] >> 24;
      const size_t i = line + (size_t)(x / scale);
      if (alpha && hd->bg1_rank[i] > hd->base_rank[i])
        out[x] = Blend(out[x], layer[x], alpha);
    }
  }
}

/* BG1 priority per pixel, carried one pixel past the layer's edge so the
 * HD layer's soft edges keep the priority of the terrain they belong to. */
static void BuildBg1Rank(Dkc2Hd *hd) {
  const int width = hd->width;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < width; x++) {
      const size_t i = (size_t)y * (size_t)width + (size_t)x;
      uint8_t rank = (uint8_t)(hd->bg[0][i] >> 24);
      if (!hd->bg[0][i]) {
        static const int kNeighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int n = 0; n < 4; n++) {
          const int nx = x + kNeighbours[n][0], ny = y + kNeighbours[n][1];
          if (nx < 0 || nx >= width || ny < 0 || ny >= kHeight)
            continue;
          const uint32_t neighbour = hd->bg[0][(size_t)ny * width + nx];
          if (neighbour && (uint8_t)(neighbour >> 24) > rank)
            rank = (uint8_t)(neighbour >> 24);
        }
      }
      hd->bg1_rank[i] = rank;
    }
  }
}

const uint32_t *Dkc2HdEndFrame(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram,
                               const uint8_t *frame, size_t frame_pitch,
                               int *width, int *height) {
  if (!hd || !hd->active || !ppu || !ram || !frame ||
      frame_pitch < (size_t)hd->width * 4u)
    return NULL;
  hd->origin = (hd->width - kNativeWidth) / 2 - ppu->wsPresentationXBias;
  /* The displayed OAM was built from the previous game update. */
  PrepareSprites(hd, ppu, hd->has_previous ? hd->previous_wram : ram);
  memcpy(hd->previous_wram, ram, sizeof hd->previous_wram);
  hd->has_previous = true;

  /* With terrain art BG1 is not in the frame: objects are restored by rank
   * against BG2/BG3 and the HD BG1 layer is drawn by priority afterwards. */
  const bool layered = hd->layer_frame;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < hd->width; x++) {
      const size_t i = (size_t)y * (size_t)hd->width + (size_t)x;
      const uint8_t *bgra = frame + (size_t)y * frame_pitch + (size_t)x * 4u;
      uint32_t pixel = 0xff000000u | (uint32_t)bgra[2] << 16 |
                       (uint32_t)bgra[1] << 8 | bgra[0];
      uint8_t rank = 0;
      for (unsigned layer = layered ? 1u : 0u; layer < kBgLayers; layer++) {
        const uint8_t layer_rank = (uint8_t)(hd->bg[layer][i] >> 24);
        if (layer_rank > rank)
          rank = layer_rank;
      }
      const uint32_t object = hd->obj[i];
      if (object && !ReplacedPixel(hd, i) && (uint8_t)(object >> 24) > rank) {
        pixel = 0xff000000u | Rgb(object);
        rank = (uint8_t)(object >> 24);
      }
      /* The HD BG1 layer is composed over this base by rank alone, but the
       * base still shows BG1 where it is on top: the layer's soft edges then
       * fade into BG1 itself instead of into the layers it hides. */
      if (layered && hd->bg[0][i] && (uint8_t)(hd->bg[0][i] >> 24) > rank)
        pixel = 0xff000000u | Rgb(hd->bg[0][i]);
      hd->base[i] = pixel;
      hd->base_rank[i] = rank;
    }
  }
  Upscale(hd, hd->base, hd->output);
  const ScreenEntry *screen = layered ? MatchScreen(hd) : NULL;
  if (layered) {
    Upscale(hd, hd->bg[0], hd->layer);
    BuildBg1Rank(hd);
  }
  if (screen)
    DrawScreenArt(hd, screen, ppu->brightnessMult[31]);
  else if (layered || hd->dump_dir[0])
    ProcessTerrain(hd, ppu, ram);
  if (hd->dump_dir[0])
    DumpScreen(hd, ppu, ram);
  if (layered)
    CompositeTerrain(hd);
  for (unsigned k = 0; k < hd->sprite_count; k++) {
    if (!hd->sprites[k].replaced)
      continue;
    DumpSprite(hd, ppu, &hd->sprites[k]);
    DrawSprite(hd, ppu, k);
  }
  *width = hd->width * hd->scale;
  *height = kHeight * hd->scale;
  return hd->output;
}
