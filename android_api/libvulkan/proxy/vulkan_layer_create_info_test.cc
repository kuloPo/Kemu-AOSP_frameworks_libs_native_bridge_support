// Copyright (C) 2026 The Android Open Source Project
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

#include "vulkan_layer_create_info.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace berberis {
namespace {

struct Holder {
  virtual ~Holder() = default;
};

// Use distinct, non-callable tokens to detect missing or repeated conversion.
template <typename Function>
Function Translate(Function function) {
  return function ? reinterpret_cast<Function>(reinterpret_cast<uintptr_t>(function) ^ 0x1000)
                  : nullptr;
}

// Store a discriminator that has no enumerator without forming an invalid enum value.
template <typename CreateInfo>
void SetFunction(CreateInfo& info, uint32_t function) {
  static_assert(sizeof(function) == sizeof(info.function));
  memcpy(&info.function, &function, sizeof(function));
}

class Conversion {
 public:
  const void* Chain(const void* source, std::unique_ptr<Holder>& holder, bool& oom) {
    if (!source) {
      return nullptr;
    }
    if (fail_tail && static_cast<const VkBaseInStructure*>(source)->sType ==
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
      oom = true;
      return nullptr;
    }
    auto chain = [this](const void* next, std::unique_ptr<Holder>& extensions, bool& failed) {
      return Chain(next, extensions, failed);
    };
    auto proc = [this](auto function, const char* name) {
      proc_names.emplace_back(name);
      return Translate(function);
    };
    auto callback = [this](auto function, const char* name) {
      callback_names.emplace_back(name);
      return Translate(function);
    };
    switch (static_cast<const VkBaseInStructure*>(source)->sType) {
      case VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO:
        return ConvertVulkanLayerCreateInfo<VkLayerInstanceCreateInfo, VkLayerInstanceLink>(
            static_cast<const VkLayerInstanceCreateInfo*>(source),
            holder,
            oom,
            chain,
            proc,
            callback);
      case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:
        return ConvertVulkanLayerCreateInfo<VkLayerDeviceCreateInfo, VkLayerDeviceLink>(
            static_cast<const VkLayerDeviceCreateInfo*>(source),
            holder,
            oom,
            chain,
            proc,
            callback);
      default:
        return source;
    }
  }

