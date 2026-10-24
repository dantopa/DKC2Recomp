#include "dkc2_native_render.h"

#include <string.h>

enum {
  kSpriteTable = 0x0d84,
  kSpriteStride = 0x5e,
  kSpriteRenderOrder = 0x16fe,
  kSpriteGraphicOffset = 0x18,
  kSpritePropertyOffset = 0x12,
  kSpriteWorldXOffset = 0x06,
  kSpriteWorldYOffset = 0x0a,
  /* File offset of the compound-graphic directory at $BC:8000. */
  kGraphicDirectory = 0x3c8000,
  kGraphicLimit = 0x35a0,
  kCameraX = 0x17ba,
  kCameraY = 0x17c0,
  kNmiSubMode = 0x0096,
  /* Level-name card: a static picture with no camera or terrain stream. */
  kNameCardNmiSubMode = 11,
  /* The column builder reads the level map $0100 pixels west of the BG1
   * world coordinate ($B5:ACA8-$B5:ACB7). */
  kLevelMapOffsetX = 0x0100,
  kLevelMapRows = 0x2000,
  /* Mode 1 BG1 composition ranks (PPU z-buffer high byte). */
  kBg1RankLow = 0x80,
  kBg1RankHigh = 0xc0,
};

static uint16_t Read16(const uint8_t *data, size_t address) {
  return (uint16_t)(data[address] | (data[address + 1] << 8));
}

static uint32_t ColorWithRank(const Ppu *ppu, uint8_t index, uint8_t rank) {
  const uint16_t color = ppu->cgram[index];
  return (uint32_t)rank << 24 |
         (uint32_t)ppu->brightnessMult[color & 0x1f] << 16 |
         (uint32_t)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
         ppu->brightnessMult[(color >> 10) & 0x1f];
}

/* One 4-bpp planar pixel: planes 0/1 in the row word, planes 2/3 eight words
 * later. */
static uint8_t Planar4bppVram(const uint16_t *vram, uint16_t character_word,
                              unsigned x, unsigned y) {
  const uint16_t low = vram[(uint16_t)(character_word + y) & 0x7fff];
  const uint16_t high = vram[(uint16_t)(character_word + y + 8) & 0x7fff];
  const unsigned shift = 7u - x;
  return (uint8_t)(((low >> shift) & 1u) | ((low >> (shift + 8u)) & 1u) << 1 |
                   ((high >> shift) & 1u) << 2 |
                   ((high >> (shift + 8u)) & 1u) << 3);
}

/* The same planar layout stored as bytes in a ROM sprite payload. */
static uint8_t Planar4bppBytes(const uint8_t *character, unsigned x,
                               unsigned y) {
  const unsigned shift = 7u - x;
  const uint8_t *row = character + y * 2u;
  return (uint8_t)(((row[0] >> shift) & 1u) | ((row[1] >> shift) & 1u) << 1 |
                   ((row[16] >> shift) & 1u) << 2 |
                   ((row[17] >> shift) & 1u) << 3);
}

/* Mirrors the PPU's OAM X decode (PpuDecodeOamX, ppu.c): 9-bit X, wrapped
 * negative past the presented right margin or, in the ambiguous band, unless
 * the game hinted the slot positive. */
static int OamScreenX(const Ppu *ppu, unsigned index) {
  int x = ppu->oam[index] & 0xff;
  x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
  const int positive_limit =
      256 + ppu->extraRightCur +
      (ppu->wsPresentationXBias > 0 ? ppu->wsPresentationXBias : 0);
  if (x >= positive_limit) {
    x -= 512;
  } else if (x >= 256 && ppu->wsOamRightHintStrict) {
    const unsigned slot = index >> 1;
    if (!(ppu->wsOamRightHint[slot >> 3] & (1u << (slot & 7))))
      x -= 512;
  }
  /* Widescreen HUD slots move to the screen edges
   * (PpuAdjustWidescreenHudOamX). */
  const uint8_t y = (uint8_t)(ppu->oam[index] >> 8);
  const unsigned slot = index >> 1;
  const bool hud_line = ppu->wsHudOamHeight &&
                        (y >= 224 || y < ppu->wsHudOamHeight);
  const bool hud_slot =
      hud_line &&
      ((ppu->wsHudOamSlots && slot >= ppu->wsHudOamFirstSlot &&
        slot < (unsigned)ppu->wsHudOamFirstSlot + ppu->wsHudOamSlots) ||
       (ppu->wsHudOamSlots2 && slot >= ppu->wsHudOamFirstSlot2 &&
        slot < (unsigned)ppu->wsHudOamFirstSlot2 + ppu->wsHudOamSlots2));
  if (hud_slot) {
    const bool full_budget = (ppu->wsHudSplitHeight & 0x80) != 0;
    if (x >= 0 && x < ppu->wsHudLeftEnd)
      x -= full_budget ? ppu->extraLeftRight : ppu->extraLeftCur;
    else if (x >= ppu->wsHudRightStart && x < 256)
      x += full_budget ? ppu->extraLeftRight : ppu->extraRightCur;
  }
  return x - ppu->wsPresentationXBias;
}

