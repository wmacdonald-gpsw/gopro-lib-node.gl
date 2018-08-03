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

#include <string.h>

#include "buffer.h"
#include "glincludes.h"
#include "nodes.h"

int ngli_graphic_buffer_allocate(struct glcontext *gl,
                                 struct graphic_buffer *buffer,
                                 int size,
                                 int usage)
{
#ifdef VULKAN_BACKEND
    if (!buffer->vkbuf) {
        struct glcontext *vk = gl;

        /* Create buffer object */
        VkBufferCreateInfo buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(vk->device, &buffer_create_info, NULL, &buffer->vkbuf) != VK_SUCCESS)
            return -1;

        /* Allocate GPU memory and associate it with buffer object */
        VkMemoryRequirements mem_req;
        vkGetBufferMemoryRequirements(vk->device, buffer->vkbuf, &mem_req);
        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = ngli_vk_find_memory_type(vk, mem_req.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        VkResult vkret = vkAllocateMemory(vk->device, &alloc_info, NULL, &buffer->vkmem);
        if (vkret != VK_SUCCESS)
            return -1;
        vkBindBufferMemory(vk->device, buffer->vkbuf, buffer->vkmem, 0);

        buffer->size = size;
        buffer->usage = usage;
    }
#else
    if (!buffer->id) {
        ngli_glGenBuffers(gl, 1, &buffer->id);
        ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer->id);
        ngli_glBufferData(gl, GL_ARRAY_BUFFER, size, NULL, usage);
        buffer->size = size;
        buffer->usage = usage;
    }
#endif
    buffer->refcount++;
    return 0;
}


void ngli_graphic_buffer_bind(struct glcontext *gl,
                              struct graphic_buffer *buffer,
                              struct pipeline *pipeline,
                              int offset,
                              int size,
                              int index,
                              int type)
{
#ifdef VULKAN_BACKEND
    for (uint32_t i = 0; i < gl->nb_framebuffers; i++) {
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
        vkUpdateDescriptorSets(gl->device, 1, &write_descriptor_set, 0, NULL);
    }
#else
    ngli_glBindBufferRange(gl, type, index, buffer->id, offset, size);
#endif
}

void ngli_graphic_buffer_upload(struct glcontext *gl,
                                struct graphic_buffer *buffer,
                                void *data,
                                int size)
{
#ifdef VULKAN_BACKEND
    struct glcontext *vk = gl;
    void *mapped_mem;
    vkMapMemory(vk->device, buffer->vkmem, 0, size, 0, &mapped_mem); // FIXME: check ret
    memcpy(mapped_mem, data, size);
    vkUnmapMemory(vk->device, buffer->vkmem);
#else
    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer->id);
    ngli_glBufferSubData(gl, GL_ARRAY_BUFFER, 0, size, data);
#endif
}

void *ngli_graphic_buffer_map(struct glcontext *gl,
                              struct graphic_buffer *buffer)
{
#ifdef VULKAN_BACKEND
     void *mapped_memory;
     vkMapMemory(gl->device, buffer->vkmem, 0, buffer->size, 0, &mapped_memory);
     return mapped_memory;
#else
     return NULL;
#endif
}

void ngli_graphic_buffer_unmap(struct glcontext *gl,
                               struct graphic_buffer *buffer)
{
#ifdef VULKAN_BACKEND
    vkUnmapMemory(gl->device, buffer->vkmem);
#else
#endif
}

void ngli_graphic_buffer_free(struct glcontext *gl,
                              struct graphic_buffer *buffer)
{
    if (!buffer)
        return;

    if (buffer->refcount-- == 1) {
#ifdef VULKAN_BACKEND
        struct glcontext *vk = gl;
        vkDestroyBuffer(vk->device, buffer->vkbuf, NULL);
        vkFreeMemory(vk->device, buffer->vkmem, NULL);
#else
        ngli_glDeleteBuffers(gl, 1, &buffer->id);
        buffer->id = 0;
#endif
    }
}
