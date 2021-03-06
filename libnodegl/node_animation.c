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

#include <float.h>
#include <stddef.h>
#include <string.h>
#include "animation.h"
#include "log.h"
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"

#define OFFSET(x) offsetof(struct animation_priv, x)
static const struct node_param animatedfloat_params[] = {
    {"keyframes", PARAM_TYPE_NODELIST, OFFSET(animkf), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED,
                  .node_types=(const int[]){NGL_NODE_ANIMKEYFRAMEFLOAT, -1},
                  .desc=NGLI_DOCSTRING("float key frames to interpolate from")},
    {NULL}
};

static const struct node_param animatedvec2_params[] = {
    {"keyframes", PARAM_TYPE_NODELIST, OFFSET(animkf), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED,
                  .node_types=(const int[]){NGL_NODE_ANIMKEYFRAMEVEC2, -1},
                  .desc=NGLI_DOCSTRING("vec2 key frames to interpolate from")},
    {NULL}
};

static const struct node_param animatedvec3_params[] = {
    {"keyframes", PARAM_TYPE_NODELIST, OFFSET(animkf), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED,
                  .node_types=(const int[]){NGL_NODE_ANIMKEYFRAMEVEC3, -1},
                  .desc=NGLI_DOCSTRING("vec3 key frames to interpolate from")},
    {NULL}
};

static const struct node_param animatedvec4_params[] = {
    {"keyframes", PARAM_TYPE_NODELIST, OFFSET(animkf), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED,
                  .node_types=(const int[]){NGL_NODE_ANIMKEYFRAMEVEC4, -1},
                  .desc=NGLI_DOCSTRING("vec4 key frames to interpolate from")},
    {NULL}
};

static const struct node_param animatedquat_params[] = {
    {"keyframes", PARAM_TYPE_NODELIST, OFFSET(animkf), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED,
                  .node_types=(const int[]){NGL_NODE_ANIMKEYFRAMEQUAT, -1},
                  .desc=NGLI_DOCSTRING("quaternion key frames to interpolate from")},
    {NULL}
};

static void mix_float(void *user_arg, void *dst,
                      const struct animkeyframe_priv *kf0,
                      const struct animkeyframe_priv *kf1,
                      double ratio)
{
    double *dstd = dst;
    dstd[0] = NGLI_MIX(kf0->scalar, kf1->scalar, ratio);
}

static void mix_quat(void *user_arg, void *dst,
                     const struct animkeyframe_priv *kf0,
                     const struct animkeyframe_priv *kf1,
                     double ratio)
{
    ngli_quat_slerp(dst, kf0->value, kf1->value, ratio);
}

static void mix_vector(void *user_arg, void *dst,
                       const struct animkeyframe_priv *kf0,
                       const struct animkeyframe_priv *kf1,
                       double ratio, int len)
{
    float *dstf = dst;
    for (int i = 0; i < len; i++)
        dstf[i] = NGLI_MIX(kf0->value[i], kf1->value[i], ratio);
}

#define DECLARE_VEC_MIX_FUNC(len)                               \
static void mix_vec##len(void *user_arg, void *dst,             \
                         const struct animkeyframe_priv *kf0,   \
                         const struct animkeyframe_priv *kf1,   \
                         double ratio)                          \
{                                                               \
    return mix_vector(user_arg, dst, kf0, kf1, ratio, len);     \
}

DECLARE_VEC_MIX_FUNC(2)
DECLARE_VEC_MIX_FUNC(3)
DECLARE_VEC_MIX_FUNC(4)

static void cpy_scalar(void *user_arg, void *dst,
                       const struct animkeyframe_priv *kf)
{
    memcpy(dst, &kf->scalar, sizeof(kf->scalar));
}

static void cpy_values(void *user_arg, void *dst,
                       const struct animkeyframe_priv *kf)
{
    memcpy(dst, kf->value, sizeof(kf->value));
}

static ngli_animation_mix_func_type get_mix_func(int node_class)
{
    switch (node_class) {
        case NGL_NODE_ANIMATEDFLOAT: return mix_float;
        case NGL_NODE_ANIMATEDVEC2:  return mix_vec2;
        case NGL_NODE_ANIMATEDVEC3:  return mix_vec3;
        case NGL_NODE_ANIMATEDVEC4:  return mix_vec4;
        case NGL_NODE_ANIMATEDQUAT:  return mix_quat;
    }
    return NULL;
}

