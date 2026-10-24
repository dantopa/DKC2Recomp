#ifndef DKC2_HD_H
#define DKC2_HD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snes/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HD presentation. Each frame is re-composed at an integer scale: the PPU
 * renders without objects while BG1-BG3 and OBJ are exported with their
 * composition ranks, then sprite-table objects are drawn at full output
 * resolution with the console's priority rules. Everything else (terrain,
 * HUD, loose objects) is scaled from the authentic frame, square or smooth.
 *
 * A sprite frame's high-resolution image comes from, in order:
 *  - a pack file "<graphic>_<palette>.rgba" (four hex digits, one digit) in
 *    the pack directory, for externally produced art;
 *  - the runtime filter applied to the ROM frame (no external data).
 *
 * Pack and dump files: "DKHD", width and height as little-endian uint16,
 * then width * height straight-alpha RGBA8 pixels of the unflipped frame.
 * Pack images may use any size; they are sampled over the frame's bounds.
 * The dump also appends "<graphic> <palette> <type> <width> <height>" (hex,
 * hex, hex, decimal, decimal) to index.txt for each frame it writes.
 *
 * Still screens (title, menus): screens.txt in the pack lists 16-digit hex
 * keys; each has "s_<key>.ref", the native 256x224 BG1 it replaces, and
 * "s_<key>.rgba", art covering the native area. When the captured BG1
 * reproduces a reference the art replaces BG1 there, dimmed with the
 * screen brightness, under any object in front of it. With DKC2_HD_DUMP set,
 * a BG1 that stays unchanged for a second outside levels is written as
 * "s_<key>.ref".
 *
 * Works at any frame width the PPU renders (widescreen margins included).
 * Limits: Mode 1 scenes only, and object color math is not reproduced.
 */
enum {
  kDkc2HdMinScale = 2,
  kDkc2HdMaxScale = 8,
};

typedef enum Dkc2HdFilter {
  kDkc2HdFilterNearest = 0,
  kDkc2HdFilterSmooth = 1,
} Dkc2HdFilter;

typedef struct Dkc2HdConfig {
  int scale;
  Dkc2HdFilter filter;      /* sprites without pack art */
  Dkc2HdFilter background;  /* the scaled authentic frame */
  const char *pack_dir;  /* optional */
  const char *dump_dir;  /* optional: writes each new 1x sprite frame */
} Dkc2HdConfig;

typedef struct Dkc2Hd Dkc2Hd;

/* Straight-alpha ARGB image owned by the compositor. The id is unique for
 * the process, so a renderer may keep an upload of it under that id. */
typedef struct Dkc2HdImage {
  uint64_t id;
  int width, height;
  uint32_t pixels[];
} Dkc2HdImage;

/* An image over the output rectangle (x, y, width, height), sampled nearest:
 * output column hx shows image column hx * image->width / width, counted
 * from the right edge when flipped (rows likewise). */
typedef struct Dkc2HdQuad {
  const Dkc2HdImage *image;
  int x, y, width, height;
  bool flip_x, flip_y;
  uint8_t brightness; /* scales the color; 255 leaves it */
  uint8_t rank;       /* sprites: composition rank */
  uint8_t index;      /* sprites: sprite-table order */
} Dkc2HdQuad;

/*
 * One frame decided at 1x, for an executor to draw at the output scale
 * (width * scale by height * scale): the CPU one behind Dkc2HdEndFrame, or a
 * GPU renderer. In order:
 *  1. base upscaled with the background filter (opaque);
 *  2. when layer is set, the BG1 layer: layer upscaled the same way, art
 *     quads replacing its pixels (no blending), then blended over the output
 *     where occlusion alpha is set;
 *  3. sprite quads blended in order, each output pixel hidden when, for
 *     the occlusion R, G, B of its 1x pixel, rank <= R, or G && G >= rank,
 *     or B && B - 1 > index.
 * Valid until the next frame call or Dkc2HdDestroy.
 */
typedef struct Dkc2HdScene {
  uint64_t serial; /* differs for every scene */
  int width, height, scale;
  Dkc2HdFilter background;
  const uint32_t *base;
  /* Per pixel: R the highest background rank, G the rank of an object pixel
   * outside the sprite table, B the index + 1 of the sprite-table record
   * owning an object pixel, alpha 0xff where the BG1 layer is in front. */
  const uint32_t *occlusion;
  const uint32_t *layer; /* BG1, alpha 0xff where present; or NULL */
  const Dkc2HdQuad *art;
  unsigned art_count;
  const Dkc2HdQuad *sprites;
  unsigned sprite_count;
} Dkc2HdScene;

/* Reads DKC2_HD=<scale>, DKC2_HD_FILTER=nearest|smooth, DKC2_HD_PACK and
 * DKC2_HD_DUMP; the background is smooth. False when DKC2_HD is unset;
 * *error is set when it is present but invalid. */
bool Dkc2HdConfigFromEnvironment(Dkc2HdConfig *config, const char **error);

Dkc2Hd *Dkc2HdCreate(const Dkc2HdConfig *config, const uint8_t *rom,
                     size_t rom_size);
void Dkc2HdDestroy(Dkc2Hd *hd);

/* Call before Dkc2DrawPpuFrame with the width it renders (256 plus both
 * widescreen margins). Routes the frame through the HD path when the scene
 * is supported; otherwise leaves the PPU untouched. */
void Dkc2HdBeginFrame(Dkc2Hd *hd, Ppu *ppu, int frame_width);

/* Call after Dkc2DrawPpuFrame with the BGRA frame it rendered (pitch in
 * bytes). Returns the composed frame (width x height ARGB, tightly packed),
 * or NULL when the frame was rendered normally and should be presented as
 * is. */
const uint32_t *Dkc2HdEndFrame(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram,
                               const uint8_t *frame, size_t frame_pitch,
                               int *width, int *height);

/* The same frame as a scene for an external executor, or NULL as above. */
const Dkc2HdScene *Dkc2HdEndFrameScene(Dkc2Hd *hd, const Ppu *ppu,
                                       const uint8_t *ram,
                                       const uint8_t *frame,
                                       size_t frame_pitch);

#ifdef __cplusplus
}
#endif

#endif
