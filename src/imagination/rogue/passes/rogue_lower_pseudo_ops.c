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
#include "rogue_builder.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \file rogue_lower_pseudo_ops.c
 *
 * \brief Contains the rogue_lower_pseudo_ops pass.
 */

static inline bool rogue_lower_FABS(rogue_builder *b, rogue_alu_instr *fabs)
{
   rogue_alu_instr *mbyp = rogue_MBYP0(b, fabs->dst[0].ref, fabs->src[0].ref);
   rogue_merge_instr_comment(&mbyp->instr, &fabs->instr, "fabs");
   rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_ABS);
   rogue_instr_delete(&fabs->instr);

   return true;
}

static inline bool rogue_lower_FNEG(rogue_builder *b, rogue_alu_instr *fneg)
{
   rogue_alu_instr *mbyp = rogue_MBYP0(b, fneg->dst[0].ref, fneg->src[0].ref);
   rogue_merge_instr_comment(&mbyp->instr, &fneg->instr, "fneg");
   rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_NEG);
   rogue_instr_delete(&fneg->instr);

   return true;
}

static inline bool rogue_lower_FNABS(rogue_builder *b, rogue_alu_instr *fnabs)
{
   rogue_alu_instr *mbyp = rogue_MBYP0(b, fnabs->dst[0].ref, fnabs->src[0].ref);
   rogue_merge_instr_comment(&mbyp->instr, &fnabs->instr, "fnabs");
   rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_ABS);
   rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_NEG);
   rogue_instr_delete(&fnabs->instr);

   return true;
}

static inline bool rogue_lower_FFLR(rogue_builder *b, rogue_alu_instr *fflr)
{
   rogue_alu_instr *fadd =
      rogue_FADD(b,
                 fflr->dst[0].ref,
                 fflr->src[0].ref,
                 rogue_ref_reg(rogue_const_reg(b->shader, 0)));
   rogue_merge_instr_comment(&fadd->instr, &fflr->instr, "fflr");
   rogue_set_alu_src_mod(fadd, 0, ROGUE_ALU_SRC_MOD_FLR);
   rogue_instr_delete(&fflr->instr);

   return true;
}

/* TODO: Put movc into instruction groups where possible. */

static inline bool rogue_lower_CND(rogue_builder *b,
                                   rogue_alu_instr *cnd,
                                   rogue_ref ref_true,
                                   rogue_ref ref_false)
{
   /* Source 0. */
   rogue_alu_instr *tst_mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), cnd->src[0].ref);
   rogue_set_instr_group_next(&tst_mbyp0->instr, true);
   rogue_merge_instr_comment(&tst_mbyp0->instr, &cnd->instr, "cnd (src0)");

   /* Source 1. */
   rogue_alu_instr *tst_mbyp1 =
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), cnd->src[1].ref);
   rogue_set_instr_group_next(&tst_mbyp1->instr, true);
   rogue_merge_instr_comment(&tst_mbyp1->instr, &cnd->instr, "cnd (src1)");

   /* Test. */
   rogue_alu_instr *tst = rogue_TST(b,
                                    rogue_ref_io(ROGUE_IO_FTT),
                                    rogue_ref_io(ROGUE_IO_P0),
                                    rogue_ref_io(ROGUE_IO_FT0),
                                    rogue_ref_io(ROGUE_IO_FT1));
   rogue_merge_instr_comment(&tst->instr, &cnd->instr, "cnd (test)");

   /* Propagate source modifiers and test condition. */
   tst_mbyp0->src[0].mod = cnd->src[0].mod;
   tst_mbyp1->src[0].mod = cnd->src[1].mod;
   tst->mod = cnd->mod;

   /* Result: 0/1. */
   rogue_alu_instr *cmov = rogue_CMOV(b,
                                      cnd->dst[0].ref,
                                      rogue_ref_io(ROGUE_IO_P0),
                                      ref_true,
                                      ref_false);
   rogue_merge_instr_comment(&cmov->instr, &cnd->instr, "cnd (set result)");

   rogue_instr_delete(&cnd->instr);

   return true;
}

static inline bool rogue_lower_CNDB(rogue_builder *b, rogue_alu_instr *cndb)
{
   return rogue_lower_CND(b,
                          cndb,
                          rogue_ref_reg(rogue_const_reg(b->shader, 1)),
                          rogue_ref_reg(rogue_const_reg(b->shader, 0)));
}

