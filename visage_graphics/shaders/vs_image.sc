$input a_position, a_color0, a_color1, a_color2, a_texcoord0, a_texcoord1, a_texcoord2
$output v_coordinates, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos, v_shader_values

// Feathers: an image quad. a_texcoord0.zw is TEXELS PER PIXEL per axis (ImageAtlas::setImageCoordinates for an
// ImageVertex), so the shift a clip makes in pixels moves the texture coordinate by the texels under those pixels.
// vs_tinted_texture shifts by the pixels themselves, which is only right for an image drawn at its own size: a
// stretched image that was clipped showed the whole picture squashed into the visible part, and sampled past its own
// sub-rect into its atlas neighbours. a_texcoord2 is that sub-rect (texels, half-texel inset), clamped to in fs_image.

#include <shader_include.sh>

uniform vec4 u_bounds;
uniform vec4 u_atlas_scale;

void main() {
  vec2 min = a_texcoord1.xy;
  vec2 max = a_texcoord1.zw;
  vec2 clamped = clamp(a_position.xy, min, max);
  vec2 delta = clamped - a_position.xy;

  v_position = clamped;
  v_gradient_texture_pos = a_color0;
  v_gradient_pos = a_color1;
  v_gradient_pos2 = a_color2;
  v_coordinates = (a_texcoord0.xy + delta * a_texcoord0.zw) * u_atlas_scale.xy;
  v_shader_values = a_texcoord2 * u_atlas_scale.xyxy;
  vec2 adjusted_position = clamped * u_bounds.xy + u_bounds.zw;
  gl_Position = vec4(adjusted_position, 0.5, 1.0);
}
