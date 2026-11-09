$input v_coordinates, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos, v_shader_values

// Feathers: fs_tinted_texture, with the coordinate held inside the image's own atlas sub-rect (v_shader_values,
// see vs_image) — whatever a clip or a rounding does to the coordinate, no neighbour in the atlas is ever sampled.

#include <shader_include.sh>

SAMPLER2D(s_gradient, 0);
SAMPLER2D(s_texture, 1);

void main() {
  vec4 color = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  vec2 uv = clamp(v_coordinates, v_shader_values.xy, v_shader_values.zw);
  vec4 texel = texture2D(s_texture, uv);
  gl_FragColor = clampToAlpha(color * vec4(texel.rgb * texel.a, texel.a));
}
