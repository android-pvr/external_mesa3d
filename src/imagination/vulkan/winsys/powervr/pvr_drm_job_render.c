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

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#include "drm-uapi/pvr_drm.h"
#include "fw-api/pvr_rogue_fwif_shared.h"
#include "pvr_drm.h"
#include "pvr_drm_bo.h"
#include "pvr_drm_job_common.h"
#include "pvr_drm_job_render.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_util.h"

#define PVR_DRM_FREE_LIST_LOCAL 0U
#define PVR_DRM_FREE_LIST_GLOBAL 1U
#define PVR_DRM_FREE_LIST_MAX 2U

struct pvr_drm_winsys_free_list {
   struct pvr_winsys_free_list base;

   uint32_t handle;

   struct pvr_drm_winsys_free_list *parent;
};

#define to_pvr_drm_winsys_free_list(free_list) \
   container_of(free_list, struct pvr_drm_winsys_free_list, base)

struct pvr_drm_winsys_rt_dataset {
   struct pvr_winsys_rt_dataset base;
   uint32_t handle;
};

#define to_pvr_drm_winsys_rt_dataset(rt_dataset) \
   container_of(rt_dataset, struct pvr_drm_winsys_rt_dataset, base)

VkResult pvr_drm_winsys_free_list_create(
   struct pvr_winsys *const ws,
   struct pvr_winsys_vma *const free_list_vma,
   uint32_t initial_num_pages,
   uint32_t max_num_pages,
   uint32_t grow_num_pages,
   uint32_t grow_threshold,
   struct pvr_winsys_free_list *const parent_free_list,
   struct pvr_winsys_free_list **const free_list_out)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct drm_pvr_ioctl_create_free_list_args free_list_args = {
      .free_list_gpu_addr = free_list_vma->dev_addr.addr,
      .initial_num_pages = initial_num_pages,
      .max_num_pages = max_num_pages,
      .grow_num_pages = grow_num_pages,
      .grow_threshold = grow_threshold,
      .vm_context_handle = drm_ws->vm_context,
   };
   struct pvr_drm_winsys_free_list *drm_free_list;

   drm_free_list = vk_zalloc(drm_ws->alloc,
                             sizeof(*drm_free_list),
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_free_list)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   drm_free_list->base.ws = ws;

   if (parent_free_list)
      drm_free_list->parent = to_pvr_drm_winsys_free_list(parent_free_list);

   if (drmIoctl(drm_ws->render_fd,
                DRM_IOCTL_PVR_CREATE_FREE_LIST,
                &free_list_args)) {
      vk_free(drm_ws->alloc, drm_free_list);

      /* Returns VK_ERROR_INITIALIZATION_FAILED to match pvrsrv. */
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to create free list. Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   drm_free_list->handle = free_list_args.handle;

   *free_list_out = &drm_free_list->base;

   return VK_SUCCESS;
}

void pvr_drm_winsys_free_list_destroy(struct pvr_winsys_free_list *free_list)
{
   struct pvr_drm_winsys_free_list *const drm_free_list =
      to_pvr_drm_winsys_free_list(free_list);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(free_list->ws);
   struct drm_pvr_ioctl_destroy_free_list_args args = {
      .handle = drm_free_list->handle,
   };

   if (drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST, &args)) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Error destroying free list. Errno: %d - %s.",
                errno,
                strerror(errno));
   }

   vk_free(drm_ws->alloc, free_list);
}

static void pvr_drm_render_ctx_static_state_init(
   struct pvr_winsys_render_ctx_create_info *create_info,
   struct rogue_fwif_static_rendercontext_state *static_state)
{
   struct pvr_winsys_render_ctx_static_state *ws_static_state =
      &create_info->static_state;
   struct rogue_fwif_geom_registers_caswitch *geom_regs =
      &static_state->ctxswitch_regs[0];

   memset(static_state, 0, sizeof(*static_state));

   geom_regs->geom_reg_vdm_context_state_base_addr =
      ws_static_state->vdm_ctx_state_base_addr;
   /* geom_reg_vdm_context_state_resume_addr is unused and zeroed. */
   geom_regs->geom_reg_ta_context_state_base_addr =
      ws_static_state->geom_ctx_state_base_addr;

