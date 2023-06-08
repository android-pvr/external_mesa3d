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

static inline bool
rogue_lower_MINMAX(rogue_builder *b, rogue_alu_instr *alu, bool max)
{
   rogue_alu_instr *mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), alu->src[0].ref);
   rogue_set_instr_group_next(&mbyp0->instr, true);

   rogue_alu_instr *mbyp1 =
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), alu->src[1].ref);
   rogue_set_instr_group_next(&mbyp1->instr, true);

   rogue_alu_instr *tst2 = rogue_TST2(b,
                                      rogue_ref_io(ROGUE_IO_FTT),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_ref_io(ROGUE_IO_FT1));
   rogue_set_alu_op_mod(tst2, max ? ROGUE_ALU_OP_MOD_G : ROGUE_ALU_OP_MOD_L);
   rogue_set_instr_group_next(&tst2->instr, true);

   rogue_MOVC(b,
              alu->dst[0].ref,
              rogue_none(),
              rogue_ref_io(ROGUE_IO_FTT),
              rogue_ref_io(ROGUE_IO_FT0),
              rogue_ref_io(ROGUE_IO_FT1),
              rogue_none(),
              rogue_none());

   /* Propagate source modifiers and type. */
   mbyp0->src[0].mod = alu->src[0].mod;
   mbyp1->src[0].mod = alu->src[1].mod;
   tst2->mod |= alu->mod;

   rogue_instr_delete(&alu->instr);

   return true;
}

static inline bool rogue_lower_CMP(rogue_builder *b, rogue_alu_instr *alu)
{
   rogue_alu_instr *mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), alu->src[0].ref);
   rogue_set_instr_group_next(&mbyp0->instr, true);

   rogue_alu_instr *mbyp1 =
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), alu->src[1].ref);
   rogue_set_instr_group_next(&mbyp1->instr, true);

   rogue_alu_instr *pck_const0 =
      rogue_PCK_CONST0(b, rogue_ref_io(ROGUE_IO_FT2));
   rogue_set_instr_group_next(&pck_const0->instr, true);

   rogue_alu_instr *tst2 = rogue_TST2(b,
                                      rogue_ref_io(ROGUE_IO_FTT),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_ref_io(ROGUE_IO_FT1));
   rogue_set_instr_group_next(&tst2->instr, true);

   rogue_MOVC(b,
              alu->dst[0].ref,
              rogue_none(),
              rogue_ref_io(ROGUE_IO_FTT),
              rogue_ref_imm(~0),
              rogue_ref_io(ROGUE_IO_FT2),
              rogue_none(),
              rogue_none());

   /* Propagate source modifiers, condition and type. */
   mbyp0->src[0].mod = alu->src[0].mod;
   mbyp1->src[0].mod = alu->src[1].mod;
   tst2->mod |= alu->mod;

   rogue_instr_delete(&alu->instr);

   return true;
}

/* TODO NEXT!: Check if registers are being written to that require special
 * behaviour, like vertex out.
 */
/* TODO NEXT!: Make sure that SSA regs aren't being used, late passes must
 * happen after SSA.
 */
