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
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#include "drm-uapi/pvr_drm.h"
#include "pvr_device_info.h"
#include "pvr_drm.h"
#include "pvr_drm_bo.h"
#include "pvr_drm_job_compute.h"
#include "pvr_drm_job_null.h"
#include "pvr_drm_job_render.h"
#include "pvr_drm_job_transfer.h"
#include "pvr_drm_public.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "pvr_winsys_helper.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"

static int pvr_drm_get_param(struct pvr_drm_winsys *drm_ws,
                             enum drm_pvr_param param,
                             uint64_t *const value_out)
{
   struct drm_pvr_ioctl_get_param_args args = {
      .param = param,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_GET_PARAM, &args);
   if (ret)
      return -errno;

   *value_out = args.value;

   return 0;
}

static void pvr_drm_finish_heaps(struct pvr_drm_winsys *const drm_ws)
{
   if (!pvr_winsys_helper_winsys_heap_finish(&drm_ws->vis_test_heap.base)) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Visibility test heap in use, can't deinit");
   }

   if (drm_ws->rgn_hdr_heap_present) {
      if (!pvr_winsys_helper_winsys_heap_finish(&drm_ws->rgn_hdr_heap.base)) {
         vk_errorf(NULL,
                   VK_ERROR_UNKNOWN,
                   "Region header heap in use, can't deinit");
      }
   }

   if (!pvr_winsys_helper_winsys_heap_finish(&drm_ws->usc_heap.base))
      vk_errorf(NULL, VK_ERROR_UNKNOWN, "USC heap in use, can't deinit");

   if (!pvr_winsys_helper_winsys_heap_finish(&drm_ws->pds_heap.base))
      vk_errorf(NULL, VK_ERROR_UNKNOWN, "PDS heap in use, can't deinit");

   if (!pvr_winsys_helper_winsys_heap_finish(&drm_ws->general_heap.base))
      vk_errorf(NULL, VK_ERROR_UNKNOWN, "General heap in use, can't deinit");
}

static void pvr_drm_winsys_destroy(struct pvr_winsys *ws)
{
   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(ws);

   pvr_winsys_helper_free_static_memory(drm_ws->general_vma,
                                        drm_ws->pds_vma,
                                        drm_ws->usc_vma);

   pvr_drm_finish_heaps(drm_ws);

   vk_free(drm_ws->alloc, drm_ws);
}

static int pvr_drm_override_quirks(struct pvr_drm_winsys *drm_ws,
                                   struct pvr_device_info *dev_info)
{
   uint64_t quirks_musthave0 = 0;
   uint64_t quirks0 = 0;
   int ret;

   ret = pvr_drm_get_param(drm_ws, DRM_PVR_PARAM_QUIRKS0, &quirks0);
   if (ret)
      goto err_out;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_QUIRKS_MUSTHAVE0,
                           &quirks_musthave0);
   if (ret)
      goto err_out;

