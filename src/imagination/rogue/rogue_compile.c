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
#include "util/macros.h"
/* FIXME: Remove once the compiler/driver interface is finalised. */
#include "vulkan/vulkan_core.h"

/**
 * \file rogue_compile.c
 *
 * \brief Contains NIR to Rogue translation functions, and Rogue passes.
 */

/* Helpers. */
static rogue_ref nir_alu_src32(rogue_shader *shader,
                               const nir_alu_instr *alu,
                               unsigned src_num,
                               unsigned *src_components)
{
   bool is_ssa = alu->src[src_num].src.is_ssa;
   assert(nir_src_bit_size(alu->src[src_num].src) == 32);

   unsigned num_components = nir_src_num_components(alu->src[src_num].src);
   unsigned components_required =
      nir_ssa_alu_instr_src_components(alu, src_num);

   assert(components_required == 1 || num_components == components_required);

   if (src_components)
      *src_components = components_required;

   if (num_components > 1) {
      assert(is_ssa);
      /* Select the component. */
      unsigned read_mask = nir_alu_instr_src_read_mask(alu, src_num);
      unsigned component = ffs(read_mask) - 1;
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader,
                                components_required,
                                alu->src[src_num].src.ssa->index,
                                component));
   }

   return rogue_ref_reg(
      is_ssa ? rogue_ssa_reg(shader, alu->src[src_num].src.ssa->index)
             : rogue_temp_reg(shader, alu->src[src_num].src.reg.reg->index));
}

static rogue_ref nir_alu_dst32(rogue_shader *shader,
                               const nir_alu_instr *alu,
                               unsigned *dst_components)
{
   bool is_ssa = alu->dest.dest.is_ssa;
   assert(nir_dest_bit_size(alu->dest.dest) == 32);

   unsigned num_components = nir_dest_num_components(alu->dest.dest);

   if (dst_components)
      *dst_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader,
                                                       num_components,
                                                       alu->dest.dest.ssa.index,
                                                       0));
   }

   return rogue_ref_reg(
      is_ssa ? rogue_ssa_reg(shader, alu->dest.dest.ssa.index)
             : rogue_temp_reg(shader, alu->dest.dest.reg.reg->index));
}

static rogue_ref nir_intr_src32(rogue_shader *shader,
                                const nir_intrinsic_instr *intr,
                                unsigned src_num,
                                unsigned *src_components)
{
   bool is_ssa = intr->src[src_num].is_ssa;
   assert(nir_src_bit_size(intr->src[src_num]) == 32);

   unsigned num_components = nir_src_num_components(intr->src[src_num]);

   if (src_components)
      *src_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader,
                                num_components,
                                intr->src[src_num].ssa->index,
                                0));
   }

   return rogue_ref_reg(
      is_ssa ? rogue_ssa_reg(shader, intr->src[src_num].ssa->index)
             : rogue_temp_reg(shader, intr->src[src_num].reg.reg->index));
}

static rogue_ref nir_intr_dst32(rogue_shader *shader,
                                const nir_intrinsic_instr *intr,
                                unsigned *dst_components)
{
   bool is_ssa = intr->dest.is_ssa;
   assert(nir_dest_bit_size(intr->dest) == 32);

   unsigned num_components = nir_dest_num_components(intr->dest);

   if (dst_components)
      *dst_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      assert(is_ssa);
      return rogue_ref_regarray(rogue_ssa_vec_regarray(shader,
                                                       num_components,
                                                       intr->dest.ssa.index,
                                                       0));
   }

   return rogue_ref_reg(is_ssa
                           ? rogue_ssa_reg(shader, intr->dest.ssa.index)
                           : rogue_temp_reg(shader, intr->dest.reg.reg->index));
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