static inline bool rogue_lower_CSEL(rogue_builder *b, rogue_alu_instr *alu)
{
   rogue_alu_instr *mbyp0 =
      rogue_MBYP0(b, rogue_ref_io(ROGUE_IO_FT0), alu->src[0].ref);
   rogue_set_instr_group_next(&mbyp0->instr, true);
   rogue_merge_instr_comment(&mbyp0->instr, &alu->instr, "csel (src0)");

   rogue_alu_instr *mbyp1 =
      rogue_MBYP1(b, rogue_ref_io(ROGUE_IO_FT1), alu->src[1].ref);
   rogue_set_instr_group_next(&mbyp1->instr, true);
   rogue_merge_instr_comment(&mbyp1->instr, &alu->instr, "csel (src1)");

   rogue_alu_instr *tst1 = rogue_TST1(b,
                                      rogue_ref_io(ROGUE_IO_FTT),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0));
   rogue_set_instr_group_next(&tst1->instr, true);
   rogue_merge_instr_comment(&tst1->instr, &alu->instr, "csel (test)");

   rogue_alu_instr *movc = rogue_MOVC(b,
                                      alu->dst[0].ref,
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FTT),
                                      rogue_ref_io(ROGUE_IO_FT1),
                                      alu->src[2].ref,
                                      rogue_none(),
                                      rogue_none());
   rogue_merge_instr_comment(&movc->instr, &alu->instr, "csel (set)");

   /* Propagate source modifiers, condition and type. */
   mbyp0->src[0].mod = alu->src[0].mod;
   mbyp1->src[0].mod = alu->src[1].mod;
   tst1->mod |= alu->mod;

   rogue_instr_delete(&alu->instr);

   return true;
}

static inline bool rogue_lower_SETPRED(rogue_builder *b,
                                       rogue_alu_instr *setpred)
{
   assert(rogue_ref_is_io_p0(&setpred->dst[0].ref));
   rogue_bitwise_instr *byp0c =
      rogue_BYP0C(b, rogue_ref_io(ROGUE_IO_FT3), setpred->src[0].ref);
   rogue_set_instr_group_next(&byp0c->instr, true);
   rogue_merge_instr_comment(&byp0c->instr, &setpred->instr, "setpred (src)");

   rogue_bitwise_instr *tst =
      rogue_alu_op_mod_is_set(setpred, ROGUE_ALU_OP_MOD_INVERT)
         ? rogue_TZ(b, rogue_ref_io(ROGUE_IO_P0), rogue_ref_io(ROGUE_IO_FT3))
         : rogue_TNZ(b, rogue_ref_io(ROGUE_IO_P0), rogue_ref_io(ROGUE_IO_FT3));
   rogue_merge_instr_comment(&tst->instr, &setpred->instr, "setpred (test)");

   rogue_instr_delete(&setpred->instr);

   return true;
}

static inline bool rogue_lower_GETPRED(rogue_builder *b,
                                       rogue_alu_instr *getpred)
{
   assert(rogue_ref_is_io_p0(&getpred->src[0].ref));
   rogue_alu_instr *add64_32 = rogue_ADD64_32(b,
                                              getpred->dst[0].ref,
                                              rogue_none(),
                                              rogue_ref_imm(0),
                                              rogue_ref_imm(0),
                                              rogue_ref_imm(0),
                                              rogue_ref_io(ROGUE_IO_P0));

   rogue_merge_instr_comment(&add64_32->instr, &getpred->instr, "getpred");
   rogue_instr_delete(&getpred->instr);

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
                                        rogue_none(),
                                        rogue_none(),
                                        rogue_none());

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

static inline bool rogue_lower_IADD16(rogue_builder *b, rogue_alu_instr *iadd16)
{
   rogue_alu_instr *add16 = rogue_ADD16(b,
                                        rogue_ref_io(ROGUE_IO_FT0),
                                        iadd16->src[0].ref,
                                        iadd16->src[1].ref);

   /* Propagate op/source mods. */
   add16->mod = iadd16->mod;
   add16->src[0].mod = iadd16->src[0].mod;
   add16->src[1].mod = iadd16->src[1].mod;

   /* Operate on lower 16 bits by default. */
   rogue_set_alu_src_mod(add16, 0, ROGUE_ALU_SRC_MOD_E0);
   rogue_set_alu_src_mod(add16, 1, ROGUE_ALU_SRC_MOD_E0);

   rogue_set_instr_group_next(&add16->instr, true);
   rogue_merge_instr_comment(&add16->instr, &iadd16->instr, "iadd16");

   /* Copy over lower 16 bits of result, set upper 16 bits to zero. */
   rogue_alu_instr *movc = rogue_MOVC(b,
                                      iadd16->dst[0].ref,
                                      rogue_none(),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_ref_imm(0),
                                      rogue_none(),
                                      rogue_none());
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);
   rogue_add_instr_comment(&movc->instr, "iadd16 (mask)");

   rogue_instr_delete(&iadd16->instr);

   return true;
}

