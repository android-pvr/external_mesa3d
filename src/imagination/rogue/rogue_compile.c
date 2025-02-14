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

#include "compiler/shader_enums.h"
#include "compiler/spirv/nir_spirv.h"
#include "nir/nir.h"
#include "rogue.h"
#include "rogue_builder.h"
#include "rogue_op_helpers.h"
#include "util/macros.h"
/* FIXME: Remove once the compiler/driver interface is finalised. */
#include "vulkan/vulkan_core.h"

/**
 * \file rogue_compile.c
 *
 * \brief Contains NIR to Rogue translation functions, and Rogue passes.
 */

/* Helpers. */
static rogue_ref alu_src(rogue_shader *shader,
                         const nir_alu_instr *alu,
                         unsigned src_num,
                         unsigned *src_components,
                         unsigned bits)
{
   bool is_ssa = alu->src[src_num].src.is_ssa;
   assert(nir_src_bit_size(alu->src[src_num].src) == bits);
   assert(util_is_power_of_two_nonzero(bits));
   assert(bits >= 8 && bits <= 64);

   unsigned num_components = nir_src_num_components(alu->src[src_num].src);
   unsigned components_required =
      nir_ssa_alu_instr_src_components(alu, src_num);

   /* No 64-bit vectors. */
   assert(bits != 64 || num_components == 1);

   assert(components_required == 1 || num_components == components_required);

   if (src_components) {
      if (*src_components)
         assert(components_required == *src_components);
      else
         *src_components = components_required;
   }

   unsigned index = is_ssa ? alu->src[src_num].src.ssa->index
                           : alu->src[src_num].src.reg.reg->index;

   /* Special case for 64-bit - just return the whole regarray;
    * use rogue_ssa_ref64 if the components are needed.
    */
   if (bits == 64) {
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0));
   }

   if (num_components > 1) {
      assert(is_ssa);
      /* Select the component. */
      unsigned read_mask = nir_alu_instr_src_read_mask(alu, src_num);
      unsigned component = ffs(read_mask) - 1;
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, components_required, index, component));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, index)
                               : rogue_temp_reg(shader, index));
}

static rogue_ref alu_dst(rogue_shader *shader,
                         const nir_alu_instr *alu,
                         unsigned *dst_components,
                         ASSERTED unsigned bits)
{
   bool is_ssa = alu->dest.dest.is_ssa;
   assert(nir_dest_bit_size(alu->dest.dest) == bits);
   assert(util_is_power_of_two_nonzero(bits));
   assert(bits >= 8 && bits <= 64);

   unsigned num_components = nir_dest_num_components(alu->dest.dest);

   /* No 64-bit vectors. */
   assert(bits != 64 || num_components == 1);

   if (dst_components) {
      if (*dst_components)
         assert(num_components == *dst_components);
      else
         *dst_components = num_components;
   }

   unsigned index = is_ssa ? alu->dest.dest.ssa.index
                           : alu->dest.dest.reg.reg->index;

   /* Special case for 64-bit - just return the whole regarray;
    * use rogue_ssa_ref64 if the components are needed.
    */
   if (bits == 64) {
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0));
   }

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, index)
                               : rogue_temp_reg(shader, index));
}

static rogue_ref intr_src(rogue_shader *shader,
                          const nir_intrinsic_instr *intr,
                          unsigned src_num,
                          unsigned *src_components,
                          unsigned bits)
{
   bool is_ssa = intr->src[src_num].is_ssa;
   assert(nir_src_bit_size(intr->src[src_num]) == bits);
   assert(util_is_power_of_two_nonzero(bits));
   assert(bits >= 8 && bits <= 64);

   unsigned num_components = nir_src_num_components(intr->src[src_num]);

   /* No 64-bit vectors. */
   assert(bits != 64 || num_components == 1);

   if (src_components)
      *src_components = num_components;

   unsigned index = is_ssa ? intr->src[src_num].ssa->index
                           : intr->src[src_num].reg.reg->index;

   /* Special case for 64-bit - just return the whole regarray;
    * use rogue_ssa_ref64 if the components are needed.
    */
   if (bits == 64) {
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0));
   }

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, index)
                               : rogue_temp_reg(shader, index));
}

static rogue_ref intr_dst(rogue_shader *shader,
                          const nir_intrinsic_instr *intr,
                          unsigned *dst_components,
                          ASSERTED unsigned bits)
{
   bool is_ssa = intr->dest.is_ssa;
   assert(nir_dest_bit_size(intr->dest) == bits);
   assert(util_is_power_of_two_nonzero(bits));
   assert(bits >= 8 && bits <= 64);

   unsigned num_components = nir_dest_num_components(intr->dest);

   /* No 64-bit vectors. */
   assert(bits != 64 || num_components == 1);

   if (dst_components)
      *dst_components = num_components;

   unsigned index = is_ssa ? intr->dest.ssa.index : intr->dest.reg.reg->index;

   /* Special case for 64-bit - just return the whole regarray;
    * use rogue_ssa_ref64 if the components are needed.
    */
   if (bits == 64) {
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0));
   }

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, index)
                               : rogue_temp_reg(shader, index));
}

static rogue_ref nir_tex_src32(rogue_shader *shader,
                               const nir_tex_instr *tex,
                               unsigned src_num,
                               unsigned *src_components)
{
   bool is_ssa = tex->src[src_num].src.is_ssa;
   assert(nir_src_bit_size(tex->src[src_num].src) == 32);

   unsigned num_components = nir_src_num_components(tex->src[src_num].src);
   ASSERTED unsigned components_required = nir_tex_instr_src_size(tex, src_num);

   assert(num_components == components_required);

   if (src_components)
      *src_components = num_components;

   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader,
                                num_components,
                                tex->src[src_num].src.ssa->index,
                                0));
   }

   return rogue_ref_reg(
      is_ssa ? rogue_ssa_reg(shader, tex->src[src_num].src.ssa->index)
             : rogue_temp_reg(shader, tex->src[src_num].src.reg.reg->index));
}

static rogue_ref nir_tex_src32_component(rogue_shader *shader,
                                         const nir_tex_instr *tex,
                                         unsigned src_num,
                                         unsigned comp_num)
{
   bool is_ssa = tex->src[src_num].src.is_ssa;
   assert(nir_src_bit_size(tex->src[src_num].src) == 32);

   unsigned num_components = nir_src_num_components(tex->src[src_num].src);
   ASSERTED unsigned components_required = nir_tex_instr_src_size(tex, src_num);

   assert(num_components == components_required);

   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader,
                                1,
                                tex->src[src_num].src.ssa->index,
                                comp_num));
   }

   assert(comp_num == 0);

   return rogue_ref_reg(
      is_ssa ? rogue_ssa_reg(shader, tex->src[src_num].src.ssa->index)
             : rogue_temp_reg(shader, tex->src[src_num].src.reg.reg->index));
}

static rogue_ref nir_tex_dst32(rogue_shader *shader,
                               const nir_tex_instr *tex,
                               unsigned *dst_components,
                               bool *is_16bit)
{
   bool is_ssa = tex->dest.is_ssa;
   assert(nir_dest_bit_size(tex->dest) == 32 ||
          nir_dest_bit_size(tex->dest) == 16);

   unsigned num_components = nir_dest_num_components(tex->dest);
   ASSERTED unsigned components_required = nir_tex_instr_result_size(tex);

   assert(num_components == components_required);

   if (dst_components)
      *dst_components = num_components;

   if (is_16bit)
      *is_16bit = (nir_dest_bit_size(tex->dest) == 16);

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, tex->dest.ssa.index, 0));
   }

   return rogue_ref_reg(is_ssa
                           ? rogue_ssa_reg(shader, tex->dest.ssa.index)
                           : rogue_temp_reg(shader, tex->dest.reg.reg->index));
}

static rogue_ref
nir_dst32_component(rogue_shader *shader, nir_dest dest, unsigned comp_num)
{
   bool is_ssa = dest.is_ssa;
   assert(nir_dest_bit_size(dest) == 32 || nir_dest_bit_size(dest) == 16);

   unsigned num_components = nir_dest_num_components(dest);

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, 1, dest.ssa.index, comp_num));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, dest.ssa.index)
                               : rogue_temp_reg(shader, dest.reg.reg->index));
}

static rogue_ref
rogue_nir_src32(rogue_shader *shader, nir_src src, unsigned *src_components)
{
   bool is_ssa = src.is_ssa;
   assert(nir_src_bit_size(src) == 32);

   unsigned num_components = nir_src_num_components(src);

   if (src_components)
      *src_components = num_components;

   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, src.ssa->index, 0));
   }

   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, src.ssa->index)
                               : rogue_temp_reg(shader, src.reg.reg->index));
}

static rogue_ref
rogue_nir_src32_component(rogue_shader *shader, nir_src src, unsigned comp_num)
{
   bool is_ssa = src.is_ssa;
   assert(nir_src_bit_size(src) == 32);

   if (nir_src_num_components(src) > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, 1, src.ssa->index, comp_num));
   }

   assert(comp_num == 0);
   return rogue_ref_reg(is_ssa ? rogue_ssa_reg(shader, src.ssa->index)
                               : rogue_temp_reg(shader, src.reg.reg->index));
}

static rogue_ref nir_intr_dst32_component(rogue_shader *shader,
                                          const nir_intrinsic_instr *intr,
                                          unsigned component)
{
   assert(intr->dest.is_ssa);
   assert(nir_dest_bit_size(intr->dest) == 32);

   unsigned num_components = nir_dest_num_components(intr->dest);
   assert(num_components > 1 || component == 0);
   return rogue_ref_regarray(
      rogue_ssa_vec_regarray(shader, 1, intr->dest.ssa.index, component));
}

/* 64-bit restricted to scalars. */
static rogue_ref64 nir_ssa_alu_src64(rogue_shader *shader,
                                     const nir_alu_instr *alu,
                                     unsigned src_num)
{
   assert(alu->src[src_num].src.is_ssa);
   assert(nir_src_bit_size(alu->src[src_num].src) == 64);
   assert(nir_src_num_components(alu->src[src_num].src) == 1);
   assert(nir_ssa_alu_instr_src_components(alu, src_num) == 1);

   return rogue_ssa_ref64(shader, alu->src[src_num].src.ssa->index);
}

static rogue_ref64 nir_ssa_alu_dst64(rogue_shader *shader,
                                     const nir_alu_instr *alu)
{
   assert(alu->dest.dest.is_ssa);
   assert(nir_dest_bit_size(alu->dest.dest) == 64);
   assert(nir_dest_num_components(alu->dest.dest) == 1);

   return rogue_ssa_ref64(shader, alu->dest.dest.ssa.index);
}

static rogue_ref64 nir_ssa_intr_src64(rogue_shader *shader,
                                      const nir_intrinsic_instr *intr,
                                      unsigned src_num)
{
   assert(intr->src[src_num].is_ssa);
   assert(nir_src_bit_size(intr->src[src_num]) == 64);
   assert(nir_src_num_components(intr->src[src_num]) == 1);

   return rogue_ssa_ref64(shader, intr->src[src_num].ssa->index);
}

static rogue_ref64 nir_ssa_intr_dst64(rogue_shader *shader,
                                      const nir_intrinsic_instr *intr)
{
   assert(intr->dest.is_ssa);
   assert(nir_dest_bit_size(intr->dest) == 64);
   assert(nir_dest_num_components(intr->dest) == 1);

   return rogue_ssa_ref64(shader, intr->dest.ssa.index);
}

static rogue_ref nir_shared_reg_indexed(rogue_builder *b,
                                        nir_src index,
                                        unsigned index_comp,
                                        unsigned offset)
{
   if (nir_src_is_const(index))
      return rogue_ref_reg(
         rogue_shared_reg(b->shader,
                          nir_src_comp_as_uint(index, index_comp) + offset));

   rogue_MOV(b,
             rogue_ref_reg(rogue_index_reg(b->shader, 0)),
             rogue_nir_src32_component(b->shader, index, index_comp));

   rogue_reg *dst_val =
      rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0);

   rogue_MOV(b,
             rogue_ref_reg(dst_val),
             rogue_ref_reg_indexed(rogue_shared_reg(b->shader, offset), 0));

   return rogue_ref_reg(dst_val);
}

static rogue_ref64 nir_shared_reg_indexed64(rogue_builder *b,
                                            nir_src index,
                                            unsigned index_comp,
                                            unsigned offset)
{
   if (nir_src_is_const(index))
      return rogue_shared_ref64(b->shader,
                                nir_src_comp_as_uint(index, index_comp) +
                                   offset);

   rogue_MOV(b,
             rogue_ref_reg(rogue_index_reg(b->shader, 0)),
             rogue_nir_src32_component(b->shader, index, index_comp));

   rogue_ref64 dst_val =
      rogue_ssa_ref64(b->shader, b->shader->ctx->next_ssa_idx++);

   rogue_MOV(b,
             dst_val.lo32,
             rogue_ref_reg_indexed(rogue_shared_reg(b->shader, offset), 0));

   rogue_MOV(b,
             dst_val.hi32,
             rogue_ref_reg_indexed(rogue_shared_reg(b->shader, offset + 1), 0));

   return dst_val;
}

static inline nir_alu_type nir_cmp_type(nir_op op)
{
   switch (op) {
   case nir_op_fcsel:
   case nir_op_fcsel_gt:
   case nir_op_fcsel_ge:
   case nir_op_flt32:
   case nir_op_fge32:
   case nir_op_feq32:
   case nir_op_fneu32:
      return nir_type_float;

   case nir_op_i32csel_gt:
   case nir_op_i32csel_ge:
   case nir_op_ige32:
   case nir_op_ilt32:
   case nir_op_ieq32:
   case nir_op_ine32:
      return nir_type_int;

   case nir_op_b32csel:
   case nir_op_ult32:
   case nir_op_uge32:
      return nir_type_uint;

   default:
      break;
   }

   unreachable();
}

static inline enum compare_func nir_cmp_func(nir_op op)
{
   switch (op) {
   case nir_op_flt32:
   case nir_op_ilt32:
   case nir_op_ult32:
      return COMPARE_FUNC_LESS;

   case nir_op_fcsel_gt:
   case nir_op_i32csel_gt:
      return COMPARE_FUNC_GREATER;

   case nir_op_fcsel_ge:
   case nir_op_i32csel_ge:
   case nir_op_fge32:
   case nir_op_ige32:
   case nir_op_uge32:
      return COMPARE_FUNC_GEQUAL;

   case nir_op_fcsel:
   case nir_op_b32csel:
   case nir_op_feq32:
   case nir_op_ieq32:
      return COMPARE_FUNC_EQUAL;

   case nir_op_fneu32:
   case nir_op_ine32:
      return COMPARE_FUNC_NOTEQUAL;

   default:
      break;
   }

   unreachable();
}

static void
trans_nir_jump_break_cont(rogue_builder *b, nir_jump_instr *jump, bool cont)
{
   /* Conditional mask count register. */
   rogue_ref emc = rogue_ref_emc(b->shader);

   rogue_alu_instr *mov =
      rogue_MOV(b,
                emc,
                rogue_ref_imm(b->shader->loop_nestings + (cont ? 1 : 2)));
   rogue_add_instr_comment(&mov->instr, cont ? "continue" : "break");

   rogue_ctrl_instr *cnd =
      rogue_CNDEF(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(0));
   rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_NEVER);
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
   rogue_add_instr_comment(&cnd->instr, "flush_Pe");
   rogue_push_block(b);
}

static void trans_nir_jump(rogue_builder *b, nir_jump_instr *jump)
{
   switch (jump->type) {
   case nir_jump_break:
      return trans_nir_jump_break_cont(b, jump, false);

   case nir_jump_continue:
      return trans_nir_jump_break_cont(b, jump, true);

   default:
      break;
   }

   unreachable("Unsupported NIR jump instruction type.");
}

