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
#include "rogue_isa.h"
#include "util/macros.h"
#include "util/memstream.h"
#include "util/u_dynarray.h"

#include <stdbool.h>

/**
 * \file rogue_encode.c
 *
 * \brief Contains hardware encoding functions.
 */

#define util_dynarray_append_mem(buf, size, mem) \
   memcpy(util_dynarray_grow_bytes((buf), 1, size), mem, size)

static unsigned rogue_calc_da(const rogue_instr_group *group)
{
   unsigned da = group->size.header;

   if (group->header.alu == ROGUE_ALU_MAIN) {
      for (unsigned u = ROGUE_INSTR_PHASE_COUNT; u > 0; --u) {
         enum rogue_instr_phase p = u - 1;
         if (p > ROGUE_INSTR_PHASE_1)
            da += group->size.instrs[p];
      }
   } else if (group->header.alu == ROGUE_ALU_BITWISE) {
      for (unsigned u = ROGUE_INSTR_PHASE_COUNT; u > 0; --u) {
         enum rogue_instr_phase p = u - 1;
         da += group->size.instrs[p];
      }
   } else if (group->header.alu == ROGUE_ALU_CONTROL) {
      const rogue_instr *instr = group->instrs[ROGUE_INSTR_PHASE_CTRL];
      const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);

      if (!rogue_ctrl_op_has_srcs(ctrl->op) &&
          !rogue_ctrl_op_has_dsts(ctrl->op)) {
         da = 0;
      } else {
         da += group->size.instrs[ROGUE_INSTR_PHASE_CTRL];
      }

   } else {
      unreachable("Unsupported instruction group ALU.");
   }

   return da;
}

#define P(type) BITFIELD64_BIT(ROGUE_INSTR_PHASE_##type)
static enum oporg rogue_calc_oporg(uint64_t alu_phases)
{
   bool P0 = !!(alu_phases & P(0));
   bool P1 = !!(alu_phases & P(1));
   bool P2 = !!(alu_phases & (P(2_PCK) | P(2_TST) | P(2_MOV)));
   bool PBE = !!(alu_phases & P(BACKEND));

   if (P0 && P1 && P2 && PBE)
      return OPORG_P0_P1_P2_BE;
   else if (P0 && !P1 && P2 && PBE)
      return OPORG_P0_P2_BE;
   else if (P0 && P1 && P2 && !PBE)
      return OPORG_P0_P1_P2;
   else if (P0 && !P1 && P2 && !PBE)
      return OPORG_P0_P2;
   else if (P0 && P1 && !P2 && !PBE)
      return OPORG_P0_P1;
   else if (!P0 && !P1 && !P2 && PBE)
      return OPORG_BE;
   else if (!P0 && !P1 && P2 && !PBE)
      return OPORG_P2;
   else if (P0 && !P1 && !P2 && !PBE)
      return OPORG_P0;

   unreachable("Invalid ALU phase combination.");
}

static enum opcnt rogue_calc_opcnt(uint64_t bitwise_phases)
{
   enum opcnt opcnt = 0;

   if (bitwise_phases & P(0_BITMASK) || bitwise_phases & P(0_SHIFT1) ||
       bitwise_phases & P(0_COUNT)) {
      opcnt |= OPCNT_P0;
   }

   if (bitwise_phases & P(1_LOGICAL))
      opcnt |= OPCNT_P1;

   if (bitwise_phases & P(2_SHIFT2) || bitwise_phases & P(2_TEST))
      opcnt |= OPCNT_P2;

   return opcnt;
}

static inline bool rogue_ref_needs_olchk(const rogue_ref *ref)
{
   enum rogue_reg_class class;
   unsigned index;

   if (!rogue_ref_reg_regarray_info(ref, &class, &index, NULL))
      return false;

   /* Pixout class should be lowered at this point,
    * but just in case...
    */
   if (class == ROGUE_REG_CLASS_PIXOUT)
      return true;

   if (class == ROGUE_REG_CLASS_SPECIAL)
      return rogue_special_reg_infos[index].needs_olchk;

   return false;
}

static inline bool rogue_group_instr_needs_olchk(rogue_instr_group *group)
{
   if (group->header.alu != ROGUE_ALU_MAIN)
      return false;

   if (!(group->header.phases & BITFIELD64_BIT(ROGUE_INSTR_PHASE_BACKEND)))
      return false;

   rogue_backend_instr *backend =
      rogue_instr_as_backend(group->instrs[ROGUE_INSTR_PHASE_BACKEND]);
   if (backend->op == ROGUE_BACKEND_OP_ATST) {
      if (!rogue_backend_op_mod_is_set(backend, ROGUE_BACKEND_OP_MOD_IFB))
         return true;
   }

   return false;
}

static bool rogue_should_set_olchk(rogue_instr_group *group)
{
   /* Only fragment shaders need overlap checks. */
   if (group->block->shader->stage != MESA_SHADER_NONE &&
       group->block->shader->stage != MESA_SHADER_FRAGMENT)
      return false;

   /* Check for specific backend instructions that require olchk to be set. */
   if (rogue_group_instr_needs_olchk(group))
      return true;

   /* Check if sources/dests read from/write to special registers that require
    * olchk to be set.
    */
   for (unsigned src = 0; src < ARRAY_SIZE(group->io_sel.srcs); ++src)
      if (rogue_ref_needs_olchk(&group->io_sel.srcs[src]))
         return true;

   for (unsigned dst = 0; dst < ARRAY_SIZE(group->io_sel.dsts); ++dst)
      if (rogue_ref_needs_olchk(&group->io_sel.dsts[dst]))
         return true;

   return false;
}

static void rogue_encode_instr_group_header(rogue_instr_group *group,
                                            struct util_dynarray *binary)
{
   rogue_instr_group_header_encoding h = { 0 };

   h.da = rogue_calc_da(group);
   h.length = (group->size.total / 2) % 16;
   h.ext = (group->size.header == 3);

   h.olchk = rogue_should_set_olchk(group);

   rogue_ref *w0ref = rogue_instr_group_io_sel_ref(&group->io_sel, ROGUE_IO_W0);
   rogue_ref *w1ref = rogue_instr_group_io_sel_ref(&group->io_sel, ROGUE_IO_W1);

   h.w1p = !rogue_ref_is_null(w1ref);
   h.w0p = !rogue_ref_is_null(w0ref);

   rogue_cc cc = { 0 };
   switch (group->header.exec_cond) {
   case ROGUE_EXEC_COND_PE_TRUE:
      cc._ = CC_PE_TRUE;
      break;

   case ROGUE_EXEC_COND_P0_TRUE:
      cc._ = CC_P0_TRUE;
      break;

   case ROGUE_EXEC_COND_PE_ANY:
      cc._ = CC_PE_ANY;
      break;

   case ROGUE_EXEC_COND_P0_FALSE:
      cc._ = CC_P0_FALSE;
      break;

   default:
      unreachable("Unsupported condition code.");
   }

   h.cc = cc.cc;
   h.ccext = cc.ccext;

   switch (group->header.alu) {
   case ROGUE_ALU_MAIN:
      h.alutype = ALUTYPE_MAIN;
      h.oporg = rogue_calc_oporg(group->header.phases);
      break;

   case ROGUE_ALU_BITWISE:
      h.alutype = ALUTYPE_BITWISE;
      h.opcnt = rogue_calc_opcnt(group->header.phases);
      break;

   case ROGUE_ALU_CONTROL:
      h.alutype = ALUTYPE_CONTROL;
#define OM(op_mod) ROGUE_CTRL_OP_MOD_##op_mod
      const rogue_instr *instr = group->instrs[ROGUE_INSTR_PHASE_CTRL];
      const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);
      switch (ctrl->op) {
      case ROGUE_CTRL_OP_NOP:
         h.ctrlop = CTRLOP_NOP;
         h.miscctl = rogue_ctrl_op_mod_is_set(ctrl, OM(END));
         break;

      case ROGUE_CTRL_OP_WOP:
         h.ctrlop = CTRLOP_WOP;
         break;

      case ROGUE_CTRL_OP_BR:
      case ROGUE_CTRL_OP_BA:
         h.ctrlop = CTRLOP_BA;
         break;

      case ROGUE_CTRL_OP_CNDST:
      case ROGUE_CTRL_OP_CNDEF:
      case ROGUE_CTRL_OP_CNDEND:
      case ROGUE_CTRL_OP_CNDLT:
      case ROGUE_CTRL_OP_CNDSM:
         h.ctrlop = CTRLOP_CND;
         break;

      case ROGUE_CTRL_OP_WDF:
         h.ctrlop = CTRLOP_WDF;
         h.miscctl = rogue_ref_get_drc_index(&ctrl->src[0].ref);
         break;

      default:
         unreachable("Unsupported ctrl op.");
      }
#undef OM
      break;

   default:
      unreachable("Unsupported instruction group ALU.");
   }

   if (group->header.alu != ROGUE_ALU_CONTROL) {
      h.end = group->header.end;
      /* h.atom = ; */ /* Unused for now */
      h.rpt = group->header.repeat - 1;
   }

   util_dynarray_append_mem(binary, group->size.header, &h);
}
#undef P

typedef union rogue_instr_encoding {
   rogue_alu_instr_encoding alu;
   rogue_backend_instr_encoding backend;
   rogue_ctrl_instr_encoding ctrl;
   rogue_bitwise_instr_encoding bitwise;
} PACKED rogue_instr_encoding;

