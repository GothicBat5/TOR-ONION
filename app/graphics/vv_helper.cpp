#include "gpu/vulkan/vulkan_fence_helper.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_function_pointers.h"

namespace gpu {

VulkanFenceHelper::FenceHandle::FenceHandle() = default;
VulkanFenceHelper::FenceHandle::FenceHandle(VkFence fence,uint64_t generation_id) : fence_(fence), generation_id_(generation_id) {  }
VulkanFenceHelper::FenceHandle::FenceHandle(const FenceHandle& other) = default;
VulkanFenceHelper::FenceHandle& VulkanFenceHelper::FenceHandle::operator=(const FenceHandle& other) = default;

VulkanFenceHelper::VulkanFenceHelper(VulkanDeviceQueue* device_queue) : device_queue_(device_queue) {  }

VulkanFenceHelper::~VulkanFenceHelper() {
  DCHECK(tasks_pending_fence_.empty());
  DCHECK(cleanup_tasks_.empty());
}

void VulkanFenceHelper::Destroy() {
  PerformImmediateCleanup();
}

VkResult VulkanFenceHelper::GetFence(VkFence* fence) {
  VkFenceCreateInfo create_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.pNext = nullptr, .flags = 0,};
  return vkCreateFence(device_queue_->GetVulkanDevice(), &create_info,nullptr /* pAllocator */, fence);
}

VulkanFenceHelper::FenceHandle VulkanFenceHelper::EnqueueFence(VkFence fence) {
  FenceHandle handle(fence, next_generation_++);
  cleanup_tasks_.emplace_back(handle, std::move(tasks_pending_fence_));
  tasks_pending_fence_ = std::vector<CleanupTask>();
  return handle;
}

bool VulkanFenceHelper::Wait(FenceHandle handle, uint64_t timeout_in_nanoseconds) {
  if (HasPassed(handle)) return true;
  VkResult result = vkWaitForFences(device_queue_->GetVulkanDevice(), 1, &handle.fence_, true, timeout_in_nanoseconds);

  ProcessCleanupTasks();

  return result == VK_SUCCESS;
}

bool VulkanFenceHelper::HasPassed(FenceHandle handle) {

  ProcessCleanupTasks();

  return current_generation_ >= handle.generation_id_;
}

void VulkanFenceHelper::EnqueueCleanupTaskForSubmittedWork(CleanupTask task) {
  tasks_pending_fence_.emplace_back(std::move(task));
}

void VulkanFenceHelper::ProcessCleanupTasks(uint64_t retired_generation_id) {
  VkDevice device = device_queue_->GetVulkanDevice();

  if (!retired_generation_id) retired_generation_id = current_generation_;

  for (const auto& tasks_for_fence : cleanup_tasks_) 
  {
    if (tasks_for_fence.UsingCallback()) continue;

    VkResult result = vkGetFenceStatus(device, tasks_for_fence.fence);
    if (result == VK_NOT_READY) {
      retired_generation_id = std::min(retired_generation_id, tasks_for_fence.generation_id - 1);
      break;
    }
    if (result == VK_SUCCESS) {
      retired_generation_id = std::max(tasks_for_fence.generation_id, retired_generation_id); continue;
    }
    DLOG(ERROR) << "vkGetFenceStatus() failed: " << result;
    PerformImmediateCleanup();
    return;
  }

  current_generation_ = retired_generation_id;
  std::vector<CleanupTask> tasks_to_run;
  while (!cleanup_tasks_.empty()) {
    TasksForFence& tasks_for_fence = cleanup_tasks_.front();
    if (tasks_for_fence.generation_id > current_generation_)
      break;
    if (tasks_for_fence.fence != VK_NULL_HANDLE) {
      DCHECK_EQ(vkGetFenceStatus(device, tasks_for_fence.fence), VK_SUCCESS);
      vkDestroyFence(device, tasks_for_fence.fence, nullptr);
    }
    tasks_to_run.insert(tasks_to_run.end(), std::make_move_iterator(tasks_for_fence.tasks.begin()), std::make_move_iterator(tasks_for_fence.tasks.end()));
    cleanup_tasks_.pop_front();
  }

  for (auto& task : tasks_to_run) std::move(task).Run(device_queue_.get(), false /* device_lost */);
}

VulkanFenceHelper::FenceHandle VulkanFenceHelper::GenerateCleanupFence() {
  if (tasks_pending_fence_.empty()) return FenceHandle();
  VkFence fence = VK_NULL_HANDLE;
  VkResult result = GetFence(&fence);
  if (result != VK_SUCCESS) {
    PerformImmediateCleanup();
    return FenceHandle();
  }
  result = vkQueueSubmit(device_queue_->GetVulkanQueue(), 0, nullptr, fence);
  if (result != VK_SUCCESS) {
    vkDestroyFence(device_queue_->GetVulkanDevice(), fence, nullptr);
    PerformImmediateCleanup();
    return FenceHandle();
  }

  return EnqueueFence(fence);
}

