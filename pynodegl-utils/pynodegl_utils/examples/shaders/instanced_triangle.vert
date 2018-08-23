#version 330
precision highp float;
in vec4 ngl_position;
in vec2 ngl_uvcoord;
in vec3 ngl_normal;
uniform mat4 ngl_modelview_matrix;
uniform mat4 ngl_projection_matrix;
uniform mat3 ngl_normal_matrix;

in vec4 edge_color;
in vec4 instance_position;
in mat4 instance_transformation;

out vec2 var_uvcoord;
out vec3 var_normal;
out vec4 var_triangle_color;

void main()
{
    gl_Position = ngl_projection_matrix * ngl_modelview_matrix * (instance_position + instance_transformation * ngl_position);
    var_uvcoord = ngl_uvcoord;
    var_normal = ngl_normal_matrix * ngl_normal;
    var_triangle_color = edge_color;
}
