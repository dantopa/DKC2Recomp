#include "switch_present_deko3d.h"

#include <switch.h>
#include <deko3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop_viewport.h"
#include "dkc2_hd.h"
#include "flat_fsh_dksh.h"
#include "hd_composite_fsh_dksh.h"
#include "hd_quad_fsh_dksh.h"
#include "hd_upscale_fsh_dksh.h"
#include "rect_vsh_dksh.h"

/*
 * Frames are recorded into one command buffer, a region of command memory
 * per frame in flight; a frame's fence guards its region and its staging
 * buffer. The output is sized once, at start, by the dock state. The device
 * keeps deko3d's upper-left origin: fragment row 0,
 * memory row 0 and texture coordinate 0 are the top row everywhere, so the
 * OpenGL passes (whose targets keep row 0 at coordinate 0) port unchanged
 * and the presented image needs no flip.
 */
enum {
  kFrames = 3, /* swapchain images, and frames recorded ahead */
  kCmdMemSize = 2u << 20,
  kStagingSize = 16u << 20,
  kPoolBlockSize = 32u << 20,
  kPoolBlocks = 24, /* HD pack images: at most 768 MB */
  kCodeMemSize = 64u << 10,
  kUboSize = DK_UNIFORM_BUF_ALIGNMENT,
  kStagingAlignment = 256,
  /* Image descriptors: HD images by table slot, then the fixed images. */
  kDescriptorSource = kDkc2HdImageSlots,
  kDescriptorInput = kDescriptorSource + kFrames,
  kDescriptorOutput = kDescriptorInput + kFrames * 4,
  kDescriptorLayer = kDescriptorOutput + 1,
  kDescriptorCount = kDescriptorLayer + 1,
  kSamplerNearest = 0,
  kSamplerLinear = 1,
  kSamplerCount = 2,
};

/* The 1x scene inputs, as the OpenGL presenter numbers them. */
enum { kInputBase = 0, kInputOcclusion = 1, kInputLayer = 2, kInputTiles = 3 };

/* rect_vsh.glsl's uniform block. */
typedef struct DekoDraw {
  float rect[4];
  float target[4];
  float uv[4];
  float source[4];
  float add[4];
  float flip[4];
  float native[4];
  float order[4];
} DekoDraw;

typedef struct DekoTexture {
  DkMemBlock memory;
  DkImage image;
  DkImageView view;
  int width;
  int height;
} DekoTexture;

typedef struct DekoHdImage {
  uint64_t id; /* 0: free slot */
  uint64_t serial; /* last scene that used it */
  DkImage image;
} DekoHdImage;

struct Dkc2Deko3d {
  Dkc2SdlPresenterGlStats *stats;
  /* 1920x1080 docked, 1280x720 handheld, as SDL sizes its window. */
  int output_width, output_height;
  char *error;
  size_t error_capacity;
  DkDevice device;
  DkQueue queue;
  DkMemBlock swapchain_memory;
  DkImage swapchain_images[kFrames];
  DkSwapchain swapchain;
  DkMemBlock code_memory;
  DkShader rect_vsh, flat_fsh, upscale_fsh, quad_fsh, composite_fsh;
  /* Descriptors and the uniform block, CPU-written. */
  DkMemBlock data_memory;
  DkImageDescriptor *image_descriptors;
  DkGpuAddr ubo;
  DkMemBlock cmd_memory;
  DkCmdBuf cmdbuf;
  DkMemBlock staging_memory;
  uint32_t staging_used;
  DkFence fences[kFrames];
  bool fence_pending[kFrames];
  unsigned frame;
  DekoTexture source[kFrames];
  DekoTexture inputs[kFrames][4];
  DekoTexture hd_output;
  DekoTexture hd_layer;
  uint64_t hd_serial;
  int hd_width, hd_height; /* the composed scene's output size */
  DkMemBlock pool[kPoolBlocks];
  unsigned pool_created, pool_current;
  uint32_t pool_used;
  DekoHdImage *hd_images; /* kDkc2HdImageSlots */
  unsigned hd_image_count;
  DkMemBlock capture_memory;
};

