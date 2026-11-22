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
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	corev1 "k8s.io/api/core/v1"
)

func envMap(c *corev1.Container) map[string]string {
	m := map[string]string{}
	for _, e := range c.Env {
		m[e.Name] = e.Value
	}
	return m
}

func TestMergeDriverCapabilities(t *testing.T) {
	cases := map[string]string{
		"":                 "compute,compat32,graphics,utility,video,display",
		"compute":          "compute,compat32,graphics,utility,video,display", // user value no longer disables graphics
		"compute,utility":  "compute,utility,compat32,graphics,video,display",
		" video , compute": "video,compute,compat32,graphics,utility,display",
		"all":              "all",
		"compute,compute":  "compute,compat32,graphics,utility,video,display",
	}
	for in, want := range cases {
		if got := mergeDriverCapabilities(in); got != want {
			t.Errorf("merge(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestGraphicsRequested(t *testing.T) {
	type tc struct {
		cfg  bool
		ann  map[string]string
		want bool
	}
	for i, c := range []tc{
		{false, nil, false},
		{true, nil, true},
		{false, map[string]string{GraphicsModeAnnotation: "enabled"}, true},
		{false, map[string]string{GraphicsModeAnnotation: "TRUE"}, true},
		{true, map[string]string{GraphicsModeAnnotation: "disabled"}, false}, // pod opt-out beats node default
		{true, map[string]string{GraphicsModeAnnotation: "garbage"}, true},   // unparsable -> default
		{false, map[string]string{GraphicsModeAnnotation: "garbage"}, false},
	} {
		if got := GraphicsRequested(c.cfg, c.ann); got != c.want {
			t.Errorf("case %d: got %v want %v", i, got, c.want)
		}
	}
}

func TestApplyGraphicsEnv_Disabled(t *testing.T) {
	ctr := &corev1.Container{}
	if ApplyGraphicsEnv(ctr, nil, GraphicsConfig{}) || len(ctr.Env) != 0 {
		t.Fatalf("must be a no-op when graphics is off, got %v", ctr.Env)
	}
}

func TestApplyGraphicsEnv_Defaults(t *testing.T) {
	ctr := &corev1.Container{}
	if !ApplyGraphicsEnv(ctr, nil, GraphicsConfig{Enable: true}) {
		t.Fatal("expected applied")
	}
	m := envMap(ctr)
	if m["NVIDIA_DRIVER_CAPABILITIES"] != "compute,compat32,graphics,utility,video,display" {
		t.Errorf("caps = %q", m["NVIDIA_DRIVER_CAPABILITIES"])
	}
	for _, k := range []string{"VK_ICD_FILENAMES", "MESA_LOADER_DRIVER_OVERRIDE", "VK_LAYER_PATH", "__GLX_VENDOR_LIBRARY_NAME"} {
		if _, ok := m[k]; ok {
			t.Errorf("%s must not be set by default", k) // no ICD pin (can break the loader), no zink, no explicit-layer var
		}
	}
	if m["HAMI_GRAPHICS_MODE"] != "enabled" {
		t.Errorf("marker missing")
	}
}

func TestApplyGraphicsEnv_ZinkAndICD(t *testing.T) {
	ctr := &corev1.Container{}
	ann := map[string]string{GraphicsZinkAnnotation: "true", GraphicsICDAnnotation: "/etc/vulkan/icd.d/nvidia_icd.json"}
	ApplyGraphicsEnv(ctr, ann, GraphicsConfig{Enable: true, VulkanICDPath: "/ignored"})
	m := envMap(ctr)
	if m["VK_ICD_FILENAMES"] != "/etc/vulkan/icd.d/nvidia_icd.json" {
		t.Errorf("annotation must beat config: %q", m["VK_ICD_FILENAMES"])
	}
	for k, v := range map[string]string{"MESA_LOADER_DRIVER_OVERRIDE": "zink", "GALLIUM_DRIVER": "zink", "__GLX_VENDOR_LIBRARY_NAME": "mesa"} {
		if m[k] != v {
			t.Errorf("%s = %q want %q", k, m[k], v)
		}
	}
	if !strings.HasSuffix(m["__EGL_VENDOR_LIBRARY_FILENAMES"], "50_mesa.json") {
		t.Errorf("EGL vendor not pinned")
	}
}

func TestApplyGraphicsEnv_PreservesUserEnv(t *testing.T) {
	ctr := &corev1.Container{Env: []corev1.EnvVar{
		{Name: "NVIDIA_DRIVER_CAPABILITIES", Value: "compute"},
		{Name: "VK_ICD_FILENAMES", Value: "/mine.json"},
		{Name: "HAMI_GRAPHICS_MODE", ValueFrom: &corev1.EnvVarSource{}},
	}}
	ApplyGraphicsEnv(ctr, nil, GraphicsConfig{Enable: true, VulkanICDPath: "/cfg.json"})
	m := envMap(ctr)
	if m["NVIDIA_DRIVER_CAPABILITIES"] != "compute,compat32,graphics,utility,video,display" {
		t.Errorf("caps not merged: %q", m["NVIDIA_DRIVER_CAPABILITIES"])
	}
	if m["VK_ICD_FILENAMES"] != "/mine.json" {
		t.Errorf("user ICD overridden: %q", m["VK_ICD_FILENAMES"])
	}
	n := 0
	for _, e := range ctr.Env {
		if e.Name == "HAMI_GRAPHICS_MODE" {
			n++
			if e.ValueFrom == nil {
				t.Errorf("ValueFrom env must be left untouched")
			}
		}
	}
	if n != 1 {
		t.Errorf("duplicate HAMI_GRAPHICS_MODE entries: %d", n)
	}
}

func TestVulkanAllocationEnvs(t *testing.T) {
	// happy path: two GPUs, two independent limits
	got, err := VulkanAllocationEnvs([]string{"GPU-aaa", "GPU-bbb"}, []int64{4096, 2048})
	if err != nil || got["HAMI_VK_LIMITS"] != "GPU-aaa=4096,GPU-bbb=2048" {
		t.Fatalf("two-GPU happy path: got %v err %v", got, err)
	}
}

func TestVulkanAllocationEnvs_MismatchedLengths(t *testing.T) {
	// more UUIDs than limits → must fail (not silently skip the extra GPU)
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa", "GPU-bbb"}, []int64{4096}); err == nil {
		t.Fatal("mismatched lengths must be rejected (fail closed)")
	}
	// more limits than UUIDs → must also fail
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa"}, []int64{4096, 2048}); err == nil {
		t.Fatal("mismatched lengths must be rejected (fail closed)")
	}
}

func TestVulkanAllocationEnvs_EmptyUUID(t *testing.T) {
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa", "", "GPU-ccc"}, []int64{4096, 100, 2048}); err == nil {
		t.Fatal("empty UUID must be rejected (fail closed)")
	}
}

