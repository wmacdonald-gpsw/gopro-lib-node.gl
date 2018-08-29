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

#ifndef DARRAY_H
#define DARRAY_H

#include <stdint.h>

struct darray;


// creation/destruction functions
struct darray *ngli_darray_create(uint32_t element_size);
void ngli_darray_freep(struct darray **v);

// information functions
uint32_t ngli_darray_size(const struct darray *v);
void ngli_darray_clear(struct darray *v);

// access functions
void *ngli_darray_get(struct darray *v, uint32_t index);
void *ngli_darray_begin(struct darray *v);
void *ngli_darray_end(struct darray *v);

// modify functions
void *ngli_darray_add(struct darray *v);
void ngli_darray_resize(struct darray *v, uint32_t size);
void ngli_darray_expand(struct darray *v, uint32_t count);
void *ngli_darray_expand_to(struct darray *v, uint32_t index);
void ngli_darray_reserve(struct darray *v, uint32_t capacity);

#endif