struct rogue_nir_tex_smp_info {
   unsigned channels;
   bool pack_f16 : 1;
   bool fcnorm : 1;
   bool is_array : 1;
   bool layer_is_cube_idx : 1;
   bool int_coord : 1;
   bool nn_coord : 1;
   bool point_sampler : 1;
   bool lod_bias : 1;
   bool is_gather : 1;
   enum glsl_sampler_dim dim;
   unsigned image_base;
   unsigned sampler_base;
   nir_src *coords;
   nir_src *proj;
   nir_src *lod;
   nir_src *ddx;
   nir_src *ddy;
   nir_src *offset;
   nir_src *ms_idx;
   unsigned secondary_comp;
   nir_src *secondary_idx;
   nir_src *image_idx;
   nir_src *sampler_idx;
   nir_src *store_data;
};

static rogue_backend_instr *
rogue_nir_emit_texture_sample(rogue_builder *b,
                              rogue_ref dst,
                              struct rogue_nir_tex_smp_info *info)
{
   const struct pvr_device_info *dev_info = b->shader->ctx->compiler->dev_info;

   unsigned coord_components;
   switch (info->dim) {
   case GLSL_SAMPLER_DIM_1D:
   case GLSL_SAMPLER_DIM_BUF:
      coord_components = 1;
      break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_MS:
   case GLSL_SAMPLER_DIM_SUBPASS:
   case GLSL_SAMPLER_DIM_SUBPASS_MS:
      coord_components = 2;
      break;
   case GLSL_SAMPLER_DIM_CUBE:
      if (info->int_coord) {
         coord_components = 2;
         info->is_array = true;
      } else {
         coord_components = 3;
      }
      break;
   case GLSL_SAMPLER_DIM_3D:
      coord_components = 3;
      break;
   default:
      unreachable("Unsupported glsl_sampler_dim");
   }

   assert(info->coords);
   unsigned smp_data_components = coord_components;

   if (info->is_array && !PVR_HAS_FEATURE(dev_info, tpu_array_textures))
      smp_data_components += 2;

   if (info->proj) {
      assert(nir_src_num_components(*info->proj) == 1);
      smp_data_components++;
   }

   if (info->lod || info->is_gather) {
      assert(!(info->lod && info->is_gather));
      assert(!info->lod || (nir_src_num_components(*info->lod) == 1));
      smp_data_components++;
   }
   if (info->ddx || info->ddy) {
      assert(info->ddx && info->ddy);
      assert(nir_src_num_components(*info->ddx) ==
             nir_src_num_components(*info->ddy));
      assert(nir_src_num_components(*info->ddx) == coord_components);
      smp_data_components += coord_components;
   }
   if (info->ms_idx || info->offset) {
      assert(!info->ms_idx || (nir_src_num_components(*info->ms_idx) == 1));
      assert(!info->offset ||
             (nir_src_num_components(*info->offset) == coord_components));
      smp_data_components++;
   }
   if (info->store_data) {
      assert(nir_src_num_components(*info->store_data) == info->channels);
      smp_data_components += info->channels;
   }

   rogue_ref smp_data_ref;
   if (smp_data_components == coord_components) {
      smp_data_ref = rogue_nir_src32(b->shader, *info->coords, NULL);
   } else {
      /* Move all the data into contiguous temp regs */
      unsigned data_base_idx = b->shader->ctx->next_ssa_idx;
      ++b->shader->ctx->next_ssa_idx;
      rogue_regarray *smp_data = rogue_ssa_vec_regarray(b->shader,
                                                        smp_data_components,
                                                        data_base_idx,
                                                        0);
      unsigned data_idx = 0;

#define ADD_SMP_DATA(nir_src, component)                                \
   do {                                                                 \
      rogue_regarray *data =                                            \
         rogue_ssa_vec_regarray(b->shader, 1, data_base_idx, data_idx); \
      rogue_ref src =                                                   \
         rogue_nir_src32_component(b->shader, *nir_src, component);     \
      rogue_MOV(b, rogue_ref_regarray(data), src);                      \
      ++data_idx;                                                       \
   } while (0)

      for (unsigned i = 0; i < coord_components; i++)
         ADD_SMP_DATA(info->coords, i);

      if (info->proj)
         ADD_SMP_DATA(info->proj, 0);

      if (info->lod) {
         ADD_SMP_DATA(info->lod, 0);
      } else if (info->is_gather) {
         rogue_regarray *data =
            rogue_ssa_vec_regarray(b->shader, 1, data_base_idx, data_idx++);
         rogue_MOV(b, rogue_ref_regarray(data), rogue_ref_imm(0));
      }

      if (info->ddx) {
         for (unsigned i = 0; i < coord_components; i++) {
            ADD_SMP_DATA(info->ddx, i);
            ADD_SMP_DATA(info->ddy, i);
         }
      }

      if (info->is_array && !PVR_HAS_FEATURE(dev_info, tpu_array_textures)) {
         assert(info->secondary_idx);
         bool cube_array = info->is_array &&
                           (info->dim == GLSL_SAMPLER_DIM_CUBE);

         rogue_ref layer_src = rogue_nir_src32_component(b->shader,
                                                         *info->coords,
                                                         coord_components);

         if (!info->int_coord) {
            rogue_ref layer_src_float = layer_src;
            layer_src = rogue_ref_reg(
               rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
            rogue_PCK_U32(b, layer_src, layer_src_float);
         }

         rogue_ref layer_max = rogue_ref_reg(
            rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
         rogue_alu_instr *max =
            rogue_MAX(b, layer_max, layer_src, rogue_ref_imm(0));
         rogue_set_alu_op_mod(max, ROGUE_ALU_OP_MOD_S32);

         rogue_ref layer = rogue_ref_reg(
            rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
         rogue_ref max_layer_index = nir_shared_reg_indexed(
            b,
            *info->secondary_idx,
            info->secondary_comp,
            PVR_DESC_IMAGE_SECONDARY_OFFSET_ARRAYMAXINDEX(dev_info));

         if (cube_array && !info->layer_is_cube_idx) {
            /* max_cube_idx = view_max_layer_index / 6 - 1
             * =>
             * max_layer_index = 6 * (max_cube_idx + 1) - 1
             * =>
             * max_layer_index = 6 * max_cube_idx + 5
             */
            rogue_ref max_cube_idx = max_layer_index;
            max_layer_index = rogue_ref_reg(
               rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
            rogue_MADD32(b,
                         max_layer_index,
                         rogue_none(),
                         max_cube_idx,
                         rogue_ref_imm(6),
                         rogue_ref_imm(5),
                         rogue_none());
         }

         rogue_alu_instr *min = rogue_MIN(b, layer, layer_max, max_layer_index);
         rogue_set_alu_op_mod(min, ROGUE_ALU_OP_MOD_S32);

         if (cube_array && info->layer_is_cube_idx) {
            rogue_ref cube_idx = layer;
            layer = rogue_ref_reg(
               rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
            rogue_IMUL32(b, layer, cube_idx, rogue_ref_imm(6));
         }

         rogue_ref64 addr_base =
            nir_shared_reg_indexed64(b,
                                     *info->secondary_idx,
                                     info->secondary_comp,
                                     PVR_DESC_IMAGE_SECONDARY_OFFSET_ARRAYBASE);
         rogue_ref addr_stride =
            nir_shared_reg_indexed(b,
                                   *info->secondary_idx,
                                   info->secondary_comp,
                                   PVR_DESC_IMAGE_SECONDARY_OFFSET_ARRAYSTRIDE);
         rogue_ref64 addr_override =
            rogue_ssa_ref64(b->shader, b->shader->ctx->next_ssa_idx++);

         rogue_MADD64(b,
                      addr_override.lo32,
                      addr_override.hi32,
                      addr_stride,
                      layer,
                      addr_base.lo32,
                      addr_base.hi32,
                      rogue_none());

         rogue_MOV(
            b,
            rogue_ref_regarray(
               rogue_ssa_vec_regarray(b->shader, 1, data_base_idx, data_idx++)),
            addr_override.lo32);
         rogue_MOV(
            b,
            rogue_ref_regarray(
               rogue_ssa_vec_regarray(b->shader, 1, data_base_idx, data_idx++)),
            addr_override.hi32);
      }

#define ADD_SMP_OPT(src, comp, bits, shift)                                \
   do {                                                                    \
      rogue_ref temp_and = rogue_ref_reg(                                  \
         rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0)); \
      rogue_ref temp_shl = rogue_ref_reg(                                  \
         rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0)); \
      rogue_ref next_smp_opts = rogue_ref_reg(                             \
         rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0)); \
      rogue_IAND(b,                                                        \
                 temp_and,                                                 \
                 rogue_nir_src32_component(b->shader, *src, comp),         \
                 rogue_ref_imm((1 << bits) - 1));                          \
      rogue_ISHL(b, temp_shl, temp_and, rogue_ref_imm(shift));             \
      rogue_IOR(b, next_smp_opts, smp_opts, temp_shl);                     \
      smp_opts = next_smp_opts;                                            \
   } while (0)

      if (info->ms_idx || info->offset) {
         rogue_ref smp_opts = rogue_ref_imm(0);
         if (info->offset) {
            for (unsigned i = 0; i < coord_components; i++)
               ADD_SMP_OPT(info->offset, i, 5, 5 * i);
         }

         if (info->ms_idx)
            ADD_SMP_OPT(info->ms_idx, 0, 3, 16);

         rogue_MOV(
            b,
            rogue_ref_regarray(
               rogue_ssa_vec_regarray(b->shader, 1, data_base_idx, data_idx)),
            smp_opts);
         ++data_idx;
      }

      if (info->store_data) {
         /* store data comes in pre-packed. */
         for (unsigned i = 0; i < info->channels; i++)
            ADD_SMP_DATA(info->store_data, i);
      }

#undef ADD_SMP_OPT
#undef ADD_SMP_DATA

      assert(data_idx == smp_data_components);
      smp_data_ref = rogue_ref_regarray(smp_data);
   }

   rogue_ref image_state;
   if (info->image_idx) {
      rogue_MOV(b,
                rogue_ref_reg(rogue_index_reg(b->shader, 0)),
                rogue_nir_src32_component(b->shader, *info->image_idx, 0));

      image_state =
         rogue_ref_reg_indexed(rogue_shared_reg(b->shader, info->image_base),
                               0);
   } else {
      image_state = rogue_ref_regarray(
         rogue_shared_regarray(b->shader, 4, info->image_base));
   }

   rogue_ref smp_state;
   if (info->point_sampler) {
      enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
      const struct pvr_pipeline_layout *pipeline_layout =
         b->shader->ctx->pipeline_layout;
      smp_state = rogue_ref_regarray(rogue_shared_regarray(
         b->shader,
         4,
         pipeline_layout->point_sampler_in_dwords_per_stage[pvr_stage]));
   } else if (info->sampler_idx) {
      rogue_MOV(b,
                rogue_ref_reg(rogue_index_reg(b->shader, 1)),
                rogue_nir_src32_component(b->shader, *info->sampler_idx, 0));

      smp_state =
         rogue_ref_reg_indexed(rogue_shared_reg(b->shader, info->sampler_base),
                               1);
   } else {
      smp_state = rogue_ref_regarray(
         rogue_shared_regarray(b->shader, 4, info->sampler_base));
   }

   rogue_backend_instr *smp;

   switch (coord_components) {
   case 1:
      smp = rogue_SMP1D(b,
                        dst,
                        rogue_ref_drc(0),
                        image_state,
                        smp_data_ref,
                        smp_state,
                        rogue_none(),
                        rogue_ref_val(info->channels));
      break;
   case 2:
      smp = rogue_SMP2D(b,
                        dst,
                        rogue_ref_drc(0),
                        image_state,
                        smp_data_ref,
                        smp_state,
                        rogue_none(),
                        rogue_ref_val(info->channels));
      break;
   case 3:
      smp = rogue_SMP3D(b,
                        dst,
                        rogue_ref_drc(0),
                        image_state,
                        smp_data_ref,
                        smp_state,
                        rogue_none(),
                        rogue_ref_val(info->channels));
      break;
   default:
      unreachable("Invalid coord_components");
   }

   if (info->proj)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_PROJ);

   if (info->lod || info->is_gather) {
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_PPLOD);
      if (info->lod_bias)
         rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_BIAS);
      else
         rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_REPLACE);
   }

   if (info->is_array && !PVR_HAS_FEATURE(dev_info, tpu_array_textures))
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_TAO);

   if (info->ddx)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_GRADIENT);

   if (info->ms_idx)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_SNO);

   if (info->offset)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_SOO);

   assert(!info->int_coord || !info->nn_coord);
   if (info->int_coord)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_INTEGER);
   else if (info->nn_coord)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_NNCOORDS);

   if (info->fcnorm)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_FCNORM);

   if (info->pack_f16)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_F16);

   if (info->store_data)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_WRT);

   if (info->is_gather)
      rogue_set_backend_op_mod(smp, ROGUE_BACKEND_OP_MOD_DATA);

   return smp;
}

static void rogue_nir_emit_gather(rogue_builder *b,
                                  nir_tex_instr *tex,
                                  struct rogue_nir_tex_smp_info *info,
                                  unsigned component)
{
   assert(info->is_gather);

   /* Driver provides gather suitable sampler right after the normal one. */
   info->sampler_base += 4;
   info->channels = 4;
   /* Lod will be replaced with constant 0 with is_gather */
   info->lod = NULL;

   unsigned smp_data_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref smp_data = rogue_ref_regarray(
      rogue_ssa_vec_regarray(b->shader, 2 * 2 * 4, smp_data_idx, 0));

   rogue_nir_emit_texture_sample(b, smp_data, info);

   /*
    *	tg4 wants the samples in this order:
    *   bottom-left, bottom-right, top-right, top-left
    *	whereas the hardware returns
    *   top-left, top-right, bottom-left, bottom-right
    */
   static const unsigned sample_map[] = {
      2, /* Bottom-left */
      3, /* Bottom-right */
      1, /* Top-right */
      0, /* Top-left */
   };
   for (unsigned i = 0; i < ARRAY_SIZE(sample_map); i++) {
      rogue_ref smp_data_comp = rogue_ref_regarray(
         rogue_ssa_vec_regarray(b->shader,
                                1,
                                smp_data_idx,
                                sample_map[i] * 4 + component));
      rogue_ref dst_comp = nir_dst32_component(b->shader, tex->dest, i);
      rogue_MOV(b, dst_comp, smp_data_comp);
   }
}