static void trans_nir_texop_tex(rogue_builder *b, nir_tex_instr *tex)
{
   unsigned channels = nir_dest_num_components(tex->dest);
   unsigned coord_components = tex->coord_components;
   rogue_regarray *dst =
      rogue_ssa_vec_regarray(b->shader, channels, tex->dest.ssa.index, 0);
   rogue_regarray *smp_coords = NULL;
   /* TODO NEXT: get from driver. */
   rogue_regarray *image_state = rogue_shared_regarray(b->shader, 4, 0);
   rogue_regarray *smp_state = rogue_shared_regarray(b->shader, 4, 4);

   assert(channels == 4);
   assert(coord_components == 2);
   assert(tex->sampler_dim == GLSL_SAMPLER_DIM_2D);
   assert(!tex->is_array);
   assert(!tex->is_shadow);
   assert(!tex->is_new_style_shadow);
   assert(!tex->is_sparse);
   assert(!tex->texture_non_uniform);
   assert(!tex->sampler_non_uniform);

   /* TODO NEXT: process tex->texture_index and tex->sampler_index */

   for (unsigned u = 0; u < tex->num_srcs; ++u) {
      switch (tex->src[u].src_type) {
      case nir_tex_src_coord:
         assert(!smp_coords);
         smp_coords = rogue_ssa_vec_regarray(b->shader,
                                             coord_components,
                                             tex->src[u].src.ssa->index,
                                             0);
         continue;

      default:
         break;
      }

      unreachable("Unsupported NIR tex instruction op.");
   }

   assert(smp_coords);

   rogue_backend_instr *smp2d = rogue_SMP2D(b,
                                            rogue_ref_regarray(dst),
                                            rogue_ref_drc(0),
                                            rogue_ref_regarray(image_state),
                                            rogue_ref_regarray(smp_coords),
                                            rogue_ref_regarray(smp_state),
                                            rogue_none(),
                                            rogue_ref_val(channels));

   /* rogue_set_backend_op_mod(smp2d, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
   rogue_set_backend_op_mod(smp2d, ROGUE_BACKEND_OP_MOD_FCNORM);
}

static void trans_nir_tex(rogue_builder *b, nir_tex_instr *tex)
{
   switch (tex->op) {
   case nir_texop_tex:
      return trans_nir_texop_tex(b, tex);

   default:
      break;
   }

   unreachable("Unsupported NIR tex instruction op.");
}

static void trans_nir_load_const32(rogue_builder *b,
                                   nir_load_const_instr *load_const)
{
   unsigned dst_index = load_const->def.index;
   rogue_reg *dst = rogue_ssa_reg(b->shader, dst_index);

   uint32_t imm = nir_const_value_as_uint(load_const->value[0], 32);

   rogue_alu_instr *mov = rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_imm(imm));
   rogue_add_instr_comment(&mov->instr, "load_const_32");
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
   case 32:
      return trans_nir_load_const32(b, load_const);

   case 64:
      return trans_nir_load_const64(b, load_const);

   default:
      break;
   }

   unreachable("Unsupported load_const bit size.");
}

static void trans_nir_intrinsic_load_input_fs(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   struct rogue_fs_build_data *fs_data = &b->shader->ctx->stage_data.fs;

   unsigned load_size;
   rogue_ref dst = nir_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);

   if (io_semantics.location == VARYING_SLOT_POS && component < 2) {
      rogue_reg *src = NULL;

      switch (component) {
      case 0:
         src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_X_P);
         break;

      case 1:
         src = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_Y_P);
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

   unsigned load_size;
   rogue_ref dst = nir_intr_dst32(b->shader, intr, &load_size);
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

static void trans_nir_intrinsic_store_output_fs(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   rogue_reg *dst = rogue_pixout_reg(b->shader, nir_src_as_uint(intr->src[1]));

   unsigned store_size;
   rogue_ref src = nir_intr_src32(b->shader, intr, 0, &store_size);
   /* Pixel output registers can't be used with repeat > 1, so store_size
    * will always be limited to 1.
    */
   assert(store_size == 1);

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

   ASSERTED unsigned store_size;
   rogue_ref src = nir_intr_src32(b->shader, intr, 0, &store_size);
   assert(store_size == 1);

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

static void trans_nir_intrinsic_load_global32(rogue_builder *b,
                                              nir_intrinsic_instr *intr,
                                              bool constant)
{
   unsigned load_size;
   rogue_ref dst = nir_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16); /* TODO: support even larger load sizes. */

   rogue_ref64 src_addr = nir_ssa_intr_src64(b->shader, intr, 0);

   rogue_backend_instr *ld = rogue_LD(b,
                                      dst,
                                      rogue_ref_drc(0),
                                      rogue_ref_val(load_size),
                                      src_addr.ref64);
   rogue_add_instr_commentf(&ld->instr,
                            constant ? "load_global_constant32x%u"
                                     : "load_global32x%u",
                            load_size);
}

