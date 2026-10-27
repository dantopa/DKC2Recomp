#include "dkc2_hd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>  /* _findfirst/_findnext: MSVC has no <dirent.h> */
#else
#include <dirent.h>
#endif

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
  /* Distinct sprite frame and palette pairs remembered (art and dump
   * state). */
  kSpriteArtSlots = 8192,
  /* Full-screen art (title and other still screens). */
  kScreenSlots = 16,
  /* Frames BG1 must stay unchanged before the dump records it as a screen. */
  kScreenStableFrames = 60,
  /* Art quads of one frame: every terrain block the frame can overlap, or a
   * screen. */
  kArtMax = (kHeight / kBlock + 2) * (kMaxWidth / kBlock + 2) + 1,
  /* Learned filter table (see dkc2_hd.h): four patterns of four rank
   * windows, each 4-bit, per output subpixel five int8 logits. */
  kLearnedScale = 4,
  kLearnedPatterns = 4,
  kLearnedWindows = 1 << 16,
  kLearnedOutputs = 5,
  kLearnedRow = kLearnedScale * kLearnedScale * kLearnedOutputs,
  kLearnedHeader = 8,
  /* Sprite quads: every sprite-table record and every OAM entry as its own
   * loose group. */
  kQuadMax = kDkc2NativeSpriteMax + kDkc2NativeOamEntries,
  /* Character type the dump gives loose objects in index.txt. */
  kLooseType = 0x0ffe,
  /* Backgrounds with tile art: BG2 and BG3 (BG1 has terrain blocks). */
  kTileFirstLayer = 1,
  kTile = 8,
  kTilePixels = kTile * kTile,
  kTileColumns = kMaxWidth / kTile + 2,
  kTileRows = kHeight / kTile + 2,
  /* Tile quads of one frame: every cell of each tiled layer. */
  kTileMax = (kBgLayers - kTileFirstLayer) * kTileColumns * kTileRows,
  /* Distinct background tiles remembered (art and dump state). */
  kTileArtSlots = 16384,
  /* Verified cells of one layer, looked up by their captured pixels. */
  kTileSeenSlots = 4096,
  /* Background panoramas (see dkc2_hd.h): pack entries, the largest
   * tilemap in cells per side, and the reproduced reference cells a frame
   * must show before its matches count. */
  kPanoramaSlots = 8,
  kMapSide = 64,
  kPanoramaMinCells = 16,
};

/* Pixels each learned pattern reads, as (row, column) offsets from the top
 * left of a cell of four frame pixels; the first pattern is the cell. */
static const int kLearnedPattern[kLearnedPatterns][4][2] = {
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
    {{-1, 0}, {-1, 1}, {2, 0}, {2, 1}},
    {{0, -1}, {1, -1}, {0, 2}, {1, 2}},
    {{-1, -1}, {-1, 2}, {2, -1}, {2, 2}},
};
/* Table logits are stored as round(logit * scale). */
static const float kLearnedLogitScale = 15.875f;

typedef Dkc2HdImage HdImage;

typedef enum PackState {
  kPackUnknown = 0,
  kPackLoaded,
  kPackMissing,
} PackState;

typedef struct CacheEntry {
  uint64_t key;
  HdImage *image;
} CacheEntry;

/* Pack art and dump state of one terrain block or sprite frame. */
typedef struct PackEntry {
  uint64_t key;  /* 0: free slot */
  HdImage *image;
  uint8_t state;  /* PackState */
  bool dumped;
} PackEntry;

typedef struct ScreenEntry {
  uint64_t key;
  HdImage *reference;  /* native BG1 the art was made for */
  HdImage *art;
} ScreenEntry;

/* Art for a whole background tilemap and the cells it was made for. */
typedef struct PanoramaEntry {
  uint64_t key;
  unsigned layer;
  HdImage *art;
  /* Character key (0: not referenced) and tilemap flips per cell, row by
   * row over kMapSide columns. */
  uint64_t cells[kMapSide * kMapSide];
  uint8_t flips[kMapSide * kMapSide];
  /* This frame: referenced cells reproduced, and those that differ. */
  unsigned compared, wrong;
} PanoramaEntry;

/* An OAM entry (frame columns) and what the HD path makes of it. */
typedef struct LooseEntry {
  Dkc2NativeOamEntry oam;
  uint8_t rank;
  bool record;  /* drawn as part of a sprite-table record */
  /* Pixels where it is the top-most entry, and those the PPU shows
   * otherwise. */
  uint16_t compared, wrong;
  int16_t group;  /* loose group, or -1: it stays in the base */
} LooseEntry;

/* Touching verified loose entries of one rank, drawn as one quad. */
typedef struct LooseGroup {
  int left, top, right, bottom;  /* frame bounds, right and bottom past */
  uint8_t rank;
  uint8_t front;     /* its lowest OAM slot */
  uint8_t position;  /* in the draw order */
} LooseGroup;

/* A sprite-table record or a loose group, in back-to-front draw order. */
typedef struct DrawItem {
  bool loose;
  uint8_t item;
} DrawItem;

/* A background cell VRAM reproduced, keyed by its captured pixels. */
typedef struct TileSeen {
  uint64_t content;  /* 0: free slot */
  uint64_t key;
  uint16_t entry;
} TileSeen;

/* A whole cell VRAM did not reproduce (widescreen margins), left for the
 * cells that were. */
typedef struct TilePending {
  int x, y;
} TilePending;

typedef enum QuadKind {
  kQuadArt,     /* replaces the BG1 layer's pixels */
  kQuadSprite,  /* blends where the occlusion map does not hide it */
  kQuadTile,    /* blends where the tile mask shows its layer */
} QuadKind;

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
  int8_t *learned;  /* the learned filter's table, when that filter is on */
  /* Smooth background: first source tap and four Catmull-Rom weights for
   * each output phase of a source pixel. */
  int tap_offset[kDkc2HdMaxScale];
  float tap_weight[kDkc2HdMaxScale][4];
  char pack_dir[kPathMax];
  /* Name hash of every file in the pack directory, sorted: art is opened
   * only when listed, so keys without art never reach the file system. */
  uint64_t *pack_files;
  size_t pack_file_count;
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
  /* Sprite art by graphic and palette content (see SpriteArtKey). */
  PackEntry sprite_art[kSpriteArtSlots];
  unsigned sprite_art_count;
  CacheEntry cache[kCacheSlots];
  unsigned cache_count;
  uint32_t base[kMaxPixels];
  /* BG1 art: when the pack lists terrain blocks or screens, BG1 leaves the
   * rendered frame and is composed here, art where the pack has it and the
   * background filter elsewhere. */
  bool layer_pack;
  /* BG2/BG3 cells are reproduced: the pack has backgrounds.txt or a dump
   * collects them. */
  bool tile_pass;
  /* This frame composes BG1 here: the pack has BG1 art, BG1 is drawn on
   * the main screen only (captures see the main screen; scenes such as
   * Mainbrace Mayhem draw BG1 on the subscreen through color math) and
   * color math does not blend it with the subscreen (captures are taken
   * before it; the game select menu dims the unselected rows by halving BG1
   * with BG2, the map adds BG2 and BG3 to it). */
  bool layer_frame;
  ScreenEntry screens[kScreenSlots];
  unsigned screen_count;
  PanoramaEntry panoramas[kPanoramaSlots];
  unsigned panorama_count;
  /* Dump: hash of the native BG1, frames it has been unchanged, and the
   * last one written. */
  uint64_t screen_hash;
  unsigned screen_stable;
  uint64_t screen_dumped;
  PackEntry terrain[kTerrainSlots];
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
  /* The scene of the last frame (see Dkc2HdScene). */
  uint32_t occlusion[kMaxPixels];
  uint32_t layer_source[kMaxPixels];
  Dkc2HdQuad art[kArtMax];
  unsigned art_count;
  /* Loose objects (OAM entries outside the sprite table, see
   * PrepareLoose). */
  LooseEntry oam[kDkc2NativeOamEntries];
  uint8_t oam_top[kMaxPixels];      /* top-most OAM slot + 1 */
  uint8_t loose_owner[kMaxPixels];  /* claiming loose group + 1 */
  LooseGroup loose[kDkc2NativeOamEntries];
  unsigned loose_count;
  uint16_t loose_raw[kDkc2NativeFrameMax * kDkc2NativeFrameMax];
  uint8_t record_front[kDkc2NativeSpriteMax];  /* lowest OAM slot, or 0xff */
  uint8_t record_position[kDkc2NativeSpriteMax];
  DrawItem order[kQuadMax];
  unsigned order_count;
  Dkc2HdQuad quads[kQuadMax];
  /* Per quad, for Dkc2HdProbePixel: pack file name and whether it is art. */
  char quad_name[kQuadMax][24];
  bool quad_art[kQuadMax];
  unsigned quad_count;
  /* Background tile art (see ProcessTiles). */
  PackEntry tile_art[kTileArtSlots];
  unsigned tile_art_count;
  TileSeen tile_seen[kTileSeenSlots];
  TilePending tile_pending[kTileColumns * kTileRows];
  Dkc2HdQuad tiles[kTileMax];
  unsigned tile_count;
  /* With DKC2_HD_DUMP, the frame's VRAM cells as " layer,x,y,key,flips"
   * (x, y: tilemap cell), written to tiles.txt when the frame dumped a new
   * character, so generated art can see a whole layer. */
  char tile_layout[kTileMax * 32];
  size_t tile_layout_length;
  bool tile_layout_new;
  uint8_t tile_mask[kMaxPixels];
  Dkc2HdScene scene;
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
  } else if (strcmp(filter, "learned") == 0) {
    config->filter = kDkc2HdFilterLearned;
  } else if (strcmp(filter, "learned-pixel") == 0) {
    config->filter = kDkc2HdFilterLearnedPixel;
  } else {
    *error = "DKC2_HD_FILTER must be nearest, smooth, learned or learned-pixel";
    return false;
  }
  config->scale = (int)scale;
  config->background = kDkc2HdFilterSmooth;
  config->pack_dir = getenv("DKC2_HD_PACK");
  config->dump_dir = getenv("DKC2_HD_DUMP");
  const char *table = getenv("DKC2_HD_FILTER_TABLE");
  config->filter_table = table && *table ? table : NULL;
  return true;
}

