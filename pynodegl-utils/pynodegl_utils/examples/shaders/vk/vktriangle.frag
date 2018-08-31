#version 450
#extension GL_ARB_separate_shader_objects : enable

precision mediump float;
layout(location = 1) in vec4 var_color;
layout(location = 0) out vec4 output_color;

void main(void)
{
    output_color = var_color;
}
