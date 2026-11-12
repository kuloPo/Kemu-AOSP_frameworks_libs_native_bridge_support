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

#ifndef NATIVE_BRIDGE_VULKAN_LAYER_CREATE_INFO_H_
#define NATIVE_BRIDGE_VULKAN_LAYER_CREATE_INFO_H_

#include <vulkan/vk_layer_interface.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace berberis {

template <typename CreateInfo, typename Link, typename Base>
struct VulkanLayerCreateInfoHolder : Base {
  CreateInfo data;
  std::unique_ptr<Link[]> links;
  std::unique_ptr<Base> extensions;
};

// The supported guest/host pairs have matching pointer widths and loader struct
// layouts. Copy the chain so a layer can advance pLayerInfo without modifying
// another architecture's representation, and convert the callable fields.
template <typename CreateInfo,
          typename Link,
          typename Base,
          typename ConvertChain,
          typename ConvertProc,
          typename ConvertCallback>
const void* ConvertVulkanLayerCreateInfo(const CreateInfo* source,
                                         std::unique_ptr<Base>& holder,
                                         bool& out_of_memory,
                                         ConvertChain convert_chain,
                                         ConvertProc convert_proc,
                                         ConvertCallback convert_callback) {
  // A payload of unknown layout cannot be converted, so the node is left out
  // of the chain like any other structure without a conversion. The value may
  // have no enumerator and is therefore not read through the enum type.
  uint32_t function;
  static_assert(sizeof(function) == sizeof(source->function));
  memcpy(&function, &source->function, sizeof(function));
  if (function != VK_LAYER_FUNCTION_LINK && function != VK_LAYER_FUNCTION_DATA_CALLBACK) {
    return convert_chain(source->pNext, holder, out_of_memory);
  }
  auto* converted = new (std::nothrow) VulkanLayerCreateInfoHolder<CreateInfo, Link, Base>;
  if (!converted) {
    out_of_memory = true;
    return nullptr;
  }
  converted->extensions = std::move(holder);
  holder.reset(converted);
  converted->data = *source;
  bool chain_out_of_memory = false;
  converted->data.pNext = convert_chain(source->pNext, converted->extensions, chain_out_of_memory);
  if (chain_out_of_memory) {
    out_of_memory = true;
    return nullptr;
  }

  switch (source->function) {
    case VK_LAYER_FUNCTION_LINK: {
      size_t count = 0;
      for (auto* link = source->u.pLayerInfo; link; link = link->pNext) {
        ++count;
      }
      if (!count) {
        converted->data.u.pLayerInfo = nullptr;
        break;
      }
      converted->links.reset(new (std::nothrow) Link[count]);
      if (!converted->links) {
        out_of_memory = true;
        return nullptr;
      }
      auto* source_link = source->u.pLayerInfo;
      for (size_t i = 0; i < count; ++i, source_link = source_link->pNext) {
        auto& link = converted->links[i];
        link = *source_link;
        link.pNext = i + 1 < count ? &converted->links[i + 1] : nullptr;
        link.pfnNextGetInstanceProcAddr =
            convert_proc(source_link->pfnNextGetInstanceProcAddr, "vkGetInstanceProcAddr");
        if constexpr (std::is_same_v<Link, VkLayerDeviceLink>) {
          link.pfnNextGetDeviceProcAddr =
              convert_proc(source_link->pfnNextGetDeviceProcAddr, "vkGetDeviceProcAddr");
        }
      }
      converted->data.u.pLayerInfo = converted->links.get();
      break;
    }
    case VK_LAYER_FUNCTION_DATA_CALLBACK:
      if constexpr (std::is_same_v<CreateInfo, VkLayerInstanceCreateInfo>) {
        converted->data.u.pfnSetInstanceLoaderData =
            convert_callback(source->u.pfnSetInstanceLoaderData, "vkSetInstanceLoaderData");
      } else {
        converted->data.u.pfnSetDeviceLoaderData =
            convert_callback(source->u.pfnSetDeviceLoaderData, "vkSetDeviceLoaderData");
      }
      break;
  }
  return &converted->data;
}

}  // namespace berberis

#endif  // NATIVE_BRIDGE_VULKAN_LAYER_CREATE_INFO_H_