static void trans_nir_intrinsic_load_global64(rogue_builder *b,
                                              nir_intrinsic_instr *intr,
                                              bool constant)
{
   rogue_ref64 dst = nir_ssa_intr_dst64(b->shader, intr);
   rogue_ref64 src_addr = nir_ssa_intr_src64(b->shader, intr, 0);

   rogue_backend_instr *ld =
      rogue_LD(b, dst.ref64, rogue_ref_drc(0), rogue_ref_val(2), src_addr.ref64);
   rogue_add_instr_comment(&ld->instr,
                           constant ? "load_global_constant64"
                                    : "load_global64");
}

static void trans_nir_intrinsic_load_global(rogue_builder *b,
                                            nir_intrinsic_instr *intr,
                                            bool constant)
{
   unsigned bit_size = nir_dest_bit_size(intr->dest);

   switch (bit_size) {
   case 32:
      return trans_nir_intrinsic_load_global32(b, intr, constant);

   case 64:
      return trans_nir_intrinsic_load_global64(b, intr, constant);

   default:
      break;
   }

   unreachable("Unsupported load_global bit size.");
}

static void trans_nir_intrinsic_store_global(rogue_builder *b,
                                             nir_intrinsic_instr *intr)
{
   rogue_ref64 dst_addr = nir_ssa_intr_src64(b->shader, intr, 1);

   unsigned store_size;
   rogue_ref src = nir_intr_src32(b->shader, intr, 0, &store_size);
   assert(store_size == 1); /* TODO: Burst store support. */

   rogue_backend_instr *st = rogue_ST(b,
                                      src,
                                      rogue_ref_val(2),
                                      rogue_ref_drc(0),
                                      rogue_ref_val(store_size),
                                      dst_addr.ref64,
                                      rogue_none());

   /* TODO: cache flags */
   rogue_add_instr_comment(&st->instr, "store_global");
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
trans_nir_intrinsic_load_local_invocation_id_img(rogue_builder *b,
                                                 nir_intrinsic_instr *intr,
                                                 bool yz)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   unsigned load_size;
   rogue_ref dst = nir_intr_dst32(b->shader, intr, &load_size);
   assert(load_size == 1);

   assert(cs_data->local_id_regs[yz] != ROGUE_REG_UNUSED);
   rogue_reg *src = rogue_vtxin_reg(b->shader, cs_data->local_id_regs[yz]);
   rogue_alu_instr *mov = rogue_MOV(b, dst, rogue_ref_reg(src));

   rogue_add_instr_commentf(&mov->instr,
                            "load_local_invocation_id.%s",
                            yz ? "yz" : "x");
}

static void trans_nir_intrinsic_load_workgroup_id_img(rogue_builder *b,
                                                      nir_intrinsic_instr *intr,
                                                      unsigned component)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   unsigned load_size;
   rogue_ref dst = nir_intr_dst32(b->shader, intr, &load_size);
   assert(load_size == 1);

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
   rogue_ref src = nir_intr_src32(b->shader, intr, 0, NULL);
   rogue_backend_instr *atst_if = rogue_ATST_IF(b, src, rogue_ref_imm(0));
   /* For ATST false = discard; pass if == 0, discard if != 0. */
   rogue_set_backend_op_mod(atst_if, ROGUE_BACKEND_OP_MOD_EQUAL);
   rogue_add_instr_comment(&atst_if->instr, "discard_if");
}

static void trans_nir_intrinsic(rogue_builder *b, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return trans_nir_intrinsic_load_input(b, intr);

   case nir_intrinsic_store_output:
      return trans_nir_intrinsic_store_output(b, intr);

   case nir_intrinsic_load_vulkan_desc_set_table_base_addr_img:
      return trans_nir_intrinsic_load_vulkan_desc_set_table_base_addr_img(b,
                                                                          intr);

   case nir_intrinsic_load_global_constant:
      return trans_nir_intrinsic_load_global(b, intr, true);

   case nir_intrinsic_load_global:
      return trans_nir_intrinsic_load_global(b, intr, false);

   case nir_intrinsic_store_global:
      return trans_nir_intrinsic_store_global(b, intr);

   case nir_intrinsic_load_push_consts_base_addr_img:
      return trans_nir_load_push_consts_base_addr_img(b, intr);

   case nir_intrinsic_load_local_invocation_id_x_img:
      return trans_nir_intrinsic_load_local_invocation_id_img(b, intr, false);

   case nir_intrinsic_load_local_invocation_id_yz_img:
      return trans_nir_intrinsic_load_local_invocation_id_img(b, intr, true);

   case nir_intrinsic_load_workgroup_id_x_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 0);

   case nir_intrinsic_load_workgroup_id_y_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 1);

   case nir_intrinsic_load_workgroup_id_z_img:
      return trans_nir_intrinsic_load_workgroup_id_img(b, intr, 2);

   case nir_intrinsic_load_num_workgroups_base_addr_img:
      return trans_nir_intrinsic_load_num_workgroups_base_addr_img(b, intr);

   case nir_intrinsic_discard:
      return trans_nir_intrinsic_discard(b, intr);

   case nir_intrinsic_discard_if:
      return trans_nir_intrinsic_discard_if(b, intr);

   default:
      break;
   }

   unreachable("Unsupported NIR intrinsic instruction.");
}

