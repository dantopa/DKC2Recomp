#version 460
/* desktop_present_sdl.c kHdUpscaleFragmentSource for deko3d: a 1x input
 * (native size) nearest, or premultiplied Catmull-Rom kept within the 2x2
 * source pixels nearest the output pixel. */
layout (std140, binding = 0) uniform Draw {
  vec4 rect;
  vec4 target;
  vec4 uv;
  vec4 source_rect;
  vec4 add;
  vec4 flip;
  vec4 native;   /* source width, height; scale; smooth */
  vec4 order;
};

layout (location = 0) out vec4 out_color;
layout (binding = 0) uniform sampler2D source;

vec4 texel(vec2 p) {
  p = clamp(p, vec2(0.0), native.xy - 1.0);
  return texture(source, (p + 0.5) / native.xy);
}

vec4 premultiplied(vec2 p) {
  vec4 c = texel(p);
  return vec4(c.rgb * c.a, c.a);
}

float weight(float t) {
  t = abs(t);
  if (t < 1.0) return (1.5 * t - 2.5) * t * t + 1.0;
  if (t < 2.0) return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
  return 0.0;
}

vec4 weights(float t) {
  return vec4(weight(t), weight(t - 1.0), weight(t - 2.0), weight(t - 3.0));
}

vec4 row(vec2 p, vec4 w) {
  return w.x * premultiplied(p) + w.y * premultiplied(p + vec2(1.0, 0.0))
       + w.z * premultiplied(p + vec2(2.0, 0.0))
       + w.w * premultiplied(p + vec2(3.0, 0.0));
}

void widen(vec4 c, inout vec3 low, inout vec3 high, inout bool any) {
  if (c.a < 0.5) return;
  low = min(low, c.rgb);
  high = max(high, c.rgb);
  any = true;
}

void main() {
  float scale = native.z;
  vec2 o = floor(gl_FragCoord.xy);
  vec2 cell = floor((o + 0.5) / scale);
  if (native.w == 0.0) {
    out_color = texel(cell);
    return;
  }
  vec2 position = (o - cell * scale + 0.5) / scale - 0.5;
  vec2 offset = vec2(position.x < 0.0 ? -2.0 : -1.0,
                     position.y < 0.0 ? -2.0 : -1.0);
  vec2 first = cell + offset;
  vec4 wx = weights(position.x - offset.x);
  vec4 wy = weights(position.y - offset.y);
  vec4 sum = wy.x * row(first, wx)
           + wy.y * row(first + vec2(0.0, 1.0), wx)
           + wy.z * row(first + vec2(0.0, 2.0), wx)
           + wy.w * row(first + vec2(0.0, 3.0), wx);
  vec2 near = clamp(first + 1.0, vec2(0.0), native.xy - 1.0);
  vec3 low = vec3(1.0);
  vec3 high = vec3(0.0);
  bool any = false;
  widen(texel(near), low, high, any);
  widen(texel(near + vec2(1.0, 0.0)), low, high, any);
  widen(texel(near + vec2(0.0, 1.0)), low, high, any);
  widen(texel(near + vec2(1.0, 1.0)), low, high, any);
  if (sum.a * 255.0 < 1.0 || !any) {
    out_color = vec4(0.0);
    return;
  }
  out_color = vec4(clamp(sum.rgb / sum.a, low, high), min(sum.a, 1.0));
}
