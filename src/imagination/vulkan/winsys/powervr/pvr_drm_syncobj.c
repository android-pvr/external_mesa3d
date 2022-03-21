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

#include "pvr_drm.h"
#include "pvr_drm_syncobj.h"
#include "pvr_winsys.h"
#include "util/libsync.h"
#include "util/os_time.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

VkResult
pvr_drm_winsys_syncobj_create(struct pvr_winsys *ws,
                              bool signaled,
                              struct pvr_winsys_syncobj **const syncobj_out)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct pvr_drm_winsys_syncobj *drm_syncobj;
   uint32_t flags = 0;
   int ret;

   drm_syncobj = vk_alloc(drm_ws->alloc,
                          sizeof(*drm_syncobj),
                          8,
                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_syncobj)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   drm_syncobj->base.ws = ws;

   if (signaled)
      flags |= DRM_SYNCOBJ_CREATE_SIGNALED;

   ret = drmSyncobjCreate(drm_ws->render_fd, flags, &drm_syncobj->handle);
   if (ret) {
      vk_free(drm_ws->alloc, drm_syncobj);
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *syncobj_out = &drm_syncobj->base;

   return VK_SUCCESS;
}

void pvr_drm_winsys_syncobj_destroy(struct pvr_winsys_syncobj *syncobj)
{
   struct pvr_drm_winsys_syncobj *drm_syncobj;
   struct pvr_drm_winsys *drm_ws;

   assert(syncobj);

   drm_syncobj = to_pvr_drm_winsys_syncobj(syncobj);
   drm_ws = to_pvr_drm_winsys(syncobj->ws);

   drmSyncobjDestroy(drm_ws->render_fd, drm_syncobj->handle);
   vk_free(drm_ws->alloc, drm_syncobj);
}

VkResult
pvr_drm_winsys_syncobjs_reset(struct pvr_winsys *ws,
                              struct pvr_winsys_syncobj **const syncobjs,
                              uint32_t count)
{
   struct pvr_drm_winsys *drm_ws;
   uint32_t submit_count = 0;
   uint32_t handles[count];
   int ret;

   for (uint32_t i = 0; i < count; i++) {
      struct pvr_drm_winsys_syncobj *drm_syncobj;

      if (!syncobjs[i])
         continue;

      drm_syncobj = to_pvr_drm_winsys_syncobj(syncobjs[i]);
      handles[submit_count++] = drm_syncobj->handle;
   }

   if (submit_count == 0)
      return VK_SUCCESS;

   drm_ws = to_pvr_drm_winsys(ws);

   ret = drmSyncobjReset(drm_ws->render_fd, handles, submit_count);
   if (ret)
      return vk_error(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return VK_SUCCESS;
}

VkResult
pvr_drm_winsys_syncobjs_signal(struct pvr_winsys *ws,
                               struct pvr_winsys_syncobj **const syncobjs,
                               uint32_t count)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   uint32_t submit_count = 0;
   VkResult result;
   int ret;

   STACK_ARRAY(uint32_t, handles, count);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < count; i++) {
      struct pvr_drm_winsys_syncobj *drm_syncobj;

      if (!syncobjs[i])
         continue;

      drm_syncobj = to_pvr_drm_winsys_syncobj(syncobjs[i]);
      handles[submit_count++] = drm_syncobj->handle;
   }

   if (submit_count == 0) {
      result = VK_SUCCESS;
      goto end_syncobjs_signal;
   }

   ret = drmSyncobjSignal(drm_ws->render_fd, handles, submit_count);
   if (ret)
      result = vk_error(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   else
      result = VK_SUCCESS;

end_syncobjs_signal:
   STACK_ARRAY_FINISH(handles);

   return result;
}

