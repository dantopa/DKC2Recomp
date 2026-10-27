#ifndef DKC2_HD_H
#define DKC2_HD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dkc2_hdma.h"
#include "snes/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HD presentation. Each frame is re-composed at an integer scale: the PPU
 * renders without objects while BG1-BG3 and OBJ are exported with their
 * composition ranks, then sprite-table objects are drawn at full output
 * resolution with the console's priority rules. Objects outside the sprite
 * table (single bananas, the HUD, text) are read from OAM: touching entries
 * of one priority that the frame shows form a loose object, drawn the same
 * way. Everything else (terrain) is scaled from the authentic frame, square
 * or smooth.
 *
 * A sprite frame's high-resolution image comes from, in order:
 *  - a pack file "<graphic>_p<palette>.rgba" (four hex digits, then eight:
 *    a hash of the palette's colors 1-15) in the pack directory, for
 *    externally produced art; the same art applies whichever palette slot
 *    the game assigns;
 *  - else a pack file "<graphic>_<slot>.rgba" (one digit: the palette slot);
 *  - the runtime filter applied to the ROM frame (no external data), or
 *    the learned filter when its table loads.
 *
 * Learned filter table: "DKLT", version 1, scale 4, pattern count 4, a
 * zero byte, then int8 logits [4 patterns][65536 windows][16 subpixels][5].
 * Each frame pixel gets the luminance rank of its color within the frame
 * (1-15, 0 transparent). For every cell of four pixels (top left at each
 * pixel and one before the first) each pattern packs the ranks of four
 * pixels around the cell, high nibble first, into its window; the four
 * patterns' logits are summed per subpixel (divided by 15.875) and a
 * softmax over them weights the cell's four premultiplied colors (the fifth
 * output is transparent). A cell's 4x4 subpixels start half a frame pixel
 * down and right of its top-left pixel. The image is made at 4x and drawn
 * like pack art; frames with more than 15 colors take the smooth filter.
 *
 * Pack and dump files: "DKHD", width and height as little-endian uint16,
 * then width * height straight-alpha RGBA8 pixels of the unflipped frame.
 * Pack images may use any size; they are sampled over the frame's bounds.
 * The dump writes "<graphic>_p<palette>.rgba" and appends
 * "<graphic> p<palette> <type> <width> <height>" (hex, "p" and hex, hex,
 * decimal, decimal) to index.txt for each frame it writes.
 *
 * Loose objects use "o_<key>.rgba" (sixteen hex digits: a hash of the
 * object's size and the raw color of every pixel, so it holds through fades
 * and palette slots), else the runtime filter. The dump writes them at full
 * brightness and appends "o <key> 0ffe <width> <height>" to index.txt.
 *
 * Still screens (title, menus): screens.txt in the pack lists 16-digit hex
 * keys; each has "s_<key>.ref", the native 256x224 BG1 it replaces, and
 * "s_<key>.rgba", art covering the native area. When the captured BG1
 * reproduces a reference the art replaces BG1 there, dimmed with the
 * screen brightness, under any object in front of it. With DKC2_HD_DUMP set,
 * a BG1 that stays unchanged for a second outside levels is written as
 * "s_<key>.ref".
 *
 * Background tiles (BG2, BG3), when the pack has backgrounds.txt (its
 * presence enables them): "b_<key>.rgba" (sixteen hex digits: a hash
 * of the raw color of every pixel of the unflipped 8x8 character) replaces
 * a cell wherever the layer shows it untouched by color math, flipped like
 * the tilemap entry and dimmed with the screen brightness. A cell must be
 * reproduced from VRAM with the registers its scanlines were drawn with;
 * widescreen margin cells take the character of a reproduced cell with the
 * same pixels. The dump writes each new character as "b_<key>.rgba".
 *
 * Background panoramas (needs the tile pass): panoramas.txt lists 16-digit
 * hex keys; each has "l_<key>.ref", cells of one layer as the dump writes
 * them to tiles.txt (" layer,x,y,key,flips"), and "l_<key>.rgba", art for
 * that layer's whole tilemap at an integer multiple of its size (tilemap
 * pixel x, y at art x * m, y * m). When at least 7/8 of the reproduced cells
 * a frame shows of the referenced ones match, the art replaces the layer's
 * cells: each run of scanlines drawn with the same registers shows the art
 * where its scroll puts it, so per-line scroll (parallax bands, water) is
 * kept. Lines that add the fixed color to the layer add it to the art too;
 * other color math leaves those lines native.
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
  /* Sprites only: tables learned offline (see below), one per style. */
  kDkc2HdFilterLearned = 2,
  kDkc2HdFilterLearnedPixel = 3,
} Dkc2HdFilter;

/* Learned filter tables, relative to the working directory (the user data
 * folder in the desktop host) unless DKC2_HD_FILTER_TABLE names one: the
 * smooth-art style, and the pixel-art style. */
#define kDkc2HdDefaultFilterTable "hdfilter/sprites_x4.lut"
#define kDkc2HdPixelFilterTable "hdfilter/sprites_pixel_x4.lut"

typedef struct Dkc2HdConfig {
  int scale;
  Dkc2HdFilter filter;      /* sprites without pack art */
  Dkc2HdFilter background;  /* the scaled authentic frame */
  const char *pack_dir;  /* optional */
  const char *dump_dir;  /* optional: writes each new 1x sprite frame */
  const char *filter_table;  /* overrides the learned filter's table */
} Dkc2HdConfig;