   STATIC_ASSERT(ARRAY_SIZE(geom_regs->geom_state) ==
                 ARRAY_SIZE(ws_static_state->geom_state));
   for (uint32_t i = 0; i < ARRAY_SIZE(ws_static_state->geom_state); i++) {
      geom_regs->geom_state[i].geom_reg_vdm_context_store_task0 =
         ws_static_state->geom_state[i].vdm_ctx_store_task0;
      geom_regs->geom_state[i].geom_reg_vdm_context_store_task1 =
         ws_static_state->geom_state[i].vdm_ctx_store_task1;
      geom_regs->geom_state[i].geom_reg_vdm_context_store_task2 =
         ws_static_state->geom_state[i].vdm_ctx_store_task2;

      geom_regs->geom_state[i].geom_reg_vdm_context_resume_task0 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task0;
      geom_regs->geom_state[i].geom_reg_vdm_context_resume_task1 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task1;
      geom_regs->geom_state[i].geom_reg_vdm_context_resume_task2 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task2;

      /* {store, resume}_task{3, 4} are unused and zeroed. */
   }
}

struct pvr_drm_winsys_render_ctx {
   struct pvr_winsys_render_ctx base;

   /* Handle to kernel context. */
   uint32_t handle;
};

#define to_pvr_drm_winsys_render_ctx(ctx) \
   container_of(ctx, struct pvr_drm_winsys_render_ctx, base)

VkResult pvr_drm_winsys_render_ctx_create(
   struct pvr_winsys *ws,
   struct pvr_winsys_render_ctx_create_info *create_info,
   struct pvr_winsys_render_ctx **const ctx_out)
{
   struct rogue_fwif_static_rendercontext_state static_ctx_state;
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct drm_pvr_ioctl_create_context_args ctx_args = {
      .type = DRM_PVR_CTX_TYPE_RENDER,
      .priority = pvr_drm_from_winsys_priority(create_info->priority),
      .static_context_state = (uint64_t)&static_ctx_state,
      .static_context_state_len = sizeof(static_ctx_state),
      .callstack_addr = create_info->vdm_callstack_addr.addr,
      .vm_context_handle = drm_ws->vm_context,
   };

   struct pvr_drm_winsys_render_ctx *drm_ctx;
   int ret;

   drm_ctx = vk_zalloc(drm_ws->alloc,
                       sizeof(*drm_ctx),
                       8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_ctx)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_drm_render_ctx_static_state_init(create_info, &static_ctx_state);

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx_args);
   if (ret) {
      vk_free(drm_ws->alloc, drm_ctx);
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to create render context, Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   drm_ctx->base.ws = ws;
   drm_ctx->handle = ctx_args.handle;

   *ctx_out = &drm_ctx->base;

   return VK_SUCCESS;
}

void pvr_drm_winsys_render_ctx_destroy(struct pvr_winsys_render_ctx *ctx)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   struct pvr_drm_winsys_render_ctx *drm_ctx =
      to_pvr_drm_winsys_render_ctx(ctx);
   struct drm_pvr_ioctl_destroy_context_args args = {
      .handle = drm_ctx->handle,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_CONTEXT, &args);
   if (ret) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Error destroying render context. Errno: %d - %s.",
                errno,
                strerror(errno));
   }

   vk_free(drm_ws->alloc, drm_ctx);
}

VkResult pvr_drm_render_target_dataset_create(
   struct pvr_winsys *const ws,
   const struct pvr_winsys_rt_dataset_create_info *const create_info,
   UNUSED const struct pvr_device_info *dev_info,
   struct pvr_winsys_rt_dataset **const rt_dataset_out)
{
   struct pvr_drm_winsys_free_list *drm_free_list =
      to_pvr_drm_winsys_free_list(create_info->local_free_list);

   /* 0 is just a placeholder. It doesn't indicate an invalid handle. */
   uint32_t parent_free_list_handle =
      drm_free_list->parent ? drm_free_list->parent->handle : 0;

   struct drm_pvr_ioctl_create_hwrt_dataset_args args = {
      .geom_data_args = {
         .tpc_dev_addr = create_info->tpc_dev_addr.addr,
         .tpc_size = create_info->tpc_size,
         .tpc_stride = create_info->tpc_stride,
         .vheap_table_dev_addr = create_info->vheap_table_dev_addr.addr,
         .rtc_dev_addr = create_info->rtc_dev_addr.addr,
      },

