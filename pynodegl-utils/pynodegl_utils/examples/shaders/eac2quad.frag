// aliasing issues
// #define sample(sampler_2d, coord) texture2DLod(sampler_2d, coord, 0.0)
// derivative and lod issue
// #define sample(sampler_2d, coord) texture(sampler_2d, coord)

#define DIRECT_RENDERING_ANDROID
// #define DIRECT_RENDERING_IPHONE
#if defined(DIRECT_RENDERING_ANDROID)
    #extension GL_OES_EGL_image_external : require
    uniform mediump samplerExternalOES texture0_external_sampler;
    uniform mediump samplerExternalOES texture1_external_sampler;
#elif defined(DIRECT_RENDERING_IPHONE)
    uniform mediump sampler2D texture0_y_sampler;
    uniform mediump sampler2D texture0_uv_sampler;
    uniform mediump sampler2D texture1_y_sampler;
    uniform mediump sampler2D texture1_uv_sampler;
#else
    uniform mediump sampler2D texture0;
    uniform mediump sampler2D texture1;
#endif

precision highp float;

// specific shader to do to avoid switch
#define MODE_HEVC_EAC               0
#define MODE_HEVC_EAC_DUAL          1
#define MODE_H264_EAC               2
#define MODE_H264_FISHEYES          3
#define MODE_HEVC_FISHEYES_DUAL     4

in mediump vec2 var_uvcoord;
uniform int mode;

vec3 yuv2rgb(in vec3 yuv)
{
    // YUV offset
    // const vec3 offset = vec3(-0.0625, -0.5, -0.5);
    const vec3 offset = vec3(-0.0625, -0.5, -0.5);
    // RGB coefficients
    const vec3 Rcoeff = vec3( 1.164, 0.000,  1.596);
    const vec3 Gcoeff = vec3( 1.164, -0.391, -0.813);
    const vec3 Bcoeff = vec3( 1.164, 2.018,  0.000);

    vec3 rgb;

    yuv = clamp(yuv, 0.0, 1.0);

    yuv += offset;

    rgb.r = dot(yuv, Rcoeff);
    rgb.g = dot(yuv, Gcoeff);
    rgb.b = dot(yuv, Bcoeff);
    return rgb;
}

vec4 sample_yuv(in sampler2D texture_y, in sampler2D texture_uv, in vec2 textcoord)
{
    vec3 yuv;
    yuv.x = texture(texture_y, textcoord).r;
    // Get the U and V values
    yuv.y = texture(texture_uv, textcoord).r;
    yuv.z = texture(texture_uv, textcoord).g;
    return vec4(yuv2rgb(yuv), 1.0f);
}

// uniform mediump sampler2D texture1;
// varying highp vec2 quad_coord;

// uniform highp float aspect_ratio;
// uniform highp float angle;
// uniform highp float psi;
// uniform highp float theta;
// uniform highp float phi;
// uniform highp float offset;

out mediump vec4 output_color;

// #define EAC 0.0
// #define EAC_DUAL 1.0

// highp float pi = radians(180.0);


// // perfect EAC correction
// #define CUSTOM_ATAN(x) atan(x)

// no EAC correction
// #define CUSTOM_ATAN(x) ((x / 2.0) / 2.0 * pi)

// http://www-labs.iro.umontreal.ca/~mignotte/IFT2425/Documents/EfficientApproximationArctgFunction.pdf
// (9)
// #define CUSTOM_ATAN(x) (pi / 4.0 * x - x * (abs(x) - 1.0) * (0.2447 + 0.0663 * abs(x)))


// https://en.wikipedia.org/wiki/Line–plane_intersection#Algebraic_form
// highp vec3 plane_line_intersection(highp vec3 plane_point, highp vec3 plane_normal, highp vec3 line_point, highp vec3 line_vector) {
//     // be aware of division by 0
//     return line_point + line_vector * dot(plane_point - line_point, plane_normal) / dot(line_vector, plane_normal);
// }

