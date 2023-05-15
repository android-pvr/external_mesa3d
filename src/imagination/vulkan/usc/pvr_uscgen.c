/*
 * Copyright © 2023 Imagination Technologies Ltd.
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

#include "pvr_job_transfer.h"
#include "pvr_private.h"
#include "pvr_uscgen.h"
#include "rogue/rogue.h"
#include "rogue/rogue_builder.h"
#include "util/bitscan.h"
#include "util/u_dynarray.h"
#include "vulkan/util/vk_format.h"

#include <stdbool.h>

/* Expects emit_count ROGUE_NUM_PBESTATE_STATE_WORDS entries */
void pvr_uscgen_eot(const char *name,
                    uint32_t emit_count,
                    const uint32_t *emit_state,
                    unsigned *temps_used,
                    struct util_dynarray *binary)
{
   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_reg *state_word_0 = rogue_temp_reg(shader, 0);
   rogue_reg *state_word_1 = rogue_temp_reg(shader, 1);
   rogue_backend_instr *emitpix = NULL;

   rogue_set_shader_name(shader, name);
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   for (unsigned u = 0; u < emit_count; u++) {
      if (u > 0)
         rogue_WOP(&b);

      rogue_MOV(&b, rogue_ref_reg(state_word_0), rogue_ref_imm(emit_state[0]));
      rogue_MOV(&b, rogue_ref_reg(state_word_1), rogue_ref_imm(emit_state[1]));

      emitpix = rogue_EMITPIX(&b,
                              rogue_ref_reg(state_word_0),
                              rogue_ref_reg(state_word_1));

      emit_state += 2;
   }

   assert(emitpix);

   rogue_set_backend_op_mod(emitpix, ROGUE_BACKEND_OP_MOD_FREEP);
   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_passthrough_vtx(struct util_dynarray *binary, bool rta)
{
   rogue_builder b;
   rogue_reg *dst;
   rogue_reg *src;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_VERTEX);
   rogue_set_shader_name(shader,
                         rta ? "passthrough vertex (RTA)"
                             : "passthrough vertex");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   for (unsigned u = 0; u < 3; ++u) {
      dst = rogue_vtxout_reg(b.shader, u);
      src = rogue_vtxin_reg(b.shader, u);
      rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src));
   }

   if (rta) {
      dst = rogue_vtxout_reg(b.shader, 4);
      src = rogue_vtxin_reg(b.shader, 3);
      rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src));
   }

   dst = rogue_vtxout_reg(b.shader, 3);
   rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_imm_f(1.0f));

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}

struct pvr_uscgen_load_op_context {
   const struct pvr_load_op *load_op;
   struct pvr_uscgen_properties *load_op_properties;
   uint32_t next_sh_reg;
   uint32_t next_temp_reg;
};

