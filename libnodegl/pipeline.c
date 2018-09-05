/*
 * Copyright 2016-2018 GoPro Inc.
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
#include <limits.h>

#include "glincludes.h"
#include "hmap.h"
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "spirv.h"
#include "utils.h"
#include "renderer.h"

static struct pipeline *get_pipeline(struct ngl_node *node)
{
    struct pipeline *ret = NULL;
    if (node->class->id == NGL_NODE_RENDER) {
        struct render *s = node->priv_data;
        ret = &s->pipeline;
    } else if (node->class->id == NGL_NODE_COMPUTE) {
        struct compute *s = node->priv_data;
        ret = &s->pipeline;
    } else {
        ngli_assert(0);
    }
    return ret;
}

#ifdef VULKAN_BACKEND
// TODO
#else
static int acquire_next_available_texture_unit(uint64_t *used_texture_units)
{
    for (int i = 0; i < sizeof(*used_texture_units) * 8; i++) {
        if (!(*used_texture_units & (1 << i))) {
            *used_texture_units |= (1 << i);
            return i;
        }
    }
    LOG(ERROR, "no texture unit available");
    return -1;
}

static int update_default_sampler(const struct glcontext *gl,
                                  struct pipeline *s,
                                  struct texture *texture,
                                  const struct textureprograminfo *info,
                                  uint64_t *used_texture_units,
                                  int *sampling_mode)
{
    ngli_assert(info->sampler_id >= 0);

    int texture_index = acquire_next_available_texture_unit(used_texture_units);
    if (texture_index < 0)
        return -1;

    *sampling_mode = NGLI_SAMPLING_MODE_2D;

    ngli_glActiveTexture(gl, GL_TEXTURE0 + texture_index);
    ngli_glBindTexture(gl, texture->target, texture->id);
    ngli_glUniform1i(gl, info->sampler_id, texture_index);
    return 0;
}

#if defined(TARGET_ANDROID)
static int update_sampler2D(const struct glcontext *gl,
                            struct pipeline *s,
                            struct texture *texture,
                            const struct textureprograminfo *info,
                            uint64_t *used_texture_units,
                            int *sampling_mode)
{
    if (texture->target == GL_TEXTURE_EXTERNAL_OES) {
        ngli_assert(info->external_sampler_id >= 0);

        *sampling_mode = NGLI_SAMPLING_MODE_EXTERNAL_OES;

        if (info->sampler_id >= 0)
            ngli_glUniform1i(gl, info->sampler_id, s->disabled_texture_unit);

        int texture_index = acquire_next_available_texture_unit(used_texture_units);
        if (texture_index < 0)
            return -1;

        ngli_glActiveTexture(gl, GL_TEXTURE0 + texture_index);
        ngli_glBindTexture(gl, texture->target, texture->id);
        ngli_glUniform1i(gl, info->external_sampler_id, texture_index);

    } else if (info->sampler_id >= 0) {

        int ret = update_default_sampler(gl, s, texture, info, used_texture_units, sampling_mode);

        if (info->external_sampler_id >= 0) {
            ngli_glBindTexture(gl, GL_TEXTURE_EXTERNAL_OES, 0);
            ngli_glUniform1i(gl, info->external_sampler_id, s->disabled_texture_unit);
        }

        return ret;
    }
    return 0;
}
#elif defined(TARGET_IPHONE)
static int update_sampler2D(const struct glcontext *gl,
                             struct pipeline *s,
                             struct texture *texture,
                             const struct textureprograminfo *info,
                             uint64_t *used_texture_units,
                             int *sampling_mode)
{
    if (texture->upload_fmt == NGLI_HWUPLOAD_FMT_VIDEOTOOLBOX_NV12_DR) {
        ngli_assert(info->y_sampler_id >= 0 || info->uv_sampler_id >= 0);

        *sampling_mode = NGLI_SAMPLING_MODE_NV12;

        if (info->sampler_id >= 0)
            ngli_glUniform1i(gl, info->sampler_id, s->disabled_texture_unit);

        if (info->y_sampler_id >= 0) {
            int texture_index = acquire_next_available_texture_unit(used_texture_units);
            if (texture_index < 0)
                return -1;

            GLint id = CVOpenGLESTextureGetName(texture->ios_textures[0]);
            ngli_glActiveTexture(gl, GL_TEXTURE0 + texture_index);
            ngli_glBindTexture(gl, texture->target, id);
            ngli_glUniform1i(gl, info->y_sampler_id, texture_index);
        }

        if (info->uv_sampler_id >= 0) {
            int texture_index = acquire_next_available_texture_unit(used_texture_units);
            if (texture_index < 0)
                return -1;

            GLint id = CVOpenGLESTextureGetName(texture->ios_textures[1]);
            ngli_glActiveTexture(gl, GL_TEXTURE0 + texture_index);
            ngli_glBindTexture(gl, texture->target, id);
            ngli_glUniform1i(gl, info->uv_sampler_id, texture_index);
        }
    } else if (info->sampler_id >= 0) {
        int ret = update_default_sampler(gl, s, texture, info, used_texture_units, sampling_mode);

        if (info->y_sampler_id >= 0)
            ngli_glUniform1i(gl, info->y_sampler_id, s->disabled_texture_unit);

        if (info->uv_sampler_id >= 0)
            ngli_glUniform1i(gl, info->uv_sampler_id, s->disabled_texture_unit);

        return ret;
    }
    return 0;
}
#else
static int update_sampler2D(const struct glcontext *gl,
                             struct pipeline *s,
                             struct texture *texture,
                             const struct textureprograminfo *info,
                             uint64_t *used_texture_units,
                             int *sampling_mode)
{
    if (info->sampler_id >= 0)
        return update_default_sampler(gl, s, texture, info, used_texture_units, sampling_mode);
    return 0;
}
#endif

static int update_sampler3D(const struct glcontext *gl,
                             struct pipeline *s,
                             struct texture *texture,
                             const struct textureprograminfo *info,
                             uint64_t *used_texture_units,
                             int *sampling_mode)
{
    if (info->sampler_id >= 0)
        return update_default_sampler(gl, s, texture, info, used_texture_units, sampling_mode);
    return 0;
}

static int update_images_and_samplers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    struct pipeline *s = get_pipeline(node);

    if (s->textures) {
        uint64_t used_texture_units = s->used_texture_units;

        if (s->disabled_texture_unit >= 0) {
            ngli_glActiveTexture(gl, GL_TEXTURE0 + s->disabled_texture_unit);
            ngli_glBindTexture(gl, GL_TEXTURE_2D, 0);
#if defined(TARGET_ANDROID)
            ngli_glBindTexture(gl, GL_TEXTURE_EXTERNAL_OES, 0);
#endif
        }

        for (int i = 0; i < s->nb_texture_pairs; i++) {
            const struct nodeprograminfopair *pair = &s->texture_pairs[i];
            const struct textureprograminfo *info = pair->program_info;
            const struct ngl_node *tnode = pair->node;
            struct texture *texture = tnode->priv_data;

            if (info->sampler_type == GL_IMAGE_2D) {
                LOG(VERBOSE,
                    "image at location=%d will use texture_unit=%d",
                    info->sampler_id,
                    info->sampler_value);

                if (info->sampler_id >= 0) {
                    ngli_glBindImageTexture(gl,
                                            info->sampler_value,
                                            texture->id,
                                            0,
                                            GL_FALSE,
                                            0,
                                            texture->access,
                                            texture->internal_format);
                }
            } else {
                int sampling_mode = NGLI_SAMPLING_MODE_NONE;
                switch (texture->target) {
                case GL_TEXTURE_2D:
#if defined(TARGET_ANDROID)
                case GL_TEXTURE_EXTERNAL_OES:
#endif
                    update_sampler2D(gl, s, texture, info, &used_texture_units, &sampling_mode);
                    break;
                case GL_TEXTURE_3D:
                    update_sampler3D(gl, s, texture, info, &used_texture_units, &sampling_mode);
                    break;
                }

                if (info->sampling_mode_id >= 0)
                    ngli_glUniform1i(gl, info->sampling_mode_id, sampling_mode);
            }

            if (info->coord_matrix_id >= 0)
                ngli_glUniformMatrix4fv(gl, info->coord_matrix_id, 1, GL_FALSE, texture->coordinates_matrix);

            if (info->dimensions_id >= 0) {
                const float dimensions[3] = {texture->width, texture->height, texture->depth};
                if (texture->target == GL_TEXTURE_3D)
                    ngli_glUniform3fv(gl, info->dimensions_id, 1, dimensions);
                else
                    ngli_glUniform2fv(gl, info->dimensions_id, 1, dimensions);
            }

            if (info->ts_id >= 0)
                ngli_glUniform1f(gl, info->ts_id, texture->data_src_ts);
        }
    }

    return 0;
}
#endif

#ifdef VULKAN_BACKEND
static void update_one_uniform(const struct ngl_node *node, void* buffer)
{
    const struct uniform *u = node->priv_data;
    switch (node->class->id) {
        case NGL_NODE_UNIFORMFLOAT: {
            *(float *)buffer = (float)u->scalar;
            break;
        }
        case NGL_NODE_UNIFORMVEC2: {
            memcpy(buffer, u->vector, 2 * sizeof(float));
            break;
        }
        case NGL_NODE_UNIFORMVEC3: {
            memcpy(buffer, u->vector, 3 * sizeof(float));
            break;
        }
        case NGL_NODE_UNIFORMVEC4: {
            memcpy(buffer, u->vector, 4 * sizeof(float));
            break;
        }
        case NGL_NODE_UNIFORMINT: {
           *(int *)buffer = u->ival;
            break;
        }
        case NGL_NODE_UNIFORMQUAT: {
            memcpy(buffer, u->vector, 4 * sizeof(float));
            break;
        }
        case NGL_NODE_UNIFORMMAT4: {
            memcpy(buffer, u->matrix, sizeof(u->matrix));
            break;
        }
        default:
            LOG(ERROR, "unsupported uniform of type %s", node->class->name);
            break;
    }
}
#endif

static int update_uniforms(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;

    if (!s->nb_uniform_pairs)
        return 0;

    void *mapped_memory = ngli_renderer_map_buffer(vk, s->uniform_rendererbuffer);
    for (int i = 0; i < s->nb_uniform_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->uniform_pairs[i];
        const int offset = (intptr_t)pair->program_info; // HACK / won't work with quaternion
        update_one_uniform(pair->node, mapped_memory + offset);
    }
    ngli_renderer_unmap_buffer(vk, s->uniform_rendererbuffer);
#else
    struct glcontext *gl = ctx->glcontext;

    for (int i = 0; i < s->nb_uniform_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->uniform_pairs[i];
        const struct uniformprograminfo *info = pair->program_info;

        const GLint uid = info->id;
        if (uid < 0)
            continue;
        const struct ngl_node *unode = pair->node;
        switch (unode->class->id) {
        case NGL_NODE_UNIFORMFLOAT: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniform1f(gl, uid, u->scalar);
            break;
        }
        case NGL_NODE_UNIFORMVEC2: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniform2fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMVEC3: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniform3fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMVEC4: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniform4fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMINT: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniform1i(gl, uid, u->ival);
            break;
        }
        case NGL_NODE_UNIFORMQUAT: {
            const struct uniform *u = unode->priv_data;
            if (info->type == GL_FLOAT_MAT4)
                ngli_glUniformMatrix4fv(gl, uid, 1, GL_FALSE, u->matrix);
            else if (info->type == GL_FLOAT_VEC4)
                ngli_glUniform4fv(gl, uid, 1, u->vector);
            else
                LOG(ERROR,
                    "quaternion uniform '%s' must be declared as vec4 or mat4 in the shader",
                    pair->name);
            break;
        }
        case NGL_NODE_UNIFORMMAT4: {
            const struct uniform *u = unode->priv_data;
            ngli_glUniformMatrix4fv(gl, uid, 1, GL_FALSE, u->matrix);
            break;
        }
        case NGL_NODE_BUFFERFLOAT: {
            const struct buffer *buffer = unode->priv_data;
            ngli_glUniform1fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC2: {
            const struct buffer *buffer = unode->priv_data;
            ngli_glUniform2fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC3: {
            const struct buffer *buffer = unode->priv_data;
            ngli_glUniform3fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC4: {
            const struct buffer *buffer = unode->priv_data;
            ngli_glUniform4fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        default:
            LOG(ERROR, "unsupported uniform of type %s", unode->class->name);
            break;
        }
    }
#endif

    return 0;
}

static int update_buffers(struct ngl_node *node)
{
#ifdef VULKAN_BACKEND
    // TODO
#else
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);
    struct glcontext *gl = ctx->glcontext;
    if (s->buffers &&
        gl->features & NGLI_FEATURE_SHADER_STORAGE_BUFFER_OBJECT) {
        int i = 0;
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->buffers, entry))) {
            const struct ngl_node *bnode = entry->data;
            const struct buffer *buffer = bnode->priv_data;
            ngli_glBindBufferBase(gl, GL_SHADER_STORAGE_BUFFER, s->buffer_ids[i], buffer->buffer_id);
            i++;
        }
    }
#endif

    return 0;
}

#ifndef VULKAN_BACKEND
static struct uniformprograminfo *get_uniform_info(struct hmap *uniforms,
                                                   const char *basename,
                                                   const char *suffix)
{
    char name[MAX_ID_LEN];
    snprintf(name, sizeof(name), "%s%s", basename, suffix);
    return ngli_hmap_get(uniforms, name);
}

static int get_uniform_location(struct hmap *uniforms,
                                const char *basename,
                                const char *suffix)
{
    const struct uniformprograminfo *active_uniform = get_uniform_info(uniforms, basename, suffix);
    return active_uniform ? active_uniform->id : -1;
}

static void load_textureprograminfo(struct textureprograminfo *info,
                                    struct hmap *active_uniforms,
                                    const char *tex_key)
{
    const struct uniformprograminfo *sampler = get_uniform_info(active_uniforms, tex_key, "");
    if (!sampler) // Allow _sampler suffix
        sampler = get_uniform_info(active_uniforms, tex_key, "_sampler");
    if (sampler) {
        info->sampler_value = sampler->binding;
        info->sampler_type  = sampler->type;
        info->sampler_id    = sampler->id;
    } else {
        info->sampler_value =
        info->sampler_type  =
        info->sampler_id    = -1;
    }

    info->sampling_mode_id    = get_uniform_location(active_uniforms, tex_key, "_sampling_mode");
    info->coord_matrix_id     = get_uniform_location(active_uniforms, tex_key, "_coord_matrix");
    info->dimensions_id       = get_uniform_location(active_uniforms, tex_key, "_dimensions");
    info->ts_id               = get_uniform_location(active_uniforms, tex_key, "_ts");

#if defined(TARGET_ANDROID)
    info->external_sampler_id = get_uniform_location(active_uniforms, tex_key, "_external_sampler");
#elif defined(TARGET_IPHONE)
    info->y_sampler_id        = get_uniform_location(active_uniforms, tex_key, "_y_sampler");
    info->uv_sampler_id       = get_uniform_location(active_uniforms, tex_key, "_uv_sampler");
#endif
}
#endif

#ifdef VULKAN_BACKEND
static void destroy_pipeline(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    vkDeviceWaitIdle(vk->device);
    if (s->command_buffers) {
        vkFreeCommandBuffers(vk->device, vk->command_pool,
                            vk->nb_frames, s->command_buffers);
        free(s->command_buffers);
    }
    vkDestroyPipeline(vk->device, s->vkpipeline, NULL);
}

static VkResult create_command_buffers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    s->command_buffers = calloc(vk->nb_frames, sizeof(*s->command_buffers));
    if (!s->command_buffers)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkCommandBufferAllocateInfo command_buffers_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = vk->nb_frames,
    };

    VkResult ret = vkAllocateCommandBuffers(vk->device, &command_buffers_allocate_info,
                                            s->command_buffers);
    if (ret != VK_SUCCESS)
        return ret;

    return VK_SUCCESS;
}
#endif

int ngli_pipeline_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);

    int ret = ngli_node_init(s->program);
    if (ret < 0)
        return ret;

    struct program *program = s->program->priv_data;

    /* Uniforms */