static void trans_nir_texop_tex(rogue_builder *b, nir_tex_instr *tex)
{
   struct rogue_nir_tex_smp_info info = { 0 };

   bool pack_f16;
   rogue_ref dst = nir_tex_dst32(b->shader, tex, NULL, &pack_f16);
   assert(!pack_f16);

   unsigned channels = nir_dest_num_components(tex->dest);

   assert(channels <= 4);
   assert(!tex->is_shadow);
   assert(!tex->is_new_style_shadow);
   assert(!tex->is_sparse);
   assert(!tex->texture_non_uniform);
   assert(!tex->sampler_non_uniform);

   info.is_gather = (tex->op == nir_texop_tg4);
   info.channels = channels;
   info.dim = tex->sampler_dim;
   info.is_array = tex->is_array;
   info.pack_f16 = pack_f16;
   info.image_base = tex->texture_index;
   info.sampler_base = tex->sampler_index;
   info.layer_is_cube_idx = true;
   if (tex->op == nir_texop_txb)
      info.lod_bias = true;
   if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float)
      info.fcnorm = true;
   if (tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms) {
      info.int_coord = true;
      info.point_sampler = true;
   }

   for (unsigned u = 0; u < tex->num_srcs; ++u) {
      switch (tex->src[u].src_type) {
      case nir_tex_src_coord:
         info.coords = &tex->src[u].src;
         break;
      case nir_tex_src_bias:
      case nir_tex_src_lod:
         info.lod = &tex->src[u].src;
         break;
      case nir_tex_src_projector:
         info.proj = &tex->src[u].src;
         break;
      case nir_tex_src_ddx:
         info.ddx = &tex->src[u].src;
         break;
      case nir_tex_src_ddy:
         info.ddy = &tex->src[u].src;
         break;
      case nir_tex_src_offset:
         info.offset = &tex->src[u].src;
         break;
      case nir_tex_src_ms_index:
         info.ms_idx = &tex->src[u].src;
         break;
      case nir_tex_src_texture_offset:
         info.image_idx = &tex->src[u].src;
         continue;
      case nir_tex_src_sampler_offset:
         info.sampler_idx = &tex->src[u].src;
         continue;
      case nir_tex_src_backend1:
         info.secondary_comp = 0;
         info.secondary_idx = &tex->src[u].src;
         continue;
      default:
         unreachable("Unsupported NIR tex source type.");
      }
   }

   if (tex->op == nir_texop_tg4) {
      rogue_nir_emit_gather(b, tex, &info, tex->component);
      return;
   }

   rogue_nir_emit_texture_sample(b, dst, &info);
}

static void rogue_nir_texture_size(rogue_builder *b,
                                   enum glsl_sampler_dim dim,
                                   bool is_array,
                                   nir_src index,
                                   unsigned index_comp,
                                   nir_dest dest)
{
   const struct pvr_device_info *dev_info = b->shader->ctx->compiler->dev_info;

   unsigned coord_components;
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
   case GLSL_SAMPLER_DIM_BUF:
      coord_components = 1;
      break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_CUBE:
   case GLSL_SAMPLER_DIM_MS:
      coord_components = 2;
      break;
   case GLSL_SAMPLER_DIM_3D:
      coord_components = 3;
      break;
   default:
      unreachable("Unsupported glsl_sampler_dim");
   }

   for (int i = 0; i < coord_components; i++) {
      unsigned offset;
      switch (i) {
      case 0:
         offset = PVR_DESC_IMAGE_SECONDARY_OFFSET_WIDTH(dev_info);
         break;
      case 1:
         offset = PVR_DESC_IMAGE_SECONDARY_OFFSET_HEIGHT(dev_info);
         break;
      case 2:
         offset = PVR_DESC_IMAGE_SECONDARY_OFFSET_DEPTH(dev_info);
         break;
      default:
         unreachable("Invalid coord component count");
      }
      rogue_MOV(b,
                nir_dst32_component(b->shader, dest, i),
                nir_shared_reg_indexed(b, index, index_comp, offset));
   }
   if (is_array) {
      unsigned offset = PVR_DESC_IMAGE_SECONDARY_OFFSET_ARRAYMAXINDEX(dev_info);
      rogue_IADD32(b,
                   nir_dst32_component(b->shader, dest, coord_components),
                   nir_shared_reg_indexed(b, index, index_comp, offset),
                   rogue_ref_imm(1));
   }
}

static void rogue_nir_texture_samples(rogue_builder *b,
                                      unsigned base_index,
                                      nir_src *index,
                                      unsigned index_comp,
                                      nir_dest dest)
{
   rogue_ref smpcnt = rogue_ref_reg(
      rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));

   rogue_ref img_word0;
   if (index)
      img_word0 = nir_shared_reg_indexed(b, *index, index_comp, base_index + 1);
   else
      img_word0 = rogue_ref_reg(rogue_shared_reg(b->shader, base_index + 1));

   /* USHR cannot have shared reg in src0, since s1 cannot encode shared regs */
   rogue_ref shr_src = rogue_ref_reg(
      rogue_ssa_vec_reg(b->shader, b->shader->ctx->next_ssa_idx++, 0));
   rogue_MOV(b, shr_src, img_word0);

   rogue_USHR(b, smpcnt, shr_src, rogue_ref_imm(30));
   rogue_ISHL(b,
              nir_dst32_component(b->shader, dest, 0),
              rogue_ref_imm(1),
              smpcnt);
}

static void rogue_nir_texture_levels(rogue_builder *b,
                                     unsigned base_index,
                                     nir_src *index,
                                     unsigned index_comp,
                                     nir_dest dest)
{
   rogue_ref img_word1;
   if (index)
      img_word1 = nir_shared_reg_indexed(b, *index, index_comp, base_index + 2);
   else
      img_word1 = rogue_ref_reg(rogue_shared_reg(b->shader, base_index + 2));

   rogue_IAND(b,
              nir_dst32_component(b->shader, dest, 0),
              img_word1,
              rogue_ref_imm(0xf));
}

static void trans_nir_texop_query(rogue_builder *b, nir_tex_instr *tex)
{
   unsigned lod_index = ROGUE_REG_UNUSED;
   unsigned secondary_index = ROGUE_REG_UNUSED;
   unsigned texture_offset_src = ROGUE_REG_UNUSED;

   bool pack_f16;
   unsigned dst_components;
   nir_tex_dst32(b->shader, tex, &dst_components, &pack_f16);
   assert(!pack_f16);

   assert(!tex->texture_non_uniform);

   for (unsigned u = 0; u < tex->num_srcs; ++u) {
      unsigned src_components;
      nir_tex_src32(b->shader, tex, u, &src_components);
      switch (tex->src[u].src_type) {
      case nir_tex_src_texture_offset:
         assert(texture_offset_src == ROGUE_REG_UNUSED);
         texture_offset_src = u;
         continue;

      case nir_tex_src_backend1:
         assert(secondary_index == ROGUE_REG_UNUSED);
         secondary_index = u;
         continue;

      case nir_tex_src_lod:
         assert(lod_index == ROGUE_REG_UNUSED);
         assert(nir_src_is_const(tex->src[u].src));
         assert(nir_src_as_uint(tex->src[u].src) == 0);
         lod_index = u;
         continue;

      default:
         unreachable("Unsupported NIR tex source type.");
      }
   }

   if (tex->op == nir_texop_txs) {
      assert(secondary_index != ROGUE_REG_UNUSED);
      return rogue_nir_texture_size(b,
                                    tex->sampler_dim,
                                    tex->is_array,
                                    tex->src[secondary_index].src,
                                    0,
                                    tex->dest);
   }

   if (tex->op == nir_texop_texture_samples) {
      return rogue_nir_texture_samples(b,
                                       tex->texture_index,
                                       texture_offset_src == ROGUE_REG_UNUSED
                                          ? NULL
                                          : &tex->src[texture_offset_src].src,
                                       0,
                                       tex->dest);
   }

   return rogue_nir_texture_levels(b,
                                   tex->texture_index,
                                   texture_offset_src == ROGUE_REG_UNUSED
                                      ? NULL
                                      : &tex->src[texture_offset_src].src,
                                   0,
                                   tex->dest);
}

static void trans_nir_tex(rogue_builder *b, nir_tex_instr *tex)
{
   switch (tex->op) {
   case nir_texop_tex:
   case nir_texop_txb:
   case nir_texop_txl:
   case nir_texop_txd:
   case nir_texop_txf:
   case nir_texop_txf_ms:
   case nir_texop_tg4:
      return trans_nir_texop_tex(b, tex);

   case nir_texop_txs:
   case nir_texop_query_levels:
   case nir_texop_texture_samples:
      return trans_nir_texop_query(b, tex);

   default:
      break;
   }

   unreachable("Unsupported NIR tex instruction op.");
}

static void trans_nir_intrinsic_image(rogue_builder *b,
                                      nir_intrinsic_instr *intr)
{
   struct rogue_nir_tex_smp_info info = {
      .dim = nir_intrinsic_image_dim(intr),
      .is_array = nir_intrinsic_image_array(intr),
      .image_base = 0,
      .sampler_base = 0,
      .int_coord = true,
      .nn_coord = false,
      .point_sampler = true,
      .image_idx = &intr->src[0],
      .secondary_idx = &intr->src[0],
      .secondary_comp = 1,
      .coords = &intr->src[1],
      .ms_idx = (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_MS
                    ? &intr->src[2]
                    : NULL),
      .pack_f16 = false,
      .fcnorm = false,
      .layer_is_cube_idx = false,
      .store_data = NULL,
      .lod_bias = false,
      .lod = NULL,
      .proj = NULL,
      .ddx = NULL,
      .ddy = NULL,
      .offset = NULL,
      .sampler_idx = NULL,
   };
   rogue_ref dst = rogue_ref_null();

   switch (intr->intrinsic) {
   case nir_intrinsic_bindless_image_size:
      return rogue_nir_texture_size(b,
                                    info.dim,
                                    info.is_array,
                                    *info.image_idx,
                                    0,
                                    intr->dest);

   case nir_intrinsic_bindless_image_samples:
      return rogue_nir_texture_samples(b,
                                       info.image_base,
                                       info.image_idx,
                                       0,
                                       intr->dest);

   case nir_intrinsic_bindless_image_load:
      info.lod = &intr->src[3];
      info.channels = nir_dest_num_components(intr->dest);
      info.pack_f16 = (nir_intrinsic_dest_type(intr) == nir_type_float16);
      info.fcnorm = (nir_alu_type_get_base_type(
                        nir_intrinsic_dest_type(intr)) == nir_type_float);
      dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);
      break;

   case nir_intrinsic_bindless_image_store:
      info.lod = &intr->src[4];
      info.store_data = &intr->src[3];
      info.channels = nir_src_num_components(intr->src[3]);
      dst = rogue_ref_regarray(
         rogue_ssa_vec_regarray(b->shader,
                                info.channels,
                                b->shader->ctx->next_ssa_idx++,
                                0));
      break;

   case nir_intrinsic_bindless_image_texel_address:
   default:
      unreachable("Unsupported nir_intrinsic_image op");
   }

   rogue_nir_emit_texture_sample(b, dst, &info);
}

static void trans_nir_load_const_bits(rogue_builder *b,
                                      nir_load_const_instr *load_const,
                                      unsigned bit_size)
{
   unsigned dst_index = load_const->def.index;
   rogue_reg *dst = rogue_ssa_reg(b->shader, dst_index);

   uint32_t imm = nir_const_value_as_uint(load_const->value[0], bit_size);

   rogue_alu_instr *mov = rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_imm(imm));
   rogue_add_instr_commentf(&mov->instr, "load_const_%u", bit_size);
}

static void trans_nir_load_const64(rogue_builder *b,
                                   nir_load_const_instr *load_const)
{
   unsigned dst_index = load_const->def.index;
   rogue_ref64 dst = rogue_ssa_ref64(b->shader, dst_index);

   uint64_t imm = nir_const_value_as_uint(load_const->value[0], 64);
   rogue_ref imm_lo32 = rogue_ref_imm(imm & 0xffffffff);
   rogue_ref imm_hi32 = rogue_ref_imm((imm >> 32) & 0xffffffff);

   rogue_alu_instr *mov = rogue_MOV(b, dst.lo32, imm_lo32);
   rogue_add_instr_comment(&mov->instr, "load_const_64.lo32");

   mov = rogue_MOV(b, dst.hi32, imm_hi32);
   rogue_add_instr_comment(&mov->instr, "load_const_64.hi32");
}

static void trans_nir_load_const(rogue_builder *b,
                                 nir_load_const_instr *load_const)
{
   unsigned bit_size = load_const->def.bit_size;

   switch (bit_size) {
   case 8:
   case 16:
   case 32:
      return trans_nir_load_const_bits(b, load_const, bit_size);

   case 64:
      return trans_nir_load_const64(b, load_const);

   default:
      break;
   }

   unreachable("Unsupported load_const bit size.");
}

static void trans_nir_intrinsic_load_preamble(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   unsigned sh_idx = nir_intrinsic_base(intr);
   rogue_reg *sh_reg = rogue_shared_reg(b->shader, sh_idx);

   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(sh_reg));
   rogue_add_instr_comment(&mov->instr, "load_preamble");
}

static void trans_nir_intrinsic_load_input_fs(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   struct rogue_fs_build_data *fs_data = &b->shader->ctx->stage_data.fs;

   unsigned load_size = 0;
   rogue_ref dst = intr_dst(b->shader, intr, &load_size, 32);
   assert(load_size <= 16);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);

   if (io_semantics.location == VARYING_SLOT_POS && component < 2) {
      rogue_reg *src = NULL;

      switch (component) {
      case 0:
         if (fs_data->msaa_mode == ROGUE_MSAA_MODE_PIXEL)
            src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_X_P);
         else
            src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_X_S);
         break;

      case 1:
         if (fs_data->msaa_mode == ROGUE_MSAA_MODE_PIXEL)
            src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_Y_P);
         else
            src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_Y_S);
         break;
      }
      assert(src);

      rogue_instr *instr = &rogue_MOV(b, dst, rogue_ref_reg(src))->instr;
      rogue_add_instr_commentf(instr,
                               "load_input_fs_coord_%c",
                               'x' + component);
      return;
   }

   unsigned coeff_index = rogue_coeff_index_fs(&fs_data->iterator_args,
                                               io_semantics.location,
                                               component) *
                          ROGUE_COEFF_ALIGN;

   enum glsl_interp_mode mode = rogue_interp_mode_fs(&fs_data->iterator_args,
                                                     io_semantics.location,
                                                     component);

   switch (mode) {
   case INTERP_MODE_NONE:
   case INTERP_MODE_SMOOTH: {
      rogue_regarray *coeffs =
         rogue_coeff_regarray(b->shader,
                              ROGUE_COEFF_ALIGN * load_size,
                              coeff_index);
      unsigned wcoeff_index =
         rogue_coeff_index_fs(&fs_data->iterator_args, ~0, 0) *
         ROGUE_COEFF_ALIGN;
      rogue_regarray *wcoeffs =
         rogue_coeff_regarray(b->shader, ROGUE_COEFF_ALIGN, wcoeff_index);

      rogue_backend_instr *fitrp =
         rogue_FITRP_PIXEL(b,
                           dst,
                           rogue_ref_drc(0),
                           rogue_ref_regarray(coeffs),
                           rogue_ref_regarray(wcoeffs),
                           rogue_ref_val(load_size));
      rogue_add_instr_comment(&fitrp->instr, "load_input_fs_smooth");
      break;
   }

   case INTERP_MODE_NOPERSPECTIVE: {
      rogue_regarray *coeffs =
         rogue_coeff_regarray(b->shader,
                              ROGUE_COEFF_ALIGN * load_size,
                              coeff_index);

      rogue_backend_instr *fitr = rogue_FITR_PIXEL(b,
                                                   dst,
                                                   rogue_ref_drc(0),
                                                   rogue_ref_regarray(coeffs),
                                                   rogue_ref_val(load_size));
      rogue_add_instr_comment(&fitr->instr, "load_input_fs_npc");
      break;
   }

   case INTERP_MODE_FLAT:
      for (unsigned u = 0; u < load_size; ++u) {
         unsigned coeff_c_index =
            coeff_index + u * ROGUE_COEFF_ALIGN + ROGUE_COEFF_COMPONENT_C;
         rogue_reg *coeff_c = rogue_coeff_reg(b->shader, coeff_c_index);

         rogue_alu_instr *mov;
         if (load_size > 1) {
            rogue_ref dst_component =
               nir_intr_dst32_component(b->shader, intr, u);
            mov = rogue_MOV(b, dst_component, rogue_ref_reg(coeff_c));
            rogue_add_instr_commentf(&mov->instr,
                                     "load_input_fs_flat.%c",
                                     'x' + u);
         } else {
            mov = rogue_MOV(b, dst, rogue_ref_reg(coeff_c));
            rogue_add_instr_comment(&mov->instr, "load_input_fs_flat");
         }
      }
      break;

   default:
      unreachable("Unsupported Interpolation mode");
   }
}

