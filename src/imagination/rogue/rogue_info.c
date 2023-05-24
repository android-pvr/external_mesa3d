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

#include "rogue.h"

/**
 * \file rogue_info.c
 *
 * \brief Contains information and definitions for defined types and structures.
 */

/* TODO: Adjust according to core configurations:
 * At compiler ctx init time, make a copy of these structs and overwrite
 * nums, valid bits, etc. accordingly.
 * Need to do this for temps/vtxin especially.
 */

/* TODO: Remaining restrictions, e.g. some registers are only
 * usable by a particular instruction (vertex output) etc. */
#define S(n) BITFIELD64_BIT(ROGUE_IO_S##n)
const rogue_reg_class_info rogue_reg_class_infos[ROGUE_REG_CLASS_COUNT] = {
   [ROGUE_REG_CLASS_INVALID] = { .name = "!INVALID!", .str = "!INVALID!", },
   [ROGUE_REG_CLASS_SSA] = { .name = "ssa", .str = "R", },
   [ROGUE_REG_CLASS_EMC] = { .name = "emc", .str = "E", .num = 1, }, /* Virtual. */
   [ROGUE_REG_CLASS_TEMP] = { .name = "temp", .str = "r", .num = 128, .supported_io_srcs = S(0) | S(1) | S(2) | S(3) | S(4) | S(5), },
   [ROGUE_REG_CLASS_COEFF] = { .name = "coeff", .str = "cf", .num = 4096, .supported_io_srcs = S(0) | S(2) | S(3), },
   [ROGUE_REG_CLASS_SHARED] = { .name = "shared", .str = "sh", .num = 4096, .supported_io_srcs = S(0) | S(2) | S(3), },
   [ROGUE_REG_CLASS_SPECIAL] = { .name = "special", .str = "sr", .num = ROGUE_SPECIAL_REG_COUNT, .supported_io_srcs = S(1) | S(2) | S(4), },
   [ROGUE_REG_CLASS_INTERNAL] = { .name = "internal", .str = "i", .num = 0, .supported_io_srcs = S(0) | S(1) | S(2) | S(3) | S(4) | S(5), },
   [ROGUE_REG_CLASS_CONST] = { .name = "const", .str = "sc", .num = 240, .supported_io_srcs = S(0) | S(1) | S(2) | S(3) | S(4) | S(5), },
   [ROGUE_REG_CLASS_PIXOUT] = { .name = "pixout", .str = "po", .num = 4, .supported_io_srcs = S(0) | S(2) | S(3), },
   [ROGUE_REG_CLASS_VTXIN] = { .name = "vtxin", .str = "vi", .num = 128, .supported_io_srcs = S(0) | S(1) | S(2) | S(3) | S(4) | S(5), },
   [ROGUE_REG_CLASS_VTXOUT] = { .name = "vtxout", .str = "vo", .num = 256, },
   [ROGUE_REG_CLASS_IDX0] = { .name = "index0", .str = "idx0", .num = 1, .supported_io_srcs = S(0) | S(2) | S(3), },
   [ROGUE_REG_CLASS_IDX1] = { .name = "index1", .str = "idx1", .num = 1, .supported_io_srcs = S(0) | S(2) | S(3), },
};
#undef S

#define SR(reg) ROGUE_SPECIAL_REG_##reg
const rogue_special_reg_info rogue_special_reg_infos[ROGUE_SPECIAL_REG_COUNT] = {
   [SR(CONSTS_0_START) ... SR(CONSTS_0_END)] = { .valid = true, },

   [SR(PIXOUT_0)] = { .str = "pixout0", .valid = true, .needs_olchk = true, },
   [SR(PIXOUT_1)] = { .str = "pixout1", .valid = true, .needs_olchk = true, },
   [SR(PIXOUT_2)] = { .str = "pixout2", .valid = true, .needs_olchk = true, },
   [SR(PIXOUT_3)] = { .str = "pixout3", .valid = true, .needs_olchk = true, },

   [SR(INTL_0)] = { .str = "internal0", },
   [SR(INTL_1)] = { .str = "internal1", },
   [SR(INTL_2)] = { .str = "internal2", },
   [SR(INTL_3)] = { .str = "internal3", },
   [SR(INTL_4)] = { .str = "internal4", },
   [SR(INTL_5)] = { .str = "internal5", },
   [SR(INTL_6)] = { .str = "internal6", },
   [SR(INTL_7)] = { .str = "internal7", },

   [SR(FACE_ORIENT)] = { .str = "face_orientation", .valid = true, },
   [SR(CLUSTER_NUM)] = { .str = "cluster_number", .valid = true, },
   [SR(OUTPUT_PART)] = { .str = "output_partition", .valid = true, .needs_olchk = true, },
   [SR(TASK_ID)] = { .str = "task_id", .valid = true, },
   [SR(SLOT_NUM)] = { .str = "slot_num", .valid = true, },
   [SR(TILE_X_PIX)] = { .str = "tile_x_pixels", .valid = true, },
   [SR(TILE_Y_PIX)] = { .str = "tile_6_pixels", .valid = true, },
   [SR(INST_NUM)] = { .str = "instance_num_in_slot", .valid = true, },
   [SR(DM_TASK_TYPE)] = { .str = "data_master_and_task_type", .valid = true, },
   [SR(SAMP_NUM)] = { .str = "sample_number", .valid = true, },

   [SR(TILED_LD_COMP_0)] = { .str = "tiled_ld_component_0", .valid = true, .needs_olchk = true, },
   [SR(TILED_LD_COMP_1)] = { .str = "tiled_ld_component_1", .valid = true, .needs_olchk = true, },
   [SR(TILED_LD_COMP_2)] = { .str = "tiled_ld_component_2", .valid = true, .needs_olchk = true, },
   [SR(TILED_LD_COMP_3)] = { .str = "tiled_ld_component_3", .valid = true, .needs_olchk = true, },

   [SR(TILED_ST_COMP_0)] = { .str = "tiled_st_component_0", .valid = true, .needs_olchk = true, },
   [SR(TILED_ST_COMP_1)] = { .str = "tiled_st_component_1", .valid = true, .needs_olchk = true, },
   [SR(TILED_ST_COMP_2)] = { .str = "tiled_st_component_2", .valid = true, .needs_olchk = true, },
   [SR(TILED_ST_COMP_3)] = { .str = "tiled_st_component_3", .valid = true, .needs_olchk = true, },

   [SR(BATCH_NUM)] = { .str = "batch_num", .valid = true, },
   [SR(INST_VALID)] = { .str = "instance_valid", .valid = true, },

   [SR(CONSTS_1_START) ... SR(CONSTS_1_END)] = { .valid = true, },

   [SR(TILE_XY)] = { .str = "tile_xy_coords", .valid = true, },

   [SR(X_P)] = { .str = "x_pixel_mode", .valid = true, },
   [SR(X_S)] = { .str = "x_sample_mode", .valid = true, },

   [SR(Y_P)] = { .str = "y_pixel_mode", .valid = true, },
   [SR(Y_S)] = { .str = "y_sample_mode", .valid = true, },

   [SR(SH_ALLOC_SIZE)] = { .str = "sh_alloc_size", .valid = true, },

   [SR(G0)] = { .str = "global0", },
   [SR(G1)] = { .str = "global1", },
   [SR(G2)] = { .str = "global2", },
   [SR(G3)] = { .str = "global3", },
   [SR(G4)] = { .str = "global4", },
   [SR(G5)] = { .str = "global5", },
   [SR(G6)] = { .str = "global6", },
   [SR(G7)] = { .str = "global7", },

   [SR(LOCAL_ADDR_INST_NUM)] = { .str = "local_addr_inst_num", .valid = true, },

   [SR(TILE_X_P)] = { .str = "x_in_tile_pixel_mode", .valid = true, },
   [SR(TILE_X_S)] = { .str = "x_in_tile_sample_mode", .valid = true, },
   [SR(TILE_Y_P)] = { .str = "y_in_tile_pixel_mode", .valid = true, },
   [SR(TILE_Y_S)] = { .str = "y_in_tile_sample_mode", .valid = true, },

   [SR(RENDER_TGT_ID)] = { .str = "render_target_id", },

   [SR(TILED_LD_COMP_4)] = { .str = "tiled_ld_component_4", .needs_olchk = true, },
   [SR(TILED_LD_COMP_5)] = { .str = "tiled_ld_component_5", .needs_olchk = true, },
   [SR(TILED_LD_COMP_6)] = { .str = "tiled_ld_component_6", .needs_olchk = true, },
   [SR(TILED_LD_COMP_7)] = { .str = "tiled_ld_component_7", .needs_olchk = true, },

   [SR(TILED_ST_COMP_4)] = { .str = "tiled_st_component_4", .needs_olchk = true, },
   [SR(TILED_ST_COMP_5)] = { .str = "tiled_st_component_5", .needs_olchk = true, },
   [SR(TILED_ST_COMP_6)] = { .str = "tiled_st_component_6", .needs_olchk = true, },
   [SR(TILED_ST_COMP_7)] = { .str = "tiled_st_component_7", .needs_olchk = true, },

   [SR(CONSTS_2_START) ... SR(CONSTS_2_END)] = { .valid = true, },

   [SR(TIMER_80NS)] = { .str = "timer_80ns", .valid = true, },

   [SR(PIXOUT_4)] = { .str = "pixout4", .needs_olchk = true, },
   [SR(PIXOUT_5)] = { .str = "pixout5", .needs_olchk = true, },
   [SR(PIXOUT_6)] = { .str = "pixout6", .needs_olchk = true, },
   [SR(PIXOUT_7)] = { .str = "pixout7", .needs_olchk = true, },
};
#undef SR

