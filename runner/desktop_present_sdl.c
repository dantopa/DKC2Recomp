#include "desktop_present_sdl.h"

#include "desktop_launcher.h"
#include "dkc2_hd.h"
#include "desktop_viewport.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_RGBA16F_ARB
#define GL_RGBA16F_ARB 0x881A
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

static void SetError(char *error, size_t capacity, const char *message);

/* OpenGL 2.0 shader entry points, resolved through SDL at context creation:
 * the platform GL header only declares the 1.x fixed-function API. */
typedef struct Dkc2GlShaderApi {
  PFNGLCREATESHADERPROC CreateShader;
  PFNGLSHADERSOURCEPROC ShaderSource;
  PFNGLCOMPILESHADERPROC CompileShader;
  PFNGLGETSHADERIVPROC GetShaderiv;
  PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
  PFNGLDELETESHADERPROC DeleteShader;
  PFNGLCREATEPROGRAMPROC CreateProgram;
  PFNGLATTACHSHADERPROC AttachShader;
  PFNGLLINKPROGRAMPROC LinkProgram;
  PFNGLGETPROGRAMIVPROC GetProgramiv;
  PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
  PFNGLDELETEPROGRAMPROC DeleteProgram;
  PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
  PFNGLUSEPROGRAMPROC UseProgram;
  PFNGLUNIFORM1IPROC Uniform1i;
  PFNGLUNIFORM1FPROC Uniform1f;
  PFNGLUNIFORM2FPROC Uniform2f;
  PFNGLUNIFORM4FPROC Uniform4f;
  PFNGLACTIVETEXTUREPROC ActiveTexture;
  /* EXT_framebuffer_object, for offscreen captures and the CRT passes.
   * Optional. */
  PFNGLGENFRAMEBUFFERSEXTPROC GenFramebuffers;
  PFNGLBINDFRAMEBUFFEREXTPROC BindFramebuffer;
  PFNGLFRAMEBUFFERTEXTURE2DEXTPROC FramebufferTexture2D;
  PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC CheckFramebufferStatus;
  PFNGLDELETEFRAMEBUFFERSEXTPROC DeleteFramebuffers;
  bool fbo;
} Dkc2GlShaderApi;

static Dkc2GlShaderApi s_gl;