static inline bool rogue_lower_IMUL16(rogue_builder *b, rogue_alu_instr *imul16)
{
   rogue_alu_instr *mul16 = rogue_MUL16(b,
                                        rogue_ref_io(ROGUE_IO_FT0),
                                        imul16->src[0].ref,
                                        imul16->src[1].ref);

   /* Propagate op/source mods. */
   mul16->mod = imul16->mod;
   mul16->src[0].mod = imul16->src[0].mod;
   mul16->src[1].mod = imul16->src[1].mod;

   /* Operate on lower 16 bits by default. */
   rogue_set_alu_src_mod(mul16, 0, ROGUE_ALU_SRC_MOD_E0);
   rogue_set_alu_src_mod(mul16, 1, ROGUE_ALU_SRC_MOD_E0);

   rogue_set_instr_group_next(&mul16->instr, true);
   rogue_merge_instr_comment(&mul16->instr, &imul16->instr, "imul16");

   /* Copy over lower 16 bits of result, set upper 16 bits to zero. */
   rogue_alu_instr *movc = rogue_MOVC(b,
                                      imul16->dst[0].ref,
                                      rogue_none(),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_ref_imm(0),
                                      rogue_none(),
                                      rogue_none());
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);
   rogue_add_instr_comment(&movc->instr, "imul16 (mask)");

   rogue_instr_delete(&imul16->instr);

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

static inline bool rogue_lower_MUL_HILO(rogue_builder *b,
                                        rogue_alu_instr *mul_hilo,
                                        bool high,
                                        bool is_signed)
{
   rogue_ref dst_lo = high ? rogue_none() : mul_hilo->dst[0].ref;
   rogue_ref dst_hi = high ? mul_hilo->dst[0].ref : rogue_none();

   rogue_ref src0 = mul_hilo->src[0].ref;
   rogue_ref src1 = mul_hilo->src[1].ref;

   rogue_alu_instr *madd64 = rogue_MADD64(b,
                                          dst_lo,
                                          dst_hi,
                                          src0,
                                          src1,
                                          rogue_ref_imm(0),
                                          rogue_ref_imm(0),
                                          rogue_none());

   /* Propagate op/source mods. */
   madd64->mod = mul_hilo->mod;
   madd64->src[0].mod = mul_hilo->src[0].mod; /* abs/neg(S0) */
   madd64->src[1].mod = mul_hilo->src[1].mod; /* abs/neg(S1) */

   if (is_signed)
      rogue_set_alu_op_mod(madd64, ROGUE_ALU_OP_MOD_S);

   rogue_merge_instr_commentf(&madd64->instr,
                              &mul_hilo->instr,
                              "%cmul_%s",
                              is_signed ? 'i' : 'u',
                              high ? "high" : "low");
   rogue_instr_delete(&mul_hilo->instr);

   return true;
}