static uint32_t Align(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static void DebugCallback(void *user, const char *context, DkResult result,
                          const char *message) {
  Dkc2Deko3d *deko = (Dkc2Deko3d *)user;
  fprintf(stderr, "deko3d: %s: %s (%d)\n", context ? context : "?",
          message ? message : "", (int)result);
  if (deko && deko->error && !deko->error[0])
    (void)snprintf(deko->error, deko->error_capacity, "deko3d %s: %s",
                   context ? context : "?", message ? message : "");
}

static DkMemBlock MakeMemory(DkDevice device, uint32_t size, uint32_t flags) {
  DkMemBlockMaker maker;
  dkMemBlockMakerDefaults(&maker, device, Align(size, DK_MEMBLOCK_ALIGNMENT));
  maker.flags = flags;
  return dkMemBlockCreate(&maker);
}

static void MakeLayout(DkDevice device, DkImageFormat format, uint32_t flags,
                       int width, int height, DkImageLayout *layout) {
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, device);
  maker.flags = flags;
  maker.format = format;
  maker.dimensions[0] = (uint32_t)width;
  maker.dimensions[1] = (uint32_t)height;
  dkImageLayoutInitialize(layout, &maker);
}

static void TextureDestroy(DekoTexture *texture) {
  if (texture->memory) dkMemBlockDestroy(texture->memory);
  memset(texture, 0, sizeof *texture);
}

/* A texture in its own memory, described at descriptor. */
static bool TextureCreate(Dkc2Deko3d *deko, DekoTexture *texture,
                          DkImageFormat format, uint32_t flags, int width,
                          int height, uint32_t descriptor) {
  DkImageLayout layout;
  MakeLayout(deko->device, format, flags, width, height, &layout);
  texture->memory =
      MakeMemory(deko->device, (uint32_t)dkImageLayoutGetSize(&layout),
                 DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!texture->memory) return false;
  dkImageInitialize(&texture->image, &layout, texture->memory, 0);
  dkImageViewDefaults(&texture->view, &texture->image);
  dkImageDescriptorInitialize(&deko->image_descriptors[descriptor],
                              &texture->view, false, false);
  texture->width = width;
  texture->height = height;
  return true;
}

static void LoadShader(Dkc2Deko3d *deko, DkShader *shader, uint32_t *offset,
                       const void *code, uint32_t size) {
  memcpy((uint8_t *)dkMemBlockGetCpuAddr(deko->code_memory) + *offset, code,
         size);
  DkShaderMaker maker;
  dkShaderMakerDefaults(&maker, deko->code_memory, *offset);
  dkShaderInitialize(shader, &maker);
  *offset += Align(size, DK_SHADER_CODE_ALIGNMENT);
}

/* Starts recording the current frame's commands: its command memory and
 * staging buffer are free, and the state every pass shares is bound. */
static void BeginCommands(Dkc2Deko3d *deko) {
  const unsigned slot = deko->frame % kFrames;
  dkCmdBufClear(deko->cmdbuf);
  dkCmdBufAddMemory(deko->cmdbuf, deko->cmd_memory, slot * kCmdMemSize,
                    kCmdMemSize);
  deko->staging_used = 0;
  const DkGpuAddr data = dkMemBlockGetGpuAddr(deko->data_memory);
  dkCmdBufBindImageDescriptorSet(deko->cmdbuf, data, kDescriptorCount);
  dkCmdBufBindSamplerDescriptorSet(
      deko->cmdbuf, data + kDescriptorCount * sizeof(DkImageDescriptor),
      kSamplerCount);
  dkCmdBufBindUniformBuffer(deko->cmdbuf, DkStage_Vertex, 0, deko->ubo,
                            kUboSize);
  dkCmdBufBindUniformBuffer(deko->cmdbuf, DkStage_Fragment, 0, deko->ubo,
                            kUboSize);
  DkRasterizerState rasterizer;
  dkRasterizerStateDefaults(&rasterizer);
  rasterizer.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(deko->cmdbuf, &rasterizer);
  DkDepthStencilState depth;
  dkDepthStencilStateDefaults(&depth);
  depth.depthTestEnable = false;
  depth.depthWriteEnable = false;
  dkCmdBufBindDepthStencilState(deko->cmdbuf, &depth);
  /* The OpenGL presenter's glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA). */
  DkBlendState blend;
  dkBlendStateDefaults(&blend);
  dkBlendStateSetOps(&blend, DkBlendOp_Add, DkBlendOp_Add);
  dkBlendStateSetFactors(&blend, DkBlendFactor_SrcAlpha,
                         DkBlendFactor_InvSrcAlpha, DkBlendFactor_SrcAlpha,
                         DkBlendFactor_InvSrcAlpha);
  dkCmdBufBindBlendState(deko->cmdbuf, 0, &blend);
  dkCmdBufBindVtxAttribState(deko->cmdbuf, NULL, 0);
  dkCmdBufBindVtxBufferState(deko->cmdbuf, NULL, 0);
}

static void BeginFrame(Dkc2Deko3d *deko) {
  const unsigned slot = deko->frame % kFrames;
  if (deko->fence_pending[slot]) {
    (void)dkFenceWait(&deko->fences[slot], -1);
    deko->fence_pending[slot] = false;
  }
  BeginCommands(deko);
}

