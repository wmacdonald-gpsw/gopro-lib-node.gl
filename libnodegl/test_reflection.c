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

int main(int argc, char *argv[])
{
    const char *filepath = argv[1];

    uint32_t shader_size;
    char *shader_code = read_file(filepath, &shader_size);
    if (!shader_code)
        return -1;

    struct shader_reflection *s = ngli_spirv_create_reflection((uint32_t*)shader_code, shader_size);
    if (!s)
        return -1;
    free(shader_code);

    // output reflection
    printf("%s\n", filepath);

    if (s->variables) {
        printf("\t%d variables\n", ngli_hmap_count(s->variables));
        const struct hmap_entry *variable_entry = NULL;
        while ((variable_entry = ngli_hmap_next(s->variables, variable_entry))) {
            const struct shader_variable_reflection *variable = variable_entry->data;
            printf("\t\t%s %u\n", (variable->flag & NGL_SHADER_VARIABLE_INPUT) != 0 ? "input" : "output", variable->offset);
            printf("\t\tname: %s\n", variable_entry->key);
        }
    }

    if (s->buffers) {
        printf("\t%d buffers\n", ngli_hmap_count(s->buffers));
        const struct hmap_entry *buffer_entry = NULL;
        while ((buffer_entry = ngli_hmap_next(s->buffers, buffer_entry))) {
            struct shader_buffer_reflection *buffer = buffer_entry->data;
            printf("\t\ttype: %s\n", (buffer->flag & NGL_SHADER_BUFFER_UNIFORM) != 0 ? "uniform" : "constant");
            printf("\t\tsize: %d\n", buffer->size);
            if (buffer->variables) {
                const struct hmap_entry *variable_entry = NULL;
                while ((variable_entry = ngli_hmap_next(buffer->variables, variable_entry))) {
                    struct shader_variable_reflection *variable = variable_entry->data;
                    printf("\t\t\toffset: %d\n", variable->offset);
                    printf("\t\t\tname: %s\n", variable_entry->key);
                }
            }
        }
    }
    ngli_spirv_destroy_reflection(&s);

    return 0;
}
