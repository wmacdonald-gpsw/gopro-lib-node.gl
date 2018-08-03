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
#include <limits.h>

#include "buffer.h"
#include "glincludes.h"
#include "hmap.h"
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "spirv.h"
#include "utils.h"

#define TEXTURES_TYPES_LIST (const int[]){NGL_NODE_TEXTURE2D,       \
                                          NGL_NODE_TEXTURE3D,       \
                                          -1}

#define PROGRAMS_TYPES_LIST (const int[]){NGL_NODE_PROGRAM,         \
                                          -1}

#define UNIFORMS_TYPES_LIST (const int[]){NGL_NODE_BUFFERFLOAT,     \
                                          NGL_NODE_BUFFERVEC2,      \
                                          NGL_NODE_BUFFERVEC3,      \
                                          NGL_NODE_BUFFERVEC4,      \
                                          NGL_NODE_UNIFORMFLOAT,    \
                                          NGL_NODE_UNIFORMVEC2,     \
                                          NGL_NODE_UNIFORMVEC3,     \
                                          NGL_NODE_UNIFORMVEC4,     \
                                          NGL_NODE_UNIFORMQUAT,     \
                                          NGL_NODE_UNIFORMINT,      \
                                          NGL_NODE_UNIFORMMAT4,     \
                                          -1}

#define ATTRIBUTES_TYPES_LIST (const int[]){NGL_NODE_BUFFERFLOAT,   \
                                            NGL_NODE_BUFFERVEC2,    \
                                            NGL_NODE_BUFFERVEC3,    \
                                            NGL_NODE_BUFFERVEC4,    \
                                            -1}

#define GEOMETRY_TYPES_LIST (const int[]){NGL_NODE_CIRCLE,          \
                                          NGL_NODE_GEOMETRY,        \
                                          NGL_NODE_QUAD,            \
                                          NGL_NODE_TRIANGLE,        \
                                          -1}

#define BUFFERS_TYPES_LIST (const int[]){NGL_NODE_BUFFERFLOAT,      \
                                         NGL_NODE_BUFFERVEC2,       \
                                         NGL_NODE_BUFFERVEC3,       \
                                         NGL_NODE_BUFFERVEC4,       \
                                         NGL_NODE_BUFFERINT,        \
                                         NGL_NODE_BUFFERIVEC2,      \
                                         NGL_NODE_BUFFERIVEC3,      \
                                         NGL_NODE_BUFFERIVEC4,      \
                                         NGL_NODE_BUFFERUINT,       \
                                         NGL_NODE_BUFFERUIVEC2,     \
                                         NGL_NODE_BUFFERUIVEC3,     \
                                         NGL_NODE_BUFFERUIVEC4,     \
                                         -1}

#define OFFSET(x) offsetof(struct render, x)
static const struct node_param render_params[] = {
    {"geometry", PARAM_TYPE_NODE, OFFSET(geometry), .flags=PARAM_FLAG_CONSTRUCTOR,
                 .node_types=GEOMETRY_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("geometry to be rasterized")},
    {"program",  PARAM_TYPE_NODE, OFFSET(pipeline.program),
                 .node_types=PROGRAMS_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("program to be executed")},
    {"textures", PARAM_TYPE_NODEDICT, OFFSET(pipeline.textures),
                 .node_types=TEXTURES_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("textures made accessible to the `program`")},
    {"uniforms", PARAM_TYPE_NODEDICT, OFFSET(pipeline.uniforms),
                 .node_types=UNIFORMS_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("uniforms made accessible to the `program`")},
    {"buffers",  PARAM_TYPE_NODEDICT, OFFSET(pipeline.buffers),
                 .node_types=BUFFERS_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("buffers made accessible to the `program`")},
    {"attributes", PARAM_TYPE_NODEDICT, OFFSET(attributes),
                 .node_types=ATTRIBUTES_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("extra vertex attributes made accessible to the `program`")},
    {"instance_attributes", PARAM_TYPE_NODEDICT, OFFSET(instance_attributes),
                 .node_types=ATTRIBUTES_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("per instance extra vertex attributes made accessible to the `program`")},
    {"nb_instances", PARAM_TYPE_INT, OFFSET(nb_instances),
                 .desc=NGLI_DOCSTRING("number of instances to draw")},
    {NULL}
};

