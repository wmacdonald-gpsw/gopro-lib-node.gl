/*
 * Copyright 2018 GoPro Inc.
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

#include <stdlib.h>
#include <string.h>

#include "glincludes.h"
#include "log.h"
#include "darray.h"
#include "nodes.h"
#include "program.h"
#include "renderer.h"
#include "spirv.h"

#ifdef VULKAN_BACKEND
static void add_shader_binding(struct darray *bindings, uint32_t index, uint32_t type)
{
    VkDescriptorSetLayoutBinding *descriptor_set_layout_binding = ngli_darray_add(bindings);
    descriptor_set_layout_binding->binding = index;
    descriptor_set_layout_binding->descriptorType = type;
    descriptor_set_layout_binding->descriptorCount = 1;
    // TODO: should depends on node_render vs node_compute
    descriptor_set_layout_binding->stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
}

static void add_shader_constant(struct darray *constants, uint32_t size, uint32_t stage)
{
    VkPushConstantRange *push_constant_range = ngli_darray_add(constants);
    VkPushConstantRange *previous_push_constant_range =  ((VkPushConstantRange *)ngli_darray_get(constants, ngli_darray_size(constants)-1));
    push_constant_range->stageFlags = stage;
    push_constant_range->offset = previous_push_constant_range->offset + previous_push_constant_range->size;
    push_constant_range->size = size;
}

#else
static int ngli_program_check_status(const struct glcontext *gl, GLuint id, GLenum status)
{
    char *info_log = NULL;
    int info_log_length = 0;

    void (*get_info)(const struct glcontext *gl, GLuint id, GLenum pname, GLint *params);
    void (*get_log)(const struct glcontext *gl, GLuint id, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    const char *type_str;

    if (status == GL_COMPILE_STATUS) {
        type_str = "compile";
        get_info = ngli_glGetShaderiv;
        get_log  = ngli_glGetShaderInfoLog;
    } else if (status == GL_LINK_STATUS) {
        type_str = "link";
        get_info = ngli_glGetProgramiv;
        get_log  = ngli_glGetProgramInfoLog;
    } else {
        ngli_assert(0);
    }

    GLint result = GL_FALSE;
    get_info(gl, id, status, &result);
    if (result == GL_TRUE)
        return 0;

    get_info(gl, id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (!info_log_length)
        return -1;

    info_log = malloc(info_log_length);
    if (!info_log)
        return -1;

    get_log(gl, id, info_log_length, NULL, info_log);
    while (info_log_length && strchr(" \r\n", info_log[info_log_length - 1]))
        info_log[--info_log_length] = 0;

    LOG(ERROR, "could not %s shader: %s", type_str, info_log);
    return -1;
}

static void free_pinfo(void *user_arg, void *data)
{
    free(data);
}

static struct hmap *ngli_program_probe_uniforms(const char *node_name, struct glcontext *gl, GLuint pid)
{
    struct hmap *umap = ngli_hmap_create();
    if (!umap)
        return NULL;
    ngli_hmap_set_free(umap, free_pinfo, NULL);

    int nb_active_uniforms;
    ngli_glGetProgramiv(gl, pid, GL_ACTIVE_UNIFORMS, &nb_active_uniforms);
    for (int i = 0; i < nb_active_uniforms; i++) {
        char name[MAX_ID_LEN];
        struct uniformprograminfo *info = malloc(sizeof(*info));
        if (!info) {
            ngli_hmap_freep(&umap);
            return NULL;
        }
        ngli_glGetActiveUniform(gl, pid, i, sizeof(name), NULL,
                                &info->size, &info->type, name);

        /* Remove [0] suffix from names of uniform arrays */
        name[strcspn(name, "[")] = 0;
        info->id = ngli_glGetUniformLocation(gl, pid, name);

        if (info->type == GL_IMAGE_2D) {
            ngli_glGetUniformiv(gl, pid, info->id, &info->binding);
        } else {
            info->binding = -1;
        }

        LOG(DEBUG, "%s.uniform[%d/%d]: %s location:%d size=%d type=0x%x binding=%d", node_name,
            i + 1, nb_active_uniforms, name, info->id, info->size, info->type, info->binding);

        int ret = ngli_hmap_set(umap, name, info);
        if (ret < 0) {
            ngli_hmap_freep(&umap);
            return NULL;
        }
    }

    return umap;
}