static bool LoadShaderApi(Dkc2GlProcLoader load) {
#define DKC2_GL_LOAD(name, proc) \
  s_gl.name = (proc)load("gl" #name); \
  if (!s_gl.name) return false
  DKC2_GL_LOAD(CreateShader, PFNGLCREATESHADERPROC);
  DKC2_GL_LOAD(ShaderSource, PFNGLSHADERSOURCEPROC);
  DKC2_GL_LOAD(CompileShader, PFNGLCOMPILESHADERPROC);
  DKC2_GL_LOAD(GetShaderiv, PFNGLGETSHADERIVPROC);
  DKC2_GL_LOAD(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
  DKC2_GL_LOAD(DeleteShader, PFNGLDELETESHADERPROC);
  DKC2_GL_LOAD(CreateProgram, PFNGLCREATEPROGRAMPROC);
  DKC2_GL_LOAD(AttachShader, PFNGLATTACHSHADERPROC);
  DKC2_GL_LOAD(LinkProgram, PFNGLLINKPROGRAMPROC);
  DKC2_GL_LOAD(GetProgramiv, PFNGLGETPROGRAMIVPROC);
  DKC2_GL_LOAD(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
  DKC2_GL_LOAD(DeleteProgram, PFNGLDELETEPROGRAMPROC);
  DKC2_GL_LOAD(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
  DKC2_GL_LOAD(UseProgram, PFNGLUSEPROGRAMPROC);
  DKC2_GL_LOAD(Uniform1i, PFNGLUNIFORM1IPROC);
  DKC2_GL_LOAD(Uniform1f, PFNGLUNIFORM1FPROC);
  DKC2_GL_LOAD(Uniform2f, PFNGLUNIFORM2FPROC);
  DKC2_GL_LOAD(Uniform4f, PFNGLUNIFORM4FPROC);
  DKC2_GL_LOAD(ActiveTexture, PFNGLACTIVETEXTUREPROC);
#undef DKC2_GL_LOAD
  s_gl.GenFramebuffers = (PFNGLGENFRAMEBUFFERSEXTPROC)
      load("glGenFramebuffersEXT");
  s_gl.BindFramebuffer = (PFNGLBINDFRAMEBUFFEREXTPROC)
      load("glBindFramebufferEXT");
  s_gl.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)
      load("glFramebufferTexture2DEXT");
  s_gl.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)
      load("glCheckFramebufferStatusEXT");
  s_gl.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSEXTPROC)
      load("glDeleteFramebuffersEXT");
  s_gl.fbo = s_gl.GenFramebuffers && s_gl.BindFramebuffer &&
             s_gl.FramebufferTexture2D && s_gl.CheckFramebufferStatus &&
             s_gl.DeleteFramebuffers;
  return true;
}

#include "desktop_reconstruct_shader.h"
#define kQuadVertexSource kReconstructVertexSource

/*
 * CRT television display (desktop_crt.h describes the model). Five GLSL
 * 1.20 programs over half-float targets, in linear light:
 *
 *   lines    the frame's rows decoded from sRGB and resampled horizontally
 *            to the viewport width with a Gaussian in source pixels (the
 *            video bandwidth), one target row per source line;
 *   beam     every output pixel sums the Gaussian beams of the nearby
 *            lines, each as wide as its brightness per channel, so bright
 *            lines merge and dark lines stay thin;
 *   down     a 4x4 box reduction of the beam image (and of that again);
 *   blur     a separable Gaussian, run twice on each reduction for the
 *            phosphor glow (a quarter of the size) and the halation (a
 *            sixteenth);
 *   compose  the beam image through the tube's curvature and rounded
 *            corners, plus its glow and halation with the direct light
 *            reduced by the same fractions, then the phosphor mask in
 *            window pixels with the inverse of its mean transmission as
 *            gain, a soft knee, the vignette, sRGB encoding, and a
 *            triangular dither of half a code value.
 */
static const char kCrtLinesFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D source;\n"
    "uniform vec2 source_size;\n"
    "uniform float sigma_h;\n"
    "varying vec2 uv;\n"
    "vec3 decode(vec3 c) {\n"
    "  vec3 lo = c / 12.92;\n"
    "  vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));\n"
    "  return mix(lo, hi, step(vec3(0.04045), c));\n"
    "}\n"
    "void main() {\n"
    "  float sx = uv.x * source_size.x;\n"
    "  float row = (floor(uv.y * source_size.y) + 0.5) / source_size.y;\n"
    "  float c = floor(sx);\n"
    "  float inv = -0.5 / (sigma_h * sigma_h);\n"
    "  vec3 acc = vec3(0.0);\n"
    "  float wsum = 0.0;\n"
    "  for (int k = -2; k <= 2; k++) {\n"
    "    float tx = c + float(k);\n"
    "    float d = tx + 0.5 - sx;\n"
    "    float w = exp(d * d * inv);\n"
    "    vec3 s = texture2D(source, vec2((tx + 0.5) / source_size.x, row)).rgb;\n"
    "    acc += w * decode(s);\n"
    "    wsum += w;\n"
    "  }\n"
    "  gl_FragColor = vec4(acc / wsum, 1.0);\n"
    "}\n";

static const char kCrtBeamFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D lines;\n"
    "uniform vec2 lines_size;\n"
    "uniform float sigma_dark;\n"
    "uniform float sigma_bright;\n"
    "uniform float beam_fade;\n"
    "varying vec2 uv;\n"
    "vec3 line_at(float i) {\n"
    "  if (i < 0.0 || i >= lines_size.y) return vec3(0.0);\n"
    "  return texture2D(lines, vec2(uv.x, (i + 0.5) / lines_size.y)).rgb;\n"
    "}\n"
    "void main() {\n"
    "  float y = uv.y * lines_size.y;\n"
    "  float i0 = floor(y - 0.5);\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int k = -1; k <= 2; k++) {\n"
    "    float i = i0 + float(k);\n"
    "    float d = y - (i + 0.5);\n"
    "    vec3 L = clamp(line_at(i), 0.0, 1.0);\n"
    "    vec3 s = mix(vec3(sigma_dark), vec3(sigma_bright), sqrt(L));\n"
    "    acc += L * exp(-d * d / (2.0 * s * s)) / (s * 2.5066283);\n"
    "  }\n"
    "  vec3 flat_color = line_at(clamp(floor(y), 0.0, lines_size.y - 1.0));\n"
    "  gl_FragColor = vec4(mix(flat_color, acc, beam_fade), 1.0);\n"
    "}\n";

static const char kCrtDownFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D source;\n"
    "uniform vec2 source_size;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  vec2 t = 1.0 / source_size;\n"
    "  vec3 c = texture2D(source, uv + vec2(-t.x, -t.y)).rgb +\n"
    "           texture2D(source, uv + vec2(t.x, -t.y)).rgb +\n"
    "           texture2D(source, uv + vec2(-t.x, t.y)).rgb +\n"
    "           texture2D(source, uv + vec2(t.x, t.y)).rgb;\n"
    "  gl_FragColor = vec4(c * 0.25, 1.0);\n"
    "}\n";

static const char kCrtBlurFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D source;\n"
    "uniform vec2 source_size;\n"
    "uniform vec2 direction;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  vec2 step = direction / source_size;\n"
    "  vec3 acc = texture2D(source, uv).rgb;\n"
    "  float wsum = 1.0;\n"
    "  for (int k = 1; k <= 4; k++) {\n"
    "    float w = exp(-float(k * k) / 8.0);\n"
    "    acc += w * (texture2D(source, uv + step * float(k)).rgb +\n"
    "                texture2D(source, uv - step * float(k)).rgb);\n"
    "    wsum += 2.0 * w;\n"
    "  }\n"
    "  gl_FragColor = vec4(acc / wsum, 1.0);\n"
    "}\n";

static const char kCrtComposeFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D image;\n"
    "uniform sampler2D glow;\n"
    "uniform sampler2D halo;\n"
    "uniform vec2 target_size;\n"
    "uniform float glow_amount;\n"
    "uniform float halo_amount;\n"
    "uniform vec2 curvature;\n"
    "uniform float corner_radius;\n"
    "uniform float vignette;\n"
    "uniform int mask_kind;\n"
    "uniform float mask_pitch;\n"
    "uniform float mask_strength;\n"
    "uniform float mask_gain;\n"
    "uniform float knee;\n"
    "varying vec2 uv;\n"
    "vec3 encode(vec3 c) {\n"
    "  c = clamp(c, 0.0, 1.0);\n"
    "  vec3 lo = c * 12.92;\n"
    "  vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;\n"
    "  return mix(lo, hi, step(vec3(0.0031308), c));\n"
    "}\n"
    "vec3 soft_knee(vec3 x) {\n"
    "  vec3 t = max(x - knee, 0.0) / (1.0 - knee);\n"
    "  vec3 e = exp(-2.0 * t);\n"
    "  vec3 bent = knee + (1.0 - knee) * (1.0 - e) / (1.0 + e);\n"
    "  return mix(x, bent, step(vec3(knee), x));\n"
    "}\n"
    "float ign(vec2 p) {\n"
    "  return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));\n"
    "}\n"
    "void main() {\n"
    "  vec2 p = uv * 2.0 - 1.0;\n"
    "  vec2 q = p;\n"
    "  q.x = p.x * (1.0 + curvature.x * p.y * p.y);\n"
    "  q.y = p.y * (1.0 + curvature.y * p.x * p.x);\n"
    "  q *= 1.0 + max(curvature.x, curvature.y);\n"
    "  vec2 s = (q + 1.0) * 0.5;\n"
    "  vec2 px = s * target_size;\n"
    "  vec2 e = min(px, target_size - px);\n"
    "  float radius = corner_radius * target_size.y;\n"
    "  vec2 cr = max(vec2(radius) - e, 0.0);\n"
    "  float inside = radius - length(cr);\n"
    "  if (radius <= 0.0) inside = min(e.x, e.y);\n"
    "  float edge = smoothstep(-0.5, 0.5, inside);\n"
    "  vec2 sc = clamp(s, 0.0, 1.0);\n"
    "  vec3 c = texture2D(image, sc).rgb * (1.0 - glow_amount - halo_amount) +\n"
    "           texture2D(glow, sc).rgb * glow_amount +\n"
    "           texture2D(halo, sc).rgb * halo_amount;\n"
    "  if (mask_kind > 0) {\n"
    "    float col = floor(gl_FragCoord.x);\n"
    "    float stripe = floor(mod(col, mask_pitch) * 3.0 / mask_pitch);\n"
    "    vec3 sel = vec3(stripe == 0.0 ? 1.0 : 0.0, stripe == 1.0 ? 1.0 : 0.0,\n"
    "                    stripe == 2.0 ? 1.0 : 0.0);\n"
    "    vec3 pass = mix(vec3(1.0), sel, mask_strength);\n"
    "    if (mask_kind == 2) {\n"
    "      float group = floor(col / mask_pitch);\n"
    "      float shift = mod(group, 2.0) * mask_pitch * 0.5;\n"
    "      float yy = mod(floor(gl_FragCoord.y) + shift, mask_pitch);\n"
    "      if (yy < 1.0) pass *= 1.0 - mask_strength;\n"
    "    }\n"
    "    c = c * mask_gain * pass;\n"
    "  }\n"
    "  c = soft_knee(c);\n"
    "  float vig = 1.0 - vignette * smoothstep(0.5, 1.5, length(p));\n"
    "  c *= edge * vig;\n"
    "  vec3 out_c = encode(c);\n"
    "  float noise = ign(gl_FragCoord.xy) - ign(gl_FragCoord.xy + vec2(17.0, 41.0));\n"
    "  out_c += noise * (0.5 / 255.0);\n"
    "  gl_FragColor = vec4(out_c, 1.0);\n"
    "}\n";

/*
 * HD scenes (dkc2_hd.h describes the scene and its rules), drawn into RGBA8
 * targets at the output scale. Every pass works on whole output pixels: the
 * output pixel is floor(gl_FragCoord), its 1x pixel floor(pixel / scale),
 * and textures are read at texel centers with nearest sampling, so each
 * pass computes what the CPU compositor computes for that pixel.
 *
 *   upscale    a 1x input (base or BG1) nearest, or premultiplied
 *              Catmull-Rom with each channel kept within the 2x2 source
 *              pixels nearest the output pixel (anti-ringing);
 *   quad       an image over an output rectangle; for sprites, discarded
 *              where the occlusion map hides it;
 *   composite  the BG1 layer where the occlusion map puts it in front.
 */
static const char kHdUpscaleFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D source;\n"
    "uniform vec2 source_size;\n"
    "uniform float scale;\n"
    "uniform bool smooth;\n"
    "vec4 texel(vec2 p) {\n"
    "  p = clamp(p, vec2(0.0), source_size - 1.0);\n"
    "  return texture2D(source, (p + 0.5) / source_size);\n"
    "}\n"
    "vec4 premultiplied(vec2 p) {\n"
    "  vec4 c = texel(p);\n"
    "  return vec4(c.rgb * c.a, c.a);\n"
    "}\n"
    "float weight(float t) {\n"
    "  t = abs(t);\n"
    "  if (t < 1.0) return (1.5 * t - 2.5) * t * t + 1.0;\n"
    "  if (t < 2.0) return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;\n"
    "  return 0.0;\n"
    "}\n"
    "vec4 weights(float t) {\n"
    "  return vec4(weight(t), weight(t - 1.0), weight(t - 2.0),\n"
    "              weight(t - 3.0));\n"
    "}\n"
    "vec4 row(vec2 p, vec4 w) {\n"
    "  return w.x * premultiplied(p) + w.y * premultiplied(p + vec2(1.0, 0.0))\n"
    "       + w.z * premultiplied(p + vec2(2.0, 0.0))\n"
    "       + w.w * premultiplied(p + vec2(3.0, 0.0));\n"
    "}\n"
    "void widen(vec4 c, inout vec3 low, inout vec3 high, inout bool any) {\n"
    "  if (c.a < 0.5) return;\n"
    "  low = min(low, c.rgb);\n"
    "  high = max(high, c.rgb);\n"
    "  any = true;\n"
    "}\n"
    "void main() {\n"
    "  vec2 o = floor(gl_FragCoord.xy);\n"
    "  vec2 cell = floor((o + 0.5) / scale);\n"
    "  if (!smooth) {\n"
    "    gl_FragColor = texel(cell);\n"
    "    return;\n"
    "  }\n"
    "  vec2 position = (o - cell * scale + 0.5) / scale - 0.5;\n"
    "  vec2 offset = vec2(position.x < 0.0 ? -2.0 : -1.0,\n"
    "                     position.y < 0.0 ? -2.0 : -1.0);\n"
    "  vec2 first = cell + offset;\n"
    "  vec4 wx = weights(position.x - offset.x);\n"
    "  vec4 wy = weights(position.y - offset.y);\n"
    "  vec4 sum = wy.x * row(first, wx)\n"
    "           + wy.y * row(first + vec2(0.0, 1.0), wx)\n"
    "           + wy.z * row(first + vec2(0.0, 2.0), wx)\n"
    "           + wy.w * row(first + vec2(0.0, 3.0), wx);\n"
    "  vec2 near = clamp(first + 1.0, vec2(0.0), source_size - 1.0);\n"
    "  vec3 low = vec3(1.0);\n"
    "  vec3 high = vec3(0.0);\n"
    "  bool any = false;\n"
    "  widen(texel(near), low, high, any);\n"
    "  widen(texel(near + vec2(1.0, 0.0)), low, high, any);\n"
    "  widen(texel(near + vec2(0.0, 1.0)), low, high, any);\n"
    "  widen(texel(near + vec2(1.0, 1.0)), low, high, any);\n"
    "  if (sum.a * 255.0 < 1.0 || !any) {\n"
    "    gl_FragColor = vec4(0.0);\n"
    "    return;\n"
    "  }\n"
    "  gl_FragColor = vec4(clamp(sum.rgb / sum.a, low, high),\n"
    "                      min(sum.a, 1.0));\n"
    "}\n";

/* Quad kinds: art replaces the BG1 layer's pixels, a sprite blends where
 * the occlusion map does not hide it, a tile where the tile mask holds its
 * rank (layer + 1). Occlusion (from the scene's ARGB words): r the
 * background rank, g an object rank, b a sprite-table owner + 1, a the BG1
 * layer in front. */
enum { kHdQuadArt = 0, kHdQuadSprite = 1, kHdQuadTile = 2 };

static const char kHdQuadFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D image;\n"
    "uniform sampler2D occlusion;\n"
    "uniform sampler2D tile_mask;\n"
    "uniform vec2 image_size;\n"
    "uniform vec2 native_size;\n"
    "uniform vec4 rect;\n"
    "uniform vec2 flip;\n"
    "uniform float scale;\n"
    "uniform float brightness;\n"
    "uniform float rank;\n"
    "uniform float index;\n"
    "uniform int kind;\n"
    "void main() {\n"
    "  vec2 o = floor(gl_FragCoord.xy);\n"
    "  vec2 local = mix(o - rect.xy, rect.xy + rect.zw - 1.0 - o, flip);\n"
    "  vec2 cell = floor((local * image_size + 0.5) / rect.zw);\n"
    "  vec4 color = texture2D(image, (cell + 0.5) / image_size);\n"
    "  if (kind != 0) {\n"
    "    if (color.a == 0.0) discard;\n"
    "    vec2 pixel = floor((o + 0.5) / scale);\n"
    "    vec2 at = (pixel + 0.5) / native_size;\n"
    "    if (kind == 1) {\n"
    "      vec4 hide = floor(texture2D(occlusion, at) * 255.0 + 0.5);\n"
    "      if (rank <= hide.r || (hide.g > 0.0 && hide.g >= rank) ||\n"
    "          (hide.b > 0.0 && hide.b - 1.0 > index)) discard;\n"
    "    } else if (floor(texture2D(tile_mask, at).r * 255.0 + 0.5) != rank) {\n"
    "      discard;\n"
    "    }\n"
    "  }\n"
    "  if (brightness < 255.0)\n"
    "    color.rgb = floor((floor(color.rgb * 255.0 + 0.5) * brightness\n"
    "                       + 0.5) / 255.0) / 255.0;\n"
    "  gl_FragColor = color;\n"
    "}\n";

static const char kHdCompositeFragmentSource[] =
    "#version 120\n"
    "uniform sampler2D layer;\n"
    "uniform sampler2D occlusion;\n"
    "uniform vec2 output_size;\n"
    "uniform vec2 native_size;\n"
    "uniform float scale;\n"
    "void main() {\n"
    "  vec2 o = floor(gl_FragCoord.xy);\n"
    "  vec4 color = texture2D(layer, (o + 0.5) / output_size);\n"
    "  vec2 pixel = floor((o + 0.5) / scale);\n"
    "  float front = texture2D(occlusion, (pixel + 0.5) / native_size).a;\n"
    "  if (color.a == 0.0 || front < 0.5) discard;\n"
    "  gl_FragColor = color;\n"
    "}\n";

static GLuint CompileShader(GLenum kind, const char *source,
                            const char *name, char *error,
                            size_t capacity) {
  GLuint shader = s_gl.CreateShader(kind);
  if (!shader) {
    SetError(error, capacity, "glCreateShader failed");
    return 0;
  }
  const GLchar *sources[1] = {source};
  s_gl.ShaderSource(shader, 1, sources, NULL);
  s_gl.CompileShader(shader);
  GLint status = GL_FALSE;
  s_gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    char log[512] = {0};
    s_gl.GetShaderInfoLog(shader, sizeof log - 1, NULL, log);
    (void)snprintf(error, capacity, "%s shader failed to compile: %s", name,
                   log);
    s_gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}

/* Build a vertex+fragment program; 0 with the reason in error. */
static GLuint BuildProgram(const char *fragment_source, const char *name,
                           char *error, size_t capacity) {
  GLuint vertex = CompileShader(GL_VERTEX_SHADER, kQuadVertexSource, name,
                                error, capacity);
  if (!vertex) return 0;
  GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragment_source, name,
                                  error, capacity);
  if (!fragment) {
    s_gl.DeleteShader(vertex);
    return 0;
  }
  GLuint program = s_gl.CreateProgram();
  s_gl.AttachShader(program, vertex);
  s_gl.AttachShader(program, fragment);
  s_gl.LinkProgram(program);
  s_gl.DeleteShader(vertex);
  s_gl.DeleteShader(fragment);
  GLint status = GL_FALSE;
  s_gl.GetProgramiv(program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    char log[512] = {0};
    s_gl.GetProgramInfoLog(program, sizeof log - 1, NULL, log);
    (void)snprintf(error, capacity, "%s shader failed to link: %s", name,
                   log);
    s_gl.DeleteProgram(program);
    return 0;
  }
  return program;
}

