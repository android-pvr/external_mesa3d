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
#include "fw-api/pvr_rogue_fwif_client.h"
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

   struct drm_pvr_ioctl_create_context_args ctx_args = {
      .type = DRM_PVR_CTX_TYPE_COMPUTE,
      .priority = pvr_drm_from_winsys_priority(create_info->priority),
      .static_context_state = (__u64)&static_ctx_state,
      .static_context_state_len = (__u32)sizeof(static_ctx_state),
   };

   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
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

static void pvr_drm_compute_cmd_init(
   const struct pvr_winsys_compute_submit_info *restrict submit_info,
   struct drm_pvr_cmd_compute *restrict cmd)
{
   struct drm_pvr_cmd_compute_format_1 *compute_cmd =
      &cmd->data.cmd_compute_format_1;
   struct drm_pvr_compute_regs_format_1 *regs = &compute_cmd->regs;

   memset(cmd, 0, sizeof(*cmd));

   cmd->format = DRM_PVR_CMD_COMPUTE_FORMAT_1;

   compute_cmd->frame_num = submit_info->frame_num;

   if (submit_info->flags & PVR_WINSYS_COMPUTE_FLAG_PREVENT_ALL_OVERLAP) {
      compute_cmd->flags |= DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP;
   }

   if (submit_info->flags & PVR_WINSYS_COMPUTE_FLAG_SINGLE_CORE)
      compute_cmd->flags |= DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE;

   regs->tpu_border_colour_table = submit_info->regs.tpu_border_colour_table;
   regs->cdm_item = submit_info->regs.cdm_item;
   regs->compute_cluster = submit_info->regs.compute_cluster;
   regs->cdm_ctrl_stream_base = submit_info->regs.cdm_ctrl_stream_base;
   regs->cdm_context_state_base_addr =
      submit_info->regs.cdm_ctx_state_base_addr;
   regs->tpu = submit_info->regs.tpu;
   regs->cdm_resume_pds1 = submit_info->regs.cdm_resume_pds1;
}

VkResult pvr_drm_winsys_compute_submit(
   const struct pvr_winsys_compute_ctx *ctx,
   const struct pvr_winsys_compute_submit_info *submit_info,
   struct vk_sync *signal_sync)
{
   const struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   const struct pvr_drm_winsys_compute_ctx *drm_ctx =
      to_pvr_drm_winsys_compute_ctx(ctx);

   struct drm_pvr_cmd_compute compute_cmd;

   struct drm_pvr_job_compute_args job_args = {
      .cmd = (__u64)&compute_cmd,
   };

   struct drm_pvr_ioctl_submit_job_args args = {
      .job_type = DRM_PVR_JOB_TYPE_COMPUTE,
      .context_handle = drm_ctx->handle,
      .ext_job_ref = submit_info->job_num,
      .data = (__u64)&job_args,
   };

   uint32_t num_syncs = 0;
   uint32_t *handles;
   VkResult result;
   int ret;

   pvr_drm_compute_cmd_init(submit_info, &compute_cmd);

   handles = vk_alloc(drm_ws->alloc,
                      sizeof(*handles) * submit_info->wait_count,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < submit_info->wait_count; i++) {
      struct vk_sync *sync = submit_info->waits[i];

      if (!sync)
         continue;

      if (submit_info->stage_flags[i] & PVR_PIPELINE_STAGE_COMPUTE_BIT) {
         handles[num_syncs++] = vk_sync_as_drm_syncobj(sync)->syncobj;
         submit_info->stage_flags[i] &= ~PVR_PIPELINE_STAGE_COMPUTE_BIT;
      }
   }

   job_args.in_syncobj_handles = (__u64)handles;
   job_args.num_in_syncobj_handles = num_syncs;

   job_args.out_syncobj = vk_sync_as_drm_syncobj(signal_sync)->syncobj;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_SUBMIT_JOB, &args);
   if (ret) {
      /* Returns VK_ERROR_OUT_OF_DEVICE_MEMORY to match pvrsrv. */
      result = vk_errorf(NULL,
                         VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to submit compute job. Errno: %d - %s.",
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