#define PVR_QUIRK_MUSTHAVE_CHECK_SET(quirks_x, musthave_x, number)        \
   do {                                                                   \
      if ((musthave_x & DRM_PVR_QUIRK_MASK(DRM_PVR_QUIRK_BRN##number)) && \
          dev_info->quirks.has_brn##number) {                             \
         musthave_x &= ~DRM_PVR_QUIRK_MASK(DRM_PVR_QUIRK_BRN##number);    \
      }                                                                   \
      dev_info->quirks.has_brn##number =                                  \
         quirks_x & DRM_PVR_QUIRK_MASK(DRM_PVR_QUIRK_BRN##number);        \
   } while (0)

   /*
    * For each quirk, check that if it is a "must have" that it is set in
    * dev_info, then set the dev_info value to the one received from the kernel.
    */
   PVR_QUIRK_MUSTHAVE_CHECK_SET(quirks0, quirks_musthave0, 48545);
   PVR_QUIRK_MUSTHAVE_CHECK_SET(quirks0, quirks_musthave0, 49927);
   PVR_QUIRK_MUSTHAVE_CHECK_SET(quirks0, quirks_musthave0, 51764);
   PVR_QUIRK_MUSTHAVE_CHECK_SET(quirks0, quirks_musthave0, 62269);

#undef PVR_QUIRK_MUSTHAVE_CHECK_SET

   /* If any "must have" quirks are not supported then fail. */
   if (quirks_musthave0) {
      ret = -ENODEV;
      goto err_out;
   }

err_out:
   return ret;
}

static int pvr_drm_override_enhancements(struct pvr_drm_winsys *drm_ws,
                                         struct pvr_device_info *dev_info)
{
   uint64_t enhancements0 = 0;
   int ret;

   ret = pvr_drm_get_param(drm_ws, DRM_PVR_PARAM_ENHANCEMENTS0, &enhancements0);
   if (ret)
      goto err_out;

/* clang-format off */
#define PVR_ENHANCEMENT_SET(enhancements_x, number) \
   dev_info->enhancements.has_ern##number =         \
      enhancements_x & DRM_PVR_ENHANCEMENT_MASK(DRM_PVR_ENHANCEMENT_ERN##number)
   /* clang-format on */

   PVR_ENHANCEMENT_SET(enhancements0, 35421);

#undef PVR_ENHANCEMENT_SET

err_out:
   return ret;
}

static int
pvr_drm_winsys_device_info_init(struct pvr_winsys *ws,
                                struct pvr_device_info *dev_info,
                                struct pvr_device_runtime_info *runtime_info)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   uint64_t min_free_list_pages = 0;
   int ret;

   ret = pvr_device_info_init(dev_info, drm_ws->bvnc);
   if (ret) {
      mesa_logw("Unsupported BVNC: %u.%u.%u.%u\n",
                PVR_BVNC_UNPACK_B(drm_ws->bvnc),
                PVR_BVNC_UNPACK_V(drm_ws->bvnc),
                PVR_BVNC_UNPACK_N(drm_ws->bvnc),
                PVR_BVNC_UNPACK_C(drm_ws->bvnc));
      return ret;
   }

   ret = pvr_drm_override_quirks(drm_ws, dev_info);
   if (ret) {
      mesa_logw("Failed to get quirks for this GPU\n");
      return ret;
   }

   ret = pvr_drm_override_enhancements(drm_ws, dev_info);
   if (ret) {
      mesa_logw("Failed to get enhancements for this GPU\n");
      return ret;
   }

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_FREE_LIST_MIN_PAGES,
                           &min_free_list_pages);
   if (ret)
      return ret;

   runtime_info->min_free_list_size = min_free_list_pages
                                      << ROGUE_BIF_PM_PHYSICAL_PAGE_SHIFT;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_COMMON_STORE_ALLOC_REGION_SIZE,
                           &runtime_info->reserved_shared_size);
   if (ret)
      return ret;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_COMMON_STORE_PARTITION_SPACE_SIZE,
                           &runtime_info->total_reserved_partition_size);
   if (ret)
      return ret;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_NUM_PHANTOMS,
                           &runtime_info->num_phantoms);
   if (ret)
      return ret;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_MAX_COEFFS,
                           &runtime_info->max_coeffs);
   if (ret)
      return ret;

   ret = pvr_drm_get_param(drm_ws,
                           DRM_PVR_PARAM_CDM_MAX_LOCAL_MEM_SIZE_REGS,
                           &runtime_info->cdm_max_local_mem_size_regs);
   if (ret)
      return ret;

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support)) {
      /* TODO: When kernel support is added, fetch the actual core count. */
      mesa_logw("Core count fetching is unimplemented. Setting 1 for now.");
      runtime_info->core_count = 1;
   } else {
      runtime_info->core_count = 1;
   }

   return 0;
}

static void pvr_drm_winsys_get_heaps_info(struct pvr_winsys *ws,
                                          struct pvr_winsys_heaps *heaps)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);

   heaps->general_heap = &drm_ws->general_heap.base;
   heaps->pds_heap = &drm_ws->pds_heap.base;
   heaps->usc_heap = &drm_ws->usc_heap.base;
   heaps->vis_test_heap = &drm_ws->vis_test_heap.base;

   if (drm_ws->rgn_hdr_heap_present)
      heaps->rgn_hdr_heap = &drm_ws->rgn_hdr_heap.base;
   else
      heaps->rgn_hdr_heap = &drm_ws->general_heap.base;
}

