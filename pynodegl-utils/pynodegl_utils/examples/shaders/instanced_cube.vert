precision highp float;
in vec4 ngl_position;
in vec2 ngl_uvcoord;
in vec3 ngl_normal;
uniform mat4 ngl_modelview_matrix;
uniform mat4 ngl_projection_matrix;
uniform mat3 ngl_normal_matrix;

in mat4 instance_transform;
in vec4 instance_color;

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
