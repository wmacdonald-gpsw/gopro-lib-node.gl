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

struct spirvheader
{
    uint32_t magic;
    uint32_t version;
    uint32_t gen_magic;
    uint32_t bound;
    uint32_t reserved;
};

struct shaderblockvariableinternal
{
    const char *name;
    uint16_t offset;
};

struct shadertypeinternal
{
    const char *name;
    struct darray *variables;
    uint16_t size;
    uint16_t flag;
    uint8_t index;
};

struct shaderdescinternal
{
    struct darray *types;
    struct darray *attributes;
    struct darray *bindings;
};

struct shaderdesc* ngli_shaderdesc_create(const uint32_t *code, size_t size)
{
    // header
    if (size < sizeof(struct spirvheader))
        return NULL;

    struct spirvheader *header = (struct spirvheader*)code;
    if (header->magic != 0x07230203)
        return NULL;
    if (header->version != 0x00010000) // XXX: allow more?
        return NULL;

    code += sizeof(struct spirvheader) / sizeof(uint32_t);
    size -= sizeof(struct spirvheader);

    // data
    struct shaderdescinternal internal = {0};
    internal.types = ngli_darray_create(sizeof(struct shadertypeinternal));
    internal.attributes = ngli_darray_create(sizeof(uint32_t));
    internal.bindings = ngli_darray_create(sizeof(uint32_t));

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
                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->name = name;
                break;
            }

            // OpMemberName
            case 6: {
                const uint32_t type_id = code[1];
                // const uint32_t variable_index = code[2];
                const char *name = (const char *)&code[3];

                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                if (!type->variables) {
                    type->variables = ngli_darray_create(sizeof(struct shaderblockvariableinternal));
                }
                struct shaderblockvariableinternal *variable = ngli_darray_add(type->variables);
                variable->name = name;

                break;
            }

            // OpTypeFloat
            case 22: {
                const uint32_t type_id = code[1];
                const uint32_t type_size = code[2];
                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = type_size / 8;
                break;
            }

            // OpTypedarray
            case 23: {
                const uint32_t type_id = code[1];
                const uint32_t component_type_id = code[2];
                const uint32_t component_count = code[3];
                struct shadertypeinternal *component_type = ngli_darray_expand_to(internal.types, component_type_id);
                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = component_type->size * component_count;
                break;
            }

            // OpTypeMatrix
            case 24: {
                const uint32_t type_id = code[1];
                const uint32_t column_type_id = code[2];
                const uint32_t column_count = code[3];
                struct shadertypeinternal *column_type = ngli_darray_expand_to(internal.types, column_type_id);
                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->size = column_type->size * column_count;
                break;
            }

            // OpTypeImage
            case 25: {
                const uint32_t type_id = code[1];
                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->flag |= NGLI_SHADER_TEXTURE;
                break;
            }

            // OpTypeSampler
            case 26: {
                const uint32_t pointer_id = code[1];
                const uint32_t type_id = code[2];

                struct shadertypeinternal *pointer_type = ngli_darray_expand_to(internal.types, pointer_id);
                pointer_type->flag = NGLI_SHADER_INDIRECTION;
                pointer_type->index = type_id;

                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->flag |= NGLI_SHADER_SAMPLER;
                break;
            }

            // OpTypeSampledImage
            case 27: {
                const uint32_t pointer_id = code[1];
                const uint32_t type_id = code[2];

                struct shadertypeinternal *pointer_type = ngli_darray_expand_to(internal.types, pointer_id);
                pointer_type->flag = NGLI_SHADER_INDIRECTION;
                pointer_type->index = type_id;

                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                type->flag |= NGLI_SHADER_SAMPLER;
                break;
            }

            // OpTypeRuntimeArray
            case 29: {
                const uint32_t pointer_id = code[1];
                const uint32_t type_id = code[2];

                struct shadertypeinternal *pointer_type = ngli_darray_expand_to(internal.types, pointer_id);
                pointer_type->flag = NGLI_SHADER_INDIRECTION;
                pointer_type->index = type_id;
                break;
            }

            // OpTypeStruct
            case 30: {
                const uint32_t type_id = code[1];

                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                const uint8_t last_variable_index = ngli_darray_size(type->variables)-1;
                const uint32_t member_type_id = code[2 + last_variable_index];
                struct shadertypeinternal *member_type = ngli_darray_expand_to(internal.types, member_type_id);
                struct shaderblockvariableinternal *variable = ngli_darray_get(type->variables, last_variable_index);
                type->size = variable->offset + member_type->size;
                break;
            }

            // OpTypePointer
            case 32: {
                const uint32_t pointer_id = code[1];
                const uint32_t storage_type = code[2];
                const uint32_t type_id = code[3];

                switch (storage_type) {
                    case 0:     // UniformConstant
                    case 2: {   // Uniform
                        struct shadertypeinternal *pointer_type = ngli_darray_expand_to(internal.types, pointer_id);
                        pointer_type->flag = NGLI_SHADER_INDIRECTION;
                        pointer_type->index = type_id;
                        break;
                    }

                    // PushConstant
                    case 9: {
                        struct shadertypeinternal *pointer_type = ngli_darray_expand_to(internal.types, pointer_id);
                        pointer_type->flag = NGLI_SHADER_INDIRECTION;
                        pointer_type->index = type_id;

                        struct shadertypeinternal *block_type = ngli_darray_expand_to(internal.types, type_id);
                        block_type->flag &= ~NGLI_SHADER_UNIFORM;
                        block_type->flag |= NGLI_SHADER_CONSTANT;
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

                struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                switch (storage_type) {
                    // Input
                    case 1: {
                        type->flag |= NGLI_SHADER_INPUT;
                        break;
                    }

                    case 0:     // UniformConstant
                    case 2:     // Uniform
                    case 9: {   // PushConstant
                        // indirection to proper structure
                        uint32_t block_id = pointer_id;
                        struct shadertypeinternal *block_type = NULL;
                        do {
                            block_type = ngli_darray_expand_to(internal.types, block_id);
                            block_id = block_type->index;
                        } while(block_type->flag == NGLI_SHADER_INDIRECTION);

                        type->variables = block_type->variables;
                        type->size = block_type->size;
                        type->flag = block_type->flag;
                        *((uint32_t*)ngli_darray_add(internal.bindings)) = type_id;
                        break;
                    }

                    // Output
                    case 3: {
                        type->flag |= NGLI_SHADER_OUTPUT;
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
                        struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->flag |= NGLI_SHADER_BLOCK;
                        type->flag |= NGLI_SHADER_UNIFORM;
                        break;
                    }

                    // BufferBlock
                    case 3: {
                        struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->flag |= NGLI_SHADER_BLOCK;
                        type->flag |= NGLI_SHADER_STORAGE;
                        break;
                    }

                    // Location
                    case 30: {
                        const uint32_t index = code[3];
                        struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                        type->index = index;
                        type->flag |= NGLI_SHADER_ATTRIBUTE;

                        *((uint32_t*)ngli_darray_add(internal.attributes)) = type_id;
                        break;
                    }

                    // Binding
                    case 33: {
                        const uint32_t index = code[3];
                        struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
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

                switch(decoration) {
                    // Offset
                    case 35: {
                        const uint32_t offset = code[4];
                        struct shadertypeinternal *type = ngli_darray_expand_to(internal.types, type_id);
                        struct shaderblockvariableinternal *variable = ngli_darray_get(type->variables, variable_index);
                        variable->offset = offset;
                    }
                }
                break;
            }
        }
        code += word_count;
        size -= instruction_size;
    }

    // items number
    const uint32_t nb_attributes = ngli_darray_size(internal.attributes);
    const uint32_t nb_bindings = ngli_darray_size(internal.bindings);

    // allocate shader_reflection memory
    uint32_t attribute_bytes = nb_attributes * sizeof(struct shaderattribute);
    uint32_t bindings_bytes = 0;
    for (uint32_t i = 0; i < nb_bindings; i++) {
        const uint32_t bindings_type_index = *(uint32_t *)ngli_darray_get(internal.bindings, i);
        struct shadertypeinternal *type = ngli_darray_get(internal.types, bindings_type_index);
        if ((type->flag & NGLI_SHADER_BLOCK)) {
            bindings_bytes += sizeof(struct shaderblock);
            if (type->variables)
                bindings_bytes += ngli_darray_size(type->variables) * sizeof(struct shaderblockvariable);
        }
        else if ((type->flag & NGLI_SHADER_TEXTURE)) {
            bindings_bytes += sizeof(struct shadertexture);
        }
    }
    uint32_t reflection_bytes = sizeof(struct shaderdesc) + attribute_bytes + bindings_bytes;
    uint8_t *allocation = malloc(reflection_bytes);

    // initialize shaderdesc
    struct shaderdesc *desc = (struct shaderdesc*)allocation;
    desc->attributes = nb_attributes ? ngli_hmap_create() : NULL;
    desc->bindings = nb_bindings ? ngli_hmap_create() : NULL;

    // initialize attributes
    struct shaderattribute *attribute = (struct shaderattribute*)(allocation + sizeof(struct shaderdesc));
    for (uint32_t i = 0; i < nb_attributes; i++) {
        const uint32_t attribute_type_index = *(uint32_t *)ngli_darray_get(internal.attributes, i);
        struct shadertypeinternal *type = ngli_darray_get(internal.types, attribute_type_index);

        attribute->index = type->index;
        attribute->flag = type->flag;
        ngli_hmap_set(desc->attributes, type->name, attribute++);
    }
    ngli_darray_freep(&internal.attributes);

    // initialize bindings
    struct shaderbinding *binding = (struct shaderbinding*)attribute;
    for (uint32_t i = 0; i < nb_bindings; i++) {
        const uint32_t bindings_type_index = *(uint32_t *)ngli_darray_get(internal.bindings, i);
        struct shadertypeinternal *type = ngli_darray_get(internal.types, bindings_type_index);
        binding->index = type->index;
        binding->flag = type->flag;

        // initialize block
        uint32_t binding_byte = 0;
        if ((type->flag & NGLI_SHADER_BLOCK)) {
            struct shaderblock *block = (struct shaderblock *)binding;
            block->size = type->size;

            binding_byte += sizeof(struct shaderblock);
            if (type->variables) {
                block->variables = ngli_hmap_create();
                struct shaderblockvariable *variable = (struct shaderblockvariable *)(((uint8_t*)binding) + binding_byte);
                const uint32_t nb_variables = ngli_darray_size(type->variables);
                for (uint32_t j = 0; j < nb_variables; j++) {
                    struct shaderblockvariableinternal *v = ngli_darray_get(type->variables, j);
                    variable->offset = v->offset;
                    ngli_hmap_set(block->variables, v->name, variable++);
                }
                ngli_darray_freep(&type->variables);
                binding_byte += nb_variables * sizeof(struct shaderblockvariable);
            }
        }
        // initialize texture
        else if ((type->flag & NGLI_SHADER_TEXTURE)) {
            struct shadertexture *texture = (struct shadertexture *)binding;
            texture->format = 0;
            binding_byte += sizeof(struct shadertexture);
        }
        else
            ngli_assert(0);

        ngli_hmap_set(desc->bindings, type->name, binding);
        binding = (struct shaderbinding*)(((uint8_t*)binding) + binding_byte);
    }
    ngli_darray_freep(&internal.bindings);
    ngli_darray_freep(&internal.types);

    return desc;
}

void ngli_shaderdesc_freep(struct shaderdesc **desc)
{
    struct shaderdesc *s = *desc;

    if (!s)
        return;

    ngli_hmap_freep(&s->attributes);
    if (s->bindings) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(s->bindings, entry))) {
            struct shaderbinding *binding = entry->data;
            if((binding->flag) & NGLI_SHADER_BLOCK) {
                struct shaderblock *block = (struct shaderblock *)binding;
                ngli_hmap_freep(&block->variables);
            }
        }
        ngli_hmap_freep(&s->bindings);
    }
    free(s);
    s = NULL;
}