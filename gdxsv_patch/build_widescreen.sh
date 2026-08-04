#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TOOLCHAIN_PREFIX=${REGDX_SH_ELF_PREFIX:-/opt/toolchains/dc/sh-elf/bin/sh-elf-}

mkdir -p "$SCRIPT_DIR/bin"
"${TOOLCHAIN_PREFIX}as" --little --isa=sh4 \
    "$SCRIPT_DIR/src/widescreen_transition_matte.s" \
    -o "$SCRIPT_DIR/bin/widescreen_transition_matte.o"
"${TOOLCHAIN_PREFIX}as" --little --isa=sh4 \
    "$SCRIPT_DIR/src/widescreen_result_black.s" \
    -o "$SCRIPT_DIR/bin/widescreen_result_black.o"
"${TOOLCHAIN_PREFIX}as" --little --isa=sh4 \
    "$SCRIPT_DIR/src/widescreen_hud.s" \
    -o "$SCRIPT_DIR/bin/widescreen_hud.o"
"${TOOLCHAIN_PREFIX}ld" -m shlelf \
    -T "$SCRIPT_DIR/src/widescreen.ld" \
    "$SCRIPT_DIR/bin/widescreen_transition_matte.o" \
    "$SCRIPT_DIR/bin/widescreen_result_black.o" \
    "$SCRIPT_DIR/bin/widescreen_hud.o" \
    -o "$SCRIPT_DIR/bin/widescreen.elf"
"${TOOLCHAIN_PREFIX}objcopy" -O binary \
    --only-section gdx.widescreen.transition \
    "$SCRIPT_DIR/bin/widescreen.elf" \
    "$SCRIPT_DIR/bin/widescreen_transition_matte.bin"
"${TOOLCHAIN_PREFIX}objcopy" -O binary \
    --only-section gdx.widescreen.result \
    "$SCRIPT_DIR/bin/widescreen.elf" \
    "$SCRIPT_DIR/bin/widescreen_result_black.bin"
"${TOOLCHAIN_PREFIX}objcopy" -O binary \
    --only-section gdx.widescreen.hud \
    "$SCRIPT_DIR/bin/widescreen.elf" \
    "$SCRIPT_DIR/bin/widescreen_hud.bin"
"${TOOLCHAIN_PREFIX}nm" -n \
    "$SCRIPT_DIR/bin/widescreen.elf" \
    > "$SCRIPT_DIR/bin/widescreen.nm"
/usr/bin/python3 "$SCRIPT_DIR/conv_widescreen.py" \
    "$SCRIPT_DIR/bin/widescreen_transition_matte.bin" \
    "$SCRIPT_DIR/bin/widescreen_result_black.bin" \
    "$SCRIPT_DIR/bin/widescreen_hud.bin" \
    "$SCRIPT_DIR/bin/widescreen.nm" \
    "$SCRIPT_DIR/../core/gdxsv/gdxsv_widescreen_patch.inc"
