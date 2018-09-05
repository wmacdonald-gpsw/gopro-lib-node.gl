
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

#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

struct glcontext;
struct program;
struct rendererbuffer;

// shaders
void *ngli_renderer_create_shader(struct glcontext *glcontext, const uint8_t *data, uint32_t data_size);
void ngli_renderer_destroy_shader(struct glcontext *glcontext, void *handle);

// buffers
struct rendererbuffer *ngli_renderer_create_buffer(struct glcontext *glcontext, uint32_t size, uint32_t usage);
void ngli_renderer_destroy_buffer(struct glcontext *glcontext, struct rendererbuffer *handle);
void ngli_renderer_bind_buffer(struct glcontext *glcontext, struct program *p, struct rendererbuffer *rb, uint32_t offset, uint32_t size, uint32_t index, uint32_t type);
void *ngli_renderer_map_buffer(struct glcontext *glcontext, struct rendererbuffer* handle);
void ngli_renderer_unmap_buffer(struct glcontext *glcontext, struct rendererbuffer* handle);

// debug
void ngli_renderer_start_time(struct glcontext *glcontext);
void ngli_renderer_stop_time(struct glcontext *glcontext);
uint64_t ngli_renderer_get_time(struct glcontext *glcontext);
void ngli_renderer_marker_begin(struct glcontext *glcontext, const char *name);
void ngli_renderer_marker_end(struct glcontext *glcontext);

#endif