static inline bool rogue_lower_INEGABS16(rogue_builder *b,
                                         rogue_alu_instr *alu,
                                         bool neg,
                                         bool abs)
{
   assert(neg || abs);

   rogue_alu_instr *add16 = rogue_ADD16(b,
                                        rogue_ref_io(ROGUE_IO_FT0),
                                        alu->src[0].ref,
                                        rogue_ref_imm(0));

   rogue_set_alu_op_mod(add16, ROGUE_ALU_OP_MOD_S);

   /* Operate on lower 16 bits by default. */
   rogue_set_alu_src_mod(add16, 0, ROGUE_ALU_SRC_MOD_E0);
   rogue_set_alu_src_mod(add16, 1, ROGUE_ALU_SRC_MOD_E0);

   if (neg)
      rogue_set_alu_src_mod(add16, 0, ROGUE_ALU_SRC_MOD_NEG);

   if (abs)
      rogue_set_alu_src_mod(add16, 0, ROGUE_ALU_SRC_MOD_ABS);

   rogue_set_instr_group_next(&add16->instr, true);
   rogue_merge_instr_commentf(&add16->instr,
                              &alu->instr,
                              "i%s16",
                              rogue_neg_abs_str(neg, abs));

   /* Copy over lower 16 bits of result, set upper 16 bits to zero. */
   rogue_alu_instr *movc = rogue_MOVC(b,
                                      alu->dst[0].ref,
                                      rogue_none(),
                                      rogue_none(),
                                      rogue_ref_io(ROGUE_IO_FT0),
                                      rogue_ref_imm(0),
                                      rogue_none(),
                                      rogue_none());
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E0);
   rogue_set_alu_dst_mod(movc, 0, ROGUE_ALU_DST_MOD_E1);

   rogue_instr_delete(&alu->instr);

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

   case ROGUE_ALU_OP_GETPRED:
      return rogue_lower_GETPRED(b, alu);

   case ROGUE_ALU_OP_FABS:
      return rogue_lower_FNEGABS(b, alu, false, true);

   case ROGUE_ALU_OP_FNEG:
      return rogue_lower_FNEGABS(b, alu, true, false);

   case ROGUE_ALU_OP_FNABS:
      return rogue_lower_FNEGABS(b, alu, true, true);

   case ROGUE_ALU_OP_FFLR:
      return rogue_lower_FFLR(b, alu);

   case ROGUE_ALU_OP_IADD16:
      return rogue_lower_IADD16(b, alu);

   case ROGUE_ALU_OP_IADD32:
      return rogue_lower_IADD32(b, alu);

   case ROGUE_ALU_OP_IADD64:
      return rogue_lower_IADD64(b, alu);

   case ROGUE_ALU_OP_IMUL16:
      return rogue_lower_IMUL16(b, alu);

   case ROGUE_ALU_OP_IMUL32:
      return rogue_lower_IMUL32(b, alu);

   case ROGUE_ALU_OP_IMUL64:
      return rogue_lower_IMUL64(b, alu);

   case ROGUE_ALU_OP_UMUL_HIGH:
      return rogue_lower_MUL_HILO(b, alu, true, false);

   case ROGUE_ALU_OP_UMUL_LOW:
      return rogue_lower_MUL_HILO(b, alu, false, false);

   case ROGUE_ALU_OP_IMUL_HIGH:
      return rogue_lower_MUL_HILO(b, alu, true, true);

   case ROGUE_ALU_OP_IMUL_LOW:
      return rogue_lower_MUL_HILO(b, alu, false, true);

   case ROGUE_ALU_OP_INEG16:
      return rogue_lower_INEGABS16(b, alu, true, false);

   case ROGUE_ALU_OP_INEG32:
      return rogue_lower_INEGABS32(b, alu, true, false);

   case ROGUE_ALU_OP_INEG64:
      return rogue_lower_INEGABS64(b, alu, true, false);

   case ROGUE_ALU_OP_IABS16:
      return rogue_lower_INEGABS16(b, alu, false, true);

   case ROGUE_ALU_OP_IABS32:
      return rogue_lower_INEGABS32(b, alu, false, true);

   case ROGUE_ALU_OP_IABS64:
      return rogue_lower_INEGABS64(b, alu, false, true);

   case ROGUE_ALU_OP_MIN:
      return rogue_lower_MINMAX(b, alu, false);

   case ROGUE_ALU_OP_MAX:
      return rogue_lower_MINMAX(b, alu, true);

   case ROGUE_ALU_OP_CMP:
      return rogue_lower_CMP(b, alu);

   case ROGUE_ALU_OP_CSEL:
      return rogue_lower_CSEL(b, alu);

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

