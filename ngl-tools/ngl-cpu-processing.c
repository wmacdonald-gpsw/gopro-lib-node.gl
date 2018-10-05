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

#include <nodegl.h>
#include <sxplayer.h>

#include "common.h"
#include "player.h"

static const char *luminance_shader = \
"#version 100"                                                          "\n" \
"precision mediump float;"                                              "\n" \
                                                                        "\n" \
"uniform float luminance;"                                              "\n" \
"uniform sampler2D tex0_sampler;"                                       "\n" \
"varying vec2 var_tex0_coord;"                                          "\n" \
"varying vec2 var_uvcoord;"                                             "\n" \
                                                                        "\n" \
"void main()"                                                           "\n" \
"{"                                                                     "\n" \
"    vec4 color = texture2D(tex0_sampler, var_tex0_coord);"             "\n" \
"    float y = 1.0 - var_uvcoord.y;"                                    "\n" \
"    if (y >= luminance - 0.001 && y <= luminance + 0.001)"             "\n" \
"        color.rgba = vec4(1.0, 1.0, 1.0, 1.0);"                        "\n" \
"    gl_FragColor = color;"                                             "\n" \
"}"                                                                     "\n";

static void read_callback(uint8_t *data, int width, int height, int linesize, void *user_data)
{
    float luminance = 0.0;
    int nb_pixels = linesize * height;
    for (int i = 0; i < nb_pixels; i++) {
        uint8_t *rgba = data + (i * 4);
        luminance += rgba[0] * 0.2126 + rgba[1] * 0.7152 + rgba[2] * 0.0722;
    }
    luminance = luminance / (nb_pixels * 255.0);

    struct ngl_node *luminance_uniform = user_data;
    ngl_node_param_set(luminance_uniform, "value", luminance);
}

static struct ngl_node *get_scene(const char *filename)
{
    /* XXX: demo only, allocations need to be checked */

    static const float corner[3] = {-1.0, -1.0, 0.0};
    static const float width[3]  = { 2.0,  0.0, 0.0};
    static const float height[3] = { 0.0,  2.0, 0.0};

    struct ngl_node *group = ngl_node_create(NGL_NODE_GROUP);

    /* First pass */
    struct ngl_node *luminance = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    ngl_node_param_set(luminance, "value", 0.0);

    struct ngl_node *quad = ngl_node_create(NGL_NODE_QUAD);
    ngl_node_param_set(quad, "corner", corner);
    ngl_node_param_set(quad, "width", width);
    ngl_node_param_set(quad, "height", height);

    struct ngl_node *media = ngl_node_create(NGL_NODE_MEDIA, filename);

    struct ngl_node *texture = ngl_node_create(NGL_NODE_TEXTURE2D);
    ngl_node_param_set(texture, "data_src", media);

    struct ngl_node *render0 = ngl_node_create(NGL_NODE_RENDER, quad);
    ngl_node_param_set(render0, "textures", "tex0", texture);

    struct ngl_node *camera = ngl_node_create(NGL_NODE_CAMERA, render0);
    ngl_node_param_set(camera, "read_callback", read_callback);
    ngl_node_param_set(camera, "read_data", luminance);
    ngl_node_param_set(camera, "pipe_width", 256);
    ngl_node_param_set(camera, "pipe_height", 256);

    struct ngl_node *color_texture = ngl_node_create(NGL_NODE_TEXTURE2D);
    ngl_node_param_set(color_texture, "width", 256);
    ngl_node_param_set(color_texture, "height", 256);

    struct ngl_node *rtt = ngl_node_create(NGL_NODE_RENDERTOTEXTURE, camera, color_texture);

    ngl_node_param_add(group, "children", 1, &rtt);

    /* Second pass */
    struct ngl_node *program = ngl_node_create(NGL_NODE_PROGRAM);
    ngl_node_param_set(program, "fragment", luminance_shader);

    struct ngl_node *render1 = ngl_node_create(NGL_NODE_RENDER, quad);
    ngl_node_param_set(render1, "program", program);
    ngl_node_param_set(render1, "textures", "tex0", texture);

    ngl_node_param_set(render1, "uniforms", "luminance", luminance);

    ngl_node_param_add(group, "children", 1, &render1);

    ngl_node_unrefp(&quad);
    ngl_node_unrefp(&media);
    ngl_node_unrefp(&texture);
    ngl_node_unrefp(&render0);
    ngl_node_unrefp(&camera);
    ngl_node_unrefp(&color_texture);
    ngl_node_unrefp(&rtt);

    ngl_node_unrefp(&program);
    ngl_node_unrefp(&luminance);
    ngl_node_unrefp(&render1);

    return group;
}

static int probe(const char *filename, struct sxplayer_info *info)
{
    struct sxplayer_ctx *ctx = sxplayer_create(filename);
    if (!ctx)
        return -1;

    int ret = sxplayer_get_info(ctx, info);
    if (ret < 0)
        return ret;

    sxplayer_free(&ctx);

    return 0;
}

static void update_time(struct player *player, int64_t seek_at)
{
    if (seek_at >= 0) {
        player->clock_off = gettime() - seek_at;
        player->frame_ts = seek_at;
        return;
    }

    if (!player->paused) {
        const int64_t now = gettime();
        if (player->clock_off < 0 || now - player->clock_off > player->duration)
            player->clock_off = now;

        player->frame_ts = now - player->clock_off;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <media>\n", argv[0]);
        return -1;
    }

    struct sxplayer_info info = {0};
    int ret = probe(argv[1], &info);
    if (ret < 0)
        return ret;

    struct ngl_node *scene = get_scene(argv[1]);
    if (!scene)
        return -1;

    struct player player = {0};
    ret = player_init(&player, "ngl-player", scene, info.width, info.height, info.duration);
    ngl_node_unrefp(&scene);
    if (ret < 0)
        goto end;

    do {
        update_time(&player, -1);
        ngl_draw(player.ngl, player.frame_ts / 1000000.0);
        glfwPollEvents();
    } while (glfwGetKey(player.window, GLFW_KEY_ESCAPE) != GLFW_PRESS &&
             glfwWindowShouldClose(player.window) == 0);

    player_main_loop();
end:
    player_uninit();
    return ret;
}