static void BuildReconstructProgram(Dkc2SdlPresenter *presenter) {
  presenter->program = BuildProgram(kReconstructFragmentSource, "reconstruct",
                                    presenter->shader_error,
                                    sizeof presenter->shader_error);
  if (!presenter->program) return;
  GLuint program = presenter->program;
  presenter->uniform_source = s_gl.GetUniformLocation(program, "source");
  presenter->uniform_source_size =
      s_gl.GetUniformLocation(program, "source_size");
  presenter->uniform_output_size =
      s_gl.GetUniformLocation(program, "output_size");
  presenter->uniform_mode = s_gl.GetUniformLocation(program, "mode");
  presenter->uniform_strength = s_gl.GetUniformLocation(program, "strength");
  presenter->uniform_softness = s_gl.GetUniformLocation(program, "softness");
  presenter->uniform_shading = s_gl.GetUniformLocation(program, "shading");
}

static void BuildCrtPrograms(Dkc2SdlPresenter *presenter) {
  static const char *const sources[kDkc2CrtPassCount] = {
      kCrtLinesFragmentSource, kCrtBeamFragmentSource,
      kCrtDownFragmentSource, kCrtBlurFragmentSource,
      kCrtComposeFragmentSource};
  static const char *const names[kDkc2CrtPassCount] = {
      "crt lines", "crt beam", "crt downsample", "crt blur", "crt compose"};
  if (!s_gl.fbo) {
    SetError(presenter->crt_error, sizeof presenter->crt_error,
             "crt display unavailable: framebuffer objects missing");
    return;
  }
  for (int pass = 0; pass < kDkc2CrtPassCount; pass++) {
    presenter->crt_program[pass] =
        BuildProgram(sources[pass], names[pass], presenter->crt_error,
                     sizeof presenter->crt_error);
    if (!presenter->crt_program[pass]) {
      for (int built = 0; built < pass; built++) {
        s_gl.DeleteProgram(presenter->crt_program[built]);
        presenter->crt_program[built] = 0;
      }
      return;
    }
  }
}