static rogue_ref pvr_uscgen_rogue_pack(rogue_builder *b,
                                       struct pvr_uscgen_load_op_context *ctx,
                                       rogue_ref src,
                                       VkFormat fmt)
{
   enum pvr_pbe_accum_format pbe_accum_format;
   uint32_t nr_components;
   rogue_alu_instr *pck;
   rogue_ref dst;

   pbe_accum_format = pvr_get_pbe_accum_format(fmt);
   nr_components = vk_format_get_nr_components(fmt);

   switch (pbe_accum_format) {
   case PVR_PBE_ACCUM_FORMAT_U8:
   case PVR_PBE_ACCUM_FORMAT_UINT8:
      dst = rogue_ref_reg(rogue_temp_reg(b->shader, ctx->next_temp_reg++));
      pck = rogue_PCK_U8888(b, dst, src);
      if (pbe_accum_format == PVR_PBE_ACCUM_FORMAT_U8)
         rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_SCALE);
      rogue_set_instr_repeat(&pck->instr, nr_components);
      return dst;

   case PVR_PBE_ACCUM_FORMAT_S8:
   case PVR_PBE_ACCUM_FORMAT_SINT8:
      dst = rogue_ref_reg(rogue_temp_reg(b->shader, ctx->next_temp_reg++));
      pck = rogue_PCK_S8888(b, dst, src);
      if (pbe_accum_format == PVR_PBE_ACCUM_FORMAT_S8)
         rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_SCALE);
      rogue_set_instr_repeat(&pck->instr, nr_components);
      return dst;

   case PVR_PBE_ACCUM_FORMAT_U16:
   case PVR_PBE_ACCUM_FORMAT_UINT16:
   case PVR_PBE_ACCUM_FORMAT_S16:
   case PVR_PBE_ACCUM_FORMAT_SINT16:
   case PVR_PBE_ACCUM_FORMAT_F16:
      for (unsigned i = 0; i < nr_components; i += 2) {
         unsigned size = MIN2(2, nr_components - i);
         ASSERTED enum rogue_reg_class reg_class;
         unsigned src_index = ~0;
         rogue_ref src_i;
         rogue_ref dst_i;

         rogue_ref_reg_regarray_info(&src, &reg_class, &src_index, NULL);
         assert(src_index != ~0);
         assert(reg_class == ROGUE_REG_CLASS_TEMP);

         src_i = rogue_ref_regarray(
            rogue_temp_regarray(b->shader, size, src_index + i));
         dst_i = rogue_ref_reg(
            rogue_temp_reg(b->shader, ctx->next_temp_reg + i / 2));

         switch (pbe_accum_format) {
         default:
            unreachable("logically impossible");
         case PVR_PBE_ACCUM_FORMAT_U16:
         case PVR_PBE_ACCUM_FORMAT_UINT16:
            pck = rogue_PCK_U1616(b, dst_i, src_i);
            break;
         case PVR_PBE_ACCUM_FORMAT_S16:
         case PVR_PBE_ACCUM_FORMAT_SINT16:
            pck = rogue_PCK_S1616(b, dst_i, src_i);
            break;
         case PVR_PBE_ACCUM_FORMAT_F16:
            pck = rogue_PCK_F16F16(b, dst_i, src_i);
            break;
         }

         if (pbe_accum_format == PVR_PBE_ACCUM_FORMAT_U16 ||
             pbe_accum_format == PVR_PBE_ACCUM_FORMAT_S16)
            rogue_set_alu_op_mod(pck, ROGUE_ALU_OP_MOD_SCALE);

         rogue_set_instr_repeat(&pck->instr, size);
      }
      dst =
         rogue_ref_regarray(rogue_temp_regarray(b->shader,
                                                DIV_ROUND_UP(nr_components, 2),
                                                ctx->next_temp_reg));
      ctx->next_temp_reg += DIV_ROUND_UP(nr_components, 2);
      return dst;

   case PVR_PBE_ACCUM_FORMAT_F32:
   case PVR_PBE_ACCUM_FORMAT_UINT32:
   case PVR_PBE_ACCUM_FORMAT_SINT32:
      /* Packing is a no-op here */
      return src;

   case PVR_PBE_ACCUM_FORMAT_U1010102:
      if (fmt == VK_FORMAT_A2B10G10R10_UINT_PACK32) {
         /* Need to swap R and B components */
         ASSERTED enum rogue_reg_class reg_class;
         unsigned src_index = ~0;
         rogue_reg *swap_dst = rogue_temp_reg(b->shader, ctx->next_temp_reg);

         rogue_ref_reg_regarray_info(&src, &reg_class, &src_index, NULL);
         assert(src_index != ~0);
         assert(reg_class == ROGUE_REG_CLASS_TEMP);

         rogue_MOV(b,
                   rogue_ref_reg(swap_dst),
                   rogue_ref_reg(rogue_temp_reg(b->shader, src_index + 2)));
         rogue_MOV(b,
                   rogue_ref_reg(rogue_temp_reg(b->shader, src_index + 2)),
                   rogue_ref_reg(rogue_temp_reg(b->shader, src_index)));
         rogue_MOV(b,
                   rogue_ref_reg(rogue_temp_reg(b->shader, src_index)),
                   rogue_ref_reg(swap_dst));
      }
      dst = rogue_ref_reg(rogue_temp_reg(b->shader, ctx->next_temp_reg++));
      pck = rogue_PCK_2F10F10F10(b, dst, src);
      rogue_set_instr_repeat(&pck->instr, 4);
      return dst;

   default:
      unreachable("Unknown pbe accum format. Implementation error");
   }

   unreachable("Cannot reach this");
}