#ifndef VULKAN_BACKEND
static int update_geometry_uniforms(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

    const float *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    if (s->modelview_matrix_location_id >= 0) {
        ngli_glUniformMatrix4fv(gl, s->modelview_matrix_location_id, 1, GL_FALSE, modelview_matrix);
    }

    if (s->projection_matrix_location_id >= 0) {
        ngli_glUniformMatrix4fv(gl, s->projection_matrix_location_id, 1, GL_FALSE, projection_matrix);
    }

    if (s->normal_matrix_location_id >= 0) {
        float normal_matrix[3*3];
        ngli_mat3_from_mat4(normal_matrix, modelview_matrix);
        ngli_mat3_inverse(normal_matrix, normal_matrix);
        ngli_mat3_transpose(normal_matrix, normal_matrix);
        ngli_glUniformMatrix3fv(gl, s->normal_matrix_location_id, 1, GL_FALSE, normal_matrix);
    }

    return 0;
}
#endif

#define GEOMETRY_OFFSET(x) offsetof(struct geometry, x)
static const struct {
    const char *const_name;
    int offset;
} attrib_const_map[] = {
    {"ngl_position", GEOMETRY_OFFSET(vertices_buffer)},
    {"ngl_uvcoord",  GEOMETRY_OFFSET(uvcoords_buffer)},
    {"ngl_normal",   GEOMETRY_OFFSET(normals_buffer)},
};

#ifdef VULKAN_BACKEND
static VkResult create_graphics_pipeline(struct ngl_node *node, VkPipeline *pipeline_dstp)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct render *s = node->priv_data;

    struct pipeline *pipeline = &s->pipeline;
    const struct program *program = pipeline->program->priv_data;

    /* Vertex input state */
    VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = s->nb_binds,
        .pVertexBindingDescriptions = s->nb_binds ? s->bind_descs : NULL,
        .vertexAttributeDescriptionCount = s->nb_binds,
        .pVertexAttributeDescriptions = s->nb_binds ? s->attr_descs : NULL,
    };

    struct geometry *geometry = s->geometry->priv_data;

    /* Input Assembly State */
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = geometry->topology,
    };

    /* Viewport */
    VkViewport viewport = {
        .width = vk->config.width,
        .height = vk->config.height,
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };

    VkRect2D scissor = {
        .extent = vk->extent,
    };

    VkPipelineViewportStateCreateInfo viewport_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    /* Rasterization */
    VkPipelineRasterizationStateCreateInfo rasterization_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
    };

    /* Multisampling */
    VkPipelineMultisampleStateCreateInfo multisampling_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    /* Depth & stencil */
    // XXX

    /* Blend */
    const struct glstate *vkstate = &ctx->glstate;
    VkPipelineColorBlendAttachmentState colorblend_attachment_state = {
        .blendEnable = vkstate->blend,
        .srcColorBlendFactor = vkstate->blend_src_factor,
        .dstColorBlendFactor = vkstate->blend_dst_factor,
        .colorBlendOp = vkstate->blend_op,
        .srcAlphaBlendFactor = vkstate->blend_src_factor_a,
        .dstAlphaBlendFactor = vkstate->blend_dst_factor_a,
        .alphaBlendOp = vkstate->blend_op_a,
        .colorWriteMask = vkstate->color_write_mask,
    };

    VkPipelineColorBlendStateCreateInfo colorblend_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment_state,
    };

    /* Dynamic states */
#if 0
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_LINE_WIDTH,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = NGLI_ARRAY_NB(dynamic_states),
        .pDynamicStates = dynamic_states,

    };