typedef struct Dkc2Hd Dkc2Hd;

/* Straight-alpha ARGB image owned by the compositor. The id is unique for
 * the process, so a renderer may keep an upload of it under that id. */
typedef struct Dkc2HdImage {
  uint64_t id;
  int width, height;
  uint32_t pixels[];
} Dkc2HdImage;

/* An image's source rectangle over the output rectangle (x, y, width,
 * height), sampled nearest: output column hx shows image column source_x +
 * hx * source_width / width, counted from the right edge when flipped (rows
 * likewise). A source_width of 0 stands for the whole image. */
typedef struct Dkc2HdQuad {
  const Dkc2HdImage *image;
  int x, y, width, height;
  int source_x, source_y, source_width, source_height;
  bool flip_x, flip_y;
  uint8_t brightness; /* scales the color; 255 leaves it */
  /* Sprites: composition rank. Tiles: layer + 1, plus kDkc2HdTileMath for
   * panorama lines that add the fixed color. */
  uint8_t rank;
  uint8_t index;      /* sprites: position in the draw order */
  /* Tiles: BGR555 color added to each channel (each 5-bit value expanded to
   * 8 bits) before the brightness; 0 adds nothing. */
  uint16_t add_color;
} Dkc2HdQuad;

/* Tile mask and tile rank flag: the layer's pixel had the fixed color added
 * by color math (only on layers a panorama replaces). */
enum { kDkc2HdTileMath = 0x80 };

/*
 * One frame decided at 1x, for an executor to draw at the output scale
 * (width * scale by height * scale): the CPU one behind Dkc2HdEndFrame, or a
 * GPU renderer. In order:
 *  1. base upscaled with the background filter (opaque), then tile quads
 *     blended over it, each output pixel drawn only where the tile mask of
 *     its 1x pixel equals the quad's rank (its layer + 1);
 *  2. when layer is set, the BG1 layer: layer upscaled the same way, art
 *     quads replacing its pixels (no blending), then blended over the output
 *     where occlusion alpha is set;
 *  3. sprite quads (records and loose objects, back to front) blended in
 *     order, each output pixel hidden when, for the occlusion R, G, B of its
 *     1x pixel, rank <= R, or G && G >= rank, or B && B - 1 > index.
 * Valid until the next frame call or Dkc2HdDestroy.
 */
typedef struct Dkc2HdScene {
  uint64_t serial; /* differs for every scene */
  int width, height, scale;
  Dkc2HdFilter background;
  const uint32_t *base;
  const Dkc2HdQuad *tiles;
  unsigned tile_count;
  /* Per pixel: the layer + 1 (BG2 or BG3) the base shows untouched, or 0;
   * with kDkc2HdTileMath where a panorama layer shows with the fixed color
   * added. */
  const uint8_t *tile_mask;
  /* Per pixel: R the highest background rank, G the rank of an object pixel
   * no quad draws, B the index + 1 of the quad owning an object pixel, alpha
   * 0xff where the BG1 layer is in front. */
  const uint32_t *occlusion;
  const uint32_t *layer; /* BG1, alpha 0xff where present; or NULL */
  const Dkc2HdQuad *art;
  unsigned art_count;
  const Dkc2HdQuad *sprites;
  unsigned sprite_count;
} Dkc2HdScene;

/* What the last composed frame shows at a pixel of the 1x frame (columns
 * counted from the frame's left edge, widescreen margins included). */
typedef struct Dkc2HdProbe {
  bool object;  /* a sprite quad draws there (the front-most one is read) */
  bool art;     /* that quad shows pack art, else the sprite filter */
  char name[24];  /* its pack file name, without ".rgba" */
  int x, y, width, height;  /* the quad in 1x frame pixels */
} Dkc2HdProbe;

/* Reads DKC2_HD=<scale>, DKC2_HD_FILTER=nearest|smooth|learned|learned-pixel (with
 * DKC2_HD_FILTER_TABLE), DKC2_HD_PACK and DKC2_HD_DUMP; the background is
 * smooth. False when DKC2_HD is unset;
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
 * bytes) and the registers its scanlines were drawn with (NULL: no tile
 * art). Returns the composed frame (width x height ARGB, tightly packed),
 * or NULL when the frame was rendered normally and should be presented as
 * is. */
const uint32_t *Dkc2HdEndFrame(Dkc2Hd *hd, const Ppu *ppu, const uint8_t *ram,
                               const Dkc2HdmaLines *lines,
                               const uint8_t *frame, size_t frame_pitch,
                               int *width, int *height);

/* The same frame as a scene for an external executor, or NULL as above. */
const Dkc2HdScene *Dkc2HdEndFrameScene(Dkc2Hd *hd, const Ppu *ppu,
                                       const uint8_t *ram,
                                       const Dkc2HdmaLines *lines,
                                       const uint8_t *frame,
                                       size_t frame_pitch);

/* Fills *probe for pixel (x, y) of the last composed frame. False when the
 * last frame was not composed or the pixel is outside it. */
bool Dkc2HdProbePixel(const Dkc2Hd *hd, int x, int y, Dkc2HdProbe *probe);

#ifdef __cplusplus
}
#endif

#endif