static unsigned rogue_alu_movc_ft(const rogue_ref *ref)
{
   switch (rogue_ref_get_io(ref)) {
   case ROGUE_IO_NONE:
   case ROGUE_IO_FT0:
      return MOVW_FT0;

   case ROGUE_IO_FT1:
      return MOVW_FT1;

   case ROGUE_IO_FT2:
      return MOVW_FT2;

   case ROGUE_IO_FTE:
      return MOVW_FTE;

   default:
      break;
   }

   unreachable("Invalid source.");
}

static inline unsigned rogue_pck_fmt(enum rogue_alu_op op)
{
   switch (op) {
   case ROGUE_ALU_OP_PCK_CONST0:
      return PCK_FMT_ZERO;

   case ROGUE_ALU_OP_PCK_U8888:
   case ROGUE_ALU_OP_UPCK_U8888:
      return PCK_FMT_U8888;

   case ROGUE_ALU_OP_PCK_S8888:
   case ROGUE_ALU_OP_UPCK_S8888:
      return PCK_FMT_S8888;

   case ROGUE_ALU_OP_PCK_U1616:
   case ROGUE_ALU_OP_UPCK_U1616:
      return PCK_FMT_U1616;

   case ROGUE_ALU_OP_PCK_S1616:
   case ROGUE_ALU_OP_UPCK_S1616:
      return PCK_FMT_S1616;

   case ROGUE_ALU_OP_PCK_F16F16:
   case ROGUE_ALU_OP_UPCK_F16F16:
      return PCK_FMT_F16F16;

   case ROGUE_ALU_OP_PCK_U32:
   case ROGUE_ALU_OP_UPCK_U32:
      return PCK_FMT_U32;

   case ROGUE_ALU_OP_PCK_S32:
   case ROGUE_ALU_OP_UPCK_S32:
      return PCK_FMT_S32;

   case ROGUE_ALU_OP_PCK_F32:
      return PCK_FMT_F32;

   case ROGUE_ALU_OP_PCK_2F10F10F10:
      return PCK_FMT_2F10F10F10;

   default:
      break;
   }

   unreachable("Unsupported alu op.");
}

