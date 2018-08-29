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
#include "darray.h"

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
    struct darray *variables;
    uint16_t size;
    uint8_t index;
    uint8_t flag;
};

struct shader_internal
{
    struct darray *types;
    struct darray *attribute_indices;
    struct darray *block_indices;
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
    internal.types = ngli_darray_create(sizeof(struct shader_type_internal));
    internal.attribute_indices = ngli_darray_create(sizeof(uint32_t));
    internal.block_indices = ngli_darray_create(sizeof(uint32_t));

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

                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                type->name = name;
                break;
            }

            // OpMemberName
            case 6: {
                const uint32_t type_id = code[1];
                // const uint32_t variable_index = code[2];
                const char *name = (const char *)&code[3];

                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                if (!type->variables) {
                    type->variables = ngli_darray_create(sizeof(struct shader_variable_internal));
                }
                struct shader_variable_internal *variable = ngli_darray_add(type->variables);
                variable->name = name;
                break;
            }

            // OpTypeFloat
            case 22: {
                const uint32_t type_id = code[1];
                const uint32_t type_size = code[2];
                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = type_size / 8;
                break;
            }

            // OpTypedarray
            case 23: {
                const uint32_t type_id = code[1];
                const uint32_t component_type_id = code[2];
                const uint32_t component_count = code[3];
                struct shader_type_internal *component_type = ngli_darray_expand_to(internal.types, component_type_id);
                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = component_type->size * component_count;
                break;
            }

            // OpTypeMatrix
            case 24: {
                const uint32_t type_id = code[1];
                const uint32_t column_type_id = code[2];
                const uint32_t column_count = code[3];
                struct shader_type_internal *column_type = ngli_darray_expand_to(internal.types, column_type_id);
                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = column_type->size * column_count;
                break;
            }

            // OpTypeRuntimeArray
            case 29: {
                const uint32_t pointer_id = code[1];
                const uint32_t type_id = code[2];

                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, pointer_id);
                type->index = type_id;
                type->flag = (uint8_t)-1;
                break;
            }

            // OpTypeStruct
            case 30: {
                const uint32_t type_id = code[1];

                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                const uint8_t last_variable_index = ngli_darray_size(type->variables)-1;
                const uint32_t member_type_id = code[2 + last_variable_index];
                struct shader_type_internal *member_type = ngli_darray_expand_to(internal.types, member_type_id);
                struct shader_variable_internal *variable = ngli_darray_get(type->variables, last_variable_index);
                type->size = variable->offset + member_type->size;
                break;
            }

            // OpTypePointer
            case 32: {
                const uint32_t pointer_id = code[1];
                const uint32_t storage_type = code[2];
                const uint32_t type_id = code[3];

                switch (storage_type) {
                    // Uniform
                    case 2: {
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, pointer_id);
                        type->flag = (uint8_t)-1;
                        type->index = type_id;
                        break;
                    }

                    // PushConstant
                    case 9: {
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, pointer_id);
                        type->flag = (uint8_t)-1;
                        type->index = type_id;

                        struct shader_type_internal *block_type = ngli_darray_expand_to(internal.types, type_id);
                        block_type->flag &= ~NGLI_SHADER_BLOCK_UNIFORM;
                        block_type->flag |= NGLI_SHADER_BLOCK_CONSTANT;
                        break;
                    }
                }
                break;
            }

            // OpVariable
            case 59: {
                const uint32_t pointer_id = code[1];
                const uint32_t type_id = code[2];
                const uint32_t storage_type = code[3];

                struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                switch (storage_type) {
                    // Input
                    case 1: {
                        type->flag |= NGLI_SHADER_ATTRIBUTE_INPUT;
                        break;
                    }

                    case 2:     // Uniform
                    case 9: {   // PushConstant
                        // indirection to proper structureu
                        uint32_t block_id = pointer_id;
                        struct shader_type_internal *block_type = NULL;
                        do {
                            block_type = ngli_darray_expand_to(internal.types, block_id);
                            block_id = block_type->index;
                        } while(block_type->flag == (uint8_t)-1);

                        type->variables = block_type->variables;
                        type->size = block_type->size;
                        type->flag = block_type->flag;
                        *((uint32_t*)ngli_darray_add(internal.block_indices)) = type_id;
                        break;
                    }

                    // Output
                    case 3: {
                        type->flag |= NGLI_SHADER_ATTRIBUTE_OUTPUT;
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
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->flag |= NGLI_SHADER_BLOCK_UNIFORM;
                        break;
                    }

                    // BufferBlock
                    case 3: {
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->flag |= NGLI_SHADER_BLOCK_STORAGE;
                        break;
                    }

                    // Location
                    case 30: {
                        const uint32_t index = code[3];
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->index = index;

                        *((uint32_t*)ngli_darray_add(internal.attribute_indices)) = type_id;
                        break;
                    }

                    // Binding
                    case 33: {
                        const uint32_t index = code[3];
                        struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->index = index;
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
                    struct shader_type_internal *type = ngli_darray_expand_to(internal.types, type_id);
                    struct shader_variable_internal *variable = ngli_darray_get(type->variables, variable_index);
                    variable->offset = offset;
                }
                break;
            }
        }
        code += word_count;
        size -= instruction_size;
    }

    // items number
    const uint32_t nb_attributes = ngli_darray_size(internal.attribute_indices);
    const uint32_t nb_blocks = ngli_darray_size(internal.block_indices);

    // allocate shader_reflection memory
    uint32_t attribute_bytes = nb_attributes * sizeof(struct shader_attribute_reflection);
    uint32_t block_bytes = nb_blocks * sizeof(struct shader_block_reflection);
    for (uint32_t i = 0; i < nb_blocks; i++) {
        const uint32_t block_type_index = *(uint32_t *)ngli_darray_get(internal.block_indices, i);
        struct shader_type_internal *type = ngli_darray_get(internal.types, block_type_index);
        if (type->variables)
            block_bytes += ngli_darray_size(type->variables) * sizeof(struct shader_variable_reflection);
    }
    uint32_t reflection_bytes = sizeof(struct shader_reflection) + attribute_bytes + block_bytes;
    uint8_t *allocation = malloc(reflection_bytes);

    // initialize shader_reflection
    struct shader_reflection *reflection = (struct shader_reflection*)allocation;
    reflection->attributes = nb_attributes ? ngli_hmap_create() : NULL;
    reflection->blocks = nb_blocks ? ngli_hmap_create() : NULL;

    // initialize attributes
    struct shader_attribute_reflection *attribute = (struct shader_attribute_reflection*)(allocation + sizeof(struct shader_reflection));
    for (uint32_t i = 0; i < nb_attributes; i++) {
        const uint32_t attribute_type_index = *(uint32_t *)ngli_darray_get(internal.attribute_indices, i);
        struct shader_type_internal *type = ngli_darray_get(internal.types, attribute_type_index);

        attribute->index = type->index;
        attribute->flag = type->flag;
        ngli_hmap_set(reflection->attributes, type->name, attribute++);
    }
    ngli_darray_freep(&internal.attribute_indices);

    // initialize blocks
    struct shader_block_reflection *block = (struct shader_block_reflection*)attribute;
    for (uint32_t i = 0; i < nb_blocks; i++) {
        const uint32_t block_type_index = *(uint32_t *)ngli_darray_get(internal.block_indices, i);
        struct shader_type_internal *type_internal = ngli_darray_get(internal.types, block_type_index);

        block->flag = type_internal->flag;
        block->index = type_internal->index;
        block->size = type_internal->size;
        block->variables = NULL;
        struct shader_variable_reflection *variable = (struct shader_variable_reflection*)(((uint8_t*)block) + sizeof(struct shader_block_reflection));
        if (type_internal->variables)
        {
            block->variables = ngli_hmap_create();
            const uint32_t nb_variables = ngli_darray_size(type_internal->variables);
            for (uint32_t j = 0; j < nb_variables; j++) {
                struct shader_variable_internal *variable_internal = ngli_darray_get(type_internal->variables, j);
                variable->offset = variable_internal->offset;
                variable->flag = 0;
                ngli_hmap_set(block->variables, variable_internal->name, variable++);
            }
            ngli_darray_freep(&type_internal->variables);
        }
        ngli_hmap_set(reflection->blocks, type_internal->name, block);
        block = (struct shader_block_reflection*)variable;
    }
    ngli_darray_freep(&internal.block_indices);
    ngli_darray_freep(&internal.types);

    return reflection;
}

void ngli_spirv_destroy_reflection(struct shader_reflection **reflection)
{
    struct shader_reflection *s = *reflection;

    if (!s)
        return;

    ngli_hmap_freep(&s->attributes);

    if (s->blocks) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->blocks, entry))) {
            struct shader_block_reflection *block = entry->data;
            ngli_hmap_freep(&block->variables);
        }
    }
    free(s);
    s = NULL;
}
