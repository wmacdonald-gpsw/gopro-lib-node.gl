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

#include "buffer.h"
#include "glincludes.h"
#include "hmap.h"
#include "log.h"
#include "math_utils.h"
#include "memory.h"
#include "nodegl.h"
#include "nodes.h"
#include "spirv.h"
#include "utils.h"

static struct pipeline *get_pipeline(struct ngl_node *node)
{
    struct pipeline *ret = NULL;
    if (node->class->id == NGL_NODE_RENDER) {
        struct render_priv *s = node->priv_data;
        ret = &s->pipeline;
    } else if (node->class->id == NGL_NODE_COMPUTE) {
        struct compute_priv *s = node->priv_data;
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
        if (!(*used_texture_units & (1ULL << i))) {
            *used_texture_units |= (1ULL << i);
            return i;
        }
    }
    LOG(ERROR, "no texture unit available");
    return -1;
}

static const struct {
    const char *name;
    GLenum type;
} tex_specs[] = {
    [0] = {"2D",  GL_TEXTURE_2D},
    [1] = {"OES", GL_TEXTURE_EXTERNAL_OES},
};

static int get_disabled_texture_unit(const struct glcontext *gl,
                                     struct pipeline *s,
                                     uint64_t *used_texture_units,
                                     int type_index)
{
    int tex_unit = s->disabled_texture_unit[type_index];
    if (tex_unit >= 0)
        return tex_unit;

    tex_unit = acquire_next_available_texture_unit(used_texture_units);
    if (tex_unit < 0)
        return -1;

    TRACE("using texture unit %d for disabled %s textures",
          tex_unit, tex_specs[type_index].name);
    s->disabled_texture_unit[type_index] = tex_unit;

    ngli_glActiveTexture(gl, GL_TEXTURE0 + tex_unit);
    ngli_glBindTexture(gl, GL_TEXTURE_2D, 0);
    if (gl->features & NGLI_FEATURE_OES_EGL_EXTERNAL_IMAGE)
        ngli_glBindTexture(gl, GL_TEXTURE_EXTERNAL_OES, 0);

    return tex_unit;
}

static int bind_texture_plane(const struct glcontext *gl,
                              struct texture_priv *texture,
                              uint64_t *used_texture_units,
                              int index,
                              int location)
{
    GLuint id = texture->planes[index].id;
    GLenum target = texture->planes[index].target;
    int texture_index = acquire_next_available_texture_unit(used_texture_units);
    if (texture_index < 0)
        return -1;
    ngli_glActiveTexture(gl, GL_TEXTURE0 + texture_index);
    ngli_glBindTexture(gl, target, id);
    ngli_glUniform1i(gl, location, texture_index);
    return 0;
}

static int update_sampler(const struct glcontext *gl,
                          struct pipeline *s,
                          struct texture_priv *texture,
                          const struct textureprograminfo *info,
                          uint64_t *used_texture_units,
                          int *sampling_mode)
{
    struct {
        int id;
        int type_index;
        int bound;
    } samplers[] = {
        { info->sampler_location,          0, 0 },
        { info->y_sampler_location,        0, 0 },
        { info->uv_sampler_location,       0, 0 },
        { info->external_sampler_location, 1, 0 },
    };

    *sampling_mode = NGLI_SAMPLING_MODE_NONE;
    if (texture->layout == NGLI_TEXTURE_LAYOUT_DEFAULT) {
        if (info->sampler_location >= 0) {
            if (info->sampler_type == GL_IMAGE_2D) {
                GLuint id = texture->planes[0].id;
                GLuint unit = info->sampler_value;
                ngli_glBindImageTexture(gl, unit, id, 0, GL_FALSE, 0, texture->access, texture->internal_format);
            } else {
                int ret = bind_texture_plane(gl, texture, used_texture_units, 0, info->sampler_location);
                if (ret < 0)
                    return ret;
                *sampling_mode = NGLI_SAMPLING_MODE_DEFAULT;
            }
            samplers[0].bound = 1;
        }
    } else if (texture->layout == NGLI_TEXTURE_LAYOUT_NV12) {
        if (info->y_sampler_location >= 0) {
            int ret = bind_texture_plane(gl, texture, used_texture_units, 0, info->y_sampler_location);
            if (ret < 0)
                return ret;
            samplers[1].bound = 1;
            *sampling_mode = NGLI_SAMPLING_MODE_NV12;
        }
        if (info->uv_sampler_location >= 0) {
            int ret = bind_texture_plane(gl, texture, used_texture_units, 1, info->uv_sampler_location);
            if (ret < 0)
                return ret;
            samplers[2].bound = 1;
            *sampling_mode = NGLI_SAMPLING_MODE_NV12;
        }
    } else if (texture->layout == NGLI_TEXTURE_LAYOUT_MEDIACODEC) {
        if (info->external_sampler_location >= 0) {
            int ret = bind_texture_plane(gl, texture, used_texture_units, 0, info->external_sampler_location);
            if (ret < 0)
                return ret;
            samplers[3].bound = 1;
            *sampling_mode = NGLI_SAMPLING_MODE_EXTERNAL_OES;
        }
    }

    for (int i = 0; i < NGLI_ARRAY_NB(samplers); i++) {
        if (samplers[i].id < 0)
            continue;
        if (samplers[i].bound)
            continue;
        int disabled_texture_unit = get_disabled_texture_unit(gl, s, used_texture_units, samplers[i].type_index);
        if (disabled_texture_unit < 0)
            return -1;
        ngli_glUniform1i(gl, samplers[i].id, disabled_texture_unit);
    }

    return 0;
}