static void BuildHdPrograms(Dkc2SdlPresenter *presenter) {
  static const char *const sources[kDkc2HdPassCount] = {
      kHdUpscaleFragmentSource, kHdQuadFragmentSource,
      kHdCompositeFragmentSource};
  static const char *const names[kDkc2HdPassCount] = {
      "hd upscale", "hd quad", "hd composite"};
  if (!s_gl.fbo) {
    SetError(presenter->hd_error, sizeof presenter->hd_error,
             "gpu hd unavailable: framebuffer objects missing");
    return;
  }
  for (int pass = 0; pass < kDkc2HdPassCount; pass++) {
    presenter->hd_program[pass] =
        BuildProgram(sources[pass], names[pass], presenter->hd_error,
                     sizeof presenter->hd_error);
    if (!presenter->hd_program[pass]) {
      for (int built = 0; built < pass; built++) {
        s_gl.DeleteProgram(presenter->hd_program[built]);
        presenter->hd_program[built] = 0;
      }
      return;
    }
  }
  glGenTextures(4, presenter->hd_input);
  for (int input = 0; input < 4; input++) {
    glBindTexture(GL_TEXTURE_2D, presenter->hd_input[input]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

static bool CrtProgramsReady(const Dkc2SdlPresenter *presenter) {
  for (int pass = 0; pass < kDkc2CrtPassCount; pass++)
    if (!presenter->crt_program[pass]) return false;
  return s_gl.fbo;
}

static void SetError(char *error, size_t capacity, const char *message) {
  if (!error || capacity == 0) return;
  (void)snprintf(error, capacity, "%s", message ? message : "SDL error");
}

static bool SetSdlSwapInterval(void *user, int interval) {
  (void)user;
  return SDL_GL_SetSwapInterval(interval) == 0;
}

static bool EnvironmentEnabled(const char *name) {
  const char *value = getenv(name);
  return value && *value && *value != '0';
}

/* Render targets: half-float for the CRT passes, RGBA8 for HD scenes. */
static void DestroyTarget(Dkc2GlTarget *target) {
  if (!target) return;
  if (target->fbo && s_gl.fbo) s_gl.DeleteFramebuffers(1, &target->fbo);
  if (target->texture) glDeleteTextures(1, &target->texture);
  memset(target, 0, sizeof *target);
}

static bool CreateTarget(Dkc2GlTarget *target, int width, int height,
                         GLint format, GLint filter) {
  memset(target, 0, sizeof *target);
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  glGenTextures(1, &target->texture);
  if (!target->texture) return false;
  glBindTexture(GL_TEXTURE_2D, target->texture);
  glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA,
               GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  s_gl.GenFramebuffers(1, &target->fbo);
  s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, target->fbo);
  s_gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                            GL_TEXTURE_2D, target->texture, 0);
  const bool complete = s_gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) ==
                        GL_FRAMEBUFFER_COMPLETE_EXT;
  s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
  if (!complete) {
    DestroyTarget(target);
    return false;
  }
  target->width = width;
  target->height = height;
  return true;
}

static void DestroyCrtTargets(Dkc2SdlPresenter *presenter) {
  DestroyTarget(&presenter->crt_lines);
  DestroyTarget(&presenter->crt_beam);
  DestroyTarget(&presenter->crt_glow[0]);
  DestroyTarget(&presenter->crt_glow[1]);
  DestroyTarget(&presenter->crt_halo[0]);
  DestroyTarget(&presenter->crt_halo[1]);
}

/* Size the CRT targets to the viewport (rebuilt only when it changes). */
static bool EnsureCrtTargets(Dkc2SdlPresenter *presenter, int viewport_width,
                             int viewport_height, int source_height) {
  if (presenter->crt_beam.width == viewport_width &&
      presenter->crt_beam.height == viewport_height &&
      presenter->crt_lines.height == source_height &&
      presenter->crt_lines.texture)
    return true;
  DestroyCrtTargets(presenter);
  const int glow_w = viewport_width / 4, glow_h = viewport_height / 4;
  const int halo_w = viewport_width / 16, halo_h = viewport_height / 16;
  const GLint half = GL_RGBA16F_ARB;
  if (!CreateTarget(&presenter->crt_lines, viewport_width, source_height,
                    half, GL_NEAREST) ||
      !CreateTarget(&presenter->crt_beam, viewport_width, viewport_height,
                    half, GL_LINEAR) ||
      !CreateTarget(&presenter->crt_glow[0], glow_w, glow_h, half,
                    GL_LINEAR) ||
      !CreateTarget(&presenter->crt_glow[1], glow_w, glow_h, half,
                    GL_LINEAR) ||
      !CreateTarget(&presenter->crt_halo[0], halo_w, halo_h, half,
                    GL_LINEAR) ||
      !CreateTarget(&presenter->crt_halo[1], halo_w, halo_h, half,
                    GL_LINEAR)) {
    DestroyCrtTargets(presenter);
    SetError(presenter->crt_error, sizeof presenter->crt_error,
             "crt display unavailable: half-float render targets failed");
    return false;
  }
  return true;
}

void Dkc2GlPipelineInitialize(Dkc2SdlPresenter *presenter, Dkc2GlProcLoader load) {
  if (!LoadShaderApi(load)) {
    SetError(presenter->shader_error, sizeof presenter->shader_error,
             "reconstruct shader unavailable: OpenGL 2.0 shader entry "
             "points missing");
    SetError(presenter->crt_error, sizeof presenter->crt_error,
             "crt display unavailable: OpenGL 2.0 shader entry points "
             "missing");
  } else {
    BuildReconstructProgram(presenter);
    BuildCrtPrograms(presenter);
    BuildHdPrograms(presenter);
  }
  if (presenter->program == 0 && presenter->shader_error[0])
    fprintf(stderr, "warning: %s\n", presenter->shader_error);
  if (!CrtProgramsReady(presenter) && presenter->crt_error[0])
    fprintf(stderr, "warning: %s\n", presenter->crt_error);
  if (!presenter->hd_program[0] && presenter->hd_error[0])
    fprintf(stderr, "warning: %s\n", presenter->hd_error);
}

bool Dkc2GlPipelineCrtAvailable(const Dkc2SdlPresenter *presenter) {
  return presenter && CrtProgramsReady(presenter);
}