void Dkc2NativeDecodeOamEntry(const Ppu *ppu, unsigned slot,
                              Dkc2NativeOamEntry *entry) {
  static const uint8_t kSizes[8][2] = {{8, 16},  {8, 32},  {8, 64},
                                       {16, 32}, {16, 64}, {32, 64},
                                       {16, 32}, {16, 32}};
  const unsigned index = slot * 2u;
  entry->size = kSizes[PPU_objSize(ppu)]
                      [(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
  entry->x = OamScreenX(ppu, index);
  entry->y = ppu->oam[index] >> 8;
  if (entry->y + entry->size > 256)
    entry->y -= 256;
  entry->attributes = ppu->oam[index + 1];
}

uint8_t Dkc2NativeOamPixel(const Ppu *ppu, const Dkc2NativeOamEntry *entry,
                           int x, int y) {
  const uint16_t attributes = entry->attributes;
  const int row = attributes & 0x8000u ? entry->size - 1 - y : y;
  const int col = attributes & 0x4000u ? entry->size - 1 - x : x;
  const unsigned tile =
      (((attributes & 0xffu) >> 4) + ((unsigned)row >> 3)) << 4 |
      (((attributes & 0xfu) + ((unsigned)col >> 3)) & 0xfu);
  const uint16_t base = attributes & 0x100u ? PPU_objTileAdr2(ppu)
                                            : PPU_objTileAdr1(ppu);
  return Planar4bppVram(ppu->vram, (uint16_t)(base + tile * 16u),
                        (unsigned)col & 7u, (unsigned)row & 7u);
}

bool Dkc2NativeTerrainBegin(const Ppu *ppu, const uint8_t *ram,
                            Dkc2NativeTerrainView *view) {
  if (!ppu || !ram || !view)
    return false;
  const Dkc2VideoLevelLayout layout =
      Dkc2VideoLevelLayoutForScene(Read16(ram, 0x0529), Read16(ram, 0x00d3));
  if (layout != kDkc2VideoLevelLayoutHorizontal || (ppu->bgmode & 7) != 1 ||
      PPU_bigTiles(ppu, 0) ||
      Read16(ram, kNmiSubMode) == kNameCardNmiSubMode)
    return false;
  const uint8_t bank = ram[0x009a];
  if (bank != 0x7e && bank != 0x7f)
    return false;

  /* Horizontal position: the BG1 world column, unwrapped from the rendered
   * scroll phase around the camera. Vertical position follows the level-map
   * row the cartridge's column builder selects for that phase. The first
   * visible scanline is line 1, so screen row 0 shows BG row vScroll + 1. */
  const uint32_t world_x = Dkc2VideoTerrainShadowX(
      (uint16_t)(ppu->hScroll[0] & 0x3ff), Read16(ram, kCameraX));
  const uint16_t scroll_y = (uint16_t)((ppu->vScroll[0] + 1u) & 0x3ff);
  view->bank_data = bank == 0x7e ? ram : ram + 0x10000;
  view->layout = layout;
  view->map_base = Read16(ram, 0x0098);
  view->metatile_base = Read16(ram, 0x17b4);
  view->character_base = (uint16_t)PPU_bgTileAdr(ppu, 0);
  view->map_x = world_x - kLevelMapOffsetX;
  view->first_tile_y =
      Dkc2VideoLevelMapTileY(scroll_y, Read16(ram, kCameraY), 0);
  view->fine_y = scroll_y & 7u;
  view->world_x = (uint16_t)world_x;
  return true;
}

void Dkc2NativeTerrainTileScreen(const Dkc2NativeTerrainView *view,
                                 uint32_t tile_x, uint32_t tile_y, int *x,
                                 int *y) {
  /* Level-map rows are periodic; measure the row distance in that period
   * so blocks straddling the wrap stay contiguous on screen. */
  uint32_t row = (tile_y - view->first_tile_y) & (kLevelMapRows - 1u);
  int relative_row = row >= kLevelMapRows / 2 ? (int)row - kLevelMapRows
                                              : (int)row;
  *x = (int)(tile_x * 8u) - (int)view->map_x;
  *y = relative_row * 8 - (int)view->fine_y;
}

void Dkc2NativeTerrainVisitBlocks(const Dkc2NativeTerrainView *view,
                                  Dkc2NativeBlockVisitor visit,
                                  void *context) {
  const uint32_t first_x = (view->map_x >> 3) & ~3u;
  const uint32_t first_y = view->first_tile_y & ~3u;
  for (uint32_t by = 0; by < (kDkc2NativeHeight + 8u) / 32u + 2u; by++) {
    for (uint32_t bx = 0; bx < kDkc2NativeWidth / 32u + 2u; bx++) {
      const uint32_t tile_x = first_x + bx * 4u;
      const uint32_t tile_y = (first_y + by * 4u) & (kLevelMapRows - 1u);
      int x = 0, y = 0;
      Dkc2NativeTerrainTileScreen(view, tile_x, tile_y, &x, &y);
      if (x >= kDkc2NativeWidth || y >= kDkc2NativeHeight ||
          x + kDkc2NativeBlockSize <= 0 || y + kDkc2NativeBlockSize <= 0)
        continue;
      visit(context, tile_x, tile_y, x, y);
    }
  }
}

void Dkc2NativeTerrainDecodeBlock(const Ppu *ppu,
                                  const Dkc2NativeTerrainView *view,
                                  uint32_t tile_x, uint32_t tile_y,
                                  uint32_t *pixels) {
  for (unsigned ty = 0; ty < 4; ty++) {
    for (unsigned tx = 0; tx < 4; tx++) {
      uint16_t entry = 0;
      const bool decoded = Dkc2VideoDecodeLevelTile(
          view->bank_data, 0x10000u, view->map_base, view->metatile_base,
          view->layout, tile_x + tx,
          (tile_y + ty) & (kLevelMapRows - 1u), &entry);
      const uint16_t character_word =
          (uint16_t)(view->character_base + (entry & 0x3ffu) * 16u);
      const uint8_t palette = (uint8_t)(((entry >> 10) & 7u) << 4);
      const uint8_t rank = (entry & 0x2000u) ? kBg1RankHigh : kBg1RankLow;
      for (unsigned py = 0; py < 8; py++) {
        const unsigned source_y = (entry & 0x8000u) ? 7u - py : py;
        uint32_t *row = pixels + (ty * 8u + py) * kDkc2NativeBlockSize +
                        tx * 8u;
        for (unsigned px = 0; px < 8; px++) {
          const unsigned source_x = (entry & 0x4000u) ? 7u - px : px;
          const uint8_t color =
              decoded ? Planar4bppVram(ppu->vram, character_word, source_x,
                                       source_y)
                      : 0;
          row[px] = color ? ColorWithRank(ppu, (uint8_t)(palette | color), rank)
                          : 0u;
        }
      }
    }
  }
}

unsigned Dkc2NativeCollectSprites(const uint8_t *ram, const uint8_t *rom,
                                  size_t rom_size,
                                  Dkc2NativeSprite sprites[kDkc2NativeSpriteMax]) {
  if (!ram || !rom || rom_size < kGraphicDirectory + kGraphicLimit + 3u)
    return 0;
  const int camera_x = Read16(ram, kCameraX);
  const int camera_y = Read16(ram, kCameraY);
  unsigned count = 0;
  /* The render-order table lists record addresses front to back (earlier
   * entries reach OAM first and win), so collect it in reverse. */
  for (unsigned index = kDkc2NativeSpriteMax; index-- > 0;) {
    const size_t record = Read16(ram, kSpriteRenderOrder + index * 2u);
    if (record < kSpriteTable ||
        record >= kSpriteTable + kDkc2NativeSpriteMax * kSpriteStride ||
        (record - kSpriteTable) % kSpriteStride != 0 ||
        Read16(ram, record) == 0)
      continue;
    const uint16_t graphic = Read16(ram, record + kSpriteGraphicOffset);
    if (graphic == 0 || graphic >= kGraphicLimit || (graphic & 3u))
      continue;
    const uint8_t *entry = rom + kGraphicDirectory + graphic;
    const size_t address =
        (size_t)(entry[0] | entry[1] << 8 | entry[2] << 16) & 0x3fffffu;
    if (address >= rom_size)
      continue;
    Dkc2NativeSprite *sprite = &sprites[count++];
    sprite->graphic = rom + address;
    sprite->available = rom_size - address;
    sprite->x = (int16_t)(Read16(ram, record + kSpriteWorldXOffset) -
                          camera_x) - 1;
    sprite->y = (int16_t)(Read16(ram, record + kSpriteWorldYOffset) -
                          camera_y);
    sprite->properties = Read16(ram, record + kSpritePropertyOffset);
    sprite->graphic_id = graphic;
    sprite->type = Read16(ram, record);
  }
  return count;
}

/* Decodes a compound graphic: large 16x16 pieces, then two groups of 8x8
 * pieces, followed by the 4-bpp payload uploaded in two DMA runs. Earlier
 * pieces take precedence where pieces overlap. */
bool Dkc2NativeDecodeSpriteFrame(const Ppu *ppu,
                                 const Dkc2NativeSprite *sprite,
                                 Dkc2NativeSpriteFrame *frame) {
  const uint8_t *data = sprite->graphic;
  if (!ppu || !data || sprite->available < 8)
    return false;
  const unsigned large = data[0], small = data[1], start = data[2];
  const unsigned extra = data[3], extra_start = data[4];
  const unsigned dma1 = data[5], dma2_start = data[6], dma2 = data[7];
  const unsigned count = large + small + extra;
  const size_t header = 8u + count * 2u;
  if (count == 0 || count > 64 ||
      header + (dma1 + dma2) * 32u > sprite->available)
    return false;

  int left = 1 << 16, top = 1 << 16, right = -(1 << 16), bottom = -(1 << 16);
  for (unsigned i = 0; i < count; i++) {
    const int size = i < large ? 16 : 8;
    const int offset_x = (int)data[8 + i * 2] - 128;
    const int offset_y = (int)data[9 + i * 2] - 128;
    if (offset_x < left) left = offset_x;
    if (offset_y < top) top = offset_y;
    if (offset_x + size > right) right = offset_x + size;
    if (offset_y + size > bottom) bottom = offset_y + size;
  }
  if (right - left > kDkc2NativeFrameMax || bottom - top > kDkc2NativeFrameMax)
    return false;
  frame->left = left;
  frame->top = top;
  frame->width = right - left;
  frame->height = bottom - top;
  memset(frame->pixels, 0,
         sizeof(uint32_t) * (size_t)frame->width * (size_t)frame->height);

  const uint8_t palette =
      (uint8_t)(0x80u | ((sprite->properties >> 9) & 7u) << 4);
  const uint8_t rank = (uint8_t)SPRITE_PRIO_TO_PRIO(
      (sprite->properties & 0x3000u) >> 12,
      (sprite->properties & 0x800u) == 0);
  for (unsigned i = count; i-- > 0;) {
    const int size = i < large ? 16 : 8;
    const int offset_x = (int)data[8 + i * 2] - 128 - left;
    const int offset_y = (int)data[9 + i * 2] - 128 - top;
    const unsigned base = i < large ? i / 8u * 32u + i % 8u * 2u
                          : i < large + small ? start + i - large
                                              : extra_start + i - large - small;
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const unsigned tile = base + (unsigned)(y / 8) * 16u +
                              (unsigned)(x / 8);
        unsigned payload_tile;
        if (tile < dma1)
          payload_tile = tile;
        else if (tile >= dma2_start && tile < dma2_start + dma2)
          payload_tile = dma1 + tile - dma2_start;
        else
          continue;
        const uint8_t color = Planar4bppBytes(
            data + header + payload_tile * 32u, (unsigned)x % 8u,
            (unsigned)y % 8u);
        if (color)
          frame->pixels[(offset_y + y) * frame->width + offset_x + x] =
              ColorWithRank(ppu, (uint8_t)(palette | color), rank);
      }
    }
  }
  return true;
}