  bool fail_tail = false;
  std::vector<std::string> proc_names;
  std::vector<std::string> callback_names;
};

TEST(VulkanLayerCreateInfo, DeviceRoundTripPreservesConsumedLinkAndFeatureTail) {
  VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkLayerDeviceLink second{nullptr,
                           reinterpret_cast<PFN_vkGetInstanceProcAddr>(0x2000),
                           reinterpret_cast<PFN_vkGetDeviceProcAddr>(0x4000)};
  VkLayerDeviceLink first{&second,
                          reinterpret_cast<PFN_vkGetInstanceProcAddr>(0x6000),
                          reinterpret_cast<PFN_vkGetDeviceProcAddr>(0x8000)};
  VkLayerDeviceCreateInfo link{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                               &features,
                               VK_LAYER_FUNCTION_LINK,
                               {.pLayerInfo = &first}};
  VkLayerDeviceCreateInfo callback{
      VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
      &link,
      VK_LAYER_FUNCTION_DATA_CALLBACK,
      {.pfnSetDeviceLoaderData = reinterpret_cast<PFN_vkSetDeviceLoaderData>(0xa000)}};
  Conversion conversion;
  std::unique_ptr<Holder> guest_holder;
  bool oom = false;
  auto* guest_callback =
      static_cast<const VkLayerDeviceCreateInfo*>(conversion.Chain(&callback, guest_holder, oom));
  ASSERT_FALSE(oom);
  ASSERT_NE(guest_callback, nullptr);
  auto* guest_link = const_cast<VkLayerDeviceCreateInfo*>(
      static_cast<const VkLayerDeviceCreateInfo*>(guest_callback->pNext));
  ASSERT_NE(guest_link, nullptr);
  EXPECT_EQ(guest_callback->sType, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
  EXPECT_EQ(guest_callback->u.pfnSetDeviceLoaderData, Translate(callback.u.pfnSetDeviceLoaderData));
  EXPECT_EQ(guest_link->pNext, &features);
  ASSERT_NE(guest_link->u.pLayerInfo, &first);
  EXPECT_EQ(guest_link->u.pLayerInfo->pfnNextGetInstanceProcAddr,
            Translate(first.pfnNextGetInstanceProcAddr));
  EXPECT_EQ(guest_link->u.pLayerInfo->pfnNextGetDeviceProcAddr,
            Translate(first.pfnNextGetDeviceProcAddr));
  guest_link->u.pLayerInfo = guest_link->u.pLayerInfo->pNext;

  std::unique_ptr<Holder> host_holder;
  auto* host_callback = static_cast<const VkLayerDeviceCreateInfo*>(
      conversion.Chain(guest_callback, host_holder, oom));
  ASSERT_FALSE(oom);
  auto* host_link = static_cast<const VkLayerDeviceCreateInfo*>(host_callback->pNext);
  EXPECT_EQ(host_callback->u.pfnSetDeviceLoaderData, callback.u.pfnSetDeviceLoaderData);
  EXPECT_EQ(host_link->u.pLayerInfo->pfnNextGetInstanceProcAddr, second.pfnNextGetInstanceProcAddr);
  EXPECT_EQ(host_link->u.pLayerInfo->pfnNextGetDeviceProcAddr, second.pfnNextGetDeviceProcAddr);
  EXPECT_EQ(host_link->u.pLayerInfo->pNext, nullptr);
  EXPECT_EQ(host_link->pNext, &features);
  EXPECT_EQ(link.u.pLayerInfo, &first);
  EXPECT_EQ(first.pNext, &second);
  EXPECT_EQ(conversion.proc_names,
            (std::vector<std::string>{"vkGetInstanceProcAddr",
                                      "vkGetDeviceProcAddr",
                                      "vkGetInstanceProcAddr",
                                      "vkGetDeviceProcAddr",
                                      "vkGetInstanceProcAddr",
                                      "vkGetDeviceProcAddr"}));
}

TEST(VulkanLayerCreateInfo, InstanceConvertsBothKindsAndPreservesTail) {
  VkDebugUtilsMessengerCreateInfoEXT debug{
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  VkLayerInstanceLink next{nullptr, reinterpret_cast<PFN_vkGetInstanceProcAddr>(0x2000)};
  VkLayerInstanceCreateInfo link{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                 &debug,
                                 VK_LAYER_FUNCTION_LINK,
                                 {.pLayerInfo = &next}};
  VkLayerInstanceCreateInfo callback{
      VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
      &link,
      VK_LAYER_FUNCTION_DATA_CALLBACK,
      {.pfnSetInstanceLoaderData = reinterpret_cast<PFN_vkSetInstanceLoaderData>(0x4000)}};
  Conversion conversion;
  std::unique_ptr<Holder> holder;
  bool oom = false;
  auto* result =
      static_cast<const VkLayerInstanceCreateInfo*>(conversion.Chain(&callback, holder, oom));
  ASSERT_FALSE(oom);
  EXPECT_EQ(result->u.pfnSetInstanceLoaderData, Translate(callback.u.pfnSetInstanceLoaderData));
  auto* result_link = static_cast<const VkLayerInstanceCreateInfo*>(result->pNext);
  EXPECT_EQ(result_link->u.pLayerInfo->pfnNextGetInstanceProcAddr,
            Translate(next.pfnNextGetInstanceProcAddr));
  EXPECT_EQ(result_link->pNext, &debug);
  EXPECT_EQ(conversion.callback_names, (std::vector<std::string>{"vkSetInstanceLoaderData"}));
  EXPECT_EQ(conversion.proc_names, (std::vector<std::string>{"vkGetInstanceProcAddr"}));
}

TEST(VulkanLayerCreateInfo, EmptyLinkSurvivesBothDirections) {
  VkLayerDeviceCreateInfo source{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                 nullptr,
                                 VK_LAYER_FUNCTION_LINK,
                                 {.pLayerInfo = nullptr}};
  Conversion conversion;
  std::unique_ptr<Holder> holder;
  bool oom = false;
  auto* result =
      static_cast<const VkLayerDeviceCreateInfo*>(conversion.Chain(&source, holder, oom));
  ASSERT_FALSE(oom);
  EXPECT_NE(result, &source);
  EXPECT_EQ(result->u.pLayerInfo, nullptr);
  EXPECT_EQ(result->pNext, nullptr);
  EXPECT_TRUE(conversion.proc_names.empty());
  std::unique_ptr<Holder> reverse_holder;
  auto* reversed =
      static_cast<const VkLayerDeviceCreateInfo*>(conversion.Chain(result, reverse_holder, oom));
  ASSERT_FALSE(oom);
  ASSERT_NE(reversed, nullptr);
  EXPECT_EQ(reversed->u.pLayerInfo, nullptr);
  EXPECT_EQ(reversed->pNext, nullptr);
  EXPECT_TRUE(conversion.proc_names.empty());
}

TEST(VulkanLayerCreateInfo, NullFunctionsStayNull) {
  VkLayerDeviceLink next{nullptr, nullptr, nullptr};
  VkLayerDeviceCreateInfo link{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                               nullptr,
                               VK_LAYER_FUNCTION_LINK,
                               {.pLayerInfo = &next}};
  VkLayerDeviceCreateInfo callback{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                   &link,
                                   VK_LAYER_FUNCTION_DATA_CALLBACK,
                                   {.pfnSetDeviceLoaderData = nullptr}};
  Conversion conversion;
  std::unique_ptr<Holder> holder;
  bool oom = false;
  auto* result =
      static_cast<const VkLayerDeviceCreateInfo*>(conversion.Chain(&callback, holder, oom));
  ASSERT_FALSE(oom);
  EXPECT_EQ(result->u.pfnSetDeviceLoaderData, nullptr);
  auto* result_link = static_cast<const VkLayerDeviceCreateInfo*>(result->pNext);
  EXPECT_EQ(result_link->u.pLayerInfo->pfnNextGetInstanceProcAddr, nullptr);
  EXPECT_EQ(result_link->u.pLayerInfo->pfnNextGetDeviceProcAddr, nullptr);
}

TEST(VulkanLayerCreateInfo, TailAllocationFailurePropagates) {
  VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkLayerInstanceCreateInfo source{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                   &features,
                                   VK_LAYER_FUNCTION_LINK,
                                   {.pLayerInfo = nullptr}};
  Conversion conversion;
  conversion.fail_tail = true;
  std::unique_ptr<Holder> holder;
  bool oom = false;
  EXPECT_EQ(conversion.Chain(&source, holder, oom), nullptr);
  EXPECT_TRUE(oom);
  EXPECT_EQ(source.pNext, &features);
}

TEST(VulkanLayerCreateInfo, UnknownFunctionIsLeftOutOfChain) {
  VkLayerDeviceLink next{nullptr,
                         reinterpret_cast<PFN_vkGetInstanceProcAddr>(0x2000),
                         reinterpret_cast<PFN_vkGetDeviceProcAddr>(0x4000)};
  VkLayerDeviceCreateInfo link{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                               nullptr,
                               VK_LAYER_FUNCTION_LINK,
                               {.pLayerInfo = &next}};
  VkLayerDeviceCreateInfo unknown{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                  &link,
                                  VK_LAYER_FUNCTION_LINK,
                                  {.pLayerInfo = &next}};
  SetFunction(unknown, 2);
  Conversion conversion;
  std::unique_ptr<Holder> holder;
  bool oom = false;
  auto* result =
      static_cast<const VkLayerDeviceCreateInfo*>(conversion.Chain(&unknown, holder, oom));
  ASSERT_FALSE(oom);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(result, &link);
  EXPECT_EQ(result->function, VK_LAYER_FUNCTION_LINK);
  EXPECT_EQ(result->pNext, nullptr);
  EXPECT_EQ(result->u.pLayerInfo->pfnNextGetDeviceProcAddr,
            Translate(next.pfnNextGetDeviceProcAddr));
  EXPECT_EQ(conversion.proc_names,
            (std::vector<std::string>{"vkGetInstanceProcAddr", "vkGetDeviceProcAddr"}));

  VkLayerInstanceCreateInfo only{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                 nullptr,
                                 VK_LAYER_FUNCTION_LINK,
                                 {.pLayerInfo = nullptr}};
  SetFunction(only, 3);
  std::unique_ptr<Holder> empty_holder;
  EXPECT_EQ(conversion.Chain(&only, empty_holder, oom), nullptr);
  EXPECT_FALSE(oom);
  EXPECT_EQ(empty_holder.get(), nullptr);
}

}  // namespace
}  // namespace berberis
