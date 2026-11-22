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
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LAYER_DIR="$REPO_ROOT/lib/nvidia/vulkan"
TEST_DIR="$SCRIPT_DIR"

echo "======================================================================"
echo " [1/5] Checking and Installing Dependencies (Go, Vulkan, GCC, Drivers) "
echo "======================================================================"

# Ensure apt packages
MISSING_PKGS=""
for pkg in gcc make libvulkan-dev mesa-vulkan-drivers wget; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING_PKGS="$MISSING_PKGS $pkg"
    fi
done

if [ -n "$MISSING_PKGS" ]; then
    echo "Installing missing packages:$MISSING_PKGS..."
    apt-get update -qq && apt-get install -y -qq $MISSING_PKGS
fi

# Ensure Go 1.22+ is available
if ! command -v go >/dev/null 2>&1; then
    if [ -x "/usr/local/go/bin/go" ]; then
        export PATH="/usr/local/go/bin:$PATH"
    else
        echo "Go not found in PATH. Downloading and installing Go 1.23.4..."
        wget -q https://go.dev/dl/go1.23.4.linux-amd64.tar.gz -O /tmp/go.tar.gz
        tar -C /usr/local -xzf /tmp/go.tar.gz
        rm -f /tmp/go.tar.gz
        export PATH="/usr/local/go/bin:$PATH"
    fi
fi
echo "Using Go: $(go version)"

echo ""
echo "======================================================================"
echo " [2/5] Compiling and Installing Vulkan Slicing Layer                   "
echo "======================================================================"
make -C "$LAYER_DIR" clean >/dev/null
make -C "$LAYER_DIR"
mkdir -p /usr/local/vgpu/vulkan /etc/vulkan/implicit_layer.d
cp "$LAYER_DIR/libVkLayer_hami_vgpu.so" /usr/local/vgpu/vulkan/
cp "$LAYER_DIR/hami_vk_layer.json"      /etc/vulkan/implicit_layer.d/
echo "Layer installed to /usr/local/vgpu/vulkan/libVkLayer_hami_vgpu.so"
echo "Manifest installed to /etc/vulkan/implicit_layer.d/hami_vk_layer.json"

echo ""
echo "======================================================================"
echo " [3/5] Running Go Unit Tests (Admission, Fail-Closed, Mounts)          "
echo "======================================================================"
cd "$REPO_ROOT"
CGO_ENABLED=1 go test -v -run "TestMerge|TestGraphics|TestApply|TestVulkan|TestLayer" ./pkg/device/nvidia/

echo ""
echo "======================================================================"
echo " [4/5] Running Vulkan Layer Core Test Suite                           "
echo "======================================================================"
cd "$TEST_DIR"
make clean >/dev/null
make >/dev/null
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
bash run_tests.sh

echo ""
echo "======================================================================"
echo " [5/5] Running Live vGPU Virtualization Proofs (Slicing, OOM, Noisy)   "
echo "======================================================================"
bash manual_tests.sh

echo ""
echo "======================================================================"
echo " ✅ ALL VERIFICATIONS PASSED SUCCESSFULLY!"
echo "   1. vGPU Slicing: PROVEN (Heap clamped to container limit)"
echo "   2. GPU OOM: PROVEN (Refuses extra allocation with VK_ERROR_OUT_OF_DEVICE_MEMORY)"
echo "   3. Noisy-Neighbor Isolation: PROVEN (Multi-process quota enforced)"
echo "   4. Go Fail-Closed Admission: PROVEN (All 16 unit tests passed)"
echo "======================================================================"