static void pvr_uscgen_load_op_clears(rogue_builder *b,
                                      struct pvr_uscgen_load_op_context *ctx)
{
   const struct usc_mrt_setup *mrt_setup =
      ctx->load_op->clears_loads_state.mrt_setup;

   u_foreach_bit (attachment_idx,
                  ctx->load_op->clears_loads_state.rt_clear_mask) {
      VkFormat fmt =
         ctx->load_op->clears_loads_state.dest_vk_format[attachment_idx];
      uint32_t accum_size_dwords =
         DIV_ROUND_UP(pvr_get_pbe_accum_format_size_in_bytes(fmt),
                      sizeof(uint32_t));

      struct usc_mrt_resource *mrt_resource =
         &mrt_setup->mrt_resources[attachment_idx];

      char comment[50];
      snprintf(comment, sizeof(comment), "clear_attachment_%d", attachment_idx);

      /* TODO: Handle spilling to tile buffers. */
      assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);

      /* pvr_hw_pass always allocates the output regs with 4-byte alignment */
      assert(mrt_resource->reg.offset == 0);

      assert(accum_size_dwords ==
             DIV_ROUND_UP(mrt_resource->intermediate_size, sizeof(uint32_t)));

      for (int i = 0; i < accum_size_dwords; i++) {
         rogue_reg *dst =
            rogue_pixout_reg(b->shader, mrt_resource->reg.output_reg + i);
         rogue_reg *src = rogue_shared_reg(b->shader, ctx->next_sh_reg++);
         rogue_instr *instr =
            &rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_reg(src))->instr;
         rogue_add_instr_comment(instr, comment);
      }
   }

   if (ctx->load_op->clears_loads_state.depth_clear_to_reg !=
       PVR_NO_DEPTH_CLEAR_TO_REG) {
      rogue_reg *dst;
      rogue_reg *src;
      rogue_instr *instr;
      int32_t depth_idx = ctx->load_op->clears_loads_state.depth_clear_to_reg;
      struct usc_mrt_resource *mrt_resource =
         &mrt_setup->mrt_resources[depth_idx];

      /* Replicated depth is a single F32 value */
      assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);
      assert(mrt_resource->intermediate_size == sizeof(uint32_t));
      assert(mrt_resource->reg.offset == 0);

      dst = rogue_pixout_reg(b->shader, mrt_resource->reg.output_reg);
      src = rogue_shared_reg(b->shader, ctx->next_sh_reg++);
      instr = &rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_reg(src))->instr;
      rogue_add_instr_comment(instr, "clear_zreplicate");
   }
}

static void pvr_uscgen_load_op_loads(rogue_builder *b,
                                     struct pvr_uscgen_load_op_context *ctx)
{
   const struct usc_mrt_setup *mrt_setup =
      ctx->load_op->clears_loads_state.mrt_setup;

   bool need_sample_id =
      !!(ctx->load_op->clears_loads_state.unresolved_msaa_mask &
         ctx->load_op->clears_loads_state.rt_load_mask);

   rogue_regarray *smp_coords = NULL;
   rogue_regarray *lod_value = NULL;

   if (need_sample_id)
      ctx->load_op_properties->msaa_mode = ROGUE_MSAA_MODE_FULL;

   lod_value = rogue_temp_regarray(b->shader, 1, ctx->next_temp_reg++);
   rogue_MOV(b, rogue_ref_regarray(lod_value), rogue_ref_imm(0));