base::OnceClosure VulkanFenceHelper::CreateExternalCallback() {
  if (tasks_pending_fence_.empty()) return base::OnceClosure();

  uint64_t generation_id = next_generation_++;
  cleanup_tasks_.emplace_back(generation_id, std::move(tasks_pending_fence_));
  tasks_pending_fence_ = std::vector<CleanupTask>();

  return base::BindOnce([](base::WeakPtr<VulkanFenceHelper> fence_helper, uint64_t generation_id) 
  {
        if (!fence_helper)return;
        if (generation_id > fence_helper->current_generation_) {
          fence_helper->ProcessCleanupTasks(generation_id);
        }
      }, weak_factory_.GetWeakPtr(), generation_id);
}

void VulkanFenceHelper::EnqueueSemaphoreCleanupForSubmittedWork(
    VkSemaphore semaphore) {
  if (semaphore == VK_NULL_HANDLE) return;

  EnqueueSemaphoresCleanupForSubmittedWork({semaphore});
}

void VulkanFenceHelper::EnqueueSemaphoresCleanupForSubmittedWork(std::vector<VkSemaphore> semaphores) 
{
  if (semaphores.empty()) return;

  EnqueueCleanupTaskForSubmittedWork(base::BindOnce([](std::vector<VkSemaphore> semaphores, VulkanDeviceQueue* device_queue,
         bool /* is_lost */) {
        for (VkSemaphore semaphore : semaphores) {
          vkDestroySemaphore(device_queue->GetVulkanDevice(), semaphore, nullptr);
        }
      },
      std::move(semaphores)));
}

void VulkanFenceHelper::EnqueueImageCleanupForSubmittedWork(VkImage image, VkDeviceMemory memory) 
{
  if (image == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) return;

  EnqueueCleanupTaskForSubmittedWork(base::BindOnce([](VkImage image, VkDeviceMemory memory, VulkanDeviceQueue* device_queue, bool /* is_lost */) 
  {
        if (image != VK_NULL_HANDLE) vkDestroyImage(device_queue->GetVulkanDevice(), image, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device_queue->GetVulkanDevice(), memory, nullptr);
      }, image, memory));
}

void VulkanFenceHelper::EnqueueBufferCleanupForSubmittedWork( VkBuffer buffer, VmaAllocation allocation) 
{
  if (buffer == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE) return;

  DCHECK(buffer != VK_NULL_HANDLE);
  DCHECK(allocation != VK_NULL_HANDLE);

  EnqueueCleanupTaskForSubmittedWork(base::BindOnce([](VkBuffer buffer, VmaAllocation allocation, VulkanDeviceQueue* device_queue, bool /* is_lost */) 
  {
        vma::DestroyBuffer(device_queue->vma_allocator(), buffer, allocation);
      },buffer, allocation));
}

void VulkanFenceHelper::PerformImmediateCleanup() {
  if (cleanup_tasks_.empty() && tasks_pending_fence_.empty())return;

  VkResult result = vkQueueWaitIdle(device_queue_->GetVulkanQueue());

  CHECK(result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST);
  bool device_lost = result == VK_ERROR_DEVICE_LOST;

  current_generation_ = next_generation_ - 1;

  std::vector<CleanupTask> tasks_to_run;
  
  while (!cleanup_tasks_.empty()) 
  {
    auto& tasks_for_fence = cleanup_tasks_.front();
    vkDestroyFence(device_queue_->GetVulkanDevice(), tasks_for_fence.fence, nullptr);
    tasks_to_run.insert(tasks_to_run.end(), std::make_move_iterator(tasks_for_fence.tasks.begin()), std::make_move_iterator(tasks_for_fence.tasks.end()));
    cleanup_tasks_.pop_front();
  }
  tasks_to_run.insert(tasks_to_run.end(), std::make_move_iterator(tasks_pending_fence_.begin()), std::make_move_iterator(tasks_pending_fence_.end()));
  tasks_pending_fence_.clear();
  for (auto& task : tasks_to_run) std::move(task).Run(device_queue_.get(), device_lost);
}

VulkanFenceHelper::TasksForFence::TasksForFence(FenceHandle handle, std::vector<CleanupTask> tasks) : fence(handle.fence_),
 generation_id(handle.generation_id_), tasks(std::move(tasks)) {}
VulkanFenceHelper::TasksForFence::TasksForFence(uint64_t generation_id, std::vector<CleanupTask> tasks) : generation_id(generation_id), tasks(std::move(tasks)) {}
VulkanFenceHelper::TasksForFence::~TasksForFence() = default;
VulkanFenceHelper::TasksForFence::TasksForFence(TasksForFence&& other) = default;
}  
