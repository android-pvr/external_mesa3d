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
#include "pvr_drm.h"
#include "pvr_drm_bo.h"
#include "pvr_drm_job_common.h"
#include "pvr_drm_job_render.h"
#include "pvr_drm_syncobj.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "vk_alloc.h"
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
   struct drm_pvr_ioctl_create_free_list_args free_list_args = {
      .free_list_gpu_addr = free_list_vma->dev_addr.addr,
      .initial_num_pages = initial_num_pages,
      .max_num_pages = max_num_pages,
      .grow_num_pages = grow_num_pages,
      .grow_threshold = grow_threshold
   };
   struct drm_pvr_ioctl_create_object_args args = {
      .type = DRM_PVR_OBJECT_TYPE_FREE_LIST,
      .data = (__u64)&free_list_args
   };
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
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

   if (drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_OBJECT, &args)) {
      vk_free(drm_ws->alloc, drm_free_list);

      /* Returns VK_ERROR_INITIALIZATION_FAILED to match pvrsrv. */
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to create free list. Errno: %d - %s.",
                       errno,
                       strerror(errno));
   }

   drm_free_list->handle = args.handle;

   *free_list_out = &drm_free_list->base;

   return VK_SUCCESS;
}

void pvr_drm_winsys_free_list_destroy(struct pvr_winsys_free_list *free_list)
{
   struct pvr_drm_winsys_free_list *const drm_free_list =
      to_pvr_drm_winsys_free_list(free_list);
   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(free_list->ws);
   struct drm_pvr_ioctl_destroy_object_args args = {
      .handle = drm_free_list->handle,
   };

   if (drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_OBJECT, &args)) {
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
   struct drm_pvr_static_render_context_state *static_state)
{
   struct pvr_winsys_render_ctx_static_state *ws_static_state =
      &create_info->static_state;

   memset(static_state, 0, sizeof(*static_state));

   static_state->format = DRM_PVR_SRCS_FORMAT_1;
   static_state->data.format_1.geom_reg_vdm_context_state_base_addr =
      ws_static_state->vdm_ctx_state_base_addr;
   static_state->data.format_1.geom_reg_ta_context_state_base_addr =
      ws_static_state->geom_ctx_state_base_addr;

   STATIC_ASSERT(ARRAY_SIZE(static_state->data.format_1.geom_state) ==
                 ARRAY_SIZE(ws_static_state->geom_state));
   for (uint32_t i = 0; i < ARRAY_SIZE(ws_static_state->geom_state); i++) {
      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_store_task0 =
         ws_static_state->geom_state[i].vdm_ctx_store_task0;
      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_store_task1 =
         ws_static_state->geom_state[i].vdm_ctx_store_task1;
      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_store_task2 =
         ws_static_state->geom_state[i].vdm_ctx_store_task2;

      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_resume_task0 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task0;
      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_resume_task1 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task1;
      static_state->data.format_1.geom_state[i]
         .geom_reg_vdm_context_resume_task2 =
         ws_static_state->geom_state[i].vdm_ctx_resume_task2;
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
   /* Structure hierarchy.
    *
    *  drm_pvr_ioctl_create_context_args
    * 		|
    * 		 -> drm_pvr_ioctl_create_render_context_args
    * 		| 		|
    * 		| 		 -> drm_pvr_static_render_context_state
    * 		|
    * 		 -> drm_pvr_reset_framework
    */
   struct drm_pvr_ioctl_create_context_args ctx_args = {
      .type = DRM_PVR_CTX_TYPE_RENDER,
      .priority = pvr_drm_from_winsys_priority(create_info->priority),
      .reset_framework_registers = 0ULL,
   };

   struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ws);
   struct drm_pvr_ioctl_create_render_context_args render_ctx_args;
   struct drm_pvr_static_render_context_state static_state;
   struct pvr_drm_winsys_render_ctx *drm_ctx;
   int ret;

   drm_ctx = vk_zalloc(drm_ws->alloc,
                       sizeof(*drm_ctx),
                       8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_ctx)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_drm_render_ctx_static_state_init(create_info, &static_state);

   render_ctx_args.vdm_callstack_addr = create_info->vdm_callstack_addr.addr;
   render_ctx_args.static_render_context_state = (__u64)&static_state;