static void trans_nir_intrinsic_load_input_vs(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;

   unsigned load_size = 0;
   rogue_ref dst = intr_dst(b->shader, intr, &load_size, 32);
   assert(load_size == 1); /* TODO: support any size loads. */

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned input = io_semantics.location - VERT_ATTRIB_GENERIC0;
   unsigned component = nir_intrinsic_component(intr);
   unsigned vtxin_index = ~0U;

   if (pipeline_layout) {
      rogue_vertex_inputs *vs_inputs = &b->shader->ctx->stage_data.vs.inputs;
      assert(input < vs_inputs->num_input_vars);

      /* Replace components not provided by the driver with 1.0f. */
      if (component >= vs_inputs->components[input]) {
         rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_imm_f(1.0f));
         rogue_add_instr_comment(&mov->instr, "load_input_vs (1.0f)");
         return;
      }

      vtxin_index = vs_inputs->base[input] + component;
   } else {
      /* Dummy defaults for offline compiler. */
      /* TODO: Load these from an offline description
       * if using the offline compiler.
       */

      nir_shader *nir = b->shader->ctx->nir[MESA_SHADER_VERTEX];
      vtxin_index = 0;

      /* Process inputs. */
      nir_foreach_shader_in_variable (var, nir) {
         unsigned input_components = glsl_get_components(var->type);
         unsigned bit_size =
            glsl_base_type_bit_size(glsl_get_base_type(var->type));
         assert(bit_size >= 32); /* TODO: Support smaller bit sizes. */
         unsigned reg_count = bit_size / 32;

         /* Check input location. */
         assert(var->data.location >= VERT_ATTRIB_GENERIC0 &&
                var->data.location <= VERT_ATTRIB_GENERIC15);

         if (var->data.location == io_semantics.location) {
            assert(component < input_components);
            vtxin_index += reg_count * component;
            break;
         }

         vtxin_index += reg_count * input_components;
      }
   }

   assert(vtxin_index != ~0U);

   rogue_reg *src = rogue_vtxin_reg(b->shader, vtxin_index);
   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));
   rogue_add_instr_comment(&mov->instr, "load_input_vs");
}

static void trans_nir_intrinsic_load_input(rogue_builder *b,
                                           nir_intrinsic_instr *intr)
{
   switch (b->shader->stage) {
   case MESA_SHADER_FRAGMENT:
      return trans_nir_intrinsic_load_input_fs(b, intr);

   case MESA_SHADER_VERTEX:
      return trans_nir_intrinsic_load_input_vs(b, intr);

   default:
      break;
   }

   unreachable("Unsupported NIR load_input variant.");
}

static void trans_nir_intrinsic_load_output_fs(rogue_builder *b,
                                               nir_intrinsic_instr *intr)
{
   assert(b->shader->stage == MESA_SHADER_FRAGMENT);

   unsigned reg_idx = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]);

   /* Pixel output registers can't be used with repeat >
    * 1, so load_size will always be limited to 1.
    */
   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);
   rogue_ref src = rogue_ref_reg(rogue_pixout_reg(b->shader, reg_idx));

   rogue_alu_instr *mov = rogue_MOV(b, dst, src);
   rogue_add_instr_commentf(&mov->instr, "load_output_fs");
}

static void trans_nir_intrinsic_load_output(rogue_builder *b,
                                            nir_intrinsic_instr *intr)
{
   switch (b->shader->stage) {
   case MESA_SHADER_FRAGMENT:
      return trans_nir_intrinsic_load_output_fs(b, intr);

   default:
      break;
   }

   unreachable("Unsupported NIR load_output variant.");
}

static void trans_nir_intrinsic_store_output_fs(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   unsigned reg_idx = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1]);
   rogue_reg *dst = rogue_pixout_reg(b->shader, reg_idx);

   /* Pixel output registers can't be used with repeat > 1, so store_size
    * will always be limited to 1.
    */
   rogue_ref src = intr_src(b->shader, intr, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *mov = rogue_MOV(b, rogue_ref_reg(dst), src);
   rogue_add_instr_commentf(&mov->instr, "store_output_fs");
}

static void trans_nir_intrinsic_store_output_vs(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   struct rogue_vs_build_data *vs_data = &b->shader->ctx->stage_data.vs;

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
   unsigned vtxout_index = rogue_output_index_vs(&vs_data->outputs,
                                                 io_semantics.location,
                                                 component);

   rogue_reg *dst = rogue_vtxout_reg(b->shader, vtxout_index);

   rogue_ref src = intr_src(b->shader, intr, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *mov = rogue_MOV(b, rogue_ref_reg(dst), src);
   rogue_add_instr_comment(&mov->instr, "store_output_vs");
}

static void trans_nir_intrinsic_store_output(rogue_builder *b,
                                             nir_intrinsic_instr *intr)
{
   switch (b->shader->stage) {
   case MESA_SHADER_FRAGMENT:
      return trans_nir_intrinsic_store_output_fs(b, intr);

   case MESA_SHADER_VERTEX:
      return trans_nir_intrinsic_store_output_vs(b, intr);

   default:
      break;
   }

   unreachable("Unsupported NIR store_output variant.");
}

static void trans_nir_intrinsic_load_vulkan_desc_set_table_base_addr_img(
   rogue_builder *b,
   nir_intrinsic_instr *intr)
{
   rogue_ref64 dst = nir_ssa_intr_dst64(b->shader, intr);

   /* Fetch shared registers containing descriptor set table address. */
   enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
   const struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;
   assert(pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
             .descriptor_set_addrs_table.present);

   unsigned desc_set_table_base_sh_reg =
      pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
         .descriptor_set_addrs_table.offset;
   rogue_ref64 src = rogue_shared_ref64(b->shader, desc_set_table_base_sh_reg);

   rogue_alu_instr *mov = rogue_MOV(b, dst.lo32, src.lo32);
   rogue_add_instr_comment(&mov->instr,
                           "load_vulkan_desc_set_table_base_addr_img.lo32");

   mov = rogue_MOV(b, dst.hi32, src.hi32);
   rogue_add_instr_comment(&mov->instr,
                           "load_vulkan_desc_set_table_base_addr_img.hi32");
}

static void trans_nir_intrinsic_load_global(rogue_builder *b,
                                            nir_intrinsic_instr *intr,
                                            bool constant)
{
   unsigned bit_size = nir_dest_bit_size(intr->dest);
   unsigned load_components = 0;
   rogue_ref dst = intr_dst(b->shader, intr, &load_components, bit_size);
   rogue_ref src_addr = intr_src(b->shader, intr, 0, &(unsigned){ 1 }, 64);

   rogue_backend_instr *ld =
      rogue_load_global(b, &dst, &src_addr, bit_size, load_components, constant);

   rogue_add_instr_commentf(&ld->instr,
                            "load_global%s%ux%u",
                            constant ? "_constant" : "",
                            bit_size,
                            load_components);
}

static void trans_nir_intrinsic_store_global(rogue_builder *b,
                                             nir_intrinsic_instr *intr)
{
   rogue_ref dst_addr = intr_src(b->shader, intr, 1, &(unsigned){ 1 }, 64);

   unsigned bit_size = nir_src_bit_size(intr->src[0]);

   unsigned store_components = 0;
   rogue_ref src = intr_src(b->shader, intr, 0, &store_components, bit_size);

   rogue_backend_instr *st =
      rogue_store_global(b, &dst_addr, &src, bit_size, store_components);

   /* TODO: cache flags */
   rogue_add_instr_commentf(&st->instr,
                            "store_global%ux%u",
                            bit_size,
                            store_components);
}

static void trans_nir_load_helper_invocation(rogue_builder *b,
                                             nir_intrinsic_instr *intr)
{
   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   rogue_ref valid_msk =
      rogue_ref_reg(rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_backend_instr *savmsk = rogue_SAVMSK(b, valid_msk, rogue_none());
   rogue_set_backend_op_mod(savmsk, ROGUE_BACKEND_OP_MOD_VM);

   rogue_ref imm_0 = rogue_ref_imm(0);
   rogue_alu_instr *cmp = rogue_cmp(b,
                                    &dst,
                                    &valid_msk,
                                    &imm_0,
                                    COMPARE_FUNC_EQUAL,
                                    nir_type_uint32);
   rogue_add_instr_comment(&cmp->instr, "load_helper_invocation");
}

static void trans_nir_load_special_reg(rogue_builder *b,
                                       nir_intrinsic_instr *intr,
                                       enum rogue_special_reg reg,
                                       const char *comment)
{
   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   rogue_reg *src = rogue_special_reg(b->shader, reg);
   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));

   rogue_add_instr_comment(&mov->instr, comment);
}

static void trans_nir_load_push_consts_base_addr_img(rogue_builder *b,
                                                     nir_intrinsic_instr *intr)
{
   rogue_ref64 dst = nir_ssa_intr_dst64(b->shader, intr);

   /* Fetch shared registers containing push constants address. */
   enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
   const struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;
   assert(
      pipeline_layout->sh_reg_layout_per_stage[pvr_stage].push_consts.present);
   unsigned push_consts_sh_reg =
      pipeline_layout->sh_reg_layout_per_stage[pvr_stage].push_consts.offset;

   rogue_ref64 src = rogue_shared_ref64(b->shader, push_consts_sh_reg);

   rogue_alu_instr *mov = rogue_MOV(b, dst.lo32, src.lo32);
   rogue_add_instr_comment(&mov->instr, "load_push_consts_base_addr_img.lo32");
   mov = rogue_MOV(b, dst.hi32, src.hi32);
   rogue_add_instr_comment(&mov->instr, "load_push_consts_base_addr_img.hi32");
}

static void
trans_nir_intrinsic_load_local_invocation_index(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   assert(cs_data->local_id_regs[0] != ROGUE_REG_UNUSED);
   rogue_reg *src = rogue_vtxin_reg(b->shader, cs_data->local_id_regs[0]);
   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));

   rogue_add_instr_comment(&mov->instr, "load_local_invocation_index");
}

static void trans_nir_intrinsic_load_workgroup_id_img(rogue_builder *b,
                                                      nir_intrinsic_instr *intr,
                                                      unsigned component)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   assert(cs_data->workgroup_regs[component] != ROGUE_REG_UNUSED);
   rogue_reg *src =
      rogue_coeff_reg(b->shader, cs_data->workgroup_regs[component]);

   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));
   rogue_add_instr_commentf(&mov->instr,
                            "load_workgroup_id.%c",
                            'x' + component);
}

static void
trans_nir_intrinsic_load_num_workgroups_base_addr_img(rogue_builder *b,
                                                      nir_intrinsic_instr *intr)
{
   rogue_ref64 dst = nir_ssa_intr_dst64(b->shader, intr);

   /* Fetch shared registers containing num_workgroups base address. */
   enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
   const struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;
   assert(pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
             .num_workgroups.present);
   unsigned num_wgs_sh_reg =
      pipeline_layout->sh_reg_layout_per_stage[pvr_stage].num_workgroups.offset;

   rogue_ref64 src = rogue_shared_ref64(b->shader, num_wgs_sh_reg);

   rogue_alu_instr *mov = rogue_MOV(b, dst.lo32, src.lo32);
   rogue_add_instr_comment(&mov->instr,
                           "load_num_workgroups_base_addr_img.lo32");
   mov = rogue_MOV(b, dst.hi32, src.hi32);
   rogue_add_instr_comment(&mov->instr,
                           "load_num_workgroups_base_addr_img.hi32");
}

static void trans_nir_intrinsic_load_vertex_id(rogue_builder *b,
                                               nir_intrinsic_instr *intr)
{
   unsigned vtx_id_index =
      b->shader->ctx->stage_data.vs.special_vars.vertex_id_offset;

   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   assert(vtx_id_index != ROGUE_REG_UNUSED);
   rogue_reg *src = rogue_vtxin_reg(b->shader, vtx_id_index);

   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));
   rogue_add_instr_commentf(&mov->instr, "load_vertex_id");
}

static void trans_nir_intrinsic_load_instance_id(rogue_builder *b,
                                                 nir_intrinsic_instr *intr)
{
   unsigned instance_id_index =
      b->shader->ctx->stage_data.vs.special_vars.instance_id_offset;

   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);

   assert(instance_id_index != ROGUE_REG_UNUSED);
   rogue_reg *src = rogue_vtxin_reg(b->shader, instance_id_index);

   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));
   rogue_add_instr_commentf(&mov->instr, "load_instance_id(");
}

static void trans_nir_intrinsic_discard(rogue_builder *b,
                                        nir_intrinsic_instr *intr)
{
   rogue_backend_instr *atst_never =
      rogue_ATST_IF(b, rogue_ref_imm(0), rogue_ref_imm(0));
   rogue_set_backend_op_mod(atst_never, ROGUE_BACKEND_OP_MOD_NEVER);
   rogue_add_instr_comment(&atst_never->instr, "discard");
}

static void trans_nir_intrinsic_discard_if(rogue_builder *b,
                                           nir_intrinsic_instr *intr)
{
   rogue_ref src = intr_src(b->shader, intr, 0, &(unsigned){ 1 }, 32);
   rogue_backend_instr *atst_if = rogue_ATST_IF(b, src, rogue_ref_imm(0));
   /* For ATST false = discard; pass if == 0, discard if != 0. */
   rogue_set_backend_op_mod(atst_if, ROGUE_BACKEND_OP_MOD_EQUAL);
   rogue_add_instr_comment(&atst_if->instr, "discard_if");
}

static inline const char *nir_alu_type_str(nir_alu_type type)
{
   switch (type) {
   case nir_type_int:
      return "int";

   case nir_type_uint:
      return "uint";

   case nir_type_bool:
      return "bool";

   case nir_type_float:
      return "float";

   default:
      break;
   }

   unreachable("Unsupported nir_alu_type.");
}

static inline const char *nir_rounding_mode_str(nir_rounding_mode round)
{
   switch (round) {
   case nir_rounding_mode_undef:
      return "undef";

   case nir_rounding_mode_rtne:
      return "rtne";

   case nir_rounding_mode_ru:
      return "ru";

   case nir_rounding_mode_rd:
      return "rd";

   case nir_rounding_mode_rtz:
      return "rtz";

   default:
      break;
   }

   unreachable("Unsupported nir_rounding_mode.");
}

