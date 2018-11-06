#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 1) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(binding = 10) uniform sampler2D tex0_sampler;
layout(r11f_g11f_b10f, binding = 11, location=7) uniform image2D my_image1;
layout(r8, binding = 12, location=8) uniform image2D my_image2;

struct Light
{
  vec3 eyePosOrDir;
  bool isDirectional;
  vec3 intensity;
  float attenuation;
} variableName;

layout(location=5) in test_multiloc {
    vec3 foo;
    layout(location=3) vec2 bar;
    float what;
} test_instance;

layout(location=9) in test_anon_multiloc {
    layout(location=11) vec3 anonfoo;
    vec2 anonbar;
    layout(location=10) float anonwhat;
};

layout(binding = 1) uniform ngl_uniforms_block {
    mat4 tex0_coord_matrix;
    vec2 tex0_dimensions;
    float tex0_ts;
} ngl_uniforms;

layout(binding = 3) uniform anon_block_1 {
    vec3 foo;
    vec2 bar;
};

layout(binding = 2) uniform anon_block_2 {
    mat4 yep;
    float xxx;
};

void main() {
    outColor = texture(tex0_sampler, fragTexCoord) * xxx;
    outColor = outColor * (1.0 + (ngl_uniforms.tex0_ts / 5.0));
}
