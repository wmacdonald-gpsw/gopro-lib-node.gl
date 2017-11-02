#version 100

#define PI 3.14159265358979323846

precision mediump float;
varying vec2 var_tex0_coord;
uniform sampler2D tex0_sampler;
uniform int dim;
uniform int nb_layers;
uniform int profile;
uniform float time;

uniform float gain;
uniform float lacunarity;

float f(float t)
{
#if 0
    return t;                               // value
#elif 0
    // equivalent to smoothstep(0.0, 1.0, t)
    return (3.0 - 2.0*t)*t*t;               // 3t^2 - 2t^3          (old Perlin)
#else
    return ((6.0*t - 15.0)*t + 10.0)*t*t*t; // 6t^5 - 15t^4 + 10t^3 (new Perlin)
#endif
}

float pick1d(float pos, float off)
{
    float value_point = fract(pos + off);
    float value = texture2D(tex0_sampler, vec2(value_point, 0)).x;
    return value;
}

float noise1d(float pos)
{
    float d = float(dim);
    float s = 1.0 / d;

    float v0 = pick1d(pos, 0.0 * s);
    float v1 = pick1d(pos, 1.0 * s);

    float t = pos*d - floor(pos*d);
    float tx = f(t);
    float nx = mix(v0, v1, tx);

    return nx;
}

// absolute scaled grad pos in [0;1]
vec2 get_grad_pos(vec2 pos, vec2 grad_direction)
{
    float d = float(dim) - 1.;
    float s = 1.0 / d;
    return floor(pos*d + grad_direction) * s;
}

float grad_dist(vec2 pos, vec2 grad_direction)
{
    float d = float(dim) - 1.;
    float s = 1.0 / d;
    vec2 grad_pos = get_grad_pos(pos, grad_direction);

    // distance to gradient position in [0;1]
    float dist = distance(pos, grad_pos)*d;

    dist *= 2.; // prevent overlap

    return clamp(dist, 0.0, 1.0);
}

float surflets(vec2 pos)
{
    float g00_dist = grad_dist(pos, vec2(0.0, 0.0));
    float g01_dist = grad_dist(pos, vec2(0.0, 1.0));
    float g10_dist = grad_dist(pos, vec2(1.0, 0.0));
    float g11_dist = grad_dist(pos, vec2(1.0, 1.0));

    return ((1. - g00_dist) +
            (1. - g01_dist) +
            (1. - g10_dist) +
            (1. - g11_dist));
}

float pick2d(vec2 pos, vec2 grad_off)
{
    float d = float(dim) - 1.;
    float s = 1.0 / d;

    vec2 grad_pos = floor(pos*d + grad_off) * s;
    vec2 uv = (pos - grad_pos) * d;
    vec2 grad = texture2D(tex0_sampler, grad_pos).xy;
    return dot(grad, uv);
}

// symetric version of f()
// t in [-1;1], output in [0;1] (just like f(t))
float sf(float t)
{
    return 1. - f(abs(t));
}

// pos in [0;1]
// grad is a unit vec (length(grad)==1)
float surflet(vec2 pos, vec2 grad)
{
    vec2 xy = pos*2.0 - 1.0; // make each coord in [-1;1]
    float fx = sf(xy.x); // [0;1]
    float fy = sf(xy.y); // [0;1]
    float g = dot(grad, xy);
    float r = fx*fy*g;
    return r/2.0 + .5;
}

float gradients(vec2 pos)
{
    float d = float(dim);
    float s = 1.0 / d;
    vec2 grad = texture2D(tex0_sampler, pos).xy * 2.0 - 1.0; // [0;1] -> [-1;1]
    vec2 spos = fract(pos * d);
    return surflet(spos, grad);
}

float noise2d(vec2 pos)
{
    return gradients(pos);

    float angle = time * 2.0 * PI;
    vec2 grad = vec2(cos(angle), sin(angle));
    return surflet(pos, grad);

    return surflets(pos);

    float d = float(dim) - 1.;
    float s = 1.0 / d;

    float n00 = pick2d(fract(pos), vec2(0.0, 0.0));
    float n01 = pick2d(fract(pos), vec2(0.0, 1.0));
    float n10 = pick2d(fract(pos), vec2(1.0, 0.0));
    float n11 = pick2d(fract(pos), vec2(1.0, 1.0));

    return n00;

    vec2 uv = pos - floor(pos*d) * s;

    float tx = f(uv.x);
    float ty = f(uv.y);

    float nx0 = mix(n00, n10, tx);
    float nx1 = mix(n01, n11, tx);
    float nxy = mix(nx0, nx1, ty);

    return nxy;
}

float noise3d(vec3 pos)
{
    return 0.0;
}

void main(void)
{
    vec4 color;

    if (profile == 1) { // noise 1d
        float sum = 0.0;
        float max_amp = 0.0;
        float freq = 1.0;
        float amp = 1.0;
        float pos = var_tex0_coord.x/2.0 + time;
        for (int i = 0; i < nb_layers; i++) {
            float nval = noise1d(pos * freq) * amp;
            sum += nval;
            max_amp += amp;
            freq *= lacunarity;
            amp *= gain;
        }
        float n = sum / max_amp;

        float c = step(n, var_tex0_coord.y);
        color = vec4(vec3(c), 1.0);
    } else if (profile == 2) { // noise 2d
#if 0
        float sum = 0.0;
        float max_amp = 0.0;
        float freq = 1.0;
        float amp = 1.0;
        vec2 pos = var_tex0_coord/2.0 + vec2(time);
        for (int i = 0; i < nb_layers; i++) {
            float nval = noise2d(pos * freq) * amp;
            sum += nval;
            max_amp += amp;
            freq *= lacunarity;
            amp *= gain;
        }
        float n = sum / max_amp;
#else
        float n = noise2d(var_tex0_coord);
#endif
#if 1
        color = vec4(vec3(n), 1.0);
#else
        vec4 c1 = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 c2 = vec4(1.0, 0.6, 0.7, 1.0);
        color = mix(c1, c2, n);
#endif

    } else if (profile == 3) { // noise 3d
        color = texture2D(tex0_sampler, var_tex0_coord);
    }

    gl_FragColor = color;
}
