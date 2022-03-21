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
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#include "drm-uapi/pvr_drm.h"
#include "pvr_drm.h"
#include "pvr_drm_job_common.h"
#include "pvr_drm_job_compute.h"
#include "pvr_drm_syncobj.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

static void pvr_drm_compute_ctx_static_state_init(
   const struct pvr_winsys_compute_ctx_static_state *create_info,
   struct drm_pvr_static_compute_context_state *const static_state_out)
{
   *static_state_out = (struct drm_pvr_static_compute_context_state) {
		.format = DRM_PVR_SCCS_FORMAT_1,
		.data = {
			.format_1 = {
				.cdmreg_cdm_context_state_base_addr =
					create_info->cdm_ctx_state_base_addr,

				.cdmreg_cdm_context_pds0 = create_info->cdm_ctx_store_pds0,
				.cdmreg_cdm_context_pds1 = create_info->cdm_ctx_store_pds1,

				.cdmreg_cdm_terminate_pds = create_info->cdm_ctx_terminate_pds,
				.cdmreg_cdm_terminate_pds1 = create_info->cdm_ctx_terminate_pds1,

				.cdmreg_cdm_resume_pds0 = create_info->cdm_ctx_resume_pds0,

				.cdmreg_cdm_context_pds0_b = create_info->cdm_ctx_store_pds0_b,
				.cdmreg_cdm_resume_pds0_b = create_info->cdm_ctx_resume_pds0_b,
			}
		},
	};
}

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
   struct drm_pvr_static_compute_context_state static_state;
   struct drm_pvr_ioctl_create_compute_context_args compute_ctx_args = {
      .static_compute_context_state = (__u64)&static_state,
   };

   /* Structure hierarchy.
    *
    *  drm_pvr_ioctl_create_context_args
    * 		|
    * 		 -> drm_pvr_ioctl_create_compute_context_args
    * 		| 		|
    * 		| 		 -> drm_pvr_static_compute_context_state
    * 		|
    * 		 -> drm_pvr_reset_framework
    */
   struct drm_pvr_ioctl_create_context_args ctx_args = {
      .type = DRM_PVR_CTX_TYPE_COMPUTE,
      .priority = pvr_drm_from_winsys_priority(create_info->priority),
      .reset_framework_registers = 0ULL,
      .data = (__u64)&compute_ctx_args,
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

   pvr_drm_compute_ctx_static_state_init(&create_info->static_state,
                                         &static_state);

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

VkResult pvr_drm_winsys_compute_submit(
   const struct pvr_winsys_compute_ctx *ctx,
   const struct pvr_winsys_compute_submit_info *submit_info,
   struct pvr_winsys_syncobj **const syncobj_out)
{
   pvr_finishme("powervr-km compute job submission support.");

   return VK_SUCCESS;
}