static const struct pvr_winsys_ops drm_winsys_ops = {
   .destroy = pvr_drm_winsys_destroy,
   .device_info_init = pvr_drm_winsys_device_info_init,
   .get_heaps_info = pvr_drm_winsys_get_heaps_info,
   .buffer_create = pvr_drm_winsys_buffer_create,
   .buffer_create_from_fd = pvr_drm_winsys_buffer_create_from_fd,
   .buffer_destroy = pvr_drm_winsys_buffer_destroy,
   .buffer_get_fd = pvr_drm_winsys_buffer_get_fd,
   .buffer_map = pvr_drm_winsys_buffer_map,
   .buffer_unmap = pvr_drm_winsys_buffer_unmap,
   .heap_alloc = pvr_drm_winsys_heap_alloc,
   .heap_free = pvr_drm_winsys_heap_free,
   .vma_map = pvr_drm_winsys_vma_map,
   .vma_unmap = pvr_drm_winsys_vma_unmap,
   .free_list_create = pvr_drm_winsys_free_list_create,
   .free_list_destroy = pvr_drm_winsys_free_list_destroy,
   .render_target_dataset_create = pvr_drm_render_target_dataset_create,
   .render_target_dataset_destroy = pvr_drm_render_target_dataset_destroy,
   .render_ctx_create = pvr_drm_winsys_render_ctx_create,
   .render_ctx_destroy = pvr_drm_winsys_render_ctx_destroy,
   .render_submit = pvr_drm_winsys_render_submit,
   .compute_ctx_create = pvr_drm_winsys_compute_ctx_create,
   .compute_ctx_destroy = pvr_drm_winsys_compute_ctx_destroy,
   .compute_submit = pvr_drm_winsys_compute_submit,
   .transfer_ctx_create = pvr_drm_winsys_transfer_ctx_create,
   .transfer_ctx_destroy = pvr_drm_winsys_transfer_ctx_destroy,
   .null_job_submit = pvr_drm_winsys_null_job_submit,
};

static VkResult pvr_drm_get_heap_static_data_offsets(
   struct pvr_drm_winsys *const drm_ws,
   uint32_t heap_index,
   uint32_t static_areas_count,
   struct pvr_winsys_static_data_offsets *const offsets_out)
{
   struct drm_pvr_ioctl_get_heap_info_args args = {
      .op = DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS,
      .heap_nr = heap_index,
   };
   struct drm_pvr_static_data_area *static_data_args_arr;
   int ret;

   *offsets_out = (struct pvr_winsys_static_data_offsets){ 0 };

   static_data_args_arr =
      vk_alloc(drm_ws->alloc,
               sizeof(*static_data_args_arr) * static_areas_count,
               8,
               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!static_data_args_arr)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   args.data = (__u64)static_data_args_arr;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_GET_HEAP_INFO, &args);
   if (ret) {
      vk_free(drm_ws->alloc, static_data_args_arr);
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to fetch static area offsets. Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   VG(VALGRIND_MAKE_MEM_DEFINED(static_data_args_arr,
                                sizeof(*static_data_args_arr) *
                                   static_areas_count));

   for (uint32_t i = 0; i < static_areas_count; i++) {
      switch (static_data_args_arr[i].id) {
      case DRM_PVR_STATIC_DATA_AREA_EOT:
         offsets_out->eot = static_data_args_arr[i].offset;
         break;

      case DRM_PVR_STATIC_DATA_AREA_FENCE:
         offsets_out->fence = static_data_args_arr[i].offset;
         break;

      case DRM_PVR_STATIC_DATA_AREA_VDM_SYNC:
         offsets_out->vdm_sync = static_data_args_arr[i].offset;
         break;

      case DRM_PVR_STATIC_DATA_AREA_YUV_CSC:
         offsets_out->yuv_csc = static_data_args_arr[i].offset;
         break;

      default:
         mesa_logd("Unknown drm static area id. ID: %d.",
                   static_data_args_arr[i].id);
         continue;
      }
   }

   vk_free(drm_ws->alloc, static_data_args_arr);

   return VK_SUCCESS;
}