const rogue_regalloc_info regalloc_info[ROGUE_REGALLOC_CLASS_COUNT] = {
   [ROGUE_REGALLOC_CLASS_TEMP_1] = { .class = ROGUE_REG_CLASS_TEMP, .stride = 1, },
   [ROGUE_REGALLOC_CLASS_TEMP_2] = { .class = ROGUE_REG_CLASS_TEMP, .stride = 2, },
   [ROGUE_REGALLOC_CLASS_TEMP_3] = { .class = ROGUE_REG_CLASS_TEMP, .stride = 3, },
   [ROGUE_REGALLOC_CLASS_TEMP_4] = { .class = ROGUE_REG_CLASS_TEMP, .stride = 4, },
};

const rogue_reg_dst_info rogue_reg_dst_infos[ROGUE_REG_DST_VARIANTS] = {
   {
      .num_dsts = 1,
      .bank_bits = { 1 },
      .index_bits = { 6 },
      .bytes = 1,
   },
   {
      .num_dsts = 1,
      .bank_bits = { 3 },
      .index_bits = { 11 },
      .bytes = 2,
   },
   {
      .num_dsts = 2,
      .bank_bits = { 1, 1 },
      .index_bits = { 7, 6 },
      .bytes = 2,
   },
   {
      .num_dsts = 2,
      .bank_bits = { 3, 3 },
      .index_bits = { 8, 8 },
      .bytes = 3,
   },
   {
      .num_dsts = 2,
      .bank_bits = { 3, 3 },
      .index_bits = { 11, 11 },
      .bytes = 4,
   },
};

const rogue_reg_src_info rogue_reg_lower_src_infos[ROGUE_REG_SRC_VARIANTS] = {
   {
      .num_srcs = 1,
      .mux_bits = 0,
      .bank_bits = { 1 },
      .index_bits = { 6 },
      .bytes = 1,
   },
   {
      .num_srcs = 1,
      .mux_bits = 2,
      .bank_bits = { 3 },
      .index_bits = { 11 },
      .bytes = 3,
   },
   {
      .num_srcs = 2,
      .mux_bits = 0,
      .bank_bits = { 1, 1 },
      .index_bits = { 6, 5 },
      .bytes = 2,
   },
   {
      .num_srcs = 2,
      .mux_bits = 2,
      .bank_bits = { 2, 2 },
      .index_bits = { 7, 7 },
      .bytes = 3,
   },
   {
      .num_srcs = 2,
      .mux_bits = 3,
      .bank_bits = { 3, 2 },
      .index_bits = { 11, 8 },
      .bytes = 4,
   },
   {
      .num_srcs = 3,
      .mux_bits = 2,
      .bank_bits = { 2, 2, 2 },
      .index_bits = { 7, 7, 6 },
      .bytes = 4,
   },
   {
      .num_srcs = 3,
      .mux_bits = 3,
      .bank_bits = { 3, 2, 3 },
      .index_bits = { 8, 8, 8 },
      .bytes = 5,
   },
   {
      .num_srcs = 3,
      .mux_bits = 3,
      .bank_bits = { 3, 2, 3 },
      .index_bits = { 11, 8, 11 },
      .bytes = 6,
   },
};

const rogue_reg_src_info rogue_reg_upper_src_infos[ROGUE_REG_SRC_VARIANTS] = {
   {
      .num_srcs = 1,
      .bank_bits = { 1 },
      .index_bits = { 6 },
      .bytes = 1,
   },
   {
      .num_srcs = 1,
      .bank_bits = { 3 },
      .index_bits = { 11 },
      .bytes = 3,
   },
   {
      .num_srcs = 2,
      .bank_bits = { 1, 1 },
      .index_bits = { 6, 5 },
      .bytes = 2,
   },
   {
      .num_srcs = 2,
      .bank_bits = { 2, 2 },
      .index_bits = { 7, 7 },
      .bytes = 3,
   },
   {
      .num_srcs = 2,
      .bank_bits = { 3, 2 },
      .index_bits = { 11, 8 },
      .bytes = 4,
   },
   {
      .num_srcs = 3,
      .bank_bits = { 2, 2, 2 },
      .index_bits = { 7, 7, 6 },
      .bytes = 4,
   },
   {
      .num_srcs = 3,
      .bank_bits = { 3, 2, 2 },
      .index_bits = { 8, 8, 8 },
      .bytes = 5,
   },
   {
      .num_srcs = 3,
      .bank_bits = { 3, 2, 2 },
      .index_bits = { 11, 8, 8 },
      .bytes = 6,
   },
};