void Dkc2NativeSpritePixelScreen(const Dkc2NativeSprite *sprite,
                                 const Dkc2NativeSpriteFrame *frame,
                                 int frame_x, int frame_y, int *x, int *y) {
  /* A flipped piece is placed at -offset - size and mirrored internally, so
   * relative pixel r lands at origin - 1 - r. */
  const int relative_x = frame->left + frame_x;
  const int relative_y = frame->top + frame_y;
  *x = (sprite->properties & 0x4000u) ? sprite->x - 1 - relative_x
                                      : sprite->x + relative_x;
  *y = (sprite->properties & 0x8000u) ? sprite->y - 1 - relative_y
                                      : sprite->y + relative_y;
}

uint64_t Dkc2NativeTerrainBlockKey(const Ppu *ppu,
                                   const Dkc2NativeTerrainView *view,
                                   uint32_t tile_x, uint32_t tile_y) {
  uint64_t hash = 14695981039346656037ull;
  bool opaque = false;
  for (unsigned ty = 0; ty < 4; ty++) {
    for (unsigned tx = 0; tx < 4; tx++) {
      uint16_t entry = 0;
      const bool decoded = Dkc2VideoDecodeLevelTile(
          view->bank_data, 0x10000u, view->map_base, view->metatile_base,
          view->layout, tile_x + tx,
          (tile_y + ty) & (kLevelMapRows - 1u), &entry);
      const uint16_t character_word =
          (uint16_t)(view->character_base + (entry & 0x3ffu) * 16u);
      /* Palette number and priority, then each pixel's color index in
       * screen order (flips applied). */
      const uint8_t attributes = (uint8_t)((entry >> 10) & 0xfu);
      for (unsigned py = 0; py < 8; py++) {
        const unsigned source_y = (entry & 0x8000u) ? 7u - py : py;
        for (unsigned px = 0; px < 8; px++) {
          const unsigned source_x = (entry & 0x4000u) ? 7u - px : px;
          const uint8_t color =
              decoded ? Planar4bppVram(ppu->vram, character_word, source_x,
                                       source_y)
                      : 0;
          opaque |= color != 0;
          hash = (hash ^ (uint64_t)(color ? attributes << 4 | color : 0)) *
                 1099511628211ull;
        }
      }
    }
  }
  return opaque ? (hash ? hash : 1) : 0;
}

