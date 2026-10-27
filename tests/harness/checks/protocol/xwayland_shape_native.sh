#!/usr/bin/env bash
# harness: xwayland=native
# Native-resolution X11 shapes use buffer pixels while their scene geometry
# remains logical. Exercise the same transitions on a scaled output.
export UMBRIEL_XWAYLAND_SHAPE_SCALE=2
source "$UMBRIEL_REPO/tests/harness/checks/protocol/xwayland_shape.sh"
