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

#ifndef SPIRV_H
#define SPIRV_H

#include <stdint.h>

enum {
    NGL_SHADER_VARIABLE_INPUT   = 1 << 0,
    NGL_SHADER_VARIABLE_OUTPUT  = 1 << 1,
    NGL_SHADER_BUFFER_UNIFORM   = 1 << 2,
    NGL_SHADER_BUFFER_CONSTANT  = 1 << 3,
};

struct shader_variable_reflection
{
    const char* name; //TODO: should be hash
    uint32_t hash;
    uint16_t offset;
    uint8_t flag;
};

struct shader_buffer_reflection
{
    struct shader_variable_reflection *variables;
    uint8_t nb_variables;
    uint16_t size;
    uint8_t flag;
};

struct shader_reflection
{
    struct shader_variable_reflection *variables;
    struct shader_buffer_reflection *buffers;
    uint8_t nb_variables;
    uint8_t nb_buffers;
};

int ngli_spirv_create_reflection(const uint32_t *code, size_t size, struct shader_reflection **s);
void ngli_spirv_destroy_reflection(struct shader_reflection *s);

const struct shader_buffer_reflection* ngli_spirv_get_next_buffer(const struct shader_reflection *reflection, uint32_t flag /*= 0*/, const struct shader_buffer_reflection *buffer /*= NULL*/);
const struct shader_variable_reflection* ngli_spirv_find_variable(const struct shader_reflection *reflection, const char *name);

#endif
