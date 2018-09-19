/*
 * Copyright 2017 GoPro Inc.
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

#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "transforms.h"

static const struct param_choices usage_choices = {
    .name = "buffer_usage",
    .consts = {
        {"stream_draw",  GL_STREAM_DRAW,  .desc=NGLI_DOCSTRING("modified once by the application "
                                                               "and used at most a few times as a source for drawing")},
        {"stream_read",  GL_STREAM_READ,  .desc=NGLI_DOCSTRING("modified once by reading data from the graphic pipeline "
                                                               "and used at most a few times to return the data to the application")},
        {"stream_copy",  GL_STREAM_COPY,  .desc=NGLI_DOCSTRING("modified once by reading data from the graphic pipeline "
                                                               "and used at most a few times as a source for drawing")},
        {"static_draw",  GL_STATIC_DRAW,  .desc=NGLI_DOCSTRING("modified once by the application "
                                                               "and used many times as a source for drawing")},
        {"static_read",  GL_STATIC_READ,  .desc=NGLI_DOCSTRING("modified once by reading data from the graphic pipeline "
                                                               "and used many times to return the data to the application")},
        {"static_copy",  GL_STATIC_COPY,  .desc=NGLI_DOCSTRING("modified once by reading data from the graphic pipeline "
                                                               "and used at most a few times a source for drawing")},
        {"dynamic_draw", GL_DYNAMIC_DRAW, .desc=NGLI_DOCSTRING("modified repeatedly by the application "
                                                               "and used many times as a source for drawing")},
        {"dynamic_read", GL_DYNAMIC_READ, .desc=NGLI_DOCSTRING("modified repeatedly by reading data from the graphic pipeline "
                                                               "and used many times to return data to the application")},
        {"dynamic_copy", GL_DYNAMIC_COPY, .desc=NGLI_DOCSTRING("modified repeatedly by reading data from the graphic pipeline "
                                                               "and used many times as a source for drawing")},
        {NULL}
    }
};

#define OFFSET(x) offsetof(struct buffer, x)
static const struct node_param buffer_params[] = {
    {"count",  PARAM_TYPE_INT,    OFFSET(count),
               .desc=NGLI_DOCSTRING("number of elements")},
    {"data",   PARAM_TYPE_DATA,   OFFSET(data),
               .desc=NGLI_DOCSTRING("buffer of `count` elements")},
    {"filename", PARAM_TYPE_STR,  OFFSET(filename),
               .desc=NGLI_DOCSTRING("filename from which the buffer will be read, cannot be used with `data`")},
    {"stride", PARAM_TYPE_INT,    OFFSET(data_stride),
               .desc=NGLI_DOCSTRING("stride of 1 element, in bytes")},
    {"usage",  PARAM_TYPE_SELECT, OFFSET(usage),  {.i64=GL_STATIC_DRAW},
               .desc=NGLI_DOCSTRING("buffer usage hint"),
               .choices=&usage_choices},
    {"anims", PARAM_TYPE_NODELIST, OFFSET(anims),
               .desc=NGLI_DOCSTRING("per element animations (only supported by `BufferFloat`, `BufferVec2`, `BufferVec3`, `BufferVec4`)")},
    {"transforms", PARAM_TYPE_NODELIST, OFFSET(transforms), .node_types=TRANSFORM_TYPES_LIST,
               .desc=NGLI_DOCSTRING("per element transformation chain (only suppuported by `BufferMat4`)")},
    {NULL}
};

static int buffer_init_from_data(struct ngl_node *node)
{
    struct buffer *s = node->priv_data;

    s->count = s->count ? s->count : s->data_size / s->data_stride;
    if (s->data_size != s->count * s->data_stride) {
        LOG(ERROR,
            "element count (%d) and data stride (%d) does not match data size (%d)",
            s->count,
            s->data_stride,
            s->data_size);
        return -1;
    }

    return 0;
}

static int buffer_init_from_filename(struct ngl_node *node)
{
    struct buffer *s = node->priv_data;

    s->fd = open(s->filename, O_RDONLY);
    if (s->fd < 0) {
        LOG(ERROR, "could not open '%s'", s->filename);
        return -1;
    }

    off_t filesize = lseek(s->fd, 0, SEEK_END);
    off_t ret      = lseek(s->fd, 0, SEEK_SET);
    if (filesize < 0 || ret < 0) {
        LOG(ERROR, "could not seek in '%s'", s->filename);
        return -1;
    }
    s->data_size = filesize;
    s->count = s->count ? s->count : s->data_size / s->data_stride;

    if (s->data_size != s->count * s->data_stride) {
        LOG(ERROR,
            "element count (%d) and data stride (%d) does not match data size (%d)",
            s->count,
            s->data_stride,
            s->data_size);
        return -1;
    }

    s->data = calloc(s->count, s->data_stride);
    if (!s->data)
        return -1;

    ssize_t n = read(s->fd, s->data, s->data_size);
    if (n < 0) {
        LOG(ERROR, "could not read '%s': %zd", s->filename, n);
        return -1;
    }

    if (n != s->data_size) {
        LOG(ERROR, "read %zd bytes does not match expected size of %d bytes", n, s->data_size);
        return -1;
    }

    return 0;
}

static int buffer_init_from_count(struct ngl_node *node)
{
    struct buffer *s = node->priv_data;

    s->count = s->count ? s->count : 1;
    s->data_size = s->count * s->data_stride;
    s->data = calloc(s->count, s->data_stride);
    if (!s->data)
        return -1;

    return 0;
}

static int buffer_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    struct buffer *s = node->priv_data;

    if (s->data && s->filename) {
        LOG(ERROR,
            "data and filename option cannot be set at the same time");
        return -1;
    }

    if (s->anims &&
        (node->class->id != NGL_NODE_BUFFERFLOAT &&
         node->class->id != NGL_NODE_BUFFERBVEC2 &&
         node->class->id != NGL_NODE_BUFFERBVEC3 &&
         node->class->id != NGL_NODE_BUFFERBVEC4)) {
        LOG(ERROR, "buffer animations are only supported for vec{1,2,3,4} buffers");
        return -1;
    }


    if (s->transforms && node->class->id != NGL_NODE_BUFFERMAT4) {
        LOG(ERROR, "buffer transforms are only supported for mat4 buffers");
        return -1;
    }

    int ret;
    int data_comp_size;
    int nb_comp;
    int format;

    switch (node->class->id) {
    case NGL_NODE_BUFFERBYTE:   data_comp_size = 1; nb_comp = 1; format = NGLI_FORMAT_R8_SNORM;               break;
    case NGL_NODE_BUFFERBVEC2:  data_comp_size = 1; nb_comp = 2; format = NGLI_FORMAT_R8G8_SNORM;             break;
    case NGL_NODE_BUFFERBVEC3:  data_comp_size = 1; nb_comp = 3; format = NGLI_FORMAT_R8G8B8_SNORM;           break;
    case NGL_NODE_BUFFERBVEC4:  data_comp_size = 1; nb_comp = 4; format = NGLI_FORMAT_R8G8B8A8_SNORM;         break;
    case NGL_NODE_BUFFERINT:    data_comp_size = 4; nb_comp = 1; format = NGLI_FORMAT_R32_SINT;               break;
    case NGL_NODE_BUFFERIVEC2:  data_comp_size = 4; nb_comp = 2; format = NGLI_FORMAT_R32G32_SINT;            break;
    case NGL_NODE_BUFFERIVEC3:  data_comp_size = 4; nb_comp = 3; format = NGLI_FORMAT_R32G32B32_SINT;         break;
    case NGL_NODE_BUFFERIVEC4:  data_comp_size = 4; nb_comp = 4; format = NGLI_FORMAT_R32G32B32A32_SINT;      break;
    case NGL_NODE_BUFFERSHORT:  data_comp_size = 2; nb_comp = 1; format = NGLI_FORMAT_R16_SNORM;              break;
    case NGL_NODE_BUFFERSVEC2:  data_comp_size = 2; nb_comp = 2; format = NGLI_FORMAT_R16G16_SNORM;           break;
    case NGL_NODE_BUFFERSVEC3:  data_comp_size = 2; nb_comp = 3; format = NGLI_FORMAT_R16G16B16_SNORM;        break;
    case NGL_NODE_BUFFERSVEC4:  data_comp_size = 2; nb_comp = 4; format = NGLI_FORMAT_R16G16B16A16_SNORM;     break;
    case NGL_NODE_BUFFERUBYTE:  data_comp_size = 1; nb_comp = 1; format = NGLI_FORMAT_R8_UNORM;               break;
    case NGL_NODE_BUFFERUBVEC2: data_comp_size = 1; nb_comp = 2; format = NGLI_FORMAT_R8G8_UNORM;             break;
    case NGL_NODE_BUFFERUBVEC3: data_comp_size = 1; nb_comp = 3; format = NGLI_FORMAT_R8G8B8_UNORM;           break;
    case NGL_NODE_BUFFERUBVEC4: data_comp_size = 1; nb_comp = 4; format = NGLI_FORMAT_R8G8B8A8_UNORM;         break;
    case NGL_NODE_BUFFERUINT:   data_comp_size = 4; nb_comp = 1; format = NGLI_FORMAT_R32_UINT;               break;
    case NGL_NODE_BUFFERUIVEC2: data_comp_size = 4; nb_comp = 2; format = NGLI_FORMAT_R32G32_UINT;            break;
    case NGL_NODE_BUFFERUIVEC3: data_comp_size = 4; nb_comp = 3; format = NGLI_FORMAT_R32G32B32_UINT;         break;
    case NGL_NODE_BUFFERUIVEC4: data_comp_size = 4; nb_comp = 4; format = NGLI_FORMAT_R32G32B32A32_UINT;      break;
    case NGL_NODE_BUFFERUSHORT: data_comp_size = 2; nb_comp = 1; format = NGLI_FORMAT_R16_UNORM;              break;
    case NGL_NODE_BUFFERUSVEC2: data_comp_size = 2; nb_comp = 2; format = NGLI_FORMAT_R16G16_UNORM;           break;
    case NGL_NODE_BUFFERUSVEC3: data_comp_size = 2; nb_comp = 3; format = NGLI_FORMAT_R16G16B16_UNORM;        break;
    case NGL_NODE_BUFFERUSVEC4: data_comp_size = 2; nb_comp = 4; format = NGLI_FORMAT_R16G16B16A16_UNORM;     break;
    case NGL_NODE_BUFFERFLOAT:  data_comp_size = 4; nb_comp = 1; format = NGLI_FORMAT_R32_SFLOAT;             break;
    case NGL_NODE_BUFFERVEC2:   data_comp_size = 4; nb_comp = 2; format = NGLI_FORMAT_R32G32_SFLOAT;          break;
    case NGL_NODE_BUFFERVEC3:   data_comp_size = 4; nb_comp = 3; format = NGLI_FORMAT_R32G32B32_SFLOAT;       break;
    case NGL_NODE_BUFFERVEC4:   data_comp_size = 4; nb_comp = 4; format = NGLI_FORMAT_R32G32B32A32_SFLOAT;    break;
    case NGL_NODE_BUFFERMAT4:   data_comp_size = 4; nb_comp = 16; format = NGLI_FORMAT_R32G32B32A32_SFLOAT;   break;
    default:
        ngli_assert(0);
    }

    s->data_comp = nb_comp;
    s->data_format = format;

    if (!s->data_stride)
        s->data_stride = s->data_comp * data_comp_size;

    if (s->data)
        ret = buffer_init_from_data(node);
    else if (s->filename)
        ret = buffer_init_from_filename(node);
    else
        ret = buffer_init_from_count(node);
    if (ret < 0)
        return ret;

    if (s->nb_anims && s->nb_anims != s->count) {
        LOG(ERROR,
            "animation count (%d) must match element count (%d)",
            s->nb_anims,
            s->count);
        return -1;
    }

    if (s->nb_anims && s->nb_anims != s->count) {
        LOG(ERROR,
            "transformation count (%d) must match element count (%d)",
            s->nb_anims,
            s->count);
        return -1;
    }

    if (s->nb_transforms && node->class->id == NGL_NODE_BUFFERMAT4) {
        s->mat4_data = malloc(s->data_size);
        s->mat4_transform_matrices = calloc(s->nb_transforms, sizeof(*s->mat4_transform_matrices));
        if (!s->mat4_data || !s->mat4_transform_matrices) {
            return -1;
        }
        memcpy(s->mat4_data, s->data, s->data_size);

        for (int i = 0; i < s->nb_transforms; i++) {
            const float *matrix = ngli_get_last_transformation_matrix(s->transforms[i]);
            s->mat4_transform_matrices[i] = (float *)matrix;
        }
    }

    if (s->generate_gl_buffer) {
        ngli_glGenBuffers(gl, 1, &s->buffer_id);
        ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, s->buffer_id);
        ngli_glBufferData(gl, GL_ARRAY_BUFFER, s->data_size, s->data, s->usage);
        ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, 0);
    }

    return 0;
}

static int buffer_update_vec(struct ngl_node *node, double t)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    struct buffer *s = node->priv_data;

    if (!s->nb_anims)
        return 0;

    for (int i = 0; i < s->nb_anims; i++) {
        struct ngl_node *anim_node = s->anims[i];
        struct animation *anim = anim_node->priv_data;
        int ret = ngli_node_update(anim_node, t);
        if (ret < 0)
            return ret;
        float *dst = (float *)(s->data + (i * s->data_stride));
        if (s->data_comp == 1)
            *dst = anim->scalar;
        else
            memcpy(dst, anim->values, s->data_comp * sizeof(float));
    }

    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, s->buffer_id);
    ngli_glBufferSubData(gl, GL_ARRAY_BUFFER, 0, s->data_size, s->data);
    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, 0);

    return 0;
}

static int buffer_update_mat4(struct ngl_node *node, double t)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    struct buffer *s = node->priv_data;

    if (!s->nb_transforms)
        return 0;

    static const NGLI_ALIGNED_MAT(id_matrix) = NGLI_MAT4_IDENTITY;

    for (int i = 0; i < s->nb_transforms; i++) {
        struct ngl_node *tnode = s->transforms[i];
        int ret = ngli_node_update(tnode, t);
        if (ret < 0)
            return ret;
        if (!ngli_darray_push(&ctx->modelview_matrix_stack, id_matrix))
            return -1;
        ngli_node_draw(tnode);
        ngli_darray_pop(&ctx->modelview_matrix_stack);
        const float *matrix = s->mat4_transform_matrices[i];
        float *src = (float *)(s->mat4_data + (i * s->data_stride));
        float *dst = (float *)(s->data + (i * s->data_stride));
        ngli_mat4_mul(dst, matrix, src);
    }

    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, s->buffer_id);
    ngli_glBufferSubData(gl, GL_ARRAY_BUFFER, 0, s->data_size, s->data);
    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, 0);

    return 0;
}

static void buffer_uninit(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    struct buffer *s = node->priv_data;

    free(s->mat4_data);
    free(s->mat4_transform_matrices);

    if (s->filename && s->fd) {
        int ret = close(s->fd);
        if (ret < 0) {
            LOG(ERROR, "could not properly close '%s'", s->filename);
        }
    }

    ngli_glDeleteBuffers(gl, 1, &s->buffer_id);
}

#define DEFINE_BUFFER_CLASS(class_id, class_name, type)     \
const struct node_class ngli_buffer##type##_class = {       \
    .id        = class_id,                                  \
    .name      = class_name,                                \
    .init      = buffer_init,                               \
    .uninit    = buffer_uninit,                             \
    .priv_size = sizeof(struct buffer),                     \
    .params    = buffer_params,                             \
    .params_id = "Buffer",                                  \
    .file      = __FILE__,                                  \
};

#define DEFINE_BUFFER_WITH_UPDATE_CLASS(class_id, class_name, type, update_suffix) \
const struct node_class ngli_buffer##type##_class = {                              \
    .id        = class_id,                                                         \
    .name      = class_name,                                                       \
    .init      = buffer_init,                                                      \
    .update    = buffer_update##update_suffix,                                     \
    .uninit    = buffer_uninit,                                                    \
    .priv_size = sizeof(struct buffer),                                            \
    .params    = buffer_params,                                                    \
    .params_id = "Buffer",                                                         \
    .file      = __FILE__,                                                         \
};

DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERBYTE,    "BufferByte",    byte)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERBVEC2,   "BufferBVec2",   bvec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERBVEC3,   "BufferBVec3",   bvec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERBVEC4,   "BufferBVec4",   bvec4)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERINT,     "BufferInt",     int)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERIVEC2,   "BufferIVec2",   ivec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERIVEC3,   "BufferIVec3",   ivec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERIVEC4,   "BufferIVec4",   ivec4)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERSHORT,   "BufferShort",   short)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERSVEC2,   "BufferSVec2",   svec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERSVEC3,   "BufferSVec3",   svec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERSVEC4,   "BufferSVec4",   svec4)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUBYTE,   "BufferUByte",   ubyte)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUBVEC2,  "BufferUBVec2",  ubvec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUBVEC3,  "BufferUBVec3",  ubvec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUBVEC4,  "BufferUBVec4",  ubvec4)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUINT,    "BufferUInt",    uint)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUIVEC2,  "BufferUIVec2",  uivec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUIVEC3,  "BufferUIVec3",  uivec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUIVEC4,  "BufferUIVec4",  uivec4)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUSHORT,  "BufferUShort",  ushort)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUSVEC2,  "BufferUSVec2",  usvec2)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUSVEC3,  "BufferUSVec3",  usvec3)
DEFINE_BUFFER_CLASS(NGL_NODE_BUFFERUSVEC4,  "BufferUSVec4",  usvec4)
DEFINE_BUFFER_WITH_UPDATE_CLASS(NGL_NODE_BUFFERFLOAT, "BufferFloat", float, _vec)
DEFINE_BUFFER_WITH_UPDATE_CLASS(NGL_NODE_BUFFERVEC2,  "BufferVec2",  vec2,  _vec)
DEFINE_BUFFER_WITH_UPDATE_CLASS(NGL_NODE_BUFFERVEC3,  "BufferVec3",  vec3,  _vec)
DEFINE_BUFFER_WITH_UPDATE_CLASS(NGL_NODE_BUFFERVEC4,  "BufferVec4",  vec4,  _vec)
DEFINE_BUFFER_WITH_UPDATE_CLASS(NGL_NODE_BUFFERMAT4,  "BufferMat4",  mat4,  _mat4)