void main(void) {
    // highp vec3 eye = vec3(0.0, 0.0, offset);
    // // plane projection
    // highp vec3 projection_vector = vec3((quad_coord.x - 0.5) * aspect_ratio, quad_coord.y - 0.5, -0.5 / tan(radians(angle / 2.0)));

    // // apply camera movements
    // highp mat3 rotation_matrix = mat3(
    //     cos(radians(-psi)), 0.0, -sin(radians(-psi)),
    //     0.0, 1.0, 0.0,
    //     sin(radians(-psi)), 0.0, cos(radians(-psi)))
    //     *
    //     mat3(
    //     1.0, 0.0, 0.0,
    //     0.0, cos(radians(-theta)), -sin(radians(-theta)),
    //     0.0, sin(radians(-theta)), cos(radians(-theta)))
    //     *
    //     mat3(
    //     cos(radians(-phi)), -sin(radians(-phi)), 0.0,
    //     sin(radians(-phi)), cos(radians(-phi)), 0.0,
    //     0.0, 0.0, 1.0);
    // eye = rotation_matrix * eye;
    // projection_vector = rotation_matrix * projection_vector;

    // // intersection from projection_vector from eye (which may not be in center) with the sphere
    // highp float a = dot(projection_vector, projection_vector);
    // highp float b = 2.0 * dot(projection_vector, eye);
    // highp float c = dot(eye, eye) - 1.0;
    // highp float delta = b * b - 4.0 * a * c;
    // highp float t = (-b + sqrt(delta)) / (2.0 * a);
    // highp vec3 point3d = eye + t * projection_vector;

    // highp vec3 point;
    // highp vec2 texcoord;

    // // Find face of EAC corresponding to point3d on the sphere
    // point = plane_line_intersection(vec3(0.0, 0.0, -0.5), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.z < 0.0 && -0.5 <= point.x && point.x <= 0.5 && -0.5 <= point.y && point.y <= 0.5) {
    //     // Front
    //     gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
    //     // EAC correction
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     // normalize texture coordinate for a small square
    //     texcoord = point.xy * vec2(1.0, 1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         // 1 EAC texture
    //         texcoord = mix(vec2(1.0 / 3.0, 1.0 / 2.0), vec2(2.0 / 3.0, 0.0), texcoord);
    //     } else {
    //         // DUAL EAC = 2 textures
    //         texcoord = mix(vec2(1.0 / 3.0, 1.0), vec2(2.0 / 3.0, 0.0), texcoord);
    //     }
    //     gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    // }

    // point = plane_line_intersection(vec3(-0.5, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.x < 0.0 && -0.5 <= point.y && point.y <= 0.5 && -0.5 <= point.z && point.z <= 0.5) {
    //     // Left
    //     gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     texcoord = point.zy * vec2(-1.0, 1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         texcoord = mix(vec2(0.0 / 3.0, 1.0 / 2.0), vec2(1.0 / 3.0, 0.0), texcoord);
    //     } else {
    //         texcoord = mix(vec2(0.0 / 3.0, 1.0), vec2(1.0 / 3.0, 0.0), texcoord);
    //     }
    //     gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    // }

    // point = plane_line_intersection(vec3(0.5, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.x > 0.0 && -0.5 <= point.y && point.y <= 0.5 && -0.5 <= point.z && point.z <= 0.5) {
    // //     // Right
    //     gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     texcoord = point.zy * vec2(1.0, 1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         texcoord = mix(vec2(2.0 / 3.0, 1.0 / 2.0), vec2(3.0 / 3.0, 0.0), texcoord);
    //     } else {
    //         texcoord = mix(vec2(2.0 / 3.0, 1.0), vec2(3.0 / 3.0, 0.0), texcoord);            
    //     }
    //     gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    // }

    // point = plane_line_intersection(vec3(0.0, 0.0, 0.5), vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.z > 0.0 && -0.5 <= point.x && point.x <= 0.5 && -0.5 <= point.y && point.y <= 0.5) {
    //     // Back
    //     gl_FragColor = vec4(0.0, 1.0, 1.0, 1.0);
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     texcoord = point.yx * vec2(1.0, -1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         texcoord = mix(vec2(1.0 / 3.0, 1.0 / 2.0), vec2(2.0 / 3.0, 2.0 / 2.0), texcoord);
    //         gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     } else {
    //         texcoord = mix(vec2(1.0 / 3.0, 0.0), vec2(2.0 / 3.0, 1.0), texcoord);
    //         gl_FragColor = picking_texture(texture1, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     }
    // }

    // point = plane_line_intersection(vec3(0.0, 0.5, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.y > 0.0 && -0.5 <= point.x && point.x <= 0.5 && -0.5 <= point.z && point.z <= 0.5) {
    //     // Top
    //     gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     texcoord = point.zx * vec2(1.0, 1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         texcoord = mix(vec2(3.0 / 3.0, 2.0 / 2.0), vec2(2.0 / 3.0, 1.0 / 2.0), texcoord);
    //         gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     } else {
    //         texcoord = mix(vec2(3.0 / 3.0, 1.0), vec2(2.0 / 3.0, 0.0), texcoord);
    //         gl_FragColor = picking_texture(texture1, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     }
    // }

    // point = plane_line_intersection(vec3(0.0, -0.5, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 0.0), point3d);
    // if (point3d.y < 0.0 && -0.5 <= point.x && point.x <= 0.5 && -0.5 <= point.z && point.z <= 0.5) {
    //     // Bottom
    //     gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
    //     point = CUSTOM_ATAN(2.0 * point) * 2.0 / pi;
    //     texcoord = point.zx * vec2(-1.0, 1.0) + vec2(0.5, 0.5);
    //     if (mode == EAC) {
    //         texcoord = mix(vec2(1.0 / 3.0, 2.0 / 2.0), vec2(0.0 / 3.0, 1.0 / 2.0), texcoord);
    //         gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     } else {
    //         texcoord = mix(vec2(1.0 / 3.0, 1.0), vec2(0.0 / 3.0, 0.0), texcoord);
    //         gl_FragColor = picking_texture(texture1, (osg_TextureMatrix0 * vec4(texcoord, 0.0, 1.0)).xy);
    //     }
    // }
    // // gl_FragColor = picking_texture(texture0, (osg_TextureMatrix0 * vec4(quad_coord, 0.0, 1.0)).xy);
    switch (mode) {
        case MODE_H264_EAC:
        case MODE_HEVC_EAC: {
        #if defined(DIRECT_RENDERING_ANDROID)
            output_color = texture(texture0_external_sampler, var_uvcoord);
        #elif defined(DIRECT_RENDERING_IPHONE)
            output_color = sample_yuv(texture0_y_sampler, texture0_uv_sampler, var_uvcoord);
        #else
            output_color = texture(texture0, var_uvcoord);
        #endif
            break;
        }
        case MODE_HEVC_EAC_DUAL: {
        #if defined(DIRECT_RENDERING_ANDROID)
            if (var_uvcoord.y<0.5)
                output_color = texture(texture0_external_sampler, var_uvcoord);
            else 
                output_color = texture(texture1_external_sampler, var_uvcoord);
        #elif defined(DIRECT_RENDERING_IPHONE)
            if (var_uvcoord.y<0.5)
                output_color = sample_yuv(texture0_y_sampler, texture0_uv_sampler, var_uvcoord);
            else 
                output_color = sample_yuv(texture1_y_sampler, texture1_uv_sampler, var_uvcoord);
        #else
            if (var_uvcoord.y<0.5)
                output_color = texture(texture0, var_uvcoord);
            else 
                output_color = texture(texture1, var_uvcoord);
        #endif
            break;
        }
        case MODE_HEVC_FISHEYES_DUAL:
        case MODE_H264_FISHEYES: {
        #if defined(DIRECT_RENDERING_ANDROID)
            output_color = texture(texture0_external_sampler, var_uvcoord) * texture(texture1_external_sampler, var_uvcoord);
        #elif defined(DIRECT_RENDERING_IPHONE)
            output_color = sample_yuv(texture0_y_sampler, texture0_uv_sampler, var_uvcoord) * sample_yuv(texture1_y_sampler, texture1_uv_sampler, var_uvcoord);
        #else
            // just to be sure to sample
            output_color = texture(texture0, var_uvcoord) * texture(texture1, var_uvcoord);
        #endif
            break;
        }
    }

    // output_color = sample(texture0, var_uvcoord); 
}