static int update_images_and_samplers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    if (s->textures) {
        uint64_t used_texture_units = s->used_texture_units;

        for (int i = 0; i < NGLI_ARRAY_NB(s->disabled_texture_unit); i++)
            s->disabled_texture_unit[i] = -1;

        for (int i = 0; i < s->nb_texture_pairs; i++) {
            const struct nodeprograminfopair *pair = &s->texture_pairs[i];
            const struct textureprograminfo *info = pair->program_info;
            const struct ngl_node *tnode = pair->node;
            struct texture_priv *texture = tnode->priv_data;

            int sampling_mode;
            int ret = update_sampler(gl, s, texture, info, &used_texture_units, &sampling_mode);
            if (ret < 0)
                return ret;

            if (info->sampling_mode_location >= 0)
                ngli_glUniform1i(gl, info->sampling_mode_location, sampling_mode);

            if (info->coord_matrix_location >= 0)
                ngli_glUniformMatrix4fv(gl, info->coord_matrix_location, 1, GL_FALSE, texture->coordinates_matrix);

            if (info->dimensions_location >= 0) {
                const float dimensions[3] = {texture->width, texture->height, texture->depth};
                if (info->dimensions_type == GL_FLOAT_VEC2)
                    ngli_glUniform2fv(gl, info->dimensions_location, 1, dimensions);
                else if (info->dimensions_type == GL_FLOAT_VEC3)
                    ngli_glUniform3fv(gl, info->dimensions_location, 1, dimensions);
            }

            if (info->ts_location >= 0)
                ngli_glUniform1f(gl, info->ts_location, texture->data_src_ts);
        }
    }

    return 0;
}
#endif

#ifdef VULKAN_BACKEND
static int update_samplers(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    for (int i = 0; i < s->nb_texture_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->texture_pairs[i];
        struct textureprograminfo *info = pair->program_info;
        const struct ngl_node *tnode = pair->node;
        struct texture_priv *texture = tnode->priv_data;

        if (info->binding >= 0) {
            VkDescriptorImageInfo image_info = {
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .imageView = texture->image_view,
                .sampler = texture->image_sampler,
            };
            VkWriteDescriptorSet write_descriptor_set = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = s->descriptor_sets[vk->img_index],
                .dstBinding = info->binding,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .pImageInfo = &image_info,
            };
            vkUpdateDescriptorSets(vk->device, 1, &write_descriptor_set, 0, NULL);
        }
    }

    return 0;
}
#endif

