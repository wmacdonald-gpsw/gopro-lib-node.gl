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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spirv.h"
#include "utils.h"
#include "hmap.h"

struct spirv_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t gen_magic;
    uint32_t bound;
    uint32_t reserved;
};

struct shader_variable_internal
{
    const char *name;
    uint16_t offset;
};

struct shader_type_internal
{
    const char *name;
    struct shader_variable_internal variables[8];
    uint16_t size;
    uint8_t nb_variables;
    uint8_t index;
    uint8_t flag;
};

// TODO: remove stack limitation?
struct shader_internal
{
    struct shader_type_internal types[64];

    uint16_t sizes[64];
    uint8_t variable_type_indices[64];
    uint8_t nb_variables;
    uint8_t buffer_type_indices[64];
    uint8_t nb_buffers;
};

struct shader_reflection* ngli_spirv_create_reflection(const uint32_t *code, size_t size)
{
    // header
    if (size < sizeof(struct spirv_header))
        return NULL;

    struct spirv_header *header = (struct spirv_header*)code;
    if (header->magic != 0x07230203)
        return NULL;
    if (header->version != 0x00010000) // XXX: allow more?
        return NULL;

    code += sizeof(struct spirv_header) / sizeof(uint32_t);
    size -= sizeof(struct spirv_header);

    // data
    struct shader_internal internal = {0};
    while (size > 0) {
        const uint32_t opcode0    = code[0];
        const uint16_t opcode     = opcode0 & 0xffff;
        const uint16_t word_count = opcode0 >> 16;

        // check instruction size
        const uint32_t instruction_size = word_count * sizeof(uint32_t);
        if (size < instruction_size)
            return NULL;

        switch(opcode) {
            // OpName
            case 5: {
                const uint32_t type_id = code[1];
                const char *name = (const char *)&code[2];

                struct shader_type_internal *type = &internal.types[type_id];
                type->name = name;
                break;
            }

            // OpMemberName
            case 6: {
                const uint32_t type_id = code[1];
                const uint32_t variable_index = code[2];
                const char *name = (const char *)&code[3];

                struct shader_type_internal *type = &internal.types[type_id];
                type->nb_variables++;

                struct shader_variable_internal *variable = &type->variables[variable_index];
                variable->name = name;
                break;
            }

            // OpTypeFloat
            case 22: {
                const uint32_t type_id = code[1];
                const uint32_t type_size = code[2];
                internal.sizes[type_id] = type_size / 8;
                break;
            }

            // OpTypeVector
            case 23: {
                const uint32_t type_id = code[1];
                const uint32_t component_type_id = code[2];
                const uint32_t component_count = code[3];
                internal.sizes[type_id] = internal.sizes[component_type_id] * component_count;
                break;
            }

            // OpTypeMatrix
            case 24: {
                const uint32_t type_id = code[1];
                const uint32_t column_type_id = code[2];
                const uint32_t column_count = code[3];
                internal.sizes[type_id] = internal.sizes[column_type_id] * column_count;
                break;
            }

            // OpTypeStruct
            case 30: {
                const uint32_t type_id = code[1];

                struct shader_type_internal *type = &internal.types[type_id];
                const uint8_t last_variable_index = type->nb_variables-1;
                const uint32_t member_type_id = code[2 + last_variable_index];
                type->size = type->variables[last_variable_index].offset + internal.sizes[member_type_id];
                break;
            }

            // OpTypeVariable
            case 32: {
                const uint32_t type_id = code[3];
                const uint32_t storage_type = code[2];

                struct shader_type_internal *type = &internal.types[type_id];
                switch (storage_type) {

                    // Uniform
                    case 2: {
                        type->flag |= NGL_SHADER_BUFFER_UNIFORM;
                        break;
                    }

                    // PushConstant
                    case 9: {
                        type->flag |= NGL_SHADER_BUFFER_CONSTANT;
                        break;
                    }
                }
                break;
            }

            // OpVariable
            case 59: {
                const uint32_t type_id = code[2];
                const uint32_t storage_type = code[3];

                struct shader_type_internal *type = &internal.types[type_id];
                switch (storage_type) {
                    // Input
                    case 1: {
                        type->flag |= NGL_SHADER_VARIABLE_INPUT;
                        break;
                    }

                    // Output
                    case 3: {
                        type->flag |= NGL_SHADER_VARIABLE_OUTPUT;
                        break;
                    }
                }
                break;
            }

            // OpDecorate
            case 71: {
                const uint32_t type_id = code[1];
                const uint32_t decoration = code[2];
                switch (decoration) {
                    // Block
                    case 2: {
                        internal.buffer_type_indices[internal.nb_buffers++] = type_id;
                        break;
                    }

                    // Location
                    case 30: {
                        const uint32_t index = code[3];
                        internal.types[type_id].index = index;
                        internal.variable_type_indices[internal.nb_variables++] = type_id;
                        break;
                    }
                }
                break;
            }

            // OpMemberDecorate
            case 72: {
                const uint32_t type_id = code[1];
                const uint32_t variable_index = code[2];
                const uint32_t decoration = code[3];

                // Offset
                if(decoration == 35) {
                    const uint32_t offset = code[4];
                    struct shader_variable_internal *variable = &internal.types[type_id].variables[variable_index];
                    variable->offset = offset;
                }
                break;
            }
        }
        code += word_count;
        size -= instruction_size;
    }

