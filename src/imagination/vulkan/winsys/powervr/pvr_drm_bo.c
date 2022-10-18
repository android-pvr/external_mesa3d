/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <vulkan/vulkan.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm-uapi/pvr_drm.h"
#include "pvr_drm.h"
#include "pvr_drm_bo.h"
#include "pvr_private.h"
#include "pvr_winsys_helper.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_log.h"

static int pvr_drm_create_gem_bo(struct pvr_drm_winsys *drm_ws,
                                 uint32_t drm_flags,
                                 uint64_t *const size_out,
                                 uint32_t *const handle_out)
{
   struct drm_pvr_ioctl_create_bo_args args = {
      .size = *size_out,
      .flags = drm_flags,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_BO, &args);
   if (ret)
      return -errno;

   *size_out = args.size;
   *handle_out = args.handle;

   return 0;
}

static int pvr_drm_destroy_gem_bo(struct pvr_drm_winsys *drm_ws,
                                  uint32_t handle)
{
   struct drm_gem_close args = {
      .handle = handle,
   };
   int ret;

   /* The kernel driver doesn't have a corresponding DRM_IOCTL_PVR_DESTROY_BO
    * IOCTL as DRM provides a common IOCTL for doing this.
    */
   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_GEM_CLOSE, &args);
   if (ret)
      return -errno;

   return 0;
}

static int pvr_drm_get_bo_mmap_offset(struct pvr_drm_winsys *drm_ws,
                                      uint32_t handle,
                                      uint64_t *const offset_out)
{
   struct drm_pvr_ioctl_get_bo_mmap_offset_args args = {
      .handle = handle,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &args);
   if (ret)
      return -errno;

   *offset_out = args.offset;

   return 0;
}

static void pvr_drm_buffer_acquire(struct pvr_drm_winsys_bo *drm_bo)
{
   p_atomic_inc(&drm_bo->ref_count);
}

static void pvr_drm_buffer_release(struct pvr_drm_winsys_bo *drm_bo)
{
   if (p_atomic_dec_return(&drm_bo->ref_count) == 0) {
      struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(drm_bo->base.ws);
      int ret;

      ret = pvr_drm_destroy_gem_bo(drm_ws, drm_bo->handle);
      if (ret)
         mesa_logw("Failed to destroy gem bo (err=%d)", ret);

      vk_free(drm_ws->alloc, drm_bo);
   }
}

static VkResult
pvr_drm_display_buffer_create(struct pvr_drm_winsys *drm_ws,
                              uint64_t size,
                              struct pvr_winsys_bo **const bo_out)
{
   uint32_t handle;
   VkResult result;
   int fd;
   int ret;

   ret =
      pvr_winsys_helper_display_buffer_create(drm_ws->master_fd, size, &handle);
   if (ret)
      return vk_error(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   ret = drmPrimeHandleToFD(drm_ws->master_fd, handle, DRM_CLOEXEC, &fd);
   pvr_winsys_helper_display_buffer_destroy(drm_ws->master_fd, handle);
   if (ret)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = pvr_drm_winsys_buffer_create_from_fd(&drm_ws->base, fd, bo_out);
   close(fd);
   if (result != VK_SUCCESS)
      return result;

   assert((*bo_out)->size >= size);

   return VK_SUCCESS;
}

static uint64_t pvr_drm_get_alloc_flags(uint32_t ws_flags)
{
   uint64_t drm_flags = 0U;

   if (ws_flags & PVR_WINSYS_BO_FLAG_GPU_UNCACHED)
      drm_flags |= DRM_PVR_BO_DEVICE_BYPASS_CACHE;

   if (ws_flags & PVR_WINSYS_BO_FLAG_PM_FW_PROTECT)
      drm_flags |= DRM_PVR_BO_DEVICE_PM_FW_PROTECT;

   if (ws_flags & PVR_WINSYS_BO_FLAG_CPU_ACCESS)
      drm_flags |= DRM_PVR_BO_CPU_ALLOW_USERSPACE_ACCESS;

   if (ws_flags & PVR_WINSYS_BO_FLAG_ZERO_ON_ALLOC)
      drm_flags |= DRM_PVR_BO_CREATE_ZEROED;

   return drm_flags;
}

VkResult pvr_drm_winsys_buffer_create(struct pvr_winsys *ws,
                                      uint64_t size,
                                      uint64_t alignment,
                                      enum pvr_winsys_bo_type type,
                                      uint32_t ws_flags,
                                      struct pvr_winsys_bo **const bo_out)
{
   const uint64_t drm_flags = pvr_drm_get_alloc_flags(ws_flags);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct pvr_drm_winsys_bo *drm_bo;
   uint32_t handle = 0;
   VkResult result;
   int ret;

   assert(util_is_power_of_two_nonzero(alignment));
   size = ALIGN_POT(size, alignment);

