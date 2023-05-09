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
static rogue_ref nir_ssa_reg_alu_src1(rogue_shader *shader,
                                      const nir_alu_instr *alu,
                                      unsigned src_num)
{
   assert(alu->src[src_num].src.is_ssa);
   assert(alu->src[src_num].src.ssa->bit_size == 1);

   unsigned index = alu->src[src_num].src.ssa->index;
   unsigned num_components = alu->src[src_num].src.ssa->num_components;
   unsigned num_components_used =
      nir_ssa_alu_instr_src_components(alu, src_num);

   if (num_components > 1) {
      /* Select the component. */
      unsigned read_mask = nir_alu_instr_src_read_mask(alu, src_num);
      unsigned component = ffs(read_mask) - 1;
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components_used, index, component));
   }

   return rogue_ref_reg(rogue_ssa_reg(shader, index));
}

static rogue_ref nir_ssa_reg_alu_src32(rogue_shader *shader,
                                       const nir_alu_instr *alu,
                                       unsigned src_num)
{
   assert(alu->src[src_num].src.is_ssa);
   assert(alu->src[src_num].src.ssa->bit_size == 32);

   unsigned index = alu->src[src_num].src.ssa->index;
   unsigned num_components = alu->src[src_num].src.ssa->num_components;
   unsigned num_components_used =
      nir_ssa_alu_instr_src_components(alu, src_num);

   if (num_components > 1) {
      /* Select the component. */
      unsigned read_mask = nir_alu_instr_src_read_mask(alu, src_num);
      unsigned component = ffs(read_mask) - 1;
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components_used, index, component));
   }

   return rogue_ref_reg(rogue_ssa_reg(shader, index));
}

static rogue_ref nir_ssa_reg_alu_dst1(rogue_shader *shader,
                                      const nir_alu_instr *alu,
                                      unsigned *dst_components)
{
   assert(alu->dest.dest.is_ssa);
   assert(alu->dest.dest.ssa.bit_size == 1);

   unsigned index = alu->dest.dest.ssa.index;
   unsigned num_components = alu->dest.dest.ssa.num_components;

   if (dst_components)
      *dst_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1)
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));

   return rogue_ref_reg(rogue_ssa_reg(shader, index));
}

static rogue_ref nir_ssa_reg_alu_dst32(rogue_shader *shader,
                                       const nir_alu_instr *alu,
                                       unsigned *dst_components)
{
   assert(alu->dest.dest.is_ssa);
   assert(alu->dest.dest.ssa.bit_size == 32);

   unsigned index = alu->dest.dest.ssa.index;
   unsigned num_components = alu->dest.dest.ssa.num_components;

   if (dst_components)
      *dst_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));
   }

   return rogue_ref_reg(rogue_ssa_reg(shader, index));
}

static rogue_ref nir_ssa_reg_intr_dst32(rogue_shader *shader,
                                        const nir_intrinsic_instr *intr,
                                        unsigned *dst_components)
{
   assert(intr->dest.is_ssa);
   assert(intr->dest.ssa.bit_size == 32);

   unsigned index = intr->dest.ssa.index;
   unsigned num_components = intr->dest.ssa.num_components;

   if (dst_components)
      *dst_components = num_components;

   /* SSA, so always assigning to the entire vector. */
   if (num_components > 1) {
      return rogue_ref_regarray(
         rogue_ssa_vec_regarray(shader, num_components, index, 0));
   }

   return rogue_ref_reg(rogue_ssa_reg(shader, index));
}

/* 64-bit restricted to scalars. */
static rogue_ref nir_ssa_reg_alu_src64(rogue_shader *shader,
                                       const nir_alu_instr *alu,
                                       unsigned src_num,
                                       rogue_ref *lo32,
                                       rogue_ref *hi32)
{
   assert(alu->src[src_num].src.is_ssa);
   assert(alu->src[src_num].src.ssa->bit_size == 64);
   assert(alu->src[src_num].src.ssa->num_components == 1);
   assert(nir_ssa_alu_instr_src_components(alu, src_num) == 1);

   unsigned index = alu->src[src_num].src.ssa->index;
   rogue_ref64 src = rogue_ssa_ref64(shader, index);

   if (lo32)
      *lo32 = src.lo32;

   if (hi32)
      *hi32 = src.hi32;

   return src.ref64;
}

static rogue_ref nir_ssa_reg_alu_dst64(rogue_shader *shader,
                                       const nir_alu_instr *alu,
                                       rogue_ref *lo32,
                                       rogue_ref *hi32)
{
   assert(alu->dest.dest.is_ssa);
   assert(alu->dest.dest.ssa.bit_size == 64);
   assert(alu->dest.dest.ssa.num_components == 1);

   unsigned index = alu->dest.dest.ssa.index;
   rogue_ref64 dst = rogue_ssa_ref64(shader, index);

   if (lo32)
      *lo32 = dst.lo32;

   if (hi32)
      *hi32 = dst.hi32;

   return dst.ref64;
}

static void trans_nir_jump(rogue_builder *b, nir_jump_instr *jump)
{
   switch (jump->type) {
   default:
      break;
   }

   unreachable("Unimplemented NIR jump instruction type.");
}

