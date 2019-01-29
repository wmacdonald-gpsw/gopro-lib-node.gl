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

#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>

#include "image.h"
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "texture.h"

#define FEATURE_DEPTH       (1 << 0)
#define FEATURE_STENCIL     (1 << 1)

static const struct param_choices feature_choices = {
    .name = "framebuffer_features",
    .consts = {
        {"depth",   FEATURE_DEPTH,   .desc=NGLI_DOCSTRING("depth")},
        {"stencil", FEATURE_STENCIL, .desc=NGLI_DOCSTRING("stencil")},
        {NULL}
    }
};

#define DECLARE_FORMAT_PARAM(format, size, name, doc) \
    {name, format, .desc=NGLI_DOCSTRING(doc)},

static const struct param_choices format_choices = {
    .name = "format",
    .consts = {
        NGLI_FORMATS(DECLARE_FORMAT_PARAM)
        {NULL}
    }
};

static const struct param_choices minfilter_choices = {
    .name = "min_filter",
    .consts = {
        {"nearest",                GL_NEAREST,                .desc=NGLI_DOCSTRING("nearest filtering")},
        {"linear",                 GL_LINEAR,                 .desc=NGLI_DOCSTRING("linear filtering")},
        {"nearest_mipmap_nearest", GL_NEAREST_MIPMAP_NEAREST, .desc=NGLI_DOCSTRING("nearest filtering, nearest mipmap filtering")},
        {"linear_mipmap_nearest",  GL_LINEAR_MIPMAP_NEAREST,  .desc=NGLI_DOCSTRING("linear filtering, nearest mipmap filtering")},
        {"nearest_mipmap_linear",  GL_NEAREST_MIPMAP_LINEAR,  .desc=NGLI_DOCSTRING("nearest filtering, linear mipmap filtering")},
        {"linear_mipmap_linear",   GL_LINEAR_MIPMAP_LINEAR,   .desc=NGLI_DOCSTRING("linear filtering, linear mipmap filtering")},
        {NULL}
    }
};

static const struct param_choices magfilter_choices = {
    .name = "mag_filter",
    .consts = {
        {"nearest", GL_NEAREST, .desc=NGLI_DOCSTRING("nearest filtering")},
        {"linear",  GL_LINEAR,  .desc=NGLI_DOCSTRING("linear filtering")},
        {NULL}
    }
};

static const struct param_choices wrap_choices = {
    .name = "wrap",
    .consts = {
        {"clamp_to_edge",   GL_CLAMP_TO_EDGE,   .desc=NGLI_DOCSTRING("clamp to edge wrapping")},
        {"mirrored_repeat", GL_MIRRORED_REPEAT, .desc=NGLI_DOCSTRING("mirrored repeat wrapping")},
        {"repeat",          GL_REPEAT,          .desc=NGLI_DOCSTRING("repeat pattern wrapping")},
        {NULL}
    }
};

#define OFFSET(x) offsetof(struct async_priv, x)
static const struct node_param async_params[] = {
    {"child",      PARAM_TYPE_NODE, OFFSET(child),
                   .flags=PARAM_FLAG_CONSTRUCTOR,
                   .desc=NGLI_DOCSTRING("scene to be render asynchronously")},
    {"width",      PARAM_TYPE_INT, OFFSET(width),
                   .desc=NGLI_DOCSTRING("width of the target framebuffers"),
                   .flags=PARAM_FLAG_CONSTRUCTOR},
    {"height",     PARAM_TYPE_INT, OFFSET(height),
                   .desc=NGLI_DOCSTRING("height of the target framebuffers"),
                   .flags=PARAM_FLAG_CONSTRUCTOR},
    {"features",   PARAM_TYPE_FLAGS, OFFSET(features),
                   .choices=&feature_choices,
                   .desc=NGLI_DOCSTRING("framebuffer feature mask")},
    {"format",     PARAM_TYPE_SELECT, OFFSET(format), {.i64=NGLI_FORMAT_R8G8B8A8_UNORM},
                   .choices=&format_choices,
                   .desc=NGLI_DOCSTRING("format of the pixel data")},
    {"min_filter", PARAM_TYPE_SELECT, OFFSET(min_filter), {.i64=GL_NEAREST},
                   .choices=&minfilter_choices,
                   .desc=NGLI_DOCSTRING("texture minifying function")},
    {"mag_filter", PARAM_TYPE_SELECT, OFFSET(mag_filter), {.i64=GL_NEAREST},
                   .choices=&magfilter_choices,
                   .desc=NGLI_DOCSTRING("texture magnification function")},
    {"wrap_s",     PARAM_TYPE_SELECT, OFFSET(wrap_s), {.i64=GL_CLAMP_TO_EDGE},
                   .choices=&wrap_choices,
                   .desc=NGLI_DOCSTRING("wrap parameter for the texture on the s dimension (horizontal)")},
    {"wrap_t",     PARAM_TYPE_SELECT, OFFSET(wrap_t), {.i64=GL_CLAMP_TO_EDGE},
                   .choices=&wrap_choices,
                   .desc=NGLI_DOCSTRING("wrap parameter for the texture on the t dimension (vertical)")},
    {NULL}
};