/* TODO: commonise. */
static void trans_nir_alu_pack_unorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 4);

   rogue_alu_instr *pck_u8888 = rogue_PCK_U8888(b, dst, src);
   rogue_set_instr_repeat(&pck_u8888->instr, src_components);
   rogue_set_alu_op_mod(pck_u8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_unorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 4);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_alu_instr *upck_u8888 = rogue_UPCK_U8888(b, dst, src);
   rogue_set_instr_repeat(&upck_u8888->instr, dst_components);
   rogue_set_alu_op_mod(upck_u8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_snorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 4);

   rogue_alu_instr *pck_s8888 = rogue_PCK_S8888(b, dst, src);
   rogue_set_instr_repeat(&pck_s8888->instr, src_components);
   rogue_set_alu_op_mod(pck_s8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_snorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 4);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_alu_instr *upck_s8888 = rogue_UPCK_S8888(b, dst, src);
   rogue_set_instr_repeat(&upck_s8888->instr, dst_components);
   rogue_set_alu_op_mod(upck_s8888, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_unorm_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 2);

   rogue_alu_instr *pck_u1616 = rogue_PCK_U1616(b, dst, src);
   rogue_set_instr_repeat(&pck_u1616->instr, src_components);
   rogue_set_alu_op_mod(pck_u1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_unorm_2x16(rogue_builder *b,
                                            nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 2);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_alu_instr *upck_u1616 = rogue_UPCK_U1616(b, dst, src);
   rogue_set_instr_repeat(&upck_u1616->instr, dst_components);
   rogue_set_alu_op_mod(upck_u1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_snorm_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 2);

   rogue_alu_instr *pck_s1616 = rogue_PCK_S1616(b, dst, src);
   rogue_set_instr_repeat(&pck_s1616->instr, src_components);
   rogue_set_alu_op_mod(pck_s1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_unpack_snorm_2x16(rogue_builder *b,
                                            nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 2);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_alu_instr *upck_s1616 = rogue_UPCK_S1616(b, dst, src);
   rogue_set_instr_repeat(&upck_s1616->instr, dst_components);
   rogue_set_alu_op_mod(upck_s1616, ROGUE_ALU_OP_MOD_SCALE);
}

static void trans_nir_alu_pack_half_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 2);

   rogue_alu_instr *pck_f16f16 = rogue_PCK_F16F16(b, dst, src);
   rogue_set_instr_repeat(&pck_f16f16->instr, src_components);
   /* rogue_set_alu_op_mod(pck_f16f16, ROGUE_ALU_OP_MOD_ROUNDZERO); */
}

static void trans_nir_alu_unpack_half_2x16(rogue_builder *b, nir_alu_instr *alu)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 2);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_alu_instr *upck_f16f16 = rogue_UPCK_F16F16(b, dst, src);
   rogue_set_instr_repeat(&upck_f16f16->instr, dst_components);
   /* rogue_set_alu_op_mod(upck_f16f16, ROGUE_ALU_OP_MOD_ROUNDZERO); */
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
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

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
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

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
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);
   rogue_ref src2 = nir_alu_src32(b->shader, alu, 2, NULL);

   rogue_alu_instr *ffma = rogue_FMAD(b, dst, src0, src1, src2);
   rogue_apply_alu_src_mods(ffma, alu, false);
}

static void trans_nir_alu_frcp(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *frcp = rogue_FRCP(b, dst, src);
   rogue_apply_alu_src_mods(frcp, alu, false);
}

static void trans_nir_alu_frsq(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *frsq = rogue_FRSQ(b, dst, src);
   rogue_apply_alu_src_mods(frsq, alu, false);
}