static int update_uniforms(struct ngl_node *node)
{
    struct pipeline *s = get_pipeline(node);

#ifdef VULKAN_BACKEND
    if (!s->nb_uniform_pairs && !s->nb_texture_pairs)
        return 0;

    // FIXME: check uniform type and use its size instead of source size
    void *mapped_memory = ngli_buffer_map(&s->uniform_buffer);
    for (int i = 0; i < s->nb_uniform_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->uniform_pairs[i];
        const int offset = (intptr_t)pair->program_info; // HACK / won't work with quaternion
        const struct ngl_node *unode = pair->node;
        void *datap = mapped_memory + offset;

        switch (unode->class->id) {
            case NGL_NODE_UNIFORMFLOAT: {
                const struct uniform_priv *u = unode->priv_data;
                *(float *)datap = (float)u->scalar;
                break;
            }
            case NGL_NODE_UNIFORMVEC2: {
                const struct uniform_priv *u = unode->priv_data;
                memcpy(datap, u->vector, 2 * sizeof(float));
                break;
            }
            case NGL_NODE_UNIFORMVEC3: {
                const struct uniform_priv *u = unode->priv_data;
                memcpy(datap, u->vector, 3 * sizeof(float));
                break;
            }
            case NGL_NODE_UNIFORMVEC4: {
                const struct uniform_priv *u = unode->priv_data;
                memcpy(datap, u->vector, 4 * sizeof(float));
                break;
            }
            default:
                LOG(ERROR, "unsupported uniform of type %s", unode->class->name);
                break;
        }
    }

    for (int i = 0; i < s->nb_texture_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->texture_pairs[i];
        struct textureprograminfo *info = pair->program_info;
        const struct ngl_node *tnode = pair->node;
        struct texture_priv *texture = tnode->priv_data;

        if (info->coord_matrix_offset >= 0) {
            void *datap = mapped_memory + info->coord_matrix_offset;
            memcpy(datap, texture->coordinates_matrix, sizeof(texture->coordinates_matrix));
        }
        if (info->dimensions_offset >= 0) {
            float dimensions[] = {
                texture->width,
                texture->height,
            };
            void *datap = mapped_memory + info->dimensions_offset;
            memcpy(datap, dimensions, sizeof(dimensions));
        }
        if (info->ts_offset >= 0) {
            float data_src_ts = texture->data_src_ts;
            void *datap = mapped_memory + info->ts_offset;
            memcpy(datap, &data_src_ts, sizeof(data_src_ts));
        }
    }

    ngli_buffer_unmap(&s->uniform_buffer);
#else
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    for (int i = 0; i < s->nb_uniform_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->uniform_pairs[i];
        const struct uniformprograminfo *info = pair->program_info;
        const GLint uid = info->location;
        if (uid < 0)
            continue;
        const struct ngl_node *unode = pair->node;
        switch (unode->class->id) {
        case NGL_NODE_UNIFORMFLOAT: {
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniform1f(gl, uid, u->scalar);
            break;
        }
        case NGL_NODE_UNIFORMVEC2: {
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniform2fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMVEC3: {
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniform3fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMVEC4: {
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniform4fv(gl, uid, 1, u->vector);
            break;
        }
        case NGL_NODE_UNIFORMINT: {
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniform1i(gl, uid, u->ival);
            break;
        }
        case NGL_NODE_UNIFORMQUAT: {
            const struct uniform_priv *u = unode->priv_data;
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
            const struct uniform_priv *u = unode->priv_data;
            ngli_glUniformMatrix4fv(gl, uid, 1, GL_FALSE, u->matrix);
            break;
        }
        case NGL_NODE_BUFFERFLOAT: {
            const struct buffer_priv *buffer = unode->priv_data;
            ngli_glUniform1fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC2: {
            const struct buffer_priv *buffer = unode->priv_data;
            ngli_glUniform2fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC3: {
            const struct buffer_priv *buffer = unode->priv_data;
            ngli_glUniform3fv(gl, uid, buffer->count, (const GLfloat *)buffer->data);
            break;
        }
        case NGL_NODE_BUFFERVEC4: {
            const struct buffer_priv *buffer = unode->priv_data;
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

    for (int i = 0; i < s->nb_buffer_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->buffer_pairs[i];
        const struct ngl_node *bnode = pair->node;
        const struct buffer_priv *buffer = bnode->priv_data;
        const struct bufferprograminfo *info = pair->program_info;

        ngli_glBindBufferBase(gl, info->type, info->binding, buffer->buffer.id);
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

#define OFFSET(x) offsetof(struct textureprograminfo, x)
static const struct texture_uniform_map {
    const char *suffix;
    const GLenum *allowed_types;
    size_t location_offset;
    size_t type_offset;
    size_t binding_offset;
} texture_uniform_maps[] = {
    {"",                  (const GLenum[]){GL_SAMPLER_2D, GL_SAMPLER_3D, GL_IMAGE_2D, 0}, OFFSET(sampler_location),          OFFSET(sampler_type),    OFFSET(sampler_value)},
    {"_sampling_mode",    (const GLenum[]){GL_INT, 0},                                    OFFSET(sampling_mode_location),    SIZE_MAX,                SIZE_MAX},
    {"_coord_matrix",     (const GLenum[]){GL_FLOAT_MAT4, 0},                             OFFSET(coord_matrix_location),     SIZE_MAX,                SIZE_MAX},
    {"_dimensions",       (const GLenum[]){GL_FLOAT_VEC2, GL_FLOAT_VEC3, 0},              OFFSET(dimensions_location),       OFFSET(dimensions_type), SIZE_MAX},
    {"_ts",               (const GLenum[]){GL_FLOAT, 0},                                  OFFSET(ts_location),               SIZE_MAX,                SIZE_MAX},
    {"_external_sampler", (const GLenum[]){GL_SAMPLER_EXTERNAL_OES, 0},                   OFFSET(external_sampler_location), SIZE_MAX,                SIZE_MAX},
    {"_y_sampler",        (const GLenum[]){GL_SAMPLER_2D, 0},                             OFFSET(y_sampler_location),        SIZE_MAX,                SIZE_MAX},
    {"_uv_sampler",       (const GLenum[]){GL_SAMPLER_2D, 0},                             OFFSET(uv_sampler_location),       SIZE_MAX,                SIZE_MAX},
};

static int is_allowed_type(const GLenum *allowed_types, GLenum type)
{
    for (int i = 0; allowed_types[i]; i++)
        if (allowed_types[i] == type)
            return 1;
    return 0;
}

static int load_textureprograminfo(struct textureprograminfo *info,
                                   struct hmap *active_uniforms,
                                   const char *tex_key)
{
    for (int i = 0; i < NGLI_ARRAY_NB(texture_uniform_maps); i++) {
        const struct texture_uniform_map *map = &texture_uniform_maps[i];
        const char *suffix = map->suffix;
        const struct uniformprograminfo *uniform = get_uniform_info(active_uniforms, tex_key, suffix);
        if (!uniform && !strcmp(map->suffix, "")) { // Allow _sampler suffix
            suffix = "_sampler";
            uniform = get_uniform_info(active_uniforms, tex_key, suffix);
        }
        if (uniform && !is_allowed_type(map->allowed_types, uniform->type)) {
            LOG(ERROR, "invalid type 0x%x found for texture uniform %s%s",
                uniform->type, tex_key, suffix);
            return -1;
        }

#define SET_INFO_FIELD(field) do {                                                       \
    if (map->field##_offset != SIZE_MAX)                                                 \
        *(int *)((uint8_t *)info + map->field##_offset) = uniform ? uniform->field : -1; \
} while (0)

        SET_INFO_FIELD(location);
        SET_INFO_FIELD(type);
        SET_INFO_FIELD(binding);
    }
    return 0;
}
#else
static void destroy_pipeline(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    vkDeviceWaitIdle(vk->device);

    vkFreeCommandBuffers(vk->device, s->command_pool,
                         s->nb_command_buffers, s->command_buffers);
    ngli_free(s->command_buffers);
    vkDestroyPipeline(vk->device, s->vkpipeline, NULL);
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
    s->command_buffers = ngli_calloc(s->nb_command_buffers, sizeof(*s->command_buffers));
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

static VkDescriptorSetLayoutBinding *get_descriptor_layout_binding(struct darray *binding_descriptors, int binding)
{
    int nb_descriptors = ngli_darray_count(binding_descriptors);
    for (int i = 0; i < nb_descriptors; i++) {
        VkDescriptorSetLayoutBinding *descriptor = ngli_darray_get(binding_descriptors, i);
        if (descriptor->binding == binding) {
            return descriptor;
        }
    }
    return NULL;
}

static VkResult create_descriptor_layout_bindings(struct ngl_node *node)
{
    struct pipeline *s = get_pipeline(node);
    struct program_priv *program = s->program->priv_data;

    ngli_darray_init(&s->binding_descriptors, sizeof(VkDescriptorSetLayoutBinding), 0);
    ngli_darray_init(&s->constant_descriptors, sizeof(VkPushConstantRange), 0);

    static const int stages_map[] = {
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    struct hmap *bindings_map[] = {
        program->vert_desc ? program->vert_desc->bindings : NULL,
        program->frag_desc ? program->frag_desc->bindings : NULL,
    };

    // Create descriptor sets
    int constant_offset = 0;
    for (int i = 0; i < NGLI_ARRAY_NB(bindings_map); i++) {
        const struct hmap *bindings = bindings_map[i];
        if (!bindings)
            continue;

        const struct hmap_entry *binding_entry = NULL;
        while ((binding_entry = ngli_hmap_next(bindings, binding_entry))) {
            struct spirv_binding *binding = binding_entry->data;
            if ((binding->flag & NGLI_SHADER_CONSTANT)) {
                struct spirv_block *block = binding_entry->data;
                VkPushConstantRange descriptor = {
                    .stageFlags = stages_map[i],
                    .offset = constant_offset,
                    .size = block->size,
                };
                constant_offset = block->size;
                ngli_darray_push(&s->constant_descriptors, &descriptor);
            } else if ((binding->flag & NGLI_SHADER_UNIFORM)) {
                VkDescriptorSetLayoutBinding *descriptorp = get_descriptor_layout_binding(&s->binding_descriptors, binding->index);
                if (descriptorp) {
                    descriptorp->stageFlags |= stages_map[i];
                } else {
                    VkDescriptorSetLayoutBinding descriptor = {
                        .binding = binding->index,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .descriptorCount = 1,
                        .stageFlags = stages_map[i],
                    };
                    ngli_darray_push(&s->binding_descriptors, &descriptor);
                }
            } else if (binding->flag & NGLI_SHADER_STORAGE) {
                VkDescriptorSetLayoutBinding *descriptorp = get_descriptor_layout_binding(&s->binding_descriptors, binding->index);
                if (descriptorp) {
                    descriptorp->stageFlags |= stages_map[i];
                } else {
                VkDescriptorSetLayoutBinding descriptor = {
                    .binding = binding->index,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = stages_map[i],
                };
                ngli_darray_push(&s->binding_descriptors, &descriptor);
                }
            } else if (binding->flag & NGLI_SHADER_SAMPLER) {
                VkDescriptorSetLayoutBinding *descriptorp = get_descriptor_layout_binding(&s->binding_descriptors, binding->index);
                if (descriptorp) {
                    descriptorp->stageFlags |= stages_map[i];
                } else {
                VkDescriptorSetLayoutBinding descriptor = {
                    .binding = binding->index,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = stages_map[i],
                };
                ngli_darray_push(&s->binding_descriptors, &descriptor);
                }
            }
        }
    }

    return VK_SUCCESS;
}

static VkResult create_descriptor_sets(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    const int nb_bindings = ngli_darray_count(&s->binding_descriptors);
    if (nb_bindings) {
        static const VkDescriptorType types[] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        };

        VkDescriptorPoolSize *descriptor_pool_sizes = ngli_calloc(NGLI_ARRAY_NB(types), sizeof(struct VkDescriptorPoolSize));
        if (!descriptor_pool_sizes)
            return -1;
        for (uint32_t i = 0; i < NGLI_ARRAY_NB(types); i++) {
            VkDescriptorPoolSize *descriptor_pool_size = &descriptor_pool_sizes[i];
            descriptor_pool_size->type = types[i];
            descriptor_pool_size->descriptorCount = 16; // FIXME:
        }

        VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .poolSizeCount = NGLI_ARRAY_NB(types),
            .pPoolSizes = descriptor_pool_sizes,
            .maxSets = vk->nb_framebuffers,
        };

        // TODO: descriptor tool should be shared for all nodes
        VkResult vkret = vkCreateDescriptorPool(vk->device, &descriptor_pool_create_info, NULL, &s->descriptor_pool);
        ngli_free(descriptor_pool_sizes);
        if (vkret != VK_SUCCESS)
            return -1;

        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = nb_bindings,
            .pBindings = (const VkDescriptorSetLayoutBinding *)ngli_darray_data(&s->binding_descriptors),
        };

        // create descriptor_set_layout
        vkret = vkCreateDescriptorSetLayout(vk->device, &descriptor_set_layout_create_info, NULL, &s->descriptor_set_layout);
        if (vkret != VK_SUCCESS)
            return -1;
        VkDescriptorSetLayout *descriptor_set_layouts = ngli_calloc(vk->nb_framebuffers, sizeof(*descriptor_set_layouts));
        for (uint32_t i = 0; i < vk->nb_framebuffers; i++) {
            descriptor_set_layouts[i] = s->descriptor_set_layout;
        }

        VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = s->descriptor_pool,
            .descriptorSetCount = vk->nb_framebuffers,
            .pSetLayouts = descriptor_set_layouts,
        };

        s->descriptor_sets = ngli_calloc(vk->nb_framebuffers, sizeof(*s->descriptor_sets));
        vkret = vkAllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, s->descriptor_sets);
        ngli_free(descriptor_set_layouts);
        if (vkret != VK_SUCCESS)
            return -1;
    }

    return VK_SUCCESS;
}

static VkResult create_pipeline_layout(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct pipeline *s = get_pipeline(node);

    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    const int nb_constants = ngli_darray_count(&s->constant_descriptors);
    if (nb_constants) {
        pipeline_layout_create_info.pushConstantRangeCount = nb_constants;
        pipeline_layout_create_info.pPushConstantRanges = (const VkPushConstantRange *)ngli_darray_data(&s->constant_descriptors);
    }

    if (s->descriptor_set_layout != VK_NULL_HANDLE) {
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = &s->descriptor_set_layout;
    }

    return vkCreatePipelineLayout(vk->device, &pipeline_layout_create_info, NULL, &s->pipeline_layout);
}
#endif

#ifdef VULKAN_BACKEND
static void buffer_bind(struct glcontext *vk,
                        struct buffer *buffer,
                        struct pipeline *pipeline,
                        int offset,
                        int size,
                        int index,
                        int type)
{
    for (uint32_t i = 0; i < vk->nb_framebuffers; i++) {
        VkDescriptorBufferInfo descriptor_buffer_info = {
            .buffer = buffer->vkbuf,
            .offset = offset,
            .range = size,
        };
        VkWriteDescriptorSet write_descriptor_set = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pipeline->descriptor_sets[i],
            .dstBinding = index,
            .dstArrayElement = 0,
            .descriptorType = type,
            .descriptorCount = 1,
            .pBufferInfo = &descriptor_buffer_info,
            .pImageInfo = NULL,
            .pTexelBufferView = NULL,
        };
        vkUpdateDescriptorSets(vk->device, 1, &write_descriptor_set, 0, NULL);
    }
}
#endif

int ngli_pipeline_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline *s = get_pipeline(node);
    struct program_priv *program = s->program->priv_data;

    /* Uniforms */
#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;
    VkResult vkret = create_command_pool(node, s->queue_family_id);
    if (vkret != VK_SUCCESS)
        return -1;

    vkret = create_descriptor_layout_bindings(node);
    if (vkret != VK_SUCCESS)
        return -1;

    vkret = create_descriptor_sets(node);
    if (vkret != VK_SUCCESS)
        return -1;

    vkret = create_pipeline_layout(node);
    if (vkret != VK_SUCCESS)
        return -1;

    const struct hmap *bindings_map[] = {
        program->vert_desc ? program->vert_desc->bindings : NULL,
        program->frag_desc ? program->frag_desc->bindings : NULL,
    };

    // XXX:
    if (s->textures) {
        int nb_textures = ngli_hmap_count(s->textures) * NGLI_ARRAY_NB(bindings_map);
        s->textureprograminfos = ngli_calloc(nb_textures, sizeof(*s->textureprograminfos));
        if (!s->textureprograminfos)
            return -1;

        s->texture_pairs = ngli_calloc(nb_textures, sizeof(*s->texture_pairs));
        if (!s->texture_pairs)
            return -1;
    }

    // compute uniform buffer size needed
    // VkDeviceSize          minUniformBufferOffsetAlignment;
    // VkDeviceSize          minStorageBufferOffsetAlignment;
    int uniform_buffer_size = 0;
    for (int i = 0; i < NGLI_ARRAY_NB(bindings_map); i++) {
        const struct hmap *blocks = bindings_map[i];
        if (!blocks)
            continue;

        const struct hmap_entry *block_entry = NULL;
        while ((block_entry = ngli_hmap_next(blocks, block_entry))) {
            struct spirv_binding *binding = block_entry->data;
            if ((binding->flag & NGLI_SHADER_UNIFORM)) {
                struct spirv_block *block = block_entry->data;
                uniform_buffer_size += NGLI_ALIGN(block->size, 32);
            }
        }
    }

    if (uniform_buffer_size) {
        // alocate uniform pairs
        int nb_uniforms = s->uniforms ? ngli_hmap_count(s->uniforms) : 0;
        if (nb_uniforms > 0) {
            s->uniform_pairs = ngli_calloc(nb_uniforms, sizeof(*s->uniform_pairs));
            if (!s->uniform_pairs)
                return -1;
        }

        // allocate buffer
        int ret = ngli_buffer_allocate(&s->uniform_buffer,
                                       vk,
                                       uniform_buffer_size,
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (ret < 0)
            return -1;
    }

    struct spirv_block *ngl_uniforms_blocks[] = {
        bindings_map[0] ? ngli_hmap_get(bindings_map[0], "ngl_uniforms") : NULL,
        bindings_map[1] ? ngli_hmap_get(bindings_map[1], "ngl_uniforms") : NULL,
    };
    int ngl_uniforms_block_offsets[] = {
        0,
        0,
    };

    if (uniform_buffer_size) {
        // attach uniform buffers
        int uniform_block_offset = 0;
        for (int i = 0; i < NGLI_ARRAY_NB(bindings_map); i++) {
            const struct hmap *blocks = bindings_map[i];
            if (!blocks)
                continue;
            const struct hmap_entry *block_entry = NULL;
            while ((block_entry = ngli_hmap_next(blocks, block_entry))) {
                struct spirv_block *block = block_entry->data;
                struct spirv_binding *binding = block_entry->data;
                if (!strcmp(block_entry->key, "ngl_uniforms")) {
                    ngl_uniforms_block_offsets[i] = uniform_block_offset;
                }
                if (binding->flag & NGLI_SHADER_UNIFORM) {
                    int aligned_size = NGLI_ALIGN(block->size, 32); // wtf
                    buffer_bind(vk, &s->uniform_buffer, s, uniform_block_offset, aligned_size, binding->index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                    // TODO: map static uniforms directly
                    // fill uniform pairs
                    if (s->uniforms) {
                        const struct hmap_entry *variable_entry = NULL;
                        while ((variable_entry = ngli_hmap_next(block->variables, variable_entry))) {
                            struct ngl_node *unode = ngli_hmap_get(s->uniforms, variable_entry->key);
                            if (!unode)
                                continue;
                            const struct spirv_variable *variable = variable_entry->data;
                            intptr_t uniform_offset = uniform_block_offset + variable->offset;

                            struct nodeprograminfopair pair = {
                                .node = unode,
                                .program_info = (void *)uniform_offset,
                            };
                            snprintf(pair.name, sizeof(pair.name), "%s", variable_entry->key);
                            s->uniform_pairs[s->nb_uniform_pairs++] = pair;
                        }
                    }
                    uniform_block_offset += aligned_size;
                } else if (binding->flag & NGLI_SHADER_STORAGE) {
                    if (s->buffers) {
                        struct ngl_node *bnode = ngli_hmap_get(s->buffers, block_entry->key);
                        if (!bnode)
                            continue;
                        struct buffer_priv *buffer = bnode->priv_data;
                        struct buffer *graphic_buffer = &buffer->buffer;
                        int ret = ngli_buffer_allocate(graphic_buffer,
                                                       vk,
                                                       buffer->data_size,
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                        if (ret < 0)
                            return ret;
                        ngli_buffer_upload(graphic_buffer, buffer->data, buffer->data_size);
                        buffer_bind(vk, graphic_buffer, s, 0, buffer->data_size, binding->index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                    }
                }
            }
        }
    }

    if (s->textures) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->textures, entry))) {
            struct ngl_node *tnode = entry->data;

            char name[128];
            snprintf(name, sizeof(name), "%s_sampler", entry->key);

            for (int i = 0; i < NGLI_ARRAY_NB(bindings_map); i++) {
                struct textureprograminfo *info = &s->textureprograminfos[s->nb_textureprograminfos];
                info->binding = -1;
                info->coord_matrix_offset = -1;
                info->dimensions_offset = -1;
                info->ts_offset = -1;

                int submit_info = 0;

                const struct hmap *bindings = bindings_map[i];
                if (bindings) {
                    struct spirv_binding *binding = ngli_hmap_get(bindings, name);
                    if (binding && binding->flag & NGLI_SHADER_SAMPLER) {
                        info->binding = binding->index;
                        submit_info = 1;
                    }
                }

                struct spirv_block *block = ngl_uniforms_blocks[i];
                if (block) {
                    int block_offset = ngl_uniforms_block_offsets[i];

#define GET_UNIFORM_VARIABLE(name) do {                                              \
    char uniform_name[128];                                                          \
    snprintf(uniform_name, sizeof(uniform_name), "%s_" #name, entry->key);           \
    struct spirv_variable *variable = ngli_hmap_get(block->variables, uniform_name); \
    if (variable) {                                                                  \
        info->name ## _offset = block_offset + variable->offset;                     \
        submit_info = 1;                                                             \
    }                                                                                \
} while (0)

                    GET_UNIFORM_VARIABLE(coord_matrix);
                    GET_UNIFORM_VARIABLE(dimensions);
                    GET_UNIFORM_VARIABLE(ts);
                }

                if (submit_info) {
                    struct nodeprograminfopair pair = {
                        .node = tnode,
                        .program_info = info,
                    };
                    snprintf(pair.name, sizeof(pair.name), "%s", name);
                    s->texture_pairs[s->nb_texture_pairs++] = pair;
                    s->nb_textureprograminfos++;
                }
            }
        }
    }
#else
    int nb_uniforms = s->uniforms ? ngli_hmap_count(s->uniforms) : 0;
    if (nb_uniforms > 0) {
        s->uniform_pairs = ngli_calloc(nb_uniforms, sizeof(*s->uniform_pairs));
        if (!s->uniform_pairs)
            return -1;

        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->uniforms, entry))) {
            const struct uniformprograminfo *active_uniform =
                ngli_hmap_get(program->active_uniforms, entry->key);
            if (!active_uniform) {
                LOG(WARNING, "uniform %s attached to %s not found in %s",
                    entry->key, node->label, s->program->label);
                continue;
            }

            struct nodeprograminfopair pair = {
                .node = entry->data,
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
    struct glcontext *gl = ctx->glcontext;
    int nb_textures = s->textures ? ngli_hmap_count(s->textures) : 0;
    int max_nb_textures = NGLI_MIN(gl->max_texture_image_units, sizeof(s->used_texture_units) * 8);
    if (nb_textures > max_nb_textures) {
        LOG(ERROR, "attached textures count (%d) exceeds driver limit (%d)",
            nb_textures, gl->max_texture_image_units);
        return -1;
    }

    if (nb_textures > 0) {
        s->textureprograminfos = ngli_calloc(nb_textures, sizeof(*s->textureprograminfos));
        if (!s->textureprograminfos)
            return -1;

        s->texture_pairs = ngli_calloc(nb_textures, sizeof(*s->texture_pairs));
        if (!s->texture_pairs)
            return -1;

        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->textures, entry))) {
            const char *key = entry->key;
            struct ngl_node *tnode = entry->data;
            struct texture_priv *texture = tnode->priv_data;

            struct textureprograminfo *info = &s->textureprograminfos[s->nb_textureprograminfos];

            int ret = load_textureprograminfo(info, program->active_uniforms, key);
            if (ret < 0)
                return ret;

            if (info->sampler_type == GL_IMAGE_2D) {
                texture->direct_rendering = 0;
                if (info->sampler_value >= max_nb_textures) {
                    LOG(ERROR, "maximum number (%d) of texture unit reached", max_nb_textures);
                    return -1;
                }
                if (s->used_texture_units & (1ULL << info->sampler_value)) {
                    LOG(ERROR, "texture unit %d is already used by another image", info->sampler_value);
                    return -1;
                }
                s->used_texture_units |= 1ULL << info->sampler_value;
            }

#if defined(TARGET_ANDROID)
            const int has_aux_sampler = info->external_sampler_location >= 0;
#elif defined(TARGET_IPHONE)
            const int has_aux_sampler = (info->y_sampler_location >= 0 || info->uv_sampler_location >= 0);
#else
            const int has_aux_sampler = 0;
#endif

            if (info->sampler_location < 0 && !has_aux_sampler)
                LOG(WARNING, "no sampler found for texture %s", key);

#if defined(TARGET_ANDROID) || defined(TARGET_IPHONE)
            texture->direct_rendering = texture->direct_rendering && has_aux_sampler;
            LOG(INFO, "direct rendering for texture %s.%s: %s",
                node->label, key, texture->direct_rendering ? "yes" : "no");
#endif
            s->nb_textureprograminfos++;

            struct nodeprograminfopair pair = {
                .node = tnode,
                .program_info = (void *)info,
            };
            snprintf(pair.name, sizeof(pair.name), "%s", key);
            s->texture_pairs[s->nb_texture_pairs++] = pair;
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
        s->buffer_pairs = ngli_calloc(nb_buffers, sizeof(*s->buffer_pairs));
        if (!s->buffer_pairs)
            return -1;

        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->buffers, entry))) {
            const struct bufferprograminfo *info =
                ngli_hmap_get(program->active_buffer_blocks, entry->key);
            if (!info) {
                LOG(WARNING, "buffer %s attached to %s not found in %s",
                    entry->key, node->label, s->program->label);
                continue;
            }

            struct ngl_node *bnode = entry->data;
            struct buffer_priv *buffer = bnode->priv_data;

            if (info->type == GL_UNIFORM_BUFFER &&
                buffer->data_size > gl->max_uniform_block_size) {
                LOG(ERROR, "buffer %s size (%d) exceeds max uniform block size (%d)",
                    bnode->label, buffer->data_size, gl->max_uniform_block_size);
                return -1;
            }

            int ret = ngli_node_buffer_ref(bnode);
            if (ret < 0)
                return ret;

            struct nodeprograminfopair pair = {
                .node = bnode,
                .program_info = (void *)info,
            };
            snprintf(pair.name, sizeof(pair.name), "%s", entry->key);
            s->buffer_pairs[s->nb_buffer_pairs++] = pair;
        }
    }
#endif

    return 0;
}