/* Reads a learned filter table; NULL when it is missing or malformed. */
static int8_t *LoadLearnedTable(const char *path) {
  const size_t size =
      (size_t)kLearnedPatterns * kLearnedWindows * kLearnedRow;
  FILE *file = path ? fopen(path, "rb") : NULL;
  if (!file)
    return NULL;
  uint8_t header[kLearnedHeader];
  int8_t *table = (int8_t *)malloc(size);
  bool ok = table && fread(header, 1, sizeof header, file) == sizeof header &&
            memcmp(header, "DKLT", 4) == 0 && header[4] == 1 &&
            header[5] == kLearnedScale && header[6] == kLearnedPatterns &&
            fread(table, 1, size, file) == size && fgetc(file) == EOF;
  fclose(file);
  if (!ok) {
    free(table);
    return NULL;
  }
  return table;
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
static bool ListPack(Dkc2Hd *hd);

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

/* Reads panoramas.txt: one 16-digit hex key per line, each with reference
 * cells "l_<key>.ref" (tiles.txt items of one layer) and art
 * "l_<key>.rgba". */
static void LoadPanoramas(Dkc2Hd *hd) {
  char path[kPathMax + 64];
  snprintf(path, sizeof path, "%s/panoramas.txt", hd->pack_dir);
  FILE *index = fopen(path, "r");
  if (!index)
    return;
  unsigned long long key = 0;
  while (hd->panorama_count < kPanoramaSlots &&
         fscanf(index, "%16llx", &key) == 1) {
    PanoramaEntry *panorama = &hd->panoramas[hd->panorama_count];
    memset(panorama, 0, sizeof *panorama);
    snprintf(path, sizeof path, "%s/l_%016llx.ref", hd->pack_dir, key);
    FILE *reference = fopen(path, "r");
    unsigned layer = 0, x = 0, y = 0, flips = 0, cells = 0;
    unsigned long long cell = 0;
    bool valid = reference != NULL;
    while (valid && fscanf(reference, " %u,%u,%u,%16llx,%u", &layer, &x, &y,
                           &cell, &flips) == 5) {
      valid = layer >= kTileFirstLayer && layer < kBgLayers &&
              x < kMapSide && y < kMapSide && cell != 0 &&
              (cells == 0 || layer == panorama->layer);
      panorama->layer = layer;
      panorama->cells[y * kMapSide + x] = cell;
      panorama->flips[y * kMapSide + x] = (uint8_t)(flips & 3u);
      cells++;
    }
    valid = valid && feof(reference) && cells >= kPanoramaMinCells;
    if (reference)
      fclose(reference);
    snprintf(path, sizeof path, "%s/l_%016llx.rgba", hd->pack_dir, key);
    panorama->art = valid ? LoadImage(path) : NULL;
    if (!panorama->art) {
      fprintf(stderr, "warning: DKC2_HD_PACK panorama %016llx is incomplete\n",
              key);
      continue;
    }
    panorama->key = key;
    hd->panorama_count++;
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
  if (hd->filter == kDkc2HdFilterLearned ||
      hd->filter == kDkc2HdFilterLearnedPixel) {
    const char *table = config->filter_table ? config->filter_table
                        : hd->filter == kDkc2HdFilterLearnedPixel
                            ? kDkc2HdPixelFilterTable
                            : kDkc2HdDefaultFilterTable;
    hd->learned = LoadLearnedTable(table);
    if (!hd->learned) {
      fprintf(stderr,
              "warning: HD sprite filter table %s is missing or invalid; "
              "using the smooth filter\n",
              table);
      hd->filter = kDkc2HdFilterSmooth;
    }
  }
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
    if (!ListPack(hd)) {
      Dkc2HdDestroy(hd);
      return NULL;
    }
    char path[kPathMax + 32];
    snprintf(path, sizeof path, "%s/terrain.txt", hd->pack_dir);
    FILE *index = fopen(path, "r");
    hd->layer_pack = index != NULL;
    if (index)
      fclose(index);
    LoadScreens(hd);
    hd->layer_pack |= hd->screen_count > 0;
    snprintf(path, sizeof path, "%s/backgrounds.txt", hd->pack_dir);
    index = fopen(path, "r");
    hd->tile_pass = index != NULL;
    if (index)
      fclose(index);
    LoadPanoramas(hd);
    hd->tile_pass |= hd->panorama_count > 0;
  }
  hd->tile_pass |= hd->dump_dir[0] != '\0';
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
  for (unsigned i = 0; i < kSpriteArtSlots; i++)
    free(hd->sprite_art[i].image);
  for (unsigned i = 0; i < kTileArtSlots; i++)
    free(hd->tile_art[i].image);
  for (unsigned i = 0; i < hd->screen_count; i++) {
    free(hd->screens[i].reference);
    free(hd->screens[i].art);
  }
  for (unsigned i = 0; i < hd->panorama_count; i++)
    free(hd->panoramas[i].art);
  free(hd->layer);
  free(hd->rows);
  free(hd->pack_files);
  free(hd->learned);
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
  /* Color math changes BG1 across the screen when the subscreen holds a
   * background or the fixed color is not black. Subscreen objects alone
   * only change it under themselves (the title's sparkles), which is not
   * reproduced. */
  const bool bg1_math = (PPU_mathEnabled(ppu) & 1u) &&
                        ((ppu->screenEnabled[1] & 0xfu) ||
                         (ppu->fixedColor & 0x7fffu));
  hd->layer_frame = hd->active && hd->layer_pack &&
                      (ppu->screenEnabled[0] & 1u) &&
                      !(ppu->screenEnabled[1] & 1u) && !bg1_math;
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
  static uint64_t last_id;
  HdImage *image = (HdImage *)malloc(
      sizeof(HdImage) + sizeof(uint32_t) * (size_t)width * (size_t)height);
  if (image) {
    image->id = ++last_id;
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

static uint64_t NameHash(const char *name) {
  uint64_t hash = 14695981039346656037ull;
  for (; *name; name++)
    hash = (hash ^ (unsigned char)*name) * 1099511628211ull;
  return hash;
}

static int CompareHash(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static bool AddPackFile(Dkc2Hd *hd, size_t *capacity, const char *name) {
  if (hd->pack_file_count == *capacity) {
    const size_t grown = *capacity ? *capacity * 2 : 1024;
    uint64_t *files =
        (uint64_t *)realloc(hd->pack_files, grown * sizeof *files);
    if (!files)
      return false;
    hd->pack_files = files;
    *capacity = grown;
  }
  hd->pack_files[hd->pack_file_count++] = NameHash(name);
  return true;
}

/* Lists the pack directory once. A missing directory lists nothing, which
 * loads no art, as opening each file would. False when out of memory. */
static bool ListPack(Dkc2Hd *hd) {
  size_t capacity = 0;
  bool ok = true;
#ifdef _WIN32
  char pattern[kPathMax + 4];
  snprintf(pattern, sizeof pattern, "%s/*", hd->pack_dir);
  struct _finddata_t entry;
  const intptr_t find = _findfirst(pattern, &entry);
  if (find != -1) {
    do
      ok = AddPackFile(hd, &capacity, entry.name);
    while (ok && _findnext(find, &entry) == 0);
    _findclose(find);
  }
#else
  DIR *directory = opendir(hd->pack_dir);
  if (directory) {
    const struct dirent *entry;
    while (ok && (entry = readdir(directory)) != NULL)
      ok = AddPackFile(hd, &capacity, entry->d_name);
    closedir(directory);
  }
#endif
  if (hd->pack_file_count)
    qsort(hd->pack_files, hd->pack_file_count, sizeof *hd->pack_files,
          CompareHash);
  return ok;
}

/* Pack file `name`, read only when the pack listing has it. */
static HdImage *LoadPackImage(const Dkc2Hd *hd, const char *name) {
  const uint64_t hash = NameHash(name);
  if (!hd->pack_file_count ||
      !bsearch(&hash, hd->pack_files, hd->pack_file_count,
               sizeof *hd->pack_files, CompareHash))
    return NULL;
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/%s", hd->pack_dir, name);
  return LoadImage(path);
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

/* The entry for key (non-zero) in an open-addressed table of slots entries,
 * added when new. */
static PackEntry *PackFind(PackEntry *table, unsigned slots, unsigned *count,
                           uint64_t key) {
  const unsigned slot = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 51) % slots;
  for (unsigned probe = 0; probe < slots; probe++) {
    PackEntry *entry = &table[(slot + probe) % slots];
    if (entry->key == key)
      return entry;
    if (!entry->key) {
      /* Keep a quarter free so probes stay short; past that nothing new is
       * remembered. */
      if (*count >= slots * 3u / 4u)
        return NULL;
      entry->key = key;
      (*count)++;
      return entry;
    }
  }
  return NULL;
}

/* A sprite record's graphic and the content of its palette (colors 1-15;
 * brightness is applied apart), so art follows a palette into whichever
 * slot the game assigns it. Graphic 0 is never collected, so the key is
 * never 0. */
static uint64_t SpriteArtKey(const Ppu *ppu, const SpriteState *state) {
  const unsigned palette = (state->sprite.properties >> 9) & 7u;
  uint32_t hash = 2166136261u;
  for (unsigned i = 1; i < 16; i++) {
    hash ^= ppu->cgram[0x80u + palette * 16u + i] & 0x7fffu;
    hash *= 16777619u;
  }
  return (uint64_t)hash << 16 | state->sprite.graphic_id;
}

/* Pack art for a sprite record: "<graphic>_p<palette content>.rgba", else
 * "<graphic>_<palette slot>.rgba". */
static const HdImage *PackImage(Dkc2Hd *hd, const Ppu *ppu,
                                const SpriteState *state) {
  if (!hd->pack_dir[0])
    return NULL;
  const uint64_t key = SpriteArtKey(ppu, state);
  PackEntry *entry =
      PackFind(hd->sprite_art, kSpriteArtSlots, &hd->sprite_art_count, key);
  if (entry && entry->state == kPackUnknown) {
    char name[32];
    snprintf(name, sizeof name, "%04x_p%08x.rgba", state->sprite.graphic_id,
             (unsigned)(key >> 16));
    entry->image = LoadPackImage(hd, name);
    entry->state = entry->image ? kPackLoaded : kPackMissing;
  }
  if (entry && entry->image)
    return entry->image;
  const unsigned slot = state->sprite.graphic_id / 4u;
  const unsigned palette = (state->sprite.properties >> 9) & 7u;
  if (hd->pack_state[slot][palette] == kPackUnknown) {
    char name[32];
    snprintf(name, sizeof name, "%04x_%x.rgba", slot * 4u, palette);
    hd->pack[slot][palette] = LoadPackImage(hd, name);
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

/* The decoded frame itself, opaque where it has color. */
static HdImage *FrameImage(const Dkc2NativeSpriteFrame *frame) {
  HdImage *image = NewImage(frame->width, frame->height);
  if (!image)
    return NULL;
  for (int i = 0; i < frame->width * frame->height; i++)
    image->pixels[i] =
        frame->pixels[i] ? 0xff000000u | Rgb(frame->pixels[i]) : 0;
  return image;
}

/* The learned filter (see dkc2_hd.h) at its table's scale, premultiplied
 * inside, straight alpha out. NULL when the frame has more than 15 colors
 * (it then takes the smooth filter). */
static HdImage *LearnedImage(const int8_t *table,
                             const Dkc2NativeSpriteFrame *frame) {
  const int width = frame->width, height = frame->height;
  const int pixel_count = width * height;
  /* Luminance rank of each color, 1-15 (0: transparent), ties in RGB
   * order. */
  uint32_t colors[15];
  int color_count = 0;
  for (int i = 0; i < pixel_count; i++) {
    const uint32_t color = Rgb(frame->pixels[i]);
    if (!frame->pixels[i])
      continue;
    int k = 0;
    while (k < color_count && colors[k] != color)
      k++;
    if (k < color_count)
      continue;
    if (color_count == 15)
      return NULL;
    colors[color_count++] = color;
  }
  uint64_t order[15];
  for (int k = 0; k < color_count; k++) {
    const uint32_t c = colors[k];
    const uint64_t luma = 299u * (c >> 16) + 587u * ((c >> 8) & 0xff) +
                          114u * (c & 0xff);
    order[k] = luma << 24 | c;
  }
  for (int k = 1; k < color_count; k++)
    for (int j = k; j > 0 && order[j - 1] > order[j]; j--) {
      const uint64_t swap = order[j];
      order[j] = order[j - 1];
      order[j - 1] = swap;
    }
  uint8_t ranks[kDkc2NativeFrameMax * kDkc2NativeFrameMax];
  for (int i = 0; i < pixel_count; i++) {
    ranks[i] = 0;
    if (!frame->pixels[i])
      continue;
    const uint32_t color = Rgb(frame->pixels[i]);
    for (int k = 0; k < color_count; k++)
      if ((uint32_t)(order[k] & 0xffffffu) == color)
        ranks[i] = (uint8_t)(k + 1);
  }
  const int scale = kLearnedScale, half = kLearnedScale / 2;
  HdImage *image = NewImage(width * scale, height * scale);
  if (!image)
    return NULL;
  /* Cells start at every frame pixel and one before the first; a cell's
   * subpixels sit half a pixel down and right of its top-left pixel. */
  for (int cell_y = -1; cell_y < height; cell_y++) {
    for (int cell_x = -1; cell_x < width; cell_x++) {
      const int8_t *rows[kLearnedPatterns];
      for (int t = 0; t < kLearnedPatterns; t++) {
        unsigned index = 0;
        for (int n = 0; n < 4; n++) {
          const int y = cell_y + kLearnedPattern[t][n][0];
          const int x = cell_x + kLearnedPattern[t][n][1];
          const unsigned rank = y >= 0 && y < height && x >= 0 && x < width
                                    ? ranks[y * width + x]
                                    : 0u;
          index = index << 4 | rank;
        }
        rows[t] = table + ((size_t)t * kLearnedWindows + index) * kLearnedRow;
      }
      float cell[4][4];  /* premultiplied RGBA of the cell's pixels */
      for (int n = 0; n < 4; n++) {
        const int y = cell_y + kLearnedPattern[0][n][0];
        const int x = cell_x + kLearnedPattern[0][n][1];
        const uint32_t pixel = y >= 0 && y < height && x >= 0 && x < width
                                   ? frame->pixels[y * width + x]
                                   : 0u;
        cell[n][0] = pixel ? (float)((pixel >> 16) & 0xff) / 255.0f : 0.0f;
        cell[n][1] = pixel ? (float)((pixel >> 8) & 0xff) / 255.0f : 0.0f;
        cell[n][2] = pixel ? (float)(pixel & 0xff) / 255.0f : 0.0f;
        cell[n][3] = pixel ? 1.0f : 0.0f;
      }
      for (int sub = 0; sub < scale * scale; sub++) {
        const int out_y = (cell_y + 1) * scale + sub / scale - half;
        const int out_x = (cell_x + 1) * scale + sub % scale - half;
        if (out_y < 0 || out_y >= height * scale || out_x < 0 ||
            out_x >= width * scale)
          continue;
        float logit[kLearnedOutputs], top = -1e30f;
        for (int k = 0; k < kLearnedOutputs; k++) {
          int sum = 0;
          for (int t = 0; t < kLearnedPatterns; t++)
            sum += rows[t][sub * kLearnedOutputs + k];
          logit[k] = (float)sum / kLearnedLogitScale;
          top = logit[k] > top ? logit[k] : top;
        }
        float weight[kLearnedOutputs], total = 0;
        for (int k = 0; k < kLearnedOutputs; k++)
          total += weight[k] = expf(logit[k] - top);
        /* The fifth output is transparent: it only takes weight away. */
        float rgba[4] = {0, 0, 0, 0};
        for (int n = 0; n < 4; n++)
          for (int c = 0; c < 4; c++)
            rgba[c] += weight[n] / total * cell[n][c];
        uint32_t result = 0;
        if (rgba[3] * 255.0f >= 0.5f) {
          result = (uint32_t)(rgba[3] * 255.0f + 0.5f) << 24;
          for (int c = 0; c < 3; c++) {
            float value = rgba[c] / rgba[3] * 255.0f;
            value = value < 0 ? 0 : value > 255 ? 255 : value;
            result |= (uint32_t)(value + 0.5f) << (16 - 8 * c);
          }
        }
        image->pixels[out_y * width * scale + out_x] = result;
      }
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

/* The filtered image of the frame in hd->frame. A full cache is flushed at
 * the start of a frame (the last scene may still use its images), so a
 * probe here always meets a free slot. */
static const HdImage *FilteredCached(Dkc2Hd *hd, uint64_t key) {
  unsigned slot = (unsigned)((key ^ key >> 29) * 0x9e3779b97f4a7c15ull >> 54);
  for (;; slot = (slot + 1) % kCacheSlots) {
    if (!hd->cache[slot].image)
      break;
    if (hd->cache[slot].key == key)
      return hd->cache[slot].image;
  }
  HdImage *image = NULL;
  if (hd->learned)
    image = LearnedImage(hd->learned, &hd->frame);
  if (!image)
    image = hd->filter == kDkc2HdFilterNearest
                ? FrameImage(&hd->frame)
                : SmoothImage(&hd->frame, hd->scale);
  if (image) {
    hd->cache[slot].key = key;
    hd->cache[slot].image = image;
    hd->cache_count++;
  }
  return image;
}

/* The OBJ palette of OAM attributes or record properties. */
static unsigned Palette(uint16_t attributes) { return (attributes >> 9) & 7u; }

/* Decodes the OAM and maps each pixel to its top-most entry. */
static void PrepareOam(Dkc2Hd *hd, const Ppu *ppu) {
  const int margin = (hd->width - kNativeWidth) / 2;
  memset(hd->oam_top, 0, (size_t)hd->width * kHeight);
  /* Lower slots are drawn over higher ones. */
  for (int slot = kDkc2NativeOamEntries - 1; slot >= 0; slot--) {
    LooseEntry *entry = &hd->oam[slot];
    memset(entry, 0, sizeof *entry);
    entry->group = -1;
    Dkc2NativeDecodeOamEntry(ppu, (unsigned)slot, &entry->oam);
    entry->oam.x += margin;
    const uint16_t attributes = entry->oam.attributes;
    entry->rank = (uint8_t)SPRITE_PRIO_TO_PRIO((attributes & 0x3000u) >> 12,
                                               (attributes & 0x800u) == 0);
    for (int y = 0; y < entry->oam.size; y++) {
      const int line = entry->oam.y + y;
      if (line < 0 || line >= kHeight)
        continue;
      for (int x = 0; x < entry->oam.size; x++) {
        const int column = entry->oam.x + x;
        if (column >= 0 && column < hd->width &&
            Dkc2NativeOamPixel(ppu, &entry->oam, x, y))
          hd->oam_top[(size_t)line * hd->width + column] = (uint8_t)(slot + 1);
      }
    }
  }
}

/* Decodes each live record, builds the top-most ownership map and decides
 * which records the HD path draws: only those the PPU actually showed as
 * decoded (at least 7/8 of the compared pixels). Pixels another object
 * covers are not compared: those whose top-most OAM entry has another
 * palette, since a record's own entries all carry its palette (so a
 * misdecoded record is still compared on all of them). A record mostly
 * covered (under a quarter compared) is not verified. */
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
  uint32_t owned[kDkc2NativeSpriteMax] = {0};
  uint32_t compared[kDkc2NativeSpriteMax] = {0};
  uint32_t wrong[kDkc2NativeSpriteMax] = {0};
  for (size_t i = 0; i < (size_t)hd->width * kHeight; i++) {
    if (!hd->owner[i])
      continue;
    const unsigned k = hd->owner[i] - 1u;
    owned[k]++;
    if (hd->obj[i] && hd->oam_top[i] &&
        Palette(hd->oam[hd->oam_top[i] - 1u].oam.attributes) !=
            Palette(hd->sprites[k].sprite.properties))
      continue; /* covered by another object */
    compared[k]++;
    wrong[k] += Rgb(hd->obj[i]) != Rgb(hd->native[i]) || !hd->obj[i];
  }
  for (unsigned k = 0; k < hd->sprite_count; k++) {
    SpriteState *state = &hd->sprites[k];
    state->replaced = state->decoded && compared[k] > 0 &&
                      compared[k] * 4 >= owned[k] &&
                      wrong[k] * 8 <= compared[k];
  }
}

static void DumpSprite(Dkc2Hd *hd, const Ppu *ppu, const SpriteState *state) {
  /* Frames seen during a screen fade would be dumped darkened; wait for
   * one at full brightness. */
  if (!hd->dump_dir[0] || ppu->brightnessMult[31] != 255u)
    return;
  const uint64_t key = SpriteArtKey(ppu, state);
  PackEntry *entry =
      PackFind(hd->sprite_art, kSpriteArtSlots, &hd->sprite_art_count, key);
  if (!entry || entry->dumped)
    return;
  entry->dumped = true;
  if (!Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
    return;
  const unsigned palette = (unsigned)(key >> 16);
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/%04x_p%08x.rgba", hd->dump_dir,
           state->sprite.graphic_id, palette);
  if (!WriteFrameImage(path, &hd->frame))
    return;
  snprintf(path, sizeof path, "%s/index.txt", hd->dump_dir);
  FILE *index = fopen(path, "a");
  bool ok = index && fprintf(index, "%04x p%08x %04x %d %d\n",
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

/* Whether the authentic object pixel at i belongs to a replaced record. */
static bool ReplacedPixel(const Dkc2Hd *hd, size_t i) {
  const unsigned owner = hd->owner[i];
  return owner && hd->sprites[owner - 1].replaced &&
         Rgb(hd->obj[i]) == Rgb(hd->native[i]);
}

/* Records sprite-table record k as a quad of its high-resolution image:
 * pack art, else the filtered ROM frame. */
static void AddSprite(Dkc2Hd *hd, const Ppu *ppu, unsigned k,
                      unsigned position) {
  const SpriteState *state = &hd->sprites[k];
  const HdImage *image = PackImage(hd, ppu, state);
  const bool art = image != NULL;
  const unsigned brightness = art ? ppu->brightnessMult[31] : 255u;
  if (!image) {
    if (!Dkc2NativeDecodeSpriteFrame(ppu, &state->sprite, &hd->frame))
      return;
    image = FilteredCached(hd, PaletteKey(ppu, state));
    if (!image)
      return;
  }
  const int scale = hd->scale;
  snprintf(hd->quad_name[hd->quad_count], sizeof hd->quad_name[0],
           "%04x_p%08x", state->sprite.graphic_id,
           (unsigned)(SpriteArtKey(ppu, state) >> 16));
  hd->quad_art[hd->quad_count] = art;
  Dkc2HdQuad *quad = &hd->quads[hd->quad_count++];
  quad->image = image;
  quad->width = state->width * scale;
  quad->height = state->height * scale;
  quad->flip_x = (state->sprite.properties & 0x4000u) != 0;
  quad->flip_y = (state->sprite.properties & 0x8000u) != 0;
  quad->x = quad->flip_x ? (state->sprite.x - state->left) * scale - quad->width
                         : (state->sprite.x + state->left) * scale;
  quad->y = quad->flip_y ? (state->sprite.y - state->top) * scale - quad->height
                         : (state->sprite.y + state->top) * scale;
  quad->brightness = (uint8_t)brightness;
  quad->rank = state->rank;
  quad->index = (uint8_t)position;
}

/* Object color (brightness applied) of palette pixel in an OAM palette. */
static uint32_t ObjColor(const Ppu *ppu, const LooseEntry *entry,
                         uint8_t pixel) {
  const uint16_t color =
      ppu->cgram[0x80u + ((entry->oam.attributes >> 9) & 7u) * 16u + pixel];
  return 0xff000000u | (uint32_t)ppu->brightnessMult[color & 0x1f] << 16 |
         (uint32_t)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
         ppu->brightnessMult[(color >> 10) & 0x1f];
}

static unsigned GroupRoot(uint8_t *parent, unsigned slot) {
  while (parent[slot] != slot)
    slot = parent[slot] = parent[parent[slot]];
  return slot;
}

/* Objects outside the sprite table (single bananas, the HUD, text) come
 * from OAM. Each pixel goes to its top-most entry; an entry drawing a
 * record's pixel belongs to the record, the others are loose and verified
 * like records (the PPU must show at least 7/8 of the pixels where they
 * are on top). Touching verified entries of one rank form a group drawn as
 * one quad (a 16x16 banana is several 8x8 entries in some sizes); the draw
 * order slots each group behind the first record whose lowest OAM slot is
 * below the group's, so records keep their relative order. */
static void PrepareLoose(Dkc2Hd *hd, const Ppu *ppu) {
  const size_t pixel_count = (size_t)hd->width * kHeight;
  memset(hd->loose_owner, 0, pixel_count);
  memset(hd->record_front, 0xff, sizeof hd->record_front);
  for (size_t i = 0; i < pixel_count; i++) {
    if (!hd->oam_top[i])
      continue;
    const unsigned slot = hd->oam_top[i] - 1u;
    LooseEntry *entry = &hd->oam[slot];
    const unsigned owner = hd->owner[i];
    if (owner && hd->obj[i] && Rgb(hd->obj[i]) == Rgb(hd->native[i])) {
      entry->record = true;
      if (slot < hd->record_front[owner - 1])
        hd->record_front[owner - 1] = (uint8_t)slot;
      continue;
    }
    const uint8_t pixel = Dkc2NativeOamPixel(
        ppu, &entry->oam, (int)(i % (size_t)hd->width) - entry->oam.x,
        (int)(i / (size_t)hd->width) - entry->oam.y);
    const bool shown =
        hd->obj[i] && Rgb(hd->obj[i]) == Rgb(ObjColor(ppu, entry, pixel));
    entry->compared++;
    entry->wrong += !shown;
    /* Claimed once its group is known. */
    hd->loose_owner[i] = shown ? 0xff : 0;
  }
  uint8_t parent[kDkc2NativeOamEntries];
  for (unsigned slot = 0; slot < kDkc2NativeOamEntries; slot++) {
    const LooseEntry *entry = &hd->oam[slot];
    parent[slot] = (uint8_t)slot;
    if (entry->record || !entry->compared ||
        entry->wrong * 8u > entry->compared)
      continue;
    for (unsigned other = 0; other < slot; other++) {
      const LooseEntry *near = &hd->oam[other];
      if (near->record || !near->compared ||
          near->wrong * 8u > near->compared || near->rank != entry->rank ||
          near->oam.x > entry->oam.x + entry->oam.size ||
          entry->oam.x > near->oam.x + near->oam.size ||
          near->oam.y > entry->oam.y + entry->oam.size ||
          entry->oam.y > near->oam.y + near->oam.size)
        continue;
      parent[GroupRoot(parent, slot)] = (uint8_t)GroupRoot(parent, other);
    }
  }
  /* Groups in slot order of their roots; front is the lowest slot. */
  hd->loose_count = 0;
  int16_t group_of_root[kDkc2NativeOamEntries];
  memset(group_of_root, 0xff, sizeof group_of_root);
  for (unsigned slot = 0; slot < kDkc2NativeOamEntries; slot++) {
    LooseEntry *entry = &hd->oam[slot];
    if (entry->record || !entry->compared ||
        entry->wrong * 8u > entry->compared)
      continue;
    const unsigned root = GroupRoot(parent, slot);
    if (group_of_root[root] < 0) {
      LooseGroup *group = &hd->loose[hd->loose_count];
      group->left = entry->oam.x;
      group->top = entry->oam.y;
      group->right = entry->oam.x + entry->oam.size;
      group->bottom = entry->oam.y + entry->oam.size;
      group->rank = entry->rank;
      group->front = (uint8_t)slot;
      group_of_root[root] = (int16_t)hd->loose_count++;
    }
    LooseGroup *group = &hd->loose[group_of_root[root]];
    if (entry->oam.x < group->left)
      group->left = entry->oam.x;
    if (entry->oam.y < group->top)
      group->top = entry->oam.y;
    if (entry->oam.x + entry->oam.size > group->right)
      group->right = entry->oam.x + entry->oam.size;
    if (entry->oam.y + entry->oam.size > group->bottom)
      group->bottom = entry->oam.y + entry->oam.size;
    entry->group = group_of_root[root];
  }
  /* A group too large for a frame stays in the base. */
  for (unsigned slot = 0; slot < kDkc2NativeOamEntries; slot++) {
    LooseEntry *entry = &hd->oam[slot];
    if (entry->group < 0)
      continue;
    const LooseGroup *group = &hd->loose[entry->group];
    if (group->right - group->left > kDkc2NativeFrameMax ||
        group->bottom - group->top > kDkc2NativeFrameMax)
      entry->group = -1;
  }
  for (size_t i = 0; i < pixel_count; i++) {
    if (!hd->loose_owner[i])
      continue;
    const int group = hd->oam[hd->oam_top[i] - 1u].group;
    hd->loose_owner[i] = group < 0 ? 0 : (uint8_t)(group + 1);
  }
  /* Draw order: back (higher slots) to front. */
  uint8_t sorted[kDkc2NativeOamEntries];
  unsigned sorted_count = 0;
  for (unsigned g = 0; g < hd->loose_count; g++) {
    bool used = false;
    for (unsigned slot = 0; slot < kDkc2NativeOamEntries && !used; slot++)
      used = hd->oam[slot].group == (int)g;
    if (!used)
      continue;
    unsigned n = sorted_count++;
    for (; n > 0 && hd->loose[sorted[n - 1]].front < hd->loose[g].front; n--)
      sorted[n] = sorted[n - 1];
    sorted[n] = (uint8_t)g;
  }
  hd->order_count = 0;
  unsigned next = 0;
  for (unsigned k = 0; k < hd->sprite_count; k++) {
    for (; next < sorted_count &&
           hd->loose[sorted[next]].front > hd->record_front[k];
         next++) {
      hd->loose[sorted[next]].position = (uint8_t)hd->order_count;
      hd->order[hd->order_count++] = (DrawItem){true, sorted[next]};
    }
    hd->record_position[k] = (uint8_t)hd->order_count;
    hd->order[hd->order_count++] = (DrawItem){false, (uint8_t)k};
  }
  for (; next < sorted_count; next++) {
    hd->loose[sorted[next]].position = (uint8_t)hd->order_count;
    hd->order[hd->order_count++] = (DrawItem){true, sorted[next]};
  }
}

/* Decodes loose group g into hd->frame (brightness applied) and returns its
 * key: its size and raw palette colors, so the same art follows the object
 * through fades and wherever OAM, VRAM or the palette slot put it. */
static uint64_t DecodeLoose(Dkc2Hd *hd, const Ppu *ppu, unsigned g) {
  const LooseGroup *group = &hd->loose[g];
  Dkc2NativeSpriteFrame *frame = &hd->frame;
  frame->left = frame->top = 0;
  frame->width = group->right - group->left;
  frame->height = group->bottom - group->top;
  const size_t pixel_count = (size_t)frame->width * (size_t)frame->height;
  memset(frame->pixels, 0, sizeof frame->pixels[0] * pixel_count);
  memset(hd->loose_raw, 0xff, sizeof hd->loose_raw[0] * pixel_count);
  for (int slot = kDkc2NativeOamEntries - 1; slot >= 0; slot--) {
    const LooseEntry *entry = &hd->oam[slot];
    if (entry->group != (int)g)
      continue;
    const unsigned palette = (entry->oam.attributes >> 9) & 7u;
    for (int y = 0; y < entry->oam.size; y++) {
      for (int x = 0; x < entry->oam.size; x++) {
        const uint8_t pixel = Dkc2NativeOamPixel(ppu, &entry->oam, x, y);
        if (!pixel)
          continue;
        const size_t i =
            (size_t)(entry->oam.y + y - group->top) * (size_t)frame->width +
            (size_t)(entry->oam.x + x - group->left);
        frame->pixels[i] = ObjColor(ppu, entry, pixel);
        hd->loose_raw[i] =
            ppu->cgram[0x80u + palette * 16u + pixel] & 0x7fffu;
      }
    }
  }
  uint64_t hash = 14695981039346656037ull;
  hash = (hash ^ (uint64_t)frame->width) * 1099511628211ull;
  hash = (hash ^ (uint64_t)frame->height) * 1099511628211ull;
  for (size_t i = 0; i < pixel_count; i++)
    hash = (hash ^ hd->loose_raw[i]) * 1099511628211ull;
  return hash | 1ull << 63;
}

static void DumpLoose(Dkc2Hd *hd, const Ppu *ppu, uint64_t key,
                      PackEntry *entry) {
  if (!hd->dump_dir[0] || !entry || entry->dumped ||
      ppu->brightnessMult[31] != 255u)
    return;
  entry->dumped = true;
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/o_%016llx.rgba", hd->dump_dir,
           (unsigned long long)key);
  if (!WriteFrameImage(path, &hd->frame))
    return;
  snprintf(path, sizeof path, "%s/index.txt", hd->dump_dir);
  FILE *index = fopen(path, "a");
  bool ok = index && fprintf(index, "o %016llx %04x %d %d\n",
                             (unsigned long long)key, kLooseType,
                             hd->frame.width, hd->frame.height) >= 0;
  if (index && fclose(index) != 0)
    ok = false;
  if (!ok)
    fprintf(stderr, "warning: DKC2_HD_DUMP could not update %s\n", path);
}

/* Records loose group g as a quad: pack art "o_<key>.rgba", else the
 * filtered frame. */
static void AddLoose(Dkc2Hd *hd, const Ppu *ppu, unsigned g,
                     unsigned position) {
  const LooseGroup *group = &hd->loose[g];
  const uint64_t key = DecodeLoose(hd, ppu, g);
  PackEntry *entry =
      PackFind(hd->sprite_art, kSpriteArtSlots, &hd->sprite_art_count, key);
  DumpLoose(hd, ppu, key, entry);
  if (entry && hd->pack_dir[0] && entry->state == kPackUnknown) {
    char name[32];
    snprintf(name, sizeof name, "o_%016llx.rgba", (unsigned long long)key);
    entry->image = LoadPackImage(hd, name);
    entry->state = entry->image ? kPackLoaded : kPackMissing;
  }
  const HdImage *image = entry ? entry->image : NULL;
  const bool art = image != NULL;
  const unsigned brightness = art ? ppu->brightnessMult[31] : 255u;
  if (!image)
    image = FilteredCached(hd, key ^ (uint64_t)ppu->brightnessMult[31] << 48);
  if (!image)
    return;
  const int scale = hd->scale;
  snprintf(hd->quad_name[hd->quad_count], sizeof hd->quad_name[0],
           "o_%016llx", (unsigned long long)key);
  hd->quad_art[hd->quad_count] = art;
  Dkc2HdQuad *quad = &hd->quads[hd->quad_count++];
  quad->image = image;
  quad->x = group->left * scale;
  quad->y = group->top * scale;
  quad->width = (group->right - group->left) * scale;
  quad->height = (group->bottom - group->top) * scale;
  quad->flip_x = quad->flip_y = false;
  quad->brightness = (uint8_t)brightness;
  quad->rank = group->rank;
  quad->index = (uint8_t)position;
}

/* The occlusion rules of Dkc2HdScene for a sprite quad at 1x pixel i. */
static bool SpriteHidden(const Dkc2Hd *hd, const Dkc2HdQuad *quad, size_t i) {
  const uint32_t occlusion = hd->occlusion[i];
  const unsigned background = (occlusion >> 16) & 0xffu;
  const unsigned object = (occlusion >> 8) & 0xffu;
  const unsigned owner = occlusion & 0xffu;
  return quad->rank <= background || (object && object >= quad->rank) ||
         (owner && owner - 1u > quad->index);
}

/* Draws a quad of a kind into target (output pixels). */
/* A 5-bit color channel expanded to 8 bits as the PPU does. */
static unsigned Expand5(unsigned value) { return value << 3 | value >> 2; }

static void DrawQuad(Dkc2Hd *hd, uint32_t *target, const Dkc2HdQuad *quad,
                     QuadKind kind) {
  const HdImage *image = quad->image;
  const int scale = hd->scale;
  const int out_width = hd->width * scale, out_height = kHeight * scale;
  const bool whole = quad->source_width == 0;
  const int source_x = whole ? 0 : quad->source_x;
  const int source_y = whole ? 0 : quad->source_y;
  const int source_width = whole ? image->width : quad->source_width;
  const int source_height = whole ? image->height : quad->source_height;
  /* Added per channel, in the pixel's byte order (blue lowest). */
  const unsigned add[3] = {Expand5((quad->add_color >> 10) & 0x1fu),
                           Expand5((quad->add_color >> 5) & 0x1fu),
                           Expand5(quad->add_color & 0x1fu)};
  for (int hy = 0; hy < quad->height; hy++) {
    const int y = quad->flip_y ? quad->y + quad->height - 1 - hy
                               : quad->y + hy;
    if (y < 0 || y >= out_height)
      continue;
    const uint32_t *source =
        image->pixels +
        (size_t)(source_y + hy * source_height / quad->height) *
            (size_t)image->width +
        (size_t)source_x;
    for (int hx = 0; hx < quad->width; hx++) {
      const int x = quad->flip_x ? quad->x + quad->width - 1 - hx
                                 : quad->x + hx;
      if (x < 0 || x >= out_width)
        continue;
      uint32_t pixel = source[hx * source_width / quad->width];
      const unsigned alpha = pixel >> 24;
      const size_t i =
          (size_t)(y / scale) * (size_t)hd->width + (size_t)(x / scale);
      if (kind == kQuadSprite && (!alpha || SpriteHidden(hd, quad, i)))
        continue;
      if (kind == kQuadTile && (!alpha || hd->tile_mask[i] != quad->rank))
        continue;
      if (quad->add_color) {
        uint32_t sum = pixel & 0xff000000u;
        for (int c = 0; c < 3; c++) {
          const unsigned channel = ((pixel >> (c * 8)) & 0xffu) + add[c];
          sum |= (channel > 255u ? 255u : channel) << (c * 8);
        }
        pixel = sum;
      }
      if (quad->brightness != 255u) {
        uint32_t scaled = pixel & 0xff000000u;
        for (int shift = 0; shift < 24; shift += 8)
          scaled |= (((pixel >> shift) & 0xff) * quad->brightness / 255u)
                    << shift;
        pixel = scaled;
      }
      uint32_t *destination = &target[(size_t)y * (size_t)out_width + x];
      *destination =
          kind == kQuadArt ? pixel : Blend(*destination, pixel, alpha);
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

static PackEntry *TerrainFind(Dkc2Hd *hd, uint64_t key) {
  return PackFind(hd->terrain, kTerrainSlots, &hd->terrain_count, key);
}

static const HdImage *TerrainArt(Dkc2Hd *hd, PackEntry *entry) {
  if (entry->state == kPackUnknown) {
    char name[32];
    snprintf(name, sizeof name, "t_%016llx.rgba",
             (unsigned long long)entry->key);
    entry->image = LoadPackImage(hd, name);
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

/* Records art over the frame area (x, y, width, height) of the BG1 layer. */
static void AddArt(Dkc2Hd *hd, const HdImage *image, int x, int y, int width,
                   int height, unsigned brightness) {
  if (hd->art_count >= kArtMax)
    return;
  const int scale = hd->scale;
  Dkc2HdQuad *quad = &hd->art[hd->art_count++];
  memset(quad, 0, sizeof *quad);
  quad->image = image;
  quad->x = x * scale;
  quad->y = y * scale;
  quad->width = width * scale;
  quad->height = height * scale;
  quad->brightness = (uint8_t)brightness;
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
      PackEntry *entry = TerrainFind(hd, key);
      if (!entry)
        continue;
      if (hd->layer_frame) {
        const HdImage *image = TerrainArt(hd, entry);
        if (image)
          AddArt(hd, image, x, y, kBlock, kBlock, brightness);
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

/* A CGRAM color with the screen brightness, as captures hold it. */
static uint32_t CgramRgb(const Ppu *ppu, uint8_t index) {
  const uint16_t color = ppu->cgram[index];
  return (uint32_t)ppu->brightnessMult[color & 0x1f] << 16 |
         (uint32_t)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
         ppu->brightnessMult[(color >> 10) & 0x1f];
}

/* Identifies an unflipped background character by the raw color of every
 * pixel, so it holds through fades and palette numbers. Never 0. */
static uint64_t TileKey(const Ppu *ppu, const uint8_t indices[kTilePixels]) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned i = 0; i < kTilePixels; i++) {
    hash ^= indices[i] ? ppu->cgram[indices[i]] & 0x7fffu : 0x8000u;
    hash *= 1099511628211ull;
  }
  return hash ? hash : 1;
}

/* Hash of a whole cell's captured pixels (frame position x, y), 0 when any
 * pixel lies outside the frame. */
static uint64_t TileContent(const Dkc2Hd *hd, unsigned layer, int x, int y) {
  if (x < 0 || y < 0 || x + kTile > hd->width || y + kTile > kHeight)
    return 0;
  uint64_t hash = 14695981039346656037ull;
  for (int ty = 0; ty < kTile; ty++) {
    for (int tx = 0; tx < kTile; tx++) {
      hash ^= Opaque(hd->bg[layer][(size_t)(y + ty) * hd->width + x + tx]);
      hash *= 1099511628211ull;
    }
  }
  return hash ? hash : 1;
}

static TileSeen *TileSeenFind(Dkc2Hd *hd, uint64_t content) {
  unsigned slot = (unsigned)((content * 0x9e3779b97f4a7c15ull) >> 52);
  for (;; slot = (slot + 1) % kTileSeenSlots) {
    TileSeen *seen = &hd->tile_seen[slot];
    if (!seen->content || seen->content == content)
      return seen;
  }
}

/* Records a cell's art (pack file "b_<key>.rgba") as a tile quad, and with
 * DKC2_HD_DUMP writes a new character seen at full brightness. */
static void AddTile(Dkc2Hd *hd, const Ppu *ppu, unsigned layer, int x, int y,
                    uint64_t key, uint16_t entry, const uint8_t *indices) {
  PackEntry *art =
      PackFind(hd->tile_art, kTileArtSlots, &hd->tile_art_count, key);
  if (!art)
    return;
  if (indices && hd->dump_dir[0] && !art->dumped &&
      ppu->brightnessMult[31] == 255u) {
    uint32_t pixels[kTilePixels];
    for (unsigned i = 0; i < kTilePixels; i++)
      pixels[i] = indices[i] ? 0xff000000u | CgramRgb(ppu, indices[i]) : 0;
    char path[kPathMax + 32];
    snprintf(path, sizeof path, "%s/b_%016llx.rgba", hd->dump_dir,
             (unsigned long long)key);
    art->dumped = WriteCaptureImage(path, pixels, kTile, kTile, kTile);
    hd->tile_layout_new |= art->dumped;
  }
  if (art->state == kPackUnknown && hd->pack_dir[0]) {
    char name[32];
    snprintf(name, sizeof name, "b_%016llx.rgba", (unsigned long long)key);
    art->image = LoadPackImage(hd, name);
    art->state = art->image ? kPackLoaded : kPackMissing;
  }
  if (!art->image || hd->tile_count >= kTileMax)
    return;
  const int scale = hd->scale;
  Dkc2HdQuad *quad = &hd->tiles[hd->tile_count++];
  memset(quad, 0, sizeof *quad);
  quad->image = art->image;
  quad->x = x * scale;
  quad->y = y * scale;
  quad->width = kTile * scale;
  quad->height = kTile * scale;
  quad->flip_x = (entry & 0x4000u) != 0;
  quad->flip_y = (entry & 0x8000u) != 0;
  quad->brightness = ppu->brightnessMult[31];
  quad->rank = (uint8_t)(layer + 1);
}

/* Appends the frame's background cells to tiles.txt in the dump. */
static void WriteTileLayout(const Dkc2Hd *hd) {
  char path[kPathMax + 32];
  snprintf(path, sizeof path, "%s/tiles.txt", hd->dump_dir);
  FILE *layout = fopen(path, "a");
  bool ok = layout && fprintf(layout, "%.*s\n", (int)hd->tile_layout_length,
                              hd->tile_layout) >= 0;
  if (layout && fclose(layout) != 0)
    ok = false;
  if (!ok)
    fprintf(stderr, "warning: DKC2_HD_DUMP could not update %s\n", path);
}

/* Whether two lines draw a layer with the same registers. */
static bool SameTileLine(const Dkc2HdmaLineState *a,
                         const Dkc2HdmaLineState *b, unsigned layer) {
  return a->h_scroll[layer] == b->h_scroll[layer] &&
         a->v_scroll[layer] == b->v_scroll[layer] &&
         a->bg_sc[layer] == b->bg_sc[layer] &&
         ((a->tile_base ^ b->tile_base) >> (layer * 4) & 0xfu) == 0 &&
         a->bgmode == b->bgmode && a->mosaic == b->mosaic;
}

/* One row of cells of a layer whose top lies at frame row top; frame rows
 * first..end (past) show it, drawn with the registers of state. Cells VRAM
 * reproduces (7/8 of the pixels either shows) are added and remembered by
 * their captured pixels; other whole cells are left pending. */
static void ProcessTileRow(Dkc2Hd *hd, const Ppu *ppu,
                           const Dkc2HdmaLineState *state, unsigned layer,
                           int top, int first, int end, unsigned *pending) {
  const unsigned bpp = layer == 2 ? 2u : 4u;
  const int extra = (hd->width - kNativeWidth) / 2;
  const unsigned h_scroll = state->h_scroll[layer];
  /* Captured row 0 is scanline 1. */
  const unsigned map_y = (unsigned)(top + 1) + state->v_scroll[layer];
  const uint16_t character_base =
      (uint16_t)(((state->tile_base >> (layer * 4)) & 0xfu) << 12);
  uint8_t indices[kTilePixels];
  for (int x = -(int)((h_scroll - (unsigned)extra) & 7u); x < hd->width;
       x += kTile) {
    const uint16_t entry = Dkc2NativeBgMapEntry(
        ppu, state->bg_sc[layer], (unsigned)(x - extra) + h_scroll, map_y);
    Dkc2NativeDecodeBgCharacter(ppu, bpp, character_base, entry, indices);
    size_t compared = 0, wrong = 0;
    for (int ty = 0; ty < kTile; ty++) {
      const int y = top + ty;
      if (y < first || y >= end)
        continue;
      const int sy = (entry & 0x8000u) ? kTile - 1 - ty : ty;
      for (int tx = 0; tx < kTile; tx++) {
        if (x + tx < 0 || x + tx >= hd->width)
          continue;
        const int sx = (entry & 0x4000u) ? kTile - 1 - tx : tx;
        const uint8_t index = indices[sy * kTile + sx];
        const uint32_t captured =
            hd->bg[layer][(size_t)y * hd->width + x + tx];
        if (!index && !captured)
          continue;
        compared++;
        wrong += !index || !captured ||
                 Rgb(captured) != CgramRgb(ppu, index);
      }
    }
    const uint64_t content = TileContent(hd, layer, x, top);
    if (compared == 0 || wrong * 8 > compared) {
      if (content && *pending < kTileColumns * kTileRows)
        hd->tile_pending[(*pending)++] = (TilePending){x, top};
      continue;
    }
    const uint64_t key = TileKey(ppu, indices);
    const unsigned columns = state->bg_sc[layer] & 1u ? 64u : 32u;
    const unsigned rows = state->bg_sc[layer] & 2u ? 64u : 32u;
    const unsigned cell_x = (((unsigned)(x - extra) + h_scroll) >> 3) &
                            (columns - 1u);
    const unsigned cell_y = (map_y >> 3) & (rows - 1u);
    for (unsigned p = 0; p < hd->panorama_count; p++) {
      PanoramaEntry *panorama = &hd->panoramas[p];
      const size_t cell = (size_t)cell_y * kMapSide + cell_x;
      if (panorama->layer != layer || !panorama->cells[cell])
        continue;
      panorama->compared++;
      panorama->wrong += panorama->cells[cell] != key ||
                         panorama->flips[cell] != entry >> 14;
    }
    if (hd->dump_dir[0] && ppu->brightnessMult[31] == 255u) {
      const size_t room = sizeof hd->tile_layout - hd->tile_layout_length;
      const int written = snprintf(
          hd->tile_layout + hd->tile_layout_length, room, " %u,%u,%u,%016llx,%u",
          layer, cell_x, cell_y, (unsigned long long)key,
          (unsigned)(entry >> 14));
      if (written > 0 && (size_t)written < room)
        hd->tile_layout_length += (size_t)written;
    }
    if (content) {
      TileSeen *seen = TileSeenFind(hd, content);
      *seen = (TileSeen){content, key, entry};
    }
    AddTile(hd, ppu, layer, x, top, key, entry, indices);
  }
}

/* Whether two lines draw a layer with the same registers and color math. */
static bool SamePanoramaLine(const Dkc2HdmaLineState *a,
                             const Dkc2HdmaLineState *b, unsigned layer) {
  return SameTileLine(a, b, layer) && a->cgwsel == b->cgwsel &&
         a->cgadsub == b->cgadsub && a->fixed_color == b->fixed_color;
}

/* A captured layer pixel with the fixed color added by the PPU (5-bit
 * channels, saturated, then the brightness). */
static uint32_t AddFixedColor(const Ppu *ppu, uint32_t captured,
                              uint16_t fixed) {
  uint32_t sum = 0;
  for (int c = 0; c < 3; c++) {
    const unsigned value = (captured >> (c * 8)) & 0xffu;
    unsigned level = 0;
    while (level < 31u && ppu->brightnessMult[level] != value)
      level++;
    /* brightnessMult holds 31 copies of its last entry past 31. */
    sum |= (uint32_t)ppu->brightnessMult[level + ((fixed >> (10 - 5 * c)) &
                                                  0x1fu)]
           << (c * 8);
  }
  return sum;
}

/* Lines first..end (past) of a panorama layer drawn with state: the art
 * where the scroll puts the tilemap, split where the tilemap wraps. Lines
 * that add the fixed color add it to the art, and their layer pixels with
 * that color added join the tile mask. */
static void AddPanoramaLines(Dkc2Hd *hd, const Ppu *ppu,
                             const Dkc2HdmaLineState *state, unsigned layer,
                             const PanoramaEntry *panorama, int first,
                             int end) {
  const bool math = (state->cgadsub >> layer & 1u) != 0;
  const bool readable = (state->bgmode & 7) == 1 &&
                        !(state->bgmode >> (layer + 4) & 1) &&
                        !(state->mosaic >> layer & 1);
  /* Clipping to black, subtraction, halving and windowed math leave the
   * lines native. */
  if (!readable || (state->cgwsel & 0xc0u) ||
      (math && ((state->cgadsub & 0xc0u) || (state->cgwsel & 0x30u))))
    return;
  const unsigned map_width = (state->bg_sc[layer] & 1u ? 64u : 32u) * kTile;
  const unsigned map_height = (state->bg_sc[layer] & 2u ? 64u : 32u) * kTile;
  const HdImage *art = panorama->art;
  const unsigned multiple = (unsigned)art->width / map_width;
  if (multiple == 0 || (unsigned)art->width != map_width * multiple ||
      (unsigned)art->height != map_height * multiple)
    return;
  const uint16_t add = math ? state->fixed_color & 0x7fffu : 0;
  uint8_t rank = (uint8_t)(layer + 1);
  if (add) {
    rank |= kDkc2HdTileMath;
    for (int y = first; y < end; y++) {
      for (int x = 0; x < hd->width; x++) {
        const size_t i = (size_t)y * (size_t)hd->width + (size_t)x;
        const uint32_t captured = hd->bg[layer][i];
        if (captured && (uint8_t)(captured >> 24) == hd->base_rank[i] &&
            (Rgb(hd->base[i]) == Rgb(captured) ||
             Rgb(hd->base[i]) == AddFixedColor(ppu, captured, add)))
          hd->tile_mask[i] = rank;
      }
    }
  }
  const int extra = (hd->width - kNativeWidth) / 2;
  const int scale = hd->scale;
  for (int y = first; y < end;) {
    /* Captured row 0 is scanline 1. */
    const unsigned map_y =
        ((unsigned)(y + 1) + state->v_scroll[layer]) & (map_height - 1u);
    const int height =
        end - y < (int)(map_height - map_y) ? end - y : (int)(map_height - map_y);
    for (int x = 0; x < hd->width && hd->tile_count < kTileMax;) {
      const unsigned map_x =
          ((unsigned)(x - extra) + state->h_scroll[layer]) & (map_width - 1u);
      const int width = hd->width - x < (int)(map_width - map_x)
                            ? hd->width - x
                            : (int)(map_width - map_x);
      Dkc2HdQuad *quad = &hd->tiles[hd->tile_count++];
      memset(quad, 0, sizeof *quad);
      quad->image = art;
      quad->x = x * scale;
      quad->y = y * scale;
      quad->width = width * scale;
      quad->height = height * scale;
      quad->source_x = (int)(map_x * multiple);
      quad->source_y = (int)(map_y * multiple);
      quad->source_width = width * (int)multiple;
      quad->source_height = height * (int)multiple;
      quad->brightness = ppu->brightnessMult[31];
      quad->rank = rank;
      quad->add_color = add;
      x += width;
    }
    y += height;
  }
}

/* The pack panorama for a layer whose reproduced cells match it this
 * frame, if any. */
static const PanoramaEntry *MatchPanorama(const Dkc2Hd *hd, unsigned layer) {
  for (unsigned p = 0; p < hd->panorama_count; p++) {
    const PanoramaEntry *panorama = &hd->panoramas[p];
    if (panorama->layer == layer &&
        panorama->compared >= kPanoramaMinCells &&
        panorama->wrong * 8 <= panorama->compared)
      return panorama;
  }
  return NULL;
}

/* Background tile art for BG2 and BG3. Each layer is split into rows of
 * cells drawn with the same registers (scroll can change every scanline);
 * cells cut by a register change are skipped. A whole cell VRAM does not
 * reproduce takes the character of a reproduced cell with the same captured
 * pixels: widescreen margins are drawn from a world store, not VRAM. */
static void ProcessTiles(Dkc2Hd *hd, const Ppu *ppu,
                         const Dkc2HdmaLines *lines) {
  hd->tile_count = 0;
  hd->tile_layout_length = 0;
  hd->tile_layout_new = false;
  if (!lines || !hd->tile_pass)
    return;
  for (unsigned layer = kTileFirstLayer; layer < kBgLayers; layer++) {
    memset(hd->tile_seen, 0, sizeof hd->tile_seen);
    for (unsigned p = 0; p < hd->panorama_count; p++)
      hd->panoramas[p].compared = hd->panoramas[p].wrong = 0;
    const unsigned layer_tiles = hd->tile_count;
    unsigned pending = 0;
    int row = 0;
    while (row < kHeight) {
      const Dkc2HdmaLineState *state = &lines->line[row + 1];
      const int phase =
          (int)((unsigned)(row + 1) + state->v_scroll[layer]) & (kTile - 1);
      int end = row + 1;
      while (end < kHeight && end - row < kTile - phase &&
             SameTileLine(state, &lines->line[end + 1], layer))
        end++;
      const int top = row - phase;
      const bool whole =
          (phase == 0 || row == 0) && (end - top == kTile || end == kHeight);
      const bool readable = (state->bgmode & 7) == 1 &&
                            !(state->bgmode >> (layer + 4) & 1) &&
                            !(state->mosaic >> layer & 1);
      if (whole && readable)
        ProcessTileRow(hd, ppu, state, layer, top, row, end, &pending);
      row = end;
    }
    for (unsigned p = 0; p < pending; p++) {
      const TilePending cell = hd->tile_pending[p];
      const TileSeen *seen =
          TileSeenFind(hd, TileContent(hd, layer, cell.x, cell.y));
      if (seen->content)
        AddTile(hd, ppu, layer, cell.x, cell.y, seen->key, seen->entry, NULL);
    }
    /* A matched panorama replaces the layer's cell art. */
    const PanoramaEntry *panorama = MatchPanorama(hd, layer);
    if (!panorama)
      continue;
    hd->tile_count = layer_tiles;
    for (int first = 0; first < kHeight;) {
      int end = first + 1;
      while (end < kHeight && SamePanoramaLine(&lines->line[first + 1],
                                               &lines->line[end + 1], layer))
        end++;
      AddPanoramaLines(hd, ppu, &lines->line[first + 1], layer, panorama,
                       first, end);
      first = end;
    }
  }
  if (hd->tile_layout_new)
    WriteTileLayout(hd);
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
static void AddScreenArt(Dkc2Hd *hd, const ScreenEntry *screen,
                         unsigned brightness) {
  AddArt(hd, screen->art, hd->origin, 0, kNativeWidth, kHeight, brightness);
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
      if (alpha && hd->occlusion[i] >> 24)
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

const Dkc2HdScene *Dkc2HdEndFrameScene(Dkc2Hd *hd, const Ppu *ppu,
                                       const uint8_t *ram,
                                       const Dkc2HdmaLines *lines,
                                       const uint8_t *frame,
                                       size_t frame_pitch) {
  static uint64_t last_serial;
  if (!hd || !hd->active || !ppu || !ram || !frame ||
      frame_pitch < (size_t)hd->width * 4u)
    return NULL;
  hd->origin = (hd->width - kNativeWidth) / 2 - ppu->wsPresentationXBias;
  if (hd->cache_count >= kCacheFlush)
    FlushCache(hd);
  /* The displayed OAM was built from the previous game update. */
  PrepareOam(hd, ppu);
  PrepareSprites(hd, ppu, hd->has_previous ? hd->previous_wram : ram);
  PrepareLoose(hd, ppu);
  memcpy(hd->previous_wram, ram, sizeof hd->previous_wram);
  hd->has_previous = true;

  /* With terrain art BG1 is not in the frame: objects are restored by rank
   * against BG2/BG3 and the HD BG1 layer is drawn by priority afterwards. */
  const bool layered = hd->layer_frame;
  /* The columns the PPU shows: a centered screen (the map) shows no
   * margin, so sprite art crossing into it is hidden there. */
  const int extra = (hd->width - kNativeWidth) / 2;
  const int shown_first = extra - ppu->extraLeftCur;
  const int shown_end = extra + kNativeWidth + ppu->extraRightCur;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < hd->width; x++) {
      const size_t i = (size_t)y * (size_t)hd->width + (size_t)x;
      const uint8_t *bgra = frame + (size_t)y * frame_pitch + (size_t)x * 4u;
      uint32_t pixel = 0xff000000u | (uint32_t)bgra[2] << 16 |
                       (uint32_t)bgra[1] << 8 | bgra[0];
      /* Sprites meet every background layer; the base, BG1 only when it
       * stays in the frame. */
      uint8_t background = 0, rank = 0;
      for (unsigned layer = 0; layer < kBgLayers; layer++) {
        const uint8_t layer_rank = (uint8_t)(hd->bg[layer][i] >> 24);
        if (layer_rank > background)
          background = layer_rank;
        if ((layer > 0 || !layered) && layer_rank > rank)
          rank = layer_rank;
      }
      if (x < shown_first || x >= shown_end)
        background = 0xff;
      uint32_t occlusion = (uint32_t)background << 16;
      const uint32_t object = hd->obj[i];
      if (object && !ReplacedPixel(hd, i) && !hd->loose_owner[i]) {
        if (hd->owner[i] && Rgb(object) == Rgb(hd->native[i]))
          occlusion |= hd->record_position[hd->owner[i] - 1u] + 1u;
        else
          occlusion |= (object >> 24) << 8;
        if ((uint8_t)(object >> 24) > rank) {
          pixel = 0xff000000u | Rgb(object);
          rank = (uint8_t)(object >> 24);
        }
      }
      /* The HD BG1 layer is composed over this base by rank alone, but the
       * base still shows BG1 where it is on top: the layer's soft edges then
       * fade into BG1 itself instead of into the layers it hides. */
      if (layered && hd->bg[0][i] && (uint8_t)(hd->bg[0][i] >> 24) > rank)
        pixel = 0xff000000u | Rgb(hd->bg[0][i]);
      /* A tiled layer shows here when the base pixel is its own, untouched
       * by color math. */
      uint8_t tile_layer = 0;
      for (unsigned layer = kTileFirstLayer; layer < kBgLayers; layer++) {
        const uint32_t captured = hd->bg[layer][i];
        if (captured && (uint8_t)(captured >> 24) == rank &&
            Rgb(captured) == Rgb(pixel))
          tile_layer = (uint8_t)(layer + 1);
      }
      hd->tile_mask[i] = tile_layer;
      hd->base[i] = pixel;
      hd->base_rank[i] = rank;
      hd->occlusion[i] = occlusion;
      if (layered)
        hd->layer_source[i] = Opaque(hd->bg[0][i]);
    }
  }
  hd->art_count = 0;
  const ScreenEntry *screen = layered ? MatchScreen(hd) : NULL;
  if (layered)
    BuildBg1Rank(hd);
  if (screen)
    AddScreenArt(hd, screen, ppu->brightnessMult[31]);
  else if (layered || hd->dump_dir[0])
    ProcessTerrain(hd, ppu, ram);
  if (hd->dump_dir[0])
    DumpScreen(hd, ppu, ram);
  if (layered) {
    for (size_t i = 0; i < (size_t)hd->width * kHeight; i++)
      if (hd->bg1_rank[i] > hd->base_rank[i])
        hd->occlusion[i] |= 0xff000000u;
  }
  ProcessTiles(hd, ppu, lines);
  hd->quad_count = 0;
  for (unsigned position = 0; position < hd->order_count; position++) {
    const DrawItem item = hd->order[position];
    if (item.loose) {
      AddLoose(hd, ppu, item.item, position);
    } else if (hd->sprites[item.item].replaced) {
      DumpSprite(hd, ppu, &hd->sprites[item.item]);
      AddSprite(hd, ppu, item.item, position);
    }
  }
  hd->scene = (Dkc2HdScene){
      .serial = ++last_serial,
      .width = hd->width,
      .height = kHeight,
      .scale = hd->scale,
      .background = hd->background,
      .base = hd->base,
      .tiles = hd->tiles,
      .tile_count = hd->tile_count,
      .tile_mask = hd->tile_mask,
      .occlusion = hd->occlusion,
      .layer = layered ? hd->layer_source : NULL,
      .art = hd->art,
      .art_count = hd->art_count,
      .sprites = hd->quads,
      .sprite_count = hd->quad_count,
  };
  return &hd->scene;
}

const uint32_t *Dkc2HdEndFrame(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram,
                               const Dkc2HdmaLines *lines,
                               const uint8_t *frame, size_t frame_pitch,
                               int *width, int *height) {
  const Dkc2HdScene *scene =
      Dkc2HdEndFrameScene(hd, ppu, ram, lines, frame, frame_pitch);
  if (!scene)
    return NULL;
  Upscale(hd, scene->base, hd->output);
  for (unsigned t = 0; t < scene->tile_count; t++)
    DrawQuad(hd, hd->output, &scene->tiles[t], kQuadTile);
  if (scene->layer) {
    Upscale(hd, scene->layer, hd->layer);
    for (unsigned a = 0; a < scene->art_count; a++)
      DrawQuad(hd, hd->layer, &scene->art[a], kQuadArt);
    CompositeTerrain(hd);
  }
  for (unsigned s = 0; s < scene->sprite_count; s++)
    DrawQuad(hd, hd->output, &scene->sprites[s], kQuadSprite);
  *width = scene->width * scene->scale;
  *height = scene->height * scene->scale;
  return hd->output;
}

bool Dkc2HdProbePixel(const Dkc2Hd *hd, int x, int y, Dkc2HdProbe *probe) {
  if (!hd || !hd->active || !probe || x < 0 || y < 0 || x >= hd->width ||
      y >= kHeight)
    return false;
  memset(probe, 0, sizeof *probe);
  const int scale = hd->scale;
  const int hx = x * scale + scale / 2, hy = y * scale + scale / 2;
  /* Quads are drawn back to front: the last one covering the pixel with
   * an opaque texel is in front. */
  for (unsigned q = hd->quad_count; q-- > 0;) {
    const Dkc2HdQuad *quad = &hd->quads[q];
    if (hx < quad->x || hy < quad->y || hx >= quad->x + quad->width ||
        hy >= quad->y + quad->height)
      continue;
    int ix = (int)((int64_t)(hx - quad->x) * quad->image->width / quad->width);
    int iy =
        (int)((int64_t)(hy - quad->y) * quad->image->height / quad->height);
    if (quad->flip_x)
      ix = quad->image->width - 1 - ix;
    if (quad->flip_y)
      iy = quad->image->height - 1 - iy;
    if (!(quad->image->pixels[(size_t)iy * quad->image->width + ix] >> 24))
      continue;
    probe->object = true;
    probe->art = hd->quad_art[q];
    memcpy(probe->name, hd->quad_name[q], sizeof probe->name);
    probe->x = quad->x / scale;
    probe->y = quad->y / scale;
    probe->width = quad->width / scale;
    probe->height = quad->height / scale;
    break;
  }
  return true;
}