   ctx_args.data = (__u64)&render_ctx_args;

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
   struct pvr_winsys_rt_dataset **const rt_dataset_out)
{
   /* clang-format off */
   const struct create_hwrt_geom_data_args geom_data_args_arr[1] = {
      {
         .tail_ptrs_dev_addr = (__u64)create_info->tpc_dev_addr.addr,
         .vheap_table_dev_addr =
            (__u64)create_info->vheap_table_dev_addr.addr,
         .rtc_dev_addr = (__u64)create_info->rtc_dev_addr.addr,
      },
   };
   /* clang-format on */

   struct create_hwrt_rt_data_args rt_data_args_arr[ROGUE_NUM_RTDATAS];

   struct pvr_drm_winsys_free_list *drm_free_list =
      to_pvr_drm_winsys_free_list(create_info->local_free_list);

   /* 0 is just a placeholder. It doesn't indicate an invalid handle. */
   uint32_t parent_free_list_handle =
      drm_free_list->parent ? drm_free_list->parent->handle : 0;

   struct create_hwrt_free_list_args
      free_list_args_arr[PVR_DRM_FREE_LIST_MAX] = {
         /* clang-format off */
         [PVR_DRM_FREE_LIST_LOCAL] = {
            .free_list_handle = drm_free_list->handle,
         },

         [PVR_DRM_FREE_LIST_GLOBAL] = {
            .free_list_handle = parent_free_list_handle,
         },
         /* clang-format on */
   };

   __u32 free_list_handles_count = 1U + (!!drm_free_list->parent);

   struct drm_pvr_ioctl_create_hwrt_dataset_args hwrt_args = {
      .geom_data_args = (__u64)geom_data_args_arr,
      .rt_data_args = (__u64)rt_data_args_arr,
      .free_list_args = (__u64)free_list_args_arr,

      .num_geom_datas = (__u32)ARRAY_SIZE(geom_data_args_arr),
      .num_rt_datas = (__u32)ARRAY_SIZE(rt_data_args_arr),
      .num_free_lists = free_list_handles_count,

      .region_header_size = create_info->rgn_header_size,

      .flipped_multi_sample_control =
         create_info->ppp_multi_sample_ctl_y_flipped,
      .multi_sample_control = create_info->ppp_multi_sample_ctl,
      .mtile_stride = create_info->mtile_stride,
      .screen_pixel_max = create_info->ppp_screen,

      .te_aa = create_info->te_aa,
      .te_mtile = { create_info->te_mtile1, create_info->te_mtile2 },
      .te_screen_size = create_info->te_screen,

      .tpc_size = create_info->tpc_size,
      .tpc_stride = create_info->tpc_stride,

      .isp_merge_lower_x = create_info->isp_merge_lower_x,
      .isp_merge_lower_y = create_info->isp_merge_lower_y,
      .isp_merge_scale_x = create_info->isp_merge_scale_x,
      .isp_merge_scale_y = create_info->isp_merge_scale_y,
      .isp_merge_upper_x = create_info->isp_merge_upper_x,
      .isp_merge_upper_y = create_info->isp_merge_upper_y,
      .isp_mtile_size = create_info->isp_mtile_size,

      .max_rts = create_info->max_rts,
   };

   struct drm_pvr_ioctl_create_object_args args = {
      .type = DRM_PVR_OBJECT_TYPE_HWRT_DATASET,
      .data = (__u64)&hwrt_args,
   };

   struct pvr_drm_winsys *const drm_ws = to_pvr_drm_winsys(ws);
   struct pvr_drm_winsys_rt_dataset *drm_rt_dataset;
   int ret;

   assert(free_list_handles_count <= ARRAY_SIZE(free_list_args_arr));

   drm_rt_dataset = vk_zalloc(drm_ws->alloc,
                              sizeof(*drm_rt_dataset),
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!drm_rt_dataset)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   STATIC_ASSERT(ARRAY_SIZE(rt_data_args_arr) ==
                 ARRAY_SIZE(create_info->rt_datas));

   for (uint32_t i = 0; i < ARRAY_SIZE(rt_data_args_arr); i++) {
      rt_data_args_arr[i].pm_mlist_dev_addr =
         create_info->rt_datas[i].pm_mlist_dev_addr.addr;
      rt_data_args_arr[i].macrotile_array_dev_addr =
         create_info->rt_datas[i].macrotile_array_dev_addr.addr;
      rt_data_args_arr[i].region_header_dev_addr =
         create_info->rt_datas[i].rgn_header_dev_addr.addr;
   }

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_CREATE_OBJECT, &args);
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
   struct drm_pvr_ioctl_destroy_object_args args = {
      .handle = drm_rt_dataset->handle,
   };
   int ret;

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_DESTROY_OBJECT, &args);
   if (ret) {
      vk_errorf(NULL,
                VK_ERROR_UNKNOWN,
                "Error destroying render target dataset. Errno: %d - %s.",
                errno,
                strerror(errno));
   }

   vk_free(drm_ws->alloc, drm_rt_dataset);
}

