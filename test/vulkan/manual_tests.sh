#!/usr/bin/env bash
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

set -e
T="$(cd "$(dirname "$0")" && pwd)/vkmem_test"
S="$(cd "$(dirname "$0")" && pwd)/vkmem_stress"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}"

pass=0; fail=0
check() {
  if eval "$2"; then echo "✅  $1"; pass=$((pass+1))
  else                 echo "❌  $1"; fail=$((fail+1)); fi
}

echo "============================================================"
echo "  TEST A: Can we slice the vGPU? (heap is clamped)"
echo "============================================================"
echo "  → Raw GPU heap (no layer):"
HAMI_VK_DISABLE=1 $T info
echo ""
echo "  → Virtual GPU with 2 GB slice:"
HAMI_VK_LIMIT_MB=2048 $T info
echo ""
check "vGPU slice: heap reported as 2048 MB" \
  '[ "$( HAMI_VK_LIMIT_MB=2048 $T info | sed -n "/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p" )" = 2048 ]'

echo ""
echo "============================================================"
echo "  TEST B: Does it OOM when exceeding the slice?"
echo "============================================================"
echo "  → Allocate 4 × 256 MB (1 GB) inside a 2 GB slice  [should SUCCEED]:"
HAMI_VK_LIMIT_MB=2048 $T fill 256 0 4
echo ""
echo "  → Keep allocating 256 MB chunks until 2 GB cap  [should OOM on 9th chunk]:"
HAMI_VK_LIMIT_MB=2048 $T fill 256
echo ""
check "OOM fires at the cap (VkResult=-2 in output)" \
  'HAMI_VK_LIMIT_MB=2048 $T fill 256 2>/dev/null | grep -q "VkResult=-2"'

echo ""
echo "============================================================"
echo "  TEST C: Noisy-neighbor isolation (two processes, one pool)"
echo "============================================================"
echo "  → Process 1 grabs 512 MB, Process 2 tries to take the rest:"
rm -f /tmp/manual_shared.cache
( HAMI_VK_CACHE=/tmp/manual_shared.cache HAMI_VK_LIMIT_MB=2048 $T fill 256 3 2 >/dev/null 2>&1 & )
sleep 1
HAMI_VK_CACHE=/tmp/manual_shared.cache HAMI_VK_LIMIT_MB=2048 $T fill 256 0 8
echo ""
check "Noisy neighbor: combined total never exceeds 2048 MB" \
  'HAMI_VK_CACHE=/tmp/manual_shared.cache HAMI_VK_LIMIT_MB=2048 $T fill 256 0 8 2>/dev/null | grep -q "VkResult=-2"'

echo ""
echo "============================================================"
echo "  TEST D: CPU and RAM capping (Linux cgroups — informational)"
echo "============================================================"
echo "  HAMi controls CPU and RAM at the Kubernetes level via pod resource limits,"
echo "  not through the Vulkan layer. The layer is GPU-VRAM only."
echo ""
echo "  To verify CPU/RAM isolation in a real pod, the limits are set in the pod YAML:"
echo "    resources:"
echo "      limits:"
echo "        cpu: \"2\""
echo "        memory: \"4Gi\""
echo "        nvidia.com/gpumem: 2048  # <-- this is the vGPU slice"
echo ""
echo "  On this dev machine, you can simulate the effect with systemd-run:"
echo ""
echo "  systemd-run --scope -p CPUQuota=50% -p MemoryLimit=512M <your_command>"
echo ""

echo "============================================================"
echo "  RESULTS"
echo "============================================================"
echo "passed=$pass  failed=$fail"
[ "$fail" -eq 0 ]