static void trans_nir_texop_tex(rogue_builder *b, nir_tex_instr *tex)
{
   unsigned channels = nir_dest_num_components(tex->dest);
   unsigned coord_components = tex->coord_components;
   rogue_regarray *dst =
      rogue_ssa_vec_regarray(b->shader, channels, tex->dest.ssa.index, 0);
   rogue_regarray *smp_coords = NULL;
   /* TODO NEXT: get from driver. */
   rogue_regarray *image_state = rogue_shared_regarray(b->shader, 4, 4);
   rogue_regarray *smp_state = rogue_shared_regarray(b->shader, 4, 0);
   assert(false);

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

      unreachable("Unimplemented NIR tex instruction op.");
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

   rogue_set_backend_op_mod(smp2d, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK);
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

   unreachable("Unimplemented NIR tex instruction op.");
}

static void trans_nir_load_const(rogue_builder *b,
                                 nir_load_const_instr *load_const)
{
   unsigned dst_index = load_const->def.index;
   unsigned bit_size = load_const->def.bit_size;
   switch (bit_size) {
   case 32: {
      rogue_reg *dst = rogue_ssa_reg(b->shader, dst_index);
      uint32_t imm = nir_const_value_as_uint(load_const->value[0], 32);
      rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_imm(imm));

      break;
   }

   case 64: {
      uint64_t imm = nir_const_value_as_uint(load_const->value[0], 64);

      rogue_ref64 dst = rogue_ssa_ref64(b->shader, dst_index);
      rogue_ref imm_lo32 = rogue_ref_imm(imm & 0xffffffff);
      rogue_ref imm_hi32 = rogue_ref_imm((imm >> 32) & 0xffffffff);

      rogue_MOV(b, dst.lo32, imm_lo32);
      rogue_MOV(b, dst.hi32, imm_hi32);

      break;
   }

   default:
      unreachable("Unimplemented NIR load_const bit size.");
   }
}

static void trans_nir_intrinsic_load_input_fs(rogue_builder *b,
                                              nir_intrinsic_instr *intr)
{
   struct rogue_fs_build_data *fs_data = &b->shader->ctx->stage_data.fs;

   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
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

      rogue_instr *instr = &rogue_FITRP_PIXEL(b,
                                              dst,
                                              rogue_ref_drc(0),
                                              rogue_ref_regarray(coeffs),
                                              rogue_ref_regarray(wcoeffs),
                                              rogue_ref_val(load_size))
                               ->instr;
      rogue_add_instr_comment(instr, "load_input_fs_smooth");
      break;
   }
   case INTERP_MODE_NOPERSPECTIVE: {
      rogue_regarray *coeffs =
         rogue_coeff_regarray(b->shader,
                              ROGUE_COEFF_ALIGN * load_size,
                              coeff_index);

      rogue_instr *instr = &rogue_FITR_PIXEL(b,
                                             dst,
                                             rogue_ref_drc(0),
                                             rogue_ref_regarray(coeffs),
                                             rogue_ref_val(load_size))
                               ->instr;
      rogue_add_instr_comment(instr, "load_input_fs_npc");
      break;
   }
   case INTERP_MODE_FLAT:
      for (int i = 0; i < load_size; ++i) {
         rogue_reg *coeff_c = rogue_coeff_reg(
            b->shader,
            coeff_index + i * ROGUE_COEFF_ALIGN + ROGUE_COEFF_COMPONENT_C);
         rogue_reg *dst_i = rogue_ssa_reg(b->shader, intr->dest.ssa.index + i);

         rogue_instr *instr =
            &rogue_MOV(b, rogue_ref_reg(dst_i), rogue_ref_reg(coeff_c))->instr;

         rogue_add_instr_comment(instr, "load_input_fs_flat");
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
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
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
         rogue_instr *instr = &rogue_MOV(b, dst, rogue_ref_imm_f(1.0f))->instr;
         rogue_add_instr_comment(instr, "load_input_vs (1.0f)");
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
   rogue_instr *instr = &rogue_MOV(b, dst, rogue_ref_reg(src))->instr;
   rogue_add_instr_comment(instr, "load_input_vs");
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

   unreachable("Unimplemented NIR load_input variant.");
}

static void trans_nir_intrinsic_store_output_fs(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   ASSERTED unsigned store_size = nir_src_num_components(intr->src[0]);
   assert(store_size == 1);

   /* TODO: When hoisting I/O allocation to the driver, check if this is
    * correct.
    */
   unsigned pixout_index = nir_src_as_uint(intr->src[1]);

   rogue_reg *dst = rogue_pixout_reg(b->shader, pixout_index);
   rogue_reg *src = rogue_ssa_reg(b->shader, intr->src[0].ssa->index);

   rogue_instr *instr =
      &rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_reg(src))->instr;
   rogue_add_instr_comment(instr, "store_output_fs");
}

static void trans_nir_intrinsic_store_output_vs(rogue_builder *b,
                                                nir_intrinsic_instr *intr)
{
   struct rogue_vs_build_data *vs_data = &b->shader->ctx->stage_data.vs;

   ASSERTED unsigned store_size = nir_src_num_components(intr->src[0]);
   assert(store_size == 1);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
   unsigned vtxout_index = rogue_output_index_vs(&vs_data->outputs,
                                                 io_semantics.location,
                                                 component);

   rogue_reg *dst = rogue_vtxout_reg(b->shader, vtxout_index);
   rogue_reg *src = rogue_ssa_reg(b->shader, intr->src[0].ssa->index);

   rogue_instr *instr =
      &rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_reg(src))->instr;
   rogue_add_instr_comment(instr, "store_output_vs");
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

   unreachable("Unimplemented NIR store_output variant.");
}

