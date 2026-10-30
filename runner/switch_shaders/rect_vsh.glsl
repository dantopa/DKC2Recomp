#version 460
/* A rectangle of the bound target, in pixels from its top-left corner,
 * drawn as a four-vertex triangle strip. Every deko3d pass shares this
 * block (switch_present_deko3d.c, DekoDraw). */
layout (std140, binding = 0) uniform Draw {
  vec4 rect;     /* x, y, width, height */
  vec4 target;   /* target width, height */
  vec4 uv;       /* texture coordinates at the top-left and bottom-right */
  vec4 source;   /* quad: image source rectangle */
  vec4 add;      /* quad: added color (rgb), brightness */
  vec4 flip;     /* quad: flip x, y; image width, height */
  vec4 native;   /* native width, height; scale; quad kind or smooth */
  vec4 order;    /* quad: rank, index */
};

layout (location = 0) out vec2 out_uv;

void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 pixel = rect.xy + corner * rect.zw;
  /* Clip-space y grows upward while the upper-left origin puts row 0 (and
   * gl_FragCoord.y 0) at the top, so pixel rows map to y from +1 down. */
  vec2 ndc = pixel / target.xy * 2.0 - 1.0;
  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
  out_uv = mix(uv.xy, uv.zw, corner);
}