      .rt_data_args = {
         [0] = {
            .pm_mlist_dev_addr =
               create_info->rt_datas[0].pm_mlist_dev_addr.addr,
            .macrotile_array_dev_addr =
               create_info->rt_datas[0].macrotile_array_dev_addr.addr,
            .region_header_dev_addr =
               create_info->rt_datas[0].rgn_header_dev_addr.addr,
         },
         [1] = {
            .pm_mlist_dev_addr =
               create_info->rt_datas[1].pm_mlist_dev_addr.addr,
            .macrotile_array_dev_addr =
               create_info->rt_datas[1].macrotile_array_dev_addr.addr,
            .region_header_dev_addr =
               create_info->rt_datas[1].rgn_header_dev_addr.addr,
         },
      },

      .free_list_handles = {
         [PVR_DRM_FREE_LIST_LOCAL] = drm_free_list->handle,
         [PVR_DRM_FREE_LIST_GLOBAL] = parent_free_list_handle,
      },

      .width = create_info->width,
      .height = create_info->height,
      .samples = create_info->samples,
      .layers = create_info->layers,

      .isp_merge_lower_x = create_info->isp_merge_lower_x,
      .isp_merge_lower_y = create_info->isp_merge_lower_y,
      .isp_merge_scale_x = create_info->isp_merge_scale_x,
      .isp_merge_scale_y = create_info->isp_merge_scale_y,
      .isp_merge_upper_x = create_info->isp_merge_upper_x,
      .isp_merge_upper_y = create_info->isp_merge_upper_y,

      .region_header_size = create_info->rgn_header_size,
   };

   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(ws);
   struct pvr_drm_winsys_rt_dataset *drm_rt_dataset;
   int ret;

   STATIC_ASSERT(ARRAY_SIZE(args.rt_data_args) ==
                 ARRAY_SIZE(create_info->rt_datas));

   drm_rt_dataset = vk_zalloc(drm_ws->alloc,
                              sizeof(*drm_rt_dataset),
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_rt_dataset)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &args);
   if (ret) {
      vk_free(drm_ws->alloc, drm_rt_dataset);

      /* Returns VK_ERROR_INITIALIZATION_FAILED to match pvrsrv. */
      return vk_errorf(
         NULL,
         VK_ERROR_INITIALIZATION_FAILED,
         "Failed to create render target dataset. Errno: %d - %s.",
         errno,
         strerror(errno));
   }

   drm_rt_dataset->handle = args.handle;
   drm_rt_dataset->base.ws = ws;

   *rt_dataset_out = &drm_rt_dataset->base;

   return VK_SUCCESS;
}

void pvr_drm_render_target_dataset_destroy(
   struct pvr_winsys_rt_dataset *const rt_dataset)
{
   struct pvr_drm_winsys_rt_dataset *const drm_rt_dataset =
      to_pvr_drm_winsys_rt_dataset(rt_dataset);
   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(rt_dataset->ws);
   struct drm_pvr_ioctl_destroy_hwrt_dataset_args args = {
      .handle = drm_rt_dataset->handle,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &args);
   if (ret) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Error destroying render target dataset. Errno: %d - %s.",
                errno,
                strerror(errno));
   }

   vk_free(drm_ws->alloc, drm_rt_dataset);
}

static uint32_t pvr_winsys_geom_flags_to_drm(uint32_t ws_flags)
{
   uint32_t flags = 0U;

   if (ws_flags & PVR_WINSYS_GEOM_FLAG_FIRST_GEOMETRY)
      flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST;

   if (ws_flags & PVR_WINSYS_GEOM_FLAG_LAST_GEOMETRY)
      flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST;

   if (ws_flags & PVR_WINSYS_GEOM_FLAG_SINGLE_CORE)
      flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE;

   return flags;
}

static uint32_t pvr_winsys_frag_flags_to_drm(uint32_t ws_flags)
{
   uint32_t flags = 0U;

   if (ws_flags & PVR_WINSYS_FRAG_FLAG_SINGLE_CORE)
      flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE;

   if (ws_flags & PVR_WINSYS_FRAG_FLAG_DEPTH_BUFFER_PRESENT)
      flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER;

   if (ws_flags & PVR_WINSYS_FRAG_FLAG_STENCIL_BUFFER_PRESENT)
      flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER;

   if (ws_flags & PVR_WINSYS_FRAG_FLAG_PREVENT_CDM_OVERLAP)
      flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP;

   if (ws_flags & PVR_WINSYS_FRAG_FLAG_GET_VIS_RESULTS)
      flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS;

   return flags;
}

