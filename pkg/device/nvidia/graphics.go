/*
Copyright 2024 The HAMi Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package nvidia

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	corev1 "k8s.io/api/core/v1"
)

// Graphics (Vulkan / DXVK / VKD3D-Proton / OpenGL-via-Zink) support.
//
// Division of labour:
//   - webhook (ApplyGraphicsEnv): makes the container *capable* of graphics.
//   - device plugin Allocate (VulkanAllocationEnvs, VulkanLayerMounts): hands the container
//     the HAMi Vulkan layer and the per-GPU VRAM limits, and refuses to start it if the layer
//     is not installed on the node (fail closed).
//   - libVkLayer_hami_vgpu.so (layer/): clamps the reported heap and enforces the limit.

const (
	// GraphicsModeAnnotation enables/disables graphics for a pod, overriding the device config.
	GraphicsModeAnnotation = "hami.io/graphics-mode"
	// GraphicsICDAnnotation pins the Vulkan ICD manifest (optional).
	GraphicsICDAnnotation = "hami.io/vulkan-icd"
	// GraphicsZinkAnnotation routes OpenGL through Mesa Zink for this pod.
	GraphicsZinkAnnotation = "hami.io/enable-zink"

	envDriverCapabilities = "NVIDIA_DRIVER_CAPABILITIES"
	envVulkanLimits       = "HAMI_VK_LIMITS"

	vulkanLayerSubdir            = "vgpu/vulkan" // under hostHookPath on the node
	vulkanLayerLibName           = "libVkLayer_hami_vgpu.so"
	vulkanLayerManifestName      = "hami_vk_layer.json"
	vulkanLayerContainerDir      = "/usr/local/vgpu/vulkan"
	vulkanLayerContainerManifest = "/etc/vulkan/implicit_layer.d/hami_vk_layer.json"
)

// graphicsCapabilities is what a cloud-gaming container needs from the NVIDIA container toolkit:
// compute (CUDA interop), compat32 (32-bit Wine/Proton), graphics (Vulkan/GL), utility (nvidia-smi),
// video (NVENC/NVDEC for streaming) and display.
var graphicsCapabilities = []string{"compute", "compat32", "graphics", "utility", "video", "display"}

// GraphicsConfig is the subset of NvidiaConfig used by graphics admission.
type GraphicsConfig struct {
	Enable        bool
	VulkanICDPath string
	EnableZink    bool
}

func parseGraphicsBool(v string) (value bool, ok bool) {
	switch strings.ToLower(strings.TrimSpace(v)) {
	case "true", "enabled", "1", "yes", "on":
		return true, true
	case "false", "disabled", "0", "no", "off":
		return false, true
	}
	return false, false
}

func annotationBool(annotations map[string]string, key string, def bool) bool {
	if v, ok := annotations[key]; ok {
		if b, ok := parseGraphicsBool(v); ok {
			return b
		}
	}
	return def
}

// GraphicsRequested is true when the node-wide default or the pod annotation turns graphics on.
// An unparsable annotation value is ignored (the default applies).
func GraphicsRequested(cfgEnable bool, annotations map[string]string) bool {
	return annotationBool(annotations, GraphicsModeAnnotation, cfgEnable)
}

// mergeDriverCapabilities returns existing plus every capability graphics needs.
// A user-supplied "compute" no longer silently disables graphics, and "all" is left alone.
func mergeDriverCapabilities(existing string) string {
	seen := map[string]bool{}
	var out []string
	for _, c := range strings.Split(existing, ",") {
		c = strings.TrimSpace(c)
		if c == "" {
			continue
		}
		if c == "all" {
			return "all"
		}
		if !seen[c] {
			seen[c] = true
			out = append(out, c)
		}
	}
	for _, c := range graphicsCapabilities {
		if !seen[c] {
			seen[c] = true
			out = append(out, c)
		}
	}
	return strings.Join(out, ",")
}

// upsertEnv merges into an existing literal env var, or appends a new one.
// Env vars sourced from a Secret/ConfigMap/fieldRef (ValueFrom) are never touched.
func upsertEnv(ctr *corev1.Container, name string, merge func(old string) string) {
	for i := range ctr.Env {
		if ctr.Env[i].Name != name {
			continue
		}
		if ctr.Env[i].ValueFrom != nil {
			return
		}
		ctr.Env[i].Value = merge(ctr.Env[i].Value)
		return
	}
	ctr.Env = append(ctr.Env, corev1.EnvVar{Name: name, Value: merge("")})
}

// setEnvIfAbsent never overrides what the user already set.
func setEnvIfAbsent(ctr *corev1.Container, name, value string) {
	upsertEnv(ctr, name, func(old string) string {
		if old != "" {
			return old
		}
		return value
	})
}

// ApplyGraphicsEnv is called by the admission webhook for each GPU container.
// It returns true if graphics was applied.
func ApplyGraphicsEnv(ctr *corev1.Container, annotations map[string]string, cfg GraphicsConfig) bool {
	if !GraphicsRequested(cfg.Enable, annotations) {
		return false
	}

	upsertEnv(ctr, envDriverCapabilities, mergeDriverCapabilities)

	// Only pin the ICD when explicitly configured: a path that does not exist inside the image
	// makes the Vulkan loader find *no* driver at all, which is worse than not pinning.
	icd := cfg.VulkanICDPath
	if v := annotations[GraphicsICDAnnotation]; v != "" {
		icd = v
	}
	if icd != "" {
		setEnvIfAbsent(ctr, "VK_ICD_FILENAMES", icd)
	}

	if annotationBool(annotations, GraphicsZinkAnnotation, cfg.EnableZink) {
		// All four are needed: with NVIDIA's GLX/EGL present GLVND keeps choosing NVIDIA's own GL
		// (which HAMi cannot limit) unless the vendor is pinned to Mesa. Requires Mesa+Zink in the image.
		setEnvIfAbsent(ctr, "MESA_LOADER_DRIVER_OVERRIDE", "zink")
		setEnvIfAbsent(ctr, "GALLIUM_DRIVER", "zink")
		setEnvIfAbsent(ctr, "__GLX_VENDOR_LIBRARY_NAME", "mesa")
		setEnvIfAbsent(ctr, "__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json")
	}

	// Introspection marker only. The layer is activated by its manifest being present, not by this.
	setEnvIfAbsent(ctr, "HAMI_GRAPHICS_MODE", "enabled")
	return true
}

// ---------------------------------------------------------------------------------------------
// Device-plugin (Allocate) helpers
// ---------------------------------------------------------------------------------------------

// LayerMount is a plain mount description so this file does not depend on the kubelet API package.
type LayerMount struct {
	HostPath      string
	ContainerPath string
	ReadOnly      bool
}

func vulkanLayerHostDir(hostHookPath string) string {
	return filepath.Join(hostHookPath, vulkanLayerSubdir)
}

// CheckVulkanLayerInstalled is the fail-closed gate: if graphics was requested but the layer is not
// on this node, the allocation must fail instead of silently handing out an unlimited GPU.
func CheckVulkanLayerInstalled(hostHookPath string) error {
	dir := vulkanLayerHostDir(hostHookPath)
	for _, f := range []string{vulkanLayerLibName, vulkanLayerManifestName} {
		p := filepath.Join(dir, f)
		if st, err := os.Stat(p); err != nil || st.IsDir() {
			return fmt.Errorf("graphics requested but HAMi Vulkan layer file %q is missing on this node: refusing to allocate a GPU without VRAM enforcement", p)
		}
	}
	return nil
}

// VulkanLayerMounts: the whole directory (library) at a private path, plus the manifest as a single
// file inside the loader's standard implicit-layer directory. Mounting a single file adds our layer
// without shadowing layers that already live in /etc/vulkan/implicit_layer.d, and it works on every
// loader version (VK_ADD_IMPLICIT_LAYER_PATH only exists in loader >= 1.3.234).
func VulkanLayerMounts(hostHookPath string) []LayerMount {
	dir := vulkanLayerHostDir(hostHookPath)
	return []LayerMount{
		{HostPath: dir, ContainerPath: vulkanLayerContainerDir, ReadOnly: true},
		{HostPath: filepath.Join(dir, vulkanLayerManifestName), ContainerPath: vulkanLayerContainerManifest, ReadOnly: true},
	}
}

// VulkanAllocationEnvs builds the per-container env for the layer. Limits are keyed by GPU UUID
// (Vulkan's VkPhysicalDeviceIDProperties.deviceUUID equals the NVML UUID), because Vulkan device
// order does not match CUDA/NVML order. uuids[i] pairs with memMB[i] (the container's vGPU memory in MB).
//
// Fail-closed: every allocated GPU must have a valid, non-zero limit. Mismatches, empty UUIDs,
// duplicate UUIDs, and zero/negative limits are all hard errors — the allocate call will be
// refused rather than handing out an uncapped GPU.
func VulkanAllocationEnvs(uuids []string, memMB []int64) (map[string]string, error) {
	if len(uuids) == 0 || len(uuids) != len(memMB) {
		return nil, fmt.Errorf(
			"graphics: GPU UUID list (%d) and memory limit list (%d) must be non-empty and equal length",
			len(uuids), len(memMB),
		)
	}
	seen := make(map[string]bool, len(uuids))
	parts := make([]string, 0, len(uuids))
	for i, u := range uuids {
		u = strings.TrimSpace(u)
		if u == "" {
			return nil, fmt.Errorf("graphics: empty GPU UUID at index %d: refusing to allocate without per-GPU enforcement", i)
		}
		if seen[u] {
			return nil, fmt.Errorf("graphics: duplicate GPU UUID %q at index %d: refusing to allocate", u, i)
		}
		if memMB[i] <= 0 {
			return nil, fmt.Errorf("graphics: invalid VRAM limit for GPU %q: %d MB (must be > 0)", u, memMB[i])
		}
		seen[u] = true
		parts = append(parts, fmt.Sprintf("%s=%d", u, memMB[i]))
	}
	return map[string]string{envVulkanLimits: strings.Join(parts, ",")}, nil
}
