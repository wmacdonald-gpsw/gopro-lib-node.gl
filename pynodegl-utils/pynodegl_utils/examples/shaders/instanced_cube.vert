#version 330
precision highp float;
attribute vec4 ngl_position;
attribute vec2 ngl_uvcoord;
attribute vec3 ngl_normal;
uniform mat4 ngl_modelview_matrix;
uniform mat4 ngl_projection_matrix;
uniform mat3 ngl_normal_matrix;

attribute mat4 instance_transform;
attribute vec4 instance_color;

out vec2 var_uvcoord;
out vec3 var_normal;
out vec4 var_color;


void main()
{
    gl_Position = ngl_projection_matrix * ngl_modelview_matrix * (instance_transform * ngl_position);
    var_uvcoord = ngl_uvcoord;
    var_normal = ngl_normal_matrix * ngl_normal;
    var_color = instance_color;
}