func TestVulkanAllocationEnvs_NegativeMemory(t *testing.T) {
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa"}, []int64{-1}); err == nil {
		t.Fatal("negative VRAM limit must be rejected")
	}
}

func TestVulkanAllocationEnvs_ZeroMemory(t *testing.T) {
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa"}, []int64{0}); err == nil {
		t.Fatal("zero VRAM limit must be rejected (fail closed)")
	}
}

func TestVulkanAllocationEnvs_NilInputs(t *testing.T) {
	if _, err := VulkanAllocationEnvs(nil, nil); err == nil {
		t.Fatal("nil inputs must be rejected (fail closed)")
	}
}

func TestVulkanAllocationEnvs_DuplicateUUID(t *testing.T) {
	if _, err := VulkanAllocationEnvs([]string{"GPU-aaa", "GPU-aaa"}, []int64{4096, 2048}); err == nil {
		t.Fatal("duplicate UUID must be rejected")
	}
}

func TestVulkanAllocationEnvs_MultipleGPUs(t *testing.T) {
	// verify each GPU gets its own independent limit in the output string
	got, err := VulkanAllocationEnvs([]string{"GPU-aaa", "GPU-bbb", "GPU-ccc"}, []int64{1024, 2048, 4096})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	limits := got["HAMI_VK_LIMITS"]
	for _, want := range []string{"GPU-aaa=1024", "GPU-bbb=2048", "GPU-ccc=4096"} {
		if !strings.Contains(limits, want) {
			t.Errorf("missing %q in %q", want, limits)
		}
	}
}

func TestVulkanLayerMounts_ReadOnly(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "vgpu", "vulkan")
	_ = os.MkdirAll(dir, 0o755)
	mounts := VulkanLayerMounts(root)
	for _, m := range mounts {
		if !m.ReadOnly {
			t.Errorf("mount %q → %q must be read-only", m.HostPath, m.ContainerPath)
		}
	}
}

func TestLayerInstalledAndMounts(t *testing.T) {
	root := t.TempDir()
	if err := CheckVulkanLayerInstalled(root); err == nil {
		t.Fatal("missing layer must fail closed")
	}
	dir := filepath.Join(root, "vgpu", "vulkan")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(dir, "libVkLayer_hami_vgpu.so"), []byte("x"), 0o644)
	if err := CheckVulkanLayerInstalled(root); err == nil {
		t.Fatal("library without manifest must still fail")
	}
	_ = os.WriteFile(filepath.Join(dir, "hami_vk_layer.json"), []byte("{}"), 0o644)
	if err := CheckVulkanLayerInstalled(root); err != nil {
		t.Fatalf("complete install must pass: %v", err)
	}
	want := []LayerMount{
		{HostPath: dir, ContainerPath: "/usr/local/vgpu/vulkan", ReadOnly: true},
		{HostPath: filepath.Join(dir, "hami_vk_layer.json"), ContainerPath: "/etc/vulkan/implicit_layer.d/hami_vk_layer.json", ReadOnly: true},
	}
	if got := VulkanLayerMounts(root); !reflect.DeepEqual(got, want) {
		t.Fatalf("mounts = %+v", got)
	}
}
