//go:build ignore
// +build ignore

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

// This file is intentionally excluded from the Go build.
// The Vulkan layer is a pure-C shared library compiled by lib/nvidia/vulkan/Makefile.
// It is loaded at runtime by the Vulkan loader, not linked into any Go binary.
// Including the .c source directly via Cgo causes duplicate symbol errors when the
// library is also compiled by make, and is not needed for any Go package.
//
// To rebuild the layer:
//   cd lib/nvidia/vulkan && make
// Or via the test harness:
//   bash test_graphics.sh

package vulkan