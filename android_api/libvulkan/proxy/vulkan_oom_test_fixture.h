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

#define VK_ENABLE_BETA_EXTENSIONS 1
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

#define BERBERIS_VK_ERROR_OUT_OF_HOST_MEMORY VK_ERROR_OUT_OF_HOST_MEMORY
#define BERBERIS_VK_SUCCESS VK_SUCCESS
#define BERBERIS_VK_COMMAND_BUFFER_LEVEL_PRIMARY VK_COMMAND_BUFFER_LEVEL_PRIMARY

using HostCode = uintptr_t;
using GuestAddr = uintptr_t;
bool fail_conversion = false;
std::mutex g_primary_command_buffer_mutex;
std::map<VkCommandBuffer, bool> g_primary_command_buffer;

template <typename Pointer, typename... Rest>
Pointer Convert(Pointer source, Rest&&... rest) {
  if (source && fail_conversion) {
    // Return null as well as setting OOM: cleanup must use the original count.
    std::get<sizeof...(Rest) - 1>(std::forward_as_tuple(rest...)) = true;
    return nullptr;
  }
  return source;
}

template <typename T>
struct GuestType : T {};

template <typename T>
struct GuestType<T*> {
  struct HostHolder {};
  struct GuestHolder {};
  struct HostArrayHolder {};
  struct GuestArrayHolder {};
  template <typename Holder, typename... Rest>
  GuestType(T* source, Holder&, Rest&&... rest) : value(Convert(source, rest...)) {}
  operator T*() const { return value; }
  T* value;
};

template <typename Pointer, typename Holder, typename... Rest>
Pointer ToHostType(Pointer source, Holder&, Rest&&... rest) {
  return Convert(source, rest...);
}

template <typename T>
T* ToHostAddr(GuestAddr address) {
  return reinterpret_cast<T*>(address);
}

template <typename T>
GuestAddr ToGuestAddr(T* pointer) {
  return reinterpret_cast<GuestAddr>(pointer);
}

struct FunctionCast {
  HostCode address;
  template <typename F>
  operator F() const {
    return reinterpret_cast<F>(address);
  }
};
FunctionCast AsFuncPtr(HostCode address) {
  return {address};
}

template <typename F>
struct Traits;
template <typename R, typename... Args>
struct Traits<R (*)(Args...)> {
  using Tuple = std::tuple<Args...>;
  static inline std::function<R(Args...)> callee;
  static R Call(Args... args) { return callee(args...); }
};

struct ProcessState {
  void* arguments;
  VkResult result = VK_SUCCESS;
};
struct GuestArgumentBuffer : ProcessState {
  std::function<void()> invoke;
};

template <typename F>
auto GuestParamsValues(ProcessState* state) {
  return *static_cast<typename Traits<F>::Tuple*>(state->arguments);
}
template <typename F>
auto GuestReturnReference(ProcessState* state) {
  return std::tie(state->result);
}
template <typename F>
auto HostArgumentsValues(GuestArgumentBuffer* buffer) {
  return GuestParamsValues<F>(buffer);
}
template <typename F>
auto GuestArgumentsReferences(GuestArgumentBuffer* buffer) {
  return std::apply([](auto&... args) { return std::tie(args...); },
                    *static_cast<typename Traits<F>::Tuple*>(buffer->arguments));
}
template <typename F>
auto HostResultReference(GuestArgumentBuffer* buffer) {
  return std::tie(buffer->result);
}
void RunGuestCall(GuestAddr, GuestArgumentBuffer* buffer) {
  buffer->invoke();
}

template <typename Handle>
Handle Token(uintptr_t value) {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(value);
  } else {
    return static_cast<Handle>(value);
  }
}

template <typename F, size_t InfoIndex, size_t OutputIndex, int CountIndex>
void TestCommand(const char* name,
                 void (*trampoline)(HostCode, ProcessState*),
                 void (*runner)(GuestAddr, GuestArgumentBuffer*)) {
  using Tuple = typename Traits<F>::Tuple;
  using Info = std::remove_cv_t<std::remove_pointer_t<std::tuple_element_t<InfoIndex, Tuple>>>;
  using Handle = std::remove_pointer_t<std::tuple_element_t<OutputIndex, Tuple>>;
  GuestType<Info> infos[2]{};
  static_assert(sizeof(GuestType<Info>) == sizeof(Info));
  for (int direction = 0; direction < 2; ++direction) {
    for (int mode = 0; mode < 3; ++mode) {
      // OOM before the call, successful call, and partial failure in the callee.
      fail_conversion = mode == 0;
      std::array<Handle, 4> outputs;
      outputs.fill(Token<Handle>(0x1000));
      Tuple arguments{};
      std::get<InfoIndex>(arguments) = &infos[0];
      std::get<OutputIndex>(arguments) = outputs.data() + 1;
      if constexpr (CountIndex >= 0) {
        std::get<CountIndex>(arguments) = 2;
      } else if constexpr (std::is_same_v<Info, VkDescriptorSetAllocateInfo>) {
        infos[0].descriptorSetCount = 2;
      } else {
        infos[0].commandBufferCount = 2;
      }
      int calls = 0;
      Traits<F>::callee = [&](auto... args) {
        ++calls;
        auto received = std::make_tuple(args...);
        assert(std::get<InfoIndex>(received) != nullptr);
        auto* handles = std::get<OutputIndex>(received);
        handles[0] = Token<Handle>(0x2000);
        handles[1] = mode == 2 ? Handle{} : Token<Handle>(0x3000);
        return mode == 2 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
      };
      GuestArgumentBuffer buffer{{&arguments}, {}};
      buffer.invoke = [&] { buffer.result = std::apply(&Traits<F>::Call, arguments); };
      if (direction == 0) {
        trampoline(reinterpret_cast<HostCode>(&Traits<F>::Call), &buffer);
      } else {
        runner(reinterpret_cast<GuestAddr>(&Traits<F>::Call), &buffer);
      }
      assert(outputs.front() == Token<Handle>(0x1000));
      assert(outputs.back() == Token<Handle>(0x1000));
      assert(calls == (mode == 0 ? 0 : 1));
      assert(buffer.result == (mode == 1 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY));
      assert(outputs[1] == (mode == 0 ? Handle{} : Token<Handle>(0x2000)));
      assert(outputs[2] == (mode == 1 ? Token<Handle>(0x3000) : Handle{}));
      // Do not retain a closure referencing this iteration's local variables.
      Traits<F>::callee = {};
    }
  }
  std::printf("PASS %s: both directions, conversion OOM, success, callee failure\n", name);
}
