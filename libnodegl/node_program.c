/*
 * Copyright 2016 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bstr.h"
#include "glincludes.h"
#include "log.h"
#include "nodegl.h"
#include "nodes.h"
#include "program.h"
#include "spirv.h"

#ifdef VULKAN_BACKEND
extern const int ngli_vk_default_frag_size;
extern const char *ngli_vk_default_frag;

extern const int ngli_vk_default_vert_size;
extern const char *ngli_vk_default_vert;
#else

#if defined(TARGET_ANDROID)
static const char default_fragment_shader[] =
    "#version 100"                                                                      "\n"
    "#extension GL_OES_EGL_image_external : require"                                    "\n"
    ""                                                                                  "\n"
    "precision highp float;"                                                            "\n"
    "uniform int tex0_sampling_mode;"                                                   "\n"
    "uniform sampler2D tex0_sampler;"                                                   "\n"
    "uniform samplerExternalOES tex0_external_sampler;"                                 "\n"
    "varying vec2 var_uvcoord;"                                                         "\n"
    "varying vec2 var_tex0_coord;"                                                      "\n"
    "void main(void)"                                                                   "\n"
    "{"                                                                                 "\n"
    "    if (tex0_sampling_mode == 1)"                                                  "\n"
    "        gl_FragColor = texture2D(tex0_sampler, var_tex0_coord);"                   "\n"
    "    else if (tex0_sampling_mode == 2)"                                             "\n"
    "        gl_FragColor = texture2D(tex0_external_sampler, var_tex0_coord);"          "\n"
    "    else"                                                                          "\n"
    "        gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);"                                  "\n"
    "}";
#else
static const char default_fragment_shader[] =
    "#version 100"                                                                      "\n"
    ""                                                                                  "\n"
    "precision highp float;"                                                            "\n"
    "uniform sampler2D tex0_sampler;"                                                   "\n"
    "varying vec2 var_uvcoord;"                                                         "\n"
    "varying vec2 var_tex0_coord;"                                                      "\n"
    "void main(void)"                                                                   "\n"
    "{"                                                                                 "\n"
    "    gl_FragColor = texture2D(tex0_sampler, var_tex0_coord);"                       "\n"
    "}";
#endif

static const char default_vertex_shader[] =
    "#version 100"                                                                      "\n"
    ""                                                                                  "\n"
    "precision highp float;"                                                            "\n"
    "attribute vec4 ngl_position;"                                                      "\n"
    "attribute vec2 ngl_uvcoord;"                                                       "\n"
    "attribute vec3 ngl_normal;"                                                        "\n"
    "uniform mat4 ngl_modelview_matrix;"                                                "\n"
    "uniform mat4 ngl_projection_matrix;"                                               "\n"
    "uniform mat3 ngl_normal_matrix;"                                                   "\n"

    "uniform mat4 tex0_coord_matrix;"                                                   "\n"
    "uniform vec2 tex0_dimensions;"                                                     "\n"

    "varying vec2 var_uvcoord;"                                                         "\n"
    "varying vec3 var_normal;"                                                          "\n"
    "varying vec2 var_tex0_coord;"                                                      "\n"
    "void main()"                                                                       "\n"
    "{"                                                                                 "\n"
    "    gl_Position = ngl_projection_matrix * ngl_modelview_matrix * ngl_position;"    "\n"
    "    var_uvcoord = ngl_uvcoord;"                                                    "\n"
    "    var_normal = ngl_normal_matrix * ngl_normal;"                                  "\n"
    "    var_tex0_coord = (tex0_coord_matrix * vec4(ngl_uvcoord, 0, 1)).xy;"            "\n"
    "}";

#endif

#define OFFSET(x) offsetof(struct program, x)
static const struct node_param program_params[] = {
#ifdef VULKAN_BACKEND
    {"vertex",   PARAM_TYPE_DATA, OFFSET(shaders[NGLI_SHADER_TYPE_VERTEX].data),
                 .desc=NGLI_DOCSTRING("vertex SPIR-V shader")},
    {"fragment", PARAM_TYPE_DATA, OFFSET(shaders[NGLI_SHADER_TYPE_FRAGMENT].data),
                 .desc=NGLI_DOCSTRING("fragment SPIR-V shader")},
#else
    {"vertex",   PARAM_TYPE_STR, OFFSET(shaders[NGLI_SHADER_TYPE_VERTEX].content),   {.str=default_vertex_shader},
                 .desc=NGLI_DOCSTRING("vertex shader")},
    {"fragment", PARAM_TYPE_STR, OFFSET(shaders[NGLI_SHADER_TYPE_FRAGMENT].content), {.str=default_fragment_shader},
                 .desc=NGLI_DOCSTRING("fragment shader")},
#endif
    {NULL}
};

#ifdef VULKAN_BACKEND
static int shader_check_default(struct shader *s, const char* shader_default, uint32_t shader_default_size) {
    if (!s->data) {
        s->data_size = shader_default_size;
        s->data = malloc(s->data_size);
        if (!s->data)
            return -1;
        memcpy(s->data, shader_default, s->data_size);
    }

    return 0;
}
#endif

static int program_init(struct ngl_node *node)
{
    int ret;
#ifdef VULKAN_BACKEND
    struct program *s = node->priv_data;

    // TODO: should be in default argument of programs_params
    ret = shader_check_default(&s->shaders[NGLI_SHADER_TYPE_VERTEX], ngli_vk_default_vert, ngli_vk_default_vert_size);
    if (ret != 0)
        return -1;

    ret = shader_check_default(&s->shaders[NGLI_SHADER_TYPE_FRAGMENT], ngli_vk_default_frag, ngli_vk_default_frag_size);
    if (ret != 0)
        return -1;

    ret = ngli_program_init(node);
    if (ret != 0)
        return -1;
#else
    ret = ngli_program_init(node);
    if (ret != 0)
        return -1;
#endif

    return ret;
}

static void program_uninit(struct ngl_node *node)
{
    ngli_program_uninit(node);
}

const struct node_class ngli_program_class = {
    .id        = NGL_NODE_PROGRAM,
    .name      = "Program",
    .init      = program_init,
    .uninit    = program_uninit,
    .priv_size = sizeof(struct program),
    .params    = program_params,
    .file      = __FILE__,
};
