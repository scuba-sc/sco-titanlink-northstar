#!/usr/bin/env bash
# Checks that third_party/sco-sdk/{sc_ipc.h,sc_titanlink.h} are byte for byte what sco-core ships
# at the ref in third_party/sco-sdk/SCO_SDK_PIN. Exit 1 on any difference or download failure.
set -euo pipefail
cd "$(dirname "$0")/.."
pin=$(grep -v '^#' third_party/sco-sdk/SCO_SDK_PIN | grep -v '^[[:space:]]*$' | head -n 1 | tr -d '[:space:]')
if [ -z "$pin" ]; then echo "SCO_SDK_PIN has no pin" >&2; exit 1; fi
echo "sco-core SDK pin: $pin"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
for f in sc_ipc.h sc_titanlink.h; do
    if ! curl -fsSL --retry 3 "https://raw.githubusercontent.com/scuba-sc/sco-core/$pin/include/$f" -o "$tmp/$f"; then
        echo "FAIL: could not download include/$f at $pin" >&2
        status=1
        continue
    fi
    if cmp -s "$tmp/$f" "third_party/sco-sdk/$f"; then
        echo "ok: $f matches sco-core $pin"
    else
        echo "FAIL: third_party/sco-sdk/$f differs from sco-core $pin include/$f" >&2
        diff -u "$tmp/$f" "third_party/sco-sdk/$f" | head -n 40 >&2 || true
        status=1
    fi
done
exit $status
