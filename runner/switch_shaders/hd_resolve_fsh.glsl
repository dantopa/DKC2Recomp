#version 460
/* desktop_present_sdl.c kHdResolveFragmentSource for deko3d: one of an
 * unresolved scene's 1x maps (0 base, 1 occlusion, 2 tile mask, 3 BG1
 * layer). Values are bytes (0-255); table words upload as BGRA, so their
 * byte 0 reads as b, 1 as g, 2 as r and 3 as a. */
layout (std140, binding = 0) uniform Draw {
  vec4 rect;
  vec4 target;
  vec4 uv;
  vec4 columns;  /* shown first, end; screen art first, end */
  vec4 add;
  vec4 flip;
  vec4 native;   /* native width, height; layered; map */
  vec4 order;
};

layout (location = 0) out vec4 out_color;
layout (binding = 0) uniform sampler2D frame;
layout (binding = 1) uniform sampler2D bg1;
layout (binding = 2) uniform sampler2D bg2;
layout (binding = 3) uniform sampler2D bg3;
layout (binding = 4) uniform sampler2D obj;
layout (binding = 5) uniform sampler2D native_pixels;
layout (binding = 6) uniform sampler2D owner;
layout (binding = 7) uniform sampler2D loose;
layout (binding = 8) uniform sampler2D records;
layout (binding = 9) uniform sampler2D lines;
layout (binding = 10) uniform sampler2D math;

vec4 bytes(sampler2D image, vec2 at, vec2 size) {
  return floor(texture(image, (at + 0.5) / size) * 255.0 + 0.5);
}

vec4 at(sampler2D image, vec2 p) { return bytes(image, p, native.xy); }

bool present(vec4 c) { return c != vec4(0.0); }

float added(float value, float five) {
  float level = bytes(math, vec2(value, 0.0), vec2(256.0, 1.0)).b;
  return bytes(math, vec2(level + five, 0.0), vec2(256.0, 1.0)).g;
}

bool math_shows(vec4 c, float rank, vec3 pixel, float add) {
  if (add == 0.0 || !present(c) || c.a != rank) return false;
  vec3 five = vec3(mod(add, 32.0), mod(floor(add / 32.0), 32.0),
                   floor(add / 1024.0));
  return pixel == c.rgb ||
         pixel == vec3(added(c.r, five.r), added(c.g, five.g),
                       added(c.b, five.b));
}

void main() {
  bool layered = native.z > 0.0;
  vec2 p = floor(gl_FragCoord.xy);
  vec4 b1 = at(bg1, p), b2 = at(bg2, p), b3 = at(bg3, p);
  vec3 pixel = at(frame, p).rgb;
  float background = max(b1.a, max(b2.a, b3.a));
  float rank = max(b2.a, b3.a);
  if (!layered) rank = max(rank, b1.a);
  if (p.x < columns.x || p.x >= columns.y) background = 255.0;
  vec4 hide = vec4(background, 0.0, 0.0, 0.0);
  vec4 object = at(obj, p);
  float own = at(owner, p).r;
  bool own_pixel = own > 0.0 && object.rgb == at(native_pixels, p).rgb;
  vec4 record = bytes(records, vec2(own, 0.0), vec2(256.0, 1.0));
  if (present(object) && !(own_pixel && record.b > 0.0) &&
      at(loose, p).r == 0.0) {
    if (own_pixel) hide.b = record.g; else hide.g = object.a;
    if (object.a > rank) { pixel = object.rgb; rank = object.a; }
  }
  if (layered && present(b1) && b1.a > rank) pixel = b1.rgb;
  float mask = 0.0;
  if (present(b2) && b2.a == rank && b2.rgb == pixel) mask = 2.0;
  if (present(b3) && b3.a == rank && b3.rgb == pixel) mask = 3.0;
  vec4 line = bytes(lines, vec2(0.0, p.y), vec2(1.0, native.y));
  if (math_shows(b2, rank, pixel, line.b + line.g * 256.0)) mask = 130.0;
  if (math_shows(b3, rank, pixel, line.r + line.a * 256.0)) mask = 131.0;
  if (layered) {
    float bg1_rank = b1.a;
    if (!present(b1)) {
      const vec2 around[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0),
                                     vec2(0.0, 1.0), vec2(0.0, -1.0));
      for (int n = 0; n < 4; n++) {
        vec2 q = p + around[n];
        if (q.x < 0.0 || q.y < 0.0 || q.x >= native.x || q.y >= native.y)
          continue;
        vec4 c = at(bg1, q);
        if (present(c) && c.a > bg1_rank) bg1_rank = c.a;
      }
    }
    if (bg1_rank == 0.0 && p.x >= columns.z && p.x < columns.w)
      bg1_rank = 1.0;
    if (bg1_rank > rank) hide.a = 255.0;
  }
  vec4 result = hide;
  if (native.w == 0.0) result = vec4(pixel, 255.0);
  else if (native.w == 2.0) result = vec4(mask, 0.0, 0.0, 0.0);
  else if (native.w == 3.0)
    result = present(b1) ? vec4(b1.rgb, 255.0) : vec4(0.0);
  out_color = result / 255.0;
}