#define OM(op_mod) BITFIELD64_BIT(ROGUE_ALU_OP_MOD_##op_mod)
const rogue_alu_op_mod_info rogue_alu_op_mod_infos[ROGUE_ALU_OP_MOD_COUNT] = {
   [ROGUE_ALU_OP_MOD_PARTA] = { .str = "parta", .exclude = OM(PARTB) },
   [ROGUE_ALU_OP_MOD_PARTB] = { .str = "partb", .exclude = OM(PARTA) },
   [ROGUE_ALU_OP_MOD_SIN] = { .str = "sin", .exclude = OM(COS) },
   [ROGUE_ALU_OP_MOD_COS] = { .str = "cos", .exclude = OM(SIN) },

   [ROGUE_ALU_OP_MOD_LP] = { .str = "lp", },
   [ROGUE_ALU_OP_MOD_SAT] = { .str = "sat", },
   [ROGUE_ALU_OP_MOD_SCALE] = { .str = "scale", },
   [ROGUE_ALU_OP_MOD_ROUNDZERO] = { .str = "roundzero", },

   [ROGUE_ALU_OP_MOD_Z] = { .str = "z", .exclude = OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_GZ] = { .str = "gz", .exclude = OM(Z) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_GEZ] = { .str = "gez", .exclude = OM(Z) | OM(GZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_C] = { .str = "c", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_E] = { .str = "e", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_G] = { .str = "g", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(GE) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_GE] = { .str = "ge", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(NE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_NE] = { .str = "ne", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(L) | OM(LE) },
   [ROGUE_ALU_OP_MOD_L] = { .str = "l", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(LE) },
   [ROGUE_ALU_OP_MOD_LE] = { .str = "le", .exclude = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) },

   [ROGUE_ALU_OP_MOD_F32] = { .str = "f32", .exclude = OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32) },
   [ROGUE_ALU_OP_MOD_U16] = { .str = "u16", .exclude = OM(F32) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32) },
   [ROGUE_ALU_OP_MOD_S16] = { .str = "s16", .exclude = OM(F32) | OM(U16) | OM(U8) | OM(S8) | OM(U32) | OM(S32) },
   [ROGUE_ALU_OP_MOD_U8] = { .str = "u8", .exclude = OM(F32) | OM(U16) | OM(S16) | OM(S8) | OM(U32) | OM(S32) },
   [ROGUE_ALU_OP_MOD_S8] = { .str = "s8", .exclude = OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(U32) | OM(S32) },
   [ROGUE_ALU_OP_MOD_U32] = { .str = "u32", .exclude = OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(S32) },
   [ROGUE_ALU_OP_MOD_S32] = { .str = "s32", .exclude = OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) },

   [ROGUE_ALU_OP_MOD_S] = { .str = "s", },
};
#undef OM

const rogue_alu_dst_mod_info rogue_alu_dst_mod_infos[ROGUE_ALU_DST_MOD_COUNT] = {
   [ROGUE_ALU_DST_MOD_E0] = { .str = "e0", },
   [ROGUE_ALU_DST_MOD_E1] = { .str = "e1", },
   [ROGUE_ALU_DST_MOD_E2] = { .str = "e2", },
   [ROGUE_ALU_DST_MOD_E3] = { .str = "e3", },
};

const rogue_alu_src_mod_info rogue_alu_src_mod_infos[ROGUE_ALU_SRC_MOD_COUNT] = {
   [ROGUE_ALU_SRC_MOD_FLR] = { .str = "flr", },
   [ROGUE_ALU_SRC_MOD_ABS] = { .str = "abs", },
   [ROGUE_ALU_SRC_MOD_NEG] = { .str = "neg", },
   [ROGUE_ALU_SRC_MOD_E0] = { .str = "e0", },
   [ROGUE_ALU_SRC_MOD_E1] = { .str = "e1", },
   [ROGUE_ALU_SRC_MOD_E2] = { .str = "e2", },
   [ROGUE_ALU_SRC_MOD_E3] = { .str = "e3", },
};

#define OM(op_mod) BITFIELD64_BIT(ROGUE_CTRL_OP_MOD_##op_mod)
const rogue_ctrl_op_mod_info rogue_ctrl_op_mod_infos[ROGUE_CTRL_OP_MOD_COUNT] = {
   [ROGUE_CTRL_OP_MOD_LINK] = { .str = "link", },
   [ROGUE_CTRL_OP_MOD_ALLINST] = { .str = "allinst", .exclude = OM(ANYINST) },
   [ROGUE_CTRL_OP_MOD_ANYINST] = { .str = "anyinst", .exclude = OM(ALLINST) },
   [ROGUE_CTRL_OP_MOD_END] = { .str = "end", },
   [ROGUE_CTRL_OP_MOD_NOWDF] = { .str = "nowdf", },
   [ROGUE_CTRL_OP_MOD_ALWAYS] = { .str = "always", },
   [ROGUE_CTRL_OP_MOD_P0_TRUE] = { .str = "if(p0)", },
   [ROGUE_CTRL_OP_MOD_NEVER] = { .str = "never", },
   [ROGUE_CTRL_OP_MOD_P0_FALSE] = { .str = "if(!p0)", },
};
#undef OM

#define IO(io) BITFIELD64_BIT(ROGUE_IO_##io)
#define OM(op_mod) BITFIELD64_BIT(ROGUE_CTRL_OP_MOD_##op_mod)
#define T(type) BITFIELD64_BIT(ROGUE_REF_TYPE_##type - 1)
const rogue_ctrl_op_info rogue_ctrl_op_infos[ROGUE_CTRL_OP_COUNT] = {
   [ROGUE_CTRL_OP_INVALID] = { .str = "!INVALID!", },
   [ROGUE_CTRL_OP_END] = { .str = "end", .ends_block = true, },
   [ROGUE_CTRL_OP_NOP] = { .str = "nop",
      .supported_op_mods = OM(END),
   },
   [ROGUE_CTRL_OP_WOP] = { .str = "wop", },
   [ROGUE_CTRL_OP_BR] = { .str = "br", .has_target = true, .ends_block = true,
      .supported_op_mods = OM(LINK) | OM(ALLINST) | OM(ANYINST),
   },
   [ROGUE_CTRL_OP_BA] = { .str = "ba", .ends_block = true,
      .num_srcs = 1,
      .supported_op_mods = OM(LINK) | OM(ALLINST) | OM(ANYINST),
      .supported_src_types = { [0] = T(VAL), },
   },
   [ROGUE_CTRL_OP_CNDST] = { .str = "cndst", .ends_block = true,
      .has_srcs = true, .has_dsts = true, .num_dsts = 2, .num_srcs = 2,
      .io = { .dst_set[1] = IO(W0), .src_set[0] = IO(S0), },
      .supported_op_mods = OM(ALWAYS) | OM(P0_TRUE) | OM(NEVER) | OM(P0_FALSE),
      .supported_dst_types = { [0] = T(IO), [1] = T(REG), },
      .supported_src_types = { [0] = T(REG) | T(IMM), [1] = T(VAL), },
   },
   [ROGUE_CTRL_OP_CNDEF] = { .str = "cndef", .ends_block = true,
      .has_srcs = true, .has_dsts = true, .num_dsts = 2, .num_srcs = 2,
      .io = { .dst_set[1] = IO(W0), .src_set[0] = IO(S0), },
      .supported_op_mods = OM(ALWAYS) | OM(P0_TRUE) | OM(NEVER) | OM(P0_FALSE),
      .supported_dst_types = { [0] = T(IO), [1] = T(REG), },
      .supported_src_types = { [0] = T(REG) | T(IMM), [1] = T(VAL), },
   },
   [ROGUE_CTRL_OP_CNDEND] = { .str = "cndend", .ends_block = true,
      .has_srcs = true, .has_dsts = true, .num_dsts = 2, .num_srcs = 2,
      .io = { .dst_set[1] = IO(W0), .src_set[0] = IO(S0), },
      .supported_dst_types = { [0] = T(IO), [1] = T(REG), },
      .supported_src_types = { [0] = T(REG) | T(IMM), [1] = T(VAL), },
   },
   [ROGUE_CTRL_OP_CNDLT] = { .str = "cndlt", /* .ends_block = true, */
      .has_srcs = true, .has_dsts = true, .num_dsts = 3, .num_srcs = 2,
      .io = { .dst_set[1] = IO(W0), .src_set[0] = IO(S0), },
      .supported_op_mods = OM(ALWAYS) | OM(P0_TRUE) | OM(NEVER) | OM(P0_FALSE),
      .supported_dst_types = { [0] = T(IO), [1] = T(REG), [2] = T(IO) },
      .supported_src_types = { [0] = T(REG) | T(IMM), [1] = T(VAL), },
   },
   [ROGUE_CTRL_OP_WDF] = { .str = "wdf", .num_srcs = 1,
      .supported_src_types = { [0] = T(DRC), },
   },
};
#undef T
#undef OM
#undef IO