#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;

    // compute uniform buffer size needed
    // VkDeviceSize          minUniformBufferOffsetAlignment;
    // VkDeviceSize          minStorageBufferOffsetAlignment;
    uint32_t uniform_buffer_size = 0;
    for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
        struct shader *current_shader = &program->shaders[i];
        if (!current_shader->reflection || !current_shader->reflection->bindings)
            continue;

        const struct hmap_entry *binding_entry = NULL;
        while ((binding_entry = ngli_hmap_next(current_shader->reflection->bindings, binding_entry))) {
            const struct shaderbinding *binding = binding_entry->data;
            if ((binding->flag & NGLI_SHADER_UNIFORM)) {
                const struct shaderblock *block = binding_entry->data;
                uniform_buffer_size += NGLI_ALIGN(block->size, 32);
            }
        }
    }
    if (uniform_buffer_size) {
        // allocate uniform pairs
        int nb_uniforms = s->uniforms ? ngli_hmap_count(s->uniforms) : 0;
        if (nb_uniforms > 0) {
            s->uniform_pairs = calloc(nb_uniforms, sizeof(*s->uniform_pairs));
            if (!s->uniform_pairs)
                return -1;
        }

        // allocate buffer
        s->uniform_rendererbuffer = ngli_renderer_create_buffer(vk, uniform_buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (!s->uniform_rendererbuffer)
                return -1;

        // attach uniform buffers
        void* uniform_memory = ngli_renderer_map_buffer(vk, s->uniform_rendererbuffer);
        uint32_t uniform_block_offset = 0;
        for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
            struct shader *current_shader = &program->shaders[i];
            if (!current_shader->reflection || !current_shader->reflection->bindings)
                continue;

            const struct hmap_entry *binding_entry = NULL;
            while ((binding_entry = ngli_hmap_next(current_shader->reflection->bindings, binding_entry))) {
                struct shaderbinding *binding = binding_entry->data;
                if ((binding->flag & NGLI_SHADER_UNIFORM)) {
                    const struct shaderblock *block = binding_entry->data;
                    uint32_t aligned_size = NGLI_ALIGN(block->size, 32);
                    ngli_renderer_bind_buffer(vk, program, s->uniform_rendererbuffer, uniform_block_offset, aligned_size, binding->index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

                    // TODO: map static uniforms directly
                    // fill uniform pairs
                    if (s->uniforms) {
                        const struct hmap_entry *variable_entry = NULL;
                        while ((variable_entry = ngli_hmap_next(block->variables, variable_entry))) {
                            struct ngl_node *unode = ngli_hmap_get(s->uniforms, variable_entry->key);
                            if (!unode)
                                continue;

                            int ret = ngli_node_init(unode);
                            if (ret < 0)
                                return ret;

                            const struct shaderblockvariable *variable = variable_entry->data;
                            uint64_t uniform_offset = uniform_block_offset + variable->offset;

                            // create nodeprograminfopair only for animated
                            struct uniform *uniform = unode->priv_data;
                            if (uniform->anim || uniform->transform) {
                                struct nodeprograminfopair pair = {
                                    .node = unode,
                                    .program_info = (void *)uniform_offset,
                                };
                                snprintf(pair.name, sizeof(pair.name), "%s", variable_entry->key);
                                s->uniform_pairs[s->nb_uniform_pairs++] = pair;
                            }
                            else {
                                for (uint32_t j = 0; j < vk->nb_frames; j++)
                                    update_one_uniform(unode, uniform_memory + (j*s->uniform_rendererbuffer->size) + uniform_offset);
                            }
                        }
                    }
                    uniform_block_offset += aligned_size;
                }
                else if ((binding->flag & NGLI_SHADER_STORAGE)) {
                    if (s->buffers) {
                        struct ngl_node *bnode = ngli_hmap_get(s->buffers, binding_entry->key);
                        if (!bnode)
                            continue;

                        struct buffer *buffer = bnode->priv_data;
                        buffer->generate_gl_buffer = 1;

                        int ret = ngli_node_init(bnode);
                        if (ret < 0)
                            return ret;

                        ngli_renderer_bind_buffer(vk, program, buffer->renderer_handle, 0, buffer->renderer_handle->size, binding->index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                    }
                }
            }
        }
        ngli_renderer_unmap_buffer(vk, s->uniform_rendererbuffer);
    }
#else
    int nb_uniforms = s->uniforms ? ngli_hmap_count(s->uniforms) : 0;
    if (nb_uniforms > 0) {
        s->uniform_pairs = calloc(nb_uniforms, sizeof(*s->uniform_pairs));
        if (!s->uniform_pairs)
            return -1;

        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->uniforms, entry))) {
            const struct uniformprograminfo *active_uniform =
                ngli_hmap_get(program->active_uniforms, entry->key);
            if (!active_uniform) {
                LOG(WARNING, "uniform %s attached to %s not found in %s",
                    entry->key, node->name, s->program->name);
                continue;
            }

            struct ngl_node *unode = entry->data;
            ret = ngli_node_init(unode);
            if (ret < 0)
                return ret;

            struct nodeprograminfopair pair = {
                .node = unode,
                .program_info = (void *)active_uniform,
            };
            snprintf(pair.name, sizeof(pair.name), "%s", entry->key);
            s->uniform_pairs[s->nb_uniform_pairs++] = pair;
        }
    }