#define SM(src_mod) ROGUE_ALU_SRC_MOD_##src_mod
#define DM(dst_mod) ROGUE_ALU_DST_MOD_##dst_mod
#define OM(op_mod) ROGUE_ALU_OP_MOD_##op_mod
static void rogue_encode_alu_instr(const rogue_alu_instr *alu,
                                   unsigned instr_size,
                                   rogue_instr_encoding *instr_encoding)
{
   switch (alu->op) {
   case ROGUE_ALU_OP_MBYP0:
   case ROGUE_ALU_OP_MBYP1:
   case ROGUE_ALU_OP_FRCP:
   case ROGUE_ALU_OP_FRSQ:
   case ROGUE_ALU_OP_FLOG2:
   case ROGUE_ALU_OP_FLOGCN:
   case ROGUE_ALU_OP_FEXP2:
   case ROGUE_ALU_OP_FSINC:
   case ROGUE_ALU_OP_FARCTANC:
   case ROGUE_ALU_OP_FRED:
   case ROGUE_ALU_OP_FDSX:
   case ROGUE_ALU_OP_FDSY:
   case ROGUE_ALU_OP_FDSXF:
   case ROGUE_ALU_OP_FDSYF:
      instr_encoding->alu.op = ALUOP_SNGL;

      switch (alu->op) {
      case ROGUE_ALU_OP_MBYP0:
      case ROGUE_ALU_OP_MBYP1:
         instr_encoding->alu.sngl.snglop = SNGLOP_BYP;
         break;

      case ROGUE_ALU_OP_FRCP:
         instr_encoding->alu.sngl.snglop = SNGLOP_RCP;
         break;

      case ROGUE_ALU_OP_FRSQ:
         instr_encoding->alu.sngl.snglop = SNGLOP_RSQ;
         break;

      case ROGUE_ALU_OP_FLOG2:
         instr_encoding->alu.sngl.snglop = SNGLOP_LOG;
         break;

      case ROGUE_ALU_OP_FLOGCN:
         instr_encoding->alu.sngl.snglop = SNGLOP_LOGCN;
         break;

      case ROGUE_ALU_OP_FEXP2:
         instr_encoding->alu.sngl.snglop = SNGLOP_EXP;
         break;

      case ROGUE_ALU_OP_FSINC:
         instr_encoding->alu.sngl.snglop = SNGLOP_SINC;
         break;

      case ROGUE_ALU_OP_FARCTANC:
         instr_encoding->alu.sngl.snglop = SNGLOP_ARCTANC;
         break;

      case ROGUE_ALU_OP_FRED:
         instr_encoding->alu.sngl.snglop = SNGLOP_RED;
         break;

      case ROGUE_ALU_OP_FDSX:
         instr_encoding->alu.sngl.snglop = SNGLOP_DSX;
         break;

      case ROGUE_ALU_OP_FDSY:
         instr_encoding->alu.sngl.snglop = SNGLOP_DSY;
         break;

      case ROGUE_ALU_OP_FDSXF:
         instr_encoding->alu.sngl.snglop = SNGLOP_DSXF;
         break;

      case ROGUE_ALU_OP_FDSYF:
         instr_encoding->alu.sngl.snglop = SNGLOP_DSYF;
         break;

      default:
         unreachable("Unsupported alu op.");
      }

      if (instr_size == 2) {
         instr_encoding->alu.sngl.ext0 = 1;

         if (alu->op == ROGUE_ALU_OP_FRED) {
            instr_encoding->alu.sngl.red.s0neg =
               rogue_alu_src_mod_is_set(alu, 1, SM(NEG));
            instr_encoding->alu.sngl.red.s0abs =
               rogue_alu_src_mod_is_set(alu, 1, SM(ABS));

            rogue_alu_src_mod_is_set(alu, 0, SM(NEG));

            instr_encoding->alu.sngl.red.pwen =
               rogue_ref_is_io_p0(&alu->dst[2].ref);

            if (rogue_alu_op_mod_is_set(alu, OM(SIN)))
               instr_encoding->alu.sngl.red.type = RED_TYPE_SIN;
            else if (rogue_alu_op_mod_is_set(alu, OM(COS)))
               instr_encoding->alu.sngl.red.type = RED_TYPE_COS;
            else
               unreachable("Missing range reduction type.");

            instr_encoding->alu.sngl.red.iteration =
               rogue_ref_get_val(&alu->src[0].ref);

            if (rogue_alu_op_mod_is_set(alu, OM(PARTA)))
               instr_encoding->alu.sngl.red.part = RED_PART_A;
            else if (rogue_alu_op_mod_is_set(alu, OM(PARTB)))
               instr_encoding->alu.sngl.red.part = RED_PART_B;
            else
               unreachable("Missing range reduction part.");
         } else {
            instr_encoding->alu.sngl.cmplx.s0neg =
               rogue_alu_src_mod_is_set(alu, 0, SM(NEG));
            instr_encoding->alu.sngl.cmplx.s0abs =
               rogue_alu_src_mod_is_set(alu, 0, SM(ABS));
         }
      }
      break;

   case ROGUE_ALU_OP_FADD:
   case ROGUE_ALU_OP_FMUL:
      instr_encoding->alu.op = alu->op == ROGUE_ALU_OP_FADD ? ALUOP_FADD
                                                            : ALUOP_FMUL;
      instr_encoding->alu.fdual.lp = rogue_alu_op_mod_is_set(alu, OM(LP));
      instr_encoding->alu.fdual.sat = rogue_alu_op_mod_is_set(alu, OM(SAT));
      instr_encoding->alu.fdual.s0neg =
         rogue_alu_src_mod_is_set(alu, 0, SM(NEG));
      instr_encoding->alu.fdual.s0abs =
         rogue_alu_src_mod_is_set(alu, 0, SM(ABS));
      instr_encoding->alu.fdual.s1abs =
         rogue_alu_src_mod_is_set(alu, 1, SM(ABS));
      instr_encoding->alu.fdual.s0flr =
         rogue_alu_src_mod_is_set(alu, 0, SM(FLR));
      break;

   case ROGUE_ALU_OP_FMAD:
      instr_encoding->alu.op = ALUOP_FMAD;
      instr_encoding->alu.fmad.s0neg =
         rogue_alu_src_mod_is_set(alu, 0, SM(NEG));
      instr_encoding->alu.fmad.s0abs =
         rogue_alu_src_mod_is_set(alu, 0, SM(ABS));
      instr_encoding->alu.fmad.s2neg =
         rogue_alu_src_mod_is_set(alu, 2, SM(NEG));
      instr_encoding->alu.fmad.sat = rogue_alu_op_mod_is_set(alu, OM(SAT));

      if (instr_size == 2) {
         instr_encoding->alu.fmad.ext = 1;
         instr_encoding->alu.fmad.lp = rogue_alu_op_mod_is_set(alu, OM(LP));
         instr_encoding->alu.fmad.s1abs =
            rogue_alu_src_mod_is_set(alu, 1, SM(ABS));
         instr_encoding->alu.fmad.s1neg =
            rogue_alu_src_mod_is_set(alu, 1, SM(NEG));
         instr_encoding->alu.fmad.s2flr =
            rogue_alu_src_mod_is_set(alu, 2, SM(FLR));
         instr_encoding->alu.fmad.s2abs =
            rogue_alu_src_mod_is_set(alu, 2, SM(ABS));
      }
      break;

   case ROGUE_ALU_OP_TST0:
   case ROGUE_ALU_OP_TST1:
   case ROGUE_ALU_OP_TST2: {
      instr_encoding->alu.op = ALUOP_TST;
      instr_encoding->alu.tst.pwen = rogue_ref_is_io_p0(&alu->dst[1].ref);

      rogue_tstop tstop = { 0 };
      if (rogue_alu_op_mod_is_set(alu, OM(Z)))
         tstop._ = TSTOP_Z;
      else if (rogue_alu_op_mod_is_set(alu, OM(GZ)))
         tstop._ = TSTOP_GZ;
      else if (rogue_alu_op_mod_is_set(alu, OM(GEZ)))
         tstop._ = TSTOP_GEZ;
      else if (rogue_alu_op_mod_is_set(alu, OM(C)))
         tstop._ = TSTOP_C;
      else if (rogue_alu_op_mod_is_set(alu, OM(E)))
         tstop._ = TSTOP_E;
      else if (rogue_alu_op_mod_is_set(alu, OM(G)))
         tstop._ = TSTOP_G;
      else if (rogue_alu_op_mod_is_set(alu, OM(GE)))
         tstop._ = TSTOP_GE;
      else if (rogue_alu_op_mod_is_set(alu, OM(NE)))
         tstop._ = TSTOP_NE;
      else if (rogue_alu_op_mod_is_set(alu, OM(L)))
         tstop._ = TSTOP_L;
      else if (rogue_alu_op_mod_is_set(alu, OM(LE)))
         tstop._ = TSTOP_LE;
      else
         unreachable("Invalid comparison test.");

      instr_encoding->alu.tst.tstop_2_0 = tstop._2_0;

      if (instr_size == 2) {
         instr_encoding->alu.tst.ext = 1;
         instr_encoding->alu.tst.tstop_3 = tstop._3;

         if (rogue_alu_src_mod_is_set(alu, 0, SM(E0)))
            instr_encoding->alu.tst.elem = TST_E0;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E1)))
            instr_encoding->alu.tst.elem = TST_E1;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E2)))
            instr_encoding->alu.tst.elem = TST_E2;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E3)))
            instr_encoding->alu.tst.elem = TST_E3;

         instr_encoding->alu.tst.p2end =
            !rogue_phase_occupied(ROGUE_INSTR_PHASE_2_PCK,
                                  alu->instr.group->header.phases);

         if (rogue_alu_op_mod_is_set(alu, OM(F32)))
            instr_encoding->alu.tst.type = TSTTYPE_F32;
         else if (rogue_alu_op_mod_is_set(alu, OM(U16)))
            instr_encoding->alu.tst.type = TSTTYPE_U16;
         else if (rogue_alu_op_mod_is_set(alu, OM(S16)))
            instr_encoding->alu.tst.type = TSTTYPE_S16;
         else if (rogue_alu_op_mod_is_set(alu, OM(U8)))
            instr_encoding->alu.tst.type = TSTTYPE_U8;
         else if (rogue_alu_op_mod_is_set(alu, OM(S8)))
            instr_encoding->alu.tst.type = TSTTYPE_S8;
         else if (rogue_alu_op_mod_is_set(alu, OM(U32)))
            instr_encoding->alu.tst.type = TSTTYPE_U32;
         else if (rogue_alu_op_mod_is_set(alu, OM(S32)))
            instr_encoding->alu.tst.type = TSTTYPE_S32;
         else
            unreachable("Invalid comparison type.");
      }
      break;
   }

   case ROGUE_ALU_OP_MOVC: {
      instr_encoding->alu.op = ALUOP_MOVC;

      bool e0 = rogue_alu_dst_mod_is_set(alu, 0, DM(E0));
      bool e1 = rogue_alu_dst_mod_is_set(alu, 0, DM(E1));
      bool e2 = rogue_alu_dst_mod_is_set(alu, 0, DM(E2));
      bool e3 = rogue_alu_dst_mod_is_set(alu, 0, DM(E3));
      bool e_none = !e0 && !e1 && !e2 && !e3;

      instr_encoding->alu.movc.movw0 = rogue_alu_movc_ft(&alu->src[1].ref);
      instr_encoding->alu.movc.movw1 = rogue_alu_movc_ft(&alu->src[3].ref);

      if (instr_size == 2) {
         instr_encoding->alu.movc.ext = 1;
         instr_encoding->alu.movc.p2end =
            !rogue_phase_occupied(ROGUE_INSTR_PHASE_2_TST,
                                  alu->instr.group->header.phases) &&
            !rogue_phase_occupied(ROGUE_INSTR_PHASE_2_PCK,
                                  alu->instr.group->header.phases);
         instr_encoding->alu.movc.aw = !rogue_ref_is_io_ftt(&alu->src[0].ref);

         if (e_none) {
            instr_encoding->alu.movc.maskw0 = MASKW0_EALL;
         } else {
            instr_encoding->alu.movc.maskw0 |= e0 ? MASKW0_E0 : 0;
            instr_encoding->alu.movc.maskw0 |= e1 ? MASKW0_E1 : 0;
            instr_encoding->alu.movc.maskw0 |= e2 ? MASKW0_E2 : 0;
            instr_encoding->alu.movc.maskw0 |= e3 ? MASKW0_E3 : 0;
         }
      }
      break;
   }

   case ROGUE_ALU_OP_PCK_CONST0:
   case ROGUE_ALU_OP_PCK_U8888:
   case ROGUE_ALU_OP_PCK_S8888:
   case ROGUE_ALU_OP_PCK_U1616:
   case ROGUE_ALU_OP_PCK_S1616:
   case ROGUE_ALU_OP_PCK_F16F16:
   case ROGUE_ALU_OP_PCK_U32:
   case ROGUE_ALU_OP_PCK_S32:
   case ROGUE_ALU_OP_PCK_F32:
   case ROGUE_ALU_OP_PCK_2F10F10F10:
      instr_encoding->alu.op = ALUOP_SNGL;
      instr_encoding->alu.sngl.snglop = SNGLOP_PCK;
      instr_encoding->alu.sngl.ext0 = 1;

      instr_encoding->alu.sngl.pck.pck.prog = 0;
      instr_encoding->alu.sngl.pck.pck.rtz =
         rogue_alu_op_mod_is_set(alu, OM(ROUNDZERO));
      instr_encoding->alu.sngl.pck.pck.scale =
         rogue_alu_op_mod_is_set(alu, OM(SCALE));

      instr_encoding->alu.sngl.pck.pck.format = rogue_pck_fmt(alu->op);
      break;

   case ROGUE_ALU_OP_UPCK_U8888:
   case ROGUE_ALU_OP_UPCK_S8888:
   case ROGUE_ALU_OP_UPCK_U1616:
   case ROGUE_ALU_OP_UPCK_S1616:
   case ROGUE_ALU_OP_UPCK_F16F16:
   case ROGUE_ALU_OP_UPCK_S32:
   case ROGUE_ALU_OP_UPCK_U32:
      instr_encoding->alu.op = ALUOP_SNGL;
      instr_encoding->alu.sngl.snglop = SNGLOP_PCK;
      instr_encoding->alu.sngl.ext0 = 1;

      /* TODO NEXT: rogue validation check that elem is NOT set if repeat > 1
       * and that one *is* set if == 0/1. */
      switch (alu->op) {
      case ROGUE_ALU_OP_UPCK_U8888:
      case ROGUE_ALU_OP_UPCK_S8888:
      case ROGUE_ALU_OP_UPCK_U1616:
      case ROGUE_ALU_OP_UPCK_S1616:
      case ROGUE_ALU_OP_UPCK_F16F16:
         if (rogue_alu_src_mod_is_set(alu, 0, SM(E0)))
            instr_encoding->alu.sngl.pck.upck.elem = UPCK_E0;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E1)))
            instr_encoding->alu.sngl.pck.upck.elem = UPCK_E1;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E2)))
            instr_encoding->alu.sngl.pck.upck.elem = UPCK_E2;
         else if (rogue_alu_src_mod_is_set(alu, 0, SM(E3)))
            instr_encoding->alu.sngl.pck.upck.elem = UPCK_E3;
         else if (alu->instr.group->header.repeat == 1)
            unreachable("No unpack element selectors set.");
         break;

      default:
         break;
      }

      instr_encoding->alu.sngl.pck.upck.rtz |=
         rogue_alu_op_mod_is_set(alu, OM(ROUNDZERO));
      instr_encoding->alu.sngl.pck.upck.scale |=
         rogue_alu_op_mod_is_set(alu, OM(SCALE));

      instr_encoding->alu.sngl.pck.upck.format = rogue_pck_fmt(alu->op);
      break;

   case ROGUE_ALU_OP_ADD8:
   case ROGUE_ALU_OP_MUL8:
   case ROGUE_ALU_OP_ADD16:
   case ROGUE_ALU_OP_MUL16:
      instr_encoding->alu.op = ALUOP_INT8_16;

      switch (alu->op) {
      case ROGUE_ALU_OP_ADD8:
      case ROGUE_ALU_OP_MUL8:
         instr_encoding->alu.int8_16.f = F_8_BIT;
         break;

      case ROGUE_ALU_OP_ADD16:
      case ROGUE_ALU_OP_MUL16:
         instr_encoding->alu.int8_16.f = F_16_BIT;
         break;

      default:
         unreachable("Unsupported alu op.");
      }

      instr_encoding->alu.int8_16.s = rogue_alu_op_mod_is_set(alu, OM(S));

      switch (alu->op) {
      case ROGUE_ALU_OP_ADD8:
      case ROGUE_ALU_OP_ADD16:
         instr_encoding->alu.int8_16.int8_16_op = INT8_16_OP_ADD;
         break;

      case ROGUE_ALU_OP_MUL8:
      case ROGUE_ALU_OP_MUL16:
         instr_encoding->alu.int8_16.int8_16_op = INT8_16_OP_MUL;
         break;

      default:
         unreachable("Unsupported alu op.");
      }

      if (instr_size > 1) {
         instr_encoding->alu.int8_16.ext = 1;

         instr_encoding->alu.int8_16.sat =
            rogue_alu_op_mod_is_set(alu, OM(SAT));

         instr_encoding->alu.int8_16.s0abs =
            rogue_alu_src_mod_is_set(alu, 0, SM(ABS));
         instr_encoding->alu.int8_16.s0neg =
            rogue_alu_src_mod_is_set(alu, 0, SM(NEG));
         instr_encoding->alu.int8_16.s1abs =
            rogue_alu_src_mod_is_set(alu, 1, SM(ABS));
         instr_encoding->alu.int8_16.s2abs =
            rogue_alu_src_mod_is_set(alu, 2, SM(ABS));
         instr_encoding->alu.int8_16.s2neg =
            rogue_alu_src_mod_is_set(alu, 2, SM(NEG));

         /* TODO */
         /* instr_encoding->alu.int8_16.s2ch = ; */

         if (instr_size > 2) {
            if (rogue_alu_src_mod_is_set(alu, 0, SM(E0)))
               instr_encoding->alu.int8_16.s0ch = S0CH_E0;
            else if (rogue_alu_src_mod_is_set(alu, 0, SM(E1)))
               instr_encoding->alu.int8_16.s0ch = S0CH_E1;
            else if (rogue_alu_src_mod_is_set(alu, 0, SM(E2)))
               instr_encoding->alu.int8_16.s0ch = S0CH_E2;
            else if (rogue_alu_src_mod_is_set(alu, 0, SM(E3)))
               instr_encoding->alu.int8_16.s0ch = S0CH_E3;
            else
               unreachable("No S0 channel selectors set.");

            if (rogue_alu_src_mod_is_set(alu, 1, SM(E0)))
               instr_encoding->alu.int8_16.s1ch = S0CH_E0;
            else if (rogue_alu_src_mod_is_set(alu, 1, SM(E1)))
               instr_encoding->alu.int8_16.s1ch = S0CH_E1;
            else if (rogue_alu_src_mod_is_set(alu, 1, SM(E2)))
               instr_encoding->alu.int8_16.s1ch = S0CH_E2;
            else if (rogue_alu_src_mod_is_set(alu, 1, SM(E3)))
               instr_encoding->alu.int8_16.s1ch = S0CH_E3;
            else
               unreachable("No S1 channel selectors set.");
         }
      }

      break;

   case ROGUE_ALU_OP_ADD64:
   case ROGUE_ALU_OP_ADD64_32:
   case ROGUE_ALU_OP_MADD32:
   case ROGUE_ALU_OP_MADD64: {
      instr_encoding->alu.op = ALUOP_INT32_64;

      unsigned p0_src;
      switch (alu->op) {
      case ROGUE_ALU_OP_ADD64:
         instr_encoding->alu.int32_64.int32_64_op = INT32_64_OP_ADD64_NMX;
         p0_src = 4;
         break;

      case ROGUE_ALU_OP_ADD64_32:
         instr_encoding->alu.int32_64.int32_64_op = INT32_64_OP_ADD6432;
         p0_src = 3;
         break;

      case ROGUE_ALU_OP_MADD32:
         instr_encoding->alu.int32_64.int32_64_op = INT32_64_OP_MADD32;
         p0_src = 3;
         break;

      case ROGUE_ALU_OP_MADD64:
         instr_encoding->alu.int32_64.int32_64_op = INT32_64_OP_MADD64;
         p0_src = 4;
         break;

      default:
         unreachable("Unsupported alu op.");
      }

      instr_encoding->alu.int32_64.s2neg =
         rogue_alu_src_mod_is_set(alu, 2, SM(NEG));
      instr_encoding->alu.int32_64.s = rogue_alu_op_mod_is_set(alu, OM(S));

      if (instr_size == 2) {
         instr_encoding->alu.int32_64.ext = 1;
         instr_encoding->alu.int32_64.s2abs =
            rogue_alu_src_mod_is_set(alu, 2, SM(ABS));
         instr_encoding->alu.int32_64.s1abs =
            rogue_alu_src_mod_is_set(alu, 1, SM(ABS));
         instr_encoding->alu.int32_64.s0abs =
            rogue_alu_src_mod_is_set(alu, 0, SM(ABS));
         instr_encoding->alu.int32_64.s0neg =
            rogue_alu_src_mod_is_set(alu, 0, SM(NEG));
         instr_encoding->alu.int32_64.s1neg =
            rogue_alu_src_mod_is_set(alu, 1, SM(NEG));
         instr_encoding->alu.int32_64.cin =
            rogue_ref_is_io_p0(&alu->src[p0_src].ref);
      }
      break;
   }

   default:
      unreachable("Unsupported alu op.");
   }
}
#undef OM
#undef DM
#undef SM

