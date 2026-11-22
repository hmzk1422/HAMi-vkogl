#!/bin/sh
# Run inside the HAMi device-plugin container (or its init step) at startup.
# Installs the Vulkan layer where Allocate() looks for it: <HOOK_PATH>/vgpu/vulkan/
# Atomic (temp + rename) so a container never sees a half-copied library.
set -eu
HOOK_PATH="${HOOK_PATH:-/usr/local}"          # same value as hostHookPath in the plugin
SRC="${LAYER_SRC:-/k8s-vgpu/lib/vulkan}"
DST="$HOOK_PATH/vgpu/vulkan"
mkdir -p "$DST"
for f in libVkLayer_hami_vgpu.so hami_vk_layer.json; do
  cp "$SRC/$f" "$DST/.$f.tmp" && chmod 0644 "$DST/$f.tmp" 2>/dev/null || true
  chmod 0755 "$DST/.$f.tmp" && mv -f "$DST/.$f.tmp" "$DST/$f"
done
echo "HAMi Vulkan layer installed to $DST"
