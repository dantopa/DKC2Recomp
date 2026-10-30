#version 460
/* desktop_present_sdl.c kHdQuadFragmentSource for deko3d: an art, sprite
 * or tile image over an output rectangle (kind 0, 1, 2). */
layout (std140, binding = 0) uniform Draw {
  vec4 rect;     /* x, y, width, height */
  vec4 target;
  vec4 uv;
  vec4 source;   /* image source rectangle */
  vec4 add;      /* added color (rgb), brightness */
  vec4 flip;     /* flip x, y; image width, height */
  vec4 native;   /* native width, height; scale; kind */
  vec4 order;    /* rank, index */
};

layout (location = 0) out vec4 out_color;
layout (binding = 0) uniform sampler2D image;
layout (binding = 1) uniform sampler2D occlusion;
layout (binding = 2) uniform sampler2D tile_mask;

void main() {
  float scale = native.z;
  int kind = int(native.w);
  float rank = order.x;
  vec2 o = floor(gl_FragCoord.xy);
  vec2 local = mix(o - rect.xy, rect.xy + rect.zw - 1.0 - o, flip.xy);
  vec2 cell = source.xy + floor((local * source.zw + 0.5) / rect.zw);
  vec4 color = texture(image, (cell + 0.5) / flip.zw);
  if (kind != 0) {
    if (color.a == 0.0) discard;
    vec2 pixel = floor((o + 0.5) / scale);
    vec2 at = (pixel + 0.5) / native.xy;
    if (kind == 1) {
      vec4 hide = floor(texture(occlusion, at) * 255.0 + 0.5);
      if (rank <= hide.r || (hide.g > 0.0 && hide.g >= rank) ||
          (hide.b > 0.0 && hide.b - 1.0 > order.y)) discard;
    } else if (floor(texture(tile_mask, at).r * 255.0 + 0.5) != rank) {
      discard;
    }
  }
  color.rgb = min(color.rgb + add.rgb, 1.0);
  if (add.w < 255.0)
    color.rgb = floor((floor(color.rgb * 255.0 + 0.5) * add.w + 0.5) / 255.0)
                / 255.0;
  out_color = color;
}