#define OM(op_mod) ROGUE_BACKEND_OP_MOD_##op_mod
static unsigned rogue_backend_get_cachemode(const rogue_backend_instr *backend)
{
   if (rogue_backend_op_mod_is_set(backend, OM(BYPASS)))
      return CACHEMODE_LD_BYPASS;
   else if (rogue_backend_op_mod_is_set(backend, OM(FORCELINEFILL)))
      return CACHEMODE_LD_FORCE_LINE_FILL;
   else if (rogue_backend_op_mod_is_set(backend, OM(WRITETHROUGH)))
      return CACHEMODE_ST_WRITE_THROUGH;
   else if (rogue_backend_op_mod_is_set(backend, OM(WRITEBACK)))
      return CACHEMODE_ST_WRITE_BACK;
   else if (rogue_backend_op_mod_is_set(backend, OM(LAZYWRITEBACK)))
      return CACHEMODE_ST_WRITE_BACK_LAZY;

   /* Default cache mode. */
   return CACHEMODE_LD_NORMAL; /* == CACHEMODE_ST_WRITE_THROUGH */
}

static unsigned
rogue_backend_get_slccachemode(const rogue_backend_instr *backend)
{
   if (rogue_backend_op_mod_is_set(backend, OM(SLCBYPASS)))
      return SLCCACHEMODE_BYPASS;
   else if (rogue_backend_op_mod_is_set(backend, OM(SLCWRITEBACK)))
      return SLCCACHEMODE_WRITE_BACK;
   else if (rogue_backend_op_mod_is_set(backend, OM(SLCWRITETHROUGH)))
      return SLCCACHEMODE_WRITE_THROUGH;
   else if (rogue_backend_op_mod_is_set(backend, OM(SLCNOALLOC)))
      return SLCCACHEMODE_CACHED_READS;

   /* Default SLC cache mode. */
   return SLCCACHEMODE_BYPASS;
}

static inline unsigned rogue_ref_get_vtxout_index(const rogue_ref *ref)
{
   assert(rogue_ref_is_reg(ref));

   rogue_reg *reg = ref->reg;
   assert(reg->class == ROGUE_REG_CLASS_VTXOUT);

   return reg->index;
}