static void pvr_drm_geometry_cmd_init(
   const struct pvr_winsys_render_submit_info *restrict submit_info,
   struct drm_pvr_cmd_geom *restrict cmd)
{
   const struct pvr_winsys_geometry_state *const state = &submit_info->geometry;
   struct drm_pvr_cmd_geom_format_1 *geom_cmd = &cmd->data.cmd_geom_format_1;
   struct drm_pvr_geom_regs_format_1 *regs = &geom_cmd->geom_regs;

   memset(cmd, 0, sizeof(*cmd));

   cmd->format = DRM_PVR_CMD_GEOM_FORMAT_1;

   geom_cmd->frame_num = submit_info->frame_num;

   if (state->flags & PVR_WINSYS_GEOM_FLAG_FIRST_GEOMETRY)
      geom_cmd->flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST;

   if (state->flags & PVR_WINSYS_GEOM_FLAG_LAST_GEOMETRY)
      geom_cmd->flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST;

   if (state->flags & PVR_WINSYS_GEOM_FLAG_SINGLE_CORE)
      geom_cmd->flags |= DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE;

   regs->vdm_ctrl_stream_base = state->regs.vdm_ctrl_stream_base;
   regs->tpu_border_colour_table = state->regs.tpu_border_colour_table;
   regs->ppp_ctrl = state->regs.ppp_ctrl;
   regs->te_psg = state->regs.te_psg;
   regs->tpu = state->regs.tpu;
   regs->vdm_context_resume_task0_size = state->regs.vdm_ctx_resume_task0_size;
   regs->pds_ctrl = state->regs.pds_ctrl;
}

static void pvr_drm_fragment_cmd_init(
   const struct pvr_winsys_render_submit_info *restrict submit_info,
   struct drm_pvr_cmd_frag *restrict cmd)
{
   const struct pvr_winsys_fragment_state *const state = &submit_info->fragment;
   struct drm_pvr_cmd_frag_format_1 *frag_cmd = &cmd->data.cmd_frag_format_1;
   struct drm_pvr_frag_regs_format_1 *regs = &frag_cmd->regs;

   memset(cmd, 0, sizeof(*cmd));

   cmd->format = DRM_PVR_CMD_FRAG_FORMAT_1;

   frag_cmd->frame_num = submit_info->frame_num;

   if (state->flags & PVR_WINSYS_FRAG_FLAG_DEPTH_BUFFER_PRESENT)
      frag_cmd->flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER;

   if (state->flags & PVR_WINSYS_FRAG_FLAG_STENCIL_BUFFER_PRESENT)
      frag_cmd->flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER;

   if (state->flags & PVR_WINSYS_FRAG_FLAG_PREVENT_CDM_OVERLAP)
      frag_cmd->flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP;

   if (state->flags & PVR_WINSYS_FRAG_FLAG_SINGLE_CORE)
      frag_cmd->flags |= DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE;

   frag_cmd->zls_stride = state->zls_stride;
   frag_cmd->sls_stride = state->sls_stride;

   regs->usc_pixel_output_ctrl = state->regs.usc_pixel_output_ctrl;
   regs->isp_bgobjdepth = state->regs.isp_bgobjdepth;
   regs->isp_bgobjvals = state->regs.isp_bgobjvals;
   regs->isp_aa = state->regs.isp_aa;
   regs->isp_ctl = state->regs.isp_ctl;
   regs->tpu = state->regs.tpu;
   regs->event_pixel_pds_info = state->regs.event_pixel_pds_info;
   regs->pixel_phantom = state->regs.pixel_phantom;
   regs->event_pixel_pds_data = state->regs.event_pixel_pds_data;
   regs->isp_scissor_base = state->regs.isp_scissor_base;
   regs->isp_dbias_base = state->regs.isp_dbias_base;
   regs->isp_oclqry_base = state->regs.isp_oclqry_base;
   regs->isp_zlsctl = state->regs.isp_zlsctl;
   regs->isp_zload_store_base = state->regs.isp_zload_store_base;
   regs->isp_stencil_load_store_base = state->regs.isp_stencil_load_store_base;
   regs->isp_zls_pixels = state->regs.isp_zls_pixels;