static inline bool rogue_lower_CNDSEL(rogue_builder *b, rogue_alu_instr *cndsel)
{
   return rogue_lower_CND(b, cndsel, cndsel->src[0].ref, cndsel->src[1].ref);
}

static inline bool rogue_lower_ZEROSEL(rogue_builder *b,
                                       rogue_alu_instr *zerosel)
{
   /* Source 0. */
   rogue_alu_instr *tst_mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), zerosel->src[0].ref);
   rogue_set_instr_group_next(&tst_mbyp0->instr, true);
   rogue_merge_instr_comment(&tst_mbyp0->instr,
                             &zerosel->instr,
                             "zerosel (src)");

   rogue_alu_instr *tst_mbyp1 =
      rogue_MBYP1(b,
                  rogue_ref_io(ROGUE_IO_FT1),
                  rogue_ref_reg(rogue_const_reg(b->shader, 0)));
   rogue_set_instr_group_next(&tst_mbyp1->instr, true);
   rogue_merge_instr_comment(&tst_mbyp1->instr, &zerosel->instr, "zerosel (0)");

   /* Test != 0. */
   rogue_alu_instr *tst = rogue_TST(b,
                                    rogue_ref_io(ROGUE_IO_FTT),
                                    rogue_ref_io(ROGUE_IO_P0),
                                    rogue_ref_io(ROGUE_IO_FT0),
                                    rogue_ref_io(ROGUE_IO_FT1));
   rogue_merge_instr_comment(&tst->instr, &zerosel->instr, "zerosel (test)");

   /* Propagate test condition. */
   tst->mod = zerosel->mod;

   /* Result: 0/1. */
   rogue_alu_instr *cmov = rogue_CMOV(b,
                                      zerosel->dst[0].ref,
                                      rogue_ref_io(ROGUE_IO_P0),
                                      zerosel->src[1].ref,
                                      zerosel->src[2].ref);
   rogue_merge_instr_comment(&cmov->instr,
                             &zerosel->instr,
                             "zerosel (set result)");

   rogue_instr_delete(&zerosel->instr);

   return true;
}

/* TODO: Just do a single argument ROGUE_ALU_OP_MOD_GZ without needing an MBYP.
 */
static inline bool rogue_lower_SETPRED(rogue_builder *b,
                                       rogue_alu_instr *setpred)
{
   /* Source 0. */
   rogue_alu_instr *tst_mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), setpred->src[0].ref);
   rogue_set_instr_group_next(&tst_mbyp0->instr, true);
   rogue_merge_instr_comment(&tst_mbyp0->instr,
                             &setpred->instr,
                             "setpred (src)");

   rogue_alu_instr *tst_mbyp1 =
      rogue_MBYP1(b,
                  rogue_ref_io(ROGUE_IO_FT1),
                  rogue_ref_reg(rogue_const_reg(b->shader, 0)));
   rogue_set_instr_group_next(&tst_mbyp1->instr, true);
   rogue_merge_instr_comment(&tst_mbyp1->instr,
                             &setpred->instr,
                             "setpred (src)");

   /* Test != 0. */
   rogue_alu_instr *tst = rogue_TST(b,
                                    rogue_ref_io(ROGUE_IO_FTT),
                                    rogue_ref_io(ROGUE_IO_P0),
                                    rogue_ref_io(ROGUE_IO_FT0),
                                    rogue_ref_io(ROGUE_IO_FT1));
   rogue_merge_instr_comment(&tst->instr,
                             &setpred->instr,
                             "setpred (test/set)");

   /* Set test condition (src != 0). */
   rogue_set_alu_op_mod(tst, ROGUE_ALU_OP_MOD_NE);
   rogue_set_alu_op_mod(tst, ROGUE_ALU_OP_MOD_U32);

   rogue_instr_delete(&setpred->instr);

   return true;
}