static void rogue_encode_backend_instr(const rogue_backend_instr *backend,
                                       unsigned instr_size,
                                       rogue_instr_encoding *instr_encoding)
{
   switch (backend->op) {
   case ROGUE_BACKEND_OP_FITR_PIXEL:
      instr_encoding->backend.op = BACKENDOP_FITR;
      instr_encoding->backend.fitr.p = 0;
      instr_encoding->backend.fitr.drc =
         rogue_ref_get_drc_index(&backend->src[0].ref);
      instr_encoding->backend.fitr.mode = FITR_MODE_PIXEL;
      instr_encoding->backend.fitr.sat =
         rogue_backend_op_mod_is_set(backend, OM(SAT));
      instr_encoding->backend.fitr.count =
         rogue_ref_get_val(&backend->src[2].ref);
      break;

   case ROGUE_BACKEND_OP_FITRP_PIXEL:
      instr_encoding->backend.op = BACKENDOP_FITR;
      instr_encoding->backend.fitr.p = 1;
      instr_encoding->backend.fitr.drc =
         rogue_ref_get_drc_index(&backend->src[0].ref);
      instr_encoding->backend.fitr.mode = FITR_MODE_PIXEL;
      instr_encoding->backend.fitr.sat =
         rogue_backend_op_mod_is_set(backend, OM(SAT));
      instr_encoding->backend.fitr.count =
         rogue_ref_get_val(&backend->src[3].ref);
      break;

   case ROGUE_BACKEND_OP_UVSW_WRITE:
      instr_encoding->backend.op = BACKENDOP_UVSW;
      instr_encoding->backend.uvsw.writeop = UVSW_WRITEOP_WRITE;
      instr_encoding->backend.uvsw.imm = 1;
      instr_encoding->backend.uvsw.imm_src.imm_addr =
         rogue_ref_get_vtxout_index(&backend->dst[0].ref);
      break;

   case ROGUE_BACKEND_OP_UVSW_EMIT:
      instr_encoding->backend.op = BACKENDOP_UVSW;
      instr_encoding->backend.uvsw.writeop = UVSW_WRITEOP_EMIT;
      break;

   case ROGUE_BACKEND_OP_UVSW_ENDTASK:
      instr_encoding->backend.op = BACKENDOP_UVSW;
      instr_encoding->backend.uvsw.writeop = UVSW_WRITEOP_END;
      break;

   case ROGUE_BACKEND_OP_UVSW_EMITTHENENDTASK:
      instr_encoding->backend.op = BACKENDOP_UVSW;
      instr_encoding->backend.uvsw.writeop = UVSW_WRITEOP_EMIT_END;
      break;

   case ROGUE_BACKEND_OP_UVSW_WRITETHENEMITTHENENDTASK:
      instr_encoding->backend.op = BACKENDOP_UVSW;
      instr_encoding->backend.uvsw.writeop = UVSW_WRITEOP_WRITE_EMIT_END;
      instr_encoding->backend.uvsw.imm = 1;
      instr_encoding->backend.uvsw.imm_src.imm_addr =
         rogue_ref_get_vtxout_index(&backend->dst[0].ref);
      break;

   case ROGUE_BACKEND_OP_LD: {
      instr_encoding->backend.op = BACKENDOP_DMA;
      instr_encoding->backend.dma.dmaop = DMAOP_LD;
      instr_encoding->backend.dma.ld.drc =
         rogue_ref_get_drc_index(&backend->src[0].ref);
      instr_encoding->backend.dma.ld.cachemode =
         rogue_backend_get_cachemode(backend);

      bool imm_burstlen = rogue_ref_is_val(&backend->src[1].ref);

      rogue_burstlen burstlen = {
         ._ = imm_burstlen ? rogue_ref_get_val(&backend->src[1].ref) : 0
      };

      if (imm_burstlen) {
         instr_encoding->backend.dma.ld.burstlen_2_0 = burstlen._2_0;
      } else {
         instr_encoding->backend.dma.ld.srcselbl =
            rogue_ref_get_io_src_index(&backend->src[1].ref);
      }

      instr_encoding->backend.dma.ld.srcseladd =
         rogue_ref_get_io_src_index(&backend->src[2].ref);

      if (instr_size == 3) {
         instr_encoding->backend.dma.ld.ext = 1;
         if (imm_burstlen)
            instr_encoding->backend.dma.ld.burstlen_3 = burstlen._3;

         instr_encoding->backend.dma.ld.slccachemode =
            rogue_backend_get_slccachemode(backend);
         instr_encoding->backend.dma.ld.notimmbl = !imm_burstlen;
      }

      break;
   }

   case ROGUE_BACKEND_OP_ST: {
      instr_encoding->backend.op = BACKENDOP_DMA;
      instr_encoding->backend.dma.dmaop = DMAOP_ST;
      instr_encoding->backend.dma.st.drc =
         rogue_ref_get_drc_index(&backend->src[2].ref);

      bool imm_burstlen = rogue_ref_is_val(&backend->src[3].ref);

      instr_encoding->backend.dma.st.immbl = imm_burstlen;

      if (imm_burstlen) {
         rogue_burstlen burstlen = { ._ = rogue_ref_get_val(
                                        &backend->src[3].ref) };
         instr_encoding->backend.dma.st.burstlen_2_0 = burstlen._2_0;
         instr_encoding->backend.dma.st.burstlen_3 = burstlen._3;
      } else {
         instr_encoding->backend.dma.st.srcselbl =
            rogue_ref_get_io_src_index(&backend->src[3].ref);
      }

      instr_encoding->backend.dma.st.cachemode =
         rogue_backend_get_cachemode(backend);
      instr_encoding->backend.dma.st.srcseladd =
         rogue_ref_get_io_src_index(&backend->src[4].ref);

      instr_encoding->backend.dma.st.dsize =
         rogue_ref_get_val(&backend->src[1].ref);
      instr_encoding->backend.dma.st.srcseldata =
         rogue_ref_get_io_src_index(&backend->src[0].ref);

      if (instr_size == 4) {
         instr_encoding->backend.dma.st.ext = 1;
         instr_encoding->backend.dma.st.srcmask =
            rogue_ref_get_io_src_index(&backend->src[5].ref);
         instr_encoding->backend.dma.st.slccachemode =
            rogue_backend_get_slccachemode(backend);
         instr_encoding->backend.dma.st.nottiled =
            !rogue_backend_op_mod_is_set(backend, OM(TILED));
      }

      break;
   }

   case ROGUE_BACKEND_OP_ATST:
      instr_encoding->backend.op = BACKENDOP_VISTEST;

      instr_encoding->backend.vistest.ifb =
         rogue_backend_op_mod_is_set(backend, OM(IFB));
      instr_encoding->backend.vistest.atst = 1;
      instr_encoding->backend.vistest.pwen =
         rogue_ref_is_io_p0(&backend->dst[0].ref);
      break;

   case ROGUE_BACKEND_OP_SMP1D:
   case ROGUE_BACKEND_OP_SMP2D:
   case ROGUE_BACKEND_OP_SMP3D:
      instr_encoding->backend.op = BACKENDOP_DMA;
      instr_encoding->backend.dma.dmaop = DMAOP_SMP;

      instr_encoding->backend.dma.smp.drc =
         rogue_ref_get_drc_index(&backend->src[0].ref);
      instr_encoding->backend.dma.smp.fcnorm =
         rogue_backend_op_mod_is_set(backend, OM(FCNORM));

      if (rogue_backend_op_mod_is_set(backend, OM(BIAS)))
         instr_encoding->backend.dma.smp.lodm = LODM_BIAS;
      else if (rogue_backend_op_mod_is_set(backend, OM(REPLACE)))
         instr_encoding->backend.dma.smp.lodm = LODM_REPLACE;
      else if (rogue_backend_op_mod_is_set(backend, OM(GRADIENT)))
         instr_encoding->backend.dma.smp.lodm = LODM_GRADIENTS;
      else
         instr_encoding->backend.dma.smp.lodm = LODM_NORMAL;

      switch (rogue_ref_get_val(&backend->src[5].ref)) {
      case 1:
         instr_encoding->backend.dma.smp.chan = SMPCHAN_1;
         break;

      case 2:
         instr_encoding->backend.dma.smp.chan = SMPCHAN_2;
         break;

      case 3:
         instr_encoding->backend.dma.smp.chan = SMPCHAN_3;
         break;

      case 4:
         instr_encoding->backend.dma.smp.chan = SMPCHAN_4;
         break;

      default:
         unreachable("Unsupported number of channels.");
      }

      switch (backend->op) {
      case ROGUE_BACKEND_OP_SMP1D:
         instr_encoding->backend.dma.smp.dmn = DMN_1D;
         break;

      case ROGUE_BACKEND_OP_SMP2D:
         instr_encoding->backend.dma.smp.dmn = DMN_2D;
         break;

      case ROGUE_BACKEND_OP_SMP3D:
         instr_encoding->backend.dma.smp.dmn = DMN_3D;
         break;

      default:
         unreachable("Unsupported sampler op.");
      }

      if (instr_size > 2) {
         instr_encoding->backend.dma.smp.exta = 1;

         instr_encoding->backend.dma.smp.tao =
            rogue_backend_op_mod_is_set(backend, OM(TAO));
         instr_encoding->backend.dma.smp.soo =
            rogue_backend_op_mod_is_set(backend, OM(SOO));
         instr_encoding->backend.dma.smp.sno =
            rogue_backend_op_mod_is_set(backend, OM(SNO));
         instr_encoding->backend.dma.smp.nncoords =
            rogue_backend_op_mod_is_set(backend, OM(NNCOORDS));

         if (rogue_backend_op_mod_is_set(backend, OM(DATA)))
            instr_encoding->backend.dma.smp.sbmode = SBMODE_DATA;
         else if (rogue_backend_op_mod_is_set(backend, OM(INFO)))
            instr_encoding->backend.dma.smp.sbmode = SBMODE_INFO;
         else if (rogue_backend_op_mod_is_set(backend, OM(BOTH)))
            instr_encoding->backend.dma.smp.sbmode = SBMODE_BOTH;
         else
            instr_encoding->backend.dma.smp.sbmode = SBMODE_NONE;

         instr_encoding->backend.dma.smp.proj =
            rogue_backend_op_mod_is_set(backend, OM(PROJ));
         instr_encoding->backend.dma.smp.pplod =
            rogue_backend_op_mod_is_set(backend, OM(PPLOD));
      }

      if (instr_size > 3) {
         instr_encoding->backend.dma.smp.extb = 1;

         instr_encoding->backend.dma.smp.w =
            rogue_backend_op_mod_is_set(backend, OM(WRT));

         instr_encoding->backend.dma.smp.cachemode =
            rogue_backend_get_cachemode(backend);

         instr_encoding->backend.dma.smp.swap =
            rogue_backend_op_mod_is_set(backend, OM(SCHEDSWAP));
         instr_encoding->backend.dma.smp.f16 =
            rogue_backend_op_mod_is_set(backend, OM(F16));

         instr_encoding->backend.dma.smp.slccachemode =
            rogue_backend_get_slccachemode(backend);
      }

      if (instr_size > 4) {
         instr_encoding->backend.dma.smp.extc = 1;

         instr_encoding->backend.dma.smp.array =
            rogue_backend_op_mod_is_set(backend, OM(ARRAY));
      }

      break;

   case ROGUE_BACKEND_OP_IDF:
      instr_encoding->backend.op = BACKENDOP_DMA;
      instr_encoding->backend.dma.dmaop = DMAOP_IDF;
      instr_encoding->backend.dma.idf.drc =
         rogue_ref_get_drc_index(&backend->src[0].ref);
      instr_encoding->backend.dma.idf.srcseladd =
         rogue_ref_get_io_src_index(&backend->src[1].ref);
      break;

   case ROGUE_BACKEND_OP_EMITPIX:
      instr_encoding->backend.op = BACKENDOP_EMIT;
      instr_encoding->backend.emitpix.freep =
         rogue_backend_op_mod_is_set(backend, OM(FREEP));
      break;

   default:
      unreachable("Unsupported backend op.");
   }
}
#undef OM