#define WORKER_STATE_UNINITIALIZED 0
#define WORKER_STATE_RUNNING       1
#define WORKER_STATE_STOPPED       2

static struct async_output_priv *get_output(struct async_priv *s)
{
    return &s->outputs[s->back_output_index];
}

static void swap_output(struct async_priv *s)
{
    pthread_mutex_lock(&s->output_lock);
    while (s->front_output && s->front_output->locked) {
        pthread_cond_wait(&s->output_cond, &s->output_lock);
    }
    s->front_output = &s->outputs[s->back_output_index];
    s->back_output_index = (s->back_output_index + 1) % 2;
    pthread_cond_signal(&s->output_cond);
    pthread_mutex_unlock(&s->output_lock);
}

const struct image *ngli_node_async_acquire_image(struct ngl_node *node)
{
    struct async_priv *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;
    struct glcontext *gl = ctx->glcontext;

    pthread_mutex_lock(&s->output_lock);
    while (!s->front_output)
        pthread_cond_wait(&s->output_cond, &s->output_lock);
    s->front_output->locked++;
    if (s->front_output->sync) {
        ngli_glWaitSync(gl, s->front_output->sync, 0, GL_TIMEOUT_IGNORED);
    }
    pthread_mutex_unlock(&s->output_lock);

    return &s->front_output->image;
}

void ngli_node_async_release_image(struct ngl_node *node)
{
    struct async_priv *s = node->priv_data;
    pthread_mutex_lock(&s->output_lock);
    s->front_output->locked--;
    pthread_cond_signal(&s->output_cond);
    pthread_mutex_unlock(&s->output_lock);
}

static int wait_new_frame(struct async_priv *s, double *ts)
{
    int state;

    pthread_mutex_lock(&s->worker_lock);
    while (s->last_update_time == -1. && s->state == WORKER_STATE_RUNNING)
        pthread_cond_wait(&s->worker_cond, &s->worker_lock);
    state = s->state;
    *ts = s->last_update_time;
    s->last_update_time = -1.;
    pthread_mutex_unlock(&s->worker_lock);

    if (state != WORKER_STATE_RUNNING)
        return -1;

    return 0;
}