static inline bool rogue_lower_MOV(rogue_builder *b, rogue_alu_instr *mov)
{
   rogue_instr *instr;

   /* If we're writing to a vertex output register, we need to use uvsw.write.
    */
   if (rogue_ref_is_vtxout_reg(&mov->dst[0].ref)) {
      rogue_ref src = mov->src[0].ref;
      if (rogue_ref_is_imm(&mov->src[0].ref)) {
         unsigned imm_mov_idx = b->shader->ctx->next_ssa_idx++;
         rogue_reg *imm_mov = rogue_ssa_reg(b->shader, imm_mov_idx);
         src = rogue_ref_reg(imm_mov);

         rogue_BYP0(
            b,
            rogue_ref_io(ROGUE_IO_FT0),
            src,
            rogue_ref_io(ROGUE_IO_S0),
            rogue_ref_val(rogue_ref_get_imm(&mov->src[0].ref)->imm.u32));
      }

      instr = &rogue_UVSW_WRITE(b, mov->dst[0].ref, src)->instr;
   } else if (rogue_ref_is_special_reg(&mov->src[0].ref)) {
      /* If we're loading a special register, use a movc. */
      rogue_alu_instr *alu = rogue_MOVC(b,
                                        mov->dst[0].ref,
                                        rogue_none(),
                                        rogue_none(),
                                        mov->src[0].ref,
                                        rogue_none());
      rogue_set_alu_dst_mod(alu, 0, ROGUE_ALU_DST_MOD_E0);
      rogue_set_alu_dst_mod(alu, 0, ROGUE_ALU_DST_MOD_E1);
      rogue_set_alu_dst_mod(alu, 0, ROGUE_ALU_DST_MOD_E2);
      rogue_set_alu_dst_mod(alu, 0, ROGUE_ALU_DST_MOD_E3);

      instr = &alu->instr;
   } else {
      /* If we're moving an immediate value not in special constants,
       * we need to do a bitwise bypass.
       */
      if (rogue_ref_is_imm(&mov->src[0].ref)) {
         instr = &rogue_BYP0(b,
                             rogue_ref_io(ROGUE_IO_FT0),
                             mov->dst[0].ref,
                             rogue_ref_io(ROGUE_IO_S0),
                             rogue_ref_val(
                                rogue_ref_get_imm(&mov->src[0].ref)->imm.u32))
                     ->instr;
      } else {
         instr = &rogue_MBYP0(b, mov->dst[0].ref, mov->src[0].ref)->instr;
      }
   }

   rogue_merge_instr_comment(instr, &mov->instr, "mov");
   rogue_instr_delete(&mov->instr);

   return true;
}

static inline bool rogue_lower_IADD32(rogue_builder *b, rogue_alu_instr *iadd32)
{
   rogue_alu_instr *add64_32 =
      rogue_ADD64_32(b,
                     iadd32->dst[0].ref,
                     rogue_none(),
                     iadd32->src[0].ref,
                     rogue_ref_reg(rogue_const_reg(b->shader, 0)),
                     iadd32->src[1].ref,
                     rogue_none());

   /* Propagate op/source mods. */
   add64_32->mod = iadd32->mod;
   add64_32->src[0].mod = iadd32->src[0].mod; /* abs/neg(S1 << 32 | S0) */
   add64_32->src[2].mod = iadd32->src[1].mod; /* abs/neg(S2) */

   rogue_merge_instr_comment(&add64_32->instr, &iadd32->instr, "iadd32");
   rogue_instr_delete(&iadd32->instr);

   return true;
}

static inline bool rogue_lower_IADD64(rogue_builder *b, rogue_alu_instr *iadd64)
{
   rogue_ref64 dst = rogue_ssa_ref64_from_alu_dst(b->shader, iadd64, 0);
   rogue_ref64 src0 = rogue_ssa_ref64_from_alu_src(b->shader, iadd64, 0);
   rogue_ref64 src1 = rogue_ssa_ref64_from_alu_src(b->shader, iadd64, 1);

   rogue_alu_instr *add64 = rogue_ADD64(b,
                                        dst.lo32,
                                        dst.hi32,
                                        rogue_none(),
                                        src0.lo32,
                                        src0.hi32,
                                        src1.lo32,
                                        src1.hi32,
                                        rogue_none());

   /* Propagate op/source mods. */
   add64->mod = iadd64->mod;
   add64->src[0].mod = iadd64->src[0].mod; /* abs/neg(S1 << 32 | S0) */
   add64->src[2].mod = iadd64->src[1].mod; /* abs/neg(S3 << 32 | S2) */

   rogue_merge_instr_comment(&add64->instr, &iadd64->instr, "iadd64");
   rogue_instr_delete(&iadd64->instr);

   return true;
}

