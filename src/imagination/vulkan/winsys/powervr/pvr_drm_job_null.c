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

#include "pvr_drm.h"
#include "pvr_drm_job_null.h"
#include "pvr_winsys.h"
#include "util/libsync.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_util.h"

VkResult pvr_drm_winsys_null_job_submit(struct pvr_winsys *ws,
                                        struct vk_sync **waits,
                                        uint32_t wait_count,
                                        struct vk_sync *signal_sync)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct vk_drm_syncobj *drm_signal_sync;
   struct vk_drm_syncobj *drm_wait_sync;
   int out_fd = -1;
   int ret;

   for (uint32_t i = 0U; i < wait_count; i++) {
      int wait_fd;

      if (!waits[i])
         continue;

      drm_wait_sync = vk_sync_as_drm_syncobj(waits[i]);
      ret = drmSyncobjExportSyncFile(drm_ws->render_fd,
                                     drm_wait_sync->syncobj,
                                     &wait_fd);
      if (ret) {
         close(out_fd);
         return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      ret = sync_accumulate("pvr", &out_fd, wait_fd);
      close(wait_fd);
      if (ret) {
         close(out_fd);
         return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   drm_signal_sync = vk_sync_as_drm_syncobj(signal_sync);
   ret = drmSyncobjImportSyncFile(drm_ws->render_fd,
                                  drm_signal_sync->syncobj,
                                  out_fd);
   close(out_fd);
   if (ret)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   return VK_SUCCESS;
}
