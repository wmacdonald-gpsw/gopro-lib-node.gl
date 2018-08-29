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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "darray.h"
#include "utils.h"

struct darray {
    uint32_t capacity;
    uint32_t size;
    uint32_t element_size;
    uint8_t* data;
};

struct darray *ngli_darray_create(uint32_t element_size)
{
    struct darray *v = malloc(sizeof(struct darray));
    v->element_size = element_size;
    v->size = 0;
    v->capacity = 0;
    v->data = NULL;
    return v;
}

void ngli_darray_freep(struct darray **v) {
    if (!*v)
        return;

    free((*v)->data);
    free(*v);
    *v = NULL;
}

uint32_t ngli_darray_size(const struct darray *v) {
    return v->size;
}

void ngli_darray_clear(struct darray *v) {
    v->size = 0;
}

void *ngli_darray_get(struct darray *v, uint32_t index) {
    ngli_assert(index >= 0 && index < v->size);
    return &v->data[index * v->element_size];
}

void *ngli_darray_begin(struct darray *v) {
    ngli_assert(v->size > 0);
    return v->data;
}

void *ngli_darray_end(struct darray *v) {
    ngli_assert(v->size > 0);
    return &v->data[(v->size+1) * v->element_size];
}

void ngli_darray_resize(struct darray *v, uint32_t size) {
    // allocation strategy
    if (size > v->capacity) {
        uint32_t factor = v->capacity == 0 ? size :  v->capacity;
        ngli_darray_reserve(v, factor << (size / factor));
    }
    v->size = size;
}

void ngli_darray_expand(struct darray *v, uint32_t count) {
    ngli_darray_resize(v, v->size+count);
}

void *ngli_darray_expand_to(struct darray *v, uint32_t index) {
    if (index >= v->size)
        ngli_darray_resize(v, index+1);
    return ngli_darray_get(v, index);
}

void *ngli_darray_add(struct darray *v) {
    return ngli_darray_expand_to(v, v->size);
}

void ngli_darray_reserve(struct darray *v, uint32_t capacity) {
    if (capacity > v->capacity) {
        uint32_t old_bytes = v->element_size * v->size;
        uint32_t new_bytes = v->element_size * capacity;
        v->data = v->data ? realloc(v->data, new_bytes) : malloc(new_bytes);
        memset(v->data + old_bytes, 0, new_bytes - old_bytes);
        v->capacity = capacity;
    }
}