typedef struct TerrainBlit {
  const Ppu *ppu;
  const Dkc2NativeTerrainView *view;
  uint32_t *pixels;
  size_t pitch;
} TerrainBlit;

static void BlitTerrainBlock(void *context, uint32_t tile_x, uint32_t tile_y,
                             int screen_x, int screen_y) {
  const TerrainBlit *blit = (const TerrainBlit *)context;
  uint32_t block[kDkc2NativeBlockSize * kDkc2NativeBlockSize];
  Dkc2NativeTerrainDecodeBlock(blit->ppu, blit->view, tile_x, tile_y, block);
  for (int y = 0; y < kDkc2NativeBlockSize; y++) {
    const int sy = screen_y + y;
    if (sy < 0 || sy >= kDkc2NativeHeight)
      continue;
    for (int x = 0; x < kDkc2NativeBlockSize; x++) {
      const int sx = screen_x + x;
      if (sx >= 0 && sx < kDkc2NativeWidth)
        blit->pixels[(size_t)sy * blit->pitch + (size_t)sx] =
            block[y * kDkc2NativeBlockSize + x];
    }
  }
}

bool Dkc2NativeRenderTerrain(const Ppu *ppu, const uint8_t *ram,
                             uint32_t *pixels, size_t pitch,
                             Dkc2NativeTerrainView *view) {
  if (!pixels || pitch < kDkc2NativeWidth ||
      !Dkc2NativeTerrainBegin(ppu, ram, view))
    return false;
  TerrainBlit blit = {ppu, view, pixels, pitch};
  Dkc2NativeTerrainVisitBlocks(view, BlitTerrainBlock, &blit);
  return true;
}