static void trans_nir_alu_flog2(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *flog2 = rogue_FLOG2(b, dst, src);
   rogue_apply_alu_src_mods(flog2, alu, false);
}

static void trans_nir_alu_fexp2(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *fexp2 = rogue_FEXP2(b, dst, src);
   rogue_apply_alu_src_mods(fexp2, alu, false);
}

static void trans_nir_alu_fddx(rogue_builder *b, nir_alu_instr *alu, bool fine)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *fdsx = fine ? rogue_FDSXF(b, dst, src)
                                : rogue_FDSX(b, dst, src);

   rogue_apply_alu_src_mods(fdsx, alu, false);
}

static void trans_nir_alu_fddy(rogue_builder *b, nir_alu_instr *alu, bool fine)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *fdsy = fine ? rogue_FDSYF(b, dst, src)
                                : rogue_FDSY(b, dst, src);

   rogue_apply_alu_src_mods(fdsy, alu, false);
}

static void trans_nir_alu_minmax(rogue_builder *b,
                                 nir_alu_instr *alu,
                                 enum rogue_alu_op_mod type,
                                 bool max)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *minmax = max ? rogue_MAX(b, dst, src0, src1)
                                 : rogue_MIN(b, dst, src0, src1);
   rogue_set_alu_op_mod(minmax, type);
   rogue_apply_alu_src_mods(minmax, alu, false);
}

/* Conditionally sets the output to src1 or src2 depending on whether the
 * comparison between src0 and 0 is true or false.
 */
/* reverse exists because we only have == 0, > 0 and >= 0 but not != 0, so this
 * lets us use Z and invert the srcs. */
static void trans_nir_alu_csel(rogue_builder *b,
                               nir_alu_instr *alu,
                               enum rogue_alu_op_mod comp,
                               enum rogue_alu_op_mod type,
                               bool reverse,
                               const char *comment)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, reverse ? 2 : 1, NULL);
   rogue_ref src2 = nir_alu_src32(b->shader, alu, reverse ? 1 : 2, NULL);

   rogue_alu_instr *csel = rogue_CSEL(b, dst, src0, src1, src2);
   rogue_set_alu_op_mod(csel, comp);
   rogue_set_alu_op_mod(csel, type);
   rogue_add_instr_comment(&csel->instr, comment);
   rogue_apply_alu_src_mods(csel, alu, reverse);
}

static void trans_nir_alu_fneg(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_FNEG(b, dst, src);
}

static void trans_nir_alu_fabs(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_FABS(b, dst, src);
}

static void trans_nir_alu_ffloor(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_FFLR(b, dst, src);
}

static void
trans_nir_alu_fsin_cos(rogue_builder *b, nir_alu_instr *alu, bool cos)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   enum rogue_alu_op_mod mod = cos ? ROGUE_ALU_OP_MOD_COS
                                   : ROGUE_ALU_OP_MOD_SIN;

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

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
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   unsigned src_components;
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, &src_components);
   assert(src_components == 1);

   rogue_MOV(b, dst, src);
}

static void trans_nir_alu_vecN(rogue_builder *b, nir_alu_instr *alu, unsigned n)
{
   assert(alu->dest.dest.is_ssa);
   unsigned dst_index = alu->dest.dest.ssa.index;

   rogue_ssa_vec_regarray(b->shader, n, dst_index, 0);

   rogue_regarray *dst;
   ASSERTED unsigned src_components;
   for (unsigned u = 0; u < n; ++u) {
      dst = rogue_ssa_vec_regarray(b->shader, 1, dst_index, u);
      rogue_ref src = nir_alu_src32(b->shader, alu, u, &src_components);
      assert(src_components == 1);
      rogue_MOV(b, rogue_ref_regarray(dst), src);
   }
}

static void trans_nir_alu_iadd32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *iadd32 = rogue_IADD32(b, dst, src0, src1);
   rogue_set_alu_op_mod(iadd32, ROGUE_ALU_OP_MOD_S);
   rogue_apply_alu_src_mods(iadd32, alu, false);
}

static void trans_nir_alu_iadd64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref64 src0 = nir_ssa_alu_src64(b->shader, alu, 0);
   rogue_ref64 src1 = nir_ssa_alu_src64(b->shader, alu, 1);

   rogue_alu_instr *iadd64 = rogue_IADD64(b, dst.ref64, src0.ref64, src1.ref64);
   /* N.B. No sign flag support for add64. */
   rogue_apply_alu_src_mods(iadd64, alu, false);
}