static inline gl_shader_stage
pvr_stage_to_mesa(enum pvr_stage_allocation pvr_stage)
{
   switch (pvr_stage) {
   case PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY:
      return MESA_SHADER_VERTEX;

   case PVR_STAGE_ALLOCATION_FRAGMENT:
      return MESA_SHADER_FRAGMENT;

   case PVR_STAGE_ALLOCATION_COMPUTE:
      return MESA_SHADER_COMPUTE;

   default:
      break;
   }

   unreachable("Unsupported pvr_stage_allocation.");
}

static inline enum pvr_stage_allocation
mesa_stage_to_pvr(gl_shader_stage mesa_stage)
{
   switch (mesa_stage) {
   case MESA_SHADER_VERTEX:
      return PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY;

   case MESA_SHADER_FRAGMENT:
      return PVR_STAGE_ALLOCATION_FRAGMENT;

   case MESA_SHADER_COMPUTE:
      return PVR_STAGE_ALLOCATION_COMPUTE;

   default:
      break;
   }

   unreachable("Unsupported gl_shader_stage.");
}
static bool descriptor_is_dynamic(VkDescriptorType type)
{
   return (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
}

/* TODO: Process this into loads in NIR instead. */
static void
trans_nir_intrinsic_load_vulkan_descriptor(rogue_builder *b,
                                           nir_intrinsic_instr *intr)
{
   rogue_instr *instr;
   unsigned desc_set = nir_src_comp_as_uint(intr->src[0], 0);
   unsigned binding = nir_src_comp_as_uint(intr->src[0], 1);
   ASSERTED VkDescriptorType desc_type = nir_src_comp_as_uint(intr->src[0], 2);
   assert(desc_type == nir_intrinsic_desc_type(intr));

   struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;

   unsigned desc_set_table_sh_reg;
   unsigned desc_set_offset;
   unsigned desc_offset;

   if (pipeline_layout) {
      /* Fetch shared registers containing descriptor set table address. */
      enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
      assert(pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
                .descriptor_set_addrs_table.present);
      desc_set_table_sh_reg =
         pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
            .descriptor_set_addrs_table.offset;

      /* Calculate offset for the descriptor set. */
      assert(desc_set < pipeline_layout->set_count);
      desc_set_offset = desc_set * sizeof(pvr_dev_addr_t); /* Table is an array
                                                              of addresses. */

      const struct pvr_descriptor_set_layout *set_layout =
         pipeline_layout->set_layout[desc_set];
      const struct pvr_descriptor_set_layout_mem_layout *mem_layout =
         &set_layout->memory_layout_in_dwords_per_stage[pvr_stage];

      /* Calculate offset for the descriptor/binding. */
      assert(binding < set_layout->binding_count);

      const struct pvr_descriptor_set_layout_binding *binding_layout =
         pvr_get_descriptor_binding(set_layout, binding);
      assert(binding_layout);

      /* TODO: Handle secondaries. */
      /* TODO: Handle bindings having multiple descriptors
       * (VkDescriptorSetLayoutBinding->descriptorCount).
       */

      if (descriptor_is_dynamic(binding_layout->type))
         desc_offset = set_layout->total_size_in_dwords;
      else
         desc_offset = mem_layout->primary_offset;

      desc_offset +=
         binding_layout->per_stage_offset_in_dwords[pvr_stage].primary;

      desc_offset *= sizeof(uint32_t); /* DWORDs to bytes. */
   } else {
      /* Dummy defaults for offline compiler. */
      /* TODO: Load these from an offline pipeline description
       * if using the offline compiler.
       */
      desc_set_table_sh_reg = 0;
      desc_set_offset = desc_set * sizeof(pvr_dev_addr_t);
      desc_offset = binding * sizeof(pvr_dev_addr_t);
   }

   rogue_ref64 desc_set_table_base_sh =
      rogue_shared_ref64(b->shader, desc_set_table_sh_reg);

   unsigned desc_set_table_base_addr_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_set_table_base_addr =
      rogue_ssa_ref64(b->shader, desc_set_table_base_addr_idx);

   instr =
      &rogue_MOV(b, desc_set_table_base_addr.lo32, desc_set_table_base_sh.lo32)
          ->instr;
   rogue_add_instr_comment(instr, "desc_set_table_base_addr.lo32");
   instr =
      &rogue_MOV(b, desc_set_table_base_addr.hi32, desc_set_table_base_sh.hi32)
          ->instr;
   rogue_add_instr_comment(instr, "desc_set_table_base_addr.hi32");

   /* Offset the descriptor set table address to the descriptor set entry. */
   unsigned desc_set_table_addr_offset_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_set_table_addr_offset =
      rogue_ssa_ref64(b->shader, desc_set_table_addr_offset_idx);

   rogue_MOV(b,
             desc_set_table_addr_offset.lo32,
             rogue_ref_imm(desc_set_offset));
   rogue_MOV(b, desc_set_table_addr_offset.hi32, rogue_ref_imm(0));

   unsigned desc_set_table_addr_entry_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_set_table_addr_entry =
      rogue_ssa_ref64(b->shader, desc_set_table_addr_entry_idx);

   rogue_ADD64(b,
               desc_set_table_addr_entry.lo32,
               desc_set_table_addr_entry.hi32,
               rogue_none(),
               desc_set_table_base_addr.lo32,
               desc_set_table_base_addr.hi32,
               desc_set_table_addr_offset.lo32,
               desc_set_table_addr_offset.hi32,
               rogue_none());

   /* Load the descriptor set address from the table. */
   unsigned desc_set_addr_base_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_set_addr_base =
      rogue_ssa_ref64(b->shader, desc_set_addr_base_idx);

   instr = &rogue_LD(b,
                     desc_set_addr_base.ref64,
                     rogue_ref_drc(0),
                     rogue_ref_val(2),
                     desc_set_table_addr_entry.ref64)
               ->instr;
   rogue_add_instr_comment(instr, "load descriptor set");

   /* Offset the descriptor set address to the descriptor entry. */
   unsigned desc_addr_offset_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_addr_offset =
      rogue_ssa_ref64(b->shader, desc_addr_offset_idx);

   rogue_MOV(b, desc_addr_offset.lo32, rogue_ref_imm(desc_offset));
   rogue_MOV(b, desc_addr_offset.hi32, rogue_ref_imm(0));

   unsigned desc_addr_entry_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 desc_addr_entry =
      rogue_ssa_ref64(b->shader, desc_addr_entry_idx);

   rogue_ADD64(b,
               desc_addr_entry.lo32,
               desc_addr_entry.hi32,
               rogue_none(),
               desc_set_addr_base.lo32,
               desc_set_addr_base.hi32,
               desc_addr_offset.lo32,
               desc_addr_offset.hi32,
               rogue_none());

   /* Load the descriptor address from the set. */
   unsigned desc_addr_idx = intr->dest.ssa.index;
   rogue_ref64 desc_addr = rogue_ssa_ref64(b->shader, desc_addr_idx);

   instr = &rogue_LD(b,
                     desc_addr.ref64,
                     rogue_ref_drc(0),
                     rogue_ref_val(2),
                     desc_addr_entry.ref64)
               ->instr;
   rogue_add_instr_comment(instr, "load descriptor");
}

static void trans_nir_intrinsic_load_global_constant(rogue_builder *b,
                                                     nir_intrinsic_instr *intr)
{
   /* 64-bit source address. */
   unsigned src_index = intr->src[0].ssa->index;
   rogue_ref64 src_addr = rogue_ssa_ref64(b->shader, src_index);

   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16); /* TODO: support even larger load sizes. */

   rogue_instr *instr = &rogue_LD(b,
                                  dst,
                                  rogue_ref_drc(0),
                                  rogue_ref_val(load_size),
                                  src_addr.ref64)
                            ->instr;
   rogue_add_instr_comment(instr, "load_global_constant");
}