/* TODO: hash instead? */
#define CONV(SRC_TYPE,                                                   \
             SRC_BITS,                                                   \
             SRC_COMPS,                                                  \
             DST_TYPE,                                                   \
             DST_BITS,                                                   \
             DST_COMPS,                                                  \
             ROUNDING_MODE,                                              \
             SAT)                                                        \
   (src_type == nir_type_##SRC_TYPE && src_bits == SRC_BITS &&           \
    src_components == SRC_COMPS && dst_type == nir_type_##DST_TYPE &&    \
    dst_bits == DST_BITS && dst_components == DST_COMPS && sat == SAT && \
    rounding_mode == nir_rounding_mode_##ROUNDING_MODE)

static void trans_nir_intrinsic_convert_alu_types(rogue_builder *b,
                                                  nir_intrinsic_instr *intr)
{
   nir_alu_type src_sized_type = nir_intrinsic_src_type(intr);
   nir_alu_type dst_sized_type = nir_intrinsic_dest_type(intr);

   nir_alu_type src_type = nir_alu_type_get_base_type(src_sized_type);
   nir_alu_type dst_type = nir_alu_type_get_base_type(dst_sized_type);

   unsigned src_bits = nir_alu_type_get_type_size(src_sized_type);
   unsigned dst_bits = nir_alu_type_get_type_size(dst_sized_type);

   nir_rounding_mode rounding_mode = nir_intrinsic_rounding_mode(intr);
   bool sat = nir_intrinsic_saturate(intr);

   unsigned dst_components = 0;
   rogue_ref dst = intr_dst(b->shader, intr, &dst_components, dst_bits);

   unsigned src_components = 0;
   rogue_ref src = intr_src(b->shader, intr, 0, &src_components, src_bits);

   rogue_instr *instr = NULL;
   do {
      /* Bool to integer conversions. */
      if (CONV(bool, 32, 1, uint, 8, 1, undef, false) ||
          CONV(bool, 32, 1, uint, 16, 1, undef, false) ||
          CONV(bool, 32, 1, uint, 32, 1, undef, false) ||
          CONV(bool, 32, 1, int, 8, 1, undef, false) ||
          CONV(bool, 32, 1, int, 16, 1, undef, false) ||
          CONV(bool, 32, 1, int, 32, 1, undef, false)) {
         rogue_alu_instr *csel =
            rogue_CSEL(b, dst, src, rogue_ref_imm(0), rogue_ref_imm(1));
         rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_Z);
         rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_U32);
         instr = &csel->instr;
      }

      /* Bool to float conversions. */
      if (CONV(bool, 32, 1, float, 32, 1, undef, false)) {
         rogue_alu_instr *csel = rogue_CSEL(b,
                                            dst,
                                            src,
                                            rogue_ref_imm_f(0.0f),
                                            rogue_ref_imm_f(1.0f));
         rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_Z);
         rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_U32);
         instr = &csel->instr;
      }

      /* Unsigned src_bits < dst_bits => bitcast. */
      if (CONV(uint, 8, 1, uint, 16, 1, undef, false) ||
          CONV(uint, 8, 1, uint, 32, 1, undef, false) ||
          CONV(uint, 16, 1, uint, 32, 1, undef, false)) {
         rogue_alu_instr *mov = rogue_MOV(b, dst, src);
         instr = &mov->instr;
         break;
      }

      /* Signed src_bits < dst_bits => sign-extend. */
      if (CONV(int, 8, 1, int, 16, 1, undef, false) ||
          CONV(int, 8, 1, int, 32, 1, undef, false) ||
          CONV(int, 16, 1, int, 32, 1, undef, false)) {
         rogue_bitwise_instr *isxt = rogue_ISXT(b,
                                                dst,
                                                src,
                                                rogue_ref_imm(src_bits - 1),
                                                rogue_ref_imm(0));
         instr = &isxt->instr;
         break;
      }

      /* (Un)signed src_bits > dst_bits => bitcast/mask. */
      if (CONV(int, 16, 1, int, 8, 1, undef, false) ||
          CONV(int, 32, 1, int, 8, 1, undef, false) ||
          CONV(int, 32, 1, int, 16, 1, undef, false) ||
          CONV(uint, 16, 1, uint, 8, 1, undef, false) ||
          CONV(uint, 32, 1, uint, 8, 1, undef, false) ||
          CONV(uint, 32, 1, uint, 16, 1, undef, false)) {
         rogue_bitwise_instr *iand =
            rogue_IAND(b, dst, src, rogue_ref_imm(BITFIELD_MASK(dst_bits)));
         instr = &iand->instr;
         break;
      }

      /* (Un)signed -> float => element selection allows us to skip sign
       * extension.
       */
      if (CONV(uint, 8, 1, float, 32, 1, undef, false) ||
          CONV(uint, 16, 1, float, 32, 1, undef, false) ||
          CONV(uint, 32, 1, float, 32, 1, undef, false) ||
          CONV(int, 8, 1, float, 32, 1, undef, false) ||
          CONV(int, 16, 1, float, 32, 1, undef, false) ||
          CONV(int, 32, 1, float, 32, 1, undef, false)) {
         rogue_alu_instr *upck;
         switch (src_sized_type) {
         case nir_type_uint8:
            upck = rogue_UPCK_U8888(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            rogue_set_alu_src_mod(upck, 0, ROGUE_ALU_SRC_MOD_E0);
            break;

         case nir_type_uint16:
            upck = rogue_UPCK_U1616(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            rogue_set_alu_src_mod(upck, 0, ROGUE_ALU_SRC_MOD_E0);
            break;

         case nir_type_uint32:
            upck = rogue_UPCK_U32(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            break;

         case nir_type_int8:
            upck = rogue_UPCK_S8888(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            rogue_set_alu_src_mod(upck, 0, ROGUE_ALU_SRC_MOD_E0);
            break;

         case nir_type_int16:
            upck = rogue_UPCK_S1616(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            rogue_set_alu_src_mod(upck, 0, ROGUE_ALU_SRC_MOD_E0);
            break;

         case nir_type_int32:
            upck = rogue_UPCK_S32(b, dst, src);
            /* rogue_set_alu_op_mod(upck, ROGUE_ALU_OP_MOD_ROUNDZERO); */
            break;

         default:
            unreachable();
         }

         instr = &upck->instr;
         break;
      }

      /* Float -> (un)signed => movc write masking for dst_bits < 32. */
      if (CONV(float, 32, 1, uint, 8, 1, undef, false) ||
          CONV(float, 32, 1, uint, 16, 1, undef, false) ||
          CONV(float, 32, 1, uint, 32, 1, undef, false) ||
          CONV(float, 32, 1, int, 8, 1, undef, false) ||
          CONV(float, 32, 1, int, 16, 1, undef, false) ||
          CONV(float, 32, 1, int, 32, 1, undef, false)) {
         rogue_alu_instr *mbyp0 =
            rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), rogue_ref_imm(0));
         rogue_set_instr_group_next(&mbyp0->instr, true);

         rogue_alu_instr *pck;

         switch (dst_sized_type) {
         case nir_type_uint8:
            pck = rogue_PCK_U8888(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_type_uint16:
            pck = rogue_PCK_U1616(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_type_uint32:
            pck = rogue_PCK_U32(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_type_int8:
            pck = rogue_PCK_S8888(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_type_int16:
            pck = rogue_PCK_S1616(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_type_int32:
            pck = rogue_PCK_S32(b, rogue_ref_io(ROGUE_IO_FT2), src);
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         default:
            unreachable();
         }

         rogue_set_instr_group_next(&pck->instr, true);

         rogue_alu_instr *movc = rogue_MOVC(b,
                                            dst,
                                            rogue_none(),
                                            rogue_none(),
                                            rogue_ref_io(ROGUE_IO_FT2),
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_none(),
                                            rogue_none());
         if (dst_bits >= 8)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);

         if (dst_bits >= 16)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);

         if (dst_bits >= 24)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E2);

         if (dst_bits == 32)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E3);

         instr = &pck->instr;
         break;
      }

      /* Float src_bits < dst_bits. */
      if (CONV(float, 16, 1, float, 32, 1, undef, false)) {
         rogue_alu_instr *upck_f16f16 = rogue_UPCK_F16F16(b, dst, src);
         rogue_set_alu_op_mod(upck_f16f16, ROGUE_ALU_OP_MOD_ROUNDZERO);
         rogue_set_alu_src_mod(upck_f16f16, 0, ROGUE_ALU_SRC_MOD_E0);
         instr = &upck_f16f16->instr;
         break;
      }

      /* Float src_bits > dst_bits. */
      if (CONV(float, 32, 1, float, 16, 1, undef, false) ||
          CONV(float, 32, 1, float, 16, 1, rtne, false) ||
          CONV(float, 32, 1, float, 16, 1, rtz, false)) {
         rogue_alu_instr *mbyp0 =
            rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), rogue_ref_imm(0));
         rogue_set_instr_group_next(&mbyp0->instr, true);

         rogue_alu_instr *pck;

         switch (dst_sized_type) {
         case nir_type_float16:
            pck = rogue_PCK_F16F16(b, rogue_ref_io(ROGUE_IO_FT2), src);
            break;

         default:
            unreachable();
         }

         switch (rounding_mode) {
         /* Default to rtz. */
         case nir_rounding_mode_undef:
         case nir_rounding_mode_rtz:
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_ROUNDZERO);
            break;

         case nir_rounding_mode_rtne:
            /* Do nothing; default for pck is rtne. */
            break;

         default:
            unreachable();
         }

         rogue_set_instr_group_next(&pck->instr, true);

         rogue_alu_instr *movc = rogue_MOVC(b,
                                            dst,
                                            rogue_none(),
                                            rogue_none(),
                                            rogue_ref_io(ROGUE_IO_FT2),
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_none(),
                                            rogue_none());
         if (dst_bits >= 8)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);

         if (dst_bits >= 16)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);

         if (dst_bits >= 24)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E2);

         if (dst_bits == 32)
            rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E3);

         instr = &pck->instr;
         break;
      }
   } while (0);

   if (instr) {
      rogue_add_instr_commentf(instr,
                               "%s%ux%u -> %s%ux%u (rnd: %s, sat: %c)",
                               nir_alu_type_str(src_type),
                               src_bits,
                               src_components,
                               nir_alu_type_str(dst_type),
                               dst_bits,
                               dst_components,
                               nir_rounding_mode_str(rounding_mode),
                               sat ? 'y' : 'n');
      return;
   }

   fprintf(
      stdout,
      "Unsupported conversion from %s%ux%u -> %s%ux%u (rnd: %s, sat: %c)\n",
      nir_alu_type_str(src_type),
      src_bits,
      src_components,
      nir_alu_type_str(dst_type),
      dst_bits,
      dst_components,
      nir_rounding_mode_str(rounding_mode),
      sat ? 'y' : 'n');
   unreachable();
}

#undef CONV

static void trans_nir_intrinsic_global_atomic(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   rogue_ref dst = intr_dst(b->shader, intr, &(unsigned){ 1 }, 32);
   rogue_ref64 src_addr = nir_ssa_intr_src64(b->shader, intr, 0);
   rogue_ref src_data = intr_src(b->shader, intr, 1, &(unsigned){ 1 }, 32);

   unsigned index = b->shader->ctx->next_ssa_idx++;
   rogue_ref addr_data =
      rogue_ref_regarray(rogue_ssa_vec_regarray(b->shader, 3, index, 0));
   rogue_ref addr_lo =
      rogue_ref_regarray(rogue_ssa_vec_regarray(b->shader, 1, index, 0));
   rogue_ref addr_hi =
      rogue_ref_regarray(rogue_ssa_vec_regarray(b->shader, 1, index, 1));
   rogue_ref data =
      rogue_ref_regarray(rogue_ssa_vec_regarray(b->shader, 1, index, 2));

   rogue_MOV(b, addr_lo, src_addr.lo32);
   rogue_MOV(b, addr_hi, src_addr.hi32);
   rogue_MOV(b, data, src_data);

   rogue_backend_instr *atom =
      rogue_ATOMIC(b, dst, rogue_ref_drc(0), addr_data);
   switch (nir_intrinsic_atomic_op(intr)) {
   case nir_atomic_op_iadd:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_IADD);
      break;

   case nir_atomic_op_imin:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_IMIN);
      break;

   case nir_atomic_op_umin:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_UMIN);
      break;

   case nir_atomic_op_imax:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_IMAX);
      break;

   case nir_atomic_op_umax:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_UMAX);
      break;

   case nir_atomic_op_iand:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_AND);
      break;

   case nir_atomic_op_ior:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_OR);
      break;

   case nir_atomic_op_ixor:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_XOR);
      break;

   case nir_atomic_op_xchg:
      rogue_set_backend_op_mod(atom, ROGUE_BACKEND_OP_MOD_XCHG);
      break;

   default:
      unreachable("Unsupported atomic op.");
   }
}

static void trans_nir_intrinsic_mutex_img(rogue_builder *b,
                                          nir_intrinsic_instr *intr)
{
   rogue_shader *shader = b->shader;
   enum rogue_mutex_op mutex_op = nir_intrinsic_mutex_op_img(intr);
   enum rogue_mutex_id mutex_id = nir_intrinsic_mutex_id_img(intr);
   enum rogue_ctrl_op_mod mod;

   switch (mutex_op) {
   case ROGUE_MUTEX_OP_LOCK:
      mod = ROGUE_CTRL_OP_MOD_LOCK;
      break;

   case ROGUE_MUTEX_OP_RELEASE:
      mod = ROGUE_CTRL_OP_MOD_RELEASE;
      break;

   default:
      unreachable();
   }

   if (mutex_op == ROGUE_MUTEX_OP_LOCK) {
      /* Make sure we don't double lock. */
      assert(shader->mutex_state == ROGUE_MUTEX_STATE_RELEASED);
      shader->mutex_state |= ROGUE_MUTEX_STATE_LOCKED;
   } else {
      assert(shader->mutex_state & ROGUE_MUTEX_STATE_LOCKED);
      shader->mutex_state = ROGUE_MUTEX_STATE_RELEASED;
   }

   rogue_ctrl_instr *mutex = rogue_MUTEX(b, rogue_ref_val(mutex_id));
   rogue_set_ctrl_op_mod(mutex, mod);
}

static void trans_nir_intrinsic(rogue_builder *b, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_preamble:
      return trans_nir_intrinsic_load_preamble(b, intr);

   case nir_intrinsic_load_input:
      return trans_nir_intrinsic_load_input(b, intr);

   case nir_intrinsic_store_output:
      return trans_nir_intrinsic_store_output(b, intr);

   case nir_intrinsic_load_output:
      return trans_nir_intrinsic_load_output(b, intr);

   case nir_intrinsic_load_vulkan_desc_set_table_base_addr_img:
      return trans_nir_intrinsic_load_vulkan_desc_set_table_base_addr_img(b,
                                                                          intr);

   case nir_intrinsic_load_global_constant:
      return trans_nir_intrinsic_load_global(b, intr, true);

   case nir_intrinsic_load_global:
      return trans_nir_intrinsic_load_global(b, intr, false);

   case nir_intrinsic_store_global:
      return trans_nir_intrinsic_store_global(b, intr);

   case nir_intrinsic_load_helper_invocation:
      return trans_nir_load_helper_invocation(b, intr);

   case nir_intrinsic_load_sample_id:
      return trans_nir_load_special_reg(b,
                                        intr,
                                        ROGUE_SPECIAL_REG_SAMP_NUM,
                                        "load_sample_id");

   case nir_intrinsic_load_layer_id:
      return trans_nir_load_special_reg(b,
                                        intr,
                                        ROGUE_SPECIAL_REG_RENDER_TGT_ID,
                                        "load_layer_id");

   case nir_intrinsic_load_instance_num_img:
      return trans_nir_load_special_reg(b,
                                        intr,
                                        ROGUE_SPECIAL_REG_INST_NUM,
                                        "load_instance_num_img");

   case nir_intrinsic_load_push_consts_base_addr_img:
      return trans_nir_load_push_consts_base_addr_img(b, intr);

   case nir_intrinsic_load_local_invocation_index:
      return trans_nir_intrinsic_load_local_invocation_index(b, intr);

   case nir_intrinsic_load_workgroup_id_x_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 0);

   case nir_intrinsic_load_workgroup_id_y_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 1);

   case nir_intrinsic_load_workgroup_id_z_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 2);

   case nir_intrinsic_load_num_workgroups_base_addr_img:
      return trans_nir_intrinsic_load_num_workgroups_base_addr_img(b, intr);

   case nir_intrinsic_load_vertex_id:
      return trans_nir_intrinsic_load_vertex_id(b, intr);

   case nir_intrinsic_load_instance_id:
      return trans_nir_intrinsic_load_instance_id(b, intr);

   case nir_intrinsic_discard:
      return trans_nir_intrinsic_discard(b, intr);

   case nir_intrinsic_discard_if:
      return trans_nir_intrinsic_discard_if(b, intr);

   case nir_intrinsic_convert_alu_types:
      return trans_nir_intrinsic_convert_alu_types(b, intr);

   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      return trans_nir_intrinsic_global_atomic(b, intr);

   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_size:
   case nir_intrinsic_bindless_image_samples:
   case nir_intrinsic_bindless_image_texel_address:
      return trans_nir_intrinsic_image(b, intr);

   case nir_intrinsic_mutex_img:
      return trans_nir_intrinsic_mutex_img(b, intr);

   default:
      break;
   }

   unreachable("Unsupported NIR intrinsic instruction.");
}