   STATIC_ASSERT(ARRAY_SIZE(regs->pbe_word) ==
                 ARRAY_SIZE(state->regs.pbe_word));

   STATIC_ASSERT(ARRAY_SIZE(regs->pbe_word[0]) <=
                 ARRAY_SIZE(state->regs.pbe_word[0]));

#if !defined(NDEBUG)
   /* Depending on the hardware we might have more PBE words than the firmware
    * accepts so check that the extra words are 0.
    */
   if (ARRAY_SIZE(regs->pbe_word[0]) < ARRAY_SIZE(state->regs.pbe_word[0])) {
      /* For each color attachment. */
      for (uint32_t i = 0; i < ARRAY_SIZE(state->regs.pbe_word); i++) {
         /* For each extra PBE word not used by the firmware. */
         for (uint32_t j = ARRAY_SIZE(regs->pbe_word[0]);
              j < ARRAY_SIZE(state->regs.pbe_word[0]);
              j++) {
            assert(state->regs.pbe_word[i][j] == 0);
         }
      }
   }
#endif

   memcpy(regs->pbe_word, state->regs.pbe_word, sizeof(regs->pbe_word));

   regs->tpu_border_colour_table = state->regs.tpu_border_colour_table;

   STATIC_ASSERT(ARRAY_SIZE(regs->pds_bgnd) ==
                 ARRAY_SIZE(state->regs.pds_bgnd));
   typed_memcpy(regs->pds_bgnd,
                state->regs.pds_bgnd,
                ARRAY_SIZE(regs->pds_bgnd));

   STATIC_ASSERT(ARRAY_SIZE(regs->pds_pr_bgnd) ==
                 ARRAY_SIZE(state->regs.pds_pr_bgnd));
   typed_memcpy(regs->pds_pr_bgnd,
                state->regs.pds_pr_bgnd,
                ARRAY_SIZE(regs->pds_pr_bgnd));
}

VkResult pvr_drm_winsys_render_submit(
   const struct pvr_winsys_render_ctx *ctx,
   const struct pvr_winsys_render_submit_info *submit_info,
   struct pvr_winsys_syncobj **const syncobj_geom_out,
   struct pvr_winsys_syncobj **const syncobj_frag_out)

{
   const struct pvr_drm_winsys *drm_ws = to_pvr_drm_winsys(ctx->ws);
   const struct pvr_drm_winsys_render_ctx *drm_ctx =
      to_pvr_drm_winsys_render_ctx(ctx);
   const struct pvr_drm_winsys_rt_dataset *drm_rt_dataset =
      to_pvr_drm_winsys_rt_dataset(submit_info->rt_dataset);

   struct drm_pvr_cmd_geom geom_cmd;
   struct drm_pvr_cmd_frag frag_cmd;
   struct drm_pvr_bo_ref *bo_refs = NULL;

   struct drm_pvr_job_render_args job_args = {
      .cmd_geom = (__u64)&geom_cmd,
      .cmd_frag = (__u64)&frag_cmd,
      .hwrt_data_set_handle = drm_rt_dataset->handle,
      .hwrt_data_index = submit_info->rt_data_idx,
   };

   struct drm_pvr_ioctl_submit_job_args args = {
      .job_type = DRM_PVR_JOB_TYPE_RENDER,
      .context_handle = drm_ctx->handle,
      .ext_job_ref = submit_info->job_num,
      .data = (__u64)&job_args,
   };

   struct pvr_winsys_syncobj *geom_signal_syncobj;
   struct pvr_winsys_syncobj *frag_signal_syncobj;
   struct pvr_drm_winsys_syncobj *drm_syncobj;
   uint32_t num_geom_syncobjs = 0;
   uint32_t num_frag_syncobjs = 0;
   uint32_t *handles;
   VkResult result;
   int ret;

   pvr_drm_geometry_cmd_init(submit_info, &geom_cmd);
   pvr_drm_fragment_cmd_init(submit_info, &frag_cmd);

