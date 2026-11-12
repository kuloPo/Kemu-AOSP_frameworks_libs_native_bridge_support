//
// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

package main

import (
	"berberis/cpp_types"
	"berberis/vulkan_xml"
	"strings"
	"testing"
)

func TestAreBaseTypesDeclared(t *testing.T) {
	foo_type := vulkan_xml.ExtendedStruct(cpp_types.StructType("Foo", []cpp_types.FieldInfo{
		cpp_types.Field("i", cpp_types.UInt32TType)}), false, "")
	bar_type := vulkan_xml.ExtendedStruct(cpp_types.StructType("Bar", []cpp_types.FieldInfo{
		cpp_types.Field("i", cpp_types.UInt32TType),
		cpp_types.Field("j", cpp_types.UInt64TType)}), false, "")
	declared_types := make(map[string]cpp_types.Type)
	if areBaseTypesDeclared(foo_type, declared_types) {
		t.Error("Directly referred type is not declared, but areBaseTypesDeclared reports it as declared")
	}
	if areBaseTypesDeclared(bar_type, declared_types) {
		t.Error("Directly referred type Bar is not declared, but areBaseTypesDeclared reports it as declared")
	}
	if areBaseTypesDeclared(cpp_types.ArrayType(foo_type, 1), declared_types) {
		t.Error("Directly referred array of type Foo is not declared, but areBaseTypesDeclared reports it as declared")
	}
	if areBaseTypesDeclared(cpp_types.ArrayType(bar_type, 1), declared_types) {
		t.Error("Directly referred array o type Bar is not declared, but areBaseTypesDeclared reports it as declared")
	}
	if areBaseTypesDeclared(cpp_types.PointerType(foo_type), declared_types) {
		t.Error("Pointer for non-opaque struct is not declared, but areBaseTypesDeclared reports it as declared")
	}
	if !areBaseTypesDeclared(cpp_types.PointerType(cpp_types.OpaqueType("struct Baz")), declared_types) {
		t.Error("Pointer for opaque struct must always be considered declared, but areBaseTypesDeclared reports it as not declared")
	}
	declared_types["struct Foo"] = foo_type
	declared_types["struct Bar"] = bar_type
	if !areBaseTypesDeclared(foo_type, declared_types) {
		t.Error("Directly referred defined types are declared, but areBaseTypesDeclared reports it as not declared")
	}
	if !areBaseTypesDeclared(bar_type, declared_types) {
		t.Error("Directly referred defined types are declared, but areBaseTypesDeclared reports it as not declared")
	}
	if !areBaseTypesDeclared(cpp_types.ArrayType(foo_type, 1), declared_types) {
		t.Error("Directly referred arrays of defined types are considred declared, but areBaseTypesDeclared reports them as not declared")
	}
	if !areBaseTypesDeclared(cpp_types.ArrayType(bar_type, 1), declared_types) {
		t.Error("Directly referred arrays of defined types are considred declared, but areBaseTypesDeclared reports them as not declared")
	}
	if !areBaseTypesDeclared(cpp_types.PointerType(bar_type), declared_types) {
		t.Error("Indirectly referred types are considered declared, but areBaseTypesDeclared reports them as not declared")
	}
}

func TestConversionErrorCleanupIgnoresInputsAndNonHandles(t *testing.T) {
	handle := cpp_types.AliasType("VkTestHandle", cpp_types.PointerType(cpp_types.OpaqueType("struct VkTestHandle_T")))
	count := vulkan_xml.ExtendedField("count", cpp_types.UInt32TType, nil, nil)
	command := cpp_types.FunctionType(cpp_types.VoidType, []cpp_types.FieldInfo{
		count,
		vulkan_xml.ExtendedField("pOutput", cpp_types.PointerType(handle), count, nil),
		vulkan_xml.ExtendedField("pInput", cpp_types.PointerType(cpp_types.ConstType(handle)), count, nil),
		vulkan_xml.ExtendedField("pBytes", cpp_types.PointerType(cpp_types.UInt8TType), count, nil),
		vulkan_xml.ExtendedField("pSingle", cpp_types.PointerType(handle), nil, nil),
	})
	for _, from_guest := range []bool{false, true} {
		cleanup := conversionErrorCleanup(command, from_guest, "")
		if !strings.Contains(cleanup, "std::fill_n(pOutput_host, count_host, VkTestHandle{});") {
			t.Errorf("output array was not cleared: %s", cleanup)
		}
		for _, untouched := range []string{"pInput", "pBytes", "pSingle"} {
			if strings.Contains(cleanup, untouched) {
				t.Errorf("cleanup touches %s: %s", untouched, cleanup)
			}
		}
	}
}

func TestConversionErrorCleanupUsesOriginalNestedCount(t *testing.T) {
	handle := cpp_types.AliasType("VkTestHandle", cpp_types.PointerType(cpp_types.OpaqueType("struct VkTestHandle_T")))
	count := vulkan_xml.ExtendedField("count", cpp_types.UInt32TType, nil, nil)
	info := vulkan_xml.ExtendedStruct(cpp_types.StructType("TestAllocateInfo", []cpp_types.FieldInfo{count}), false, "")
	input := vulkan_xml.ExtendedField("pInfo", cpp_types.PointerType(cpp_types.ConstType(info)), nil, nil)
	command := cpp_types.FunctionType(cpp_types.VoidType, []cpp_types.FieldInfo{
		input,
		vulkan_xml.ExtendedField("pOutput", cpp_types.PointerType(handle), input, count),
	})
	guest_to_host := conversionErrorCleanup(command, true, "")
	if strings.Contains(guest_to_host, "pInfo_host") || !strings.Contains(guest_to_host, "ToGuestAddr(pInfo_guest)") {
		t.Errorf("cleanup relies on a possibly failed conversion: %s", guest_to_host)
	}
	host_to_guest := conversionErrorCleanup(command, false, "")
	if strings.Contains(host_to_guest, "pInfo_guest") || !strings.Contains(host_to_guest, "pInfo_host->count") {
		t.Errorf("cleanup does not use original host info: %s", host_to_guest)
	}
}