static void trans_nir_intrinsic_load_global(rogue_builder *b,
                                            nir_intrinsic_instr *intr)
{
   /* 64-bit source address. */
   unsigned src_index = intr->src[0].ssa->index;
   rogue_ref64 src_addr = rogue_ssa_ref64(b->shader, src_index);

   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16); /* TODO: support even larger load sizes. */

   rogue_instr *instr = &rogue_LD(b,
                                  dst,
                                  rogue_ref_drc(0),
                                  rogue_ref_val(load_size),
                                  src_addr.ref64)
                            ->instr;
   rogue_add_instr_comment(instr, "load_global");
}

static void trans_nir_intrinsic_store_global(rogue_builder *b,
                                             nir_intrinsic_instr *intr)
{
   unsigned store_size = nir_src_num_components(intr->src[0]);
   assert(store_size == 1); /* TODO: Burst store support. */
   assert(intr->src[0].is_ssa);
   assert(intr->src[0].ssa->bit_size == 32);

   rogue_reg *src = rogue_ssa_reg(b->shader, intr->src[0].ssa->index);
   rogue_ref64 dst_addr = rogue_ssa_ref64(b->shader, intr->src[1].ssa->index);

   rogue_instr *instr = &rogue_ST(b,
                                  rogue_ref_reg(src),
                                  rogue_ref_val(2),
                                  rogue_ref_drc(0),
                                  rogue_ref_val(store_size),
                                  dst_addr.ref64,
                                  rogue_none())
                            ->instr;
   /* TODO: cache flags */
   rogue_add_instr_comment(instr, "store_global");
}

