#include "Vulkan.h"
#include "BuiltinShaders.h"
#include "SpirvInterface.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include "VulkanLoader.h"
#include "MemoryType.h"

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
          vulkanApi().vkGetInstanceProcAddr(handle, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy)
        destroy(handle, messenger, nullptr);
    }
    if (handle)
      vulkanApi().vkDestroyInstance(handle, nullptr);
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
  if (!vulkanApi().available)
    return std::unexpected(Error::Unavailable);
  auto result = std::make_unique<Instance>();
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (debug) {
    std::uint32_t count = 0;
    if (vulkanApi().vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
    std::vector<VkLayerProperties> layers(count);
    if (vulkanApi().vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS ||
        std::none_of(layers.begin(), layers.end(),
                     [&](const auto &p) { return std::strcmp(p.layerName, layer) == 0; }))
      return std::unexpected(Error::Unavailable);
  }
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "Kalandra Vulkan bring-up";
  std::uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (vulkanApi().vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
      loaderVersion < VK_API_VERSION_1_1)
    return std::unexpected(Error::Unsupported);
  application.apiVersion = std::min(loaderVersion, std::uint32_t(VK_API_VERSION_1_2));
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
  auto status = vulkanApi().vkCreateInstance(&info, nullptr, &result->handle);
  if (status != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  if (debug) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vulkanApi().vkGetInstanceProcAddr(result->handle, "vkCreateDebugUtilsMessengerEXT"));
    if (!create || create(result->handle, &callback, nullptr, &result->messenger) != VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
  }
  return result;
}
std::expected<std::vector<VkPhysicalDevice>, Error> physicals(VkInstance handle) {
  std::uint32_t count = 0;
  if (vulkanApi().vkEnumeratePhysicalDevices(handle, &count, nullptr) != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  std::vector<VkPhysicalDevice> devices(count);
  if (vulkanApi().vkEnumeratePhysicalDevices(handle, &count, devices.data()) != VK_SUCCESS)
    return std::unexpected(Error::Unavailable);
  devices.resize(count);
  // Never silently select software rendering as a GPU execution fallback.
  std::erase_if(devices, [](auto device) {
    VkPhysicalDeviceProperties p;
    vulkanApi().vkGetPhysicalDeviceProperties(device, &p);
    return p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
           p.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  });
  return devices;
}
DeviceInfo description(VkPhysicalDevice device) {
  VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &identity;
  vulkanApi().vkGetPhysicalDeviceProperties2(device, &properties);
  const auto &p = properties.properties;
  DeviceInfo result{p.deviceName,
                    p.vendorID,
                    p.deviceID,
                    p.apiVersion,
                    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU};
  std::copy_n(identity.deviceUUID, result.uuid.size(), result.uuid.begin());
  return result;
}
struct Pipeline {
  VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline handle = VK_NULL_HANDLE;
};
void destroyPipeline(VkDevice device, Pipeline &pipeline) noexcept {
  if (pipeline.handle)
    vulkanApi().vkDestroyPipeline(device, pipeline.handle, nullptr);
  if (pipeline.layout)
    vulkanApi().vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
  if (pipeline.descriptors)
    vulkanApi().vkDestroyDescriptorSetLayout(device, pipeline.descriptors, nullptr);
  pipeline = {};
}
} // namespace
struct ByteBudget {
  mutable std::mutex mutex;
  std::uint64_t bytes = 0, maximum = 0;
};
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
  mutable std::mutex queueMutex;
  std::mutex builtinMutex;
  // Weak references avoid a context/kernel ownership cycle. Immutable built-in
  // plans share one loaded object across arbitrarily many prepared programs.
  std::weak_ptr<KernelState> tiledKernel, transposeKernel;
  std::shared_ptr<ByteBudget> budget;
  std::atomic<bool> failed{false};
  std::atomic<FailurePoint> failure{FailurePoint::None};
  VkPhysicalDeviceFeatures features{};
  bool storage8 = false, storage16 = false, int8 = false;
  bool preserveFloat32 = false, preserveFloat64 = false, denormFloat64 = false;
  bool tiledMatmul = true;
  std::uint32_t maxDescriptors = 0, kernels = 0;
  ~State() {
    if (!device)
      return;
    // No command resources may be freed while the device still owns them.
    vulkanApi().vkDeviceWaitIdle(device);
    for (auto *p : {&addition, &matrix})
      destroyPipeline(device, *p);
    if (descriptors)
      vulkanApi().vkDestroyDescriptorPool(device, descriptors, nullptr);
    if (commands)
      vulkanApi().vkDestroyCommandPool(device, commands, nullptr);
    vulkanApi().vkDestroyDevice(device, nullptr);
  }
};
struct Storage {
  std::shared_ptr<State> state;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize allocation = 0;
  std::uint64_t bytes = 0, capacity = 0;
  bool coherent = false;
  VkMemoryPropertyFlags memoryFlags = 0;
  // Published storage is immutable and fence-complete. Serialize host cache
  // invalidations per allocation, without waiting on unrelated queue work.
  mutable std::mutex hostMutex;
  ~Storage() {
    if (mapped)
      vulkanApi().vkUnmapMemory(state->device, memory);
    if (buffer)
      vulkanApi().vkDestroyBuffer(state->device, buffer, nullptr);
    if (memory)
      vulkanApi().vkFreeMemory(state->device, memory, nullptr);
    std::lock_guard lock(state->budget->mutex);
    state->budget->bytes -= allocation;
  }
};
struct HostCharge {
  std::shared_ptr<ByteBudget> budget;
  std::uint64_t bytes = 0;
  ~HostCharge() {
    std::lock_guard lock(budget->mutex);
    budget->bytes -= bytes;
  }
};
namespace {
bool consumeFailure(State &state, FailurePoint point) noexcept {
  if (state.failure.load(std::memory_order_relaxed) != point)
    return false;
  return state.failure.compare_exchange_strong(point, FailurePoint::None);
}
std::expected<std::shared_ptr<Storage>, Error> allocate(const std::shared_ptr<State> &state,
                                                        std::uint64_t bytes, bool readback = false) {
  auto s = std::make_shared<Storage>();
  s->state = state;
  s->bytes = bytes;
  if (!bytes)
    return s;
  if (bytes > state->properties.limits.maxStorageBufferRange)
    return std::unexpected(Error::Unsupported);
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  s->capacity = (bytes + 3) & ~UINT64_C(3);
  if (s->capacity > state->properties.limits.maxStorageBufferRange)
    return std::unexpected(Error::Unsupported);
  info.size = s->capacity;
  info.usage = readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                        : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  auto result = consumeFailure(*state, FailurePoint::BufferCreate)
                    ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                    : vulkanApi().vkCreateBuffer(state->device, &info, nullptr, &s->buffer);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  VkMemoryRequirements required;
  vulkanApi().vkGetBufferMemoryRequirements(state->device, s->buffer, &required);
  const auto memoryType = mappedMemoryType(state->memory, required.memoryTypeBits, readback);
  if (memoryType == UINT32_MAX)
    return std::unexpected(Error::Unsupported);
  {
    std::lock_guard lock(state->budget->mutex);
    if (required.size > state->budget->maximum - state->budget->bytes)
      return std::unexpected(Error::Budget);
    state->budget->bytes += required.size;
    s->allocation = required.size;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = required.size;
  allocation.memoryTypeIndex = memoryType;
  result = consumeFailure(*state, FailurePoint::MemoryAllocate)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkAllocateMemory(state->device, &allocation, nullptr, &s->memory);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  result = consumeFailure(*state, FailurePoint::MemoryBind)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkBindBufferMemory(state->device, s->buffer, s->memory, 0);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  result = consumeFailure(*state, FailurePoint::MemoryMap)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkMapMemory(state->device, s->memory, 0, VK_WHOLE_SIZE, 0, &s->mapped);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  s->memoryFlags = state->memory.memoryTypes[memoryType].propertyFlags;
  s->coherent = s->memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return s;
}
std::expected<void, Error> visibility(const Storage &s, bool flush) {
  if (!s.bytes || s.coherent)
    return {};
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = s.memory;
  range.size = VK_WHOLE_SIZE;
  auto result = flush ? vulkanApi().vkFlushMappedMemoryRanges(s.state->device, 1, &range)
                      : vulkanApi().vkInvalidateMappedMemoryRanges(s.state->device, 1, &range);
  if (result != VK_SUCCESS)
    return std::unexpected(error(result));
  return {};
}
std::expected<void, Error> makePipeline(State &state, Pipeline &p,
                                        std::span<const std::uint32_t> code, const char *entryName,
                                        std::uint32_t bindings, std::uint32_t pushBytes,
                                        const VkSpecializationInfo *specialization = nullptr) {
  std::vector<VkDescriptorSetLayoutBinding> layouts(bindings);
  for (std::uint32_t i = 0; i < bindings; ++i)
    layouts[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptors.bindingCount = bindings;
  descriptors.pBindings = layouts.data();
  auto status = vulkanApi().vkCreateDescriptorSetLayout(state.device, &descriptors, nullptr, &p.descriptors);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &p.descriptors;
  layout.pushConstantRangeCount = pushBytes ? 1 : 0;
  layout.pPushConstantRanges = &push;
  status = consumeFailure(state, FailurePoint::PipelineLayout)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkCreatePipelineLayout(state.device, &layout, nullptr, &p.layout);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader.codeSize = code.size() * 4;
  shader.pCode = code.data();
  VkShaderModule module = VK_NULL_HANDLE;
  status = vulkanApi().vkCreateShaderModule(state.device, &shader, nullptr, &module);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  compute.layout = p.layout;
  compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  compute.stage.module = module;
  compute.stage.pName = entryName;
  compute.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
  compute.stage.pSpecializationInfo = specialization;
  status = consumeFailure(state, FailurePoint::PipelineCreate)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &compute, nullptr, &p.handle);
  vulkanApi().vkDestroyShaderModule(state.device, module, nullptr);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  return {};
}
std::expected<std::vector<std::uint32_t>, Error> readSpirv(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    return std::unexpected(Error::Unavailable);
  auto length = input.tellg();
  if (length < 20 || length > 1024 * 1024 || length % 4)
    return std::unexpected(Error::Invalid);
  std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(code.data()), length);
  if (!input || code.front() != 0x07230203)
    return std::unexpected(Error::Invalid);
  return code;
}
std::expected<void, Error> pipeline(State &state, Pipeline &p, const char *name,
                                    std::uint32_t bindings, std::uint32_t pushBytes,
                                    bool addition) {
  // Called under queueMutex. Failed preparation releases partial objects and
  // leaves the slot empty, so a recoverable allocation failure can be retried.
  if (p.handle)
    return {};
  const auto &limits = state.properties.limits;
  const std::uint32_t threads = addition ? 256 : 128;
  // The selected F32 ggml tile uses two [64 * (32/2 + 1)] vec2 arrays.
  const std::uint32_t sharedBytes = addition ? 0 : 2 * 64 * 17 * 8;
  if (limits.maxPushConstantsSize < pushBytes || state.maxDescriptors < bindings ||
      limits.maxComputeWorkGroupInvocations < threads ||
      limits.maxComputeWorkGroupSize[0] < threads ||
      limits.maxComputeSharedMemorySize < sharedBytes)
    return std::unexpected(Error::Unsupported);
  auto code = builtinShader(name);
  if (code.empty())
    return std::unexpected(Error::Unavailable);
  std::uint32_t value = addition ? VK_TRUE : 128;
  VkSpecializationMapEntry entry{0, 0, sizeof(value)};
  VkSpecializationInfo constants{1, &entry, sizeof(value), &value};
  Pipeline candidate;
  auto made = makePipeline(state, candidate, code, "main", bindings, pushBytes, &constants);
  if (!made) {
    destroyPipeline(state.device, candidate);
    return std::unexpected(made.error());
  }
  p = candidate;
  return {};
}
struct Dispatch {
  State &state;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkDescriptorSet descriptors = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  ~Dispatch() {
    if (fence)
      vulkanApi().vkDestroyFence(state.device, fence, nullptr);
    if (command)
      vulkanApi().vkFreeCommandBuffers(state.device, state.commands, 1, &command);
    if (descriptors)
      vulkanApi().vkFreeDescriptorSets(state.device, state.descriptors, 1, &descriptors);
  }
};
std::expected<void, Error> beginDispatch(Dispatch &run) {
  auto &state = run.state;
  VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocation.commandPool = state.commands;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  auto status = consumeFailure(state, FailurePoint::CommandAllocate)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkAllocateCommandBuffers(state.device, &allocation, &run.command);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  status = consumeFailure(state, FailurePoint::CommandBegin)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkBeginCommandBuffer(run.command, &begin);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  return {};
}
std::expected<void, Error> finishDispatch(Dispatch &run) {
  auto &state = run.state;
  auto status = consumeFailure(state, FailurePoint::CommandEnd)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkEndCommandBuffer(run.command);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  status = consumeFailure(state, FailurePoint::FenceCreate)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkCreateFence(state.device, &fence, nullptr, &run.fence);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &run.command;
  status = consumeFailure(state, FailurePoint::QueueSubmit)
               ? VK_ERROR_OUT_OF_DEVICE_MEMORY
               : vulkanApi().vkQueueSubmit(state.queue, 1, &submit, run.fence);
  if (status == VK_SUCCESS)
    status = vulkanApi().vkWaitForFences(state.device, 1, &run.fence, VK_TRUE, UINT64_MAX);
  // Do not simulate a failed wait while real commands still own these objects.
  // Exercise the same error/poison path only after actual fence quiescence.
  if (status == VK_SUCCESS && consumeFailure(state, FailurePoint::FenceWait))
    status = VK_ERROR_DEVICE_LOST;
  if (status != VK_SUCCESS) {
    // A failed wait is not proof that resources can be reclaimed. Settle the
    // device before releasing local command/fence/descriptor/buffer owners.
    vulkanApi().vkDeviceWaitIdle(state.device);
    state.failed = true;
    return std::unexpected(error(status));
  }
  return {};
}
std::expected<void, Error> dispatch(State &state, Pipeline &p,
                                    std::span<const std::shared_ptr<Storage>> buffers,
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
  auto status = consumeFailure(state, FailurePoint::DescriptorAllocate)
                    ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                    : vulkanApi().vkAllocateDescriptorSets(state.device, &set, &run.descriptors);
  if (status != VK_SUCCESS)
    return std::unexpected(error(status));
  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (std::uint32_t i = 0; i < buffers.size(); ++i) {
    infos[i] = {buffers[i]->buffer, 0, buffers[i]->capacity};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = run.descriptors;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vulkanApi().vkUpdateDescriptorSets(state.device, writes.size(), writes.data(), 0, nullptr);
  auto begun = beginDispatch(run);
  if (!begun)
    return std::unexpected(begun.error());
  VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vulkanApi().vkCmdPipelineBarrier(run.command,
                       VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  vulkanApi().vkCmdBindPipeline(run.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.handle);
  vulkanApi().vkCmdBindDescriptorSets(run.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                          &run.descriptors, 0, nullptr);
  if (!push.empty())
    vulkanApi().vkCmdPushConstants(run.command, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push.size_bytes(),
                       push.data());
  vulkanApi().vkCmdDispatch(run.command, x, y, 1);
  VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vulkanApi().vkCmdPipelineBarrier(run.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
  return finishDispatch(run);
}
// Called with queueMutex held. Owners and command resources remain alive until
// the real fence completes, including the existing device-loss settlement path.
std::expected<void, Error> copyStorage(State &state, const Storage &source, const Storage &target) {
  Dispatch run{state};
  auto begun = beginDispatch(run);
  if (!begun)
    return std::unexpected(begun.error());
  VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vulkanApi().vkCmdPipelineBarrier(run.command,
      VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  VkBufferCopy region{0, 0, source.capacity};
  vulkanApi().vkCmdCopyBuffer(run.command, source.buffer, target.buffer, 1, &region);
  VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vulkanApi().vkCmdPipelineBarrier(run.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
  return finishDispatch(run);
}
std::expected<void, Error> readStorage(const std::shared_ptr<Storage> &source,
                                      std::span<std::byte> output) {
  auto &state = *source->state;
  // Small/cached reads avoid an extra submission. Larger uncached mapped reads
  // otherwise serialize thousands of CPU transactions across PCIe or uncached RAM.
  if (output.size() >= 64 * 1024 &&
      !(source->memoryFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
      mappedMemoryType(state.memory, UINT32_MAX, true) != UINT32_MAX) {
    std::lock_guard queue(state.queueMutex);
    if (state.failed)
      return std::unexpected(Error::Device);
    bool room;
    {
      std::lock_guard budget(state.budget->mutex);
      room = source->capacity <= state.budget->maximum - state.budget->bytes;
    }
    if (room) {
      auto staging = allocate(source->state, source->bytes, true);
      if (staging) {
        auto copied = copyStorage(state, *source, **staging);
        if (!copied)
          return std::unexpected(copied.error());
        auto visible = visibility(**staging, false);
        if (!visible)
          return std::unexpected(visible.error());
        std::memcpy(output.data(), (*staging)->mapped, output.size());
        return {};
      }
      // Preserve transfers that fit the existing budget without staging. Other
      // failures remain real errors rather than being hidden by an optimization.
      if (staging.error() != Error::Budget && staging.error() != Error::Unsupported)
        return std::unexpected(staging.error());
    }
  }
  auto visible = visibility(*source, false);
  if (!visible)
    return std::unexpected(visible.error());
  if (!output.empty())
    std::memcpy(output.data(), source->mapped, output.size());
  return {};
}

} // namespace
struct KernelState {
  std::shared_ptr<State> state;
  Pipeline pipeline;
  SpirvInterface interface;
  bool reserved = false;
  bool tiledMatmul = false;
  ~KernelState() {
    if (pipeline.handle)
      vulkanApi().vkDestroyPipeline(state->device, pipeline.handle, nullptr);
    if (pipeline.layout)
      vulkanApi().vkDestroyPipelineLayout(state->device, pipeline.layout, nullptr);
    if (pipeline.descriptors)
      vulkanApi().vkDestroyDescriptorSetLayout(state->device, pipeline.descriptors, nullptr);
    if (reserved) {
      std::lock_guard lock(state->budget->mutex);
      --state->kernels;
    }
  }
};
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
    state->tiledMatmul = options.enableTiledMatmul;
    auto context = instance(options.validation);
    if (!context)
      return std::unexpected(context.error());
    state->instance = std::move(*context);
    auto devices = physicals(state->instance->handle);
    if (!devices)
      return std::unexpected(devices.error());
    if (options.deviceUuid) {
      auto found = std::find_if(devices->begin(), devices->end(), [&](auto device) {
        return description(device).uuid == *options.deviceUuid;
      });
      if (found == devices->end())
        return std::unexpected(Error::Unavailable);
      state->physical = *found;
    } else {
      if (options.deviceIndex >= devices->size())
        return std::unexpected(Error::Unavailable);
      state->physical = (*devices)[options.deviceIndex];
    }
    if (options.budget)
      state->budget = std::move(options.budget);
    else {
      auto budget = createByteBudget(options.maxBytes);
      if (!budget)
        return std::unexpected(budget.error());
      state->budget = std::move(*budget);
    }
    vulkanApi().vkGetPhysicalDeviceProperties(state->physical, &state->properties);
    vulkanApi().vkGetPhysicalDeviceMemoryProperties(state->physical, &state->memory);
    if (state->properties.apiVersion < VK_API_VERSION_1_1)
      return std::unexpected(Error::Unsupported);
    std::uint32_t count = 0;
    vulkanApi().vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vulkanApi().vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, queues.data());
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
    VkPhysicalDevice16BitStorageFeatures storage16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDevice8BitStorageFeatures storage8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features narrow{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    std::uint32_t extensionCount = 0;
    if (vulkanApi().vkEnumerateDeviceExtensionProperties(state->physical, nullptr, &extensionCount, nullptr) !=
        VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (vulkanApi().vkEnumerateDeviceExtensionProperties(state->physical, nullptr, &extensionCount,
                                             extensions.data()) != VK_SUCCESS)
      return std::unexpected(Error::Unavailable);
    const auto hasExtension = [&](const char *name) {
      return std::any_of(extensions.begin(), extensions.end(),
                         [&](const auto &e) { return !std::strcmp(e.extensionName, name); });
    };
    std::vector<const char *> enabledExtensions;
    if (hasExtension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME) &&
        hasExtension(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
      enabledExtensions = {VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                           VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME};
      storage16.pNext = &storage8;
      storage8.pNext = &narrow;
    }
    if (state->properties.apiVersion >= VK_API_VERSION_1_2) {
      if (hasExtension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME))
        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
      VkPhysicalDeviceFloatControlsProperties controls{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &controls;
      vulkanApi().vkGetPhysicalDeviceProperties2(state->physical, &properties);
      state->preserveFloat32 = controls.shaderSignedZeroInfNanPreserveFloat32;
      state->preserveFloat64 = controls.shaderSignedZeroInfNanPreserveFloat64;
      state->denormFloat64 = controls.shaderDenormPreserveFloat64;
    }
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &storage16;
    vulkanApi().vkGetPhysicalDeviceFeatures2(state->physical, &features);
    state->features.shaderInt16 = features.features.shaderInt16;
    state->features.shaderInt64 = features.features.shaderInt64;
    state->features.shaderFloat64 = options.enableFloat64 && features.features.shaderFloat64;
    state->storage16 = storage16.storageBuffer16BitAccess;
    state->storage8 = storage8.storageBuffer8BitAccess;
    state->int8 = narrow.shaderInt8;
    storage16.uniformAndStorageBuffer16BitAccess = storage16.storagePushConstant16 =
        storage16.storageInputOutput16 = VK_FALSE;
    storage8.uniformAndStorageBuffer8BitAccess = storage8.storagePushConstant8 = VK_FALSE;
    narrow.shaderFloat16 = VK_FALSE;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pEnabledFeatures = &state->features;
    device.pNext = &storage16;
    device.enabledExtensionCount = enabledExtensions.size();
    device.ppEnabledExtensionNames = enabledExtensions.data();
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    auto status = vulkanApi().vkCreateDevice(state->physical, &device, nullptr, &state->device);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    vulkanApi().vkGetDeviceQueue(state->device, family, 0, &state->queue);
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands.queueFamilyIndex = family;
    commands.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    status = vulkanApi().vkCreateCommandPool(state->device, &commands, nullptr, &state->commands);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    state->maxDescriptors =
        std::min({128u, state->properties.limits.maxPerStageDescriptorStorageBuffers,
                  state->properties.limits.maxDescriptorSetStorageBuffers});
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state->maxDescriptors};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptors.maxSets = 1;
    descriptors.poolSizeCount = 1;
    descriptors.pPoolSizes = &size;
    status = vulkanApi().vkCreateDescriptorPool(state->device, &descriptors, nullptr, &state->descriptors);
    if (status != VK_SUCCESS)
      return std::unexpected(error(status));
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
DeviceInfo Runtime::device() const { return description(state_->physical); }
bool Runtime::owns(const Buffer &buffer) const noexcept {
  return buffer.storage_ && buffer.storage_->state == state_;
}
bool Runtime::owns(const Kernel &kernel) const noexcept {
  return kernel.state_ && kernel.state_->state == state_;
}
std::uint32_t Kernel::inputCount() const noexcept {
  return state_ ? state_->tiledMatmul ? 2 : state_->interface.buffers - 2 : 0;
}
std::uint32_t Kernel::localSize() const noexcept {
  return state_ ? state_->tiledMatmul ? 128 : state_->interface.localSize : 0;
}
std::uint32_t Kernel::parameterWords() const noexcept {
  return state_ ? state_->tiledMatmul ? 4 : state_->interface.parameterWords : 0;
}
std::string_view Kernel::entryName() const noexcept {
  return state_ ? state_->tiledMatmul ? std::string_view("main")
                                    : std::string_view(state_->interface.entry) : std::string_view{};
}
bool Kernel::tiledMatmul() const noexcept { return state_ && state_->tiledMatmul; }
std::uint64_t Runtime::retainedBytes() const {
  std::lock_guard lock(state_->budget->mutex);
  return state_->budget->bytes;
}
std::expected<std::shared_ptr<ByteBudget>, Error> Runtime::createByteBudget(std::uint64_t maximum) {
  if (!maximum)
    return std::unexpected(Error::Invalid);
  try {
    auto result = std::make_shared<ByteBudget>();
    result->maximum = maximum;
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::shared_ptr<const HostCharge>, Error>
Runtime::reserveHostBytes(std::uint64_t bytes) {
  if (state_->failed)
    return std::unexpected(Error::Device);
  return reserveBytes(state_->budget, bytes);
}
std::uint64_t Runtime::reservedBytes(const std::shared_ptr<ByteBudget> &budget) noexcept {
  if (!budget)
    return 0;
  std::lock_guard lock(budget->mutex);
  return budget->bytes;
}
std::expected<std::shared_ptr<const HostCharge>, Error>
Runtime::reserveBytes(const std::shared_ptr<ByteBudget> &budget, std::uint64_t bytes) {
  if (!budget)
    return std::unexpected(Error::Invalid);
  try {
    auto charge = std::make_shared<HostCharge>();
    charge->budget = budget;
    std::lock_guard lock(budget->mutex);
    if (bytes > budget->maximum - budget->bytes)
      return std::unexpected(Error::Budget);
    budget->bytes += bytes;
    charge->bytes = bytes;
    return charge;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
bool Runtime::owns(const std::shared_ptr<const HostCharge> &charge) const noexcept {
  return charge && charge->budget == state_->budget;
}
std::uint64_t Runtime::validationErrors() const { return state_->instance->errors.load(); }
void Runtime::injectFailure(FailurePoint point) noexcept { state_->failure = point; }
std::expected<Buffer, Error> Runtime::upload(std::span<const float> data) {
  try {
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
    if (!buffer.storage_ || buffer.storage_->state != state_ || buffer.bytes() % sizeof(float))
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    std::vector<float> result(buffer.bytes() / sizeof(float));
    auto copied = readBytes(buffer, std::as_writable_bytes(std::span(result)));
    if (!copied)
      return std::unexpected(copied.error());
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Buffer, Error> Runtime::add(const Buffer &a, const Buffer &b) {
  try {
    std::lock_guard lock(state_->queueMutex);
    if (!a.storage_ || !b.storage_ || a.storage_->state != state_ || b.storage_->state != state_ ||
        a.bytes() != b.bytes() || a.bytes() % sizeof(float))
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
      auto prepared = pipeline(*state_, state_->addition, "add.spv", 4, 116, true);
      if (!prepared)
        return std::unexpected(prepared.error());
      std::array<std::uint32_t, 29> push{};
      push[0] = count;
      for (std::size_t base : {1u, 9u, 17u}) {
        push[base] = count;
        push[base + 1] = push[base + 2] = push[base + 3] = 1;
        push[base + 4] = 1;
        push[base + 5] = push[base + 6] = push[base + 7] = count;
      }
      auto result = dispatch(*state_, state_->addition, std::array{a.storage_, b.storage_, *output},
                             push, 1, static_cast<std::uint32_t>((count + 511) / 512));
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
        a.bytes() % sizeof(float) || b.bytes() % sizeof(float) ||
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
      auto prepared = pipeline(*state_, state_->matrix, "mul_mm.spv", 3, 64, false);
      if (!prepared)
        return std::unexpected(prepared.error());
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
      auto result = dispatch(*state_, state_->matrix, std::array{b.storage_, a.storage_, *output},
                             push, (columns + 63) / 64, (rows + 63) / 64);
      if (!result)
        return std::unexpected(result.error());
    }
    return Buffer(std::move(*output));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
} // namespace kalandra::runtime::vulkan

namespace kalandra::runtime::vulkan {
std::expected<Buffer, Error> Runtime::uploadBytes(std::span<const std::byte> bytes) {
  try {
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto storage = allocate(state_, bytes.size());
    if (!storage)
      return std::unexpected(storage.error());
    if (!bytes.empty()) {
      std::memset((*storage)->mapped, 0, (*storage)->capacity);
      std::memcpy((*storage)->mapped, bytes.data(), bytes.size());
    }
    auto visible = visibility(**storage, true);
    if (!visible)
      return std::unexpected(visible.error());
    return Buffer(std::move(*storage));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Buffer, Error> Runtime::blankBytes(std::uint64_t bytes) {
  try {
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto storage = allocate(state_, bytes);
    if (!storage)
      return std::unexpected(storage.error());
    if ((*storage)->capacity)
      std::memset((*storage)->mapped, 0, (*storage)->capacity);
    auto visible = visibility(**storage, true);
    if (!visible)
      return std::unexpected(visible.error());
    return Buffer(std::move(*storage));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::vector<std::byte>, Error> Runtime::downloadBytes(const Buffer &buffer) {
  try {
    if (!buffer.storage_ || buffer.storage_->state != state_)
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    std::vector<std::byte> result(buffer.bytes());
    auto copied = readBytes(buffer, result);
    if (!copied)
      return std::unexpected(copied.error());
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<void, Error> Runtime::readBytes(const Buffer &buffer, std::span<std::byte> output) {
  try {
    if (!buffer.storage_ || buffer.storage_->state != state_ || output.size() != buffer.bytes())
      return std::unexpected(Error::Invalid);
    std::lock_guard lock(buffer.storage_->hostMutex);
    if (state_->failed)
      return std::unexpected(Error::Device);
    return readStorage(buffer.storage_, output);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Kernel, Error> Runtime::loadKernel(const std::filesystem::path &path, const KernelContract *contract) {
  try {
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto words = readSpirv(path);
    if (!words)
      return std::unexpected(words.error());
    return createKernel(*words, contract);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Kernel, Error> Runtime::createKernel(std::span<const std::uint32_t> words, const KernelContract *contract) {
  try {
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto interface = reflectSpirv(words);
    if (!interface)
      return std::unexpected(Error::Invalid);
    // Validate the intended logical ABI before capability admission. Otherwise
    // a wrong-but-valid module could masquerade as Unsupported and select an
    // alternative instead of reporting a malformed primary artifact.
    if (contract && (interface->entry != contract->entry || interface->buffers < 2 ||
                     std::uint64_t(interface->buffers - 2) != contract->inputs ||
                     interface->parameterWords != contract->parameterWords))
      return std::unexpected(Error::Invalid);
    if (interface->buffers < 2 || interface->buffers > state_->maxDescriptors ||
        interface->localSize > state_->properties.limits.maxComputeWorkGroupSize[0] ||
        interface->localSize > state_->properties.limits.maxComputeWorkGroupInvocations ||
        (interface->int8 && !state_->int8) || (interface->int16 && !state_->features.shaderInt16) ||
        (interface->int64 && !state_->features.shaderInt64) ||
        (interface->float64 && (!state_->features.shaderFloat64 || !state_->preserveFloat64)) ||
        (interface->preserveFloat32 && !state_->preserveFloat32) ||
        (interface->preserveFloat64 && !state_->preserveFloat64) ||
        (interface->denormFloat64 && !state_->denormFloat64) ||
        (interface->storage8 && !state_->storage8) || (interface->storage16 && !state_->storage16))
      return std::unexpected(Error::Unsupported);
    // Intel's native OpFDiv f64 fails signed-zero, tiny-divisor and rounding
    // checks even with advertised float controls. Do not silently weaken the
    // numerical contract: hardware-only rejects; explicit fallback may select
    // the independently checked integer-arithmetic companion. Other f64
    // operations continue to use hardware with preservation controls below.
    if (interface->float64Division && state_->properties.vendorID == 0x8086)
      return std::unexpected(Error::Unsupported);
    std::vector<std::uint32_t> controlled;
    if (interface->float64) {
      auto native = preserveNativeFloat64(words, state_->denormFloat64);
      if (!native)
        return std::unexpected(Error::Invalid);
      controlled = std::move(*native);
      words = controlled;
      interface->preserveFloat64 = true;
      interface->denormFloat64 |= state_->denormFloat64;
    }
    auto loaded = std::make_shared<KernelState>();
    loaded->state = state_;
    loaded->interface = *interface;
    {
      std::lock_guard budget(state_->budget->mutex);
      if (state_->kernels >= 64)
        return std::unexpected(Error::Budget);
      ++state_->kernels;
      loaded->reserved = true;
    }
    auto made = makePipeline(*state_, loaded->pipeline, words, interface->entry.c_str(),
                             interface->buffers, 0);
    if (!made)
      return std::unexpected(made.error());
    Kernel result;
    result.state_ = std::move(loaded);
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Kernel, Error> Runtime::loadBuiltinTransposeFloat32() {
  std::lock_guard lock(state_->builtinMutex);
  if (state_->failed)
    return std::unexpected(Error::Device);
  if (auto cached = state_->transposeKernel.lock()) {
    Kernel result;
    result.state_ = std::move(cached);
    return result;
  }
  auto result = createKernel(builtinShader("transforms/transpose-float32.spv"));
  if (result)
    state_->transposeKernel = result->state_;
  return result;
}
std::expected<Kernel, Error> Runtime::loadMatmulKernel(const std::filesystem::path &path) {
  try {
    if (state_->failed)
      return std::unexpected(Error::Device);
    auto words = readSpirv(path);
    if (!words)
      return std::unexpected(words.error());
    const auto trusted = builtinShader("mul_mm.spv");
    const KernelContract fixedContract{"kalandra_numeric_v1_matmul_float32_vk_flat_v1", 2, 4};
    if (!std::equal(words->begin(), words->end(), trusted.begin(), trusted.end()))
      return createKernel(*words, &fixedContract);
    const auto &limits = state_->properties.limits;
    // Source execution also needs the 256-lane bitwise transpose. The raw
    // right-transposed primitive itself retains its narrower 128-lane floor.
    if (!state_->tiledMatmul || limits.maxComputeWorkGroupInvocations < 256 ||
        limits.maxComputeWorkGroupSize[0] < 256 || limits.maxPushConstantsSize < 64 ||
        limits.maxComputeSharedMemorySize < 2 * 64 * 17 * 8 || state_->maxDescriptors < 3) {
      auto alternate = path;
      alternate += ".fixed-tree.spv";
      auto fallback = readSpirv(alternate);
      if (!fallback)
        return std::unexpected(fallback.error() == Error::Unavailable ? Error::Unsupported
                                                                     : fallback.error());
      return createKernel(*fallback, &fixedContract);
    }
    std::lock_guard lock(state_->builtinMutex);
    if (state_->failed)
      return std::unexpected(Error::Device);
    if (auto cached = state_->tiledKernel.lock()) {
      Kernel result;
      result.state_ = std::move(cached);
      return result;
    }
    auto loaded = std::make_shared<KernelState>();
    loaded->state = state_;
    loaded->tiledMatmul = true;
    {
      std::lock_guard budget(state_->budget->mutex);
      if (state_->kernels >= 64)
        return std::unexpected(Error::Budget);
      ++state_->kernels;
      loaded->reserved = true;
    }
    Kernel result;
    state_->tiledKernel = loaded;
    result.state_ = std::move(loaded);
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<KernelResult, Error> Runtime::execute(const Kernel &kernel,
                                                    std::span<const Buffer> inputs,
                                                    std::span<const std::uint32_t> parameters,
                                                    std::uint64_t outputBytes,
                                                    std::uint32_t groups) {
  try {
    if (kernel.tiledMatmul()) {
      if (!owns(kernel) || inputs.size() != 2 || parameters.size() != 4 || !groups)
        return std::unexpected(Error::Invalid);
      const auto cells = std::uint64_t(parameters[0]) * parameters[2];
      if (cells > UINT32_MAX || outputBytes != cells * 4)
        return std::unexpected(Error::Invalid);
      auto result = matmulRightTransposed(inputs[0], inputs[1], parameters[0], parameters[2], parameters[1]);
      if (!result)
        return std::unexpected(result.error());
      return KernelResult{std::move(*result), std::vector<std::uint32_t>(parameters.begin(), parameters.end())};
    }
    std::lock_guard lock(state_->queueMutex);
    if (!kernel.state_ || kernel.state_->state != state_ ||
        inputs.size() + 2 != kernel.state_->interface.buffers ||
        parameters.size() != kernel.state_->interface.parameterWords ||
        parameters.size_bytes() > 1024 * 1024 || !groups)
      return std::unexpected(Error::Invalid);
    if (state_->failed)
      return std::unexpected(Error::Device);
    const auto &limits = state_->properties.limits;
    std::uint32_t x = groups, y = 1;
    if (kernel.state_->interface.flattenedGrid || kernel.state_->interface.groupOnlyGrid) {
      x = std::min({groups, limits.maxComputeWorkGroupCount[0], 65535u});
      y = static_cast<std::uint32_t>((std::uint64_t(groups) + x - 1) / x);
      // Shared GPU indices lower to uint32. Include padded groups in the
      // overflow check so inactive tail lanes cannot wrap back into the tensor.
      const auto lanes =
          kernel.state_->interface.groupOnlyGrid ? 1u : kernel.state_->interface.localSize;
      if (std::uint64_t(x) * y * lanes > UINT64_C(1) << 32)
        return std::unexpected(Error::Unsupported);
    }
    if (x > limits.maxComputeWorkGroupCount[0] || y > limits.maxComputeWorkGroupCount[1])
      return std::unexpected(Error::Unsupported);
    std::vector<std::shared_ptr<Storage>> bindings;
    for (const auto &input : inputs) {
      if (!input.storage_ || input.storage_->state != state_)
        return std::unexpected(Error::Invalid);
      if (input.bytes())
        bindings.push_back(input.storage_);
      else {
        auto dummy = allocate(state_, 4);
        if (!dummy)
          return std::unexpected(dummy.error());
        bindings.push_back(*dummy);
      }
    }
    auto output = allocate(state_, std::max(UINT64_C(4), outputBytes));
    if (!output)
      return std::unexpected(output.error());
    auto words = allocate(state_, parameters.size_bytes());
    if (!words)
      return std::unexpected(words.error());
    std::memcpy((*words)->mapped, parameters.data(), parameters.size_bytes());
    auto visible = visibility(**words, true);
    if (!visible)
      return std::unexpected(visible.error());
    std::memset((*output)->mapped, 0, (*output)->capacity);
    visible = visibility(**output, true);
    if (!visible)
      return std::unexpected(visible.error());
    bindings.push_back(*output);
    bindings.push_back(*words);
    auto run = dispatch(*state_, kernel.state_->pipeline, bindings, {}, x, y);
    if (!run)
      return std::unexpected(run.error());
    visible = visibility(**words, false);
    if (!visible)
      return std::unexpected(visible.error());
    KernelResult result{Buffer(std::move(*output)), std::vector<std::uint32_t>(parameters.size())};
    result.output.storage_->bytes = outputBytes;
    std::memcpy(result.parameters.data(), (*words)->mapped, parameters.size_bytes());
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
} // namespace kalandra::runtime::vulkan
