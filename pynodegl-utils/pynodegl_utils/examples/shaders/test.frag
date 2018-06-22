#version 100
precision mediump float;

uniform float time;
varying vec2 var_uvcoord;

#define PI 3.14159

#define NB_ANGLES 10.0

float fx(float t)
{
    float amp = exp(t*-2.5);
    //return amp;
    float s = 1. - exp(t*-10.0); // wide at the beginning, smaller at the end
    return sin(t*PI*NB_ANGLES*s) * s * amp  * .5 + .5;

    //return (sin(t*PI*5.0) + 1.0) / 2.0;
    //return .5;
}

float fy(float t)
{
    return t;
    return (sin(t*PI*NB_ANGLES) + 1.0) / 2.0 * exp(t*-1.5);
}

void main(void)
{
#if 0
    vec2 p = vec2(fx(time), fy(time));
    float d = distance(p, var_uvcoord);
    float c = step(d, 0.01);
    gl_FragColor = vec4(vec3(c), 1.0);
#else
    float t = var_uvcoord.x;
    float y = var_uvcoord.y;
    float v = 1.0 - fx(t);
    float w = 0.005;
    float c = smoothstep(v-w, v, y)
            - smoothstep(v, v+w, y);
    gl_FragColor = vec4(vec3(c), 1.0);
#endif
}