    // allocate shader_reflection memory
    uint32_t variable_bytes = internal.nb_variables * sizeof(struct shader_variable_reflection);
    uint32_t buffer_bytes = internal.nb_buffers * sizeof(struct shader_buffer_reflection);
    for (uint32_t i = 0; i < internal.nb_buffers; i++) {
        const uint8_t buffer_type_id = internal.buffer_type_indices[i];
        struct shader_type_internal *type = &internal.types[buffer_type_id];
        buffer_bytes += type->nb_variables * sizeof(struct shader_variable_reflection);
    }
    uint32_t reflection_bytes = sizeof(struct shader_reflection) + variable_bytes + buffer_bytes;
    uint8_t *allocation = malloc(reflection_bytes);

    // initialize shader_reflection
    struct shader_reflection *reflection = (struct shader_reflection*)allocation;
    reflection->variables = internal.nb_variables ? ngli_hmap_create() : NULL;
    reflection->buffers = internal.nb_buffers ? ngli_hmap_create() : NULL;

    // initialize variables
    struct shader_variable_reflection *variable = (struct shader_variable_reflection*)(allocation + sizeof(struct shader_reflection));
    for (uint32_t i = 0; i < internal.nb_variables; i++) {
        const uint8_t variable_type_id = internal.variable_type_indices[i];
        struct shader_type_internal *type = &internal.types[variable_type_id];

        variable->offset = type->index;
        variable->flag = type->flag;
        ngli_hmap_set(reflection->variables, type->name, variable++);
    }

    // initialize buffer
    struct shader_buffer_reflection *buffer = (struct shader_buffer_reflection*)variable;
    for (uint32_t i = 0; i < internal.nb_buffers; i++) {
        const uint8_t buffer_type_id = internal.buffer_type_indices[i];
        struct shader_type_internal *type_internal = &internal.types[buffer_type_id];

        buffer->flag = type_internal->flag;
        buffer->size = type_internal->size;
        buffer->variables = NULL;
        if (type_internal->nb_variables)
        {
            buffer->variables = ngli_hmap_create();
            variable = (struct shader_variable_reflection*)(((uint8_t*)buffer) + sizeof(struct shader_buffer_reflection));
            for (uint32_t j = 0; j < type_internal->nb_variables; j++) {
                struct shader_variable_internal *variable_internal = &type_internal->variables[j];
                variable->offset = variable_internal->offset;
                variable->flag = 0;
                ngli_hmap_set(buffer->variables, variable_internal->name, variable++);
            }
        }
        ngli_hmap_set(reflection->buffers, type_internal->name, buffer);
        buffer = (struct shader_buffer_reflection*)variable;
    }

    return reflection;
}

void ngli_spirv_destroy_reflection(struct shader_reflection **reflection)
{
    struct shader_reflection *s = *reflection;

    if (!s)
        return;

    ngli_hmap_freep(&s->variables);

    if (s->buffers) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->buffers, entry))) {
            struct shader_buffer_reflection *buffer = entry->data;
            ngli_hmap_freep(&buffer->variables);
        }
    }
    free(s);
    s = NULL;
}