#endif
    VkGraphicsPipelineCreateInfo graphics_pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = NGLI_ARRAY_NB(program->shader_stage_create_info),
        .pStages = program->shader_stage_create_info,
        .pVertexInputState = &vertex_input_state_create_info,
        .pInputAssemblyState = &input_assembly_state_create_info,
        .pViewportState = &viewport_state_create_info,
        .pRasterizationState = &rasterization_state_create_info,
        .pMultisampleState = &multisampling_state_create_info,
        .pDepthStencilState = NULL,
        .pColorBlendState = &colorblend_state_create_info,
        .pDynamicState = NULL,
        .layout = pipeline->pipeline_layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    return vkCreateGraphicsPipelines(vk->device, NULL, 1,
                                     &graphics_pipeline_create_info,
                                     NULL, pipeline_dstp);
}

static int init_vertex_input_attrib_desc(struct ngl_node *node)
{
    struct render *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;

    s->bind_descs     = calloc(s->nb_attribute_pairs, sizeof(*s->bind_descs));
    s->attr_descs     = calloc(s->nb_attribute_pairs, sizeof(*s->attr_descs));
    s->vkbufs         = calloc(s->nb_attribute_pairs, sizeof(*s->vkbufs));
    s->vkbufs_offsets = calloc(s->nb_attribute_pairs, sizeof(*s->vkbufs_offsets));
    if (!s->bind_descs || !s->attr_descs || !s->vkbufs || !s->vkbufs_offsets)
        return -1;

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        const struct spirv_variable *info = pair->program_info;
        struct buffer *buffer = pair->node->priv_data;
        struct graphic_buffer *graphic_buffer = &buffer->graphic_buffer;

        VkVertexInputBindingDescription bind_desc = {
            .binding = s->nb_binds,
            .stride = buffer->data_stride,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, // XXX: here for instanced rendering
        };

        VkFormat data_format = VK_FORMAT_UNDEFINED;
        int ret = ngli_format_get_vk_format(vk, buffer->data_format, &data_format);
        if (ret < 0)
            return ret;

        VkVertexInputAttributeDescription attr_desc = {
            .binding = s->nb_binds,
            .location = info->offset,
            .format = data_format,
        };

        s->bind_descs[s->nb_binds] = bind_desc;
        s->attr_descs[s->nb_binds] = attr_desc;
        s->vkbufs[s->nb_binds] = graphic_buffer->vkbuf;
        s->nb_binds++;
    }

    return 0;
}
#else
static int update_vertex_attribs(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        const struct attributeprograminfo *info = pair->program_info;
        const GLint aid = info->id;
        struct buffer *buffer = pair->node->priv_data;

        ngli_glEnableVertexAttribArray(gl, aid);
        ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer->graphic_buffer.id);
        ngli_glVertexAttribPointer(gl, aid, buffer->data_comp, GL_FLOAT, GL_FALSE, buffer->data_stride, NULL);

        if (i >= s->first_instance_attribute_index)
            ngli_glVertexAttribDivisor(gl, aid, 1);
    }

    return 0;
}

static int disable_vertex_attribs(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        const struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        const struct attributeprograminfo *info = pair->program_info;
        ngli_glDisableVertexAttribArray(gl, info->id);
    }

    return 0;
}

static int get_uniform_location(struct hmap *uniforms, const char *name)
{
    const struct uniformprograminfo *info = ngli_hmap_get(uniforms, name);
    return info ? info->id : -1;
}
#endif

static int pair_node_to_attribinfo(struct render *s, const char *name,
                                   struct ngl_node *anode)
{
    const struct ngl_node *pnode = s->pipeline.program;
    const struct program *program = pnode->priv_data;

#ifdef VULKAN_BACKEND
    const struct spirv_variable *active_attribute =
        ngli_hmap_get(program->vert_desc->attributes, name);
#else
    const struct attributeprograminfo *active_attribute =
        ngli_hmap_get(program->active_attributes, name);
#endif

    if (!active_attribute)
        return 1;

#ifndef VULKAN_BACKEND // XXX
    if (active_attribute->id < 0)
        return 0;
#endif

    struct nodeprograminfopair pair = {
        .node = anode,
        .program_info = (void *)active_attribute,
    };
    snprintf(pair.name, sizeof(pair.name), "%s", name);
    s->attribute_pairs[s->nb_attribute_pairs++] = pair;
    return 0;
}

