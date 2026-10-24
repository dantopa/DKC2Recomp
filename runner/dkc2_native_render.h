#ifndef DKC2_NATIVE_RENDER_H
#define DKC2_NATIVE_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dkc2_video.h"
#include "snes/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Semantic renderer. It draws DKC2 terrain and live game sprites from game
 * state (decompressed level map, sprite table, ROM graphics directory,
 * palettes) instead of from the PPU's streamed tilemaps and OAM.
 *
 * Pixels are ARGB with the PPU composition rank in the alpha byte (the same
 * encoding as the DKC2 build's overlay captures); 0 is transparent. Terrain
 * is produced as 32x32 level-map blocks and sprites as whole compound frames
 * so higher-resolution replacements can be keyed and drawn per asset.
 *
 * Scope: horizontal-layout Mode 1 gameplay terrain on BG1 and the 25-slot
 * sprite table, at the authentic 256-column geometry.
 */
enum {
  kDkc2NativeWidth = 256,
  kDkc2NativeHeight = 224,
  kDkc2NativeBlockSize = 32,
  kDkc2NativeSpriteMax = 25,
  kDkc2NativeFrameMax = 128,
};

typedef struct Dkc2NativeTerrainView {
  const uint8_t *bank_data;
  Dkc2VideoLevelLayout layout;
  uint16_t map_base;
  uint16_t metatile_base;
  uint16_t character_base;
  uint32_t map_x;         /* level-map pixel at screen column 0 */
  uint32_t first_tile_y;  /* level-map tile row containing screen row 0 */
  unsigned fine_y;        /* screen row 0 is this many pixels into that row */
  uint16_t world_x;       /* BG1 world column at screen column 0 */
} Dkc2NativeTerrainView;

/* Resolves the frame's terrain view. False outside the supported scene. */
bool Dkc2NativeTerrainBegin(const Ppu *ppu, const uint8_t *ram,
                            Dkc2NativeTerrainView *view);

/* Screen position of a level-map tile's top-left pixel. */
void Dkc2NativeTerrainTileScreen(const Dkc2NativeTerrainView *view,
                                 uint32_t tile_x, uint32_t tile_y, int *x,
                                 int *y);

/* Calls draw(block_tile_x, block_tile_y, screen_x, screen_y) for every
 * 32x32 block (aligned to 4x4 level-map tiles) that overlaps the screen. */
typedef void (*Dkc2NativeBlockVisitor)(void *context, uint32_t tile_x,
                                       uint32_t tile_y, int screen_x,
                                       int screen_y);
void Dkc2NativeTerrainVisitBlocks(const Dkc2NativeTerrainView *view,
                                  Dkc2NativeBlockVisitor visit,
                                  void *context);

/* Decodes one 32x32 block whose top-left level-map tile is (tile_x, tile_y)
 * into a 32x32 ARGB+rank image. */
void Dkc2NativeTerrainDecodeBlock(const Ppu *ppu,
                                  const Dkc2NativeTerrainView *view,
                                  uint32_t tile_x, uint32_t tile_y,
                                  uint32_t *pixels);

/* Identifies a block by what it draws (character pixels, palette numbers and
 * priorities), independent of palette colors and screen brightness. 0 for a
 * block with no opaque pixel. */
uint64_t Dkc2NativeTerrainBlockKey(const Ppu *ppu,
                                   const Dkc2NativeTerrainView *view,
                                   uint32_t tile_x, uint32_t tile_y);

/* The tilemap entry a background shows at map pixel (map_x, map_y), scroll
 * applied, addressed like the PPU from the layer's BGnSC value. */
uint16_t Dkc2NativeBgMapEntry(const Ppu *ppu, uint8_t bg_sc, unsigned map_x,
                              unsigned map_y);

/* CGRAM index (0: transparent) of each pixel of a tilemap entry's unflipped
 * 8x8 character, 4 bpp (Mode 1 BG1 and BG2) or 2 bpp (BG3); character_base
 * is the layer's character base in VRAM words. */
void Dkc2NativeDecodeBgCharacter(const Ppu *ppu, unsigned bpp,
                                 uint16_t character_base, uint16_t entry,
                                 uint8_t indices[64]);

typedef struct Dkc2NativeSprite {
  const uint8_t *graphic;  /* compound graphic in the ROM image */
  size_t available;        /* bytes readable from graphic */
  int x, y;                /* screen origin */
  uint16_t properties;     /* OAM property word: flips, priority, palette */
  uint16_t graphic_id;
  uint16_t type;           /* record type word: groups a character's frames */
} Dkc2NativeSprite;

/* Collects live sprite-table records back to front (draw order). Returns the
 * count. ram must be the state the displayed OAM was built from. */
unsigned Dkc2NativeCollectSprites(const uint8_t *ram, const uint8_t *rom,
                                  size_t rom_size,
                                  Dkc2NativeSprite sprites[kDkc2NativeSpriteMax]);

/* An unflipped compound frame. Pixel (0,0) lies at (left, top) relative to
 * the sprite origin. */
typedef struct Dkc2NativeSpriteFrame {
  int left, top;
  int width, height;
  uint32_t pixels[kDkc2NativeFrameMax * kDkc2NativeFrameMax];
} Dkc2NativeSpriteFrame;

bool Dkc2NativeDecodeSpriteFrame(const Ppu *ppu,
                                 const Dkc2NativeSprite *sprite,
                                 Dkc2NativeSpriteFrame *frame);

/* Screen position of an unflipped frame pixel (frame_x, frame_y), honoring
 * the sprite's flips. */
void Dkc2NativeSpritePixelScreen(const Dkc2NativeSprite *sprite,
                                 const Dkc2NativeSpriteFrame *frame,
                                 int frame_x, int frame_y, int *x, int *y);

/* One OAM entry as the PPU draws it: screen position (X after the
 * widescreen adjustments, so frame column = x + the left margin; Y signed,
 * so lines below 0 are above the screen), size and its second OAM word
 * (tile, palette, priority, flips). */
typedef struct Dkc2NativeOamEntry {
  int x, y, size;
  uint16_t attributes;
} Dkc2NativeOamEntry;

enum { kDkc2NativeOamEntries = 128 };

void Dkc2NativeDecodeOamEntry(const Ppu *ppu, unsigned slot,
                              Dkc2NativeOamEntry *entry);

/* Color index (0 transparent) of entry pixel (x, y), counted from the
 * entry's top left on screen (flips applied). */
uint8_t Dkc2NativeOamPixel(const Ppu *ppu, const Dkc2NativeOamEntry *entry,
                           int x, int y);

/* 1x reference renderers used by the PPU comparison harness. */
bool Dkc2NativeRenderTerrain(const Ppu *ppu, const uint8_t *ram,
                             uint32_t *pixels, size_t pitch,
                             Dkc2NativeTerrainView *view);
unsigned Dkc2NativeRenderSprites(const Ppu *ppu, const uint8_t *ram,
                                 const uint8_t *rom, size_t rom_size,
                                 uint32_t *pixels, size_t pitch);

#ifdef __cplusplus
}
#endif

#endif