/* TODO: Process this into loads in NIR instead. */
static void trans_nir_intrinsic_load_push_constant(rogue_builder *b,
                                                   nir_intrinsic_instr *intr)
{
   rogue_instr *instr;
   unsigned offset = nir_src_as_uint(intr->src[0]);
   struct pvr_pipeline_layout *pipeline_layout =
      b->shader->ctx->pipeline_layout;

   unsigned push_consts_sh_reg;

   if (pipeline_layout) {
      /* Fetch shared registers containing push constants address. */
      enum pvr_stage_allocation pvr_stage = mesa_stage_to_pvr(b->shader->stage);
      assert(pipeline_layout->sh_reg_layout_per_stage[pvr_stage]
                .push_consts.present);
      push_consts_sh_reg =
         pipeline_layout->sh_reg_layout_per_stage[pvr_stage].push_consts.offset;
   } else {
      /* Dummy defaults for offline compiler. */
      /* TODO: Load these from an offline pipeline description
       * if using the offline compiler.
       */
      push_consts_sh_reg = 0;
   }

   rogue_ref64 push_consts_base_sh =
      rogue_shared_ref64(b->shader, push_consts_sh_reg);

   unsigned push_consts_base_addr_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 push_consts_base_addr =
      rogue_ssa_ref64(b->shader, push_consts_base_addr_idx);

   instr =
      &rogue_MOV(b, push_consts_base_addr.lo32, push_consts_base_sh.lo32)->instr;
   rogue_add_instr_comment(instr, "push_consts_base_addr.lo32");
   instr =
      &rogue_MOV(b, push_consts_base_addr.hi32, push_consts_base_sh.hi32)->instr;
   rogue_add_instr_comment(instr, "push_consts_base_addr.hi32");

   /* Offset the push constants base address to the desired entry. */
   unsigned push_consts_addr_offset_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 push_consts_addr_offset =
      rogue_ssa_ref64(b->shader, push_consts_addr_offset_idx);

   rogue_MOV(b, push_consts_addr_offset.lo32, rogue_ref_imm(offset));
   rogue_MOV(b, push_consts_addr_offset.hi32, rogue_ref_imm(0));

   unsigned push_consts_addr_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref64 push_const_addr =
      rogue_ssa_ref64(b->shader, push_consts_addr_idx);

   rogue_ADD64(b,
               push_const_addr.lo32,
               push_const_addr.hi32,
               rogue_none(),
               push_consts_base_addr.lo32,
               push_consts_base_addr.hi32,
               push_consts_addr_offset.lo32,
               push_consts_addr_offset.hi32,
               rogue_none());

   /* Load the push constant. */
   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size <= 16); /* TODO: support even larger load sizes. */

   instr = &rogue_LD(b,
                     dst,
                     rogue_ref_drc(0),
                     rogue_ref_val(load_size),
                     push_const_addr.ref64)
               ->instr;
   rogue_add_instr_commentf(instr, "load push_constant (offset 0x%x)", offset);
}

static void
trans_nir_intrinsic_load_local_invocation_id_img(rogue_builder *b,
                                                 nir_intrinsic_instr *intr,
                                                 bool yz)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size == 1);

   assert(cs_data->local_id_regs[yz] != ROGUE_REG_UNUSED);
   rogue_reg *src = rogue_vtxin_reg(b->shader, cs_data->local_id_regs[yz]);
   rogue_instr *instr = &rogue_MOV(b, dst, rogue_ref_reg(src))->instr;

   rogue_add_instr_commentf(instr,
                            "load_local_invocation_id.%s",
                            yz ? "yz" : "x");
}

static void trans_nir_intrinsic_load_workgroup_id_img(rogue_builder *b,
                                                      nir_intrinsic_instr *intr,
                                                      unsigned component)
{
   const struct rogue_cs_build_data *cs_data = &b->shader->ctx->stage_data.cs;

   unsigned load_size;
   rogue_ref dst = nir_ssa_reg_intr_dst32(b->shader, intr, &load_size);
   assert(load_size == 1);

   assert(cs_data->workgroup_regs[component] != ROGUE_REG_UNUSED);
   rogue_reg *src =
      rogue_coeff_reg(b->shader, cs_data->workgroup_regs[component]);

   rogue_instr *instr = &rogue_MOV(b, dst, rogue_ref_reg(src))->instr;
   rogue_add_instr_commentf(instr, "load_workgroup_id.%c", 'x' + component);
}

static void trans_nir_intrinsic(rogue_builder *b, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return trans_nir_intrinsic_load_input(b, intr);

   case nir_intrinsic_store_output:
      return trans_nir_intrinsic_store_output(b, intr);

   case nir_intrinsic_load_vulkan_descriptor:
      return trans_nir_intrinsic_load_vulkan_descriptor(b, intr);

   case nir_intrinsic_load_global_constant:
      return trans_nir_intrinsic_load_global_constant(b, intr);

   case nir_intrinsic_load_global:
      return trans_nir_intrinsic_load_global(b, intr);

   case nir_intrinsic_store_global:
      return trans_nir_intrinsic_store_global(b, intr);

   case nir_intrinsic_load_push_constant:
      return trans_nir_intrinsic_load_push_constant(b, intr);

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

   default:
      break;
   }

   unreachable("Unimplemented NIR intrinsic instruction.");
}

static void trans_nir_alu_pack_unorm_4x8(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_alu_instr *pck_u8888 = rogue_PCK_U8888(b, dst, src);
   rogue_set_instr_repeat(&pck_u8888->instr, 4);
   rogue_set_alu_op_mod(pck_u8888, ROGUE_ALU_OP_MOD_SCALE);
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
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src32(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);

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
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src32(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);

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
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src32(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);
   rogue_ref src2 = nir_ssa_reg_alu_src32(b->shader, alu, 2);

   rogue_alu_instr *ffma = rogue_FMAD(b, dst, src0, src1, src2);
   rogue_apply_alu_src_mods(ffma, alu, false);
}

static void trans_nir_alu_frcp(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_alu_instr *frcp = rogue_FRCP(b, dst, src);
   rogue_apply_alu_src_mods(frcp, alu, false);
}

static void trans_nir_alu_frsq(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_alu_instr *frsq = rogue_FRSQ(b, dst, src);
   rogue_apply_alu_src_mods(frsq, alu, false);
}

static void trans_nir_alu_flog2(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_alu_instr *flog2 = rogue_FLOG2(b, dst, src);
   rogue_apply_alu_src_mods(flog2, alu, false);
}