#endif


    /* Textures */
#ifdef VULKAN_BACKEND
    // TODO
#else
    s->disabled_texture_unit = -1;

    struct glcontext *gl = ctx->glcontext;
    int nb_textures = s->textures ? ngli_hmap_count(s->textures) : 0;
    int max_nb_textures = NGLI_MIN(gl->max_texture_image_units, sizeof(s->used_texture_units) * 8);
    if (nb_textures > max_nb_textures) {
        LOG(ERROR, "attached textures count (%d) exceeds driver limit (%d)",
            nb_textures, gl->max_texture_image_units);
        return -1;
    }

    if (nb_textures > 0) {
        s->textureprograminfos = calloc(nb_textures, sizeof(*s->textureprograminfos));
        if (!s->textureprograminfos)
            return -1;

        s->texture_pairs = calloc(nb_textures, sizeof(*s->texture_pairs));
        if (!s->texture_pairs)
            return -1;

        int need_disabled_texture_unit = 0;
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->textures, entry))) {
            const char *key = entry->key;
            struct ngl_node *tnode = entry->data;
            int ret = ngli_node_init(tnode);
            if (ret < 0)
                return ret;

            struct texture *texture = tnode->priv_data;

            struct textureprograminfo *info = &s->textureprograminfos[s->nb_textureprograminfos];

            load_textureprograminfo(info, program->active_uniforms, key);

            if (info->sampler_type == GL_IMAGE_2D) {
                texture->direct_rendering = 0;
                if (info->sampler_value >= max_nb_textures) {
                    LOG(ERROR, "maximum number (%d) of texture unit reached", max_nb_textures);
                    return -1;
                }
                if (s->used_texture_units & (1 << info->sampler_value)) {
                    LOG(ERROR, "texture unit %d is already used by another image", info->sampler_value);
                    return -1;
                }
                s->used_texture_units |= 1 << info->sampler_value;
            }

#if defined(TARGET_ANDROID)
            const int has_aux_sampler = info->external_sampler_id >= 0;
#elif defined(TARGET_IPHONE)
            const int has_aux_sampler = (info->y_sampler_id >= 0 || info->uv_sampler_id >= 0);
#else
            const int has_aux_sampler = 0;
#endif

            if (info->sampler_id < 0 && !has_aux_sampler)
                LOG(WARNING, "no sampler found for texture %s", key);

            if (info->sampler_id >= 0 && has_aux_sampler)
                need_disabled_texture_unit = 1;

#if defined(TARGET_ANDROID) || defined(TARGET_IPHONE)
            texture->direct_rendering = texture->direct_rendering && has_aux_sampler;
            LOG(INFO, "direct rendering for texture %s.%s: %s",
                node->name, key, texture->direct_rendering ? "yes" : "no");
#endif
            s->nb_textureprograminfos++;

            struct nodeprograminfopair pair = {
                .node = tnode,
                .program_info = (void *)info,
            };
            snprintf(pair.name, sizeof(pair.name), "%s", key);
            s->texture_pairs[s->nb_texture_pairs++] = pair;
        }

        if (need_disabled_texture_unit) {
            s->disabled_texture_unit = acquire_next_available_texture_unit(&s->used_texture_units);
            if (s->disabled_texture_unit < 0)
                return -1;
            LOG(DEBUG, "using texture unit %d for disabled textures", s->disabled_texture_unit);
        }
    }
