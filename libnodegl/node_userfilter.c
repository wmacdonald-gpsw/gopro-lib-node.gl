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

#include "nodes.h"
#include "params.h"

struct userfilter {
    struct ngl_node *child;
    int enabled;
};

#define OFFSET(x) offsetof(struct userfilter, x)
static const struct node_param userfilter_params[] = {
    {"child",  PARAM_TYPE_NODE, OFFSET(child),
               .flags=PARAM_FLAG_CONSTRUCTOR,
               .desc=NGLI_DOCSTRING("filtered scene")},
    {"enabled", PARAM_TYPE_BOOL, OFFSET(enabled), {.i64=1},
               .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
               .desc=NGLI_DOCSTRING("set if the scene below should be rendered")},
    {NULL}
};

static int userfilter_visit(struct ngl_node *node, int is_active, double t)
{
    struct userfilter *s = node->priv_data;
    return ngli_node_visit(s->child, is_active && s->enabled, t);
}

static int userfilter_update(struct ngl_node *node, double t)
{
    struct userfilter *s = node->priv_data;
    return s->enabled ? ngli_node_update(s->child, t) : 0;
}

static void userfilter_draw(struct ngl_node *node)
{
    struct userfilter *s = node->priv_data;
    if (s->enabled)
        ngli_node_draw(s->child);
}

const struct node_class ngli_userfilter_class = {
    .id        = NGL_NODE_USERFILTER,
    .name      = "UserFilter",
    .visit     = userfilter_visit,
    .update    = userfilter_update,
    .draw      = userfilter_draw,
    .priv_size = sizeof(struct userfilter),
    .params    = userfilter_params,
    .file      = __FILE__,
};