void ngli_pipeline_uninit(struct ngl_node *node)
{
    struct pipeline *s = get_pipeline(node);

    ngli_free(s->textureprograminfos);
    ngli_free(s->texture_pairs);
    ngli_free(s->uniform_pairs);
    for (int i = 0; i < s->nb_buffer_pairs; i++) {
        struct nodeprograminfopair *pair = &s->buffer_pairs[i];
        ngli_node_buffer_unref(pair->node);
    }
    ngli_free(s->buffer_pairs);

#ifdef VULKAN_BACKEND
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;

    destroy_pipeline(node);

    vkDestroyDescriptorSetLayout(vk->device, s->descriptor_set_layout, NULL);
    vkDestroyDescriptorPool(vk->device, s->descriptor_pool, NULL);
    ngli_free(s->descriptor_sets);
    vkDestroyPipelineLayout(vk->device, s->pipeline_layout, NULL);

    vkDestroyCommandPool(vk->device, s->command_pool, NULL);

    ngli_buffer_free(&s->uniform_buffer);
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
            struct ngl_node *bnode = (struct ngl_node *)entry->data;
            int ret = ngli_node_update(bnode, t);
            if (ret < 0)
                return ret;
            ret = ngli_node_buffer_upload(bnode);
            if (ret < 0)
                return ret;
        }
    }
#endif

    return ngli_node_update(s->program, t);
}

int ngli_pipeline_upload_data(struct ngl_node *node)
{
    int ret;

#ifdef VULKAN_BACKEND
    // TODO
    if ((ret = update_uniforms(node)) < 0 ||
        (ret = update_samplers(node)) < 0)
        return ret;
#else
    if ((ret = update_uniforms(node)) < 0 ||
        (ret = update_images_and_samplers(node)) < 0 ||
        (ret = update_buffers(node)) < 0)
        return ret;
#endif

    return 0;
}