#endif

    /* Buffers */
#ifdef VULKAN_BACKEND
    // TODO
#else
    int nb_buffers = s->buffers ? ngli_hmap_count(s->buffers) : 0;
    if (nb_buffers > 0 &&
        gl->features & NGLI_FEATURE_SHADER_STORAGE_BUFFER_OBJECT) {
        s->buffer_ids = calloc(nb_buffers, sizeof(*s->buffer_ids));
        if (!s->buffer_ids)
            return -1;

        int i = 0;
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->buffers, entry))) {
            struct ngl_node *unode = entry->data;
            struct buffer *buffer = unode->priv_data;
            buffer->generate_gl_buffer = 1;

            ret = ngli_node_init(unode);
            if (ret < 0)
                return ret;

            static const GLenum props[] = {GL_BUFFER_BINDING};
            GLsizei nb_props = 1;
            GLint params = 0;
            GLsizei nb_params = 1;
            GLsizei nb_params_ret = 0;

            GLuint index = ngli_glGetProgramResourceIndex(gl,
                                                          program->program_id,
                                                          GL_SHADER_STORAGE_BLOCK,
                                                          entry->key);

            if (index != GL_INVALID_INDEX)
                ngli_glGetProgramResourceiv(gl,
                                            program->program_id,
                                            GL_SHADER_STORAGE_BLOCK,
                                            index,
                                            nb_props,
                                            props,
                                            nb_params,
                                            &nb_params_ret,
                                            &params);

            s->buffer_ids[i] = params;
            i++;
        }
    }