#define OM(op_mod) ROGUE_CTRL_OP_MOD_##op_mod
static void rogue_encode_ctrl_instr(const rogue_ctrl_instr *ctrl,
                                    unsigned instr_size,
                                    rogue_instr_encoding *instr_encoding)
{
   /* Only some control instructions have additional bytes. */
   switch (ctrl->op) {
   case ROGUE_CTRL_OP_NOP:
      memset(&instr_encoding->ctrl.nop, 0, sizeof(instr_encoding->ctrl.nop));
      break;

   case ROGUE_CTRL_OP_BR:
   case ROGUE_CTRL_OP_BA: {
      bool branch_abs = (ctrl->op == ROGUE_CTRL_OP_BA);
      rogue_offset32 offset;

      instr_encoding->ctrl.ba.abs = branch_abs;
      instr_encoding->ctrl.ba.allp =
         rogue_ctrl_op_mod_is_set(ctrl, OM(ALLINST));
      instr_encoding->ctrl.ba.anyp =
         rogue_ctrl_op_mod_is_set(ctrl, OM(ANYINST));
      instr_encoding->ctrl.ba.link = rogue_ctrl_op_mod_is_set(ctrl, OM(LINK));

      if (branch_abs) {
         offset._ = rogue_ref_get_val(&ctrl->src[0].ref);
      } else {
         rogue_instr_group *block_group =
            list_entry(ctrl->target_block->instrs.next,
                       rogue_instr_group,
                       link);
         offset._ = block_group->size.offset - ctrl->instr.group->size.offset;
      }

      instr_encoding->ctrl.ba.offset_7_1 = offset._7_1;
      instr_encoding->ctrl.ba.offset_15_8 = offset._15_8;
      instr_encoding->ctrl.ba.offset_23_16 = offset._23_16;
      instr_encoding->ctrl.ba.offset_31_24 = offset._31_24;

      break;
   }

   case ROGUE_CTRL_OP_CNDST:
   case ROGUE_CTRL_OP_CNDEF:
   case ROGUE_CTRL_OP_CNDEND:
   case ROGUE_CTRL_OP_CNDLT:
   case ROGUE_CTRL_OP_CNDSM:
      switch (ctrl->op) {
      case ROGUE_CTRL_OP_CNDST:
         instr_encoding->ctrl.cnd.cndinst = CNDINST_ST;
         break;

      case ROGUE_CTRL_OP_CNDEF:
         instr_encoding->ctrl.cnd.cndinst = CNDINST_EF;
         break;

      case ROGUE_CTRL_OP_CNDEND:
         instr_encoding->ctrl.cnd.cndinst = CNDINST_END;
         break;

      case ROGUE_CTRL_OP_CNDLT:
         instr_encoding->ctrl.cnd.cndinst = CNDINST_LT;
         break;

      case ROGUE_CTRL_OP_CNDSM:
         instr_encoding->ctrl.cnd.cndinst = CNDINST_SM;
         break;

      default:
         unreachable("Unsupported ctrl op.");
      }

      switch (ctrl->op) {
      case ROGUE_CTRL_OP_CNDST:
      case ROGUE_CTRL_OP_CNDEF:
      case ROGUE_CTRL_OP_CNDLT:
      case ROGUE_CTRL_OP_CNDSM:
         if (rogue_ctrl_op_mod_is_set(ctrl, OM(ALWAYS)))
            instr_encoding->ctrl.cnd.pcnd = PCND_ALWAYS;
         else if (rogue_ctrl_op_mod_is_set(ctrl, OM(P0_TRUE)))
            instr_encoding->ctrl.cnd.pcnd = PCND_P0_TRUE;
         else if (rogue_ctrl_op_mod_is_set(ctrl, OM(NEVER)))
            instr_encoding->ctrl.cnd.pcnd = PCND_NEVER;
         else if (rogue_ctrl_op_mod_is_set(ctrl, OM(P0_FALSE)))
            instr_encoding->ctrl.cnd.pcnd = PCND_P0_FALSE;
         else
            unreachable("Missing conditional test.");
         break;

      default:
         break;
      }

      switch (ctrl->op) {
      case ROGUE_CTRL_OP_CNDST:
      case ROGUE_CTRL_OP_CNDEF:
      case ROGUE_CTRL_OP_CNDEND:
      case ROGUE_CTRL_OP_CNDLT:
         instr_encoding->ctrl.cnd.adjust = rogue_ref_get_val(&ctrl->src[1].ref);
         break;

      default:
         break;
      }

      break;

   default:
      unreachable("Unsupported ctrl op.");
   }
}
#undef OM

#define OM(op_mod) ROGUE_BITWISE_OP_MOD_##op_mod
static void rogue_encode_bitwise_instr(const rogue_bitwise_instr *bitwise,
                                       unsigned instr_size,
                                       rogue_instr_encoding *instr_encoding)
{
   switch (bitwise->op) {
   case ROGUE_BITWISE_OP_LSL0:
      instr_encoding->bitwise.phase0 = 1;
      instr_encoding->bitwise.ph0.shft = SHFT1_LSL;
      break;

   case ROGUE_BITWISE_OP_LSL2:
      instr_encoding->bitwise.ph2.shft = SHFT2_LSL;
      break;

   case ROGUE_BITWISE_OP_SHR:
      instr_encoding->bitwise.ph2.shft = SHFT2_SHR;
      break;

   case ROGUE_BITWISE_OP_ASR:
      if (rogue_bitwise_op_mod_is_set(bitwise, OM(TWB)))
         instr_encoding->bitwise.ph2.shft = SHFT2_ASR_TWB;
      else if (rogue_bitwise_op_mod_is_set(bitwise, OM(PWB)))
         instr_encoding->bitwise.ph2.shft = SHFT2_ASR_PWB;
      else if (rogue_bitwise_op_mod_is_set(bitwise, OM(MTB)))
         instr_encoding->bitwise.ph2.shft = SHFT2_ASR_MTB;
      else if (rogue_bitwise_op_mod_is_set(bitwise, OM(FTB)))
         instr_encoding->bitwise.ph2.shft = SHFT2_ASR_FTB;
      else
         unreachable("Missing sign-bit position modifier.");
      break;

   case ROGUE_BITWISE_OP_TZ:
   case ROGUE_BITWISE_OP_TNZ:
      instr_encoding->bitwise.ph2.top =
         (bitwise->op == ROGUE_BITWISE_OP_TZ) ? TOP_TZ : TOP_TNZ;
      assert(rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_FT5 ||
             rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_FT3);
      instr_encoding->bitwise.ph2.tsrc =
         (rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_FT5) ? TSRC_FT5
                                                                  : TSRC_FT3;
      assert(rogue_ref_is_io_p0(&bitwise->dst[0].ref));
      instr_encoding->bitwise.ph2.pwen = 1;
      break;

   case ROGUE_BITWISE_OP_AND:
   case ROGUE_BITWISE_OP_OR:
   case ROGUE_BITWISE_OP_XOR:
   case ROGUE_BITWISE_OP_BYP1L:
      instr_encoding->bitwise.phase1 = 1;

      switch (bitwise->op) {
      case ROGUE_BITWISE_OP_AND:
         instr_encoding->bitwise.ph1.op = PH1OP_AND;
         break;

      case ROGUE_BITWISE_OP_OR:
         instr_encoding->bitwise.ph1.op = PH1OP_OR;
         break;

      case ROGUE_BITWISE_OP_XOR:
         instr_encoding->bitwise.ph1.op = PH1OP_XOR;
         break;

      case ROGUE_BITWISE_OP_BYP1L:
         instr_encoding->bitwise.ph1.op = PH1OP_BYP;
         break;

      default:
         unreachable("Unsupported bitwise op.");
      }

      if (bitwise->op != ROGUE_BITWISE_OP_BYP1L) {
         instr_encoding->bitwise.ph1.mska =
            !rogue_ref_is_io_none(&bitwise->src[0].ref);
         instr_encoding->bitwise.ph1.mskb =
            !rogue_ref_is_io_none(&bitwise->src[2].ref);
      }
      break;

   case ROGUE_BITWISE_OP_REV:
      instr_encoding->bitwise.phase0 = 1;
      instr_encoding->bitwise.ph0.shft = SHFT1_REV;
      break;

   case ROGUE_BITWISE_OP_MSK:
      instr_encoding->bitwise.phase0 = 1;
      instr_encoding->bitwise.ph0.bm = 1;
      break;

   case ROGUE_BITWISE_OP_BYP0B: {
      instr_encoding->bitwise.phase0 = 1;

      rogue_imm32 imm32;
      if (rogue_ref_is_val(&bitwise->src[1].ref))
         imm32._ = rogue_ref_get_val(&bitwise->src[1].ref);

      if (instr_size > 1) {
         instr_encoding->bitwise.ph0.ext = 1;
         instr_encoding->bitwise.ph0.imm_7_0 = imm32._7_0;
         instr_encoding->bitwise.ph0.imm_15_8 = imm32._15_8;
      }

      if (instr_size > 3) {
         instr_encoding->bitwise.ph0.bm = 1;
         instr_encoding->bitwise.ph0.imm_23_16 = imm32._23_16;
         instr_encoding->bitwise.ph0.imm_31_24 = imm32._31_24;
      }

      break;
   }

   case ROGUE_BITWISE_OP_BYP0C:
   case ROGUE_BITWISE_OP_CBS:
   case ROGUE_BITWISE_OP_FTB:
      instr_encoding->bitwise.phase0 = 1;

      switch (bitwise->op) {
      case ROGUE_BITWISE_OP_BYP0C:
         instr_encoding->bitwise.ph0.cnt_byp = 1;
         break;

      case ROGUE_BITWISE_OP_CBS:
         instr_encoding->bitwise.ph0.cnt = CNT_CBS;
         break;

      case ROGUE_BITWISE_OP_FTB:
         instr_encoding->bitwise.ph0.cnt = CNT_FTB;
         break;

      default:
         unreachable("Unsupported bitwise op.");
      }

      assert(rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_S2 ||
             rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_FT2);

      instr_encoding->bitwise.ph0.csrc =
         (rogue_ref_get_io(&bitwise->src[0].ref) == ROGUE_IO_S2) ? CNT_S2
                                                                 : CNT_FT2;
      break;

   case ROGUE_BITWISE_OP_BYP0S:
      instr_encoding->bitwise.phase0 = 1;
      instr_encoding->bitwise.ph0.shft = SHFT1_BYP;
      break;

   default:
      unreachable("Invalid bitwise op.");
   }
}
#undef OM