static void trans_nir_alu_iadd(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   switch (bit_size) {
   case 32:
      return trans_nir_alu_iadd32(b, alu);

   case 64:
      return trans_nir_alu_iadd64(b, alu);

   default:
      break;
   }

   unreachable("Unsupported iadd bit size.");
}

static void trans_nir_alu_imul32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *imul32 = rogue_IMUL32(b, dst, src0, src1);
   rogue_set_alu_op_mod(imul32, ROGUE_ALU_OP_MOD_S);
   rogue_apply_alu_src_mods(imul32, alu, false);
}

static void trans_nir_alu_imul64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *imul64 = rogue_IMUL64(b, dst.ref64, src0, src1);
   rogue_set_alu_op_mod(imul64, ROGUE_ALU_OP_MOD_S);
   rogue_apply_alu_src_mods(imul64, alu, false);
}

static void trans_nir_alu_imul(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);

   switch (bit_size) {
   case 32:
      return trans_nir_alu_imul32(b, alu);

   case 64:
      return trans_nir_alu_imul64(b, alu);

   default:
      break;
   }

   unreachable("Unsupported imul bit size.");
}

static void
trans_nir_alu_mul_high(rogue_builder *b, nir_alu_instr *alu, bool is_signed)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *mul_high = is_signed ? rogue_IMUL_HIGH(b, dst, src0, src1)
                                         : rogue_UMUL_HIGH(b, dst, src0, src1);
   rogue_apply_alu_src_mods(mul_high, alu, false);
}

static void
trans_nir_alu_mul_low(rogue_builder *b, nir_alu_instr *alu, bool is_signed)
{
   ASSERTED unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *mul_low = is_signed ? rogue_IMUL_LOW(b, dst, src0, src1)
                                        : rogue_UMUL_LOW(b, dst, src0, src1);
   rogue_apply_alu_src_mods(mul_low, alu, false);
}

static void trans_nir_alu_ineg32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_INEG32(b, dst, src);
}

static void trans_nir_alu_ineg64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref64 src = nir_ssa_alu_src64(b->shader, alu, 0);

   rogue_INEG64(b, dst.ref64, src.ref64);
}

static void trans_nir_alu_ineg(rogue_builder *b, nir_alu_instr *ineg)
{
   unsigned bit_size = nir_dest_bit_size(ineg->dest.dest);

   switch (bit_size) {
   case 32:
      return trans_nir_alu_ineg32(b, ineg);

   case 64:
      return trans_nir_alu_ineg64(b, ineg);

   default:
      break;
   }

   unreachable("Unsupported ineg bit size.");
}

static void trans_nir_alu_iabs32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_IABS32(b, dst, src);
}

static void trans_nir_alu_iabs64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref64 src = nir_ssa_alu_src64(b->shader, alu, 0);

   rogue_IABS64(b, dst.ref64, src.ref64);
}

static void trans_nir_alu_iabs(rogue_builder *b, nir_alu_instr *iabs)
{
   unsigned bit_size = nir_dest_bit_size(iabs->dest.dest);

   switch (bit_size) {
   case 32:
      return trans_nir_alu_iabs32(b, iabs);

   case 64:
      return trans_nir_alu_iabs64(b, iabs);

   default:
      break;
   }

   unreachable("Unsupported ineg bit size.");
}

static void trans_nir_alu_i2i64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);

   rogue_MOV(b, dst.lo32, src);
   rogue_MOV(b, dst.hi32, rogue_ref_imm(0));
}

/* Conditionally sets the output to 1 or 0 depending on whether the comparison
 * is true or false. */
static void trans_nir_alu_cmp(rogue_builder *b,
                              nir_alu_instr *alu,
                              enum rogue_alu_op_mod comp,
                              enum rogue_alu_op_mod type)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_alu_instr *cmp = rogue_CMP(b, dst, src0, src1);
   rogue_set_alu_op_mod(cmp, comp);
   rogue_set_alu_op_mod(cmp, type);
   rogue_apply_alu_src_mods(cmp, alu, false);
}

static void trans_nir_alu_f2i32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *pck_s32 = rogue_PCK_S32(b, dst, src);
   rogue_set_alu_op_mod(pck_s32, ROGUE_ALU_OP_MOD_ROUNDZERO);
}

