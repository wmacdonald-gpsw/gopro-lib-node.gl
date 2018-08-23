#version 330
precision mediump float;
in vec4 var_color;
out vec4 frag_color;
void main(void)
{
    frag_color = var_color;
}
