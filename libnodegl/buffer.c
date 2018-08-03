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
#include "glcontext.h"
#include "glincludes.h"

int ngli_buffer_allocate(struct buffer *buffer, struct glcontext *gl, int size, int usage)
{
    buffer->gl = gl;
    buffer->size = size;
    buffer->usage = usage;
#ifdef VULKAN_BACKEND
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
#else
    ngli_glGenBuffers(gl, 1, &buffer->id);
    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer->id);
    ngli_glBufferData(gl, GL_ARRAY_BUFFER, size, NULL, usage);
#endif
    return 0;
}

int ngli_buffer_upload(struct buffer *buffer, void *data, int size)
{
#ifdef VULKAN_BACKEND
    struct glcontext *vk = buffer->gl;
    void *mapped_mem;
    VkResult ret = vkMapMemory(vk->device, buffer->vkmem, 0, size, 0, &mapped_mem);
    if (ret != VK_SUCCESS)
        return -1;
    memcpy(mapped_mem, data, size);
    vkUnmapMemory(vk->device, buffer->vkmem);
#else
    struct glcontext *gl = buffer->gl;
    ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer->id);
    ngli_glBufferSubData(gl, GL_ARRAY_BUFFER, 0, size, data);
#endif
    return 0;
}

void *ngli_buffer_map(struct buffer *buffer)
{
    void *mapped_memory = NULL;
#ifdef VULKAN_BACKEND
    struct glcontext *vk = buffer->gl;
    vkMapMemory(vk->device, buffer->vkmem, 0, buffer->size, 0, &mapped_memory);
#endif
    return mapped_memory;
}

void ngli_buffer_unmap(struct buffer *buffer)
{
#ifdef VULKAN_BACKEND
    struct glcontext *vk = buffer->gl;
    vkUnmapMemory(vk->device, buffer->vkmem);
#endif
}

void ngli_buffer_free(struct buffer *buffer)
{
    if (!buffer->gl)
        return;

#ifdef VULKAN_BACKEND
    struct glcontext *vk = buffer->gl;
    vkDestroyBuffer(vk->device, buffer->vkbuf, NULL);
    vkFreeMemory(vk->device, buffer->vkmem, NULL);
#else
    ngli_glDeleteBuffers(buffer->gl, 1, &buffer->id);
    memset(buffer, 0, sizeof(*buffer));
#endif
}