/* TODO: commonise. */
static void trans_nir_alu_pack_unorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 4 }, 32);

   rogue_alu_instr *pck_u8888 = rogue_PCK_U8888(b, dst, src);
   rogue_set_instr_repeat(&pck_u8888->instr, 4);
   rogue_set_alu_op_mod(pck_u8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_unorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 4 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *upck_u8888 = rogue_UPCK_U8888(b, dst, src);
   rogue_set_instr_repeat(&upck_u8888->instr, 4);
   rogue_set_alu_op_mod(upck_u8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_snorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 4 }, 32);

   rogue_alu_instr *pck_s8888 = rogue_PCK_S8888(b, dst, src);
   rogue_set_instr_repeat(&pck_s8888->instr, 4);
   rogue_set_alu_op_mod(pck_s8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_snorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 4 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *upck_s8888 = rogue_UPCK_S8888(b, dst, src);
   rogue_set_instr_repeat(&upck_s8888->instr, 4);
   rogue_set_alu_op_mod(upck_s8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_unorm_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 2 }, 32);

   rogue_alu_instr *pck_u1616 = rogue_PCK_U1616(b, dst, src);
   rogue_set_instr_repeat(&pck_u1616->instr, 2);
   rogue_set_alu_op_mod(pck_u1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_unorm_2x16(rogue_builder *b,
                                            nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 2 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *upck_u1616 = rogue_UPCK_U1616(b, dst, src);
   rogue_set_instr_repeat(&upck_u1616->instr, 2);
   rogue_set_alu_op_mod(upck_u1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_snorm_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 2 }, 32);

   rogue_alu_instr *pck_s1616 = rogue_PCK_S1616(b, dst, src);
   rogue_set_instr_repeat(&pck_s1616->instr, 2);
   rogue_set_alu_op_mod(pck_s1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_snorm_2x16(rogue_builder *b,
                                            nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 2 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *upck_s1616 = rogue_UPCK_S1616(b, dst, src);
   rogue_set_instr_repeat(&upck_s1616->instr, 2);
   rogue_set_alu_op_mod(upck_s1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_half_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 2 }, 32);

   rogue_alu_instr *pck_f16f16 = rogue_PCK_F16F16(b, dst, src);
   rogue_set_instr_repeat(&pck_f16f16->instr, 2);
}

static void trans_nir_alu_pack_half_2x16_split(rogue_builder *b,
                                               nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);

   rogue_ref dst_intrmdt =
      rogue_ref_reg(rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   /* First/lower source. */
   rogue_alu_instr *mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), rogue_ref_imm(0));
   rogue_set_instr_group_next(&mbyp0->instr, true);

   rogue_alu_instr *pck_f16f16 =
      rogue_PCK_F16F16(b, rogue_ref_io(ROGUE_IO_FT2), src0);
   rogue_set_instr_group_next(&pck_f16f16->instr, true);

   rogue_alu_instr *movc = rogue_MOVC(b,
                                      dst_intrmdt,
                                      rogue_none(),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT2),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_none(),
                                      rogue_none());

   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);

   /* Second/upper source. */
   mbyp0 = rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), dst_intrmdt);
   rogue_set_instr_group_next(&mbyp0->instr, true);

   pck_f16f16 = rogue_PCK_F16F16(b, rogue_ref_io(ROGUE_IO_FT2), src1);
   rogue_set_instr_group_next(&pck_f16f16->instr, true);

   movc = rogue_MOVC(b,
                     dst,
                     rogue_none(),
                     rogue_none(),
                     rogue_ref_io(ROGUE_IO_FT2),
                     rogue_ref_io(ROGUE_IO_FT0),
                     rogue_none(),
                     rogue_none());

   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E2);
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E3);
}

static void trans_nir_alu_unpack_half_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 2 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *upck_f16f16 = rogue_UPCK_F16F16(b, dst, src);
   rogue_set_instr_repeat(&upck_f16f16->instr, 2);
}

static void rogue_apply_alu_src_mods(rogue_alu_instr *rogue_alu,
                                     nir_alu_instr *nir_alu,
                                     bool reverse)
{
   unsigned num_srcs = rogue_alu_op_infos[rogue_alu->op].num_srcs;
   assert(num_srcs == nir_op_infos[nir_alu->op].num_inputs);

   for (unsigned u = 0; u < num_srcs; ++u) {
      if (nir_alu->src[u].negate)
         rogue_set_alu_src_mod(rogue_alu,
                               reverse ? (num_srcs - 1) - u : u,
                               ROGUE_ALU_SRC_MOD_NEG);

      if (nir_alu->src[u].abs)
         rogue_set_alu_src_mod(rogue_alu,
                               reverse ? (num_srcs - 1) - u : u,
                               ROGUE_ALU_SRC_MOD_ABS);
   }
}

static void trans_nir_alu_fadd(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_alu_instr *fadd;
   if (alu->src[1].negate && !alu->src[0].negate) {
      fadd = rogue_FADD(b, dst, src1, src0);
      rogue_apply_alu_src_mods(fadd, alu, true);
   } else {
      fadd = rogue_FADD(b, dst, src0, src1);
      rogue_apply_alu_src_mods(fadd, alu, false);
   }
}

static void trans_nir_alu_fmul(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_alu_instr *fmul;
   if (alu->src[1].negate && !alu->src[0].negate) {
      fmul = rogue_FMUL(b, dst, src1, src0);
      rogue_apply_alu_src_mods(fmul, alu, true);
   } else {
      fmul = rogue_FMUL(b, dst, src0, src1);
      rogue_apply_alu_src_mods(fmul, alu, false);
   }
}

static void trans_nir_alu_ffma(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);
   rogue_ref src2 = alu_src(b->shader, alu, 2, &(unsigned){ 1 }, 32);

   rogue_alu_instr *ffma = rogue_FMAD(b, dst, src0, src1, src2);
   rogue_apply_alu_src_mods(ffma, alu, false);
}

static void trans_nir_alu_frcp(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *frcp = rogue_FRCP(b, dst, src);
   rogue_apply_alu_src_mods(frcp, alu, false);
}

static void trans_nir_alu_frsq(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *frsq = rogue_FRSQ(b, dst, src);
   rogue_apply_alu_src_mods(frsq, alu, false);
}

static void trans_nir_alu_flog2(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *flog2 = rogue_FLOG2(b, dst, src);
   rogue_apply_alu_src_mods(flog2, alu, false);
}

static void trans_nir_alu_fexp2(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *fexp2 = rogue_FEXP2(b, dst, src);
   rogue_apply_alu_src_mods(fexp2, alu, false);
}

static void trans_nir_alu_fddx(rogue_builder *b, nir_alu_instr *alu, bool fine)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *fdsx = fine ? rogue_FDSXF(b, dst, src)
                                : rogue_FDSX(b, dst, src);

   rogue_apply_alu_src_mods(fdsx, alu, false);
}

static void trans_nir_alu_fddy(rogue_builder *b, nir_alu_instr *alu, bool fine)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_alu_instr *fdsy = fine ? rogue_FDSYF(b, dst, src)
                                : rogue_FDSY(b, dst, src);

   rogue_apply_alu_src_mods(fdsy, alu, false);
}

#define OM(op_mod) ROGUE_ALU_OP_MOD_##op_mod
static void trans_nir_alu_minmax(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_alu_instr *minmax;

   /* Set comparison op. */
   switch (alu->op) {
   case nir_op_fmin:
   case nir_op_imin:
   case nir_op_umin:
      minmax = rogue_MIN(b, dst, src0, src1);
      break;

   case nir_op_fmax:
   case nir_op_imax:
   case nir_op_umax:
      minmax = rogue_MAX(b, dst, src0, src1);
      break;

   default:
      unreachable();
   }

   /* Set type. */
   switch (alu->op) {
   case nir_op_fmin:
   case nir_op_fmax:
      switch (bit_size) {
      case 32:
         rogue_set_alu_op_mod(minmax, OM(F32));
         break;

      default:
         unreachable();
      }
      break;

   case nir_op_imin:
   case nir_op_imax:
      switch (bit_size) {
      case 8:
         rogue_set_alu_op_mod(minmax, OM(S8));
         break;

      case 16:
         rogue_set_alu_op_mod(minmax, OM(S16));
         break;

      case 32:
         rogue_set_alu_op_mod(minmax, OM(S32));
         break;

      default:
         unreachable();
      }
      break;

   case nir_op_umin:
   case nir_op_umax:
      switch (bit_size) {
      case 8:
         rogue_set_alu_op_mod(minmax, OM(U8));
         break;

      case 16:
         rogue_set_alu_op_mod(minmax, OM(U16));
         break;

      case 32:
         rogue_set_alu_op_mod(minmax, OM(U32));
         break;

      default:
         unreachable();
      }
      break;

   default:
      unreachable();
   }

   rogue_apply_alu_src_mods(minmax, alu, false);
}
#undef OM

/* Conditionally sets the output to src1 or src2 depending on whether the
 * comparison between src0 and 0 is true or false.
 */
static void trans_nir_alu_csel(rogue_builder *b, nir_alu_instr *alu)
{
   /* Reverse exists because we only have == 0, > 0 and >= 0 but not != 0,
    * so this lets us use Z and invert the srcs.
    */
   bool reverse = (alu->op == nir_op_fcsel) || (alu->op == nir_op_b32csel);
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   nir_alu_type type = nir_cmp_type(alu->op) | bit_size;
   enum compare_func func = nir_cmp_func(alu->op);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src_cmp = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src_true =
      alu_src(b->shader, alu, reverse ? 2 : 1, &(unsigned){ 1 }, bit_size);
   rogue_ref src_false =
      alu_src(b->shader, alu, reverse ? 1 : 2, &(unsigned){ 1 }, bit_size);

   rogue_alu_instr *csel =
      rogue_csel(b, &dst, &src_cmp, &src_true, &src_false, func, type);

   rogue_apply_alu_src_mods(csel, alu, reverse);
}

static void trans_nir_alu_fneg(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_FNEG(b, dst, src);
}

static void trans_nir_alu_fabs(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_FABS(b, dst, src);
}

static void trans_nir_alu_ffloor(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_FFLR(b, dst, src);
}

static void
trans_nir_alu_fsin_cos(rogue_builder *b, nir_alu_instr *alu, bool cos)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);

   enum rogue_alu_op_mod mod = cos ? ROGUE_ALU_OP_MOD_COS
                                   : ROGUE_ALU_OP_MOD_SIN;

   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   unsigned rred_a_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref rred_a = rogue_ref_reg(rogue_ssa_reg(b->shader, rred_a_idx));

   /* TODO: How many rounds of range reduction needed for required ULP? */

   /* Range reduction part a. */
   rogue_alu_instr *rogue_alu = rogue_FRED(b,
                                           rogue_none(),
                                           rred_a,
                                           rogue_none(),
                                           rogue_ref_val(0),
                                           src,
                                           rogue_none());
   rogue_set_alu_op_mod(rogue_alu, ROGUE_ALU_OP_MOD_PARTA);
   rogue_set_alu_op_mod(rogue_alu, mod);

   unsigned rred_b_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref rred_b = rogue_ref_reg(rogue_ssa_reg(b->shader, rred_b_idx));

   /* Range reduction part b. */
   rogue_alu = rogue_FRED(b,
                          rred_b,
                          rogue_none(),
                          rogue_none(),
                          rogue_ref_val(0),
                          src,
                          rred_a);
   rogue_set_alu_op_mod(rogue_alu, ROGUE_ALU_OP_MOD_PARTB);
   rogue_set_alu_op_mod(rogue_alu, mod);

   unsigned sinc_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref sinc = rogue_ref_reg(rogue_ssa_reg(b->shader, sinc_idx));

   rogue_alu = rogue_FSINC(b, sinc, rogue_ref_io(ROGUE_IO_P0), rred_b);

   unsigned perform_mul_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref perform_mul =
      rogue_ref_reg(rogue_ssa_reg(b->shader, perform_mul_idx));

   rogue_alu = rogue_GETPRED(b, perform_mul, rogue_ref_io(ROGUE_IO_P0));

   unsigned fmul_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref fmul = rogue_ref_reg(rogue_ssa_reg(b->shader, fmul_idx));

   rogue_alu = rogue_FMUL(b, fmul, rred_b, sinc);

   rogue_alu = rogue_CSEL(b, dst, perform_mul, fmul, sinc);
   rogue_set_alu_op_mod(rogue_alu, ROGUE_ALU_OP_MOD_GZ);
   rogue_set_alu_op_mod(rogue_alu, ROGUE_ALU_OP_MOD_U32);
}

static void trans_nir_alu_mov(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);

   rogue_MOV(b, dst, src);
}

static void trans_nir_alu_vecN(rogue_builder *b, nir_alu_instr *alu, unsigned n)
{
   assert(alu->dest.dest.is_ssa);
   unsigned dst_index = alu->dest.dest.ssa.index;

   rogue_ssa_vec_regarray(b->shader, n, dst_index, 0);

   rogue_regarray *dst;
   for (unsigned u = 0; u < n; ++u) {
      dst = rogue_ssa_vec_regarray(b->shader, 1, dst_index, u);
      rogue_ref src = alu_src(b->shader, alu, u, &(unsigned){ 1 }, 32);
      rogue_MOV(b, rogue_ref_regarray(dst), src);
   }
}

static void trans_nir_alu_iadd(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_alu_instr *iadd;

   switch (bit_size) {
   case 8:
      iadd = rogue_IADD8(b, dst, src0, src1);
      break;

   case 16:
      iadd = rogue_IADD16(b, dst, src0, src1);
      break;

   case 32:
      iadd = rogue_IADD32(b, dst, src0, src1);
      break;

   case 64:
      iadd = rogue_IADD64(b, dst, src0, src1);
      break;

   default:
      unreachable("Unsupported iadd bit size.");
   }

   /* Looks like add is never even really signed? */
#if 0
   if (bit_size < 64)
      rogue_set_alu_op_mod(iadd, ROGUE_ALU_OP_MOD_S);
#endif

   rogue_apply_alu_src_mods(iadd, alu, false);
}

static void trans_nir_alu_imul(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_alu_instr *imul;

   switch (bit_size) {
   case 8:
      imul = rogue_IMUL8(b, dst, src0, src1);
      break;

   case 16:
      imul = rogue_IMUL16(b, dst, src0, src1);
      break;

   case 32:
      imul = rogue_IMUL32(b, dst, src0, src1);
      break;

   default:
      unreachable("Unsupported imul bit size.");
   }

   rogue_set_alu_op_mod(imul, ROGUE_ALU_OP_MOD_S);
   rogue_apply_alu_src_mods(imul, alu, false);
}

