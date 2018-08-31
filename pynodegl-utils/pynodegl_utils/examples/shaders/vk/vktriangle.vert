#version 450
#extension GL_ARB_separate_shader_objects : enable

precision highp float;

struct Object {
    mat4 modelview_matrix;
    mat4 projection_matrix;
    mat3 normal_matrix;
};

struct Vertex {
    vec4 position;
    vec2 uvcoord;
    vec3 normal;
};

layout(push_constant) uniform PerObject {
    Object ngl_object;
};

// struct not supported in vertex stage
layout(location=0) in vec4 ngl_position;
layout(location=1) in vec2 ngl_uvcoord;
layout(location=2) in vec3 ngl_normal;
layout(location=4) in vec4 edge_color;

layout(location=0) out gl_PerVertex {
    vec4 gl_Position;
};
layout(location=1) out vec4 var_color;

void main()
{
    gl_Position = ngl_object.projection_matrix * ngl_object.modelview_matrix * ngl_position;
    var_color = edge_color;
}