#define IO(io) BITFIELD64_BIT(ROGUE_IO_##io)
#define OM(op_mod) BITFIELD64_BIT(ROGUE_BACKEND_OP_MOD_##op_mod)
#define T(type) BITFIELD64_BIT(ROGUE_REF_TYPE_##type - 1)
#define B(n) BITFIELD64_BIT(n)
const rogue_backend_op_info rogue_backend_op_infos[ROGUE_BACKEND_OP_COUNT] = {
	[ROGUE_BACKEND_OP_INVALID] = { .str = "!INVALID!", },
   [ROGUE_BACKEND_OP_UVSW_WRITE] = { .str = "uvsw.write", .num_dsts = 1, .num_srcs = 1,
      .io = { .src_set[0] = IO(W0) /* | IO(W1) */, },
      .supported_dst_types = { [0] = T(REG), },
      .supported_src_types = { [0] = T(REG), },
   },
   [ROGUE_BACKEND_OP_UVSW_EMIT] = { .str = "uvsw.emit", },
   [ROGUE_BACKEND_OP_UVSW_ENDTASK] = { .str = "uvsw.endtask", },
   [ROGUE_BACKEND_OP_UVSW_EMITTHENENDTASK] = { .str = "uvsw.emitthenendtask", },
   [ROGUE_BACKEND_OP_UVSW_WRITETHENEMITTHENENDTASK] = { .str = "uvsw.writethenemitthenendtask", .num_dsts = 1, .num_srcs = 1,
      .io = { .src_set[0] = IO(W0) /* | IO(W1) */, },
      .supported_dst_types = { [0] = T(REG), },
      .supported_src_types = { [0] = T(REG), },
   },
   [ROGUE_BACKEND_OP_IDF] = { .str = "idf", .num_srcs = 2,
      .io = { .src_set[1] = IO(S0) | IO(S1) | IO(S2) | IO(S3) | IO(S4) | IO(S5), },
      .supported_src_types = { [0] = T(DRC), [1] = T(REGARRAY), },
      .src_stride = {
         [1] = 1,
      },
   },
   [ROGUE_BACKEND_OP_EMITPIX] = { .str = "emitpix", .num_srcs = 2,
      .io = { .src_set[0] = IO(S0), .src_set[1] = IO(S2), },
      .supported_op_mods = OM(FREEP),
      .supported_src_types = { [0] = T(REG), [1] = T(REG), },
   },
   [ROGUE_BACKEND_OP_LD] = { .str = "ld", .num_dsts = 1, .num_srcs = 3,
      .io = {
         .dst_set[0] = IO(S3),
         .src_set[1] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
         .src_set[2] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
      },
      .supported_op_mods = OM(BYPASS) | OM(FORCELINEFILL) | OM(SLCBYPASS) | OM(SLCNOALLOC),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REG) | T(VAL),
         [2] = T(REGARRAY),
      },
      .src_stride = {
         [2] = 1,
      },
      .valnum_src = 1,
      .dst_valnum_mask = B(0),
   },
   [ROGUE_BACKEND_OP_ST] = { .str = "st", .num_srcs = 6,
      .io = {
         .src_set[0] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
         .src_set[3] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
         .src_set[4] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
         .src_set[5] = IO(S0) | IO(S1) | IO(S2) | IO(S4) | IO(S5),
      },
      .supported_op_mods = OM(TILED) | OM(WRITETHROUGH) | OM(WRITEBACK) | OM(LAZYWRITEBACK) |
         OM(SLCBYPASS) | OM(SLCWRITEBACK) | OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) | OM(NOWDF),
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
         [1] = T(VAL),
         [2] = T(DRC),
         [3] = T(REG) | T(VAL),
         [4] = T(REGARRAY),
         [5] = T(REGARRAY) | T(IO),
      },
      .src_stride = {
         [4] = 1,
      },
      .valnum_src = 3,
      .src_valnum_mask = B(0),
   },
   /* TODO: Can't co-issue with TST. */
   [ROGUE_BACKEND_OP_ATST] = { .str = "atst", .num_dsts = 1, .num_srcs = 4,
      .io = { .src_set[1] = IO(S0), .src_set[2] = IO(S1), .src_set[3] = IO(S2), },
      .supported_op_mods = OM(IFB),
      .supported_dst_types = { [0] = T(IO), },
      .supported_src_types = {
         [0] = T(DRC), /* TODO: *must* be DRC0 */
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
         [3] = T(REG) | T(IMM), /* TODO: Must be special constant or non-indexed shared. */
      },
   },
	[ROGUE_BACKEND_OP_FITR_PIXEL] = { .str = "fitr.pixel", .num_dsts = 1, .num_srcs = 3,
      .io = { .dst_set[0] = IO(S3), .src_set[1] = IO(S0), },
      .supported_op_mods = OM(SAT),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REGARRAY),
         [2] = T(VAL),
      },
      .src_stride = {
         [1] = 3,
      },
      .valnum_src = 2,
      .dst_valnum_mask = B(0),
      .src_valnum_mask = B(1),
   },
	[ROGUE_BACKEND_OP_FITRP_PIXEL] = { .str = "fitrp.pixel", .num_dsts = 1, .num_srcs = 4,
      .io = { .dst_set[0] = IO(S3), .src_set[1] = IO(S0), .src_set[2] = IO(S2), },
      .supported_op_mods = OM(SAT),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REGARRAY),
         [2] = T(REGARRAY),
         [3] = T(VAL),
      },
      .src_stride = {
         [1] = 3,
         [2] = 3,
      },
      .valnum_src = 3,
      .dst_valnum_mask = B(0),
      .src_valnum_mask = B(1),
   },
	[ROGUE_BACKEND_OP_SMP1D] = { .str = "smp1d", .num_dsts = 1, .num_srcs = 6,
      .io = { .dst_set[0] = IO(S4), .src_set[1] = IO(S0), .src_set[2] = IO(S1), .src_set[3] = IO(S2), .src_set[4] = IO(S3), },
      .supported_op_mods = OM(PROJ) | OM(FCNORM) | OM(NNCOORDS) | OM(BIAS) | OM(REPLACE) |
         OM(GRADIENT) | OM(PPLOD) | OM(TAO) | OM(SOO) | OM(SNO) | OM(WRT) | OM(DATA) |
         OM(INFO) | OM(BOTH) | OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITETHROUGH) |
         OM(WRITEBACK) | OM(LAZYWRITEBACK) | OM(SLCBYPASS) | OM(SLCWRITEBACK) |
         OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) | OM(ARRAY) | OM(INTEGER) | OM(SCHEDSWAP) |
         OM(F16),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REGARRAY),
         [2] = T(REG) | T(REGARRAY),
         [3] = T(REGARRAY),
         [4] = T(REGARRAY) | T(IO),
         [5] = T(VAL),
      },
      /* TODO: This may depend on the other options set. */
      .src_stride = {
         [1] = 3,
         [2] = 1,
         [3] = 3,
         [4] = 1,
      },
      .valnum_src = 5,
      .dst_valnum_mask = B(0),
   },
	[ROGUE_BACKEND_OP_SMP2D] = { .str = "smp2d", .num_dsts = 1, .num_srcs = 6,
      .io = { .dst_set[0] = IO(S4), .src_set[1] = IO(S0), .src_set[2] = IO(S1), .src_set[3] = IO(S2), .src_set[4] = IO(S3), },
      .supported_op_mods = OM(PROJ) | OM(FCNORM) | OM(NNCOORDS) | OM(BIAS) | OM(REPLACE) |
         OM(GRADIENT) | OM(PPLOD) | OM(TAO) | OM(SOO) | OM(SNO) | OM(WRT) | OM(DATA) |
         OM(INFO) | OM(BOTH) | OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITETHROUGH) |
         OM(WRITEBACK) | OM(LAZYWRITEBACK) | OM(SLCBYPASS) | OM(SLCWRITEBACK) |
         OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) | OM(ARRAY) | OM(INTEGER) | OM(SCHEDSWAP) |
         OM(F16),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REGARRAY),
         [2] = T(REG) | T(REGARRAY),
         [3] = T(REGARRAY),
         [4] = T(REGARRAY) | T(IO),
         [5] = T(VAL),
      },
      /* TODO: This may depend on the other options set. */
      .src_stride = {
         [1] = 3,
         [2] = 1,
         [3] = 3,
         [4] = 1,
      },
      .valnum_src = 5,
      .dst_valnum_mask = B(0),
   },
	[ROGUE_BACKEND_OP_SMP3D] = { .str = "smp3d", .num_dsts = 1, .num_srcs = 6,
      .io = { .dst_set[0] = IO(S4), .src_set[1] = IO(S0), .src_set[2] = IO(S1), .src_set[3] = IO(S2), .src_set[4] = IO(S3), },
      .supported_op_mods = OM(PROJ) | OM(FCNORM) | OM(NNCOORDS) | OM(BIAS) | OM(REPLACE) |
         OM(GRADIENT) | OM(PPLOD) | OM(TAO) | OM(SOO) | OM(SNO) | OM(WRT) | OM(DATA) |
         OM(INFO) | OM(BOTH) | OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITETHROUGH) |
         OM(WRITEBACK) | OM(LAZYWRITEBACK) | OM(SLCBYPASS) | OM(SLCWRITEBACK) |
         OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) | OM(ARRAY) | OM(INTEGER) | OM(SCHEDSWAP) |
         OM(F16),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(DRC),
         [1] = T(REGARRAY),
         [2] = T(REG) | T(REGARRAY),
         [3] = T(REGARRAY),
         [4] = T(REGARRAY) | T(IO),
         [5] = T(VAL),
      },
      /* TODO: This may depend on the other options set. */
      .src_stride = {
         [1] = 3,
         [2] = 1,
         [3] = 3,
         [4] = 1,
      },
      .valnum_src = 5,
      .dst_valnum_mask = B(0),
   },
   [ROGUE_BACKEND_OP_ATST_IF] = { .str = "atst.if", .num_srcs = 2,
      .supported_op_mods = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) |
         OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS),
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
};
#undef B
#undef T
#undef OM
#undef IO