/* Runs what is recorded and waits for the GPU, then starts recording again.
 * Only between uploads: pass state does not carry across. */
static void FlushAndWait(Dkc2Deko3d *deko) {
  dkQueueSubmitCommands(deko->queue, dkCmdBufFinishList(deko->cmdbuf));
  dkQueueWaitIdle(deko->queue);
  BeginCommands(deko);
}

/* Records a copy of pixels into the image through the frame's staging
 * buffer, flushing once when it is full. */
static bool Upload(Dkc2Deko3d *deko, const DkImageView *view,
                   const void *pixels, int width, int height,
                   unsigned bytes_per_pixel) {
  const uint32_t size =
      (uint32_t)width * (uint32_t)height * bytes_per_pixel;
  if (size > kStagingSize) return false;
  uint32_t offset = Align(deko->staging_used, kStagingAlignment);
  if (offset + size > kStagingSize) {
    FlushAndWait(deko);
    offset = 0;
  }
  const uint32_t base = (deko->frame % kFrames) * kStagingSize + offset;
  memcpy((uint8_t *)dkMemBlockGetCpuAddr(deko->staging_memory) + base, pixels,
         size);
  const DkCopyBuf source = {
      dkMemBlockGetGpuAddr(deko->staging_memory) + base, 0, 0};
  const DkImageRect rect = {0, 0, 0, (uint32_t)width, (uint32_t)height, 1};
  dkCmdBufCopyBufferToImage(deko->cmdbuf, &source, view, &rect, 0);
  deko->staging_used = offset + size;
  deko->stats->uploads++;
  deko->stats->upload_bytes += size;
  return true;
}

static void BindTarget(Dkc2Deko3d *deko, const DkImageView *view, int width,
                       int height) {
  dkCmdBufBindRenderTarget(deko->cmdbuf, view, NULL);
  const DkViewport viewport = {0.0f, 0.0f, (float)width, (float)height,
                               0.0f, 1.0f};
  const DkScissor scissor = {0, 0, (uint32_t)width, (uint32_t)height};
  dkCmdBufSetViewports(deko->cmdbuf, 0, &viewport, 1);
  dkCmdBufSetScissors(deko->cmdbuf, 0, &scissor, 1);
}