static void trans_nir_alu_f2u32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *pck_u32 = rogue_PCK_U32(b, dst, src);
   rogue_set_alu_op_mod(pck_u32, ROGUE_ALU_OP_MOD_ROUNDZERO);
}

static void trans_nir_alu_i2f32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *upck_s32 = rogue_UPCK_S32(b, dst, src);
   rogue_set_alu_op_mod(upck_s32, ROGUE_ALU_OP_MOD_ROUNDZERO);
}

static void trans_nir_alu_u2f32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *upck_u32 = rogue_UPCK_U32(b, dst, src);
   rogue_set_alu_op_mod(upck_u32, ROGUE_ALU_OP_MOD_ROUNDZERO);
}

static void trans_nir_alu_b2f32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *csel =
      rogue_CSEL(b, dst, src, rogue_ref_imm_f(0.0f), rogue_ref_imm_f(1.0f));
   rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_Z);
   rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_U32);
   rogue_add_instr_comment(&csel->instr, "b2f32");
}

static void trans_nir_alu_b2i32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_alu_instr *csel =
      rogue_CSEL(b, dst, src, rogue_ref_imm(0), rogue_ref_imm(1));
   rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_Z);
   rogue_set_alu_op_mod(csel, ROGUE_ALU_OP_MOD_U32);
   rogue_add_instr_comment(&csel->instr, "b2i32");
}

static void trans_nir_alu_iand(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_IAND(b, dst, src0, src1);
}

static void trans_nir_alu_ior(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_IOR(b, dst, src0, src1);
}

static void trans_nir_alu_ixor(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_IXOR(b, dst, src0, src1);
}

static void trans_nir_alu_inot(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_INOT(b, dst, src);
}

static void trans_nir_ishr(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_ISHR(b, dst, src0, src1);
}

static void trans_nir_ishl(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_ISHL(b, dst, src0, src1);
}

static void trans_nir_ushr(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_USHR(b, dst, src0, src1);
}

static void trans_nir_bitfield_reverse(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_IREV(b, dst, src);
}

static void trans_nir_bit_count(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_ICBS(b, dst, src);
}

static void trans_nir_ufind_msb(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_alu_src32(b->shader, alu, 0, NULL);

   rogue_IFTB(b, dst, src);
}

static void
trans_nir_unpack_64_2x32_split(rogue_builder *b, nir_alu_instr *alu, bool hi32)
{
   unsigned dst_components;
   rogue_ref dst = nir_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref64 src = nir_ssa_alu_src64(b->shader, alu, 0);
   rogue_MOV(b, dst, hi32 ? src.hi32 : src.lo32);
}

