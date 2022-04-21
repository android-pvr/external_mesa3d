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

#ifndef PVR_ROGUE_FWIF_CLIENT_H
#define PVR_ROGUE_FWIF_CLIENT_H

#include <stdint.h>

#include "pvr_rogue_fwif_shared.h"

/**
 * \file pvr_rogue_fwif_client.h
 *
 * \brief Firmware header equivalent to
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_client.h from the powervr kernel
 * module.
 */

/* clang-format off */

#define ALIGN(x) __attribute__((aligned(x)))

/*
 ************************************************
 * Parameter/HWRTData control structures.
 ************************************************
 */

/*
 * Configuration registers which need to be loaded by the firmware before a geometry
 * job can be started.
 */
struct rogue_fwif_geom_regs {
	uint64_t vdm_ctrl_stream_base;
	uint64_t tpu_border_colour_table;

	uint32_t ppp_ctrl;
	uint32_t te_psg;
	uint32_t tpu;

	uint32_t vdm_context_resume_task0_size;

	/* FIXME: HIGH: FIX_HW_BRN_56279 changes the structure's layout, given we
	 * are supporting Features/ERNs/BRNs at runtime, we need to look into this
	 * and find a solution to keep layout intact.
	 */
	/* Available if FIX_HW_BRN_56279 is present. */
	uint32_t pds_ctrl;

	uint32_t view_idx;
};

/*
 * Represents a geometry command that can be used to tile a whole scene's objects as
 * per TA behavior.
 */
struct rogue_fwif_cmd_geom {
	/*
	 * rogue_fwif_cmd_geom_frag_shared field must always be at the beginning of the
	 * struct.
	 *
	 * The command struct (rogue_fwif_cmd_geom) is shared between Client and
	 * Firmware. Kernel is unable to perform read/write operations on the
	 * command struct, the SHARED region is the only exception from this rule.
	 * This region must be the first member so that Kernel can easily access it.
	 * For more info, see rogue_fwif_cmd_geom_frag_shared definition.
	 */
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared;

	struct rogue_fwif_geom_regs regs ALIGN(8);
#define ROGUE_FWIF_GEOM_FIRST (1U << 0U)
#define ROGUE_FWIF_GEOM_LAST (1U << 1U)
#define ROGUE_FWIF_GEOM_SINGLE_CORE (1U << 3U)
	uint32_t flags ALIGN(8);

	/*
	 * Holds the geometry/fragment fence value to allow the fragment partial render command
	 * to go through.
	 */
	struct rogue_fwif_ufo partial_render_geom_frag_fence;
};

/*
 * Configuration registers which need to be loaded by the firmware before ISP
 * can be started.
 */
struct rogue_fwif_frag_regs {
	uint32_t usc_pixel_output_ctrl;

	/* FIXME: HIGH: ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL changes the structure's layout. */
#define ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL 8U
	uint32_t usc_clear_register[ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL];

	uint32_t isp_bgobjdepth;
	uint32_t isp_bgobjvals;
	uint32_t isp_aa;
	uint32_t isp_ctl;

	uint32_t tpu;

	uint32_t event_pixel_pds_info;

	/* FIXME: HIGH: RGX_FEATURE_CLUSTER_GROUPING changes the structure's layout. */
	uint32_t pixel_phantom;

	uint32_t view_idx;

	uint32_t event_pixel_pds_data;
	/*
	 * FIXME: HIGH: MULTIBUFFER_OCLQRY changes the structure's layout.
	 * Commenting out for now as it's not supported by 4.V.2.51.
	 */
	/* uint32_t isp_oclqry_stride; */

	/* All values below the ALIGN(8) must be 64 bit. */
	uint64_t ALIGN(8) isp_scissor_base;
	uint64_t isp_dbias_base;
	uint64_t isp_oclqry_base;
	uint64_t isp_zlsctl;
	uint64_t isp_zload_store_base;
	uint64_t isp_stencil_load_store_base;
	/* FIXME: HIGH: RGX_FEATURE_ZLS_SUBTILE changes the structure's layout. */
	uint64_t isp_zls_pixels;

	/* FIXME: HIGH: RGX_HW_REQUIRES_FB_CDC_ZLS_SETUP changes the structure's layout. */
	uint64_t deprecated;

	/* FIXME: HIGH: RGX_PBE_WORDS_REQUIRED_FOR_RENDERS changes the structure's layout. */
#define ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS 2U
	uint64_t pbe_word[8U][ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS];
	uint64_t tpu_border_colour_table;
	uint64_t pds_bgnd[3U];
	uint64_t pds_pr_bgnd[3U];
};

struct rogue_fwif_cmd_frag {
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared ALIGN(8);

	struct rogue_fwif_frag_regs regs ALIGN(8);
	/* command control flags. */
#define ROGUE_FWIF_FRAG_SINGLE_CORE (1U << 3U)
#define ROGUE_FWIF_FRAG_DEPTHBUFFER (1U << 7U)
#define ROGUE_FWIF_FRAG_STENCILBUFFER (1U << 8U)
#define ROGUE_FWIF_FRAG_PREVENT_CDM_OVERLAP (1U << 26U)
	uint32_t flags;
	/* Stride IN BYTES for Z-Buffer in case of RTAs. */
	uint32_t zls_stride;
	/* Stride IN BYTES for S-Buffer in case of RTAs. */
	uint32_t sls_stride;
};

/*
 * Configuration registers which need to be loaded by the firmware before CDM
 * can be started.
 */
struct rogue_fwif_compute_regs {
	uint64_t tpu_border_colour_table;
	uint64_t cdm_item;
	uint64_t compute_cluster;
	uint64_t cdm_ctrl_stream_base;
	uint64_t cdm_context_state_base_addr;
	uint32_t tpu;
	uint32_t cdm_resume_pds1;
};

struct rogue_fwif_cmd_compute {
	/* Common command attributes */
	struct rogue_fwif_cmd_common common ALIGN(8);

	/* CDM registers */
	struct rogue_fwif_compute_regs regs;

	/* Control flags */
#define ROGUE_FWIF_COMPUTE_FLAG_PREVENT_ALL_OVERLAP (1U << 1U)
#define ROGUE_FWIF_COMPUTE_FLAG_SINGLE_CORE (1U << 5U)
	uint32_t flags ALIGN(8);
};

/* clang-format on */

#endif /* __PVR_ROGUE_FWIF_CLIENT_H__ */