   handles = vk_alloc(drm_ws->alloc,
                      sizeof(*handles) * submit_info->semaphore_count * 2,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < submit_info->semaphore_count; i++) {
      PVR_FROM_HANDLE(pvr_semaphore, sem, submit_info->semaphores[i]);

      if (!sem->syncobj)
         continue;

      drm_syncobj = to_pvr_drm_winsys_syncobj(sem->syncobj);

      if (submit_info->stage_flags[i] & PVR_PIPELINE_STAGE_GEOM_BIT) {
         handles[num_geom_syncobjs++] = drm_syncobj->handle;
         submit_info->stage_flags[i] &= ~PVR_PIPELINE_STAGE_GEOM_BIT;
      }

      if (submit_info->stage_flags[i] & PVR_PIPELINE_STAGE_FRAG_BIT) {
         handles[submit_info->semaphore_count + num_frag_syncobjs++] =
            drm_syncobj->handle;
         submit_info->stage_flags[i] &= ~PVR_PIPELINE_STAGE_FRAG_BIT;
      }
   }

   job_args.in_syncobj_handles_geom = (__u64)handles;
   job_args.in_syncobj_handles_frag =
      (__u64)&handles[submit_info->semaphore_count];
   job_args.num_in_syncobj_handles_geom = num_geom_syncobjs;
   job_args.num_in_syncobj_handles_frag = num_frag_syncobjs;

   result = pvr_drm_winsys_syncobj_create(ctx->ws, false, &geom_signal_syncobj);
   if (result != VK_SUCCESS)
      goto err_free_handles;

   result = pvr_drm_winsys_syncobj_create(ctx->ws, false, &frag_signal_syncobj);
   if (result != VK_SUCCESS)
      goto err_destroy_geom_signal_syncobj;

   drm_syncobj = to_pvr_drm_winsys_syncobj(geom_signal_syncobj);
   job_args.out_syncobj_geom = drm_syncobj->handle;
   drm_syncobj = to_pvr_drm_winsys_syncobj(frag_signal_syncobj);
   job_args.out_syncobj_frag = drm_syncobj->handle;

   if (submit_info->bo_count > 0U) {
      bo_refs = vk_alloc(drm_ws->alloc,
                         sizeof(*bo_refs) * submit_info->bo_count,
                         8U,
                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!bo_refs) {
         result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto err_destroy_frag_signal_syncobj;
      }

      for (uint32_t i = 0U; i < submit_info->bo_count; i++) {
         const struct pvr_winsys_job_bo *job_bo = &submit_info->bos[i];
         const struct pvr_drm_winsys_bo *drm_bo =
            to_pvr_drm_winsys_bo(job_bo->bo);

         bo_refs[i].handle = drm_bo->handle;

         if (job_bo->flags & PVR_WINSYS_JOB_BO_FLAG_WRITE)
            bo_refs[i].flags = DRM_PVR_BO_REF_WRITE;
         else
            bo_refs[i].flags = DRM_PVR_BO_REF_READ;
      }

      job_args.bo_handles = (__u64)bo_refs;
      job_args.num_bo_handles = submit_info->bo_count;
   }

   ret = drmIoctl(drm_ws->render_fd, DRM_IOCTL_PVR_SUBMIT_JOB, &args);
   if (ret) {
      /* Returns VK_ERROR_OUT_OF_DEVICE_MEMORY to match pvrsrv. */
      result = vk_errorf(NULL,
                         VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to submit render job. Errno: %d - %s.",
                         errno,
                         strerror(errno));
      goto err_free_bo_refs;
   }

   for (uint32_t i = 0; i < submit_info->semaphore_count; i++) {
      PVR_FROM_HANDLE(pvr_semaphore, sem, submit_info->semaphores[i]);

      if (!sem->syncobj)
         continue;

      if (submit_info->stage_flags[i] == 0) {
         pvr_drm_winsys_syncobj_destroy(sem->syncobj);
         sem->syncobj = NULL;
      }
   }

   vk_free(drm_ws->alloc, bo_refs);
   vk_free(drm_ws->alloc, handles);

   *syncobj_geom_out = geom_signal_syncobj;
   *syncobj_frag_out = frag_signal_syncobj;

   return VK_SUCCESS;

err_free_bo_refs:
   vk_free(drm_ws->alloc, bo_refs);

err_destroy_frag_signal_syncobj:
   pvr_drm_winsys_syncobj_destroy(frag_signal_syncobj);

err_destroy_geom_signal_syncobj:
   pvr_drm_winsys_syncobj_destroy(geom_signal_syncobj);

err_free_handles:
   vk_free(drm_ws->alloc, handles);

   return result;
}
