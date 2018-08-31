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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

#include "spirv.h"
#include "hmap.h"

static char *read_file(const char *filepath, uint32_t *size)
{
    char *buf = NULL;
    struct stat st;

    int fd = open(filepath, O_RDONLY);
    if (fd == -1)
        goto end;

    if (fstat(fd, &st) == -1)
        goto end;

    buf = malloc(st.st_size + 1);
    if (!buf)
        goto end;

    int n = read(fd, buf, st.st_size);
    buf[n] = 0;
    *size = st.st_size;

end:
    if (fd != -1)
        close(fd);
    return buf;
}

static const char *get_flag_name(uint32_t flag) {
    static char result[64];
    static const char *names[]= {
        "input",
        "output",
        "attribute",
        "block",
        "constant",
        "sampler",
        "texture",
        "uniforn",
        "storage",
        "dynamic",
        "indirection"
    };

    memset(result, 0, sizeof(result));
    char *cursor = result;
    static uint32_t nb_flags = sizeof(names) / sizeof(names[0]);
    for (uint8_t i = 0; i < nb_flags; i++) {
        if ((flag & (1 << i)) != 0) {
            uint32_t length = strlen(names[i]);
            memcpy(cursor, names[i], length);
            cursor[length] = ' ';
            cursor = cursor + length + 1;
        }
    }
    *cursor = 0;

    return result;
}

int main(int argc, char *argv[])
{
    const char *filepath = argv[1];

    uint32_t shader_size;
    char *shader_code = read_file(filepath, &shader_size);
    if (!shader_code)
        return -1;

    struct shaderdesc *s = ngli_shaderdesc_create((uint32_t*)shader_code, shader_size);
    if (!s)
        return -1;
    free(shader_code);

    // output reflection
    printf("\t%d attributes\n", s->attributes ? ngli_hmap_count(s->attributes) : 0);
    if (s->attributes) {
        const struct hmap_entry *attribute_entry = NULL;
        while ((attribute_entry = ngli_hmap_next(s->attributes, attribute_entry))) {
            const struct shaderattribute *attribute = attribute_entry->data;
            printf("\t\t%s\ttype: %s\tindex:%d\n",
                attribute_entry->key,
                get_flag_name(attribute->flag),
                attribute->index);
        }
    }

    printf("\t%d bindings\n", s->bindings ? ngli_hmap_count(s->bindings) : 0);
    if (s->bindings) {
        const struct hmap_entry *binding_entry = NULL;
        while ((binding_entry = ngli_hmap_next(s->bindings, binding_entry))) {
            const struct shaderbinding *binding = binding_entry->data;
            if ((binding->flag & NGLI_SHADER_BLOCK)) {
                const struct shaderblock *block = binding_entry->data;
                printf("\t\t%s\ttype: %s\tindex: %d\tsize: %d\n",
                    binding_entry->key,
                    get_flag_name(binding->flag),
                    binding->index,
                    block->size);

                if (block->variables) {
                    const struct hmap_entry *variable_entry = NULL;
                    while ((variable_entry = ngli_hmap_next(block->variables, variable_entry))) {
                        const struct shaderblockvariable *variable = variable_entry->data;
                        printf("\t\t\t%s\toffset: %d\n", variable_entry->key, variable->offset);
                    }
                }
            }
            else if ((binding->flag & NGLI_SHADER_TEXTURE)) {
                const struct shadertexture *texture = binding_entry->data;
                printf("\t\t%s\ttype: %s\tindex: %d\tformat: %d\n",
                    binding_entry->key,
                    get_flag_name(binding->flag),
                    binding->index,
                    texture->format);
            }
        }
    }
    ngli_shaderdesc_freep(&s);

    return 0;
}
