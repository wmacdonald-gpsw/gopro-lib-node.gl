/*
 * Copyright 2019 GoPro Inc.
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
#include "log.h"
#include "nodegl.h"
#include "nodes.h"
#include "math_utils.h"
#include "transforms.h"

static void update_trf_matrix(struct ngl_node *node, const float *quat)
{
    struct rotatequat_priv *s = node->priv_data;
    struct transform_priv *trf = &s->trf;
    ngli_mat4_rotate_from_quat(trf->matrix, quat);
}

static int rotate_init(struct ngl_node *node)
{
    struct rotatequat_priv *s = node->priv_data;
    if (!s->anim)
        update_trf_matrix(node, s->quat);
    return 0;
}

static int update_quat(struct ngl_node *node)
{
    struct rotatequat_priv *s = node->priv_data;
    if (s->anim) {
        LOG(ERROR, "updating quat while the animation is set is undefined behaviour");
        return -1;
    }
    update_trf_matrix(node, s->quat);
    return 0;
}

static int rotate_update(struct ngl_node *node, double t)
{
    struct rotatequat_priv *s = node->priv_data;
    struct transform_priv *trf = &s->trf;
    struct ngl_node *child = trf->child;
    if (s->anim) {
        struct ngl_node *anim_node = s->anim;
        struct animation_priv *anim = anim_node->priv_data;
        int ret = ngli_node_update(anim_node, t);
        if (ret < 0)
            return ret;
        update_trf_matrix(node, anim->values);
    }
    return ngli_node_update(child, t);
}

#define OFFSET(x) offsetof(struct rotatequat_priv, x)
static const struct node_param rotatequat_params[] = {
    {"child",  PARAM_TYPE_NODE, OFFSET(trf.child),
               .flags=PARAM_FLAG_CONSTRUCTOR,
               .desc=NGLI_DOCSTRING("scene to rotate")},
    {"quat",  PARAM_TYPE_DBL,  OFFSET(quat),
               .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
               .update_func=update_quat,
               .desc=NGLI_DOCSTRING("quaternion")},
    {"anim",   PARAM_TYPE_NODE, OFFSET(anim),
               .node_types=(const int[]){NGL_NODE_ANIMATEDQUAT, -1},
               .desc=NGLI_DOCSTRING("quaternion animation")},
    {NULL}
};

NGLI_STATIC_ASSERT(trf_on_top_of_rotate, OFFSET(trf) == 0);

const struct node_class ngli_rotatequat_class = {
    .id        = NGL_NODE_ROTATEQUAT,
    .name      = "RotateQuat",
    .init      = rotate_init,
    .update    = rotate_update,
    .draw      = ngli_transform_draw,
    .priv_size = sizeof(struct rotatequat_priv),
    .params    = rotatequat_params,
    .file      = __FILE__,
};