static struct hmap *ngli_program_probe_attributes(const char *node_name, struct glcontext *gl, GLuint pid)
{
    struct hmap *amap = ngli_hmap_create();
    if (!amap)
        return NULL;
    ngli_hmap_set_free(amap, free_pinfo, NULL);

    int nb_active_attributes;
    ngli_glGetProgramiv(gl, pid, GL_ACTIVE_ATTRIBUTES, &nb_active_attributes);
    for (int i = 0; i < nb_active_attributes; i++) {
        char name[MAX_ID_LEN];
        struct attributeprograminfo *info = malloc(sizeof(*info));
        if (!info) {
            ngli_hmap_freep(&amap);
            return NULL;
        }
        ngli_glGetActiveAttrib(gl, pid, i, sizeof(name), NULL,
                               &info->size, &info->type, name);

        info->id = ngli_glGetAttribLocation(gl, pid, name);
        LOG(DEBUG, "%s.attribute[%d/%d]: %s location:%d size=%d type=0x%x", node_name,
            i + 1, nb_active_attributes, name, info->id, info->size, info->type);

        int ret = ngli_hmap_set(amap, name, info);
        if (ret < 0) {
            ngli_hmap_freep(&amap);
            return NULL;
        }
    }

    return amap;
}
#endif

int ngli_program_init(struct ngl_node *node) {
    struct ngl_ctx *ctx = node->ctx;
    struct program *s = node->priv_data;

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;
    VkResult vkret = -1;

    struct darray *bindings = ngli_darray_create(sizeof(struct VkDescriptorSetLayoutBinding));
    struct darray *constants = ngli_darray_create(sizeof(struct VkPushConstantRange));

    const uint32_t constant_stages[NGLI_SHADER_TYPE_COUNT] = {
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_COMPUTE_BIT
    };
    for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
        struct shader *current_shader = &s->shaders[i];

        if (!current_shader->data)
            continue;

        current_shader->module = ngli_renderer_create_shader(vk, current_shader->data, current_shader->data_size);
        if (current_shader->module == VK_NULL_HANDLE)
            return -1;

        // reflect shader
        current_shader->reflection = ngli_shaderdesc_create((uint32_t*)current_shader->data, current_shader->data_size);
        if (!current_shader->reflection)
            return -1;

        if (current_shader->reflection->bindings) {
            const struct hmap_entry *binding_entry = NULL;
            while ((binding_entry = ngli_hmap_next(current_shader->reflection->bindings, binding_entry))) {
                const struct shaderbinding *binding = binding_entry->data;
                if ((binding->flag & NGLI_SHADER_CONSTANT)) {
                    const struct shaderblock *block = binding_entry->data;
                    add_shader_constant(constants, block->size, constant_stages[i]);
                }
                else if ((binding->flag & NGLI_SHADER_UNIFORM))
                    add_shader_binding(bindings, binding->index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                else if ((binding->flag & NGLI_SHADER_STORAGE))
                    add_shader_binding(bindings, binding->index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            };
        }
    }

     // create bindings and buffers needed
    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    // constants
    const uint32_t nb_constants = ngli_darray_size(constants);
    if (nb_constants) {
        pipeline_layout_create_info.pushConstantRangeCount = nb_constants;
        pipeline_layout_create_info.pPushConstantRanges = ngli_darray_begin(constants);
        s->flag |= NGLI_PROGRAM_CONSTANT_ATTACHED;
    }

    // bindings
    const uint32_t nb_bindings = ngli_darray_size(bindings);
    if (nb_bindings) {
        VkDescriptorPoolSize *descriptor_pool_sizes = calloc(NGLI_RENDERER_BUFFER_TYPE_COUNT, sizeof(struct VkDescriptorPoolSize));
        if (!descriptor_pool_sizes)
            goto fail;

        static VkDescriptorType types[NGLI_RENDERER_BUFFER_TYPE_COUNT] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        };
        for (uint32_t i = 0; i < NGLI_RENDERER_BUFFER_TYPE_COUNT; i++) {
            VkDescriptorPoolSize *descriptor_pool_size = &descriptor_pool_sizes[i];
            descriptor_pool_size->type = types[i];
            descriptor_pool_size->descriptorCount = 16; // ???
        }

        VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .poolSizeCount = NGLI_RENDERER_BUFFER_TYPE_COUNT,
            .pPoolSizes = descriptor_pool_sizes,
            .maxSets = vk->nb_framebuffers,
        };

        // TODO: descriptor tool should be shared for all nodes
        vkret = vkCreateDescriptorPool(vk->device, &descriptor_pool_create_info, NULL, &s->descriptor_pool);
        free(descriptor_pool_sizes);
        if (vkret != VK_SUCCESS)
            goto fail;

        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = nb_bindings,
            .pBindings = ngli_darray_begin(bindings),
        };

        // create descriptor_set_layout
        vkret = vkCreateDescriptorSetLayout(vk->device, &descriptor_set_layout_create_info, NULL, &s->descriptor_set_layout);
        if (vkret != VK_SUCCESS)
            goto fail;

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

        s->descriptor_sets = calloc(vk->nb_framebuffers, sizeof(*s->descriptor_sets));
        vkret = vkAllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, s->descriptor_sets);
        free(descriptor_set_layouts);
        if (vkret != VK_SUCCESS)
            goto fail;

        s->flag |= NGLI_PROGRAM_BUFFER_ATTACHED;
    }

    if (s->descriptor_set_layout != VK_NULL_HANDLE) {
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = &s->descriptor_set_layout;
    }

    vkret = vkCreatePipelineLayout(vk->device, &pipeline_layout_create_info, NULL, &s->layout);
    if (vkret != VK_SUCCESS)
        goto fail;

    ngli_darray_freep(&bindings);
    ngli_darray_freep(&constants);