static void
rogue_encode_bitwise_instr_group_instrs(rogue_instr_group *group,
                                        struct util_dynarray *binary)
{
   rogue_instr_encoding instr_encodings[3] = { 0 };

   rogue_foreach_phase_in_set_rev (p, group->header.phases) {
      const rogue_instr *instr = group->instrs[p];
      rogue_instr_encoding *instr_encoding;
      unsigned instr_size;

      switch (p) {
      case ROGUE_INSTR_PHASE_0_BITMASK:
      case ROGUE_INSTR_PHASE_0_SHIFT1:
      case ROGUE_INSTR_PHASE_0_COUNT:
         instr_encoding = &instr_encodings[0];
         instr_size = group->size.instrs[ROGUE_INSTR_PHASE_0_BITMASK];
         break;

      case ROGUE_INSTR_PHASE_1_LOGICAL:
         instr_encoding = &instr_encodings[1];
         instr_size = group->size.instrs[ROGUE_INSTR_PHASE_1_LOGICAL];
         break;

      case ROGUE_INSTR_PHASE_2_SHIFT2:
      case ROGUE_INSTR_PHASE_2_TEST:
         instr_encoding = &instr_encodings[2];
         instr_size = group->size.instrs[ROGUE_INSTR_PHASE_2_SHIFT2];
         break;

      default:
         unreachable();
      }

      rogue_encode_bitwise_instr(rogue_instr_as_bitwise(instr),
                                 instr_size,
                                 instr_encoding);
   }

   if (group->size.instrs[ROGUE_INSTR_PHASE_2_SHIFT2])
      util_dynarray_append_mem(binary,
                               group->size.instrs[ROGUE_INSTR_PHASE_2_SHIFT2],
                               &instr_encodings[2]);

   if (group->size.instrs[ROGUE_INSTR_PHASE_1_LOGICAL])
      util_dynarray_append_mem(binary,
                               group->size.instrs[ROGUE_INSTR_PHASE_1_LOGICAL],
                               &instr_encodings[1]);

   if (group->size.instrs[ROGUE_INSTR_PHASE_0_BITMASK])
      util_dynarray_append_mem(binary,
                               group->size.instrs[ROGUE_INSTR_PHASE_0_BITMASK],
                               &instr_encodings[0]);
}

static void rogue_encode_instr_group_instrs(rogue_instr_group *group,
                                            struct util_dynarray *binary)
{
   rogue_instr_encoding instr_encoding;

   /* Bitwise instructions are handled separately. */
   if (group->header.alu == ROGUE_ALU_BITWISE)
      return rogue_encode_bitwise_instr_group_instrs(group, binary);

   /* Reverse order for encoding. */
   rogue_foreach_phase_in_set_rev (p, group->header.phases) {
      if (!group->size.instrs[p])
         continue;

      memset(&instr_encoding, 0, sizeof(instr_encoding));

      const rogue_instr *instr = group->instrs[p];
      switch (instr->type) {
      case ROGUE_INSTR_TYPE_ALU:
         rogue_encode_alu_instr(rogue_instr_as_alu(instr),
                                group->size.instrs[p],
                                &instr_encoding);
         break;

      case ROGUE_INSTR_TYPE_BACKEND:
         rogue_encode_backend_instr(rogue_instr_as_backend(instr),
                                    group->size.instrs[p],
                                    &instr_encoding);
         break;

      case ROGUE_INSTR_TYPE_CTRL:
         rogue_encode_ctrl_instr(rogue_instr_as_ctrl(instr),
                                 group->size.instrs[p],
                                 &instr_encoding);
         break;

      case ROGUE_INSTR_TYPE_BITWISE:
         rogue_encode_bitwise_instr(rogue_instr_as_bitwise(instr),
                                    group->size.instrs[p],
                                    &instr_encoding);
         break;

      default:
         unreachable("Unsupported instruction type.");
      }

      util_dynarray_append_mem(binary, group->size.instrs[p], &instr_encoding);
   }
}

static void rogue_encode_source_map(const rogue_instr_group *group,
                                    bool upper_srcs,
                                    rogue_source_map_encoding *e)
{
   unsigned base = upper_srcs ? 3 : 0;
   unsigned index = upper_srcs ? group->encode_info.upper_src_index
                               : group->encode_info.lower_src_index;
   const rogue_reg_src_info *info = upper_srcs
                                       ? &rogue_reg_upper_src_infos[index]
                                       : &rogue_reg_lower_src_infos[index];
   const rogue_instr_group_io_sel *io_sel = &group->io_sel;

   rogue_mux mux = { 0 };

   if (!upper_srcs && rogue_ref_is_io(&io_sel->iss[0])) {
      switch (io_sel->iss[0].io) {
      case ROGUE_IO_S0:
         mux._ = IS0_S0;
         break;
      case ROGUE_IO_S3:
         mux._ = IS0_S3;
         break;
      case ROGUE_IO_S4:
         mux._ = IS0_S4;
         break;
      case ROGUE_IO_S5:
         mux._ = IS0_S5;
         break;
      case ROGUE_IO_S1:
         mux._ = IS0_S1;
         break;
      case ROGUE_IO_S2:
         mux._ = IS0_S2;
         break;

      default:
         unreachable("IS0 set to unsupported value.");
      }
   }

   rogue_sbA sbA = { 0 };
   rogue_sA sA = { 0 };

   if (!rogue_ref_is_null(&io_sel->srcs[base + 0])) {
      sbA._ = rogue_reg_bank_encoding(&io_sel->srcs[base + 0]);
      sA._ = rogue_reg_index_encoding(&io_sel->srcs[base + 0]);
   }

   rogue_sbB sbB = { 0 };
   rogue_sB sB = { 0 };

   if (!rogue_ref_is_null(&io_sel->srcs[base + 1])) {
      sbB._ = rogue_reg_bank_encoding(&io_sel->srcs[base + 1]);
      sB._ = rogue_reg_index_encoding(&io_sel->srcs[base + 1]);
   }

   rogue_sbC sbC = { 0 };
   rogue_sC sC = { 0 };

   if (!rogue_ref_is_null(&io_sel->srcs[base + 2])) {
      sbC._ = rogue_reg_bank_encoding(&io_sel->srcs[base + 2]);
      sC._ = rogue_reg_index_encoding(&io_sel->srcs[base + 2]);
   }

   /* Byte 0 is common for all encodings. */
   e->sbA_0 = sbA._0;
   e->sA_5_0 = sA._5_0;

   switch (info->num_srcs) {
   case 1:
      switch (info->bytes) {
      case 3:
         /* Byte 1 */
         assert(!upper_srcs || !mux._1_0);

         e->sA_1.mux_1_0 = mux._1_0;
         e->sA_1.sbA_2_1 = sbA._2_1;
         e->sA_1.sA_7_6 = sA._7_6;

         /* Byte 2 */
         e->sA_2.sA_10_8 = sA._10_8;

         e->ext0 = 1;
         FALLTHROUGH;

      case 1:
         break;

      default:
         unreachable("Unsupported source/bytes combination.");
      }
      break;

   case 2:
      e->ext0 = 1;
      e->sel = 1;
      switch (info->bytes) {
      case 4:
         /* Byte 3 */
         assert(!upper_srcs || !mux._2);

         e->sB_3.sA_10_8 = sA._10_8;
         e->sB_3.mux_2 = mux._2;
         e->sB_3.sbA_2 = sbA._2;
         e->sB_3.sA_7 = sA._7;
         e->sB_3.sB_7 = sB._7;

         e->ext2 = 1;
         FALLTHROUGH;

      case 3:
         /* Byte 2 */
         assert(!upper_srcs || !mux._1_0);

         e->mux_1_0 = mux._1_0;
         e->sbA_1 = sbA._1;
         e->sbB_1 = sbB._1;
         e->sA_6 = sA._6;
         e->sB_6_5 = sB._6_5;

         e->ext1 = 1;
         FALLTHROUGH;

      case 2:
         /* Byte 1 */
         e->sbB_0 = sbB._0;
         e->sB_4_0 = sB._4_0;
         break;

      default:
         unreachable("Unsupported source/bytes combination.");
      }
      break;

   case 3:
      e->ext0 = 1;
      e->ext1 = 1;
      switch (info->bytes) {
      case 6:
         /* Byte 5 */
         assert(!upper_srcs || !sC._10_8);

         e->sC_5.sC_10_8 = sC._10_8;
         e->sC_5.sA_10_8 = sA._10_8;

         e->sC_4.ext4 = 1;
         FALLTHROUGH;

      case 5:
         /* Byte 4 */
         assert(!upper_srcs || !mux._2);
         assert(!upper_srcs || !sbC._2);

         e->sC_4.sbC_2 = sbC._2;
         e->sC_4.sC_7_6 = sC._7_6;
         e->sC_4.mux_2 = mux._2;
         e->sC_4.sbA_2 = sbA._2;
         e->sC_4.sA_7 = sA._7;
         e->sC_4.sB_7 = sB._7;

         e->ext2 = 1;
         FALLTHROUGH;

      case 4:
         /* Byte 1 */
         e->sbB_0 = sbB._0;
         e->sB_4_0 = sB._4_0;

         /* Byte 2 */
         assert(!upper_srcs || !mux._1_0);

         e->mux_1_0 = mux._1_0;
         e->sbA_1 = sbA._1;
         e->sbB_1 = sbB._1;
         e->sA_6 = sA._6;
         e->sB_6_5 = sB._6_5;

         /* Byte 3 */
         e->sbC_1_0 = sbC._1_0;
         e->sC_5_0 = sC._5_0;
         break;

      default:
         unreachable("Unsupported source/bytes combination.");
      }
      break;

   default:
      unreachable("Unsupported source/bytes combination.");
   }
}

