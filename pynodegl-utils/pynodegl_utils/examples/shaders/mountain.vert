#version 100

precision highp float;
attribute vec4 ngl_position;
attribute vec2 ngl_uvcoord;
varying float var_pos;
varying float var_noise;
attribute vec3 ngl_normal;
uniform mat4 ngl_modelview_matrix;
uniform mat4 ngl_projection_matrix;
uniform mat3 ngl_normal_matrix;

uniform mat4 tex0_coord_matrix;
uniform vec2 tex0_dimensions;

varying vec2 var_uvcoord;
varying vec3 var_normal;
varying vec2 var_tex0_coord;

uniform float time;

void main()
{
    gl_Position = ngl_position;
    var_uvcoord = ngl_uvcoord;
    var_normal = ngl_normal_matrix * ngl_normal;
    var_tex0_coord = (tex0_coord_matrix * vec4(ngl_uvcoord, 0, 1)).xy;
    var_pos = var_uvcoord.x + time;
    var_noise = fract(var_pos * 8.0);
}