bool Dkc2SdlPresenterInit(Dkc2SdlPresenter *presenter, int window_scale,
                          int fullscreen, bool hidden, bool linear_filter,
                          int source_width, int source_height,
                          char *error, size_t error_capacity) {
  if (!presenter || window_scale < 1 ||
      source_width <= 0 || source_height <= 0) {
    SetError(error, error_capacity, "invalid SDL presenter settings");
    return false;
  }
  memset(presenter, 0, sizeof *presenter);
  (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  (void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  Uint32 flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;
  flags |= hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
  if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  int base_height = 240;
  int base_width =
      source_width * 7 * base_height / (source_height * 6);
  SDL_Window *window = SDL_CreateWindow(
      DKC2_PRODUCT_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      base_width * window_scale, base_height * window_scale, flags);
  if (!window) {
    SetError(error, error_capacity, SDL_GetError());
    return false;
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context || SDL_GL_MakeCurrent(window, context) != 0) {
    SetError(error, error_capacity, SDL_GetError());
    if (context) SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    return false;
  }
#ifdef __APPLE__
  /* The visible Mac host uses one exact Mach deadline as its presentation
   * authority. A second blocking OpenGL-vsync gate can quantize a timely
   * frame onto the following 60/120-Hz display callback and produce the
   * alternating micro-hitches seen during horizontal traversal. macOS still
   * composites the window atomically. Keep the old gate only as an explicit
   * diagnostic override. */
  presenter->software_paced = !hidden &&
      !EnvironmentEnabled("DKC2_KEEP_OPENGL_VSYNC");
#else
  presenter->software_paced = false;
#endif
  if (presenter->software_paced || hidden) {
    (void)SDL_GL_SetSwapInterval(0);
    presenter->vsync_status = kDkc2DesktopVsyncDisabled;
  } else {
    presenter->vsync_status =
        Dkc2DesktopEnableVsync(SetSdlSwapInterval, NULL);
    if (presenter->vsync_status != kDkc2DesktopVsyncEnabled)
      (void)SDL_GL_SetSwapInterval(0);
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  if (!texture) {
    SetError(error, error_capacity, "OpenGL texture creation failed");
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    return false;
  }
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  const GLubyte *version = glGetString(GL_VERSION);
  (void)snprintf(presenter->backend, sizeof presenter->backend,
                 "SDL2/OpenGL %s; vsync=%s; pacing=%s",
                 version ? (const char *)version : "unknown",
                 Dkc2DesktopVsyncStatusName(presenter->vsync_status),
                 presenter->software_paced ? "mach" : "swap");
  presenter->window = window;
  presenter->gl_context = context;
  presenter->texture = texture;
  presenter->linear_filter = linear_filter;
  presenter->upscaler = linear_filter ? kDkc2UpscalerBilinear
                                      : kDkc2UpscalerNearest;
  presenter->reconstruct_mode = 3;
  presenter->reconstruct_strength = 1.0f;
  presenter->reconstruct_softness = 0.5f;
  presenter->reconstruct_shading = 0.6f;
  presenter->display = kDkc2DisplayFlat;
  Dkc2CrtSettingsDefault(&presenter->crt);
  Dkc2GlPipelineInitialize(presenter, SDL_GL_GetProcAddress);
  return true;
}

static float ClampUnit(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

int Dkc2SdlPresenterSetUpscaler(Dkc2SdlPresenter *presenter, int upscaler,
                                int mode, float strength, float softness,
                                float shading) {
  if (!presenter) return kDkc2UpscalerNearest;
  if (upscaler < 0 || upscaler >= kDkc2UpscalerCount)
    upscaler = kDkc2UpscalerNearest;
  if (upscaler == kDkc2UpscalerReconstruct && presenter->program == 0)
    upscaler = presenter->linear_filter ? kDkc2UpscalerBilinear
                                        : kDkc2UpscalerNearest;
  presenter->upscaler = upscaler;
  presenter->reconstruct_mode = mode < 0 ? 0 : (mode > 4 ? 4 : mode);
  presenter->reconstruct_strength = ClampUnit(strength);
  presenter->reconstruct_softness = ClampUnit(softness);
  presenter->reconstruct_shading = ClampUnit(shading);
  if (upscaler != kDkc2UpscalerReconstruct)
    presenter->linear_filter = upscaler == kDkc2UpscalerBilinear;
  return upscaler;
}

const char *Dkc2SdlPresenterUpscalerName(int upscaler) {
  switch (upscaler) {
    case kDkc2UpscalerBilinear: return "bilinear";
    case kDkc2UpscalerReconstruct: return "reconstruct";
    default: return "nearest";
  }
}

bool Dkc2SdlPresenterUpscalerFromName(const char *name, int *upscaler) {
  if (!name || !upscaler) return false;
  if (strcmp(name, "nearest") == 0 || strcmp(name, "0") == 0) {
    *upscaler = kDkc2UpscalerNearest;
    return true;
  }
  if (strcmp(name, "bilinear") == 0 || strcmp(name, "linear") == 0 ||
      strcmp(name, "1") == 0) {
    *upscaler = kDkc2UpscalerBilinear;
    return true;
  }
  if (strcmp(name, "reconstruct") == 0 || strcmp(name, "2") == 0) {
    *upscaler = kDkc2UpscalerReconstruct;
    return true;
  }
  return false;
}

int Dkc2SdlPresenterSetDisplay(Dkc2SdlPresenter *presenter, int display,
                               const Dkc2CrtSettings *crt) {
  if (!presenter) return kDkc2DisplayFlat;
  if (display != kDkc2DisplayCrt) display = kDkc2DisplayFlat;
  if (crt) {
    presenter->crt = *crt;
    Dkc2CrtSettingsClamp(&presenter->crt);
  }
  if (display == kDkc2DisplayCrt && !CrtProgramsReady(presenter))
    display = kDkc2DisplayFlat;
  presenter->display = display;
  /* The diagnostics backend string carries the display in effect. */
  char *suffix = strstr(presenter->backend, "; display=");
  if (suffix) *suffix = '\0';
  const size_t used = strlen(presenter->backend);
  (void)snprintf(presenter->backend + used, sizeof presenter->backend - used,
                 "; display=%s", Dkc2CrtDisplayName(display));
  return display;
}

void Dkc2SdlPresenterDrawableSize(Dkc2SdlPresenter *presenter, int *width,
                                  int *height) {
  if (width) *width = 0;
  if (height) *height = 0;
  if (!presenter || !presenter->window) return;
  SDL_GL_GetDrawableSize((SDL_Window *)presenter->window, width, height);
}

void Dkc2SdlPresenterArmCapture(Dkc2SdlPresenter *presenter, uint8_t *rgb,
                                int width, int height) {
  if (!presenter) return;
  presenter->capture_rgb = rgb;
  presenter->capture_width = width;
  presenter->capture_height = height;
  presenter->capture_done = false;
}

void Dkc2SdlPresenterSetWindowSize(Dkc2SdlPresenter *presenter, int width,
                                   int height) {
  if (!presenter || !presenter->window || width <= 0 || height <= 0) return;
  SDL_SetWindowSize((SDL_Window *)presenter->window, width, height);
}

/* The frame quad. Drawn to the window the image's top row is at the top
 * (flip); drawn into a render target it is stored with row 0 at texture
 * coordinate 0, so the next pass reads it with the same coordinates it
 * would read the uploaded frame. */
static void DrawFrameQuad(bool flip) {
  const float top = flip ? 0.0f : 1.0f;
  const float bottom = flip ? 1.0f : 0.0f;
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, bottom);
  glVertex2f(-1.0f, -1.0f);
  glTexCoord2f(1.0f, bottom);
  glVertex2f(1.0f, -1.0f);
  glTexCoord2f(1.0f, top);
  glVertex2f(1.0f, 1.0f);
  glTexCoord2f(0.0f, top);
  glVertex2f(-1.0f, 1.0f);
  glEnd();
}

static void Uniform1f(GLuint program, const char *name, float value) {
  s_gl.Uniform1f(s_gl.GetUniformLocation(program, name), value);
}

static void Uniform1i(GLuint program, const char *name, int value) {
  s_gl.Uniform1i(s_gl.GetUniformLocation(program, name), value);
}

static void Uniform2f(GLuint program, const char *name, float x, float y) {
  s_gl.Uniform2f(s_gl.GetUniformLocation(program, name), x, y);
}

static void BindTextureUnit(int unit, GLuint texture) {
  s_gl.ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
  glBindTexture(GL_TEXTURE_2D, texture);
}

/* Run one program over a whole render target with the unflipped quad. */
static void DrawIntoTarget(GLuint program, const Dkc2GlTarget *target) {
  s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, target->fbo);
  glViewport(0, 0, target->width, target->height);
  s_gl.UseProgram(program);
  DrawFrameQuad(false);
}

/* The flat presentation: the source frame through the selected sampler
 * or the Reconstruct program, into the viewport of the bound framebuffer. */
static void RenderFlat(Dkc2SdlPresenter *presenter, GLuint source,
                       int source_width, int source_height,
                       const Dkc2DesktopViewport *viewport,
                       int output_height) {
  const bool reconstruct =
      presenter->upscaler == kDkc2UpscalerReconstruct && presenter->program;
  GLint sampling = presenter->linear_filter && !reconstruct ? GL_LINEAR
                                                             : GL_NEAREST;
  glBindTexture(GL_TEXTURE_2D, source);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sampling);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampling);
  glViewport(viewport->x, output_height - viewport->y - viewport->height,
             viewport->width, viewport->height);
  if (reconstruct) {
    s_gl.UseProgram(presenter->program);
    s_gl.Uniform1i(presenter->uniform_source, 0);
    s_gl.Uniform2f(presenter->uniform_source_size, (float)source_width,
                   (float)source_height);
    s_gl.Uniform2f(presenter->uniform_output_size, (float)viewport->width,
                   (float)viewport->height);
    s_gl.Uniform1i(presenter->uniform_mode, presenter->reconstruct_mode);
    s_gl.Uniform1f(presenter->uniform_strength,
                   presenter->reconstruct_strength);
    s_gl.Uniform1f(presenter->uniform_softness,
                   presenter->reconstruct_softness);
    s_gl.Uniform1f(presenter->uniform_shading,
                   presenter->reconstruct_shading);
  }
  DrawFrameQuad(true);
  if (reconstruct) s_gl.UseProgram(0);
}