static void *worker_thread(void *arg)
{
    struct async_priv *s = arg;

    ngli_thread_set_name("ngl-thread");

    s->ngl_ctx = ngli_create(0);
    if (!s->ngl_ctx)
        goto fail;

    int ret = ngl_configure(s->ngl_ctx, &s->ngl_config);
    if (ret < 0)
        goto fail;

    ret = ngl_set_scene(s->ngl_ctx, s->child);
    if (ret < 0)
        goto fail;

    pthread_mutex_init(&s->output_lock, NULL);
    pthread_cond_init(&s->output_cond, NULL);

    pthread_mutex_lock(&s->worker_lock);
    s->state = WORKER_STATE_RUNNING;
    pthread_cond_signal(&s->worker_cond);
    pthread_mutex_unlock(&s->worker_lock);

    struct glcontext *gl = s->ngl_ctx->glcontext;

    for (int i = 0; i < NGLI_ARRAY_NB(s->outputs); i++) {
        struct async_output_priv *o = &s->outputs[i];

        struct texture_params params = NGLI_TEXTURE_PARAM_DEFAULTS;
        params.format = s->format;
        params.width = s->width;
        params.height = s->height;
        params.min_filter = s->min_filter;
        params.mag_filter = s->mag_filter;
        params.wrap_s = s->wrap_s;
        params.wrap_t = s->wrap_t;
        int ret = ngli_texture_init(&o->color, gl, &params);
        if (ret < 0)
            goto fail;

        const struct texture *attachments[2] = {&o->color};
        int nb_attachments = 1;

        if (s->features) {
            params.usage = NGLI_TEXTURE_USAGE_ATTACHMENT_ONLY;
            if (s->features & FEATURE_DEPTH)
                params.format = NGLI_FORMAT_D16_UNORM;
            if (s->features & FEATURE_STENCIL)
                params.format = NGLI_FORMAT_D24_UNORM_S8_UINT;

            ret = ngli_texture_init(&o->depth, gl, &params);
            if (ret < 0)
                goto fail;

            attachments[nb_attachments++] = &o->depth;
        }

        struct fbo_params fbo_params = {
            .width = s->width,
            .height = s->height,
            .nb_attachments = nb_attachments,
            .attachments = attachments,
        };

        ret = ngli_fbo_init(&o->fbo, gl, &fbo_params);
        if (ret < 0)
            goto fail;

        struct image *image = &o->image;
        ngli_image_init(image, NGLI_IMAGE_LAYOUT_DEFAULT, &o->color);
        image->coordinates_matrix[5] = -1.0f;
        image->coordinates_matrix[13] = 1.0f;
    }

    ngli_glViewport(gl, 0, 0, s->width, s->height);

    for (;;) {
        struct async_output_priv *o = get_output(s);
        struct fbo *fbo = &o->fbo;

        ngli_fbo_bind(fbo);
        ngli_glClear(gl, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        double ts = 0.;
        int ret = wait_new_frame(s, &ts);
        if (ret < 0)
            goto fail;

        ret = ngl_draw(s->ngl_ctx, ts);
        if (ret < 0) {
            goto fail;
        }

#if 0
        /* FIXME: sync strategy might be configurable */
        if (gl->features & NGLI_FEATURE_SYNC) {
            o->sync = ngli_glFenceSync(gl, GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            ngli_glFlush(gl);
        } else {
            ngli_glFinish(gl);
        }
#else
        ngli_glFinish(gl);
#endif
        ngli_fbo_invalidate_depth_buffers(fbo);

        swap_output(s);
    }

fail:
    pthread_mutex_lock(&s->worker_lock);
    s->state = WORKER_STATE_STOPPED;
    pthread_cond_signal(&s->worker_cond);
    pthread_mutex_unlock(&s->worker_lock);

    ngl_set_scene(s->ngl_ctx, NULL);

    for (int i = 0; i < NGLI_ARRAY_NB(s->outputs); i++) {
        struct async_output_priv *o = &s->outputs[i];
        ngli_fbo_reset(&o->fbo);
        ngli_texture_reset(&o->color);
        ngli_texture_reset(&o->depth);
    }
    pthread_mutex_destroy(&s->output_lock);
    pthread_cond_destroy(&s->output_cond);

    ngl_freep(&s->ngl_ctx);

    return NULL;
}

static int worker_init(struct async_priv *s)
{
    int state;
    pthread_mutex_lock(&s->worker_lock);
    while (s->state == WORKER_STATE_UNINITIALIZED)
        pthread_cond_wait(&s->worker_cond, &s->worker_lock);
    state = s->state;
    pthread_mutex_unlock(&s->worker_lock);

    if (state != WORKER_STATE_RUNNING)
        return -1;

    return 0;
}

static int async_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct async_priv *s = node->priv_data;

    if (s->width <= 0 || s->height <= 0) {
        LOG(ERROR, "invalid target dimensions %dx%d", s->width, s->height);
        return -1;
    }

    s->ngl_config.platform = ctx->config.platform;
    s->ngl_config.backend = ctx->config.backend;
    s->ngl_config.display = ngli_glcontext_get_display(ctx->glcontext);
    s->ngl_config.handle = ngli_glcontext_get_handle(ctx->glcontext);
    s->ngl_config.swap_interval = 0;
    s->ngl_config.offscreen = 1;
    s->ngl_config.width = 1;
    s->ngl_config.height = 1;
    s->ngl_config.samples = 0;
    s->last_update_time = -1.;

    if (pthread_mutex_init(&s->worker_lock, NULL) ||
        pthread_cond_init(&s->worker_cond, NULL) ||
        pthread_create(&s->worker_tid, NULL, worker_thread, s)) {
        pthread_cond_destroy(&s->worker_cond);
        pthread_mutex_destroy(&s->worker_lock);
        LOG(ERROR, "could not create worker thread");
        return -1;
    }

    if (worker_init(s) < 0) {
        LOG(ERROR, "could not initialize worker thread");
        return -1;
    }

    return 0;
}

static void async_uninit(struct ngl_node *node)
{
    struct async_priv *s = node->priv_data;

    pthread_mutex_lock(&s->worker_lock);
    s->state = WORKER_STATE_STOPPED;
    pthread_cond_signal(&s->worker_cond);
    pthread_mutex_unlock(&s->worker_lock);
    pthread_join(s->worker_tid, NULL);
}

static int async_visit(struct ngl_node *node, int is_active, double t)
{
    return 0;
}

static int async_update(struct ngl_node *node, double t)
{
    struct async_priv *s = node->priv_data;

    pthread_mutex_lock(&s->worker_lock);
    s->last_update_time = t;
    pthread_cond_signal(&s->worker_cond);
    pthread_mutex_unlock(&s->worker_lock);

    return 0;
}

static void async_draw(struct ngl_node *node)
{
}

const struct node_class ngli_async_class = {
    .id        = NGL_NODE_ASYNC,
    .name      = "Async",
    .init      = async_init,
    .visit     = async_visit,
    .update    = async_update,
    .draw      = async_draw,
    .uninit    = async_uninit,
    .priv_size = sizeof(struct async_priv),
    .params    = async_params,
    .file      = __FILE__,
};
