#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Single-file portable check of real link.cpp; test-only in-process Win32 shims, no game.
set -euo pipefail
cd "$(dirname "$0")/.."
pin=""
while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    pin="$line"
    break
done < third_party/sco-sdk/SCO_SDK_PIN
if [[ ! "$pin" =~ ^[A-Za-z0-9._-]+$ ]]; then
    printf 'SCO_SDK_PIN has no valid pin
' >&2
    exit 1
fi
mkdir -p build/wire-golden
printf 'Portable Link check: in-process mapping shim, not Windows/game acceptance
'
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DTL_PORTABLE_TEST=1 -Itests/portable -Isrc -isystem third_party/sco-sdk \
    "-DSCO_SDK_PIN=\"$pin\"" tests/wire_golden.cpp src/link.cpp \
    -o build/wire-golden/wire_golden
build/wire-golden/wire_golden