#else
    struct glcontext *gl = ctx->glcontext;
    static const uint32_t shader_types[NGLI_SHADER_TYPE_COUNT] = {
        GL_VERTEX_SHADER,
        GL_FRAGMENT_SHADER,
        GL_COMPUTE_SHADER
    };

    s->program_id = ngli_glCreateProgram(gl);
    for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
        struct shader* current_shader = &s->shaders[i];

        if (!current_shader->content)
            continue;

        current_shader->shader_id = ngli_glCreateShader(gl, shader_types[i]);
        ngli_glShaderSource(gl, current_shader->shader_id, 1, &current_shader->content, NULL);
        ngli_glCompileShader(gl, current_shader->shader_id);
        if (ngli_program_check_status(gl, current_shader->shader_id, GL_COMPILE_STATUS) < 0)
            goto fail;

        ngli_glAttachShader(gl, s->program_id, current_shader->shader_id);
    }
    ngli_glLinkProgram(gl, s->program_id);
    if (ngli_program_check_status(gl, s->program_id, GL_LINK_STATUS) < 0)
        goto fail;

    s->active_uniforms = ngli_program_probe_uniforms(node->name, gl, s->program_id);
    if (!s->active_uniforms)
        goto fail;

    if (!s->shaders[NGLI_SHADER_TYPE_COMPUTE].content) {
        s->active_attributes = ngli_program_probe_attributes(node->name, gl, s->program_id);
        if (!s->active_attributes)
            goto fail;
    }
#endif

    return 0;

fail:
#ifdef VULKAN_BACKEND
    ngli_darray_freep(&bindings);
    ngli_darray_freep(&constants);
#endif
    ngli_program_uninit(node);
    return -1;
}

void ngli_program_uninit(struct ngl_node *node) {
    struct ngl_ctx *ctx = node->ctx;
    struct program *s = node->priv_data;

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;

    if (s->descriptor_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk->device, s->descriptor_set_layout, NULL);

    if (s->descriptor_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk->device, s->descriptor_pool, NULL);

    free(s->descriptor_sets);

    if (s->layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vk->device, s->layout, NULL);

    for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
        struct shader* current_shader = &s->shaders[i];

        if (!current_shader->data)
            continue;

        if (current_shader->reflection)
            ngli_shaderdesc_freep(&current_shader->reflection);

        if (current_shader->module != VK_NULL_HANDLE)
            ngli_renderer_destroy_shader(vk, current_shader->module);
    }
#else
    struct glcontext *gl = ctx->glcontext;

    for (uint8_t i = 0; i < NGLI_SHADER_TYPE_COUNT; i++) {
        struct shader* current_shader = &s->shaders[i];

        if (current_shader->shader_id)
            ngli_glDeleteShader(gl, current_shader->shader_id);
    }

    ngli_hmap_freep(&s->active_uniforms);
    if (s->program_id)
        ngli_glDeleteProgram(gl, s->program_id);
#endif
}