VkResult
pvr_drm_winsys_syncobjs_wait(struct pvr_winsys *ws,
                             struct pvr_winsys_syncobj **const syncobjs,
                             uint32_t count,
                             bool wait_all,
                             uint64_t timeout)
{
   const uint64_t abs_timeout = os_time_get_absolute_timeout(timeout);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   uint32_t submit_count = 0;
   uint32_t *handles;
   VkResult result;
   uint32_t flags;
   int ret;

   handles = vk_alloc(drm_ws->alloc,
                      sizeof(*handles) * count,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < count; i++) {
      struct pvr_drm_winsys_syncobj *drm_syncobj =
         to_pvr_drm_winsys_syncobj(syncobjs[i]);

      if (!syncobjs[i])
         continue;

      handles[submit_count++] = drm_syncobj->handle;
   }

   if (submit_count == 0U) {
      result = VK_SUCCESS;
      goto end_syncobjs_wait;
   }

   flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   if (wait_all)
      flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

   do {
      ret = drmSyncobjWait(drm_ws->render_fd,
                           handles,
                           submit_count,
                           timeout,
                           flags,
                           NULL);
   } while (ret == -ETIME && os_time_get_nano() < abs_timeout);

   if (ret == -ETIME)
      result = VK_TIMEOUT;
   else if (ret)
      result = vk_error(NULL, VK_ERROR_DEVICE_LOST);
   else
      result = VK_SUCCESS;

end_syncobjs_wait:
   vk_free(drm_ws->alloc, handles);

   return result;
}

static int pvr_drm_syncobj_copy(struct pvr_winsys *ws,
                                uint32_t src_handle,
                                uint32_t dst_handle)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   int ret;

   struct drm_syncobj_handle handle = {
      .handle = src_handle,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
      .fd = -1,
   };

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
   if (ret)
      return ret;

   handle.handle = dst_handle;
   handle.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);

   close(handle.fd);

   return ret;
}

static int pvr_drm_syncobj_merge(struct pvr_winsys *ws,
                                 uint32_t handle_a,
                                 uint32_t handle_b,
                                 uint32_t handle_out)
{
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   int fd_a, fd_b, fd_out;
   int ret;

   struct drm_syncobj_handle handle = {
      .handle = handle_a,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
      .fd = -1,
   };

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
   if (ret)
      return ret;

   fd_a = handle.fd;

   handle.handle = handle_b;
   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
   if (ret)
      goto err_close_fd_a;

   fd_b = handle.fd;

   fd_out = sync_merge("", fd_a, fd_b);
   if (fd_out < 0)
      goto err_close_fd_b;

   handle.handle = handle_out;
   handle.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
   handle.fd = fd_out;
   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);

   close(fd_out);

err_close_fd_b:
   close(fd_b);

err_close_fd_a:
   close(fd_a);

   return ret;
}

VkResult pvr_drm_winsys_syncobjs_merge(struct pvr_winsys_syncobj *src,
                                       struct pvr_winsys_syncobj *target,
                                       struct pvr_winsys_syncobj **syncobj_out)
{
   struct pvr_drm_winsys_syncobj *drm_target =
      to_pvr_drm_winsys_syncobj(target);
   struct pvr_drm_winsys_syncobj *drm_src = to_pvr_drm_winsys_syncobj(src);
   struct pvr_drm_winsys_syncobj *drm_output;
   struct pvr_winsys_syncobj *output = NULL;
   VkResult result;
   int ret;

   if (!drm_src) {
      *syncobj_out = target;
      return VK_SUCCESS;
   }

   result = pvr_drm_winsys_syncobj_create(src->ws, false, &output);
   if (result != VK_SUCCESS)
      return result;

   drm_output = to_pvr_drm_winsys_syncobj(output);

   if (!drm_target) {
      ret = pvr_drm_syncobj_copy(src->ws, drm_src->handle, drm_output->handle);
      if (ret) {
         result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto err_output_syncobj_destroy;
      }

      *syncobj_out = output;
      return VK_SUCCESS;
   }

   ret = pvr_drm_syncobj_merge(src->ws,
                               drm_src->handle,
                               drm_target->handle,
                               drm_output->handle);
   if (ret) {
      result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_output_syncobj_destroy;
   }

   pvr_drm_winsys_syncobj_destroy(target);

   *syncobj_out = output;

   return VK_SUCCESS;

err_output_syncobj_destroy:
   pvr_drm_winsys_syncobj_destroy(output);

   return result;
}