static void trans_nir_alu_fexp2(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_alu_instr *fexp2 = rogue_FEXP2(b, dst, src);
   rogue_apply_alu_src_mods(fexp2, alu, false);
}

/* Conditionally sets the output to src0 or src1 depending on whether the
 * comparison between them is true or false.
 */
static void trans_nir_alu_cmp_sel(rogue_builder *b,
                                  nir_alu_instr *alu,
                                  enum rogue_alu_op_mod comp,
                                  enum rogue_alu_op_mod type)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src32(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);

   rogue_alu_instr *cndsel = rogue_CNDSEL(b, dst, src0, src1);
   rogue_set_alu_op_mod(cndsel, comp);
   rogue_set_alu_op_mod(cndsel, type);
   rogue_apply_alu_src_mods(cndsel, alu, false);
}

/* Conditionally sets the output to src1 or src2 depending on whether the
 * comparison between src0 and 0 is true or false.
 */
static void trans_nir_alu_cmp_zero_sel(rogue_builder *b,
                                       nir_alu_instr *alu,
                                       enum rogue_alu_op_mod comp,
                                       enum rogue_alu_op_mod type,
                                       bool is_bin)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = is_bin ? nir_ssa_reg_alu_src1(b->shader, alu, 0)
                           : nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);
   rogue_ref src2 = nir_ssa_reg_alu_src32(b->shader, alu, 2);

   rogue_alu_instr *zerosel = rogue_ZEROSEL(b, dst, src0, src1, src2);
   rogue_set_alu_op_mod(zerosel, comp);
   rogue_set_alu_op_mod(zerosel, type);
   rogue_apply_alu_src_mods(zerosel, alu, false);
}

static void trans_nir_alu_fneg(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_FNEG(b, dst, src);
}

static void trans_nir_alu_fabs(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_FABS(b, dst, src);
}

static void
trans_nir_alu_fsin_cos(rogue_builder *b, nir_alu_instr *alu, bool cos)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   enum rogue_alu_op_mod mod = cos ? ROGUE_ALU_OP_MOD_COS
                                   : ROGUE_ALU_OP_MOD_SIN;

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

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

   unsigned fmul_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ref fmul = rogue_ref_reg(rogue_ssa_reg(b->shader, fmul_idx));

   rogue_alu = rogue_FMUL(b, fmul, rred_b, sinc);

   rogue_CMOV(b, dst, rogue_ref_io(ROGUE_IO_P0), fmul, sinc);
}

static void trans_nir_alu_vecN(rogue_builder *b, nir_alu_instr *alu, unsigned n)
{
   unsigned dst_index = alu->dest.dest.ssa.index;

   rogue_ssa_vec_regarray(b->shader, n, dst_index, 0);

   rogue_regarray *dst;
   for (unsigned u = 0; u < n; ++u) {
      dst = rogue_ssa_vec_regarray(b->shader, 1, dst_index, u);
      rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, u);
      rogue_MOV(b, rogue_ref_regarray(dst), src);
   }
}

static void trans_nir_alu_iadd64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref src0_lo, src0_hi;
   rogue_ref src1_lo, src1_hi;
   rogue_ref dst_lo, dst_hi;

   nir_ssa_reg_alu_dst64(b->shader, alu, &dst_lo, &dst_hi);
   nir_ssa_reg_alu_src64(b->shader, alu, 0, &src0_lo, &src0_hi);
   nir_ssa_reg_alu_src64(b->shader, alu, 1, &src1_lo, &src1_hi);

   rogue_ADD64(b,
               dst_lo,
               dst_hi,
               rogue_none(),
               src0_lo,
               src0_hi,
               src1_lo,
               src1_hi,
               rogue_none());
}

static void trans_nir_alu_iadd(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = alu->dest.dest.ssa.bit_size;

   switch (bit_size) {
      /* TODO: case 32: */

   case 64:
      return trans_nir_alu_iadd64(b, alu);

   default:
      break;
   }

   unreachable("Unsupported bit size.");
}

static void trans_nir_alu_imul64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref src0_lo, src0_hi;
   rogue_ref src1_lo, src1_hi;
   rogue_ref dst_lo, dst_hi;
   rogue_ref imm_0 = rogue_ref_imm(0);

   nir_ssa_reg_alu_dst64(b->shader, alu, &dst_lo, &dst_hi);
   nir_ssa_reg_alu_src64(b->shader, alu, 0, &src0_lo, &src0_hi);
   nir_ssa_reg_alu_src64(b->shader, alu, 1, &src1_lo, &src1_hi);

   rogue_MADD64(b, dst_lo, dst_hi, src0_lo, src1_lo, imm_0, imm_0, rogue_none());
}

static void trans_nir_alu_imul(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned bit_size = alu->dest.dest.ssa.bit_size;

   switch (bit_size) {
      /* TODO: case 32: */

   case 64:
      return trans_nir_alu_imul64(b, alu);

   default:
      break;
   }

   unreachable("Unsupported bit size.");
}

static void trans_nir_alu_i2i64(rogue_builder *b, nir_alu_instr *alu)
{
   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_ref dst_lo, dst_hi;
   nir_ssa_reg_alu_dst64(b->shader, alu, &dst_lo, &dst_hi);

   rogue_MOV(b, dst_lo, src);
   rogue_MOV(b, dst_hi, rogue_ref_imm(0));
}

/* Conditionally sets the output to 1 or 0 depending on whether the comparison
 * is true or false. */
