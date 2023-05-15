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

static const char *rogue_neg_abs_str(bool neg, bool abs)
{
   static const char *str[] = {
      [0] = "",
      [1] = "neg",
      [2] = "abs",
      [3] = "nabs",
   };

   return str[neg | (abs << 1)];
}

static inline bool
rogue_lower_FNEGABS(rogue_builder *b, rogue_alu_instr *alu, bool neg, bool abs)
{
   assert(neg || abs);

   rogue_alu_instr *mbyp = rogue_MBYP0(b, alu->dst[0].ref, alu->src[0].ref);
   rogue_merge_instr_commentf(&mbyp->instr,
                              &alu->instr,
                              "f%s",
                              rogue_neg_abs_str(neg, abs));

   if (neg)
      rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_NEG);

   if (abs)
      rogue_set_alu_src_mod(mbyp, 0, ROGUE_ALU_SRC_MOD_ABS);

   rogue_instr_delete(&alu->instr);

   return true;
}

static inline bool rogue_lower_FFLR(rogue_builder *b, rogue_alu_instr *fflr)
{
   rogue_alu_instr *fadd =
      rogue_FADD(b, fflr->dst[0].ref, fflr->src[0].ref, rogue_ref_imm(0));
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
   return rogue_lower_CND(b, cndb, rogue_ref_imm(~0), rogue_ref_imm(0));
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
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), rogue_ref_imm(0));
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
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), rogue_ref_imm(0));
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
         rogue_MOVI(b, src, mov->src[0].ref);
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
         instr = &rogue_MOVI(b, mov->dst[0].ref, mov->src[0].ref)->instr;
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
   rogue_alu_instr *add64_32 = rogue_ADD64_32(b,
                                              iadd32->dst[0].ref,
                                              rogue_none(),
                                              iadd32->src[0].ref,
                                              rogue_ref_imm(0),
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
   rogue_alu_instr *madd32 = rogue_MADD32(b,
                                          imul32->dst[0].ref,
                                          rogue_none(),
                                          imul32->src[0].ref,
                                          imul32->src[1].ref,
                                          rogue_ref_imm(0),
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

   rogue_alu_instr *madd64 = rogue_MADD64(b,
                                          dst.lo32,
                                          dst.hi32,
                                          imul64->src[0].ref,
                                          imul64->src[1].ref,
                                          rogue_ref_imm(0),
                                          rogue_ref_imm(0),
                                          rogue_none());

   /* Propagate op/source mods. */
   madd64->mod = imul64->mod;
   madd64->src[0].mod = imul64->src[0].mod; /* abs/neg(S0) */
   madd64->src[1].mod = imul64->src[1].mod; /* abs/neg(S1) */

   rogue_merge_instr_comment(&madd64->instr, &imul64->instr, "imul64");
   rogue_instr_delete(&imul64->instr);

   return true;
}

static inline bool
rogue_lower_UMUL_HILO(rogue_builder *b, rogue_alu_instr *umul_hilo, bool high)
{
   rogue_ref dst_lo = high ? rogue_none() : umul_hilo->dst[0].ref;
   rogue_ref dst_hi = high ? umul_hilo->dst[0].ref : rogue_none();

   rogue_ref src0 = umul_hilo->src[0].ref;
   rogue_ref src1 = umul_hilo->src[1].ref;

   rogue_alu_instr *madd64 = rogue_MADD64(b,
                                          dst_lo,
                                          dst_hi,
                                          src0,
                                          src1,
                                          rogue_ref_imm(0),
                                          rogue_ref_imm(0),
                                          rogue_none());

   /* Propagate op/source mods. */
   madd64->mod = umul_hilo->mod;
   madd64->src[0].mod = umul_hilo->src[0].mod; /* abs/neg(S0) */
   madd64->src[1].mod = umul_hilo->src[1].mod; /* abs/neg(S1) */

   rogue_merge_instr_commentf(&madd64->instr,
                              &umul_hilo->instr,
                              "umul_%s",
                              high ? "high" : "low");
   rogue_instr_delete(&umul_hilo->instr);

   return true;
}

static inline bool rogue_lower_INEGABS32(rogue_builder *b,
                                         rogue_alu_instr *alu,
                                         bool neg,
                                         bool abs)
{
   assert(neg || abs);

   rogue_alu_instr *add64_32 = rogue_ADD64_32(b,
                                              alu->dst[0].ref,
                                              rogue_none(),
                                              rogue_ref_imm(0),
                                              rogue_ref_imm(0),
                                              alu->src[0].ref,
                                              rogue_none());

   rogue_set_alu_op_mod(add64_32, ROGUE_ALU_OP_MOD_S);

   if (neg)
      rogue_set_alu_src_mod(add64_32, 2, ROGUE_ALU_SRC_MOD_NEG);

   if (abs)
      rogue_set_alu_src_mod(add64_32, 2, ROGUE_ALU_SRC_MOD_ABS);

   rogue_merge_instr_commentf(&add64_32->instr,
                              &alu->instr,
                              "i%s32",
                              rogue_neg_abs_str(neg, abs));
   rogue_instr_delete(&alu->instr);

   return true;
}

static inline bool rogue_lower_INEGABS64(rogue_builder *b,
                                         rogue_alu_instr *alu,
                                         bool neg,
                                         bool abs)
{
   assert(neg || abs);

   rogue_ref64 dst = rogue_ssa_ref64_from_alu_dst(b->shader, alu, 0);
   rogue_ref64 src = rogue_ssa_ref64_from_alu_src(b->shader, alu, 0);

   rogue_alu_instr *add64_32 = rogue_ADD64_32(b,
                                              dst.lo32,
                                              dst.hi32,
                                              src.lo32,
                                              src.hi32,
                                              rogue_ref_imm(0),
                                              rogue_none());

   rogue_set_alu_op_mod(add64_32, ROGUE_ALU_OP_MOD_S);

   if (neg)
      rogue_set_alu_src_mod(add64_32, 2, ROGUE_ALU_SRC_MOD_NEG);

   if (abs)
      rogue_set_alu_src_mod(add64_32, 2, ROGUE_ALU_SRC_MOD_ABS);

   rogue_merge_instr_commentf(&add64_32->instr,
                              &alu->instr,
                              "i%s32",
                              rogue_neg_abs_str(neg, abs));
   rogue_instr_delete(&alu->instr);

   return true;
}

static inline bool rogue_lower_alu_instr(rogue_builder *b, rogue_alu_instr *alu)
{
   /* Skip real ops. */
   if (alu->op < ROGUE_ALU_OP_PSEUDO)
      return false;

   switch (alu->op) {
   case ROGUE_ALU_OP_MOV:
      return rogue_lower_MOV(b, alu);

   case ROGUE_ALU_OP_SETPRED:
      return rogue_lower_SETPRED(b, alu);

   case ROGUE_ALU_OP_FABS:
      return rogue_lower_FNEGABS(b, alu, false, true);

   case ROGUE_ALU_OP_FNEG:
      return rogue_lower_FNEGABS(b, alu, true, false);

   case ROGUE_ALU_OP_FNABS:
      return rogue_lower_FNEGABS(b, alu, true, true);

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

   case ROGUE_ALU_OP_UMUL_HIGH:
      return rogue_lower_UMUL_HILO(b, alu, true);

   case ROGUE_ALU_OP_UMUL_LOW:
      return rogue_lower_UMUL_HILO(b, alu, false);

   case ROGUE_ALU_OP_INEG32:
      return rogue_lower_INEGABS32(b, alu, true, false);

   case ROGUE_ALU_OP_INEG64:
      return rogue_lower_INEGABS64(b, alu, true, false);

   case ROGUE_ALU_OP_IABS32:
      return rogue_lower_INEGABS32(b, alu, false, true);

   case ROGUE_ALU_OP_IABS64:
      return rogue_lower_INEGABS64(b, alu, false, true);

   default:
      break;
   }

   return false;
}

static inline const char *atst_cond_str(unsigned cond)
{
   static const char *cond_str[] = {
      [ACMPMODE_NEVER] = "never",
      [ACMPMODE_LESS] = "less",
      [ACMPMODE_EQUAL] = "equal",
      [ACMPMODE_LESSEQUAL] = "lessequal",
      [ACMPMODE_GREATER] = "greater",
      [ACMPMODE_NOTEQUAL] = "notequal",
      [ACMPMODE_GREATEREQUAL] = "greaterequal",
      [ACMPMODE_ALWAYS] = "always",
   };

   return cond_str[cond];
}

#define OM(op_mod) ROGUE_BACKEND_OP_MOD_##op_mod
static inline unsigned atst_cond_imm(rogue_backend_instr *atst)
{
   if (rogue_backend_op_mod_is_set(atst, OM(NEVER)))
      return ACMPMODE_NEVER;
   else if (rogue_backend_op_mod_is_set(atst, OM(LESS)))
      return ACMPMODE_LESS;
   else if (rogue_backend_op_mod_is_set(atst, OM(EQUAL)))
      return ACMPMODE_EQUAL;
   else if (rogue_backend_op_mod_is_set(atst, OM(LESSEQUAL)))
      return ACMPMODE_LESSEQUAL;
   else if (rogue_backend_op_mod_is_set(atst, OM(GREATER)))
      return ACMPMODE_GREATER;
   else if (rogue_backend_op_mod_is_set(atst, OM(NOTEQUAL)))
      return ACMPMODE_NOTEQUAL;
   else if (rogue_backend_op_mod_is_set(atst, OM(GREATEREQUAL)))
      return ACMPMODE_GREATEREQUAL;
   else if (rogue_backend_op_mod_is_set(atst, OM(ALWAYS)))
      return ACMPMODE_ALWAYS;

   unreachable("Invalid or no condition set.");
   return ~0;
}
#undef OM

static inline bool rogue_lower_ATST_IF(rogue_builder *b,
                                       rogue_backend_instr *backend)
{
   unsigned cond = atst_cond_imm(backend);

   /* if (S1 COND S0) */
   rogue_backend_instr *atst = rogue_ATST(b,
                                          rogue_none(),
                                          rogue_ref_drc(0),
                                          backend->src[1].ref,
                                          backend->src[0].ref,
                                          rogue_ref_imm(cond));

   rogue_merge_instr_commentf(&atst->instr,
                              &backend->instr,
                              "atst_if (%s)",
                              atst_cond_str(cond));
   rogue_instr_delete(&backend->instr);

   return true;
}

static inline bool rogue_lower_backend_instr(rogue_builder *b,
                                             rogue_backend_instr *backend)
{
   /* Skip real ops. */
   if (backend->op < ROGUE_BACKEND_OP_PSEUDO)
      return false;

   switch (backend->op) {
   case ROGUE_BACKEND_OP_ATST_IF:
      return rogue_lower_ATST_IF(b, backend);

   default:
      break;
   }

   return false;
}

static inline bool rogue_lower_ISHL(rogue_builder *b, rogue_bitwise_instr *ishl)
{
   rogue_bitwise_instr *byp0b = rogue_BYP0B(b,
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_ref_io(ROGUE_IO_FT1),
                                            rogue_ref_io(ROGUE_IO_S0),
                                            ishl->src[0].ref);
   rogue_set_instr_group_next(&byp0b->instr, true);
   rogue_bitwise_instr *lsl2 = rogue_LSL2(b,
                                          ishl->dst[0].ref,
                                          rogue_ref_io(ROGUE_IO_FT4),
                                          ishl->src[1].ref);

   rogue_merge_instr_comment(&lsl2->instr, &ishl->instr, "ishl");
   rogue_instr_delete(&ishl->instr);

   return true;
}

static inline bool rogue_lower_ISHR(rogue_builder *b, rogue_bitwise_instr *ishr)
{
   rogue_bitwise_instr *byp0b = rogue_BYP0B(b,
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_ref_io(ROGUE_IO_FT1),
                                            rogue_ref_io(ROGUE_IO_S0),
                                            ishr->src[0].ref);
   rogue_set_instr_group_next(&byp0b->instr, true);
   rogue_bitwise_instr *asr = rogue_ASR(b,
                                        ishr->dst[0].ref,
                                        rogue_ref_io(ROGUE_IO_FT4),
                                        ishr->src[1].ref);
   rogue_set_bitwise_op_mod(asr, ROGUE_BITWISE_OP_MOD_TWB);

   rogue_merge_instr_comment(&asr->instr, &ishr->instr, "ishr");
   rogue_instr_delete(&ishr->instr);

   return true;
}

static inline bool rogue_lower_USHR(rogue_builder *b, rogue_bitwise_instr *ushr)
{
   rogue_bitwise_instr *byp0b = rogue_BYP0B(b,
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_ref_io(ROGUE_IO_FT1),
                                            rogue_ref_io(ROGUE_IO_S0),
                                            ushr->src[0].ref);
   rogue_set_instr_group_next(&byp0b->instr, true);
   rogue_bitwise_instr *shr = rogue_SHR(b,
                                        ushr->dst[0].ref,
                                        rogue_ref_io(ROGUE_IO_FT4),
                                        ushr->src[1].ref);

   rogue_merge_instr_comment(&shr->instr, &ushr->instr, "ushr");
   rogue_instr_delete(&ushr->instr);

   return true;
}

static inline bool rogue_lower_IAND(rogue_builder *b, rogue_bitwise_instr *iand)
{
   rogue_instr *byp0s =
      &rogue_BYP0S(b, rogue_ref_io(ROGUE_IO_FT2), iand->src[0].ref)->instr;
   rogue_set_instr_group_next(byp0s, true);
   rogue_bitwise_instr *and = rogue_AND(b,
                                        iand->dst[0].ref,
                                        rogue_none(),
                                        rogue_ref_io(ROGUE_IO_FT2),
                                        rogue_none(),
                                        iand->src[1].ref);

   rogue_merge_instr_comment(&and->instr, &iand->instr, "iand");
   rogue_instr_delete(&iand->instr);

   return true;
}

static inline bool rogue_lower_IOR(rogue_builder *b, rogue_bitwise_instr *ior)
{
   rogue_instr *byp0s =
      &rogue_BYP0S(b, rogue_ref_io(ROGUE_IO_FT2), ior->src[0].ref)->instr;
   rogue_set_instr_group_next(byp0s, true);
   rogue_bitwise_instr * or = rogue_OR(b,
                                       ior->dst[0].ref,
                                       rogue_none(),
                                       rogue_ref_io(ROGUE_IO_FT2),
                                       rogue_none(),
                                       ior->src[1].ref);

   rogue_merge_instr_comment(& or->instr, &ior->instr, "ior");
   rogue_instr_delete(&ior->instr);

   return true;
}

static inline bool rogue_lower_IXOR(rogue_builder *b, rogue_bitwise_instr *ixor)
{
   rogue_instr *byp0s =
      &rogue_BYP0S(b, rogue_ref_io(ROGUE_IO_FT2), ixor->src[0].ref)->instr;
   rogue_set_instr_group_next(byp0s, true);
   rogue_bitwise_instr * xor = rogue_XOR(b,
                                         ixor->dst[0].ref,
                                         rogue_none(),
                                         rogue_ref_io(ROGUE_IO_FT2),
                                         rogue_none(),
                                         ixor->src[1].ref);

   rogue_merge_instr_comment(&xor->instr, &ixor->instr, "ixor");
   rogue_instr_delete(&ixor->instr);

   return true;
}

static inline bool rogue_lower_bitwise_instr(rogue_builder *b,
                                             rogue_bitwise_instr *bitwise)
{
   /* Skip real ops. */
   if (bitwise->op < ROGUE_BITWISE_OP_PSEUDO)
      return false;

   switch (bitwise->op) {
   case ROGUE_BITWISE_OP_ISHL:
      return rogue_lower_ISHL(b, bitwise);

   case ROGUE_BITWISE_OP_ISHR:
      return rogue_lower_ISHR(b, bitwise);

   case ROGUE_BITWISE_OP_USHR:
      return rogue_lower_USHR(b, bitwise);

   case ROGUE_BITWISE_OP_IAND:
      return rogue_lower_IAND(b, bitwise);

   case ROGUE_BITWISE_OP_IOR:
      return rogue_lower_IOR(b, bitwise);

   case ROGUE_BITWISE_OP_IXOR:
      return rogue_lower_IXOR(b, bitwise);

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
      b.cursor = rogue_cursor_before_instr(instr);
      switch (instr->type) {
      case ROGUE_INSTR_TYPE_ALU:
         progress |= rogue_lower_alu_instr(&b, rogue_instr_as_alu(instr));
         break;

      case ROGUE_INSTR_TYPE_BACKEND:
         progress |=
            rogue_lower_backend_instr(&b, rogue_instr_as_backend(instr));
         break;

      case ROGUE_INSTR_TYPE_BITWISE:
         progress |=
            rogue_lower_bitwise_instr(&b, rogue_instr_as_bitwise(instr));
         break;

      default:
         continue;
      }
   }

   return progress;
}
