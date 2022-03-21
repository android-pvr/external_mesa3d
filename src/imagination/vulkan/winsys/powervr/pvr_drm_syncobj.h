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

#ifndef PVR_DRM_SYNCOBJ_H
#define PVR_DRM_SYNCOBJ_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "pvr_winsys.h"
#include "util/macros.h"

struct pvr_drm_winsys_syncobj {
   struct pvr_winsys_syncobj base;

   uint32_t handle;
};

#define to_pvr_drm_winsys_syncobj(syncobj) \
   container_of(syncobj, struct pvr_drm_winsys_syncobj, base)

/*******************************************
   function prototypes
 *******************************************/

VkResult
pvr_drm_winsys_syncobj_create(struct pvr_winsys *ws,
                              bool signaled,
                              struct pvr_winsys_syncobj **const syncobj_out);
void pvr_drm_winsys_syncobj_destroy(struct pvr_winsys_syncobj *syncobj);
VkResult
pvr_drm_winsys_syncobjs_reset(struct pvr_winsys *ws,
                              struct pvr_winsys_syncobj **const syncobjs,
                              uint32_t count);
VkResult
pvr_drm_winsys_syncobjs_signal(struct pvr_winsys *ws,
                               struct pvr_winsys_syncobj **const syncobjs,
                               uint32_t count);
VkResult
pvr_drm_winsys_syncobjs_wait(struct pvr_winsys *ws,
                             struct pvr_winsys_syncobj **const syncobjs,
                             uint32_t count,
                             bool wait_all,
                             uint64_t timeout);
VkResult pvr_drm_winsys_syncobjs_merge(struct pvr_winsys_syncobj *src,
                                       struct pvr_winsys_syncobj *target,
                                       struct pvr_winsys_syncobj **out);

#endif /* PVR_DRM_SYNCOBJ_H */
