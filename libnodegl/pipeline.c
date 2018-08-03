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

static int update_uniforms(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;

    if (!s->nb_uniform_pairs)
        return 0;

    void *mapped_memory;
    vkMapMemory(vk->device, s->uniform_device_memory[vk->img_index], 0, s->uniform_device_memory_size, 0, &mapped_memory);
    for (int i = 0; i < s->nb_uniform_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->uniform_pairs[i];
        const int offset = (intptr_t)pair->program_info; // HACK / won't work with quaternion
        const struct ngl_node *unode = pair->node;
        void *datap = mapped_memory + offset;

        switch (unode->class->id) {
            case NGL_NODE_UNIFORMFLOAT: {
                const struct uniform *u = unode->priv_data;
                *(float *)datap = (float)u->scalar;
                break;
            }
            case NGL_NODE_UNIFORMVEC2: {
                const struct uniform *u = unode->priv_data;
                memcpy(datap, u->vector, 2 * sizeof(float));
                break;
            }
            case NGL_NODE_UNIFORMVEC3: {
                const struct uniform *u = unode->priv_data;
                memcpy(datap, u->vector, 3 * sizeof(float));
                break;
            }
            case NGL_NODE_UNIFORMVEC4: {
                const struct uniform *u = unode->priv_data;
                memcpy(datap, u->vector, 4 * sizeof(float));
                break;
            }
            default:
                LOG(ERROR, "unsupported uniform of type %s", unode->class->name);
                break;
        }
    }
    vkUnmapMemory(vk->device, s->uniform_device_memory[vk->img_index]);
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

#ifndef VULKAN_BACKEND
static int update_buffers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

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

    return 0;
}

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

    vkFreeCommandBuffers(vk->device, s->command_pool,
                         s->nb_command_buffers, s->command_buffers);
    free(s->command_buffers);
    vkDestroyPipeline(vk->device, s->vkpipeline, NULL);
    vkDestroyPipelineLayout(vk->device, s->pipeline_layout, NULL);
}

static VkResult create_command_pool(struct ngl_node *node, int family_id)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    VkCommandPoolCreateInfo command_pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family_id,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // XXX
    };

    return vkCreateCommandPool(vk->device, &command_pool_create_info, NULL, &s->command_pool);
}

static VkResult create_command_buffers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    s->nb_command_buffers = vk->nb_framebuffers;
    s->command_buffers = calloc(s->nb_command_buffers, sizeof(*s->command_buffers));
    if (!s->command_buffers)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkCommandBufferAllocateInfo command_buffers_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = s->nb_command_buffers,
    };

    VkResult ret = vkAllocateCommandBuffers(vk->device, &command_buffers_allocate_info,
                                            s->command_buffers);
    if (ret != VK_SUCCESS)
        return ret;

    return VK_SUCCESS;
}

static VkResult create_descriptor_pool(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    // TODO: uniform specific
    VkDescriptorPoolSize descriptor_pool_size = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = vk->nb_framebuffers * s->nb_uniform_buffers, // XXX: nb_framebuffers should be enough
    };

    VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &descriptor_pool_size,
        .maxSets = vk->nb_framebuffers * s->nb_uniform_buffers, // XXX: nb_framebuffers should be enough
    };

    return vkCreateDescriptorPool(vk->device, &descriptor_pool_create_info, NULL, &s->descriptor_pool);
}

static VkResult create_descriptor_sets(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    VkResult ret = VK_SUCCESS;
    if (s->nb_uniform_buffers) {
        VkDescriptorSetLayoutBinding *descriptor_set_layout_bindings = calloc(s->nb_uniform_buffers, sizeof(*descriptor_set_layout_bindings));
        for (int i = 0; i < s->nb_uniform_buffers; i++) {
            VkDescriptorSetLayoutBinding *descriptor_set_layout_binding = &descriptor_set_layout_bindings[i];
            descriptor_set_layout_binding->binding = i;
            descriptor_set_layout_binding->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptor_set_layout_binding->descriptorCount = 1;
            descriptor_set_layout_binding->stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = s->nb_uniform_buffers,
            .pBindings = descriptor_set_layout_bindings,
        };

        ret = vkCreateDescriptorSetLayout(vk->device, &descriptor_set_layout_create_info, NULL, &s->descriptor_set_layout);
        if (ret != VK_SUCCESS)
            return ret;

        s->descriptor_sets = calloc(vk->nb_framebuffers, sizeof(*s->descriptor_sets));
        VkDescriptorSetLayout *descriptor_set_layouts = calloc(vk->nb_framebuffers, sizeof(*descriptor_set_layouts));
        for (uint32_t i = 0; i < vk->nb_framebuffers; i++) {
            descriptor_set_layouts[i] = s->descriptor_set_layout;
        }

        VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = s->descriptor_pool,
            .descriptorSetCount = vk->nb_framebuffers,
            .pSetLayouts = descriptor_set_layouts,
        };
        ret = vkAllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, s->descriptor_sets);
        free(descriptor_set_layouts);
        free(descriptor_set_layout_bindings); // XXX: move earlier?
    }

    return ret;
}

