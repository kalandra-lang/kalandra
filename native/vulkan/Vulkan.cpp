#include "Vulkan.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <vulkan/vulkan.h>

namespace kalandra::runtime::vulkan {
namespace {
Error error(VkResult result) {
  if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
    return Error::Allocation;
  return Error::Device;
}
struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  std::atomic<std::uint64_t> errors{0};
  ~Instance() {
    if (messenger) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(handle, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy)
        destroy(handle, messenger, nullptr);
    }
    if (handle)
      vkDestroyInstance(handle, nullptr);
  }
};
VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT *message,
                                          void *context) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    static_cast<Instance *>(context)->errors.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
  }
  return VK_FALSE;
}
std::expected<std::unique_ptr<Instance>, Error> instance(bool debug) {
  auto result = std::make_unique<Instance>();
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (debug) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS ||
        std::none_of(layers.begin(), layers.end(),
                     [&](const auto &p) { return std::strcmp(p.layerName, layer) == 0; }))
      return std::unexpected(Error::Unavailable);
  }
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "Kalandra Vulkan bring-up";
  application.apiVersion = VK_API_VERSION_1_1;
  VkDebugUtilsMessengerCreateInfoEXT callback{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  callback.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  callback.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  callback.pfnUserCallback = validation;
  callback.pUserData = result.get();
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &application;
  if (debug) {
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = &layer;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &extension;
    info.pNext = &callback;
  }
  auto status = vkCreateInstance(&info, nullptr, &result->handle);
  if (status != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  if (debug) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(result->handle, "vkCreateDebugUtilsMessengerEXT"));
    if (!create || create(result->handle, &callback, nullptr, &result->messenger) != VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
  }
  return result;
}
std::expected<std::vector<VkPhysicalDevice>, Error> physicals(VkInstance handle) {
  std::uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(handle, &count, nullptr) != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(handle, &count, devices.data()) != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  devices.resize(count);
  // Never silently select software rendering as a GPU execution fallback.
  std::erase_if(devices, [](auto device) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(device, &p);
    return p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
           p.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  });
  return devices;
}
DeviceInfo description(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(device, &p);
  return {p.deviceName,
          p.vendorID,
          p.deviceID,
          p.apiVersion,
          p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
          p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU};
}
struct Pipeline {
  VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline handle = VK_NULL_HANDLE;
};
} // namespace
struct State {
  std::unique_ptr<Instance> instance;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool commands = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  Pipeline addition, matrix;
  mutable std::mutex queueMutex, budgetMutex;
  std::uint64_t bytes = 0, maxBytes = 0;
  bool failed = false;
  ~State() {
    if (!device)
      return;
    // No command resources may be freed while the device still owns them.
    vkDeviceWaitIdle(device);
    for (auto *p : {&addition, &matrix}) {
      if (p->handle)
        vkDestroyPipeline(device, p->handle, nullptr);
      if (p->layout)
        vkDestroyPipelineLayout(device, p->layout, nullptr);
      if (p->descriptors)
        vkDestroyDescriptorSetLayout(device, p->descriptors, nullptr);
    }
    if (descriptors)
      vkDestroyDescriptorPool(device, descriptors, nullptr);
    if (commands)
      vkDestroyCommandPool(device, commands, nullptr);
    vkDestroyDevice(device, nullptr);
  }
};
struct Storage {
  std::shared_ptr<State> state;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize allocation = 0;
  std::uint64_t bytes = 0;
  bool coherent = false;
  ~Storage() {
    if (mapped)
      vkUnmapMemory(state->device, memory);
    if (buffer)
      vkDestroyBuffer(state->device, buffer, nullptr);
    if (memory)
      vkFreeMemory(state->device, memory, nullptr);
    std::lock_guard lock(state->budgetMutex);
    state->bytes -= allocation;
  }
};
namespace {
std::expected<std::shared_ptr<Storage>, Error> allocate(const std::shared_ptr<State> &state,
                                                        std::uint64_t bytes) {
  auto s = std::make_shared<Storage>();
  s->state = state;
  s->bytes = bytes;
  if (!bytes)
    return s;
  if (bytes > state->properties.limits.maxStorageBufferRange)
    return std::unexpected(Error::Unsupported);
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  auto result = vkCreateBuffer(state->device, &info, nullptr, &s->buffer);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  VkMemoryRequirements required;
  vkGetBufferMemoryRequirements(state->device, s->buffer, &required);
  std::uint32_t memoryType = UINT32_MAX;
  for (std::uint32_t i = 0; i < state->memory.memoryTypeCount; ++i) {
    auto flags = state->memory.memoryTypes[i].propertyFlags;
    if ((required.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      memoryType = i;
      if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        break;
    }
  }
  if (memoryType == UINT32_MAX)
    return std::unexpected(Error::Unsupported);
  {
    std::lock_guard lock(state->budgetMutex);
    if (required.size > state->maxBytes - state->bytes)
      return std::unexpected(Error::Budget);
    state->bytes += required.size;
    s->allocation = required.size;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = required.size;
  allocation.memoryTypeIndex = memoryType;
  result = vkAllocateMemory(state->device, &allocation, nullptr, &s->memory);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  result = vkBindBufferMemory(state->device, s->buffer, s->memory, 0);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  result = vkMapMemory(state->device, s->memory, 0, VK_WHOLE_SIZE, 0, &s->mapped);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  s->coherent =
      state->memory.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return s;
}
std::expected<void, Error> visibility(const Storage &s, bool flush) {
  if (!s.bytes || s.coherent)
    return {};
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = s.memory;
  range.size = VK_WHOLE_SIZE;
  auto result = flush ? vkFlushMappedMemoryRanges(s.state->device, 1, &range)
                      : vkInvalidateMappedMemoryRanges(s.state->device, 1, &range);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  return {};
}
std::expected<void, Error> pipeline(State &state, Pipeline &p, const char *name,
                                    std::uint32_t bindings, std::uint32_t pushBytes,
                                    bool addition) {
  std::ifstream input(std::string(KALANDRA_VULKAN_SHADER_DIR) + "/" + name + ".spv",
                      std::ios::binary | std::ios::ate);
  if (!input)
    return std::unexpected(Error::Unavailable);
  auto length = input.tellg();
  if (length < 20 || length > 16 * 1024 * 1024 || length % 4)
    return std::unexpected(Error::Invalid);
  std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(code.data()), length);
  if (!input || code.front() != 0x07230203)
    return std::unexpected(Error::Invalid);
  std::vector<VkDescriptorSetLayoutBinding> layouts(bindings);
  for (std::uint32_t i = 0; i < bindings; ++i)
    layouts[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptors.bindingCount = bindings;
  descriptors.pBindings = layouts.data();
  auto status = vkCreateDescriptorSetLayout(state.device, &descriptors, nullptr, &p.descriptors);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &p.descriptors;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  status = vkCreatePipelineLayout(state.device, &layout, nullptr, &p.layout);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader.codeSize = code.size() * 4;
  shader.pCode = code.data();
  VkShaderModule module = VK_NULL_HANDLE;
  status = vkCreateShaderModule(state.device, &shader, nullptr, &module);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  std::uint32_t specialization = addition ? VK_TRUE : 128;
  VkSpecializationMapEntry entry{0, 0, sizeof(specialization)};
  VkSpecializationInfo constants{1, &entry, sizeof(specialization), &specialization};
  VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  compute.layout = p.layout;
  compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  compute.stage.module = module;
  compute.stage.pName = "main";
  compute.stage.pSpecializationInfo = &constants;
  status = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &compute, nullptr, &p.handle);
  vkDestroyShaderModule(state.device, module, nullptr);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  return {};
}
struct Dispatch {
  State &state;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkDescriptorSet descriptors = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  ~Dispatch() {
    if (fence)
      vkDestroyFence(state.device, fence, nullptr);
    if (command)
      vkFreeCommandBuffers(state.device, state.commands, 1, &command);
    if (descriptors)
      vkFreeDescriptorSets(state.device, state.descriptors, 1, &descriptors);
  }
};
std::expected<void, Error> dispatch(State &state, Pipeline &p,
                                    const std::array<std::shared_ptr<Storage>, 3> &buffers,
                                    std::span<const std::uint32_t> push, std::uint32_t x,
                                    std::uint32_t y) {
  if (state.failed)
    return std::unexpected(Error::Device);
  if (x > state.properties.limits.maxComputeWorkGroupCount[0] ||
      y > state.properties.limits.maxComputeWorkGroupCount[1])
    return std::unexpected(Error::Unsupported);
  Dispatch run{state};
  VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set.descriptorPool = state.descriptors;
  set.descriptorSetCount = 1;
  set.pSetLayouts = &p.descriptors;
  auto status = vkAllocateDescriptorSets(state.device, &set, &run.descriptors);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  std::array<VkDescriptorBufferInfo, 3> infos{};
  std::array<VkWriteDescriptorSet, 3> writes{};
  for (std::uint32_t i = 0; i < 3; ++i) {
    infos[i] = {buffers[i]->buffer, 0, buffers[i]->bytes};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = run.descriptors;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(state.device, writes.size(), writes.data(), 0, nullptr);
  VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocation.commandPool = state.commands;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  status = vkAllocateCommandBuffers(state.device, &allocation, &run.command);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  status = vkBeginCommandBuffer(run.command, &begin);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(run.command,
                       VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  vkCmdBindPipeline(run.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.handle);
  vkCmdBindDescriptorSets(run.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                          &run.descriptors, 0, nullptr);
  vkCmdPushConstants(run.command, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push.size_bytes(),
                     push.data());
  vkCmdDispatch(run.command, x, y, 1);
  VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(run.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
  status = vkEndCommandBuffer(run.command);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  status = vkCreateFence(state.device, &fence, nullptr, &run.fence);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &run.command;
  status = vkQueueSubmit(state.queue, 1, &submit, run.fence);
  if (status == VK_SUCCESS)
    status = vkWaitForFences(state.device, 1, &run.fence, VK_TRUE, UINT64_MAX);
  if (status != VK_SUCCESS) {
    // A failed wait is not proof that resources can be reclaimed. Settle the
    // device before releasing local command/fence/descriptor/buffer owners.
    vkDeviceWaitIdle(state.device);
    state.failed = true;
    return std::unexpected(error(status));
  }
  return {};
}
} // namespace
std::uint64_t Buffer::bytes() const noexcept { return storage_ ? storage_->bytes : 0; }
Runtime::~Runtime() = default;
std::expected<std::vector<DeviceInfo>, Error> Runtime::enumerate(bool debug) {
  try {
    auto context = instance(debug);
    if (!context)
      return std::unexpected(context.error());
    auto devices = physicals((*context)->handle);
    if (!devices)
      return std::unexpected(devices.error());
    std::vector<DeviceInfo> result;
    for (auto device : *devices)
      result.push_back(description(device));
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::unique_ptr<Runtime>, Error> Runtime::create(Options options) {
  try {
    auto state = std::make_shared<State>();
    auto context = instance(options.validation);
    if (!context)
      return std::unexpected(context.error());
    state->instance = std::move(*context);
    auto devices = physicals(state->instance->handle);
    if (!devices)
      return std::unexpected(devices.error());
    if (options.deviceIndex >= devices->size())
      return std::unexpected(Error::Unavailable);
    state->physical = (*devices)[options.deviceIndex];
    state->maxBytes = options.maxBytes;
    vkGetPhysicalDeviceProperties(state->physical, &state->properties);
    vkGetPhysicalDeviceMemoryProperties(state->physical, &state->memory);
    if (state->properties.apiVersion < VK_API_VERSION_1_1 ||
        state->properties.limits.maxPushConstantsSize < 116 ||
        state->properties.limits.maxComputeWorkGroupInvocations < 256 ||
        state->properties.limits.maxComputeWorkGroupSize[0] < 256 ||
        state->properties.limits.maxComputeSharedMemorySize < 64 * 20 * 8 * 2)
      return std::unexpected(Error::Unsupported);
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, queues.data());
    std::uint32_t family = UINT32_MAX;
    for (std::uint32_t i = 0; i < count; ++i)
      if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        family = i;
        break;
      }
    if (family == UINT32_MAX)
      return std::unexpected(Error::Unsupported);
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    auto status = vkCreateDevice(state->physical, &device, nullptr, &state->device);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    vkGetDeviceQueue(state->device, family, 0, &state->queue);
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands.queueFamilyIndex = family;
    commands.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    status = vkCreateCommandPool(state->device, &commands, nullptr, &state->commands);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptors.maxSets = 1;
    descriptors.poolSizeCount = 1;
    descriptors.pPoolSizes = &size;
    status = vkCreateDescriptorPool(state->device, &descriptors, nullptr, &state->descriptors);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    auto built = pipeline(*state, state->addition, "add", 4, 116, true);
    if (!built)
      return std::unexpected(built.error());
    built = pipeline(*state, state->matrix, "mul_mm", 3, 64, false);
    if (!built)
      return std::unexpected(built.error());
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
DeviceInfo Runtime::device() const { return description(state_->physical); }
std::uint64_t Runtime::retainedBytes() const {
  std::lock_guard lock(state_->budgetMutex);
  return state_->bytes;
}
std::uint64_t Runtime::validationErrors() const { return state_->instance->errors.load(); }
std::expected<Buffer, Error> Runtime::upload(std::span<const float> data) {
  try {
    std::lock_guard lock(state_->queueMutex);
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto buffer = allocate(state_, data.size_bytes());
    if (!buffer)
      return std::unexpected(buffer.error());
    if (!data.empty())
      std::memcpy((*buffer)->mapped, data.data(), data.size_bytes());
    auto visible = visibility(**buffer, true);
    if (!visible)
      return std::unexpected(visible.error());
    return Buffer(std::move(*buffer));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::vector<float>, Error> Runtime::download(const Buffer &buffer) {
  try {
    std::lock_guard lock(state_->queueMutex);
    if (!buffer.storage_ || buffer.storage_->state != state_)
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto visible = visibility(*buffer.storage_, false);
    if (!visible)
      return std::unexpected(visible.error());
    std::vector<float> result(buffer.bytes() / sizeof(float));
    if (!result.empty())
      std::memcpy(result.data(), buffer.storage_->mapped, buffer.bytes());
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Buffer, Error> Runtime::add(const Buffer &a, const Buffer &b) {
  try {
    std::lock_guard lock(state_->queueMutex);
    if (!a.storage_ || !b.storage_ || a.storage_->state != state_ || b.storage_->state != state_ ||
        a.bytes() != b.bytes())
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    const auto count = a.bytes() / sizeof(float);
    if (count > UINT32_MAX ||
        (count + 511) / 512 > state_->properties.limits.maxComputeWorkGroupCount[1])
      return std::unexpected(Error::Unsupported);
    auto output = allocate(state_, a.bytes());
    if (!output)
      return std::unexpected(output.error());
    if (count) {
      std::array<std::uint32_t, 29> push{};
      push[0] = count;
      for (std::size_t base : {1u, 9u, 17u}) {
        push[base] = count;
        push[base + 1] = push[base + 2] = push[base + 3] = 1;
        push[base + 4] = 1;
        push[base + 5] = push[base + 6] = push[base + 7] = count;
      }
      auto result = dispatch(*state_, state_->addition, {a.storage_, b.storage_, *output}, push, 1,
                             static_cast<std::uint32_t>((count + 511) / 512));
      if (!result)
        return std::unexpected(result.error());
    }
    return Buffer(std::move(*output));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Buffer, Error> Runtime::matmulRightTransposed(const Buffer &a, const Buffer &b,
                                                            std::uint32_t rows,
                                                            std::uint32_t columns,
                                                            std::uint32_t k) {
  try {
    std::lock_guard lock(state_->queueMutex);
    if (!a.storage_ || !b.storage_ || a.storage_->state != state_ || b.storage_->state != state_ ||
        std::uint64_t(rows) * k != a.bytes() / sizeof(float) ||
        std::uint64_t(columns) * k != b.bytes() / sizeof(float))
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto cells = std::uint64_t(rows) * columns;
    // Upstream shader address arithmetic is uint32; diagnose before dispatch.
    if (cells > UINT32_MAX || a.bytes() / 4 > UINT32_MAX || b.bytes() / 4 > UINT32_MAX ||
        rows > UINT32_MAX - 63 || columns > UINT32_MAX - 63)
      return std::unexpected(Error::Unsupported);
    auto output = allocate(state_, cells * sizeof(float));
    if (!output)
      return std::unexpected(output.error());
    if (cells && !k) {
      std::memset((*output)->mapped, 0, cells * sizeof(float));
      auto visible = visibility(**output, true);
      if (!visible)
        return std::unexpected(visible.error());
    } else if (cells) {
      std::array<std::uint32_t, 16> push{columns,
                                         rows,
                                         k,
                                         k,
                                         k,
                                         columns,
                                         columns * k,
                                         rows * k,
                                         static_cast<std::uint32_t>(cells),
                                         0,
                                         1,
                                         k,
                                         1,
                                         1,
                                         1,
                                         1};
      auto result = dispatch(*state_, state_->matrix, {b.storage_, a.storage_, *output}, push,
                             (columns + 63) / 64, (rows + 63) / 64);
      if (!result)
        return std::unexpected(result.error());
    }
    return Buffer(std::move(*output));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
} // namespace kalandra::runtime::vulkan
