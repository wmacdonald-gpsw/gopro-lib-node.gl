precision mediump float;
in vec4 var_color;
in float var_intensity;
out vec4 frag_color;
void main(void)
{
    frag_color = var_color * var_intensity;
}
