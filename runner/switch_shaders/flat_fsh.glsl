#version 460
/* The finished frame over the viewport, sampled as the bound sampler says. */
layout (location = 0) in vec2 in_uv;
layout (location = 0) out vec4 out_color;
layout (binding = 0) uniform sampler2D source;

void main() {
  out_color = vec4(texture(source, in_uv).rgb, 1.0);
}
