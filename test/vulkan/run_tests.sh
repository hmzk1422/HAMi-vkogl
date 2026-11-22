#!/usr/bin/env bash
# Reproduces the layer proof on any Linux box with: gcc, libvulkan-dev, mesa-vulkan-drivers (lavapipe).
#   sudo apt-get install -y gcc make libvulkan-dev mesa-vulkan-drivers
# Run as root (installs the layer exactly like the pod sees it: /usr/local/vgpu/vulkan + /etc/vulkan/implicit_layer.d).
# Copyright 2024 The HAMi Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -eu   # NOTE: no pipefail - `cmd | grep -q` would turn grep's early exit into SIGPIPE (141) for the writer
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
LAYER_DIR="$REPO_ROOT/lib/nvidia/vulkan"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
pass=0; fail=0
check() { if eval "$2"; then echo "PASS  $1"; pass=$((pass+1)); else echo "FAIL  $1"; fail=$((fail+1)); fi; }

make -C "$LAYER_DIR" clean >/dev/null && make -C "$LAYER_DIR" >/dev/null
gcc -O1 -Wall "$HERE/vkmem_test.c"   -o "$HERE/vkmem_test"   -lvulkan
gcc -O1 -Wall "$HERE/vkmem_stress.c" -o "$HERE/vkmem_stress" -lvulkan -pthread
mkdir -p /usr/local/vgpu/vulkan /etc/vulkan/implicit_layer.d
cp "$LAYER_DIR/libVkLayer_hami_vgpu.so" /usr/local/vgpu/vulkan/
cp "$LAYER_DIR/hami_vk_layer.json"      /etc/vulkan/implicit_layer.d/
rm -f /tmp/.hami_vk.cache /tmp/vgpu/hami_vk.cache
T="$HERE/vkmem_test"

check "pass-through when no limit configured (heap > 1 GB)"  '[ "$($T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" -gt 1024 ]'
check "heap clamped to 512 MB"                               '[ "$(HAMI_VK_LIMIT_MB=512 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" = 512 ]'
check "VK_EXT_memory_budget budget clamped to 512 MB"        'HAMI_VK_LIMIT_MB=512 $T info | grep -q "heap2\[0\] size=512 MB budget=512 MB"'
check "reserve headroom: 512-128 = 384 MB"                   '[ "$(HAMI_VK_LIMIT_MB=512 HAMI_VK_RESERVE_MB=128 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" = 384 ]'
check "alloc stops exactly at limit with OUT_OF_DEVICE_MEMORY" 'HAMI_VK_LIMIT_MB=512 $T fill 64 2>/dev/null | grep -q "allocated 8 x 64 MB = 512 MB, stopped with VkResult=-2"'
check "budget usage reflects allocations"                    'HAMI_VK_LIMIT_MB=512 $T fill 64 2>/dev/null | grep -q "budget=512 MB usage=512 MB"'
DEV_UUID="$($T info | sed -n 's/^device_uuid=//p' | head -n 1)"
check "UUID-keyed limit beats default"                       '[ "$(HAMI_VK_LIMITS=${DEV_UUID}=256 HAMI_VK_LIMIT_MB=2048 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" = 256 ]'
check "non-matching UUID falls back to default"              '[ "$(HAMI_VK_LIMITS=GPU-00000000-0000-0000-0000-000000000001=256 HAMI_VK_LIMIT_MB=128 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" = 128 ]'
check "HAMI_VK_DISABLE=1 disables the layer"                 '[ "$(HAMI_VK_DISABLE=1 HAMI_VK_LIMIT_MB=512 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p")" -gt 1024 ]'

# cross-process shared pool: P1 holds 256 of 512, P2 must only get the other 256
rm -f /tmp/.hami_vk.cache /tmp/vgpu/hami_vk.cache
( HAMI_VK_LIMIT_MB=512 $T fill 64 3 4 >/dev/null 2>&1 & )
sleep 1
check "two processes share ONE pool (P2 gets 256 of 512)"   'HAMI_VK_LIMIT_MB=512 $T fill 64 0 2>/dev/null | grep -q "allocated 4 x 64 MB = 256 MB"'
sleep 3

# crash safety: kill -9 a process holding everything; quota must come back
HAMI_VK_LIMIT_MB=512 $T fill 64 30 >/dev/null 2>&1 & P=$!; sleep 1; kill -9 $P; wait $P 2>/dev/null || true
check "kill -9'd process's quota is reclaimed"               'HAMI_VK_LIMIT_MB=512 $T fill 64 0 2>/dev/null | grep -q "allocated 8 x 64 MB = 512 MB"'

# concurrency: control must FAIL the assertion, enforced run must PASS it
rm -f /tmp/.hami_vk.cache
check "stress control (no layer) violates 96 MB cap [proves detector works]" '! HAMI_VK_DISABLE=1 "$HERE/vkmem_stress" 96 >/dev/null 2>&1'
check "stress with layer: 96 MB cap never exceeded"          'HAMI_VK_LIMIT_MB=96 "$HERE/vkmem_stress" 96 2>/dev/null | grep -q PASS'
check "accounting drained to 0 after stress"                 'HAMI_VK_LIMIT_MB=96 $T info | grep -q "usage=0 MB"'

echo; echo "passed=$pass failed=$fail"; [ "$fail" -eq 0 ]