static inline bool rogue_lower_INOT(rogue_builder *b, rogue_bitwise_instr *inot)
{
   rogue_instr *byp0s =
      &rogue_BYP0S(b, rogue_ref_io(ROGUE_IO_FT2), inot->src[0].ref)->instr;
   rogue_set_instr_group_next(byp0s, true);
   rogue_bitwise_instr * xor = rogue_XOR(b,
                                         inot->dst[0].ref,
                                         rogue_none(),
                                         rogue_ref_io(ROGUE_IO_FT2),
                                         rogue_none(),
                                         rogue_ref_imm(~0));

   rogue_merge_instr_comment(&xor->instr, &inot->instr, "inot");
   rogue_instr_delete(&inot->instr);

   return true;
}

static inline bool rogue_lower_IREV(rogue_builder *b, rogue_bitwise_instr *irev)
{
   rogue_bitwise_instr *rev =
      rogue_REV(b, rogue_ref_io(ROGUE_IO_FT2), irev->src[0].ref);
   rogue_set_instr_group_next(&rev->instr, true);

   /* TODO: encode same-phase bitwise ops properly then we can use byp0c. */
#if 0
   rogue_BYP0C(b, irev->dst[0].ref, rogue_ref_io(ROGUE_IO_FT2));
#else
   rogue_OR(b,
            irev->dst[0].ref,
            rogue_none(),
            rogue_ref_io(ROGUE_IO_FT2),
            rogue_none(),
            rogue_ref_imm(0));
#endif

   rogue_merge_instr_comment(&rev->instr, &irev->instr, "irev");
   rogue_instr_delete(&irev->instr);

   return true;
}

static inline bool rogue_lower_ICBS(rogue_builder *b, rogue_bitwise_instr *icbs)
{
   rogue_bitwise_instr *cbs = rogue_CBS(b, icbs->dst[0].ref, icbs->src[0].ref);

   rogue_merge_instr_comment(&cbs->instr, &icbs->instr, "icbs");
   rogue_instr_delete(&icbs->instr);

   return true;
}

static inline bool rogue_lower_IFTB(rogue_builder *b, rogue_bitwise_instr *iftb)
{
   rogue_bitwise_instr *ftb = rogue_FTB(b, iftb->dst[0].ref, iftb->src[0].ref);

   rogue_merge_instr_comment(&ftb->instr, &iftb->instr, "iftb");
   rogue_instr_delete(&iftb->instr);

   return true;
}

static inline bool rogue_lower_ISXT(rogue_builder *b, rogue_bitwise_instr *isxt)
{
   rogue_bitwise_instr *byp0b = rogue_BYP0B(b,
                                            rogue_ref_io(ROGUE_IO_FT0),
                                            rogue_ref_io(ROGUE_IO_FT1),
                                            isxt->src[1].ref, /* Sign bit
                                                                 position. */
                                            isxt->src[0].ref); /* Source. */
   rogue_set_instr_group_next(&byp0b->instr, true);

   rogue_bitwise_instr *asr = rogue_ASR(b,
                                        isxt->dst[0].ref,
                                        rogue_ref_io(ROGUE_IO_FT4),
                                        isxt->src[2].ref); /* Shift amount. */
   rogue_set_bitwise_op_mod(asr, ROGUE_BITWISE_OP_MOD_MTB);
   rogue_merge_instr_comment(&asr->instr, &isxt->instr, "isxt");

   rogue_instr_delete(&isxt->instr);

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

   case ROGUE_BITWISE_OP_INOT:
      return rogue_lower_INOT(b, bitwise);

   case ROGUE_BITWISE_OP_IREV:
      return rogue_lower_IREV(b, bitwise);

   case ROGUE_BITWISE_OP_ICBS:
      return rogue_lower_ICBS(b, bitwise);

   case ROGUE_BITWISE_OP_IFTB:
      return rogue_lower_IFTB(b, bitwise);

   case ROGUE_BITWISE_OP_ISXT:
      return rogue_lower_ISXT(b, bitwise);

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