   {
      /* TODO: Handle 2D Array and 3D volume surfaces */
      uint32_t smp_coord_size = need_sample_id ? 3 : 2;
      rogue_reg *pos_x = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_X_P);
      rogue_reg *pos_y = rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_Y_P);
      uint32_t next_coord_comp = 0;

      rogue_MOV(b,
                rogue_ref_reg(rogue_temp_reg(b->shader, ctx->next_temp_reg)),
                rogue_ref_reg(pos_x));
      rogue_MOV(
         b,
         rogue_ref_reg(rogue_temp_reg(b->shader, ctx->next_temp_reg + 1)),
         rogue_ref_reg(pos_y));
      next_coord_comp += 2;

      if (need_sample_id) {
         rogue_reg *sample_id =
            rogue_special_reg(b->shader, ROGUE_SPECIAL_REG_SAMP_NUM);
         rogue_MOV(
            b,
            rogue_ref_reg(
               rogue_temp_reg(b->shader, ctx->next_temp_reg + next_coord_comp)),
            rogue_ref_reg(sample_id));
         next_coord_comp++;
      }

      smp_coords =
         rogue_temp_regarray(b->shader, smp_coord_size, ctx->next_temp_reg);

      assert(next_coord_comp == smp_coord_size);
      ctx->next_temp_reg += smp_coord_size;
   }

   u_foreach_bit (attachment_idx,
                  ctx->load_op->clears_loads_state.rt_load_mask) {
      VkFormat fmt =
         ctx->load_op->clears_loads_state.dest_vk_format[attachment_idx];
      struct usc_mrt_resource *mrt_resource =
         &mrt_setup->mrt_resources[attachment_idx];
      uint32_t accum_size_dwords =
         DIV_ROUND_UP(pvr_get_pbe_accum_format_size_in_bytes(fmt),
                      sizeof(uint32_t));

      rogue_backend_instr *smp_instr;

      rogue_regarray *image_state =
         rogue_shared_regarray(b->shader,
                               PVR_IMAGE_DESCRIPTOR_SIZE,
                               ctx->next_sh_reg);
      rogue_regarray *sampler_state =
         rogue_shared_regarray(b->shader,
                               PVR_SAMPLER_DESCRIPTOR_SIZE,
                               ctx->next_sh_reg + PVR_IMAGE_DESCRIPTOR_SIZE);

      uint32_t nr_components = vk_format_get_nr_components(fmt);
      rogue_ref accum;
      rogue_regarray *smp_dst =
         rogue_temp_regarray(b->shader, nr_components, ctx->next_temp_reg);
      ctx->next_temp_reg += nr_components;

      smp_instr = rogue_SMP2D(b,
                              rogue_ref_regarray(smp_dst),
                              rogue_ref_drc(0),
                              rogue_ref_regarray(image_state),
                              rogue_ref_regarray(smp_coords),
                              rogue_ref_regarray(sampler_state),
                              rogue_ref_regarray(lod_value),
                              rogue_ref_val(nr_components));

      rogue_set_backend_op_mod(smp_instr, ROGUE_BACKEND_OP_MOD_REPLACE);
      if (vk_format_is_unorm(fmt) || vk_format_is_snorm(fmt))
         rogue_set_backend_op_mod(smp_instr, ROGUE_BACKEND_OP_MOD_FCNORM);

      /* Pack the sample results into accumulation format */
      accum = pvr_uscgen_rogue_pack(b, ctx, rogue_ref_regarray(smp_dst), fmt);
      if (accum.type != ROGUE_REF_TYPE_REGARRAY || accum.regarray != smp_dst)
         ctx->next_temp_reg -= nr_components;

      /* TODO: Handle spilling to tile buffers. */
      assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);

      /* pvr_hw_pass always allocates the output regs with 4-byte alignment
       */
      assert(mrt_resource->reg.offset == 0);

      assert(accum_size_dwords ==
             DIV_ROUND_UP(mrt_resource->intermediate_size, sizeof(uint32_t)));
      assert(accum_size_dwords == 1 ||
             rogue_ref_get_regarray_size(&accum) == accum_size_dwords);

      for (int i = 0; i < accum_size_dwords; i++) {
         rogue_reg *src;
         rogue_reg *dst =
            rogue_pixout_reg(b->shader, mrt_resource->reg.output_reg + i);

         ASSERTED enum rogue_reg_class reg_class;
         unsigned src_index = ~0;

         rogue_ref_reg_regarray_info(&accum, &reg_class, &src_index, NULL);
         assert(src_index != ~0);
         assert(reg_class == ROGUE_REG_CLASS_TEMP);

         src = rogue_temp_reg(b->shader, src_index + i);

         rogue_MOV(b, rogue_ref_reg(dst), rogue_ref_reg(src));
      }

      ctx->next_temp_reg -= accum_size_dwords;

      ctx->next_sh_reg +=
         PVR_IMAGE_DESCRIPTOR_SIZE + PVR_SAMPLER_DESCRIPTOR_SIZE;
   }
}