static void trans_nir_alu_cmp_bin(rogue_builder *b,
                                  nir_alu_instr *alu,
                                  enum rogue_alu_op_mod comp,
                                  enum rogue_alu_op_mod type)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst1(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src32(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src32(b->shader, alu, 1);

   rogue_alu_instr *cndb = rogue_CNDB(b, dst, src0, src1);
   rogue_set_alu_op_mod(cndb, comp);
   rogue_set_alu_op_mod(cndb, type);
   rogue_apply_alu_src_mods(cndb, alu, false);
}

static void trans_nir_alu_b2f32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src1(b->shader, alu, 0);

   rogue_alu_instr *zerosel =
      rogue_ZEROSEL(b, dst, src, rogue_ref_imm_f(1.0f), rogue_ref_imm_f(0.0f));
   rogue_set_alu_op_mod(zerosel, ROGUE_ALU_OP_MOD_NE);
   rogue_set_alu_op_mod(zerosel, ROGUE_ALU_OP_MOD_U32);
}

static void trans_nir_alu_f2i32(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst32(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src = nir_ssa_reg_alu_src32(b->shader, alu, 0);

   rogue_PCK_S32(b, dst, src);
}

/* TODO: needs additional testing/bindumping */
static void trans_nir_alu_iand(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst1(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src1(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src1(b->shader, alu, 1);

   rogue_instr *byp1 = &rogue_BYP1(b, rogue_ref_io(ROGUE_IO_FT2), src0)->instr;
   rogue_set_instr_group_next(byp1, true);
   rogue_AND(b,
             dst,
             rogue_none(),
             rogue_ref_io(ROGUE_IO_FT2),
             rogue_none(),
             src1);
}

static void trans_nir_alu_ior(rogue_builder *b, nir_alu_instr *alu)
{
   unsigned dst_components;
   rogue_ref dst = nir_ssa_reg_alu_dst1(b->shader, alu, &dst_components);
   assert(dst_components == 1);

   rogue_ref src0 = nir_ssa_reg_alu_src1(b->shader, alu, 0);
   rogue_ref src1 = nir_ssa_reg_alu_src1(b->shader, alu, 1);

   rogue_instr *byp1 = &rogue_BYP1(b, rogue_ref_io(ROGUE_IO_FT2), src0)->instr;
   rogue_set_instr_group_next(byp1, true);
   rogue_OR(b,
            dst,
            rogue_none(),
            rogue_ref_io(ROGUE_IO_FT2),
            rogue_none(),
            src1);
}

#define OM(op_mod) ROGUE_ALU_OP_MOD_##op_mod
static void trans_nir_alu(rogue_builder *b, nir_alu_instr *alu)
{
   switch (alu->op) {
   case nir_op_pack_unorm_4x8:
      return trans_nir_alu_pack_unorm_4x8(b, alu);
      return;

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

   case nir_op_fmin:
      return trans_nir_alu_cmp_sel(b, alu, OM(L), OM(F32));

   case nir_op_fmax:
      return trans_nir_alu_cmp_sel(b, alu, OM(G), OM(F32));

   case nir_op_imin:
      return trans_nir_alu_cmp_sel(b, alu, OM(L), OM(S32));

   case nir_op_imax:
      return trans_nir_alu_cmp_sel(b, alu, OM(G), OM(S32));

   case nir_op_umin:
      return trans_nir_alu_cmp_sel(b, alu, OM(L), OM(U32));

   case nir_op_umax:
      return trans_nir_alu_cmp_sel(b, alu, OM(G), OM(U32));

   case nir_op_fneg:
      return trans_nir_alu_fneg(b, alu);

   case nir_op_fabs:
      return trans_nir_alu_fabs(b, alu);

   case nir_op_fsin:
      return trans_nir_alu_fsin_cos(b, alu, false);

   case nir_op_fcos:
      return trans_nir_alu_fsin_cos(b, alu, true);

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

   case nir_op_i2i64:
      return trans_nir_alu_i2i64(b, alu);

   case nir_op_flt:
      return trans_nir_alu_cmp_bin(b, alu, OM(L), OM(F32));

   case nir_op_fge:
      return trans_nir_alu_cmp_bin(b, alu, OM(G), OM(F32));

   case nir_op_feq:
      return trans_nir_alu_cmp_bin(b, alu, OM(E), OM(F32));

   case nir_op_fneu:
      return trans_nir_alu_cmp_bin(b, alu, OM(NE), OM(F32));

   case nir_op_ilt:
      return trans_nir_alu_cmp_bin(b, alu, OM(L), OM(S32));

   case nir_op_ige:
      return trans_nir_alu_cmp_bin(b, alu, OM(G), OM(S32));

   case nir_op_ieq:
      return trans_nir_alu_cmp_bin(b, alu, OM(E), OM(S32));

   case nir_op_ine:
      return trans_nir_alu_cmp_bin(b, alu, OM(NE), OM(S32));

   case nir_op_ult:
      return trans_nir_alu_cmp_bin(b, alu, OM(L), OM(U32));

   case nir_op_uge:
      return trans_nir_alu_cmp_bin(b, alu, OM(G), OM(U32));

   case nir_op_bcsel:
      return trans_nir_alu_cmp_zero_sel(b, alu, OM(NE), OM(U32), true);

   case nir_op_b2f32:
      return trans_nir_alu_b2f32(b, alu);

   case nir_op_f2i32:
      return trans_nir_alu_f2i32(b, alu);

   case nir_op_iand:
      return trans_nir_alu_iand(b, alu);

   case nir_op_ior:
      return trans_nir_alu_ior(b, alu);

   case nir_op_fcsel:
      return trans_nir_alu_cmp_zero_sel(b, alu, OM(NE), OM(F32), false);

   case nir_op_fcsel_gt:
      return trans_nir_alu_cmp_zero_sel(b, alu, OM(G), OM(F32), false);

   case nir_op_fcsel_ge:
      return trans_nir_alu_cmp_zero_sel(b, alu, OM(GE), OM(F32), false);

   default:
      break;
   }

   unreachable("Unimplemented NIR ALU instruction.");
}
#undef OM

PUBLIC
unsigned rogue_count_used_regs(const rogue_shader *shader,
                               enum rogue_reg_class class)
{
   unsigned reg_count;
   if (rogue_reg_infos[class].num) {
      reg_count = __bitset_count(shader->regs_used[class],
                                 BITSET_WORDS(rogue_reg_infos[class].num));
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
   rogue_block *_rogue_block = rogue_push_block(b);

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
         unreachable("Unimplemented NIR instruction type.");
      }
   }

   return _rogue_block;
}

static rogue_block *trans_nir_cf_nodes(rogue_builder *b,
                                       struct exec_list *cf_node_list);

static void trans_nir_if(rogue_builder *b, nir_if *nif)
{
   /* N.B. THIS CURRENTLY ONLY DOES SIMPLE IF/ELSE! */
   /* TODO: more complex support for elifs with mask counter += 2, etc. (see isa
    * doc) */
   /* TODO: loop nesting counter? initially disallow and assert? */

   /* Set P0 if the condition is true (not equal to 0). */
   assert(nif->condition.is_ssa);
   rogue_reg *if_cnd = rogue_ssa_reg(b->shader, nif->condition.ssa->index);

   rogue_SETPRED(b, rogue_ref_io(ROGUE_IO_P0), rogue_ref_reg(if_cnd));

   /* TODO LATER: branch allinst if no active instances to start, probably needs
    * blocks to be cached */

   /* Conditional mask count register. */
   rogue_reg *cnd_mask_count[3] = {
      rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++),
      rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++),
      rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++),
   };

   rogue_ctrl_instr *ctrl = rogue_CNDST(b,
                                        rogue_ref_io(ROGUE_IO_PE),
                                        rogue_ref_reg(cnd_mask_count[0]),
                                        rogue_ref_imm(0),
                                        rogue_ref_val(1));
   rogue_set_ctrl_op_mod(ctrl, ROGUE_CTRL_OP_MOD_P0_TRUE);
   rogue_set_instr_exec_cond(&ctrl->instr, ROGUE_EXEC_COND_PE_ANY);

   /* new block: trans_nir_cf_nodes for new block */
   rogue_block *block = trans_nir_cf_nodes(b, &nif->then_list);

   /* cndef */
   ctrl = rogue_CNDEF(b,
                      rogue_ref_io(ROGUE_IO_PE),
                      rogue_ref_reg(cnd_mask_count[1]),
                      rogue_ref_reg(cnd_mask_count[0]),
                      rogue_ref_val(1));
   rogue_set_ctrl_op_mod(ctrl, ROGUE_CTRL_OP_MOD_ALWAYS);
   rogue_set_instr_exec_cond(&ctrl->instr, ROGUE_EXEC_COND_PE_ANY);

   /* new block: trans_nir_cf_nodes for new block */
   block = trans_nir_cf_nodes(b, &nif->else_list);

   /* cndend */

   /* If the else statement was empty, don't bother creating a new block. */
   if (!list_is_empty(&block->instrs))
      rogue_push_block(b);

   ctrl = rogue_CNDEND(b,
                       rogue_ref_io(ROGUE_IO_PE),
                       rogue_ref_reg(cnd_mask_count[2]),
                       rogue_ref_reg(cnd_mask_count[1]),
                       rogue_ref_val(1));
   rogue_set_instr_exec_cond(&ctrl->instr, ROGUE_EXEC_COND_PE_ANY);
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

#if 0
      case nir_cf_node_loop:
         trans_nir_loop(ctx, nir_cf_node_as_loop(node));
         break;
#endif

      default:
         unreachable("Unsupported control flow node type.");
      }
   }

   return start_block;
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

   /* Apply passes. */
   rogue_shader_passes(shader);

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
   ROGUE_PASS_V(shader, rogue_constreg);
   ROGUE_PASS_V(shader, rogue_copy_prop);
   ROGUE_PASS_V(shader, rogue_dce);
   ROGUE_PASS_V(shader, rogue_lower_pseudo_ops);
   ROGUE_PASS_V(shader, rogue_schedule_wdf, false);
   ROGUE_PASS_V(shader, rogue_schedule_uvsw, false);
   ROGUE_PASS_V(shader, rogue_trim);
   ROGUE_PASS_V(shader, rogue_regalloc);
   ROGUE_PASS_V(shader, rogue_lower_late_ops);
   ROGUE_PASS_V(shader, rogue_dce);
   ROGUE_PASS_V(shader, rogue_schedule_instr_groups, false);

   if (ROGUE_DEBUG(IR))
      rogue_print_pass_debug(shader, "after passes", stdout);
}