static void destroy_descriptor_pool_and_sets(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    vkDestroyDescriptorSetLayout(vk->device, s->descriptor_set_layout, NULL);
    vkDestroyDescriptorPool(vk->device, s->descriptor_pool, NULL);
    free(s->descriptor_sets);
}

static int init_uniforms(struct ngl_node *node, const struct shader_reflection *reflection)
{
    if (!reflection->buffers)
        return 0;

    struct pipeline *s = get_pipeline(node);

    const struct hmap_entry *block_entry = NULL;
    while ((block_entry = ngli_hmap_next(reflection->buffers, block_entry))) {
        struct shader_buffer_reflection *buffer = block_entry->data;

        // init variables only if user has set some uniforms
        if (!(buffer->flag & NGLI_SHADER_BUFFER_UNIFORM))
            continue;

        if (s->uniforms) {
            const struct hmap_entry *entry = NULL;
            while ((entry = ngli_hmap_next(buffer->variables, entry))) {
                struct ngl_node *unode = ngli_hmap_get(s->uniforms, entry->key);
                if (!unode)
                    continue;

                int ret = ngli_node_init(unode);
                if (ret < 0)
                    return ret;

                const struct shader_variable_reflection *variable = entry->data;
                intptr_t offset = s->uniform_device_memory_size + variable->offset;
                struct nodeprograminfopair pair = {
                    .node = unode,
                    .program_info = (void *)offset,
                };
                snprintf(pair.name, sizeof(pair.name), "%s", entry->key);
                s->uniform_pairs[s->nb_uniform_pairs++] = pair;
            }
        }

        // TODO: get more information on alignement and memory requirement
        uint32_t uniform_buffer_size = NGLI_ALIGN(buffer->size, 32);
        s->uniform_buffer_sizes[s->nb_uniform_buffers++] = uniform_buffer_size;
        s->uniform_device_memory_size += uniform_buffer_size;
    }

    return 0;
}
#endif

#ifdef VULKAN_BACKEND
static int get_nb_uniforms(struct hmap *blocks)
{
    int nb_uniforms = 0;
    const struct hmap_entry *block_entry = NULL;
    while ((block_entry = ngli_hmap_next(blocks, block_entry))) {
        struct shader_buffer_reflection *buffer = block_entry->data;
        nb_uniforms += ngli_hmap_count(buffer->variables);
    }
    return nb_uniforms;
}