static VkResult pvr_drm_setup_heaps(struct pvr_drm_winsys *const drm_ws)
{
   struct drm_pvr_ioctl_get_heap_info_args args = {
      .op = DRM_PVR_HEAP_OP_GET_HEAP_INFO,
   };

   bool vis_test_heap_present = false;
   bool general_heap_present = false;
   bool pds_heap_present = false;
   bool usc_heap_present = false;
   struct drm_pvr_heap heap_info;
   uint64_t num_heaps = 0;
   VkResult result;
   int ret;

   /* First, get the number of heaps. */
   ret = pvr_drm_get_param(drm_ws, DRM_PVR_PARAM_NUM_HEAPS, &num_heaps);
   if (ret) {
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to fetch number of heaps. Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   /* Set optional heap present flags to false. */
   drm_ws->rgn_hdr_heap_present = false;

   /* Now get the information for each heap. */
   args.data = (__u64)&heap_info;
   for (uint32_t i = 0; i < (uint32_t)num_heaps; i++) {
      struct pvr_winsys_static_data_offsets static_data_offsets;
      struct pvr_drm_winsys_heap *drm_heap;
      bool *heap_present_ptr;

      args.heap_nr = i;

      ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_GET_HEAP_INFO, &args);
      if (ret) {
         result = vk_errorf(NULL,
                            VK_ERROR_INITIALIZATION_FAILED,
                            "Failed to fetch heap info. Errno: %d - %s.",
                            errno,
                            strerror(errno));

         goto err_pvr_drm_heap_finish_all_heaps;
      }

      VG(VALGRIND_MAKE_MEM_DEFINED(&heap_info, sizeof(heap_info)));

      switch (heap_info.id) {
      case DRM_PVR_HEAP_GENERAL:
         assert(!general_heap_present);

         heap_present_ptr = &general_heap_present;
         drm_heap = &drm_ws->general_heap;
         break;

      case DRM_PVR_HEAP_PDS_CODE_DATA:
         assert(!pds_heap_present);

         heap_present_ptr = &pds_heap_present;
         drm_heap = &drm_ws->pds_heap;
         break;

      case DRM_PVR_HEAP_USC_CODE:
         assert(!usc_heap_present);

         heap_present_ptr = &usc_heap_present;
         drm_heap = &drm_ws->usc_heap;
         break;

      case DRM_PVR_HEAP_RGNHDR:
         assert(!drm_ws->rgn_hdr_heap_present);

         heap_present_ptr = &drm_ws->rgn_hdr_heap_present;
         drm_heap = &drm_ws->rgn_hdr_heap;
         break;

      case DRM_PVR_HEAP_VIS_TEST:
         assert(!vis_test_heap_present);

         heap_present_ptr = &vis_test_heap_present;
         drm_heap = &drm_ws->vis_test_heap;
         break;

      default:
         mesa_logd("Unknown heap id received. Ignoring it.");
         continue;
      }

      if (heap_info.nr_static_data_areas) {
         result =
            pvr_drm_get_heap_static_data_offsets(drm_ws,
                                                 args.heap_nr,
                                                 heap_info.nr_static_data_areas,
                                                 &static_data_offsets);
         if (result != VK_SUCCESS)
            goto err_pvr_drm_heap_finish_all_heaps;
      }

      result = pvr_winsys_helper_winsys_heap_init(
         &drm_ws->base,
         (pvr_dev_addr_t){ .addr = heap_info.base },
         heap_info.size,
         (pvr_dev_addr_t){ .addr = heap_info.static_data_carveout_base },
         heap_info.static_data_carveout_size,
         heap_info.page_size_log2,
         &static_data_offsets,
         &drm_heap->base);
      if (result != VK_SUCCESS)
         goto err_pvr_drm_heap_finish_all_heaps;

      *heap_present_ptr = true;

      /* For now we don't support the heap page size being different from the
       * host page size.
       */
      assert(drm_heap->base.page_size == drm_ws->base.page_size);
      assert(drm_heap->base.log2_page_size == drm_ws->base.log2_page_size);
   }

   /* Check that required heaps are present (thus initialized). */
   if (!general_heap_present || !pds_heap_present || !usc_heap_present ||
       !vis_test_heap_present) {
      result = vk_errorf(NULL,
                         VK_ERROR_INITIALIZATION_FAILED,
                         "Some required heaps aren't present.");
      goto err_pvr_drm_heap_finish_all_heaps;
   }

   return VK_SUCCESS;

err_pvr_drm_heap_finish_all_heaps:
   if (vis_test_heap_present)
      pvr_winsys_helper_winsys_heap_finish(&drm_ws->vis_test_heap.base);

   if (drm_ws->rgn_hdr_heap_present)
      pvr_winsys_helper_winsys_heap_finish(&drm_ws->rgn_hdr_heap.base);

   if (usc_heap_present)
      pvr_winsys_helper_winsys_heap_finish(&drm_ws->usc_heap.base);

   if (pds_heap_present)
      pvr_winsys_helper_winsys_heap_finish(&drm_ws->pds_heap.base);

   if (general_heap_present)
      pvr_winsys_helper_winsys_heap_finish(&drm_ws->general_heap.base);

   return result;
}