unsigned Dkc2NativeRenderSprites(const Ppu *ppu, const uint8_t *ram,
                                 const uint8_t *rom, size_t rom_size,
                                 uint32_t *pixels, size_t pitch) {
  if (!ppu || !pixels || pitch < kDkc2NativeWidth)
    return 0;
  Dkc2NativeSprite sprites[kDkc2NativeSpriteMax];
  const unsigned count =
      Dkc2NativeCollectSprites(ram, rom, rom_size, sprites);
  static Dkc2NativeSpriteFrame frame;
  unsigned drawn = 0;
  for (unsigned i = 0; i < count; i++) {
    if (!Dkc2NativeDecodeSpriteFrame(ppu, &sprites[i], &frame))
      continue;
    for (int fy = 0; fy < frame.height; fy++) {
      for (int fx = 0; fx < frame.width; fx++) {
        const uint32_t pixel = frame.pixels[fy * frame.width + fx];
        if (!pixel)
          continue;
        int x = 0, y = 0;
        Dkc2NativeSpritePixelScreen(&sprites[i], &frame, fx, fy, &x, &y);
        if (x >= 0 && x < kDkc2NativeWidth && y >= 0 &&
            y < kDkc2NativeHeight)
          pixels[(size_t)y * pitch + (size_t)x] = pixel;
      }
    }
    drawn++;
  }
  return drawn;
}