#define OM(op_mod) BITFIELD64_BIT(ROGUE_BACKEND_OP_MOD_##op_mod)
const rogue_backend_op_mod_info rogue_backend_op_mod_infos[ROGUE_BACKEND_OP_MOD_COUNT] = {
   [ROGUE_BACKEND_OP_MOD_PROJ]  = { .str = "proj", },
   [ROGUE_BACKEND_OP_MOD_FCNORM]  = { .str = "fcnorm", },
   [ROGUE_BACKEND_OP_MOD_NNCOORDS]  = { .str = "nncoords", },
   [ROGUE_BACKEND_OP_MOD_BIAS]  = { .str = "bias", .exclude = OM(REPLACE) | OM(GRADIENT) },
   [ROGUE_BACKEND_OP_MOD_REPLACE]  = { .str = "replace", .exclude = OM(BIAS) | OM(GRADIENT) },
   [ROGUE_BACKEND_OP_MOD_GRADIENT]  = { .str = "gradient", .exclude = OM(BIAS) | OM(REPLACE) },
   [ROGUE_BACKEND_OP_MOD_PPLOD]  = { .str = "pplod", .require = OM(BIAS) | OM(REPLACE) },
   [ROGUE_BACKEND_OP_MOD_TAO]  = { .str = "tao", },
   [ROGUE_BACKEND_OP_MOD_SOO]  = { .str = "soo", },
   [ROGUE_BACKEND_OP_MOD_SNO]  = { .str = "sno", },
   [ROGUE_BACKEND_OP_MOD_WRT]  = { .str = "wrt", },
   [ROGUE_BACKEND_OP_MOD_DATA]  = { .str = "data", .exclude = OM(INFO) | OM(BOTH) },
   [ROGUE_BACKEND_OP_MOD_INFO]  = { .str = "info", .exclude = OM(DATA) | OM(BOTH) },
   [ROGUE_BACKEND_OP_MOD_BOTH]  = { .str = "both", .exclude = OM(DATA) | OM(INFO) },
   [ROGUE_BACKEND_OP_MOD_TILED] = { .str = "tiled", },
   [ROGUE_BACKEND_OP_MOD_BYPASS]  = { .str = "bypass", .exclude = OM(FORCELINEFILL) | OM(WRITETHROUGH) | OM(WRITEBACK) | OM(LAZYWRITEBACK) },
   [ROGUE_BACKEND_OP_MOD_FORCELINEFILL]  = { .str = "forcelinefill", .exclude = OM(BYPASS) | OM(WRITETHROUGH) | OM(WRITEBACK) | OM(LAZYWRITEBACK) },
   [ROGUE_BACKEND_OP_MOD_WRITETHROUGH]  = { .str = "writethrough", .exclude = OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITEBACK) | OM(LAZYWRITEBACK) },
   [ROGUE_BACKEND_OP_MOD_WRITEBACK]  = { .str = "writeback", .exclude = OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITETHROUGH) | OM(LAZYWRITEBACK) },
   [ROGUE_BACKEND_OP_MOD_LAZYWRITEBACK]  = { .str = "lazywriteback", .exclude = OM(BYPASS) | OM(FORCELINEFILL) | OM(WRITETHROUGH) | OM(WRITEBACK) },
   [ROGUE_BACKEND_OP_MOD_SLCBYPASS]  = { .str = "slcbypass", .exclude = OM(SLCWRITEBACK) | OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) },
   [ROGUE_BACKEND_OP_MOD_SLCWRITEBACK]  = { .str = "slcwriteback", .exclude = OM(SLCBYPASS) | OM(SLCWRITETHROUGH) | OM(SLCNOALLOC) },
   [ROGUE_BACKEND_OP_MOD_SLCWRITETHROUGH]  = { .str = "slcwritethrough", .exclude = OM(SLCBYPASS) | OM(SLCWRITEBACK) | OM(SLCNOALLOC) },
   [ROGUE_BACKEND_OP_MOD_SLCNOALLOC]  = { .str = "slcnoalloc", .exclude = OM(SLCBYPASS) | OM(SLCWRITEBACK) | OM(SLCWRITETHROUGH) },
   [ROGUE_BACKEND_OP_MOD_ARRAY]  = { .str = "array", },
   [ROGUE_BACKEND_OP_MOD_INTEGER]  = { .str = "integer", },
   [ROGUE_BACKEND_OP_MOD_SCHEDSWAP]  = { .str = "schedswap", },
   [ROGUE_BACKEND_OP_MOD_F16]  = { .str = "f16", },
   [ROGUE_BACKEND_OP_MOD_SAT]  = { .str = "sat", },
   [ROGUE_BACKEND_OP_MOD_FREEP] = { .str = "freep", },
   [ROGUE_BACKEND_OP_MOD_NOWDF] = { .str = "nowdf", },
   [ROGUE_BACKEND_OP_MOD_IFB] = { .str = "ifb", },
   [ROGUE_BACKEND_OP_MOD_NEVER] = { .str = "never", .exclude = OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_LESS] = { .str = "less", .exclude = OM(NEVER) | OM(EQUAL) | OM(LESSEQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_EQUAL] = { .str = "equal", .exclude = OM(NEVER) | OM(LESS) | OM(LESSEQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_LESSEQUAL] = { .str = "lessequal", .exclude = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_GREATER] = { .str = "greater", .exclude = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) | OM(NOTEQUAL) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_NOTEQUAL] = { .str = "notequal", .exclude = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) | OM(GREATER) | OM(GREATEREQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_GREATEREQUAL] = { .str = "greaterequal", .exclude = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(ALWAYS) },
   [ROGUE_BACKEND_OP_MOD_ALWAYS] = { .str = "always", .exclude = OM(NEVER) | OM(LESS) | OM(EQUAL) | OM(LESSEQUAL) | OM(GREATER) | OM(NOTEQUAL) | OM(GREATEREQUAL) },
};
#undef OM