#endif

    return 0;
}

void ngli_pipeline_uninit(struct ngl_node *node)
{
    struct pipeline *s = get_pipeline(node);

    free(s->textureprograminfos);
    free(s->texture_pairs);
    free(s->uniform_pairs);

#ifdef VULKAN_BACKEND
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;

    destroy_pipeline(node);

    if (s->uniform_rendererbuffer)
        ngli_renderer_destroy_buffer(vk, s->uniform_rendererbuffer);
#else
    free(s->buffer_ids);
#endif
}

int ngli_pipeline_update(struct ngl_node *node, double t)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;
    if (s->last_width != vk->config.width ||
        s->last_height != vk->config.height) {
        LOG(INFO, "reconfigure from %dx%d to %dx%d",
            s->last_width, s->last_height,
            vk->config.width, vk->config.height);

        destroy_pipeline(node);

        VkResult ret = create_command_buffers(node);
        if (ret != VK_SUCCESS)
            return ret;

        VkResult vret = s->create_func(node, &s->vkpipeline);
        if (vret != VK_SUCCESS)
            return -1;

        s->last_width = vk->config.width;
        s->last_height = vk->config.height;
    }
#else
    struct glcontext *gl = ctx->glcontext;
#endif

    if (s->textures) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->textures, entry))) {
            int ret = ngli_node_update(entry->data, t);
            if (ret < 0)
                return ret;
        }
    }

    if (s->uniforms) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->uniforms, entry))) {
            int ret = ngli_node_update(entry->data, t);
            if (ret < 0)
                return ret;
        }
    }

#ifdef VULKAN_BACKEND
    // TODO
#else
    if (s->buffers &&
        gl->features & NGLI_FEATURE_SHADER_STORAGE_BUFFER_OBJECT) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->buffers, entry))) {
            int ret = ngli_node_update(entry->data, t);
            if (ret < 0)
                return ret;
        }
    }
#endif

    return ngli_node_update(s->program, t);
}

int ngli_pipeline_upload_data(struct ngl_node *node)
{
    update_uniforms(node);
    update_buffers(node);
#ifndef VULKAN_BACKEND // TODO
    update_images_and_samplers(node);
#endif
    return 0;
}