static ngli_animation_cpy_func_type get_cpy_func(int node_class)
{
    switch (node_class) {
        case NGL_NODE_ANIMATEDFLOAT: return cpy_scalar;
        case NGL_NODE_ANIMATEDVEC2:
        case NGL_NODE_ANIMATEDVEC3:
        case NGL_NODE_ANIMATEDVEC4:
        case NGL_NODE_ANIMATEDQUAT:  return cpy_values;
    }
    return NULL;
}

int ngl_anim_evaluate(struct ngl_node *node, void *dst, double t)
{
    if (node->class->id != NGL_NODE_ANIMATEDFLOAT &&
        node->class->id != NGL_NODE_ANIMATEDVEC2 &&
        node->class->id != NGL_NODE_ANIMATEDVEC3 &&
        node->class->id != NGL_NODE_ANIMATEDVEC4)
        return -1;

    struct animation_priv *s = node->priv_data;
    if (!s->nb_animkf)
        return -1;

    if (!s->anim_eval.kfs) {
        int ret = ngli_animation_init(&s->anim_eval, NULL,
                                      s->animkf, s->nb_animkf,
                                      get_mix_func(node->class->id),
                                      get_cpy_func(node->class->id));
        if (ret < 0)
            return ret;
    }

    struct animkeyframe_priv *kf0 = s->animkf[0]->priv_data;
    if (!kf0->function) {
        for (int i = 0; i < s->nb_animkf; i++) {
            int ret = s->animkf[i]->class->init(s->animkf[i]);
            if (ret < 0)
                return ret;
        }
    }

    return ngli_animation_evaluate(&s->anim_eval, dst, t);
}

static int animation_init(struct ngl_node *node)
{
    struct animation_priv *s = node->priv_data;
    return ngli_animation_init(&s->anim, NULL,
                               s->animkf, s->nb_animkf,
                               get_mix_func(node->class->id),
                               get_cpy_func(node->class->id));
}

static int animatedfloat_update(struct ngl_node *node, double t)
{
    struct animation_priv *s = node->priv_data;
    return ngli_animation_evaluate(&s->anim, &s->scalar, t);
}

static int animatedvec_update(struct ngl_node *node, double t)
{
    struct animation_priv *s = node->priv_data;
    return ngli_animation_evaluate(&s->anim, s->values, t);
}

const struct node_class ngli_animatedfloat_class = {
    .id        = NGL_NODE_ANIMATEDFLOAT,
    .name      = "AnimatedFloat",
    .init      = animation_init,
    .update    = animatedfloat_update,
    .priv_size = sizeof(struct animation_priv),
    .params    = animatedfloat_params,
    .file      = __FILE__,
};

const struct node_class ngli_animatedvec2_class = {
    .id        = NGL_NODE_ANIMATEDVEC2,
    .name      = "AnimatedVec2",
    .init      = animation_init,
    .update    = animatedvec_update,
    .priv_size = sizeof(struct animation_priv),
    .params    = animatedvec2_params,
    .file      = __FILE__,
};

const struct node_class ngli_animatedvec3_class = {
    .id        = NGL_NODE_ANIMATEDVEC3,
    .name      = "AnimatedVec3",
    .init      = animation_init,
    .update    = animatedvec_update,
    .priv_size = sizeof(struct animation_priv),
    .params    = animatedvec3_params,
    .file      = __FILE__,
};

const struct node_class ngli_animatedvec4_class = {
    .id        = NGL_NODE_ANIMATEDVEC4,
    .name      = "AnimatedVec4",
    .init      = animation_init,
    .update    = animatedvec_update,
    .priv_size = sizeof(struct animation_priv),
    .params    = animatedvec4_params,
    .file      = __FILE__,
};

const struct node_class ngli_animatedquat_class = {
    .id        = NGL_NODE_ANIMATEDQUAT,
    .name      = "AnimatedQuat",
    .init      = animation_init,
    .update    = animatedvec_update,
    .priv_size = sizeof(struct animation_priv),
    .params    = animatedquat_params,
    .file      = __FILE__,
};