#define OM(op_mod) BITFIELD64_BIT(ROGUE_BITWISE_OP_MOD_##op_mod)
const rogue_bitwise_op_mod_info
   rogue_bitwise_op_mod_infos[ROGUE_BITWISE_OP_MOD_COUNT] = {
      [ROGUE_BITWISE_OP_MOD_TWB] = { .str = "twb",
                                     .exclude = OM(PWB) | OM(MTB) | OM(FTB) },
      [ROGUE_BITWISE_OP_MOD_PWB] = { .str = "pwb",
                                     .exclude = OM(TWB) | OM(MTB) | OM(FTB) },
      [ROGUE_BITWISE_OP_MOD_MTB] = { .str = "mtb",
                                     .exclude = OM(TWB) | OM(PWB) | OM(FTB) },
      [ROGUE_BITWISE_OP_MOD_FTB] = { .str = "ftb",
                                     .exclude = OM(TWB) | OM(PWB) | OM(MTB) },
   };
#undef OM

#define PH(type) ROGUE_INSTR_PHASE_##type
#define OM(op_mod) BITFIELD64_BIT(ROGUE_BITWISE_OP_MOD_##op_mod)
#define IO(io) BITFIELD64_BIT(ROGUE_IO_##io)
#define T(type) BITFIELD64_BIT(ROGUE_REF_TYPE_##type - 1)
const rogue_bitwise_op_info rogue_bitwise_op_infos[ROGUE_BITWISE_OP_COUNT] = {
   [ROGUE_BITWISE_OP_INVALID] = { .str = "", },
   [ROGUE_BITWISE_OP_LSL0] = { .str = "lsl", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(0_SHIFT1),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(S2), .src_set[1] = IO(S1), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_LSL2] = { .str = "lsl", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(2_SHIFT2),
      .io = { .dst_set[0] = IO(FT5), .src_set[0] = IO(FT4), .src_set[1] = IO(S4), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_SHR] = { .str = "shr", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(2_SHIFT2),
      .io = { .dst_set[0] = IO(FT5), .src_set[0] = IO(FT4), .src_set[1] = IO(S4), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_ASR] = { .str = "asr", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(2_SHIFT2),
      .io = { .dst_set[0] = IO(FT5), .src_set[0] = IO(FT4), .src_set[1] = IO(S4), },
      .supported_op_mods = OM(TWB) | OM(PWB) | OM(MTB) | OM(FTB),
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_AND] = { .str = "and", .num_dsts = 1, .num_srcs = 4,
      .phase = PH(1_LOGICAL),
      .io = { .dst_set[0] = IO(FT4), .src_set[1] = IO(FT2), .src_set[3] = IO(S3), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
         [2] = T(REG) | T(REGARRAY) | T(IO),
         [3] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_OR] = { .str = "or", .num_dsts = 1, .num_srcs = 4,
      .phase = PH(1_LOGICAL),
      .io = { .dst_set[0] = IO(FT4), .src_set[1] = IO(FT2), .src_set[3] = IO(S3), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
         [2] = T(REG) | T(REGARRAY) | T(IO),
         [3] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_XOR] = { .str = "xor", .num_dsts = 1, .num_srcs = 4,
      .phase = PH(1_LOGICAL),
      .io = { .dst_set[0] = IO(FT4), .src_set[1] = IO(FT2), .src_set[3] = IO(S3), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
         [2] = T(REG) | T(REGARRAY) | T(IO),
         [3] = T(REG) | T(REGARRAY) | T(IO) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_BYP0B] = { .str = "byp", .num_dsts = 2, .num_srcs = 2,
      .phase = PH(0_BITMASK),
      .io = { .dst_set[0] = IO(FT0), .dst_set[1] = IO(FT1), .src_set[0] = IO(S0), .src_set[1] = IO(S1), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO) | T(VAL),
      },
   },
   [ROGUE_BITWISE_OP_BYP0S] = { .str = "byp", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0_SHIFT1),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(S2) },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
   },
   [ROGUE_BITWISE_OP_MOVI] = { .str = "movi", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_ISHL] = { .str = "ishl", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_ISHR] = { .str = "ishr", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_USHR] = { .str = "ushr", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_IAND] = { .str = "iand", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_IOR] = { .str = "ior", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_BITWISE_OP_IXOR] = { .str = "ixor", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
};
#undef T
#undef IO
#undef OM
#undef PH

const rogue_io_info rogue_io_infos[ROGUE_IO_COUNT] = {
   [ROGUE_IO_S0] = { .str = "s0", },
   [ROGUE_IO_S1] = { .str = "s1", },
   [ROGUE_IO_S2] = { .str = "s2", },
   [ROGUE_IO_S3] = { .str = "s3", },
   [ROGUE_IO_S4] = { .str = "s4", },
   [ROGUE_IO_S5] = { .str = "s5", },
   [ROGUE_IO_W0] = { .str = "w0", },
   [ROGUE_IO_W1] = { .str = "w1", },
   [ROGUE_IO_IS0] = { .str = "is0", },
   [ROGUE_IO_IS1] = { .str = "is1", },
   [ROGUE_IO_IS2] = { .str = "is2", },
   [ROGUE_IO_IS3] = { .str = "is3", },
   [ROGUE_IO_IS4] = { .str = "is4|w0", },
   [ROGUE_IO_IS5] = { .str = "is5|w1", },
   [ROGUE_IO_FT0] = { .str = "ft0", },
   [ROGUE_IO_FT1] = { .str = "ft1", },
   [ROGUE_IO_FT2] = { .str = "ft2", },
   [ROGUE_IO_FTE] = { .str = "fte", },
   [ROGUE_IO_FT3] = { .str = "ft3", },
   [ROGUE_IO_FT4] = { .str = "ft4", },
   [ROGUE_IO_FT5] = { .str = "ft5", },
   [ROGUE_IO_FTT] = { .str = "ftt", },
   [ROGUE_IO_FT0H] = { .str = "ft0h", },
   [ROGUE_IO_P0] = { .str = "p0", },
   [ROGUE_IO_PE] = { .str = "pe", },
   [ROGUE_IO_NONE] = { .str = "_", },
};

#define SM(src_mod) BITFIELD64_BIT(ROGUE_ALU_SRC_MOD_##src_mod)
#define DM(dst_mod) BITFIELD64_BIT(ROGUE_ALU_DST_MOD_##dst_mod)
#define OM(op_mod) BITFIELD64_BIT(ROGUE_ALU_OP_MOD_##op_mod)
#define PH(type) ROGUE_INSTR_PHASE_##type
#define IO(io) BITFIELD64_BIT(ROGUE_IO_##io)
#define T(type) BITFIELD64_BIT(ROGUE_REF_TYPE_##type - 1)
#define B(n) BITFIELD64_BIT(n)
const rogue_alu_op_info rogue_alu_op_infos[ROGUE_ALU_OP_COUNT] = {
   [ROGUE_ALU_OP_INVALID] = { .str = "!INVALID!", },
   [ROGUE_ALU_OP_MBYP0] = { .str = "mbyp0", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_MBYP1] = { .str = "mbyp1", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(1),
      .io = { .dst_set[0] = IO(FT1), .src_set[0] = IO(S3), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_FRCP] = { .str = "frcp", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FRSQ] = { .str = "frsq", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FLOG2] = { .str = "flog2", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FLOGCN] = { .str = "flogcn", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FEXP2] = { .str = "fexp2", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FDSX] = { .str = "fdsx", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FDSY] = { .str = "fdsy", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FDSXF] = { .str = "fdsxf", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FDSYF] = { .str = "fdsyf", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FSINC] = { .str = "fsinc", .num_dsts = 2, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
         [1] = T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FARCTANC] = { .str = "farctanc", .num_dsts = 1, .num_srcs = 1,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .src_set[0] = IO(S0), },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
      },
   },
   [ROGUE_ALU_OP_FRED] = { .str = "fred", .num_dsts = 3, .num_srcs = 3,
      .whole_pipeline = true,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(W0), .dst_set[1] = IO(W1), .src_set[1] = IO(S0), .src_set[2] = IO(S3), },
      .supported_op_mods = OM(PARTA) | OM(PARTB) | OM(SIN) | OM(COS),
      .supported_src_mods = {
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
         [2] = T(IO),
      },
      .supported_src_types = {
         [0] = T(VAL),
         [1] = T(REG) | T(REGARRAY),
         [2] = T(REG) | T(REGARRAY) | T(IO),
      },
   },
   [ROGUE_ALU_OP_FADD] = { .str = "fadd", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), .src_set[1] = IO(S1), },
      .supported_op_mods = OM(LP) | OM(SAT),
      .supported_src_mods = {
         [0] = SM(FLR) | SM(ABS) | SM(NEG),
         [1] = SM(ABS),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_FMUL] = { .str = "fmul", .num_dsts = 1, .num_srcs = 2,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), .src_set[1] = IO(S1), },
      .supported_op_mods = OM(LP) | OM(SAT),
      .supported_src_mods = {
         [0] = SM(FLR) | SM(ABS) | SM(NEG),
         [1] = SM(ABS),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_FMAD] = { .str = "fmad", .num_dsts = 1, .num_srcs = 3,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), .src_set[1] = IO(S1), .src_set[2] = IO(S2), },
      .supported_op_mods = OM(LP) | OM(SAT),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
         [2] = SM(FLR) | SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   /* TODO NEXT!: Validate - can/must only select element if non-32-bit type, element has to be same for both args if both args present, 16-bit must be 0 or 1, 32-bit must be 0-3 (can't have no element set)
    * Also validate number of sources provided/nulled out based on test op */
   [ROGUE_ALU_OP_TST] = { .str = "tst", .num_dsts = 2, .num_srcs = 2,
      .phase = PH(2_TST),
      .io = { .src_set[0] = IO(IS1), .src_set[1] = IO(IS2), },
      .supported_op_mods = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) |
         OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32),
      .supported_src_mods = {
         [0] = SM(E0) | SM(E1) | SM(E2) | SM(E3),
         [1] = SM(E0) | SM(E1) | SM(E2) | SM(E3),
      },
      .supported_dst_types = { [0] = T(IO), [1] = T(IO), }, /* FTT and either P0 or NONE */
      .supported_src_types = {
         [0] = T(REG) | T(IO),
         [1] = T(REG) | T(IO) | T(IMM),
      },
   },
   /* TODO: Support fully. */
   [ROGUE_ALU_OP_MOVC] = { .str = "movc", .num_dsts = 2, .num_srcs = 3,
      .phase = PH(2_MOV),
      .io = {
         .dst_set[0] = IO(W0),
         .src_set[1] = IO(FT0) | IO(FT1) | IO(FT2) | IO(FTE),
      },
      .supported_dst_mods = {
         [0] = DM(E0) | DM(E1) | DM(E2) | DM(E3),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), [1] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
         [2] = T(REG) | T(REGARRAY) | T(IO),
      },
   },
   [ROGUE_ALU_OP_ADD64] = { .str = "add64", .num_dsts = 3, .num_srcs = 5,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .dst_set[1] = IO(FT0H), .src_set[0] = IO(S0), .src_set[1] = IO(S1), .src_set[2] = IO(S2), .src_set[3] = IO(IS0), },
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
         [2] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), [1] = T(REG) | T(REGARRAY) | T(IO), [2] = T(IO) },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
         [3] = T(REG) | T(REGARRAY)| T(IO) | T(IMM),
         [4] = T(IO),
      },
   },
   [ROGUE_ALU_OP_ADD64_32] = { .str = "add6432", .num_dsts = 2, .num_srcs = 4,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .dst_set[1] = IO(FT0H), .src_set[0] = IO(S0), .src_set[1] = IO(S1), .src_set[2] = IO(S2), },
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
         [2] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), [1] = T(REG) | T(REGARRAY) | T(IO) },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
         [3] = T(IO),
      },
   },
   [ROGUE_ALU_OP_MADD32] = { .str = "madd32", .num_dsts = 2, .num_srcs = 4,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .dst_set[1] = IO(FT0H), .src_set[0] = IO(S0), .src_set[1] = IO(S1), .src_set[2] = IO(S2), },
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
         [2] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), [1] = T(REG) | T(REGARRAY) | T(IO) },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
         [3] = T(IO),
      },
   },
   [ROGUE_ALU_OP_MADD64] = { .str = "madd64", .num_dsts = 2, .num_srcs = 5,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .dst_set[1] = IO(FT0H), .src_set[0] = IO(S0), .src_set[1] = IO(S1), .src_set[2] = IO(S2), .src_set[3] = IO(IS0), },
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
         [2] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IO),
      },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
         [3] = T(REG) | T(REGARRAY)| T(IO) | T(IMM),
         [4] = T(IO),
      },
   },
   [ROGUE_ALU_OP_PCK_U8888] = { .str = "pck.u8888", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(SCALE) | OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 4,
   },
   [ROGUE_ALU_OP_PCK_S8888] = { .str = "pck.s8888", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(SCALE) | OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 4,
   },
   [ROGUE_ALU_OP_PCK_U1616] = { .str = "pck.u1616", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(SCALE) | OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 2,
   },
   [ROGUE_ALU_OP_PCK_S1616] = { .str = "pck.s1616", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(SCALE) | OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 2,
   },
   [ROGUE_ALU_OP_PCK_F16F16] = { .str = "pck.f16f16", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(SCALE) | OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 2,
   },
   [ROGUE_ALU_OP_PCK_U32] = { .str = "pck.u32", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 1,
   },
   [ROGUE_ALU_OP_PCK_S32] = { .str = "pck.s32", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_op_mods = OM(ROUNDZERO),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 1,
   },
   [ROGUE_ALU_OP_PCK_F32] = { .str = "pck.f32", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 1,
   },
   [ROGUE_ALU_OP_PCK_2F10F10F10] = { .str = "pck.2f10f10f10", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(2_PCK),
      .io = { .dst_set[0] = IO(FT2), .src_set[0] = IO(IS3), },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
      .src_repeat_mask = B(0),
      .max_repeat = 4,
   },
   [ROGUE_ALU_OP_UPCK_S32] = { .str = "upck.s32", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_op_mods = OM(ROUNDZERO),
      .supported_src_mods = {
         [0] = SM(E0) | SM(E1) | SM(E2) | SM(E3),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
   },
   [ROGUE_ALU_OP_UPCK_U32] = { .str = "upck.u32", .num_dsts = 1, .num_srcs = 1,
      .phase = PH(0),
      .io = { .dst_set[0] = IO(FT0), .src_set[0] = IO(S0), },
      .supported_op_mods = OM(ROUNDZERO),
      .supported_src_mods = {
         [0] = SM(E0) | SM(E1) | SM(E2) | SM(E3),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY) | T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IO),
      },
   },
   [ROGUE_ALU_OP_MOV] = { .str = "mov", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_CMOV] = { .str = "cmov", .num_dsts = 1, .num_srcs = 3,
      .supported_dst_types = { [0] = T(REG), },
      .supported_src_types = {
         [0] = T(IO),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_SETPRED] = { .str = "setpred", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(IO), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_CNDB] = { .str = "cndb", .num_dsts = 1, .num_srcs = 2,
      .supported_op_mods = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) |
         OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_CNDSEL] = { .str = "cndsel", .num_dsts = 1, .num_srcs = 2,
      .supported_op_mods = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) |
         OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_ZEROSEL] = { .str = "zerosel", .num_dsts = 1, .num_srcs = 3,
      .supported_op_mods = OM(Z) | OM(GZ) | OM(GEZ) | OM(C) | OM(E) | OM(G) | OM(GE) | OM(NE) | OM(L) | OM(LE) |
         OM(F32) | OM(U16) | OM(S16) | OM(U8) | OM(S8) | OM(U32) | OM(S32),
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
         [2] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_FABS] = { .str = "fabs", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_FNEG] = { .str = "fneg", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_FNABS] = { .str = "fnabs", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_FFLR] = { .str = "fflr", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_IADD32] = { .str = "iadd32", .num_dsts = 1, .num_srcs = 2,
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_IADD64] = { .str = "iadd64", .num_dsts = 1, .num_srcs = 2,
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REGARRAY),
         [1] = T(REGARRAY),
      },
      .dst_stride = {
         [0] = 1,
      },
      .src_stride = {
         [0] = 1,
         [1] = 1,
      },
   },
   [ROGUE_ALU_OP_IMUL32] = { .str = "imul32", .num_dsts = 1, .num_srcs = 2,
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_IMUL64] = { .str = "imul64", .num_dsts = 1, .num_srcs = 2,
      .supported_op_mods = OM(S),
      .supported_src_mods = {
         [0] = SM(ABS) | SM(NEG),
         [1] = SM(ABS) | SM(NEG),
      },
      .supported_dst_types = { [0] = T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
      .dst_stride = {
         [0] = 1,
      },
   },
   [ROGUE_ALU_OP_UMUL_HIGH] = { .str = "umul_high", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_UMUL_LOW] = { .str = "umul_low", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_IMUL_HIGH] = { .str = "imul_high", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_IMUL_LOW] = { .str = "imul_low", .num_dsts = 1, .num_srcs = 2,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = {
         [0] = T(REG) | T(REGARRAY) | T(IMM),
         [1] = T(REG) | T(REGARRAY) | T(IMM),
      },
   },
   [ROGUE_ALU_OP_INEG32] = { .str = "ineg32", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_INEG64] = { .str = "ineg64", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_IABS32] = { .str = "iabs32", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
   [ROGUE_ALU_OP_IABS64] = { .str = "iabs64", .num_dsts = 1, .num_srcs = 1,
      .supported_dst_types = { [0] = T(REG) | T(REGARRAY), },
      .supported_src_types = { [0] = T(REG) | T(REGARRAY), },
   },
};
#undef B
#undef T
#undef IO
#undef PH
#undef OM
#undef DM
#undef SM

const char *rogue_exec_cond_str[ROGUE_EXEC_COND_COUNT] = {
   [ROGUE_EXEC_COND_INVALID] = "!INVALID!",
   [ROGUE_EXEC_COND_PE_TRUE] = "if(pe)",
   [ROGUE_EXEC_COND_P0_TRUE] = "if(p0)",
   [ROGUE_EXEC_COND_PE_ANY] = "any(pe)",
   [ROGUE_EXEC_COND_P0_FALSE] = "if(!p0)",
};

const char *rogue_instr_type_str[ROGUE_INSTR_TYPE_COUNT] = {
   [ROGUE_INSTR_TYPE_INVALID] = "!INVALID!",

   [ROGUE_INSTR_TYPE_ALU] = "alu",
   [ROGUE_INSTR_TYPE_BACKEND] = "backend",
   [ROGUE_INSTR_TYPE_CTRL] = "ctrl",
   [ROGUE_INSTR_TYPE_BITWISE] = "bitwise",
   /* [ROGUE_INSTR_TYPE_F16SOP] = "f16sop", */
};

const char *const rogue_alu_str[ROGUE_ALU_COUNT] = {
   [ROGUE_ALU_INVALID] = "!INVALID!",
   [ROGUE_ALU_MAIN] = "main",
   [ROGUE_ALU_BITWISE] = "bitwise",
   [ROGUE_ALU_CONTROL] = "control",
};

const char *const rogue_instr_phase_str[ROGUE_ALU_COUNT][ROGUE_INSTR_PHASE_COUNT] = {
   /** Main/ALU (and backend) instructions. */
   [ROGUE_ALU_MAIN] = {
      [ROGUE_INSTR_PHASE_0] = "p0",
      [ROGUE_INSTR_PHASE_1] = "p1",
      [ROGUE_INSTR_PHASE_2_PCK] = "p2pck",
      [ROGUE_INSTR_PHASE_2_TST] = "p2tst",
      [ROGUE_INSTR_PHASE_2_MOV] = "p2mov",
      [ROGUE_INSTR_PHASE_BACKEND] = "backend",
   },

   /** Bitwise instructions. */
   [ROGUE_ALU_BITWISE] = {
      [ROGUE_INSTR_PHASE_0_BITMASK] = "p0bm",
      [ROGUE_INSTR_PHASE_0_SHIFT1] = "p0shf1",
      [ROGUE_INSTR_PHASE_0_COUNT] = "p0cnt",
      [ROGUE_INSTR_PHASE_1_LOGICAL] = "p1log",
      [ROGUE_INSTR_PHASE_2_SHIFT2] = "p2shf2",
      [ROGUE_INSTR_PHASE_2_TEST] = "p2tst",
   },

   /** Control instructions (no co-issuing). */
   [ROGUE_ALU_CONTROL] = {
      [ROGUE_INSTR_PHASE_CTRL] = "ctrl",
   },
};