/* The CRT presentation: the five passes, ending in the viewport of
 * final_fbo (0 for the window). False when the targets cannot be built,
 * in which case nothing was drawn and the caller falls back to Flat. */
static bool RenderCrt(Dkc2SdlPresenter *presenter, GLuint source,
                      int source_width, int source_height,
                      const Dkc2DesktopViewport *viewport, int output_height,
                      GLuint final_fbo) {
  Dkc2CrtFrameParams params;
  if (!Dkc2CrtDerive(&presenter->crt, viewport->width, viewport->height,
                     source_width, source_height, &params))
    return false;
  if (!EnsureCrtTargets(presenter, viewport->width, viewport->height,
                        source_height))
    return false;
  const GLuint *programs = presenter->crt_program;

  /* Lines: decode and resample each source row across the viewport. */
  BindTextureUnit(0, source);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  s_gl.UseProgram(programs[kDkc2CrtPassLines]);
  Uniform1i(programs[kDkc2CrtPassLines], "source", 0);
  Uniform2f(programs[kDkc2CrtPassLines], "source_size", (float)source_width,
            (float)source_height);
  Uniform1f(programs[kDkc2CrtPassLines], "sigma_h", params.sigma_h);
  DrawIntoTarget(programs[kDkc2CrtPassLines], &presenter->crt_lines);

  /* Beam: the vertical profile of every line at the viewport's height. */
  BindTextureUnit(0, presenter->crt_lines.texture);
  s_gl.UseProgram(programs[kDkc2CrtPassBeam]);
  Uniform1i(programs[kDkc2CrtPassBeam], "lines", 0);
  Uniform2f(programs[kDkc2CrtPassBeam], "lines_size",
            (float)presenter->crt_lines.width,
            (float)presenter->crt_lines.height);
  Uniform1f(programs[kDkc2CrtPassBeam], "sigma_dark", params.sigma_dark);
  Uniform1f(programs[kDkc2CrtPassBeam], "sigma_bright", params.sigma_bright);
  Uniform1f(programs[kDkc2CrtPassBeam], "beam_fade", params.beam_fade);
  DrawIntoTarget(programs[kDkc2CrtPassBeam], &presenter->crt_beam);

  /* Glow: a quarter-size reduction blurred both ways; halation: a
   * sixteenth-size reduction of that, blurred again. */
  const GLuint down = programs[kDkc2CrtPassDown];
  const GLuint blur = programs[kDkc2CrtPassBlur];
  const Dkc2GlTarget *chain[2][3] = {
      {&presenter->crt_beam, &presenter->crt_glow[0], &presenter->crt_glow[1]},
      {&presenter->crt_glow[0], &presenter->crt_halo[0],
       &presenter->crt_halo[1]}};
  for (int stage = 0; stage < 2; stage++) {
    const Dkc2GlTarget *input = chain[stage][0];
    const Dkc2GlTarget *a = chain[stage][1];
    const Dkc2GlTarget *b = chain[stage][2];
    BindTextureUnit(0, input->texture);
    s_gl.UseProgram(down);
    Uniform1i(down, "source", 0);
    Uniform2f(down, "source_size", (float)input->width, (float)input->height);
    DrawIntoTarget(down, a);
    BindTextureUnit(0, a->texture);
    s_gl.UseProgram(blur);
    Uniform1i(blur, "source", 0);
    Uniform2f(blur, "source_size", (float)a->width, (float)a->height);
    Uniform2f(blur, "direction", 1.0f, 0.0f);
    DrawIntoTarget(blur, b);
    BindTextureUnit(0, b->texture);
    Uniform2f(blur, "direction", 0.0f, 1.0f);
    DrawIntoTarget(blur, a);
  }

  /* Compose into the viewport of the final framebuffer. */
  s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, final_fbo);
  glViewport(viewport->x, output_height - viewport->y - viewport->height,
             viewport->width, viewport->height);
  const GLuint compose = programs[kDkc2CrtPassCompose];
  BindTextureUnit(2, presenter->crt_halo[0].texture);
  BindTextureUnit(1, presenter->crt_glow[0].texture);
  BindTextureUnit(0, presenter->crt_beam.texture);
  s_gl.UseProgram(compose);
  Uniform1i(compose, "image", 0);
  Uniform1i(compose, "glow", 1);
  Uniform1i(compose, "halo", 2);
  Uniform2f(compose, "target_size", (float)viewport->width,
            (float)viewport->height);
  Uniform1f(compose, "glow_amount", params.glow);
  Uniform1f(compose, "halo_amount", params.halation);
  Uniform2f(compose, "curvature", params.curvature_x, params.curvature_y);
  Uniform1f(compose, "corner_radius", params.corner_radius);
  Uniform1f(compose, "vignette", params.vignette);
  Uniform1i(compose, "mask_kind",
            params.mask == kDkc2CrtMaskNone ? 0
            : params.mask == kDkc2CrtMaskSlot ? 2 : 1);
  Uniform1f(compose, "mask_pitch",
            params.mask_pitch > 0.0f ? params.mask_pitch : 3.0f);
  Uniform1f(compose, "mask_strength", params.mask_strength);
  Uniform1f(compose, "mask_gain", params.mask_gain);
  Uniform1f(compose, "knee", params.knee);
  DrawFrameQuad(true);
  s_gl.UseProgram(0);
  BindTextureUnit(2, 0);
  BindTextureUnit(1, 0);
  BindTextureUnit(0, source);
  return true;
}

/* Clear the framebuffer and draw the frame into its viewport with the
 * display in effect. Shared by the window and the capture, so both show
 * the same chain. */
void Dkc2GlPipelineRender(Dkc2SdlPresenter *presenter, GLuint final_fbo,
                        GLuint source, int source_width, int source_height,
                        int output_width, int output_height,
                        const Dkc2DesktopViewport *viewport) {
  if (s_gl.fbo) s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, final_fbo);
  glViewport(0, 0, output_width, output_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (presenter->display == kDkc2DisplayCrt &&
      RenderCrt(presenter, source, source_width, source_height, viewport,
                output_height, final_fbo))
    return;
  if (presenter->display == kDkc2DisplayCrt) {
    /* The targets could not be built: report once and stay flat. */
    presenter->display = kDkc2DisplayFlat;
    if (presenter->crt_error[0])
      fprintf(stderr, "warning: %s; using flat\n", presenter->crt_error);
    if (s_gl.fbo) s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, final_fbo);
  }
  RenderFlat(presenter, source, source_width, source_height, viewport,
             output_height);
}

/* Shows the source frame in the window with the display in effect, takes
 * an armed capture of it, draws the overlay, and swaps. */
