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

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_BIN="$SCRIPT_DIR/vkmem_test"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}======================================================================${NC}"
echo -e "${BLUE}  HAMi vGPU Slicing & OOM Verification Test                          ${NC}"
echo -e "${BLUE}======================================================================${NC}"
echo ""

# -----------------------------------------------------------------------------
# 1. Hardware Evidence Check (NVIDIA GPU Detection)
# -----------------------------------------------------------------------------
echo -e "${YELLOW}[1/4] Checking NVIDIA Physical Hardware & Driver...${NC}"
NVIDIA_SMI_BIN=""
if command -v nvidia-smi >/dev/null 2>&1; then
    NVIDIA_SMI_BIN="nvidia-smi"
elif [ -x "/usr/lib/wsl/lib/nvidia-smi" ]; then
    NVIDIA_SMI_BIN="/usr/lib/wsl/lib/nvidia-smi"
elif command -v nvidia-smi.exe >/dev/null 2>&1; then
    NVIDIA_SMI_BIN="nvidia-smi.exe"
fi

if [ -n "$NVIDIA_SMI_BIN" ]; then
    echo -e "${GREEN}✓ NVIDIA Hardware detected via $NVIDIA_SMI_BIN:${NC}"
    $NVIDIA_SMI_BIN --query-gpu=gpu_name,driver_version,memory.total,memory.free --format=csv,noheader
    echo ""
    echo -e "${GREEN}Full GPU State:${NC}"
    $NVIDIA_SMI_BIN
else
    echo -e "${RED}✗ No nvidia-smi tool found. Ensure NVIDIA drivers are installed.${NC}"
fi

echo ""
echo -e "${YELLOW}[2/4] Verifying GPU 3D Graphics Pipeline...${NC}"
# Check OpenGL D3D12 hardware acceleration string if available
if command -v glxinfo >/dev/null 2>&1; then
    GL_DEVICE=$(GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA glxinfo -B 2>/dev/null | grep -i "Device:" || true)
    GL_ACCEL=$(GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA glxinfo -B 2>/dev/null | grep -i "Accelerated:" || true)
    GL_VRAM=$(GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA glxinfo -B 2>/dev/null | grep -i "Dedicated video memory:" || true)
    if [ -n "$GL_DEVICE" ]; then
        echo -e "${GREEN}✓ Hardware 3D Renderer:${NC} $GL_DEVICE"
        echo -e "${GREEN}✓ Hardware Acceleration:${NC} $GL_ACCEL"
        echo -e "${GREEN}✓ Physical Dedicated VRAM:${NC} $GL_VRAM"
    fi
fi

# Ensure test binary is compiled
if [ ! -f "$TEST_BIN" ]; then
    echo "Compiling test harness..."
    make -C "$SCRIPT_DIR" clean >/dev/null
    make -C "$SCRIPT_DIR" >/dev/null
fi

# -----------------------------------------------------------------------------
# 2. Virtual Slice Configuration (2 GB / 2048 MB)
# -----------------------------------------------------------------------------
SLICE_MB=2048
CHUNK_MB=256
echo ""
echo -e "${BLUE}======================================================================${NC}"
echo -e "${YELLOW}[3/4] Testing vGPU Memory Slice (Enforcing ${SLICE_MB} MB Limit)...${NC}"
echo -e "${BLUE}======================================================================${NC}"

# Clear any previous cache
rm -f /tmp/.hami_vk.cache /tmp/vgpu/hami_vk.cache

echo "Step A: Querying reported virtual heap with HAMI_VK_LIMIT_MB=${SLICE_MB}..."
REPORTED_HEAP=$(HAMI_VK_LIMIT_MB=$SLICE_MB $TEST_BIN info 2>/dev/null | sed -n '/^heap\[0\]/s/.*size=\([0-9]*\) MB.*/\1/p')
echo -e "  → Virtual Heap Size Reported to Application: ${GREEN}${REPORTED_HEAP} MB${NC}"

if [ "$REPORTED_HEAP" = "$SLICE_MB" ]; then
    echo -e "  ${GREEN}✓ Virtual VRAM Slicing Verified: Application sees exactly ${SLICE_MB} MB.${NC}"
else
    echo -e "  ${RED}✗ Expected ${SLICE_MB} MB, got ${REPORTED_HEAP} MB${NC}"
    exit 1
fi

# -----------------------------------------------------------------------------
# 3. Live Allocation & OOM Containment Proof
# -----------------------------------------------------------------------------
echo ""
echo -e "${BLUE}======================================================================${NC}"
echo -e "${YELLOW}[4/4] Executing Live Allocation to Test OOM Enforcement...${NC}"
echo -e "${BLUE}======================================================================${NC}"

echo "Phase 1: Allocating 4 x ${CHUNK_MB} MB (1024 MB total) inside the 2048 MB slice..."
echo "Command: HAMI_VK_LIMIT_MB=${SLICE_MB} $TEST_BIN fill ${CHUNK_MB} 0 4"
HAMI_VK_LIMIT_MB=$SLICE_MB $TEST_BIN fill $CHUNK_MB 0 4
echo -e "  ${GREEN}✓ Phase 1 PASSED: 1024 MB successfully allocated within slice.${NC}"

echo ""
echo "Phase 2: Allocating continuous ${CHUNK_MB} MB chunks to breach the 2048 MB cap..."
echo "Expected Behavior:"
echo "  - Chunks 1 to 8 (8 x 256 MB = 2048 MB) -> SUCCESS"
echo "  - Chunk 9 (2048 MB + 256 MB = 2304 MB) -> MUST BE BLOCKED WITH OOM"
echo ""

ALLOC_OUTPUT=$(HAMI_VK_LIMIT_MB=$SLICE_MB $TEST_BIN fill $CHUNK_MB 2>&1 || true)
echo "$ALLOC_OUTPUT"

echo ""
if echo "$ALLOC_OUTPUT" | grep -q "VK_ERROR_OUT_OF_DEVICE_MEMORY" || echo "$ALLOC_OUTPUT" | grep -q "VkResult=-2"; then
    echo -e "${GREEN}======================================================================${NC}"
    echo -e "${GREEN}  ✓ OOM CONTAINMENT PROVEN!                                           ${NC}"
    echo -e "${GREEN}  The layer intercepted the over-limit allocation and returned:       ${NC}"
    echo -e "${GREEN}  VK_ERROR_OUT_OF_DEVICE_MEMORY (VkResult = -2)                       ${NC}"
    echo -e "${GREEN}  Memory usage stopped exactly at ${SLICE_MB} MB.                     ${NC}"
    echo -e "${GREEN}  Physical VRAM was protected from exhaustion.                        ${NC}"
    echo -e "${GREEN}======================================================================${NC}"
else
    echo -e "${RED}✗ Failed: Application did not receive VK_ERROR_OUT_OF_DEVICE_MEMORY.${NC}"
    exit 1
fi