static int pair_nodes_to_attribinfo(struct ngl_node *node, struct hmap *attributes,
                                    int per_instance)
{
    if (!attributes)
        return 0;

    struct render *s = node->priv_data;

    const struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(attributes, entry))) {
        struct ngl_node *anode = entry->data;
        struct buffer *buffer = anode->priv_data;

        if (per_instance) {
            if (buffer->count != s->nb_instances) {
                LOG(ERROR,
                    "attribute buffer %s count (%d) does not match instance count (%d)",
                    entry->key,
                    buffer->count,
                    s->nb_instances);
                return -1;
            }
        } else {
            struct geometry *geometry = s->geometry->priv_data;
            struct buffer *vertices = geometry->vertices_buffer->priv_data;
            if (buffer->count != vertices->count) {
                LOG(ERROR,
                    "attribute buffer %s count (%d) does not match vertices count (%d)",
                    entry->key,
                    buffer->count,
                    vertices->count);
                return -1;
            }
        }

        int ret = pair_node_to_attribinfo(s, entry->key, anode);
        if (ret < 0)
            return ret;

        if (ret == 1) {
            const struct ngl_node *pnode = s->pipeline.program;
            LOG(WARNING, "attribute %s attached to %s not found in %s",
                entry->key, node->name, pnode->name);
        }
    }
    return 0;
}

static int render_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *vk = ctx->glcontext;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

    if (!s->pipeline.program) {
        s->pipeline.program = ngl_node_create(NGL_NODE_PROGRAM);
        if (!s->pipeline.program)
            return -1;
        int ret = ngli_node_attach_ctx(s->pipeline.program, ctx);
        if (ret < 0)
            return ret;
    }

#ifdef VULKAN_BACKEND
    s->pipeline.create_func = create_graphics_pipeline;
    s->pipeline.queue_family_id = vk->queue_family_graphics_id;
#endif

    int ret = ngli_pipeline_init(node);
    if (ret < 0)
        return ret;

    /* Instancing checks */
#ifdef VULKAN_BACKEND
    // TODO
#else
    if (s->nb_instances && !(gl->features & NGLI_FEATURE_DRAW_INSTANCED)) {
        LOG(ERROR, "context does not support instanced draws");
        return -1;
    }

    if (s->instance_attributes && !(gl->features & NGLI_FEATURE_INSTANCED_ARRAY)) {
        LOG(ERROR, "context does not support instanced arrays");
        return -1;
    }
#endif

    /* Builtin uniforms */
#ifdef VULKAN_BACKEND
    // FIXME: modelview/projection are handled differently (constants), but
    // normals need to be handled
#else
    struct ngl_node *pnode = s->pipeline.program;
    struct program *program = pnode->priv_data;
    struct hmap *uniforms = program->active_uniforms;

    s->modelview_matrix_location_id  = get_uniform_location(uniforms, "ngl_modelview_matrix");
    s->projection_matrix_location_id = get_uniform_location(uniforms, "ngl_projection_matrix");
    s->normal_matrix_location_id     = get_uniform_location(uniforms, "ngl_normal_matrix");
#endif

    /* User and builtin attribute pairs */
    const int max_nb_attributes = NGLI_ARRAY_NB(attrib_const_map)
                                + (s->attributes ? ngli_hmap_count(s->attributes) : 0)
                                + (s->instance_attributes ? ngli_hmap_count(s->instance_attributes) : 0);
    s->attribute_pairs = calloc(max_nb_attributes, sizeof(*s->attribute_pairs));
    if (!s->attribute_pairs)
        return -1;

    /* Builtin vertex attributes */
    struct geometry *geometry = s->geometry->priv_data;
    for (int i = 0; i < NGLI_ARRAY_NB(attrib_const_map); i++) {
        const int offset = attrib_const_map[i].offset;
        const char *const_name = attrib_const_map[i].const_name;
        uint8_t *buffer_node_p = ((uint8_t *)geometry) + offset;
        struct ngl_node *anode = *(struct ngl_node **)buffer_node_p;
        if (!anode)
            continue;

        ret = pair_node_to_attribinfo(s, const_name, anode);
        if (ret < 0)
            return ret;
    }

    /* User vertex attributes */
    ret = pair_nodes_to_attribinfo(node, s->attributes, 0);
    if (ret < 0)
        return ret;

    /* User per instance vertex attributes */
    s->first_instance_attribute_index = s->nb_attribute_pairs;
    ret = pair_nodes_to_attribinfo(node, s->instance_attributes, 1);
    if (ret < 0)
        return ret;

    /* Allocate buffers */
    struct buffer *buffer = geometry->indices_buffer->priv_data;
    struct graphic_buffer *graphic_buffer = &buffer->graphic_buffer;