static void ShowFrame(Dkc2SdlPresenter *presenter, GLuint source,
                      int source_width, int source_height,
                      Dkc2SdlOverlayDraw overlay_draw, void *overlay_user) {
  SDL_Window *window = (SDL_Window *)presenter->window;
  int output_width = 0;
  int output_height = 0;
  SDL_GL_GetDrawableSize(window, &output_width, &output_height);
  Dkc2DesktopViewport viewport;
  if (!Dkc2DesktopComputeViewport(output_width, output_height,
                                  source_width, source_height, &viewport))
    return;

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  Dkc2GlPipelineRender(presenter, 0, source, source_width, source_height,
                       output_width, output_height, &viewport);
  /* Offscreen capture: draw the same frame into a framebuffer object and
   * read it back. A hidden window's back buffer reads back empty on macOS,
   * so the capture never depends on the window being displayed. */
  if (presenter->capture_rgb && !presenter->capture_done &&
      presenter->capture_width == output_width &&
      presenter->capture_height == output_height && s_gl.fbo) {
    GLuint fbo = 0, color = 0;
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, output_width, output_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, source);
    s_gl.GenFramebuffers(1, &fbo);
    s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, fbo);
    s_gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, color, 0);
    if (s_gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) ==
        GL_FRAMEBUFFER_COMPLETE_EXT) {
      Dkc2GlPipelineRender(presenter, fbo, source, source_width,
                           source_height, output_width, output_height,
                           &viewport);
      s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, fbo);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, output_width, output_height, GL_RGB,
                   GL_UNSIGNED_BYTE, presenter->capture_rgb);
      const size_t row = (size_t)output_width * 3u;
      uint8_t *tmp = (uint8_t *)malloc(row);
      if (tmp) {
        for (int y = 0; y < output_height / 2; y++) {
          uint8_t *a = presenter->capture_rgb + (size_t)y * row;
          uint8_t *b = presenter->capture_rgb +
                       (size_t)(output_height - 1 - y) * row;
          memcpy(tmp, a, row);
          memcpy(a, b, row);
          memcpy(b, tmp, row);
        }
        free(tmp);
      }
      presenter->capture_done = true;
    }
    s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    s_gl.DeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  if (overlay_draw) {
    glViewport(0, 0, output_width, output_height);
    overlay_draw(overlay_user, output_width, output_height);
  }
  glFlush();
  SDL_GL_SwapWindow(window);
}

bool Dkc2SdlPresenterPresent(Dkc2SdlPresenter *presenter,
                             const uint8_t *pixels, int source_width,
                             int source_height,
                             Dkc2SdlOverlayDraw overlay_draw,
                             void *overlay_user) {
  if (!presenter || !presenter->window || !presenter->gl_context ||
      !presenter->texture || !pixels || source_width <= 0 ||
      source_height <= 0)
    return false;
  if (SDL_GL_MakeCurrent((SDL_Window *)presenter->window,
                         (SDL_GLContext)presenter->gl_context) != 0)
    return false;
  glBindTexture(GL_TEXTURE_2D, presenter->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (presenter->texture_width != source_width ||
      presenter->texture_height != source_height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, source_width, source_height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    presenter->texture_width = source_width;
    presenter->texture_height = source_height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source_width, source_height,
                    GL_BGRA, GL_UNSIGNED_BYTE, pixels);
  }
  ShowFrame(presenter, presenter->texture, source_width, source_height,
            overlay_draw, overlay_user);
  return true;
}

bool Dkc2SdlPresenterHdAvailable(const Dkc2SdlPresenter *presenter) {
  return presenter && presenter->hd_program[0] != 0;
}

/* The 1x input textures and the output-sized targets of a scene (the
 * targets are rebuilt only when the output size changes). */
static bool PrepareHdTargets(Dkc2SdlPresenter *presenter, int width,
                             int height, bool layer) {
  if (presenter->hd_output.width != width ||
      presenter->hd_output.height != height) {
    DestroyTarget(&presenter->hd_output);
    DestroyTarget(&presenter->hd_layer);
    if (!CreateTarget(&presenter->hd_output, width, height, GL_RGBA8,
                      GL_NEAREST))
      return false;
  }
  if (layer && !presenter->hd_layer.texture &&
      !CreateTarget(&presenter->hd_layer, width, height, GL_RGBA8,
                    GL_NEAREST))
    return false;
  return true;
}

static void UploadHdInput(GLuint texture, const uint32_t *pixels, int width,
                          int height) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA,
                  GL_UNSIGNED_BYTE, pixels);
}

/* The slot holding an image id, or the free slot where it goes. Image ids
 * are never 0. */
static unsigned HdImageSlot(const Dkc2SdlPresenter *presenter, uint64_t id) {
  unsigned slot = (unsigned)((id * 0x9e3779b97f4a7c15ull) >> 51) &
                  (kDkc2HdImageSlots - 1u);
  while (presenter->hd_images[slot].id && presenter->hd_images[slot].id != id)
    slot = (slot + 1u) & (kDkc2HdImageSlots - 1u);
  return slot;
}

/* Deletes the uploads the scene being drawn has not used and reinserts the
 * rest into a cleared table so every probe sequence stays unbroken. */
static void DropUnusedHdImages(Dkc2SdlPresenter *presenter) {
  static Dkc2GlHdImage kept[kDkc2HdImageMax];
  unsigned kept_count = 0;
  for (unsigned i = 0; i < kDkc2HdImageSlots; i++) {
    const Dkc2GlHdImage *entry = &presenter->hd_images[i];
    if (!entry->id) continue;
    if (entry->serial == presenter->hd_serial)
      kept[kept_count++] = *entry;
    else
      glDeleteTextures(1, &entry->texture);
  }
  memset(presenter->hd_images, 0, sizeof presenter->hd_images);
  for (unsigned i = 0; i < kept_count; i++)
    presenter->hd_images[HdImageSlot(presenter, kept[i].id)] = kept[i];
  presenter->hd_image_count = kept_count;
}

/* The uploaded texture of a scene image, 0 when none can be made. Uploads
 * live under the image id; when the table is full, those the scene being
 * drawn has not used are dropped. */