static void rogue_encode_dest_map(const rogue_instr_group *group,
                                  rogue_dest_map_encoding *e)
{
   const rogue_reg_dst_info *info =
      &rogue_reg_dst_infos[group->encode_info.dst_index];
   const rogue_instr_group_io_sel *io_sel = &group->io_sel;

   unsigned num_dsts = !rogue_ref_is_null(&io_sel->dsts[0]) +
                       !rogue_ref_is_null(&io_sel->dsts[1]);

   switch (num_dsts) {
   case 1: {
      const rogue_ref *dst_ref = !rogue_ref_is_null(&io_sel->dsts[0])
                                    ? &io_sel->dsts[0]
                                    : &io_sel->dsts[1];

      rogue_dbN dbN = { ._ = rogue_reg_bank_encoding(dst_ref) };
      rogue_dN dN = { ._ = rogue_reg_index_encoding(dst_ref) };

      switch (info->bytes) {
      case 2:
         e->dN_10_8 = dN._10_8;
         e->dbN_2_1 = dbN._2_1;
         e->dN_7_6 = dN._7_6;

         e->ext0 = 1;
         FALLTHROUGH;

      case 1:
         e->dbN_0 = dbN._0;
         e->dN_5_0 = dN._5_0;
         break;

      default:
         unreachable("Unsupported dest/bytes combination.");
      }
      break;
   }
   case 2: {
      rogue_db0 db0 = { ._ = rogue_reg_bank_encoding(&io_sel->dsts[0]) };
      rogue_d0 d0 = { ._ = rogue_reg_index_encoding(&io_sel->dsts[0]) };
      rogue_db1 db1 = { ._ = rogue_reg_bank_encoding(&io_sel->dsts[1]) };
      rogue_d1 d1 = { ._ = rogue_reg_index_encoding(&io_sel->dsts[1]) };

      switch (info->bytes) {
      case 4:
         e->d1_10_8 = d1._10_8;
         e->d0_10_8 = d0._10_8;

         e->ext2 = 1;
         FALLTHROUGH;

      case 3:
         e->db1_2_1 = db1._2_1;
         e->d1_7_6 = d1._7_6;
         e->db0_2_1 = db0._2_1;
         e->d0_7 = d0._7;

         e->ext1 = 1;
         FALLTHROUGH;

      case 2:
         e->db0_0 = db0._0;
         e->d0_6_0 = d0._6_0;

         e->db1_0 = db1._0;
         e->d1_5_0 = d1._5_0;
         break;

      default:
         unreachable("Unsupported dest/bytes combination.");
      }
   } break;

   default:
      unreachable("Unsupported dest/bytes combination.");
   }
}

static void rogue_encode_iss_map(const rogue_instr_group *group,
                                 rogue_iss_encoding *e)
{
   const rogue_instr_group_io_sel *io_sel = &group->io_sel;

   if (rogue_ref_is_io(&io_sel->iss[1]))
      switch (rogue_ref_get_io(&io_sel->iss[1])) {
      case ROGUE_IO_FT0:
         e->is1 = IS1_FT0;
         break;
      case ROGUE_IO_FTE:
         e->is1 = IS1_FTE;
         break;

      default:
         unreachable("Unsupported setting for IS1.");
      }

   if (rogue_ref_is_io(&io_sel->iss[2]))
      switch (rogue_ref_get_io(&io_sel->iss[2])) {
      case ROGUE_IO_FT1:
         e->is2 = IS2_FT1;
         break;
      case ROGUE_IO_FTE:
         e->is2 = IS2_FTE;
         break;

      default:
         unreachable("Unsupported setting for IS2.");
      }

   if (rogue_ref_is_io(&io_sel->iss[3]))
      switch (rogue_ref_get_io(&io_sel->iss[3])) {
      case ROGUE_IO_FT0:
         e->is3 = IS3_FT0;
         break;
      case ROGUE_IO_FT1:
         e->is3 = IS3_FT1;
         break;
      case ROGUE_IO_S2:
         e->is3 = IS3_S2;
         break;
      case ROGUE_IO_FTE:
         e->is3 = IS3_FTE;
         break;

      default:
         unreachable("Unsupported setting for IS3.");
      }

   if (rogue_ref_is_io(&io_sel->iss[4]))
      switch (rogue_ref_get_io(&io_sel->iss[4])) {
      case ROGUE_IO_FT0:
         e->is4 = IS4_FT0;
         break;
      case ROGUE_IO_FT1:
         e->is4 = IS4_FT1;
         break;
      case ROGUE_IO_FT2:
         e->is4 = IS4_FT2;
         break;
      case ROGUE_IO_FTE:
         e->is4 = IS4_FTE;
         break;

      default:
         unreachable("Unsupported setting for IS4.");
      }

   if (rogue_ref_is_io(&io_sel->iss[5]))
      switch (rogue_ref_get_io(&io_sel->iss[5])) {
      case ROGUE_IO_FT0:
         e->is5 = IS5_FT0;
         break;
      case ROGUE_IO_FT1:
         e->is5 = IS5_FT1;
         break;
      case ROGUE_IO_FT2:
         e->is5 = IS5_FT2;
         break;
      case ROGUE_IO_FTE:
         e->is5 = IS5_FTE;
         break;

      default:
         unreachable("Unsupported setting for IS5.");
      }
}

static void rogue_encode_instr_group_io(const rogue_instr_group *group,
                                        struct util_dynarray *binary)
{
   if (group->size.lower_srcs) {
      rogue_source_map_encoding lower_srcs = { 0 };
      rogue_encode_source_map(group, false, &lower_srcs);
      util_dynarray_append_mem(binary, group->size.lower_srcs, &lower_srcs);
   }

   if (group->size.upper_srcs) {
      rogue_source_map_encoding upper_srcs = { 0 };
      rogue_encode_source_map(group, true, &upper_srcs);
      util_dynarray_append_mem(binary, group->size.upper_srcs, &upper_srcs);
   }

   if (group->size.iss) {
      rogue_iss_encoding internal_src_sel = { 0 };
      rogue_encode_iss_map(group, &internal_src_sel);
      util_dynarray_append_mem(binary, group->size.iss, &internal_src_sel);
   }

   if (group->size.dsts) {
      rogue_dest_map_encoding dests = { 0 };
      rogue_encode_dest_map(group, &dests);
      util_dynarray_append_mem(binary, group->size.dsts, &dests);
   }
}

static void rogue_encode_instr_group_padding(const rogue_instr_group *group,
                                             struct util_dynarray *binary)
{
   if (group->size.word_padding)
      util_dynarray_append(binary, uint8_t, 0xff);

   if (group->size.align_padding) {
      assert(!(group->size.align_padding % 2));
      unsigned align_words = group->size.align_padding / 2;
      util_dynarray_append(binary, uint8_t, 0xf0 | align_words);
      for (unsigned u = 0; u < group->size.align_padding - 1; ++u)
         util_dynarray_append(binary, uint8_t, 0xff);
   }
}

static void rogue_encode_instr_group(rogue_instr_group *group,
                                     struct util_dynarray *binary)
{
   rogue_encode_instr_group_header(group, binary);
   rogue_encode_instr_group_instrs(group, binary);
   rogue_encode_instr_group_io(group, binary);
   rogue_encode_instr_group_padding(group, binary);
}

static void rogue_dump_shader_binary(rogue_shader *shader,
                                     struct util_dynarray *binary)
{
   static unsigned index = 0;
   char filename[PATH_MAX];
   sprintf(filename,
           "%u_%s.bin",
           index++,
           shader->stage == MESA_SHADER_NONE
              ? "uscprog"
              : _mesa_shader_stage_to_string(shader->stage));

   FILE *fp = fopen(filename, "wb");
   assert(fp);
   ASSERTED size_t elems_written =
      fwrite(util_dynarray_begin(binary),
             sizeof(uint8_t),
             util_dynarray_num_elements(binary, uint8_t),
             fp);
   assert(elems_written == util_dynarray_num_elements(binary, uint8_t));
   fclose(fp);
}

PUBLIC
void rogue_encode_shader(rogue_build_ctx *ctx,
                         rogue_shader *shader,
                         struct util_dynarray *binary)
{
   if (!shader->is_grouped)
      unreachable("Can't encode shader with ungrouped instructions.");

   util_dynarray_init(binary, ctx);

   rogue_foreach_instr_group_in_shader (group, shader)
      rogue_encode_instr_group(group, binary);

   if (ROGUE_DEBUG(DUMP_BINARY))
      rogue_dump_shader_binary(shader, binary);
}
