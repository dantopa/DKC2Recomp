#version 460
/* desktop_present_sdl.c kHdCompositeFragmentSource for deko3d: the BG1
 * layer where the occlusion map puts it in front. */
layout (std140, binding = 0) uniform Draw {
  vec4 rect;
  vec4 target;   /* output width, height */
  vec4 uv;
  vec4 source;
  vec4 add;
  vec4 flip;
  vec4 native;   /* native width, height; scale */
  vec4 order;
};

layout (location = 0) out vec4 out_color;
layout (binding = 0) uniform sampler2D layer;
layout (binding = 1) uniform sampler2D occlusion;

void main() {
  vec2 o = floor(gl_FragCoord.xy);
  vec4 color = texture(layer, (o + 0.5) / target.xy);
  vec2 pixel = floor((o + 0.5) / native.z);
  float front = texture(occlusion, (pixel + 0.5) / native.xy).a;
  if (color.a == 0.0 || front < 0.5) discard;
  out_color = color;
}