static inline bool rogue_lower_IMUL32(rogue_builder *b, rogue_alu_instr *imul32)
{
   rogue_alu_instr *madd32 =
      rogue_MADD32(b,
                   imul32->dst[0].ref,
                   rogue_none(),
                   imul32->src[0].ref,
                   imul32->src[1].ref,
                   rogue_ref_reg(rogue_const_reg(b->shader, 0)),
                   rogue_none());

   /* Propagate op/source mods. */
   madd32->mod = imul32->mod;
   madd32->src[0].mod = imul32->src[0].mod; /* abs/neg(S0) */
   madd32->src[1].mod = imul32->src[1].mod; /* abs/neg(S1) */

   rogue_merge_instr_comment(&madd32->instr, &imul32->instr, "imul32");
   rogue_instr_delete(&imul32->instr);

   return true;
}

static inline bool rogue_lower_IMUL64(rogue_builder *b, rogue_alu_instr *imul64)
{
   rogue_ref64 dst = rogue_ssa_ref64_from_alu_dst(b->shader, imul64, 0);

   rogue_alu_instr *madd64 =
      rogue_MADD64(b,
                   dst.lo32,
                   dst.hi32,
                   imul64->src[0].ref,
                   imul64->src[1].ref,
                   rogue_ref_reg(rogue_const_reg(b->shader, 0)),
                   rogue_ref_reg(rogue_const_reg(b->shader, 0)),
                   rogue_none());

   /* Propagate op/source mods. */
   madd64->mod = imul64->mod;
   madd64->src[0].mod = imul64->src[0].mod; /* abs/neg(S0) */
   madd64->src[1].mod = imul64->src[1].mod; /* abs/neg(S1) */

   rogue_merge_instr_comment(&madd64->instr, &imul64->instr, "imul64");
   rogue_instr_delete(&imul64->instr);

   return true;
}

static inline bool rogue_lower_alu_instr(rogue_builder *b, rogue_alu_instr *alu)
{
   switch (alu->op) {
   case ROGUE_ALU_OP_MOV:
      return rogue_lower_MOV(b, alu);

   case ROGUE_ALU_OP_SETPRED:
      return rogue_lower_SETPRED(b, alu);

   case ROGUE_ALU_OP_FABS:
      return rogue_lower_FABS(b, alu);

   case ROGUE_ALU_OP_FNEG:
      return rogue_lower_FNEG(b, alu);

   case ROGUE_ALU_OP_FNABS:
      return rogue_lower_FNABS(b, alu);

   case ROGUE_ALU_OP_FFLR:
      return rogue_lower_FFLR(b, alu);

   case ROGUE_ALU_OP_CNDB:
      return rogue_lower_CNDB(b, alu);

   case ROGUE_ALU_OP_CNDSEL:
      return rogue_lower_CNDSEL(b, alu);

   case ROGUE_ALU_OP_ZEROSEL:
      return rogue_lower_ZEROSEL(b, alu);

   case ROGUE_ALU_OP_IADD32:
      return rogue_lower_IADD32(b, alu);

   case ROGUE_ALU_OP_IADD64:
      return rogue_lower_IADD64(b, alu);

   case ROGUE_ALU_OP_IMUL32:
      return rogue_lower_IMUL32(b, alu);

   case ROGUE_ALU_OP_IMUL64:
      return rogue_lower_IMUL64(b, alu);

   default:
      break;
   }

   return false;
}

/* TODO: This should only really be called after a distribute_src_mods pass (to
 * come later). */
PUBLIC
bool rogue_lower_pseudo_ops(rogue_shader *shader)
{
   if (shader->is_grouped)
      return false;

   bool progress = false;

   rogue_builder b;
   rogue_builder_init(&b, shader);

   rogue_foreach_instr_in_shader_safe (instr, shader) {
      /* Skip real ops. */
      if (rogue_instr_supported_phases(instr))
         continue;

      b.cursor = rogue_cursor_before_instr(instr);
      switch (instr->type) {
      case ROGUE_INSTR_TYPE_ALU:
         progress |= rogue_lower_alu_instr(&b, rogue_instr_as_alu(instr));
         break;
      default:
         continue;
      }
   }

   return progress;
}