#ifdef VULKAN_BACKEND
    ret = ngli_graphic_buffer_allocate(gl, graphic_buffer, buffer->data_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
#else
    ret = ngli_graphic_buffer_allocate(gl, graphic_buffer, buffer->data_size, buffer->usage);
#endif
    if (ret < 0)
        return ret;
    ngli_graphic_buffer_upload(gl, graphic_buffer, buffer->data, buffer->data_size);

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        struct buffer *buffer = (struct buffer *)pair->node->priv_data;
        struct graphic_buffer *graphic_buffer = &buffer->graphic_buffer;
#ifdef VULKAN_BACKEND
        ret = ngli_graphic_buffer_allocate(gl, graphic_buffer, buffer->data_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
#else
        ret = ngli_graphic_buffer_allocate(gl, graphic_buffer, buffer->data_size, buffer->usage);
#endif
        if (ret < 0)
            return ret;
        ngli_graphic_buffer_upload(gl, graphic_buffer, buffer->data, buffer->data_size);
    }

#ifdef VULKAN_BACKEND
    ret = init_vertex_input_attrib_desc(node);
    if (ret < 0)
        return ret;
#else
    if (gl->features & NGLI_FEATURE_VERTEX_ARRAY_OBJECT) {
        ngli_glGenVertexArrays(gl, 1, &s->vao_id);
        ngli_glBindVertexArray(gl, s->vao_id);
        update_vertex_attribs(node);
    }
#endif

    return 0;
}

static void render_uninit(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

#ifdef VULKAN_BACKEND
    free(s->bind_descs);
    free(s->attr_descs);
    free(s->vkbufs);
    free(s->vkbufs_offsets);
#else
    if (gl->features & NGLI_FEATURE_VERTEX_ARRAY_OBJECT) {
        ngli_glDeleteVertexArrays(gl, 1, &s->vao_id);
    }
#endif

    ngli_pipeline_uninit(node);

    struct geometry *geometry = s->geometry->priv_data;
    struct buffer *buffer = geometry->indices_buffer->priv_data;
    ngli_graphic_buffer_free(gl, &buffer->graphic_buffer);

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        struct buffer *buffer = (struct buffer *)pair->node->priv_data;
        ngli_graphic_buffer_free(gl, &buffer->graphic_buffer);
    }

    free(s->attribute_pairs);
}

static int render_update(struct ngl_node *node, double t)
{
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;
    struct render *s = node->priv_data;

    int ret = ngli_node_update(s->geometry, t);
    if (ret < 0)
        return ret;

    for (int i = 0; i < s->nb_attribute_pairs; i++) {
        struct nodeprograminfopair *pair = &s->attribute_pairs[i];
        struct ngl_node *bnode = (struct ngl_node *)pair->node;
        int ret = ngli_node_update(bnode, t);
        if (ret < 0)
            return ret;

        struct buffer *buffer = bnode->priv_data;
        if (buffer->dynamic && buffer->graphic_buffer_last_upload_time != node->last_update_time) {
            ngli_graphic_buffer_upload(gl, &buffer->graphic_buffer, buffer->data, buffer->data_size);
        }
    }

    return ngli_pipeline_update(node, t);
}

static void render_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct render *s = node->priv_data;