static void
trans_nir_alu_mul_high(rogue_builder *b, nir_alu_instr *alu, bool is_signed)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_alu_instr *mul_high = is_signed ? rogue_IMUL_HIGH(b, dst, src0, src1)
                                         : rogue_UMUL_HIGH(b, dst, src0, src1);
   rogue_apply_alu_src_mods(mul_high, alu, false);
}

static void
trans_nir_alu_mul_low(rogue_builder *b, nir_alu_instr *alu, bool is_signed)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_alu_instr *mul_low = is_signed ? rogue_IMUL_LOW(b, dst, src0, src1)
                                        : rogue_UMUL_LOW(b, dst, src0, src1);
   rogue_apply_alu_src_mods(mul_low, alu, false);
}

static void trans_nir_alu_ineg(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);

   switch (bit_size) {
   case 8:
      rogue_INEG8(b, dst, src);
      return;

   case 16:
      rogue_INEG16(b, dst, src);
      return;

   case 32:
      rogue_INEG32(b, dst, src);
      return;

   case 64:
      rogue_INEG64(b, dst, src);
      return;

   default:
      break;
   }

   unreachable("Unsupported ineg bit size.");
}

static void trans_nir_alu_iabs(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);

   switch (bit_size) {
   case 8:
      rogue_IABS8(b, dst, src);
      return;

   case 16:
      rogue_IABS16(b, dst, src);
      return;

   case 32:
      rogue_IABS32(b, dst, src);
      return;

   case 64:
      rogue_IABS64(b, dst, src);
      return;

   default:
      break;
   }

   unreachable("Unsupported iabs bit size.");
}

static void trans_nir_alu_cmp(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);

   unsigned bit_size = nir_src_bit_size(alu->src[0].src);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   nir_alu_type type = nir_cmp_type(alu->op) | bit_size;
   enum compare_func func = nir_cmp_func(alu->op);

   rogue_alu_instr *cmp = rogue_cmp(b, &dst, &src0, &src1, func, type);
   rogue_apply_alu_src_mods(cmp, alu, false);
   /* rogue_add_instr_comment(&cmp->instr, nirop); */
}

/* TODO: commonise handling certain alu functions with n arguments? */
/* TODO: Masking out here is super inefficient. This is mainly for functions
 * that use ~0 in rogue_lower_pseudo_ops, find a better way! */
static void trans_nir_alu_iand(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_ref _dst = dst;
   if (bit_size < 32)
      _dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_IAND(b, _dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, _dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "i_mask_%u", bit_size);
   }
}

static void trans_nir_alu_ior(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_ref _dst = dst;
   if (bit_size < 32)
      _dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_IOR(b, _dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, _dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "i_mask_%u", bit_size);
   }
}

static void trans_nir_alu_ixor(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);

   rogue_ref _dst = dst;
   if (bit_size < 32)
      _dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_IXOR(b, _dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, _dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "i_mask_%u", bit_size);
   }
}

static void trans_nir_alu_inot(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);

   rogue_ref _dst = dst;
   if (bit_size < 32)
      _dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_INOT(b, _dst, src);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, _dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "i_mask_%u", bit_size);
   }
}

static void trans_nir_ishr(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref shr_dst = dst;
   if (bit_size < 32)
      shr_dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   if (bit_size < 32)
      rogue_ISXT(b, shr_dst, src0, rogue_ref_imm(bit_size - 1), src1);
   else
      rogue_ISHR(b, dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, shr_dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "ishr_mask_%u", bit_size);
   }
}

static void trans_nir_ishl(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref shl_dst = dst;
   if (bit_size < 32)
      shl_dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_ISHL(b, shl_dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, shl_dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "ishl_mask_%u", bit_size);
   }
}

static void trans_nir_ushr(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref shr_dst = dst;
   if (bit_size < 32)
      shr_dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_USHR(b, shr_dst, src0, src1);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, dst, shr_dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "ushr_mask_%u", bit_size);
   }
}

static void trans_nir_bitfield_insert(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref base = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref insert = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);
   rogue_ref offset = alu_src(b->shader, alu, 2, &(unsigned){ 1 }, bit_size);
   rogue_ref bits = alu_src(b->shader, alu, 3, &(unsigned){ 1 }, bit_size);

   /* TODO: bitfield_insert pseudo-op */
   rogue_bitwise_instr *msk = rogue_MSK(b,
                                        rogue_ref_io(ROGUE_IO_FT0),
                                        rogue_ref_io(ROGUE_IO_FT1),
                                        bits,
                                        offset);
   rogue_set_instr_group_next(&msk->instr, true);

   rogue_bitwise_instr *lsl0 = rogue_LSL0(b,
                                          rogue_ref_io(ROGUE_IO_FT2),
                                          insert,
                                          rogue_ref_io(ROGUE_IO_S1));
   rogue_set_instr_group_next(&lsl0->instr, true);

   rogue_bitwise_instr * or = rogue_OR(b,
                                       dst,
                                       rogue_ref_io(ROGUE_IO_FT1),
                                       rogue_ref_io(ROGUE_IO_FT2),
                                       rogue_ref_io(ROGUE_IO_FT1),
                                       base);

   rogue_add_instr_comment(& or->instr, "bitfield_insert");
}

static void
trans_nir_bitfield_extract(rogue_builder *b, nir_alu_instr *alu, bool is_signed)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, bit_size);

   rogue_ref base = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, bit_size);
   rogue_ref offset = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, bit_size);
   rogue_ref bits = alu_src(b->shader, alu, 2, &(unsigned){ 1 }, bit_size);

   /* TODO: bitfield_extract pseudo-op */
   rogue_bitwise_instr *msk = rogue_MSK(b,
                                        rogue_ref_io(ROGUE_IO_FT0),
                                        rogue_ref_io(ROGUE_IO_FT1),
                                        bits,
                                        offset);
   rogue_set_instr_group_next(&msk->instr, true);

   rogue_bitwise_instr *byp0s =
      rogue_BYP0S(b, rogue_ref_io(ROGUE_IO_FT2), base);
   rogue_set_instr_group_next(&byp0s->instr, true);

   rogue_bitwise_instr * or = rogue_OR(b,
                                       rogue_ref_io(ROGUE_IO_FT4),
                                       rogue_ref_io(ROGUE_IO_FT1),
                                       rogue_ref_io(ROGUE_IO_FT2),
                                       rogue_ref_io(ROGUE_IO_FT1),
                                       rogue_ref_imm(0));
   rogue_set_instr_group_next(& or->instr, true);

   rogue_bitwise_instr *shr;
   if (is_signed) {
      /* Arithmetic right shift using mask top bit (FT0 = bits + offset). */
      shr = rogue_ASR(b, dst, rogue_ref_io(ROGUE_IO_FT4), offset);
      rogue_set_bitwise_op_mod(shr, ROGUE_BITWISE_OP_MOD_MTB);
   } else {
      shr = rogue_SHR(b, dst, rogue_ref_io(ROGUE_IO_FT4), offset);
   }

   rogue_add_instr_commentf(&shr->instr,
                            "%cbitfield_extract",
                            is_signed ? 'i' : 'u');
}

static void trans_nir_bitfield_reverse(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_IREV(b, dst, src);
}

static void trans_nir_bit_count(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_ICBS(b, dst, src);
}

static void trans_nir_ufind_msb(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   rogue_IFTB(b, dst, src);
}

static void
trans_nir_unpack_64_2x32_split(rogue_builder *b, nir_alu_instr *alu, bool hi32)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 32);
   rogue_ref64 src = nir_ssa_alu_src64(b->shader, alu, 0);
   rogue_MOV(b, dst, hi32 ? src.hi32 : src.lo32);
}

static void trans_nir_pack_64_2x32_split(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref src0 = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);
   rogue_ref src1 = alu_src(b->shader, alu, 1, &(unsigned){ 1 }, 32);

   rogue_MOV(b, dst.lo32, src0);
   rogue_MOV(b, dst.hi32, src1);
}

static void
trans_nir_unpack_32_2x16_split(rogue_builder *b, nir_alu_instr *alu, bool hi32)
{
   rogue_ref dst = alu_dst(b->shader, alu, &(unsigned){ 1 }, 16);
   rogue_ref src = alu_src(b->shader, alu, 0, &(unsigned){ 1 }, 32);

   if (hi32)
      rogue_USHR(b, dst, src, rogue_ref_imm(16));
   else
      rogue_IAND(b, dst, src, rogue_ref_imm(0x0000ffff));
}

#define OM(op_mod) ROGUE_ALU_OP_MOD_##op_mod
static void trans_nir_alu(rogue_builder *b, nir_alu_instr *alu)
{
   switch (alu->op) {
   case nir_op_pack_unorm_4x8:
      return trans_nir_alu_pack_unorm_4x8(b, alu);

   case nir_op_unpack_unorm_4x8:
      return trans_nir_alu_unpack_unorm_4x8(b, alu);

   case nir_op_pack_snorm_4x8:
      return trans_nir_alu_pack_snorm_4x8(b, alu);

   case nir_op_unpack_snorm_4x8:
      return trans_nir_alu_unpack_snorm_4x8(b, alu);

   case nir_op_pack_unorm_2x16:
      return trans_nir_alu_pack_unorm_2x16(b, alu);

   case nir_op_unpack_unorm_2x16:
      return trans_nir_alu_unpack_unorm_2x16(b, alu);

   case nir_op_pack_snorm_2x16:
      return trans_nir_alu_pack_snorm_2x16(b, alu);

   case nir_op_unpack_snorm_2x16:
      return trans_nir_alu_unpack_snorm_2x16(b, alu);

   case nir_op_pack_half_2x16:
      return trans_nir_alu_pack_half_2x16(b, alu);

   case nir_op_pack_half_2x16_split:
      return trans_nir_alu_pack_half_2x16_split(b, alu);

   case nir_op_unpack_half_2x16:
      return trans_nir_alu_unpack_half_2x16(b, alu);

   case nir_op_fadd:
      return trans_nir_alu_fadd(b, alu);

   case nir_op_fmul:
      return trans_nir_alu_fmul(b, alu);

   case nir_op_ffma:
      return trans_nir_alu_ffma(b, alu);

   case nir_op_frcp:
      return trans_nir_alu_frcp(b, alu);

   case nir_op_frsq:
      return trans_nir_alu_frsq(b, alu);

   case nir_op_flog2:
      return trans_nir_alu_flog2(b, alu);

   case nir_op_fexp2:
      return trans_nir_alu_fexp2(b, alu);

   case nir_op_fddx:
   case nir_op_fddx_coarse:
      return trans_nir_alu_fddx(b, alu, false);

   case nir_op_fddx_fine:
      return trans_nir_alu_fddx(b, alu, true);

   case nir_op_fddy:
   case nir_op_fddy_coarse:
      return trans_nir_alu_fddy(b, alu, false);

   case nir_op_fddy_fine:
      return trans_nir_alu_fddy(b, alu, true);

   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_imin:
   case nir_op_imax:
   case nir_op_umin:
   case nir_op_umax:
      return trans_nir_alu_minmax(b, alu);

   case nir_op_fneg:
      return trans_nir_alu_fneg(b, alu);

   case nir_op_ffloor:
      return trans_nir_alu_ffloor(b, alu);

   case nir_op_fabs:
      return trans_nir_alu_fabs(b, alu);

   case nir_op_fsin:
      return trans_nir_alu_fsin_cos(b, alu, false);

   case nir_op_fcos:
      return trans_nir_alu_fsin_cos(b, alu, true);

   case nir_op_mov:
      return trans_nir_alu_mov(b, alu);

   case nir_op_vec2:
      return trans_nir_alu_vecN(b, alu, 2);

   case nir_op_vec3:
      return trans_nir_alu_vecN(b, alu, 3);

   case nir_op_vec4:
      return trans_nir_alu_vecN(b, alu, 4);

   case nir_op_iadd:
      return trans_nir_alu_iadd(b, alu);

   case nir_op_imul:
      return trans_nir_alu_imul(b, alu);

   case nir_op_umul_high:
      return trans_nir_alu_mul_high(b, alu, false);

   case nir_op_umul_low:
      return trans_nir_alu_mul_low(b, alu, false);

   case nir_op_imul_high:
      return trans_nir_alu_mul_high(b, alu, true);

   case nir_op_ineg:
      return trans_nir_alu_ineg(b, alu);

   case nir_op_iabs:
      return trans_nir_alu_iabs(b, alu);

   case nir_op_flt32:
   case nir_op_fge32:
   case nir_op_feq32:
   case nir_op_fneu32:
   case nir_op_ilt32:
   case nir_op_ige32:
   case nir_op_ieq32:
   case nir_op_ine32:
   case nir_op_ult32:
   case nir_op_uge32:
      return trans_nir_alu_cmp(b, alu);

   case nir_op_iand:
      return trans_nir_alu_iand(b, alu);

   case nir_op_ior:
      return trans_nir_alu_ior(b, alu);

   case nir_op_ixor:
      return trans_nir_alu_ixor(b, alu);

   case nir_op_inot:
      return trans_nir_alu_inot(b, alu);

   case nir_op_fcsel:
   case nir_op_fcsel_gt:
   case nir_op_fcsel_ge:
   case nir_op_b32csel:
   case nir_op_i32csel_gt:
   case nir_op_i32csel_ge:
      return trans_nir_alu_csel(b, alu);

   case nir_op_ishr:
      return trans_nir_ishr(b, alu);

   case nir_op_ishl:
      return trans_nir_ishl(b, alu);

   case nir_op_ushr:
      return trans_nir_ushr(b, alu);

   case nir_op_bitfield_insert:
      return trans_nir_bitfield_insert(b, alu);

   case nir_op_ubitfield_extract:
      return trans_nir_bitfield_extract(b, alu, false);

   case nir_op_ibitfield_extract:
      return trans_nir_bitfield_extract(b, alu, true);

   case nir_op_bitfield_reverse:
      return trans_nir_bitfield_reverse(b, alu);

   case nir_op_bit_count:
      return trans_nir_bit_count(b, alu);

   case nir_op_ufind_msb:
      return trans_nir_ufind_msb(b, alu);

   case nir_op_unpack_64_2x32_split_x:
      return trans_nir_unpack_64_2x32_split(b, alu, false);

   case nir_op_unpack_64_2x32_split_y:
      return trans_nir_unpack_64_2x32_split(b, alu, true);

   case nir_op_pack_64_2x32_split:
      return trans_nir_pack_64_2x32_split(b, alu);

   case nir_op_unpack_32_2x16_split_x:
      return trans_nir_unpack_32_2x16_split(b, alu, false);

   case nir_op_unpack_32_2x16_split_y:
      return trans_nir_unpack_32_2x16_split(b, alu, true);

   default:
      break;
   }

   unreachable("Unsupported NIR ALU instruction.");
}
#undef OM

PUBLIC
unsigned rogue_count_used_regs(const rogue_shader *shader,
                               enum rogue_reg_class class)
{
   unsigned reg_count;
   if (rogue_reg_class_infos[class].num) {
      reg_count =
         __bitset_count(shader->regs_used[class],
                        BITSET_WORDS(rogue_reg_class_infos[class].num));
   } else {
      reg_count = list_length(&shader->regs[class]);
   }

#ifndef NDEBUG
   /* Check that registers are contiguous. */
   rogue_foreach_reg (reg, shader, class) {
      assert(reg->index < reg_count);
   }
#endif /* NDEBUG */

   return reg_count;
}

static inline void rogue_feedback_used_regs(rogue_build_ctx *ctx,
                                            const rogue_shader *shader)
{
   /* TODO NEXT: Use this counting method elsewhere as well. */
   ctx->common_data[shader->stage].temps =
      rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);
   ctx->common_data[shader->stage].internals =
      rogue_count_used_regs(shader, ROGUE_REG_CLASS_INTERNAL);
}