static void trans_nir_pack_64_2x32_split(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref64 dst = nir_ssa_alu_dst64(b->shader, alu);
   rogue_ref src0 = nir_alu_src32(b->shader, alu, 0, NULL);
   rogue_ref src1 = nir_alu_src32(b->shader, alu, 1, NULL);

   rogue_MOV(b, dst.lo32, src0);
   rogue_MOV(b, dst.hi32, src1);
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
      return trans_nir_alu_minmax(b, alu, OM(F32), false);

   case nir_op_fmax:
      return trans_nir_alu_minmax(b, alu, OM(F32), true);

   case nir_op_imin:
      return trans_nir_alu_minmax(b, alu, OM(S32), false);

   case nir_op_imax:
      return trans_nir_alu_minmax(b, alu, OM(S32), true);

   case nir_op_umin:
      return trans_nir_alu_minmax(b, alu, OM(U32), false);

   case nir_op_umax:
      return trans_nir_alu_minmax(b, alu, OM(U32), true);

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

   case nir_op_i2i64:
      return trans_nir_alu_i2i64(b, alu);

   case nir_op_flt32:
      return trans_nir_alu_cmp(b, alu, OM(L), OM(F32));

   case nir_op_fge32:
      return trans_nir_alu_cmp(b, alu, OM(GE), OM(F32));

   case nir_op_feq32:
      return trans_nir_alu_cmp(b, alu, OM(E), OM(F32));

   case nir_op_fneu32:
      return trans_nir_alu_cmp(b, alu, OM(NE), OM(F32));

   case nir_op_ilt32:
      return trans_nir_alu_cmp(b, alu, OM(L), OM(S32));

   case nir_op_ige32:
      return trans_nir_alu_cmp(b, alu, OM(GE), OM(S32));

   case nir_op_ieq32:
      return trans_nir_alu_cmp(b, alu, OM(E), OM(S32));

   case nir_op_ine32:
      return trans_nir_alu_cmp(b, alu, OM(NE), OM(S32));

   case nir_op_ult32:
      return trans_nir_alu_cmp(b, alu, OM(L), OM(U32));

   case nir_op_uge32:
      return trans_nir_alu_cmp(b, alu, OM(GE), OM(U32));

   case nir_op_f2i32:
      return trans_nir_alu_f2i32(b, alu);

   case nir_op_f2u32:
      return trans_nir_alu_f2u32(b, alu);

   case nir_op_i2f32:
      return trans_nir_alu_i2f32(b, alu);

   case nir_op_b2f32:
      return trans_nir_alu_b2f32(b, alu);

   case nir_op_u2f32:
      return trans_nir_alu_u2f32(b, alu);

   case nir_op_b2i32:
      return trans_nir_alu_b2i32(b, alu);

   case nir_op_iand:
      return trans_nir_alu_iand(b, alu);

   case nir_op_ior:
      return trans_nir_alu_ior(b, alu);

   case nir_op_ixor:
      return trans_nir_alu_ixor(b, alu);

   case nir_op_inot:
      return trans_nir_alu_inot(b, alu);

   case nir_op_fcsel:
      return trans_nir_alu_csel(b, alu, OM(Z), OM(F32), true, "fcsel");

   case nir_op_fcsel_gt:
      return trans_nir_alu_csel(b, alu, OM(GZ), OM(F32), false, "fcsel_gt");

   case nir_op_fcsel_ge:
      return trans_nir_alu_csel(b, alu, OM(GEZ), OM(F32), false, "fcsel_ge");

   case nir_op_b32csel:
      return trans_nir_alu_csel(b, alu, OM(Z), OM(U32), true, "b32csel");

   case nir_op_i32csel_gt:
      return trans_nir_alu_csel(b, alu, OM(GZ), OM(S32), false, "i32csel_gt");

   case nir_op_i32csel_ge:
      return trans_nir_alu_csel(b, alu, OM(GEZ), OM(S32), false, "i32csel_ge");

   case nir_op_ishr:
      return trans_nir_ishr(b, alu);

   case nir_op_ishl:
      return trans_nir_ishl(b, alu);

   case nir_op_ushr:
      return trans_nir_ushr(b, alu);

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

   const bool has_if = !nir_cf_list_is_empty_block(&nif->then_list);
   const bool has_else = !nir_cf_list_is_empty_block(&nif->else_list);
   assert(has_if || has_else);

   /* Set P0 if the condition is true (not equal to 0). */
   rogue_SETPRED(b, rogue_ref_io(ROGUE_IO_P0), rogue_ref_reg(if_cnd));

   /* Check P0 and increment mask if false. */
   cnd = rogue_CNDST(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));

   /* If the if block is empty, flip the condition and just emit the else block.
    */
   rogue_set_ctrl_op_mod(cnd,
                         has_if ? ROGUE_CTRL_OP_MOD_P0_TRUE
                                : ROGUE_CTRL_OP_MOD_P0_FALSE);

   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

   /* If block. */
   if (has_if)
      trans_nir_cf_nodes(b, &nif->then_list);

   /* Else: if masked out due to failing if condition, enable, otherwise if we
    * did the if, mask out the else block, otherwise just leave the mask
    * unchanged. */
   if (has_if && has_else) {
      cnd =
         rogue_CNDEF(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
      rogue_set_ctrl_op_mod(cnd, ROGUE_CTRL_OP_MOD_ALWAYS);
      rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);
   }

   /* Else block. */
   if (has_else)
      trans_nir_cf_nodes(b, &nif->else_list);

   /* Restore the mask to what it was before this if code. */
   cnd = rogue_CNDEND(b, rogue_ref_io(ROGUE_IO_PE), emc, emc, rogue_ref_val(1));
   rogue_set_instr_exec_cond(&cnd->instr, ROGUE_EXEC_COND_PE_ANY);

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

   rogue_ctrl_instr *loop_start = cnd;

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

   rogue_push_block(b);

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

   rogue_ctrl_instr *loop_end = br;

   loop_start->loop_start = true;
   loop_start->loop_link = &loop_end->instr;
   loop_end->loop_link = &loop_start->instr;

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

      /* There shouldn't be any empty NIR blocks - these can't be deleted! */
      assert(block->nir_index == ~0U);

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
      assert(reg->bit_size == 32);

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
