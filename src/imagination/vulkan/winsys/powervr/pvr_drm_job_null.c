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

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#include "drm-uapi/pvr_drm.h"
#include "pvr_drm.h"
#include "pvr_drm_job_null.h"
#include "pvr_winsys.h"
#include "util/libsync.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_util.h"

VkResult pvr_drm_winsys_null_job_submit(struct pvr_winsys *ws,
                                        struct vk_sync **waits,
                                        uint32_t wait_count,
                                        struct vk_sync *signal_sync)
{
   const struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);

   struct drm_pvr_job_null_args job_args = {
      /* bo_handles is unused and zeroed. */
      /* num_bo_handles is unused and zeroed. */
      .flags = 0,
   };

   struct drm_pvr_ioctl_submit_job_args args = {
      .job_type = DRM_PVR_JOB_TYPE_NULL,
      .data = (__u64)&job_args,
   };

   uint32_t num_syncs = 0;
   VkResult result;
   int ret;

   STACK_ARRAY(uint32_t, handles, wait_count);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_sync *sync = waits[i];

      if (!sync)
         continue;

      handles[num_syncs++] = vk_sync_as_drm_syncobj(sync)->syncobj;
   }

   job_args.in_syncobj_handles = (__u64)handles;
   job_args.num_in_syncobj_handles = num_syncs;

   job_args.out_syncobj = vk_sync_as_drm_syncobj(signal_sync)->syncobj;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_SUBMIT_JOB, &args);
   if (ret) {
      result = vk_errorf(NULL,
                         VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to submit null job. Errno: %d - %s.",
                         errno,
                         strerror(errno));
      goto err_free_handles;
   }

   STACK_ARRAY_FINISH(handles);

   return VK_SUCCESS;

err_free_handles:
   STACK_ARRAY_FINISH(handles);

   return result;
}