struct pvr_winsys *pvr_drm_winsys_create(int master_fd,
                                         int render_fd,
                                         const VkAllocationCallbacks *alloc)
{
   struct pvr_drm_winsys *drm_ws;
   VkResult result;
   int ret;

   drm_ws =
      vk_alloc(alloc, sizeof(*drm_ws), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_ws) {
      vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   drm_ws->base.ops = &drm_winsys_ops;
   os_get_page_size(&drm_ws->base.page_size);
   drm_ws->base.log2_page_size = util_logbase2(drm_ws->base.page_size);

   drm_ws->base.syncobj_type = vk_drm_syncobj_get_type(render_fd);
   drm_ws->base.sync_types[0] = &drm_ws->base.syncobj_type;
   drm_ws->base.sync_types[1] = NULL;

   drm_ws->master_fd = master_fd;
   drm_ws->render_fd = render_fd;
   drm_ws->alloc = alloc;

   ret = pvr_drm_get_param(drm_ws, DRM_PVR_PARAM_GPU_ID, &drm_ws->bvnc);
   if (ret) {
      vk_error(NULL, VK_ERROR_INITIALIZATION_FAILED);
      goto err_vk_free_drm_ws;
   }

   result = pvr_drm_setup_heaps(drm_ws);
   if (result != VK_SUCCESS)
      goto err_vk_free_drm_ws;

   result =
      pvr_winsys_helper_allocate_static_memory(&drm_ws->base,
                                               pvr_drm_heap_alloc_reserved,
                                               &drm_ws->general_heap.base,
                                               &drm_ws->pds_heap.base,
                                               &drm_ws->usc_heap.base,
                                               &drm_ws->general_vma,
                                               &drm_ws->pds_vma,
                                               &drm_ws->usc_vma);
   if (result != VK_SUCCESS)
      goto err_pvr_heap_finish;

   result = pvr_winsys_helper_fill_static_memory(&drm_ws->base,
                                                 drm_ws->general_vma,
                                                 drm_ws->pds_vma,
                                                 drm_ws->usc_vma);
   if (result != VK_SUCCESS)
      goto err_pvr_free_static_memory;

   return &drm_ws->base;

err_pvr_free_static_memory:
   pvr_winsys_helper_free_static_memory(drm_ws->general_vma,
                                        drm_ws->pds_vma,
                                        drm_ws->usc_vma);

err_pvr_heap_finish:
   pvr_drm_finish_heaps(drm_ws);

err_vk_free_drm_ws:
   vk_free(alloc, drm_ws);

   return NULL;
}
