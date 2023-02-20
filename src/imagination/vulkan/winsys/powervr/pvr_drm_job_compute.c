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
#include "pvr_drm_job_common.h"
#include "pvr_drm_job_compute.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"

struct pvr_drm_winsys_compute_ctx {
   struct pvr_winsys_compute_ctx base;

   /* Handle to kernel context. */
   uint32_t handle;
};

#define to_pvr_drm_winsys_compute_ctx(ctx) \
   container_of(ctx, struct pvr_drm_winsys_compute_ctx, base)

VkResult pvr_drm_winsys_compute_ctx_create(
   struct pvr_winsys *ws,
   const struct pvr_winsys_compute_ctx_create_info *create_info,
   struct pvr_winsys_compute_ctx **const ctx_out)
{
   struct rogue_fwif_static_computecontext_state static_ctx_state = {
      .ctxswitch_regs = {
         .cdmreg_cdm_context_pds0 =
            create_info->static_state.cdm_ctx_store_pds0,
         .cdmreg_cdm_context_pds1 =
            create_info->static_state.cdm_ctx_store_pds1,

         .cdmreg_cdm_terminate_pds =
            create_info->static_state.cdm_ctx_terminate_pds,
         .cdmreg_cdm_terminate_pds1 =
            create_info->static_state.cdm_ctx_terminate_pds1,

         .cdmreg_cdm_resume_pds0 =
            create_info->static_state.cdm_ctx_resume_pds0,

         .cdmreg_cdm_context_pds0_b =
            create_info->static_state.cdm_ctx_store_pds0_b,
         .cdmreg_cdm_resume_pds0_b =
            create_info->static_state.cdm_ctx_resume_pds0_b,
      },
   };

   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct drm_pvr_ioctl_create_context_args ctx_args = {
      .type = DRM_PVR_CTX_TYPE_COMPUTE,
      .priority = pvr_drm_from_winsys_priority(create_info->priority),
      .static_context_state = (__u64)&static_ctx_state,
      .static_context_state_len = (__u32)sizeof(static_ctx_state),
      .vm_context_handle = drm_ws->vm_context,
   };

   struct pvr_drm_winsys_compute_ctx *drm_ctx;
   int ret;

   drm_ctx = vk_alloc(drm_ws->alloc,
                      sizeof(*drm_ctx),
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_ctx)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx_args);
   if (ret) {
      vk_free(drm_ws->alloc, drm_ctx);
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to create compute context, Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   drm_ctx->base.ws = ws;
   drm_ctx->handle = ctx_args.handle;

   *ctx_out = &drm_ctx->base;

   return VK_SUCCESS;
}

void pvr_drm_winsys_compute_ctx_destroy(struct pvr_winsys_compute_ctx *ctx)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   struct pvr_drm_winsys_compute_ctx *drm_ctx =
      to_pvr_drm_winsys_compute_ctx(ctx);
   struct drm_pvr_ioctl_destroy_context_args args = {
      .handle = drm_ctx->handle,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_CONTEXT, &args);
   if (ret) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Error destroying compute context. Errno: %d - %s.",
                errno,
                strerror(errno));
   }

   vk_free(drm_ws->alloc, drm_ctx);
}

static uint32_t pvr_winsys_compute_flags_to_drm(uint32_t ws_flags)
{
   uint32_t flags = 0U;

   if (ws_flags & PVR_WINSYS_COMPUTE_FLAG_PREVENT_ALL_OVERLAP)
      flags |= DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP;

   if (ws_flags & PVR_WINSYS_COMPUTE_FLAG_SINGLE_CORE)
      flags |= DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE;

   return flags;
}

VkResult pvr_drm_winsys_compute_submit(
   const struct pvr_winsys_compute_ctx *ctx,
   const struct pvr_winsys_compute_submit_info *submit_info,
   UNUSED const struct pvr_device_info *dev_info,
   struct vk_sync *signal_sync)
{
   const struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   const struct pvr_drm_winsys_compute_ctx *drm_ctx =
      to_pvr_drm_winsys_compute_ctx(ctx);

   struct drm_pvr_sync_op sync_ops[2];
   struct drm_pvr_job job_args = {
      .type = DRM_PVR_JOB_TYPE_COMPUTE,
      .context_handle = drm_ctx->handle,
      .cmd_stream = (__u64)&submit_info->fw_stream[0],
      .cmd_stream_len = submit_info->fw_stream_len,
      /* bo_handles is unused and zeroed. */
      /* num_bo_handles is unused and zeroed. */
      .flags = pvr_winsys_compute_flags_to_drm(submit_info->flags),
      .sync_ops = DRM_PVR_OBJ_ARRAY(0, sync_ops),
   };

   struct drm_pvr_ioctl_submit_jobs_args args = {
      .jobs = DRM_PVR_OBJ_ARRAY(1, &job_args),
   };

   int ret;

   if (submit_info->wait) {
      struct vk_sync *sync = submit_info->wait;

      assert(!(sync->flags & VK_SYNC_IS_TIMELINE));
      sync_ops[job_args.sync_ops.count++] = (struct drm_pvr_sync_op){
         .handle = vk_sync_as_drm_syncobj(sync)->syncobj,
         .flags = DRM_PVR_SYNC_OP_FLAG_WAIT |
                  DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
         .value = 0,
      };
   }

   assert(!(signal_sync->flags & VK_SYNC_IS_TIMELINE));
   sync_ops[job_args.sync_ops.count++] = (struct drm_pvr_sync_op){
      .handle = vk_sync_as_drm_syncobj(signal_sync)->syncobj,
      .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL |
               DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
      .value = 0,
   };

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args);
   if (ret) {
      /* Returns VK_ERROR_OUT_OF_DEVICE_MEMORY to match pvrsrv. */
      return vk_errorf(NULL,
                       VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to submit compute job. Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   return VK_SUCCESS;
}
