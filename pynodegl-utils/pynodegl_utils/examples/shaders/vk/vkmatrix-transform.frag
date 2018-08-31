#version 450
#extension GL_ARB_separate_shader_objects : enable

precision mediump float;

layout(binding=0) uniform Parameter {
    vec4 color;
};
// uniform sampler2D tex0_sampler;
layout(location = 0) out vec4 output_color;

void main(void)
{
    output_color = color;
}
