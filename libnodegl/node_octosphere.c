/*
 * Copyright 2017 GoPro Inc.
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

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "glincludes.h"
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "utils.h"

#define OFFSET(x) offsetof(struct geometry, x)
static const struct node_param octosphere_params[] = {
    {"subdivision", PARAM_TYPE_INT, OFFSET(subdivision), {.i64=0}},
    {"uvmapping_3d", PARAM_TYPE_INT, OFFSET(uvmapping_3d), {.i64=0}},
    {NULL}
};

static const GLfloat directions[4][3] = {
    {-1.0f, 0.0f, 0.0f},
    { 0.0f, 0.0f,-1.0f},
    { 1.0f, 0.0f, 0.0f},
    { 0.0f, 0.0f, 1.0f},
};

static int create_lower_strip(int steps, int vtop, int vbot, int t, GLushort *indices)
{
    for (int i = 1; i < steps; i++) {
        indices[t++] = vbot;
        indices[t++] = vtop - 1;
        indices[t++] = vtop;

        indices[t++] = vbot++;
        indices[t++] = vtop++;
        indices[t++] = vbot;
    }
    indices[t++] = vbot;
    indices[t++] = vtop - 1;
    indices[t++] = vtop;

    return t;
}

static int create_upper_strip(int steps, int vtop, int vbot, int t, GLushort *indices)
{
    indices[t++] = vbot;
    indices[t++] = vtop - 1;
    indices[t++] = ++vbot;
    for (int i = 1; i <= steps; i++) {
        indices[t++] = vtop - 1;
        indices[t++] = vtop;
        indices[t++] = vbot;

        indices[t++] = vbot;
        indices[t++] = vtop++;
        indices[t++] = ++vbot;
    }

    return t;
}

static int create_vertex_line(GLfloat *from, GLfloat *to, int steps, int v, GLfloat *vertices)
{
    for (int i = 1; i <= steps; i++)
        ngli_vec3_lerp(vertices + v++ * 3, from, to, i / (GLfloat)steps);

    return v;
}

static int init(GLfloat **verticesp, int *nb_verticesp,
                GLushort **indicesp, int *nb_indicesp,
                GLfloat **uvcoordsp, GLfloat **normalsp,
                int subdivision, int uvmapping_3d)
{
    int r = 1 << subdivision;
    *nb_verticesp = 4 * (r + 1) * (r + 1) - 3 * (2 * r - 1);
    *nb_indicesp = ((1 << (subdivision * 2 + 3))) * 3;

    *verticesp = calloc(*nb_verticesp * 3, sizeof(**verticesp));

    *indicesp = calloc(*nb_indicesp, sizeof(**indicesp));

    int uvcoords_comp = uvmapping_3d ? 3 : 2;
    *uvcoordsp = calloc(*nb_verticesp * uvcoords_comp, sizeof(**uvcoordsp));

    *normalsp = calloc(*nb_verticesp * 3, sizeof(**normalsp));

    if (!*verticesp || !*indicesp || !*uvcoordsp || !*normalsp)
        goto error;

    GLfloat *vertices = *verticesp;
    int nb_vertices = *nb_verticesp;

    GLushort *indices = *indicesp;
    GLfloat *uvcoords = *uvcoordsp;
    GLfloat *normals = *normalsp;

    int v = 0;
    int t = 0;
    int vbot = 0;

    static const GLfloat up[3]      = {0.0, 1.0, 0.0};
    static const GLfloat down[3]    = {0.0,-1.0, 0.0};
    static const GLfloat forward[3] = {0.0, 0.0, 1.0};

    for (int i = 0; i < 4; i++)
        memcpy(vertices + v++ * 3, down, sizeof(down));

    for (int i = 1; i <= r; i++) {
        GLfloat progress = i / (GLfloat)r;
        GLfloat from[3];
        GLfloat to[3];
        ngli_vec3_lerp(to, (GLfloat *)down, (GLfloat *)forward, progress);
        memcpy(vertices + v++ * 3, to, sizeof(to));
        for (int d = 0; d < 4; d++) {
            memcpy(from, to, sizeof(to));
            ngli_vec3_lerp((GLfloat *)to, (GLfloat *)down, (GLfloat *)directions[d], progress);
            t = create_lower_strip(i, v, vbot, t, indices);
            v = create_vertex_line(from, to, i, v, vertices);
            vbot += i > 1 ? (i - 1) : 1;
        }
        vbot = v - 1 - i * 4;
    }

    for (int i = r - 1; i >= 1; i--) {
        GLfloat progress = i / (GLfloat)r;
        GLfloat from[3];
        GLfloat to[3];
        ngli_vec3_lerp(to, (GLfloat *)up, (GLfloat *)forward, progress);
        memcpy(vertices + v++ * 3, to, sizeof(to));
        for (int d = 0; d < 4; d++) {
            memcpy(from, to, sizeof(to));
            ngli_vec3_lerp(to, (GLfloat *)up, (GLfloat *)directions[d], progress);
            t = create_upper_strip(i, v, vbot, t, indices);
            v = create_vertex_line(from, to, i, v, vertices);
            vbot += i + 1;
        }
        vbot = v - 1 - i * 4;
    }

    for (int i = 0; i < 4; i++) {
        indices[t++] = vbot;
        indices[t++] = v;
        indices[t++] = ++vbot;
        memcpy(vertices + v++ * 3, up, sizeof(up));
    };

    for (int i = 0; i < nb_vertices; i++) {
        ngli_vec3_norm(vertices + i * 3, vertices + i * 3);
        memcpy(normals + i * 3, vertices + i * 3, 3 * sizeof(*normals));
    }

    if (uvmapping_3d) {
        for (int i = 0; i < nb_vertices; i++) {
            GLfloat *vertice = &vertices[i * 3];
            GLfloat *uv = &uvcoords[i * 3];

            uv[0] = (vertice[0] + 1.0) / 2.0;
            uv[1] = (vertice[1] + 1.0) / 2.0;
            uv[2] = (vertice[2] + 1.0) / 2.0;
        }
    } else {
        GLfloat prev_x = 1.0f;
        for (int i = 0; i < nb_vertices; i++) {
            GLfloat *vertice = &vertices[i * 3];
            GLfloat *uv = &uvcoords[i * 2];

            if (vertice[0] == prev_x)
                uvcoords[(i - 1) * 2] = 1.0f;
            prev_x = vertice[0];

            uv[0] = atan2f(vertice[0], vertice[2]) / (-2.0f * M_PI);
            if (uv[0] < 0.0f)
                uv[0] += 1.0f;
            uv[1] = asinf(vertice[1]) / M_PI + 0.5f;
        }
        uvcoords[(nb_vertices - 4) * 2] = uvcoords[0] = 0.125f;
        uvcoords[(nb_vertices - 3) * 2] = uvcoords[2] = 0.375f;
        uvcoords[(nb_vertices - 2) * 2] = uvcoords[4] = 0.625f;
        uvcoords[(nb_vertices - 1) * 2] = uvcoords[6] = 0.875f;
    }

    return 0;
error:
    free(*verticesp);
    free(*indicesp);
    free(*uvcoordsp);
    free(*normalsp);

    return -1;
}

static int octosphere_init(struct ngl_node *node)
{
    struct geometry *s = node->priv_data;

    GLfloat *vertices = NULL;
    int nb_vertices = 0;

    GLushort *indices = NULL;
    int nb_indices = 0;

    GLfloat *uvcoords = NULL;
    GLfloat *normals = NULL;

    int ret = init(&vertices,
         &nb_vertices,
         &indices,
         &nb_indices,
         &uvcoords,
         &normals,
         s->subdivision,
         s->uvmapping_3d);
    if (ret < 0)
        return ret;

    s->vertices_buffer = ngli_geometry_generate_buffer(node->ctx,
                                                       NGL_NODE_BUFFERVEC3,
                                                       nb_vertices,
                                                       nb_vertices * 3 * sizeof(*vertices),
                                                       (void *)vertices);

    s->indices_buffer = ngli_geometry_generate_buffer(node->ctx,
                                                      NGL_NODE_BUFFERUSHORT,
                                                      nb_indices,
                                                      nb_indices * sizeof(*indices),
                                                      (void *)indices);

    int uvcoords_node = s->uvmapping_3d ? NGL_NODE_BUFFERVEC3: NGL_NODE_BUFFERVEC2;
    int uvcoords_comp = s->uvmapping_3d ? 3 : 2;
    s->uvcoords_buffer = ngli_geometry_generate_buffer(node->ctx,
                                                       uvcoords_node,
                                                       nb_vertices,
                                                       nb_vertices * uvcoords_comp * sizeof(*uvcoords),
                                                       (void *)uvcoords);

    s->normals_buffer = ngli_geometry_generate_buffer(node->ctx,
                                                      NGL_NODE_BUFFERVEC3,
                                                      nb_vertices,
                                                      nb_vertices * 3 * sizeof(*normals),
                                                      (void *)vertices);

    if (!s->vertices_buffer || !s->indices_buffer || !s->uvcoords_buffer || !s->normals_buffer)
        ret = -1;

    free(vertices);
    free(indices);
    free(uvcoords);
    free(normals);

    s->draw_mode = GL_TRIANGLES;

    return ret;
}

#define NODE_UNREFP(node) do {                    \
    if (node) {                                   \
        ngli_node_detach_ctx(node);               \
        ngl_node_unrefp(&node);                   \
    }                                             \
} while (0)

static void octosphere_uninit(struct ngl_node *node)
{
    struct geometry *s = node->priv_data;

    NODE_UNREFP(s->vertices_buffer);
    NODE_UNREFP(s->uvcoords_buffer);
    NODE_UNREFP(s->normals_buffer);
    NODE_UNREFP(s->indices_buffer);
}

const struct node_class ngli_octosphere_class = {
    .id        = NGL_NODE_OCTOSPHERE,
    .name      = "OctoSphere",
    .init      = octosphere_init,
    .uninit    = octosphere_uninit,
    .priv_size = sizeof(struct geometry),
    .params    = octosphere_params,
};