void pvr_uscgen_load_op(struct util_dynarray *binary,
                        struct pvr_uscgen_properties *load_op_properties,
                        const struct pvr_load_op *load_op)
{
   struct pvr_uscgen_load_op_context ctx;

   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_FRAGMENT);
   rogue_set_shader_name(shader, "load_op");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   ctx.load_op = load_op;
   ctx.next_sh_reg = 0;
   ctx.next_temp_reg = 0;

   ctx.load_op_properties = load_op_properties;
   load_op_properties->shareds_dest_offset = ctx.next_sh_reg;
   load_op_properties->msaa_mode = ROGUE_MSAA_MODE_PIXEL;

   if (load_op->clears_loads_state.rt_clear_mask)
      pvr_uscgen_load_op_clears(&b, &ctx);

   if (load_op->clears_loads_state.rt_load_mask)
      pvr_uscgen_load_op_loads(&b, &ctx);

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   load_op_properties->const_shareds_count = ctx.next_sh_reg;
   load_op_properties->temps_count =
      rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_idfwdf(struct util_dynarray *binary,
                       struct pvr_smp_layout *layout,
                       unsigned *temps_used)
{
   rogue_builder b;
   rogue_backend_instr *be;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "idfwdf");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   unsigned smp_coords_idx = 0; /* 64-bit, so 0 and 1. */
   unsigned st_src_idx = 2;
   unsigned smp_dst_idx = 3;

   rogue_regarray *coords_lo =
      rogue_shared_regarray(shader, 1, layout->coords_idx);
   rogue_regarray *coords_hi =
      rogue_shared_regarray(shader, 1, layout->coords_idx + 1);
   rogue_regarray *shared_lod =
      rogue_shared_regarray(shader, 2, layout->lod_idx);
   rogue_regarray *image_state =
      rogue_shared_regarray(shader, 4, layout->img_state_idx);
   rogue_regarray *sampler_state =
      rogue_shared_regarray(shader, 4, layout->smp_state_idx);

   rogue_ref64 smp_coords = rogue_temp_ref64(shader, smp_coords_idx);

   rogue_reg *st_src = rogue_temp_reg(shader, st_src_idx);
   rogue_reg *smp_dst = rogue_temp_reg(shader, smp_dst_idx);

   /* Do a memory store and a texture read to fence any loads/texture writes
    * from previous kernels.
    */

   /* r0 = sh1 */
   rogue_MOV(&b, smp_coords.lo32, rogue_ref_regarray(coords_hi));

   /* r1 = sh0 */
   rogue_MOV(&b, smp_coords.hi32, rogue_ref_regarray(coords_lo));

   /* r2 = c0 */
   rogue_MOV(&b, rogue_ref_reg(st_src), rogue_ref_imm(0));

   /* MEM[r[0..1]] = r2 */
   be = rogue_ST(&b,
                 rogue_ref_reg(st_src),
                 rogue_ref_val(2),
                 rogue_ref_drc(0),
                 rogue_ref_val(1),
                 smp_coords.ref64,
                 rogue_none());
   /* rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_WRITETHROUGH);
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_NOWDF);

   /* idf drc0, r0 */
   /* wdf drc0 */
   rogue_IDF(&b, rogue_ref_drc(0), smp_coords.ref64);

   /* Fence any texture writes from the preceding kernel, and
    * do a block of reads to flush the MADD cache through. */
   for (unsigned y = 0; y < 2; ++y) {
      for (unsigned x = 0; x < 4; ++x) {
         rogue_MOV(&b, smp_coords.lo32, rogue_ref_imm(x));
         rogue_MOV(&b, smp_coords.hi32, rogue_ref_imm(y));
         be = rogue_SMP2D(&b,
                          rogue_ref_reg(smp_dst),
                          rogue_ref_drc(0),
                          rogue_ref_regarray(image_state),
                          smp_coords.ref64,
                          rogue_ref_regarray(sampler_state),
                          rogue_ref_regarray(shared_lod),
                          rogue_ref_val(1));

         /* rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
         rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_FCNORM);
         rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_REPLACE);
         rogue_add_instr_commentf(&be->instr, "Sample %u,%u", x, y);
      }
   }

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_nop(struct util_dynarray *binary)
{
   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "NOP");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}