static void BindPass(Dkc2Deko3d *deko, const DkShader *fragment, bool blend,
                     uint32_t write_mask) {
  const DkShader *shaders[] = {&deko->rect_vsh, fragment};
  dkCmdBufBindShaders(deko->cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
  DkColorState color;
  dkColorStateDefaults(&color);
  dkColorStateSetBlendEnable(&color, 0, blend);
  dkCmdBufBindColorState(deko->cmdbuf, &color);
  DkColorWriteState write;
  dkColorWriteStateDefaults(&write);
  dkColorWriteStateSetMask(&write, 0, write_mask);
  dkCmdBufBindColorWriteState(deko->cmdbuf, &write);
}

static void BindTexture(Dkc2Deko3d *deko, uint32_t binding,
                        uint32_t descriptor, uint32_t sampler) {
  dkCmdBufBindTexture(deko->cmdbuf, DkStage_Fragment, binding,
                      dkMakeTextureHandle(descriptor, sampler));
}

static void Draw(Dkc2Deko3d *deko, const DekoDraw *draw) {
  dkCmdBufPushConstants(deko->cmdbuf, deko->ubo, kUboSize, 0, sizeof *draw,
                        draw);
  dkCmdBufDraw(deko->cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
  deko->stats->draws++;
}

/* A draw over the whole width by height target. */
static DekoDraw WholeTarget(int width, int height) {
  DekoDraw draw;
  memset(&draw, 0, sizeof draw);
  draw.rect[2] = draw.target[0] = (float)width;
  draw.rect[3] = draw.target[1] = (float)height;
  draw.uv[2] = draw.uv[3] = 1.0f;
  return draw;
}

/* Shows the image at descriptor (width by height) over the viewport, then
 * submits and presents the frame. */
static bool Show(Dkc2Deko3d *deko, uint32_t descriptor, int width,
                 int height, bool linear_filter,
                 const Dkc2Deko3dCapture *capture) {
  const unsigned slot = deko->frame % kFrames;
  Dkc2DesktopViewport viewport;
  const bool visible = Dkc2DesktopComputeViewport(
      deko->output_width, deko->output_height, width, height, &viewport);
  const int image = dkQueueAcquireImage(deko->queue, deko->swapchain);
  DkImageView view;
  dkImageViewDefaults(&view, &deko->swapchain_images[image]);
  BindTarget(deko, &view, deko->output_width, deko->output_height);
  dkCmdBufClearColorFloat(deko->cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f,
                          0.0f, 1.0f);
  if (visible) {
    BindPass(deko, &deko->flat_fsh, false, DkColorMask_RGBA);
    BindTexture(deko, 0, descriptor,
                linear_filter ? kSamplerLinear : kSamplerNearest);
    DekoDraw draw = WholeTarget(deko->output_width, deko->output_height);
    draw.rect[0] = (float)viewport.x;
    draw.rect[1] = (float)viewport.y;
    draw.rect[2] = (float)viewport.width;
    draw.rect[3] = (float)viewport.height;
    Draw(deko, &draw);
  }
  const bool capturing =
      capture && capture->rgb && capture->done && !*capture->done &&
      capture->width == deko->output_width &&
      capture->height == deko->output_height && deko->capture_memory;
  if (capturing) {
    dkCmdBufBarrier(deko->cmdbuf, DkBarrier_Full, 0);
    const DkImageRect rect = {0, 0, 0, (uint32_t)deko->output_width,
                              (uint32_t)deko->output_height, 1};
    const DkCopyBuf target = {dkMemBlockGetGpuAddr(deko->capture_memory), 0,
                              0};
    dkCmdBufCopyImageToBuffer(deko->cmdbuf, &view, &rect, &target, 0);
  }
  dkCmdBufSignalFence(deko->cmdbuf, &deko->fences[slot], true);
  deko->fence_pending[slot] = true;
  dkQueueSubmitCommands(deko->queue, dkCmdBufFinishList(deko->cmdbuf));
  dkQueuePresentImage(deko->queue, deko->swapchain, image);
  deko->frame++;
  if (capturing) {
    dkQueueWaitIdle(deko->queue);
    const uint8_t *rgba =
        (const uint8_t *)dkMemBlockGetCpuAddr(deko->capture_memory);
    const size_t pixels =
        (size_t)deko->output_width * (size_t)deko->output_height;
    for (size_t i = 0; i < pixels; i++) {
      capture->rgb[i * 3u] = rgba[i * 4u];
      capture->rgb[i * 3u + 1u] = rgba[i * 4u + 1u];
      capture->rgb[i * 3u + 2u] = rgba[i * 4u + 2u];
    }
    *capture->done = true;
  }
  return true;
}

Dkc2Deko3d *Dkc2Deko3dCreate(Dkc2SdlPresenterGlStats *stats, char *error,
                             size_t error_capacity) {
  if (error && error_capacity) error[0] = '\0';
  Dkc2Deko3d *deko = (Dkc2Deko3d *)calloc(1, sizeof *deko);
  DekoHdImage *hd_images =
      (DekoHdImage *)calloc(kDkc2HdImageSlots, sizeof *hd_images);
  if (!deko || !hd_images) {
    free(deko);
    free(hd_images);
    if (error) (void)snprintf(error, error_capacity, "out of memory");
    return NULL;
  }
  deko->stats = stats;
  deko->error = error;
  deko->error_capacity = error_capacity;
  deko->hd_images = hd_images;
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  deko->output_width = docked ? 1920 : 1280;
  deko->output_height = docked ? 1080 : 720;

  DkDeviceMaker device_maker;
  dkDeviceMakerDefaults(&device_maker);
  device_maker.userData = deko;
  device_maker.cbDebug = DebugCallback;
  deko->device = dkDeviceCreate(&device_maker);
  if (!deko->device) goto fail;

  /* Present images are not compressed, so a capture can copy them. */
  DkImageLayout layout;
  MakeLayout(deko->device, DkImageFormat_RGBA8_Unorm,
             DkImageFlags_UsageRender | DkImageFlags_UsagePresent,
             deko->output_width, deko->output_height, &layout);
  const uint32_t image_size =
      Align((uint32_t)dkImageLayoutGetSize(&layout),
            dkImageLayoutGetAlignment(&layout));
  deko->swapchain_memory =
      MakeMemory(deko->device, kFrames * image_size,
                 DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!deko->swapchain_memory) goto fail;
  const DkImage *images[kFrames];
  for (unsigned i = 0; i < kFrames; i++) {
    dkImageInitialize(&deko->swapchain_images[i], &layout,
                      deko->swapchain_memory, i * image_size);
    images[i] = &deko->swapchain_images[i];
  }
  DkSwapchainMaker swapchain_maker;
  dkSwapchainMakerDefaults(&swapchain_maker, deko->device, nwindowGetDefault(),
                           images, kFrames);
  deko->swapchain = dkSwapchainCreate(&swapchain_maker);
  if (!deko->swapchain) goto fail;

  deko->code_memory = MakeMemory(deko->device, kCodeMemSize,
                                 DkMemBlockFlags_CpuUncached |
                                     DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Code);
  if (!deko->code_memory) goto fail;
  uint32_t code_offset = 0;
  LoadShader(deko, &deko->rect_vsh, &code_offset, rect_vsh_dksh,
             rect_vsh_dksh_size);
  LoadShader(deko, &deko->flat_fsh, &code_offset, flat_fsh_dksh,
             flat_fsh_dksh_size);
  LoadShader(deko, &deko->upscale_fsh, &code_offset, hd_upscale_fsh_dksh,
             hd_upscale_fsh_dksh_size);
  LoadShader(deko, &deko->quad_fsh, &code_offset, hd_quad_fsh_dksh,
             hd_quad_fsh_dksh_size);
  LoadShader(deko, &deko->composite_fsh, &code_offset,
             hd_composite_fsh_dksh, hd_composite_fsh_dksh_size);

  const uint32_t sampler_offset =
      kDescriptorCount * (uint32_t)sizeof(DkImageDescriptor);
  const uint32_t ubo_offset = Align(
      sampler_offset + kSamplerCount * (uint32_t)sizeof(DkSamplerDescriptor),
      DK_UNIFORM_BUF_ALIGNMENT);
  deko->data_memory =
      MakeMemory(deko->device, ubo_offset + kUboSize,
                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!deko->data_memory) goto fail;
  uint8_t *data = (uint8_t *)dkMemBlockGetCpuAddr(deko->data_memory);
  deko->image_descriptors = (DkImageDescriptor *)data;
  DkSamplerDescriptor *samplers =
      (DkSamplerDescriptor *)(data + sampler_offset);
  for (unsigned i = 0; i < kSamplerCount; i++) {
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.minFilter = sampler.magFilter =
        i == kSamplerLinear ? DkFilter_Linear : DkFilter_Nearest;
    sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] =
        DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize(&samplers[i], &sampler);
  }
  deko->ubo = dkMemBlockGetGpuAddr(deko->data_memory) + ubo_offset;

  deko->cmd_memory = MakeMemory(deko->device, kFrames * kCmdMemSize,
                                DkMemBlockFlags_CpuUncached |
                                    DkMemBlockFlags_GpuCached);
  deko->staging_memory = MakeMemory(deko->device, kFrames * kStagingSize,
                                    DkMemBlockFlags_CpuUncached |
                                        DkMemBlockFlags_GpuCached);
  deko->capture_memory =
      MakeMemory(deko->device,
                 (uint32_t)deko->output_width * (uint32_t)deko->output_height * 4u,
                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!deko->cmd_memory || !deko->staging_memory || !deko->capture_memory)
    goto fail;
  DkCmdBufMaker cmdbuf_maker;
  dkCmdBufMakerDefaults(&cmdbuf_maker, deko->device);
  deko->cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
  DkQueueMaker queue_maker;
  dkQueueMakerDefaults(&queue_maker, deko->device);
  queue_maker.flags = DkQueueFlags_Graphics;
  deko->queue = dkQueueCreate(&queue_maker);
  if (!deko->cmdbuf || !deko->queue) goto fail;
  /* The caller's error buffer is only for creation; later reports go to
   * stderr alone. */
  deko->error = NULL;
  deko->error_capacity = 0;
  return deko;

fail:
  if (error && error_capacity && !error[0])
    (void)snprintf(error, error_capacity, "deko3d initialization failed");
  Dkc2Deko3dDestroy(deko);
  return NULL;
}

static void ResetHdImages(Dkc2Deko3d *deko) {
  memset(deko->hd_images, 0, kDkc2HdImageSlots * sizeof *deko->hd_images);
  deko->hd_image_count = 0;
  deko->pool_current = 0;
  deko->pool_used = 0;
}

void Dkc2Deko3dDestroy(Dkc2Deko3d *deko) {
  if (!deko) return;
  if (deko->queue) {
    dkQueueWaitIdle(deko->queue);
    dkQueueDestroy(deko->queue);
  }
  if (deko->cmdbuf) dkCmdBufDestroy(deko->cmdbuf);
  for (unsigned i = 0; i < kFrames; i++) {
    TextureDestroy(&deko->source[i]);
    for (unsigned input = 0; input < 4; input++)
      TextureDestroy(&deko->inputs[i][input]);
  }
  TextureDestroy(&deko->hd_output);
  TextureDestroy(&deko->hd_layer);
  for (unsigned i = 0; i < deko->pool_created; i++)
    dkMemBlockDestroy(deko->pool[i]);
  if (deko->capture_memory) dkMemBlockDestroy(deko->capture_memory);
  if (deko->staging_memory) dkMemBlockDestroy(deko->staging_memory);
  if (deko->cmd_memory) dkMemBlockDestroy(deko->cmd_memory);
  if (deko->data_memory) dkMemBlockDestroy(deko->data_memory);
  if (deko->code_memory) dkMemBlockDestroy(deko->code_memory);
  if (deko->swapchain) dkSwapchainDestroy(deko->swapchain);
  if (deko->swapchain_memory) dkMemBlockDestroy(deko->swapchain_memory);
  if (deko->device) dkDeviceDestroy(deko->device);
  free(deko->hd_images);
  free(deko);
}

void Dkc2Deko3dOutputSize(const Dkc2Deko3d *deko, int *width, int *height) {
  if (width) *width = deko ? deko->output_width : 0;
  if (height) *height = deko ? deko->output_height : 0;
}

bool Dkc2Deko3dPresent(Dkc2Deko3d *deko, const uint8_t *pixels, int width,
                       int height, bool linear_filter,
                       const Dkc2Deko3dCapture *capture) {
  if (!deko || !pixels || width <= 0 || height <= 0) return false;
  DekoTexture *source = &deko->source[deko->frame % kFrames];
  if (source->width != width || source->height != height) {
    /* A new frame size (widescreen changes): rebuild every frame's. */
    dkQueueWaitIdle(deko->queue);
    for (unsigned i = 0; i < kFrames; i++) {
      TextureDestroy(&deko->source[i]);
      if (!TextureCreate(deko, &deko->source[i], DkImageFormat_BGRA8_Unorm,
                         0, width, height, kDescriptorSource + i))
        return false;
    }
  }
  BeginFrame(deko);
  if (!Upload(deko, &source->view, pixels, width, height, 4)) return false;
  dkCmdBufBarrier(deko->cmdbuf, DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  return Show(deko, kDescriptorSource + deko->frame % kFrames, width, height,
              linear_filter, capture);
}

static unsigned HdImageSlot(const Dkc2Deko3d *deko, uint64_t id) {
  unsigned slot = (unsigned)((id * 0x9e3779b97f4a7c15ull) >> 51) &
                  (kDkc2HdImageSlots - 1u);
  while (deko->hd_images[slot].id && deko->hd_images[slot].id != id)
    slot = (slot + 1u) & (kDkc2HdImageSlots - 1u);
  return slot;
}

/* Memory for an image from the HD pool, growing it a block at a time. */
static bool PoolAllocate(Dkc2Deko3d *deko, uint32_t size, uint32_t alignment,
                         DkMemBlock *block, uint32_t *offset) {
  if (size > kPoolBlockSize) return false;
  for (;;) {
    if (deko->pool_current < deko->pool_created) {
      const uint32_t at = Align(deko->pool_used, alignment);
      if (at + size <= kPoolBlockSize) {
        *block = deko->pool[deko->pool_current];
        *offset = at;
        deko->pool_used = at + size;
        return true;
      }
      deko->pool_current++;
      deko->pool_used = 0;
      continue;
    }
    if (deko->pool_created == kPoolBlocks) return false;
    DkMemBlock created =
        MakeMemory(deko->device, kPoolBlockSize,
                   DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!created) return false;
    deko->pool[deko->pool_created++] = created;
  }
}

/* Uploads an HD image the first time a scene uses it. False when the table
 * or the pool is full. */
static bool EnsureHdImage(Dkc2Deko3d *deko, const Dkc2HdImage *image,
                          uint64_t serial) {
  const unsigned slot = HdImageSlot(deko, image->id);
  DekoHdImage *entry = &deko->hd_images[slot];
  if (entry->id) {
    entry->serial = serial;
    return true;
  }
  if (deko->hd_image_count == kDkc2HdImageMax) return false;
  DkImageLayout layout;
  MakeLayout(deko->device, DkImageFormat_BGRA8_Unorm, 0, image->width,
             image->height, &layout);
  DkMemBlock block;
  uint32_t offset;
  if (!PoolAllocate(deko, (uint32_t)dkImageLayoutGetSize(&layout),
                    dkImageLayoutGetAlignment(&layout), &block, &offset))
    return false;
  dkImageInitialize(&entry->image, &layout, block, offset);
  DkImageView view;
  dkImageViewDefaults(&view, &entry->image);
  dkImageDescriptorInitialize(&deko->image_descriptors[slot], &view, false,
                              false);
  if (!Upload(deko, &view, image->pixels, image->width, image->height, 4))
    return false;
  entry->id = image->id;
  entry->serial = serial;
  deko->hd_image_count++;
  return true;
}

static bool EnsureHdImages(Dkc2Deko3d *deko, const Dkc2HdQuad *quads,
                           unsigned count, uint64_t serial) {
  for (unsigned q = 0; q < count; q++)
    if (!EnsureHdImage(deko, quads[q].image, serial)) return false;
  return true;
}

/* A 5-bit color channel expanded to 8 bits as the PPU does, as a shader
 * color value. */
static float HdExpand5(unsigned value) {
  return (float)(value << 3 | value >> 2) / 255.0f;
}

/* desktop_present_sdl.c DrawHdQuads: kind 0 art, 1 sprite, 2 tile. */
static void DrawHdQuads(Dkc2Deko3d *deko, const Dkc2HdScene *scene,
                        const Dkc2HdQuad *quads, unsigned count, int kind,
                        int width, int height) {
  DekoDraw draw = WholeTarget(width, height);
  draw.native[0] = (float)scene->width;
  draw.native[1] = (float)scene->height;
  draw.native[2] = (float)scene->scale;
  draw.native[3] = (float)kind;
  for (unsigned q = 0; q < count; q++) {
    const Dkc2HdQuad *quad = &quads[q];
    const Dkc2HdImage *image = quad->image;
    const unsigned slot = HdImageSlot(deko, image->id);
    if (deko->hd_images[slot].id != image->id) continue;
    BindTexture(deko, 0, slot, kSamplerNearest);
    draw.rect[0] = (float)quad->x;
    draw.rect[1] = (float)quad->y;
    draw.rect[2] = (float)quad->width;
    draw.rect[3] = (float)quad->height;
    const bool whole = quad->source_width == 0;
    draw.source[0] = whole ? 0.0f : (float)quad->source_x;
    draw.source[1] = whole ? 0.0f : (float)quad->source_y;
    draw.source[2] = (float)(whole ? image->width : quad->source_width);
    draw.source[3] = (float)(whole ? image->height : quad->source_height);
    draw.add[0] = HdExpand5(quad->add_color & 0x1fu);
    draw.add[1] = HdExpand5((quad->add_color >> 5) & 0x1fu);
    draw.add[2] = HdExpand5((quad->add_color >> 10) & 0x1fu);
    draw.add[3] = (float)quad->brightness;
    draw.flip[0] = quad->flip_x ? 1.0f : 0.0f;
    draw.flip[1] = quad->flip_y ? 1.0f : 0.0f;
    draw.flip[2] = (float)image->width;
    draw.flip[3] = (float)image->height;
    draw.order[0] = (float)quad->rank;
    draw.order[1] = (float)quad->index;
    Draw(deko, &draw);
  }
}

/* Sizes the scene inputs and targets, waiting for the GPU before any is
 * rebuilt. */
static bool PrepareHd(Dkc2Deko3d *deko, const Dkc2HdScene *scene) {
  const int width = scene->width * scene->scale;
  const int height = scene->height * scene->scale;
  const bool inputs = deko->inputs[0][0].width != scene->width ||
                      deko->inputs[0][0].height != scene->height;
  const bool targets =
      deko->hd_output.width != width || deko->hd_output.height != height;
  const bool layer = scene->layer &&
                     (targets || deko->hd_layer.width != width);
  if (!inputs && !targets && !layer) return true;
  dkQueueWaitIdle(deko->queue);
  if (inputs) {
    for (unsigned i = 0; i < kFrames; i++) {
      for (unsigned input = 0; input < 4; input++) {
        DekoTexture *texture = &deko->inputs[i][input];
        TextureDestroy(texture);
        if (!TextureCreate(deko, texture,
                           input == kInputTiles ? DkImageFormat_R8_Unorm
                                                : DkImageFormat_BGRA8_Unorm,
                           0, scene->width, scene->height,
                           kDescriptorInput + i * 4 + input))
          return false;
      }
    }
  }
  if (targets) {
    TextureDestroy(&deko->hd_output);
    TextureDestroy(&deko->hd_layer);
    if (!TextureCreate(deko, &deko->hd_output, DkImageFormat_RGBA8_Unorm,
                       DkImageFlags_UsageRender, width, height,
                       kDescriptorOutput))
      return false;
  }
  if (scene->layer && !deko->hd_layer.memory &&
      !TextureCreate(deko, &deko->hd_layer, DkImageFormat_RGBA8_Unorm,
                     DkImageFlags_UsageRender, width, height,
                     kDescriptorLayer))
    return false;
  return true;
}

/* desktop_present_sdl.c ComposeHd: draws the scene into hd_output. */
static bool ComposeHd(Dkc2Deko3d *deko, const Dkc2HdScene *scene) {
  if (!PrepareHd(deko, scene)) return false;
  const int width = scene->width * scene->scale;
  const int height = scene->height * scene->scale;
  /* Every image the scene draws is resident before any pass is recorded;
   * when the table or the pool fills, the GPU finishes and they all start
   * over. */
  bool resident = false;
  for (int attempt = 0; attempt < 2 && !resident; attempt++) {
    if (attempt) {
      FlushAndWait(deko);
      ResetHdImages(deko);
    }
    resident =
        EnsureHdImages(deko, scene->tiles, scene->tile_count, scene->serial) &&
        EnsureHdImages(deko, scene->art, scene->art_count, scene->serial) &&
        EnsureHdImages(deko, scene->sprites, scene->sprite_count,
                       scene->serial);
  }
  if (!resident) return false;
  const unsigned set = deko->frame % kFrames;
  DekoTexture *inputs = deko->inputs[set];
  if (!Upload(deko, &inputs[kInputBase].view, scene->base, scene->width,
              scene->height, 4) ||
      !Upload(deko, &inputs[kInputOcclusion].view, scene->occlusion,
              scene->width, scene->height, 4) ||
      (scene->layer &&
       !Upload(deko, &inputs[kInputLayer].view, scene->layer, scene->width,
               scene->height, 4)) ||
      (scene->tile_count &&
       !Upload(deko, &inputs[kInputTiles].view, scene->tile_mask,
               scene->width, scene->height, 1)))
    return false;
  dkCmdBufBarrier(deko->cmdbuf, DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

  const uint32_t input = kDescriptorInput + set * 4;
  DekoDraw upscale = WholeTarget(width, height);
  upscale.native[0] = (float)scene->width;
  upscale.native[1] = (float)scene->height;
  upscale.native[2] = (float)scene->scale;
  upscale.native[3] = scene->background == kDkc2HdFilterSmooth ? 1.0f : 0.0f;
  BindTarget(deko, &deko->hd_output.view, width, height);
  BindPass(deko, &deko->upscale_fsh, false, DkColorMask_RGBA);
  BindTexture(deko, 0, input + kInputBase, kSamplerNearest);
  Draw(deko, &upscale);
  BindTexture(deko, 1, input + kInputOcclusion, kSamplerNearest);
  BindTexture(deko, 2, input + kInputTiles, kSamplerNearest);
  /* Blended passes keep the output opaque, as the CPU blend does. */
  if (scene->tile_count) {
    BindPass(deko, &deko->quad_fsh, true, DkColorMask_RGB);
    DrawHdQuads(deko, scene, scene->tiles, scene->tile_count, 2, width,
                height);
  }
  if (scene->layer) {
    BindTarget(deko, &deko->hd_layer.view, width, height);
    BindPass(deko, &deko->upscale_fsh, false, DkColorMask_RGBA);
    BindTexture(deko, 0, input + kInputLayer, kSamplerNearest);
    Draw(deko, &upscale);
    BindPass(deko, &deko->quad_fsh, false, DkColorMask_RGBA);
    DrawHdQuads(deko, scene, scene->art, scene->art_count, 0, width, height);
    dkCmdBufBarrier(deko->cmdbuf, DkBarrier_Fragments,
                    DkInvalidateFlags_Image);
    BindTarget(deko, &deko->hd_output.view, width, height);
    BindPass(deko, &deko->composite_fsh, true, DkColorMask_RGB);
    BindTexture(deko, 0, kDescriptorLayer, kSamplerNearest);
    Draw(deko, &upscale);
  }
  BindPass(deko, &deko->quad_fsh, true, DkColorMask_RGB);
  DrawHdQuads(deko, scene, scene->sprites, scene->sprite_count, 1, width,
              height);
  dkCmdBufBarrier(deko->cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);
  deko->hd_serial = scene->serial;
  deko->hd_width = width;
  deko->hd_height = height;
  return true;
}

bool Dkc2Deko3dPresentHd(Dkc2Deko3d *deko, const Dkc2HdScene *scene,
                         bool linear_filter,
                         const Dkc2Deko3dCapture *capture) {
  if (!deko || !scene) return false;
  BeginFrame(deko);
  if (scene->serial != deko->hd_serial && !ComposeHd(deko, scene)) {
    /* Nothing recorded runs half done: finish it, show nothing new. */
    FlushAndWait(deko);
    return false;
  }
  return Show(deko, kDescriptorOutput, deko->hd_width, deko->hd_height,
              linear_filter, capture);
}