#ifdef VULKAN_BACKEND
    struct glcontext *vk = ctx->glcontext;

    ngli_pipeline_upload_data(node);

    VkCommandBuffer cmd_buf = s->pipeline.command_buffers[vk->img_index];

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };

    VkResult ret = vkBeginCommandBuffer(cmd_buf, &command_buffer_begin_info);
    if (ret != VK_SUCCESS)
        return;

    const float *rgba = vk->config.clear_color;
    VkClearValue clear_color = {.color.float32={rgba[0], rgba[1], rgba[2], rgba[3]}};
    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk->render_pass,
        .framebuffer = vk->framebuffers[vk->img_index],
        .renderArea = {
            .extent = vk->extent,
        },
        .clearValueCount = 1,
        .pClearValues = &clear_color,
    };

    vkCmdBeginRenderPass(cmd_buf, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline.vkpipeline);

    vkCmdBindVertexBuffers(cmd_buf, 0, s->nb_binds, s->vkbufs, s->vkbufs_offsets);

    const size_t matrix_size = 4 * 4 * sizeof(float);
    const float *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    vkCmdPushConstants(cmd_buf, s->pipeline.pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, matrix_size,
                       modelview_matrix);
    vkCmdPushConstants(cmd_buf, s->pipeline.pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, matrix_size, matrix_size,
                       projection_matrix);

    const struct geometry *geometry = s->geometry->priv_data;
    const struct buffer *indices_buffer = geometry->indices_buffer->priv_data;

    const int index_node_type = geometry->indices_buffer->class->id;
    ngli_assert(index_node_type == NGL_NODE_BUFFERUSHORT ||
                index_node_type == NGL_NODE_BUFFERUINT);
    VkIndexType index_type = index_node_type == NGL_NODE_BUFFERUINT ? VK_INDEX_TYPE_UINT32
                                                                    : VK_INDEX_TYPE_UINT16;
    vkCmdBindIndexBuffer(cmd_buf, indices_buffer->graphic_buffer.vkbuf, 0, index_type);

    //if (s->pipeline.nb_uniform_buffers) {
        // TODO: should handle all type of buffer in different stages
    if (s->pipeline.descriptor_sets) {
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline.pipeline_layout,
                                0, 1, &s->pipeline.descriptor_sets[vk->img_index], 0, NULL);
    }

    vkCmdDrawIndexed(cmd_buf, indices_buffer->count, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd_buf);

    ret = vkEndCommandBuffer(cmd_buf);
    if (ret != VK_SUCCESS)
        return;

    vk->command_buffers[vk->nb_command_buffers++] = cmd_buf;
#else
    struct glcontext *gl = ctx->glcontext;

    const struct program *program = s->pipeline.program->priv_data;

    ngli_glUseProgram(gl, program->program_id);

    if (gl->features & NGLI_FEATURE_VERTEX_ARRAY_OBJECT) {
        ngli_glBindVertexArray(gl, s->vao_id);
    } else {
        update_vertex_attribs(node);
    }

    update_geometry_uniforms(node);

    int ret = ngli_pipeline_upload_data(node);
    if (ret < 0) {
        LOG(ERROR, "pipeline upload data error");
    }

    const struct geometry *geometry = s->geometry->priv_data;
    const struct buffer *indices_buffer = geometry->indices_buffer->priv_data;

    GLenum indices_type;
    ngli_format_get_gl_format_type(gl, indices_buffer->data_format, NULL, NULL, &indices_type);

    ngli_glBindBuffer(gl, GL_ELEMENT_ARRAY_BUFFER, indices_buffer->graphic_buffer.id);
    if (s->nb_instances > 0) {
        ngli_glDrawElementsInstanced(gl, geometry->topology, indices_buffer->count, indices_type, 0, s->nb_instances);
    } else {
        ngli_glDrawElements(gl, geometry->topology, indices_buffer->count, indices_type, 0);
    }

    if (!(gl->features & NGLI_FEATURE_VERTEX_ARRAY_OBJECT)) {
        disable_vertex_attribs(node);
    }
#endif
}

const struct node_class ngli_render_class = {
    .id        = NGL_NODE_RENDER,
    .name      = "Render",
    .init      = render_init,
    .uninit    = render_uninit,
    .update    = render_update,
    .draw      = render_draw,
    .priv_size = sizeof(struct render),
    .params    = render_params,
    .file      = __FILE__,
};