static bool ssa_def_cb(nir_ssa_def *ssa, void *state)
{
   rogue_shader *shader = (rogue_shader *)state;

   if (ssa->num_components == 1) {
      if (ssa->bit_size == 32) {
         rogue_ssa_reg(shader, ssa->index);
      } else if (ssa->bit_size == 64) {
         rogue_ssa_vec_regarray(shader, 2, ssa->index, 0);
      }
   } else {
      rogue_ssa_vec_regarray(shader, ssa->num_components, ssa->index, 0);
   }

   /* Keep track of the last SSA index so we can use more. */
   shader->ctx->next_ssa_idx = MAX2(shader->ctx->next_ssa_idx, ssa->index);

   return true;
}

static rogue_block *trans_nir_block(rogue_builder *b, nir_block *block)
{
   rogue_block *_rogue_block = rogue_push_nir_block(b, block->index);

   nir_foreach_instr (instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         trans_nir_alu(b, nir_instr_as_alu(instr));
         break;

      case nir_instr_type_intrinsic:
         trans_nir_intrinsic(b, nir_instr_as_intrinsic(instr));
         break;

      case nir_instr_type_load_const:
         trans_nir_load_const(b, nir_instr_as_load_const(instr));
         break;

      case nir_instr_type_jump:
         trans_nir_jump(b, nir_instr_as_jump(instr));
         break;

      case nir_instr_type_tex:
         trans_nir_tex(b, nir_instr_as_tex(instr));
         break;

      default:
         unreachable("Unsupported NIR instruction type.");
      }
   }

   return _rogue_block;
}

static rogue_block *trans_nir_cf_nodes(rogue_builder *b,
                                       struct exec_list *cf_node_list);

static void rogue_init_emc(rogue_builder *b)
{
   rogue_shader *shader = b->shader;
   rogue_ref emc = rogue_ref_emc(shader);

   /* Init the emc counter for the first time. */
   if (!shader->emc_initialised) {
      shader->emc_initialised = true;

      rogue_ctrl_instr *cnd = rogue_CNDST(b,
                                          rogue_ref_io(ROGUE_IO_PE),
                                          emc,
                                          rogue_ref_imm(0),
                                          rogue_ref_val(1));
      rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
      rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
      rogue_add_instr_comment(&cnd->instr, "cf_init");
      rogue_push_block(b);
   }
}

static void trans_nir_if(rogue_builder *b, nir_if *nif)
{
   rogue_shader *shader = b->shader;
   rogue_ctrl_instr *cnd;

   rogue_init_emc(b);

   /* Conditional mask count register. */
   rogue_ref emc = rogue_ref_emc(shader);

   ++shader->loop_nestings;

   /* Condition register. */
   bool is_ssa = nif->condition.is_ssa;
   rogue_reg *if_cnd =
      is_ssa ? rogue_ssa_reg(shader, nif->condition.ssa->index)
             : rogue_temp_reg(shader, nif->condition.reg.reg->index);

   const bool has_then = !nir_cf_list_is_empty_block(&nif->then_list);
   const bool has_else = !nir_cf_list_is_empty_block(&nif->else_list);
   assert(has_then || has_else);

   /* Set P0 if the condition is true (not equal to 0). */
   rogue_SETPRED(b, rogue_ref_io(ROGUE_IO_P0), rogue_ref_reg(if_cnd));

   /* Check P0 and increment mask if false. */
   cnd = rogue_CNDST(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));

   /* If the if block is empty, flip the condition and just emit the else block.
    */
   rogue_set_ctrl_op_mod(cnd,
                         has_then ? ROGUE_CTRL_OP_MOD_P0_TRUE
                                  : ROGUE_CTRL_OP_MOD_P0_FALSE);

   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

   /* If block. */
   rogue_block *if_then = NULL;
   if (has_then)
      if_then = trans_nir_cf_nodes(b, &nif->then_list);

   rogue_block *else_check = NULL;
   /* Else: if masked out due to failing if condition, enable, otherwise if we
    * did the if, mask out the else block, otherwise just leave the mask
    * unchanged. */
   if (has_then && has_else) {
      else_check = rogue_push_block(b);
      cnd =
         rogue_CNDEF(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
      rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
      rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
   }

   /* Else block. */
   rogue_block *if_else = NULL;
   if (has_else)
      if_else = trans_nir_cf_nodes(b, &nif->else_list);

   rogue_block *end_if = rogue_push_block(b);

   /* Restore the mask to what it was before this if code. */
   cnd = rogue_CNDEND(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

   /* Whether to skip the contents of the nir_if
    * if all instances are predicated out.
    */
   /* TODO: This condition is fairly arbitrary and has only really
    * been chosen because we set this flag in rogue_nir_compute_instance_check;
    * ideally we'd like to set it based on whether the then/else_lists have
    * a certain threshold of instructions present.
    *
    * NEXT: modify trans_nir_cf_nodes to pass back how many instructions
    * have been translated, and set this based on that?
    */
   bool br_skip = (nif->control == nir_selection_control_dont_flatten);
   if (br_skip) {
      /* Backup cursor position. */
      rogue_cursor cursor = b->cursor;

      rogue_ctrl_instr *br_skip;
      if (has_then) {
         b->cursor = rogue_cursor_before_block(if_then);
         rogue_push_block(b);
         br_skip = rogue_BR(b, has_else ? else_check : end_if);
         rogue_set_ctrl_op_mod(br_skip, ROGUE_CTRL_OP_MOD_ALLINST);
      }

      if (has_else) {
         b->cursor = rogue_cursor_before_block(if_else);
         rogue_push_block(b);
         br_skip = rogue_BR(b, end_if);
         rogue_set_ctrl_op_mod(br_skip, ROGUE_CTRL_OP_MOD_ALLINST);
      }

      /* Restore cursor position. */
      b->cursor = cursor;
   }

   --shader->loop_nestings;
}

static void trans_nir_loop(rogue_builder *b, nir_loop *nloop)
{
   rogue_shader *shader = b->shader;
   rogue_ctrl_instr *cnd;

   assert(!nir_loop_has_continue_construct(nloop));

   rogue_init_emc(b);

   /* Back-up previous loop nestings; only apply to inner loop. */
   unsigned pushed_nestings = shader->loop_nestings;
   shader->loop_nestings = 0;

   /* Conditional mask count register. */
   rogue_ref emc = rogue_ref_emc(shader);

   /* Increment any non-running instances. */
   cnd = rogue_CNDST(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(2));
   rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
   rogue_add_instr_comment(&cnd->instr, "loop_init");

   rogue_ctrl_instr *loop_start_instr = cnd;

   /* Start of loop block. */
   rogue_block *loop_body = rogue_push_block_labelled(b, "loop_body");

   /* Loop body. */
   trans_nir_cf_nodes(b, &nloop->body);

   /* End of loop/loop check, at this point emc is either:
    * - 0: Loop finished and should be run again.
    * - 1: continue; was hit, loop should be run again.
    * - 2: break; was hit, loop should not be run again.
    * - n + 2 (n > 0): Instance was masked out prior to the loop.
    *
    * We do a cndend 1 followed by a cndst.always 1 so that every emc value > 1
    * remains unchanged but 0/1 will run again.
    */
   cnd = rogue_CNDEND(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

   rogue_push_block(b);

   cnd = rogue_CNDST(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
   rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

   rogue_block *loop_end = rogue_push_block(b);

   /* Unconditional loop test, since NIR loops are infinite loops. If any
    * instances (including this one) are still running, P0 will be set to 1 and
    * the following conditional branch will succeed. Otherwise, this will
    * restore the mask counter to pre-loop and not take the branch. */
   cnd = rogue_CNDLT(b,
                     rogue_ref_io(ROGUE_IO_PE),
                     emc,
                     rogue_ref_io(ROGUE_IO_P0),
                     emc,
                     rogue_ref_val(2));
   rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
   rogue_add_instr_comment(&cnd->instr, "loop_test");

   rogue_push_block(b);

   rogue_ctrl_instr *br = rogue_BR(b, loop_body);
   rogue_set_instr_exec_cond(&br->instr, ROGUE_EXEC_COND_P0_TRUE);

   rogue_ctrl_instr *loop_end_instr = br;

   loop_start_instr->loop_start = true;
   loop_start_instr->loop_link = &loop_end_instr->instr;
   loop_end_instr->loop_link = &loop_start_instr->instr;

   /* TODO: Don't do this for short loops. */
   bool br_skip = true;
   if (br_skip) {
      /* Backup cursor position. */
      rogue_cursor cursor = b->cursor;

      b->cursor = rogue_cursor_before_block(loop_body);
      rogue_push_block(b);

      rogue_ctrl_instr *br_skip = rogue_BR(b, loop_end);
      rogue_set_ctrl_op_mod(br_skip, ROGUE_CTRL_OP_MOD_ALLINST);

      /* Restore cursor position. */
      b->cursor = cursor;
   }

   /* Pop loop nestings. */
   assert(shader->loop_nestings == 0);
   shader->loop_nestings = pushed_nestings;

   ++shader->loops;
}

static rogue_block *trans_nir_cf_nodes(rogue_builder *b,
                                       struct exec_list *cf_node_list)
{
   rogue_block *start_block = NULL;

   foreach_list_typed (nir_cf_node, node, node, cf_node_list) {
      switch (node->type) {
      case nir_cf_node_block: {
         rogue_block *block = trans_nir_block(b, nir_cf_node_as_block(node));

         if (!start_block)
            start_block = block;

         break;
      }

      case nir_cf_node_if:
         trans_nir_if(b, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         trans_nir_loop(b, nir_cf_node_as_loop(node));
         break;

      default:
         unreachable("Unsupported control flow node type.");
      }
   }

   return start_block;
}

/* TODO: handle other instructions/build data. */
static bool fs_data_cb(UNUSED const rogue_instr *instr,
                       const void *instr_as,
                       unsigned op,
                       void *user_data)
{
   struct rogue_fs_build_data *data = user_data;
   bool discard = false;
   bool side_effects = false;

   if (op == ROGUE_BACKEND_OP_ATST) {
      const rogue_backend_instr *atst = instr_as;
      bool ifb = rogue_backend_op_mod_is_set(atst, ROGUE_BACKEND_OP_MOD_IFB);

      discard |= !ifb;
      side_effects |= !ifb;
   }

   data->discard = discard;
   data->side_effects = side_effects;

   return true;
}

static void rogue_collect_late_fs_build_data(rogue_shader *shader)
{
   struct rogue_fs_build_data *data = &shader->ctx->stage_data.fs;
   rogue_instr_filter filter = { 0 };
   BITSET_SET(filter.backend_mask, ROGUE_BACKEND_OP_ATST);
   rogue_find_instrs(shader, &filter, fs_data_cb, data);
}

static void rogue_collect_late_build_data(rogue_shader *shader)
{
   switch (shader->stage) {
   case MESA_SHADER_FRAGMENT:
      return rogue_collect_late_fs_build_data(shader);

   case MESA_SHADER_VERTEX:
      break;

   case MESA_SHADER_COMPUTE:
      break;

   default:
      unreachable("Unsupported shader stage.");
   }
}

static inline void rogue_trim_empty_blocks(rogue_shader *shader)
{
   ASSERTED rogue_block *final_block =
      list_last_entry(&shader->blocks, rogue_block, link);

   rogue_foreach_block_safe (block, shader) {
      if (!list_is_empty(&block->instrs))
         continue;

      /* If the final block is empty we're in trouble. */
      assert(block != final_block);

      if (!list_is_empty(&block->uses)) {
         rogue_block *next_block =
            list_entry(block->link.next, rogue_block, link);

         rogue_foreach_block_use_safe (use, block) {
            rogue_instr *instr = use->instr;
            rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);

            rogue_unlink_instr_use_block(instr, &ctrl->block_use);
            ctrl->target_block = next_block;
            rogue_link_instr_use_block(instr,
                                       &ctrl->block_use,
                                       ctrl->target_block);
         }
      }

      list_del(&block->link);
   }
}

/**
 * \brief Translates a NIR shader to Rogue.
 *
 * \param[in] ctx Shared multi-stage build context.
 * \param[in] nir NIR shader.
 * \return A rogue_shader* if successful, or NULL if unsuccessful.
 */
PUBLIC
rogue_shader *rogue_nir_to_rogue(rogue_build_ctx *ctx, const nir_shader *nir)
{
   gl_shader_stage stage = nir->info.stage;
   rogue_shader *shader = rogue_shader_create(ctx, stage);
   if (!shader)
      return NULL;

   shader->ctx = ctx;

   /* Make sure we only have a single function. */
   assert(exec_list_length(&nir->functions) == 1);

   rogue_builder b;
   rogue_builder_init(&b, shader);

   nir_function_impl *entry = nir_shader_get_entrypoint((nir_shader *)nir);

   /* Reserve temps and check all are 32-bit vec1s. */
   nir_foreach_register (reg, &entry->registers) {
      assert(reg->num_components == 1);
      assert(reg->num_array_elems == 0);
      assert(reg->bit_size <= 32);

      rogue_temp_reg(shader, reg->index);
   }

   /* Go through SSA used by NIR and "reserve" them so that sub-arrays won't be
    * declared before the parent arrays. */
   nir_foreach_block_unstructured (block, entry) {
      nir_foreach_instr (instr, block) {
         if (instr->type == nir_instr_type_load_const) {
            nir_load_const_instr *load_const = nir_instr_as_load_const(instr);
            if (load_const->def.num_components > 1)
               continue;
         }
         nir_foreach_ssa_def(instr, ssa_def_cb, shader);
      }
   }
   ++shader->ctx->next_ssa_idx;

   nir_index_blocks(entry);

   /* Translate shader entrypoint. */
   trans_nir_cf_nodes(&b, &entry->body);
   rogue_END(&b);

   /* Trim empty blocks. */
   rogue_trim_empty_blocks(shader);

   /* Apply passes. */
   rogue_shader_passes(shader);

   /* Collect late build data. */
   rogue_collect_late_build_data(shader);

   rogue_feedback_used_regs(ctx, shader);

   return shader;
}

/**
 * \brief Performs Rogue passes on a shader.
 *
 * \param[in] shader The shader.
 */
PUBLIC
void rogue_shader_passes(rogue_shader *shader)
{
   rogue_validate_shader(shader, "before passes");

   if (ROGUE_DEBUG(IR_PASSES))
      rogue_print_pass_debug(shader, "before passes", stdout);

   /* Passes */
   /* TODO: likely want/need to loop/repeat the first set of these. */
   ROGUE_PASS_V(shader, rogue_constreg);
   /* ROGUE_PASS_V(shader, rogue_copy_prop); */
   /* ROGUE_PASS_V(shader, rogue_dce); */
   ROGUE_PASS_V(shader, rogue_schedule_st_regs);
   ROGUE_PASS_V(shader, rogue_lower_pseudo_ops);
   ROGUE_PASS_V(shader, rogue_constreg);
   /* ROGUE_PASS_V(shader, rogue_copy_prop); */
   ROGUE_PASS_V(shader, rogue_schedule_wdf, false);
   ROGUE_PASS_V(shader, rogue_schedule_uvsw, false);
   ROGUE_PASS_V(shader, rogue_trim);
   ROGUE_PASS_V(shader, rogue_regalloc);
   ROGUE_PASS_V(shader, rogue_lower_late_ops);
   /* ROGUE_PASS_V(shader, rogue_dce); */
   ROGUE_PASS_V(shader, rogue_schedule_instr_groups, false);

   if (ROGUE_DEBUG(IR))
      rogue_print_pass_debug(shader, "after passes", stdout);
}
