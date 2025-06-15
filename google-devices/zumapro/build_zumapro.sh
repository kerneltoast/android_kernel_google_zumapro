#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

exec tools/bazel run \
    --config=stamp \
    --config=zumapro \
    //private/devices/google/zumapro:dist "$@"
