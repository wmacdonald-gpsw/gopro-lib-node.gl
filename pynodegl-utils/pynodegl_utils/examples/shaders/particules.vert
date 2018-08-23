precision highp float;

in vec4 ngl_position;
uniform mat4 ngl_modelview_matrix;
uniform mat4 ngl_projection_matrix;
uniform mat3 ngl_normal_matrix;

out vec2 var_uvcoord;

layout(std430, binding = 0) buffer opositions_buffer {
    vec3 opositions[];
};

void main(void)
{
    vec4 position = vec4(opositions[gl_InstanceID], 1.0);
    gl_Position = ngl_projection_matrix * ngl_modelview_matrix * (ngl_position + position);
}