   if (type == PVR_WINSYS_BO_TYPE_DISPLAY)
      return pvr_drm_display_buffer_create(drm_ws, size, bo_out);

   drm_bo = vk_zalloc(drm_ws->alloc,
                      sizeof(*drm_bo),
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_bo)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   ret = pvr_drm_create_gem_bo(drm_ws, drm_flags, &size, &handle);
   if (ret) {
      result = vk_error(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_vk_free_drm_bo;
   }

   drm_bo->base.size = size;
   drm_bo->base.ws = ws;
   drm_bo->handle = handle;
   drm_bo->flags = drm_flags;

   p_atomic_set(&drm_bo->ref_count, 1);

   *bo_out = &drm_bo->base;

   return VK_SUCCESS;

err_vk_free_drm_bo:
   vk_free(drm_ws->alloc, drm_bo);

   return result;
}

VkResult
pvr_drm_winsys_buffer_create_from_fd(struct pvr_winsys *ws,
                                     int fd,
                                     struct pvr_winsys_bo **const bo_out)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct pvr_drm_winsys_bo *drm_bo;
   uint32_t handle;
   VkResult result;
   off_t size;
   int ret;

   drm_bo = vk_zalloc(drm_ws->alloc,
                      sizeof(*drm_bo),
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_bo)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   size = lseek(fd, 0, SEEK_END);
   if (size == (off_t)-1) {
      result = vk_error(NULL, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      goto err_vk_free_drm_bo;
   }

   ret = drmPrimeFDToHandle(drm_ws->render_fd, fd, &handle);
   if (ret) {
      result = vk_error(NULL, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      goto err_vk_free_drm_bo;
   }

   drm_bo->base.ws = ws;
   drm_bo->base.size = (uint64_t)size;
   drm_bo->base.is_imported = true;
   drm_bo->handle = handle;

   p_atomic_set(&drm_bo->ref_count, 1);

   *bo_out = &drm_bo->base;

   return VK_SUCCESS;

err_vk_free_drm_bo:
   vk_free(drm_ws->alloc, drm_bo);

   return result;
}

void pvr_drm_winsys_buffer_destroy(struct pvr_winsys_bo *bo)
{
   struct pvr_drm_winsys_bo *drm_bo = to_pvr_drm_winsys_bo(bo);

   pvr_drm_buffer_release(drm_bo);
}

VkResult pvr_drm_winsys_buffer_get_fd(struct pvr_winsys_bo *bo,
                                      int *const fd_out)
{
   struct pvr_drm_winsys_bo *drm_bo = to_pvr_drm_winsys_bo(bo);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(bo->ws);
   int ret;

   ret = drmPrimeHandleToFD(drm_ws->render_fd,
                            drm_bo->handle,
                            DRM_CLOEXEC,
                            fd_out);
   if (ret)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   return VK_SUCCESS;
}

void *pvr_drm_winsys_buffer_map(struct pvr_winsys_bo *bo)
{
   struct pvr_drm_winsys_bo *drm_bo = to_pvr_drm_winsys_bo(bo);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(bo->ws);
   uint64_t offset = 0;
   void *map;
   int ret;

   assert(!bo->map);

   ret = pvr_drm_get_bo_mmap_offset(drm_ws, drm_bo->handle, &offset);
   if (ret) {
      vk_error(NULL, VK_ERROR_MEMORY_MAP_FAILED);
      return NULL;
   }

   map = mmap(NULL,
              bo->size,
              PROT_READ | PROT_WRITE,
              MAP_SHARED,
              drm_ws->render_fd,
              offset);
   if (map == MAP_FAILED) {
      vk_error(NULL, VK_ERROR_MEMORY_MAP_FAILED);
      return NULL;
   }

   VG(VALGRIND_MALLOCLIKE_BLOCK(map,
                                bo->size,
                                0,
                                drm_bo->flags & DRM_PVR_BO_CREATE_ZEROED));

   pvr_drm_buffer_acquire(drm_bo);
   bo->map = map;

   return map;
}

void pvr_drm_winsys_buffer_unmap(struct pvr_winsys_bo *bo)
{
   struct pvr_drm_winsys_bo *drm_bo = to_pvr_drm_winsys_bo(bo);

   assert(bo->map);

   if (munmap(bo->map, bo->size))
      vk_error(NULL, VK_ERROR_UNKNOWN);

   VG(VALGRIND_FREELIKE_BLOCK(bo->map, 0));

   bo->map = NULL;

   pvr_drm_buffer_release(drm_bo);
}

/* This function must be used to allocate inside reserved region and must be
 * used internally only. This also means whoever is using it, must know what
 * they are doing.
 */
struct pvr_winsys_vma *
pvr_drm_heap_alloc_reserved(struct pvr_winsys_heap *const heap,
                            const pvr_dev_addr_t reserved_dev_addr,
                            uint64_t size,
                            uint64_t alignment)
{
   const struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(heap->ws);
   struct pvr_drm_winsys_vma *drm_vma;

   assert(util_is_power_of_two_nonzero(alignment));

   drm_vma = vk_zalloc(drm_ws->alloc,
                       sizeof(*drm_vma),
                       8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_vma) {
      vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   /* The powervr kernel mode driver returns a page aligned size when
    * allocating buffers.
    */
   alignment = MAX2(alignment, heap->page_size);
   size = ALIGN_POT(size, alignment);

   /* TODO: Should we keep track of the reserved allocations? */

   drm_vma->base.dev_addr = reserved_dev_addr;
   drm_vma->base.heap = heap;
   drm_vma->base.size = size;

   p_atomic_inc(&heap->ref_count);

   return &drm_vma->base;
}

struct pvr_winsys_vma *pvr_drm_winsys_heap_alloc(struct pvr_winsys_heap *heap,
                                                 uint64_t size,
                                                 uint64_t alignment)
{
   const struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(heap->ws);
   struct pvr_drm_winsys_vma *drm_vma;
   bool ret;

   drm_vma = vk_alloc(drm_ws->alloc,
                      sizeof(*drm_vma),
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_vma) {
      vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   ret = pvr_winsys_helper_heap_alloc(heap, size, alignment, &drm_vma->base);
   if (!ret) {
      vk_free(drm_ws->alloc, drm_vma);
      return NULL;
   }

   return &drm_vma->base;
}

void pvr_drm_winsys_heap_free(struct pvr_winsys_vma *vma)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(vma->heap->ws);
   struct pvr_drm_winsys_vma *drm_vma = to_pvr_drm_winsys_vma(vma);
   const uint64_t carveout_addr = vma->heap->static_data_carveout_addr.addr;

   /* A vma with an existing device mapping should not be freed. */
   assert(!drm_vma->base.bo);

   /* Check if we are dealing with carveout address range. */
   if (vma->dev_addr.addr >= carveout_addr &&
       vma->dev_addr.addr <
          (carveout_addr + vma->heap->static_data_carveout_size)) {
      /* For the reserved addresses just decrement the reference count. */
      p_atomic_dec(&vma->heap->ref_count);
   } else {
      /* Free allocated virtual space. */
      pvr_winsys_helper_heap_free(vma);
   }

   vk_free(drm_ws->alloc, drm_vma);
}

pvr_dev_addr_t pvr_drm_winsys_vma_map(struct pvr_winsys_vma *vma,
                                      struct pvr_winsys_bo *bo,
                                      uint64_t offset,
                                      uint64_t size)
{
   struct pvr_drm_winsys_bo *const drm_bo = to_pvr_drm_winsys_bo(bo);
   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(bo->ws);
   const uint32_t virt_offset = offset & (vma->heap->page_size - 1);
   const uint64_t aligned_virt_size =
      ALIGN_POT(virt_offset + size, vma->heap->page_size);
   const uint32_t phys_page_offset = offset - virt_offset;

   struct drm_pvr_ioctl_vm_map_args args = { .device_addr = vma->dev_addr.addr,
                                             .flags = 0U,
                                             .handle = drm_bo->handle,
                                             .offset = phys_page_offset,
                                             .size = aligned_virt_size };

   /* Address should not be mapped already. */
   assert(!vma->bo);

   /* Check if bo and vma can accommodate the given size and offset. */
   if (ALIGN_POT(offset + size, vma->heap->page_size) > bo->size ||
       aligned_virt_size > vma->size) {
      vk_error(NULL, VK_ERROR_MEMORY_MAP_FAILED);
      return PVR_DEV_ADDR_INVALID;
   }

   if (drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_VM_MAP, &args)) {
      vk_error(NULL, VK_ERROR_MEMORY_MAP_FAILED);
      return PVR_DEV_ADDR_INVALID;
   }

   pvr_drm_buffer_acquire(drm_bo);

   vma->bo = &drm_bo->base;
   vma->bo_offset = offset;
   vma->mapped_size = aligned_virt_size;

   return (pvr_dev_addr_t){ .addr = vma->dev_addr.addr + virt_offset };
}

void pvr_drm_winsys_vma_unmap(struct pvr_winsys_vma *vma)
{
   struct pvr_drm_winsys_bo *const drm_bo = to_pvr_drm_winsys_bo(vma->bo);
   struct pvr_drm_winsys_vma *const drm_vma = to_pvr_drm_winsys_vma(vma);
   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(vma->bo->ws);

   struct drm_pvr_ioctl_vm_unmap_args args = { .device_addr =
                                                  vma->dev_addr.addr };

   /* Address should be mapped. */
   assert(drm_vma->base.bo);

   if (drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_VM_UNMAP, &args))
      vk_errorf(NULL, VK_ERROR_UNKNOWN, "Unmap failed");

   pvr_drm_buffer_release(drm_bo);

   drm_vma->base.bo = NULL;
}