static void get_spirv_counters(struct program *program,
                               int *block_countp,
                               int *uniform_countp)
{
    int block_count = 0;
    int uniform_count = 0;

    struct hmap *vert_blocks = program->vert_reflection->buffers;
    if (vert_blocks) {
        block_count += ngli_hmap_count(vert_blocks);
        uniform_count += get_nb_uniforms(vert_blocks);
    }

    struct hmap *frag_blocks = program->frag_reflection->buffers;
    if (frag_blocks) {
        block_count += ngli_hmap_count(frag_blocks);
        uniform_count += get_nb_uniforms(frag_blocks);
    }

    *block_countp = block_count;
    *uniform_countp = uniform_count;
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
    VkResult vkret = create_command_pool(node, s->queue_family_id);
    if (vkret != VK_SUCCESS)
        return -1;

    int nb_uniforms;
    int nb_buffers;
    get_spirv_counters(program, &nb_buffers, &nb_uniforms);

    if (nb_buffers) {
        s->uniform_buffer_sizes = calloc(nb_buffers, sizeof(*s->uniform_buffer_sizes));
        s->nb_uniform_buffers = 0;

        s->uniform_pairs = calloc(nb_uniforms, sizeof(*s->uniform_pairs));
        if (!s->uniform_pairs)
            return -1;

        // uniform vertex shader init
        ret = init_uniforms(node, program->vert_reflection);
        if (ret < 0)
            return ret;

        // uniform fragment shader init
        ret = init_uniforms(node, program->frag_reflection);
        if (ret < 0)
            return ret;

        if (s->nb_uniform_buffers) {
            vkret = create_descriptor_pool(node);
            if (vkret != VK_SUCCESS)
                return -1;

            vkret = create_descriptor_sets(node);
            if (vkret != VK_SUCCESS)
                return -1;

            // create uniform buffers
            s->uniform_buffers = calloc(s->nb_uniform_buffers * vk->nb_framebuffers, sizeof(*s->uniform_buffers));
            if (!s->uniform_buffers)
                return -1;

            s->uniform_device_memory = calloc(vk->nb_framebuffers, sizeof(*s->uniform_device_memory));
            if (!s->uniform_device_memory)
                return -1;

            for (int j = 0; j < vk->nb_framebuffers; j++) {

                // create uniform buffers for each framebuffers
                for (int i = 0; i < s->nb_uniform_buffers; i++) {
                    VkBufferCreateInfo buffer_create_info = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                        .size = s->uniform_buffer_sizes[i],
                        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    };

                    vkret = vkCreateBuffer(vk->device, &buffer_create_info, NULL,
                                           &s->uniform_buffers[j * s->nb_uniform_buffers + i]);
                    if (vkret != VK_SUCCESS)
                        return -1;
                }

                // TODO: better way to retrieve this information?
                VkMemoryRequirements mem_req;
                vkGetBufferMemoryRequirements(vk->device, s->uniform_buffers[j * s->nb_uniform_buffers + 0 /* XXX */], &mem_req);
                int memory_type_index = ngli_vk_find_memory_type(vk, mem_req.memoryTypeBits,
                                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

                // allocate whole uniform buffers memory (for each frame buffers)
                VkMemoryAllocateInfo memory_allocate_info = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = s->uniform_device_memory_size,
                    .memoryTypeIndex = memory_type_index,
                };
                vkret = vkAllocateMemory(vk->device, &memory_allocate_info, NULL, &s->uniform_device_memory[j]);
                if (vkret != VK_SUCCESS)
                    return vkret;

                // bind vkbuffer with memory allocation
                uint32_t device_memory_offset = 0;
                for (int i = 0; i < s->nb_uniform_buffers; i++) {
                    uint32_t uniform_buffer_index = j * s->nb_uniform_buffers + i;
                    vkBindBufferMemory(vk->device, s->uniform_buffers[uniform_buffer_index],
                                       s->uniform_device_memory[j], device_memory_offset);

                    VkDescriptorBufferInfo descriptor_buffer_info = {
                        .buffer = s->uniform_buffers[uniform_buffer_index],
                        .offset = 0,
                        .range = VK_WHOLE_SIZE,
                    };

                    VkWriteDescriptorSet write_descriptor_set = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = s->descriptor_sets[j],
                        .dstBinding = i,
                        .dstArrayElement = 0,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .descriptorCount = 1,
                        .pBufferInfo = &descriptor_buffer_info,
                        .pImageInfo = NULL,
                        .pTexelBufferView = NULL,
                    };
                    vkUpdateDescriptorSets(vk->device, 1, &write_descriptor_set, 0, NULL);
                    device_memory_offset += s->uniform_buffer_sizes[i];
                }
            }
        }
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

    if (s->uniform_buffers) {
        for (int i = 0; i < vk->nb_framebuffers; i++) {
            vkDestroyBuffer(vk->device, s->uniform_buffers[i], NULL);
            vkFreeMemory(vk->device, s->uniform_device_memory[i], NULL);
        }

        free(s->uniform_buffers);
        free(s->uniform_device_memory);
    }

    destroy_descriptor_pool_and_sets(node);

    destroy_pipeline(node);
    vkDestroyCommandPool(vk->device, s->command_pool, NULL);
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
#ifndef VULKAN_BACKEND // TODO
    update_images_and_samplers(node);
    update_buffers(node);
#endif
    return 0;
}
