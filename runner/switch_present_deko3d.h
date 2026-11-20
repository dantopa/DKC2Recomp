#ifndef DKC2_SWITCH_PRESENT_DEKO3D_H
#define DKC2_SWITCH_PRESENT_DEKO3D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "desktop_present_sdl.h"

/*
 * The Switch renderer on deko3d, the console's native graphics API, behind
 * the Dkc2SdlPresenter entry points (desktop_present_sdl.c delegates when
 * the presenter holds one). It owns the default window's swapchain, so no
 * SDL window or OpenGL context exists beside it, and the in-game overlay is
 * not drawn. Frames show flat (nearest or bilinear) over the viewport; HD
 * scenes are composed with the same three passes as the OpenGL presenter.
 */
typedef struct Dkc2Deko3d Dkc2Deko3d;

/* NULL when the device or swapchain cannot be made, with the reason in
 * error. Draw calls and uploads are added to stats. */
Dkc2Deko3d *Dkc2Deko3dCreate(Dkc2SdlPresenterGlStats *stats, char *error,
                             size_t error_capacity);
void Dkc2Deko3dDestroy(Dkc2Deko3d *deko);
void Dkc2Deko3dOutputSize(const Dkc2Deko3d *deko, int *width, int *height);

/* A one-shot readback of the presented image (RGB, row 0 at the top) into
 * rgb, when width and height match the output size; *done is set once it
 * is filled. */
typedef struct Dkc2Deko3dCapture {
  uint8_t *rgb;
  int width;
  int height;
  bool *done;
} Dkc2Deko3dCapture;

/* Presents a BGRA frame (rows of width pixels). */
bool Dkc2Deko3dPresent(Dkc2Deko3d *deko, const uint8_t *pixels, int width,
                       int height, bool linear_filter,
                       const Dkc2Deko3dCapture *capture);
/* Presents an HD scene, composing it only when it is a new scene. */
bool Dkc2Deko3dPresentHd(Dkc2Deko3d *deko, const Dkc2HdScene *scene,
                         bool linear_filter, const Dkc2Deko3dCapture *capture);

#endif