static GLuint HdImageTexture(Dkc2SdlPresenter *presenter,
                             const Dkc2HdImage *image) {
  unsigned slot = HdImageSlot(presenter, image->id);
  Dkc2GlHdImage *found = &presenter->hd_images[slot];
  if (found->id) {
    found->serial = presenter->hd_serial;
    return found->texture;
  }
  if (presenter->hd_image_count == kDkc2HdImageMax) {
    DropUnusedHdImages(presenter);
    if (presenter->hd_image_count == kDkc2HdImageMax) return 0;
    slot = HdImageSlot(presenter, image->id);
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  if (!texture) return 0;
  /* Unit 0: during the sprite pass unit 1 holds the occlusion map. */
  BindTextureUnit(0, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width, image->height, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, image->pixels);
  presenter->hd_images[slot] =
      (Dkc2GlHdImage){image->id, texture, presenter->hd_serial};
  presenter->hd_image_count++;
  return texture;
}

/* Draws quads with the quad program into the bound target (width by
 * height output pixels). */
static void DrawHdQuads(Dkc2SdlPresenter *presenter, const Dkc2HdScene *scene,
                        const Dkc2HdQuad *quads, unsigned count, int kind,
                        int width, int height) {
  const GLuint program = presenter->hd_program[kDkc2HdPassQuad];
  s_gl.UseProgram(program);
  Uniform1i(program, "image", 0);
  Uniform1i(program, "occlusion", 1);
  Uniform1i(program, "tile_mask", 2);
  Uniform2f(program, "native_size", (float)scene->width,
            (float)scene->height);
  Uniform1f(program, "scale", (float)scene->scale);
  Uniform1i(program, "kind", kind);
  const GLint rect = s_gl.GetUniformLocation(program, "rect");
  for (unsigned q = 0; q < count; q++) {
    const Dkc2HdQuad *quad = &quads[q];
    const GLuint texture = HdImageTexture(presenter, quad->image);
    if (!texture) continue;
    BindTextureUnit(0, texture);
    Uniform2f(program, "image_size", (float)quad->image->width,
              (float)quad->image->height);
    s_gl.Uniform4f(rect, (float)quad->x, (float)quad->y, (float)quad->width,
                   (float)quad->height);
    Uniform2f(program, "flip", quad->flip_x ? 1.0f : 0.0f,
              quad->flip_y ? 1.0f : 0.0f);
    Uniform1f(program, "brightness", (float)quad->brightness);
    Uniform1f(program, "rank", (float)quad->rank);
    Uniform1f(program, "index", (float)quad->index);
    /* Output row 0 is the target's first row (texture coordinate 0). */
    const float left = 2.0f * (float)quad->x / (float)width - 1.0f;
    const float right =
        2.0f * (float)(quad->x + quad->width) / (float)width - 1.0f;
    const float bottom = 2.0f * (float)quad->y / (float)height - 1.0f;
    const float top =
        2.0f * (float)(quad->y + quad->height) / (float)height - 1.0f;
    glBegin(GL_QUADS);
    glVertex2f(left, bottom);
    glVertex2f(right, bottom);
    glVertex2f(right, top);
    glVertex2f(left, top);
    glEnd();
  }
}

/* Draws a scene into hd_output in the order dkc2_hd.h gives. False when
 * the targets cannot be built. */
static bool ComposeHd(Dkc2SdlPresenter *presenter, const Dkc2HdScene *scene) {
  const int width = scene->width * scene->scale;
  const int height = scene->height * scene->scale;
  if (!PrepareHdTargets(presenter, width, height, scene->layer != NULL))
    return false;
  presenter->hd_serial = scene->serial;
  /* All inputs get storage together, so a later scene with a layer or
   * tiles finds its texture sized even when earlier ones had none. */
  if (presenter->hd_input_width != scene->width) {
    for (int input = 0; input < 3; input++) {
      glBindTexture(GL_TEXTURE_2D, presenter->hd_input[input]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scene->width, scene->height,
                   0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, presenter->hd_input[3]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, scene->width,
                 scene->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    presenter->hd_input_width = scene->width;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  UploadHdInput(presenter->hd_input[0], scene->base, scene->width,
                scene->height);
  UploadHdInput(presenter->hd_input[1], scene->occlusion, scene->width,
                scene->height);
  if (scene->layer)
    UploadHdInput(presenter->hd_input[2], scene->layer, scene->width,
                  scene->height);
  if (scene->tile_count) {
    /* One byte per pixel: rows are not 4-byte aligned. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, presenter->hd_input[3]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, scene->width, scene->height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, scene->tile_mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  }
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);

  const GLuint upscale = presenter->hd_program[kDkc2HdPassUpscale];
  s_gl.UseProgram(upscale);
  Uniform1i(upscale, "source", 0);
  Uniform2f(upscale, "source_size", (float)scene->width,
            (float)scene->height);
  Uniform1f(upscale, "scale", (float)scene->scale);
  Uniform1i(upscale, "smooth",
            scene->background == kDkc2HdFilterSmooth);
  BindTextureUnit(0, presenter->hd_input[0]);
  DrawIntoTarget(upscale, &presenter->hd_output);
  BindTextureUnit(1, presenter->hd_input[1]);
  /* Blended passes keep the output opaque, as the CPU blend does. */
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  if (scene->tile_count) {
    BindTextureUnit(2, presenter->hd_input[3]);
    glEnable(GL_BLEND);
    DrawHdQuads(presenter, scene, scene->tiles, scene->tile_count,
                kHdQuadTile, width, height);
    glDisable(GL_BLEND);
  }
  if (scene->layer) {
    BindTextureUnit(0, presenter->hd_input[2]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    DrawIntoTarget(upscale, &presenter->hd_layer);
    DrawHdQuads(presenter, scene, scene->art, scene->art_count, kHdQuadArt,
                width, height);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    const GLuint composite = presenter->hd_program[kDkc2HdPassComposite];
    BindTextureUnit(0, presenter->hd_layer.texture);
    s_gl.UseProgram(composite);
    Uniform1i(composite, "layer", 0);
    Uniform1i(composite, "occlusion", 1);
    Uniform2f(composite, "output_size", (float)width, (float)height);
    Uniform2f(composite, "native_size", (float)scene->width,
              (float)scene->height);
    Uniform1f(composite, "scale", (float)scene->scale);
    glEnable(GL_BLEND);
    DrawIntoTarget(composite, &presenter->hd_output);
  } else {
    s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, presenter->hd_output.fbo);
    glViewport(0, 0, width, height);
  }
  glEnable(GL_BLEND);
  DrawHdQuads(presenter, scene, scene->sprites, scene->sprite_count,
              kHdQuadSprite, width, height);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  s_gl.UseProgram(0);
  BindTextureUnit(2, 0);
  BindTextureUnit(1, 0);
  BindTextureUnit(0, 0);
  s_gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
  return true;
}

bool Dkc2SdlPresenterPresentHd(Dkc2SdlPresenter *presenter,
                               const Dkc2HdScene *scene,
                               Dkc2SdlOverlayDraw overlay_draw,
                               void *overlay_user) {
  if (!Dkc2SdlPresenterHdAvailable(presenter) || !presenter->window ||
      !presenter->gl_context || !scene)
    return false;
  if (SDL_GL_MakeCurrent((SDL_Window *)presenter->window,
                         (SDL_GLContext)presenter->gl_context) != 0)
    return false;
  if (scene->serial != presenter->hd_serial && !ComposeHd(presenter, scene))
    return false;
  ShowFrame(presenter, presenter->hd_output.texture,
            scene->width * scene->scale, scene->height * scene->scale,
            overlay_draw, overlay_user);
  return true;
}

void Dkc2SdlPresenterSetTitle(Dkc2SdlPresenter *presenter,
                              const char *title) {
  if (presenter && presenter->window && title)
    SDL_SetWindowTitle((SDL_Window *)presenter->window, title);
}

bool Dkc2SdlPresenterSetFullscreen(Dkc2SdlPresenter *presenter,
                                   bool fullscreen) {
  if (!presenter || !presenter->window)
    return false;
  Uint32 flags = fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
  return SDL_SetWindowFullscreen((SDL_Window *)presenter->window, flags) == 0;
}

bool Dkc2SdlPresenterIsFullscreen(const Dkc2SdlPresenter *presenter) {
  if (!presenter || !presenter->window)
    return false;
  return (SDL_GetWindowFlags((SDL_Window *)presenter->window) &
          (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

const char *Dkc2SdlPresenterBackend(const Dkc2SdlPresenter *presenter) {
  return presenter && presenter->backend[0] ? presenter->backend : "SDL2";
}

Dkc2DesktopVsyncStatus Dkc2SdlPresenterVsyncStatus(
    const Dkc2SdlPresenter *presenter) {
  return presenter ? presenter->vsync_status
                   : kDkc2DesktopVsyncUnsupported;
}

void *Dkc2SdlPresenterNativeWindow(const Dkc2SdlPresenter *presenter) {
  if (!presenter || !presenter->window) return NULL;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo((SDL_Window *)presenter->window, &info))
    return NULL;
#if defined(SDL_VIDEO_DRIVER_COCOA)
  if (info.subsystem == SDL_SYSWM_COCOA) return (void *)info.info.cocoa.window;
#endif
  return NULL;
}

bool Dkc2SdlPresenterUsesSoftwarePacing(
    const Dkc2SdlPresenter *presenter) {
  return presenter && presenter->software_paced;
}

void Dkc2GlPipelineDestroy(Dkc2SdlPresenter *presenter) {
  if (!presenter) return;
  DestroyCrtTargets(presenter);
  for (int pass = 0; pass < kDkc2CrtPassCount; pass++) {
    if (presenter->crt_program[pass]) s_gl.DeleteProgram(presenter->crt_program[pass]);
    presenter->crt_program[pass] = 0;
  }
  if (presenter->program) s_gl.DeleteProgram(presenter->program);
  presenter->program = 0;
  for (int pass = 0; pass < kDkc2HdPassCount; pass++) {
    if (presenter->hd_program[pass])
      s_gl.DeleteProgram(presenter->hd_program[pass]);
    presenter->hd_program[pass] = 0;
  }
  if (presenter->hd_input[0]) glDeleteTextures(4, presenter->hd_input);
  memset(presenter->hd_input, 0, sizeof presenter->hd_input);
  for (unsigned i = 0; i < kDkc2HdImageSlots; i++)
    if (presenter->hd_images[i].id)
      glDeleteTextures(1, &presenter->hd_images[i].texture);
  memset(presenter->hd_images, 0, sizeof presenter->hd_images);
  presenter->hd_image_count = 0;
  DestroyTarget(&presenter->hd_output);
  DestroyTarget(&presenter->hd_layer);
}

void Dkc2SdlPresenterDestroy(Dkc2SdlPresenter *presenter) {
  if (!presenter) return;
  if (presenter->window && presenter->gl_context)
    (void)SDL_GL_MakeCurrent((SDL_Window *)presenter->window,
                            (SDL_GLContext)presenter->gl_context);
  if (presenter->gl_context) Dkc2GlPipelineDestroy(presenter);
  if (presenter->texture) glDeleteTextures(1, &presenter->texture);
  if (presenter->gl_context)
    SDL_GL_DeleteContext((SDL_GLContext)presenter->gl_context);
  if (presenter->window) SDL_DestroyWindow((SDL_Window *)presenter->window);
  memset(presenter, 0, sizeof *presenter);
}
