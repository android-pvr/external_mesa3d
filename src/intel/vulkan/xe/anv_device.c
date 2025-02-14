/*
 * Copyright © 2023 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "xe/anv_device.h"
#include "anv_private.h"

#include "drm-uapi/gpu_scheduler.h"
#include "drm-uapi/xe_drm.h"

bool anv_xe_device_destroy_vm(struct anv_device *device)
{
   struct drm_xe_vm_destroy destroy = {
      .vm_id = device->vm_id,
   };
   return intel_ioctl(device->fd, DRM_IOCTL_XE_VM_DESTROY, &destroy) == 0;
}

VkResult anv_xe_device_setup_vm(struct anv_device *device)
{
   struct drm_xe_vm_create create = {
      .flags = DRM_XE_VM_CREATE_SCRATCH_PAGE,
   };
   if (intel_ioctl(device->fd, DRM_IOCTL_XE_VM_CREATE, &create) != 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "vm creation failed");

   device->vm_id = create.vm_id;
   return VK_SUCCESS;
}

static VkQueueGlobalPriorityKHR
drm_sched_priority_to_vk_priority(enum drm_sched_priority drm_sched_priority)
{
   switch (drm_sched_priority) {
   case DRM_SCHED_PRIORITY_MIN:
      return VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
   case DRM_SCHED_PRIORITY_NORMAL:
      return VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
   case DRM_SCHED_PRIORITY_HIGH:
      return VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
   default:
      unreachable("Invalid drm_sched_priority");
      return VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
   }
}

static void *
xe_query_alloc_fetch(struct anv_physical_device *device, uint32_t query_id)
{
   struct drm_xe_device_query query = {
      .query = query_id,
   };
   if (intel_ioctl(device->local_fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
      return NULL;

   void *data = calloc(1, query.size);
   if (!data)
      return NULL;

   query.data = (uintptr_t)data;
   if (intel_ioctl(device->local_fd, DRM_IOCTL_XE_DEVICE_QUERY, &query)) {
      free(data);
      return NULL;
   }

   return data;
}

VkResult
anv_xe_physical_device_get_parameters(struct anv_physical_device *device)
{
   struct drm_xe_query_config *config;

   config = xe_query_alloc_fetch(device, DRM_XE_DEVICE_QUERY_CONFIG);
   if (!config)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "unable to query device config");

   device->has_exec_timeline = true;
   device->max_context_priority =
         drm_sched_priority_to_vk_priority(config->info[XE_QUERY_CONFIG_MAX_ENGINE_PRIORITY]);

   free(config);
   return VK_SUCCESS;
}

VkResult
anv_xe_physical_device_init_memory_types(struct anv_physical_device *device)
{
   if (anv_physical_device_has_vram(device)) {
      device->memory.type_count = 3;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
         .heapIndex = 0,
      };
      device->memory.types[1] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 1,
      };
      device->memory.types[2] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         /* This memory type either comes from heaps[0] if there is only
          * mappable vram region, or from heaps[2] if there is both mappable &
          * non-mappable vram regions.
          */
         .heapIndex = device->vram_non_mappable.size > 0 ? 2 : 0,
      };
   } else if (device->info.has_llc) {
      /* Big core GPUs share LLC with the CPU and thus one memory type can be
       * both cached and coherent at the same time.
       *
       * But some game engines can't handle single type well
       * https://gitlab.freedesktop.org/mesa/mesa/-/issues/7360#note_1719438
       *
       * TODO: But with current UAPI we can't change the mmap mode in Xe, so
       * here only supporting two memory types.
       */
      device->memory.type_count = 2;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
         .heapIndex = 0,
      };
      device->memory.types[1] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
   } else {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "No memory heaps types set for non llc devices yet on Xe");
   }
   return VK_SUCCESS;
}

VkResult
anv_xe_device_check_status(struct vk_device *vk_device)
{
   struct anv_device *device = container_of(vk_device, struct anv_device, vk);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < device->queue_count; i++) {
      struct drm_xe_engine_get_property engine_get_property = {
         .engine_id = device->queues[i].engine_id,
         .property = XE_ENGINE_GET_PROPERTY_BAN,
      };
      int ret = intel_ioctl(device->fd, DRM_IOCTL_XE_ENGINE_GET_PROPERTY,
                            &engine_get_property);

      if (ret || engine_get_property.value) {
         result = vk_device_set_lost(&device->vk, "One or more queues banned");
         break;
      }
   }

   return result;
}