VkResult pvr_drm_winsys_render_submit(
   const struct pvr_winsys_render_ctx *ctx,
   const struct pvr_winsys_render_submit_info *submit_info,
   UNUSED const struct pvr_device_info *dev_info,
   struct vk_sync *signal_sync_geom,
   struct vk_sync *signal_sync_frag)

{
   const struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   const struct pvr_drm_winsys_render_ctx *drm_ctx =
      to_pvr_drm_winsys_render_ctx(ctx);
   const struct pvr_winsys_geometry_state *const geom_state =
      &submit_info->geometry;
   const struct pvr_winsys_fragment_state *const frag_state =
      &submit_info->fragment;
   const struct pvr_drm_winsys_rt_dataset *drm_rt_dataset =
      to_pvr_drm_winsys_rt_dataset(submit_info->rt_dataset);

   struct drm_pvr_job_render_args job_args = {
      .geom_cmd_stream = (__u64)&geom_state->fw_stream[0],
      .geom_cmd_stream_len = geom_state->fw_stream_len,
      .frag_cmd_stream = (__u64)&frag_state->fw_stream[0],
      .frag_cmd_stream_len = frag_state->fw_stream_len,
      .hwrt_data_set_handle = drm_rt_dataset->handle,
      .hwrt_data_index = submit_info->rt_data_idx,
      .geom_flags = pvr_winsys_geom_flags_to_drm(geom_state->flags),
      .frag_flags = pvr_winsys_frag_flags_to_drm(frag_state->flags),
   };

   struct drm_pvr_ioctl_submit_job_args args = {
      .job_type = DRM_PVR_JOB_TYPE_RENDER,
      .context_handle = drm_ctx->handle,
      .data = (__u64)&job_args,
   };

   uint32_t num_geom_syncs = 0;
   uint32_t num_frag_syncs = 0;
   uint32_t *handles;
   VkResult result;
   int ret;

   handles = vk_alloc(drm_ws->alloc,
                      sizeof(*handles) * (submit_info->wait_count + 1) * 2,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < submit_info->wait_count; i++) {
      struct vk_sync *sync = submit_info->waits[i];

      if (!sync)
         continue;

      if (submit_info->stage_flags[i] & PVR_PIPELINE_STAGE_GEOM_BIT) {
         handles[num_geom_syncs++] = vk_sync_as_drm_syncobj(sync)->syncobj;
         submit_info->stage_flags[i] &= ~PVR_PIPELINE_STAGE_GEOM_BIT;
      }

      if (submit_info->stage_flags[i] & PVR_PIPELINE_STAGE_FRAG_BIT) {
         handles[submit_info->wait_count + num_frag_syncs++] =
            vk_sync_as_drm_syncobj(sync)->syncobj;
         submit_info->stage_flags[i] &= ~PVR_PIPELINE_STAGE_FRAG_BIT;
      }
   }

   if (submit_info->barrier_geom) {
      struct vk_drm_syncobj *drm_sync =
         vk_sync_as_drm_syncobj(submit_info->barrier_geom);

      handles[num_geom_syncs++] = drm_sync->syncobj;
   }

   if (submit_info->barrier_frag) {
      struct vk_drm_syncobj *drm_sync =
         vk_sync_as_drm_syncobj(submit_info->barrier_frag);

      handles[submit_info->wait_count + num_frag_syncs++] = drm_sync->syncobj;
   }

   args.in_syncobj_handles = (__u64)handles;
   job_args.in_syncobj_handles_frag = (__u64)&handles[submit_info->wait_count];
   args.num_in_syncobj_handles = num_geom_syncs;
   job_args.num_in_syncobj_handles_frag = num_frag_syncs;

   if (signal_sync_geom) {
      job_args.out_syncobj_geom =
         vk_sync_as_drm_syncobj(signal_sync_geom)->syncobj;
   }
   if (signal_sync_frag) {
      job_args.out_syncobj_frag =
         vk_sync_as_drm_syncobj(signal_sync_frag)->syncobj;
   }

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_SUBMIT_JOB, &args);
   if (ret) {
      /* Returns VK_ERROR_OUT_OF_DEVICE_MEMORY to match pvrsrv. */
      result = vk_errorf(NULL,
                         VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to submit render job. Errno: %d - %s.",
                         errno,
                         strerror(errno));
      goto err_free_handles;
   }

   vk_free(drm_ws->alloc, handles);

   return VK_SUCCESS;

err_free_handles:
   vk_free(drm_ws->alloc, handles);

   return result;
}
