//===- subzero/src/IceAssemblerARM32.h - Assembler for ARM32 ----*- C++ -*-===//
//
// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Modified by the Subzero authors.
//
//===----------------------------------------------------------------------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the Assembler class for ARM32.
///
/// Note: All references to ARM "section" documentation refers to the "ARM
/// Architecture Reference Manual, ARMv7-A and ARMv7-R edition". See:
/// http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0406c
///
//===----------------------------------------------------------------------===//

#ifndef SUBZERO_SRC_ICEASSEMBLERARM32_H
#define SUBZERO_SRC_ICEASSEMBLERARM32_H

#include "IceAssembler.h"
#include "IceConditionCodesARM32.h"
#include "IceDefs.h"
#include "IceFixups.h"
#include "IceInstARM32.h"
#include "IceRegistersARM32.h"
#include "IceTargetLowering.h"

namespace Ice {
namespace ARM32 {

/// Encoding of an ARM 32-bit instruction.
using IValueT = uint32_t;

/// An Offset value (+/-) used in an ARM 32-bit instruction.
using IOffsetT = int32_t;

/// Handles encoding of bottom/top 16 bits of an address using movw/movt.
class MoveRelocatableFixup : public AssemblerFixup {
  MoveRelocatableFixup &operator=(const MoveRelocatableFixup &) = delete;
  MoveRelocatableFixup(const MoveRelocatableFixup &) = default;

public:
  MoveRelocatableFixup() = default;
  size_t emit(GlobalContext *Ctx, const Assembler &Asm) const override;
};

class AssemblerARM32 : public Assembler {
  AssemblerARM32(const AssemblerARM32 &) = delete;
  AssemblerARM32 &operator=(const AssemblerARM32 &) = delete;

public:
  // Rotation values.
  enum RotationValue {
    kRotateNone, // Omitted
    kRotate8,    // ror #8
    kRotate16,   // ror #16
    kRotate24    // ror #24
  };

  class TargetInfo {
    TargetInfo(const TargetInfo &) = delete;
    TargetInfo &operator=(const TargetInfo &) = delete;

  public:
    TargetInfo(bool HasFramePointer, SizeT FrameOrStackReg,
               int32_t StackAdjustment)
        : HasFramePointer(HasFramePointer), FrameOrStackReg(FrameOrStackReg),
          StackAdjustment(StackAdjustment) {}
    explicit TargetInfo(const TargetLowering *Target)
        : HasFramePointer(Target->hasFramePointer()),
          FrameOrStackReg(Target->getFrameOrStackReg()),
          StackAdjustment(Target->getStackAdjustment()) {}
    const bool HasFramePointer;
    const SizeT FrameOrStackReg;
    const int32_t StackAdjustment;
  };

  explicit AssemblerARM32(bool use_far_branches = false)
      : Assembler(Asm_ARM32) {
    // TODO(kschimpf): Add mode if needed when branches are handled.
    (void)use_far_branches;
  }
  ~AssemblerARM32() override {
    if (BuildDefs::asserts()) {
      for (const Label *Label : CfgNodeLabels) {
        Label->finalCheck();
      }
      for (const Label *Label : LocalLabels) {
        Label->finalCheck();
      }
    }
  }

  MoveRelocatableFixup *createMoveFixup(bool IsMovW, const Constant *Value);

  void alignFunction() override {
    const SizeT Align = 1 << getBundleAlignLog2Bytes();
    SizeT BytesNeeded = Utils::OffsetToAlignment(Buffer.getPosition(), Align);
    constexpr IValueT UndefinedInst = 0xe7fedef0; // udf #60896
    constexpr SizeT InstSize = sizeof(IValueT);
    assert(BytesNeeded % InstARM32::InstSize == 0);
    while (BytesNeeded > 0) {
      AssemblerBuffer::EnsureCapacity ensured(&Buffer);
      emitInst(UndefinedInst);
      BytesNeeded -= InstSize;
    }
  }

  SizeT getBundleAlignLog2Bytes() const override { return 4; }

  const char *getAlignDirective() const override { return ".p2alignl"; }

  llvm::ArrayRef<uint8_t> getNonExecBundlePadding() const override {
    // Use a particular UDF encoding -- TRAPNaCl in LLVM: 0xE7FEDEF0
    // http://llvm.org/viewvc/llvm-project?view=revision&revision=173943
    static const uint8_t Padding[] = {0xE7, 0xFE, 0xDE, 0xF0};
    return llvm::ArrayRef<uint8_t>(Padding, 4);
  }

  void padWithNop(intptr_t Padding) override {
    (void)Padding;
    llvm_unreachable("Not yet implemented.");
  }

  Ice::Label *getCfgNodeLabel(SizeT NodeNumber) override {
    assert(NodeNumber < CfgNodeLabels.size());
    return CfgNodeLabels[NodeNumber];
  }

  Label *getOrCreateCfgNodeLabel(SizeT NodeNumber) {
    return getOrCreateLabel(NodeNumber, CfgNodeLabels);
  }

  Label *getOrCreateLocalLabel(SizeT Number) {
    return getOrCreateLabel(Number, LocalLabels);
  }

  void bindLocalLabel(const Cfg *Func, const InstARM32Label *InstL,
                      SizeT Number) {
    if (BuildDefs::dump() &&
        !Func->getContext()->getFlags().getDisableHybridAssembly()) {
      constexpr SizeT InstSize = 0;
      emitTextInst(InstL->getName(Func) + ":", InstSize);
    }
    Label *L = getOrCreateLocalLabel(Number);
    if (!getPreliminary())
      this->bind(L);
  }

  bool fixupIsPCRel(FixupKind Kind) const override {
    (void)Kind;
    // TODO(kschimpf) Decide if we need this.
    return false;
  }

  /// Accessors to keep track of the number of bytes generated inside
  /// InstARM32::emit() methods, when run inside of
  /// InstARM32::emitUsingTextFixup().
  void resetEmitTextSize() { EmitTextSize = 0; }
  void incEmitTextSize(size_t Amount) { EmitTextSize += Amount; }
  void decEmitTextSize(size_t Amount) { EmitTextSize -= Amount; }
  size_t getEmitTextSize() const { return EmitTextSize; }

  void bind(Label *label);

  // List of instructions implemented by integrated assembler.

  void adc(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void add(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void and_(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
            bool SetFlags, CondARM32::Cond Cond);

  void b(Label *L, CondARM32::Cond Cond);

  void bx(RegARM32::GPRRegister Rm, CondARM32::Cond Cond = CondARM32::AL);

  void bkpt(uint16_t Imm16);

  void cmp(const Operand *OpRn, const Operand *OpSrc1, CondARM32::Cond Cond);

  void bic(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void eor(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void ldr(const Operand *OpRt, const Operand *OpAddress, CondARM32::Cond Cond,
           const TargetInfo &TInfo);

  void ldr(const Operand *OpRt, const Operand *OpAddress, CondARM32::Cond Cond,
           const TargetLowering *Lowering) {
    const TargetInfo TInfo(Lowering);
    ldr(OpRt, OpAddress, Cond, TInfo);
  }

  void mov(const Operand *OpRd, const Operand *OpSrc, CondARM32::Cond Cond);

  void movw(const Operand *OpRd, const Operand *OpSrc, CondARM32::Cond Cond);

  void movt(const Operand *OpRd, const Operand *OpSrc, CondARM32::Cond Cond);

  void mla(const Operand *OpRd, const Operand *OpRn, const Operand *OpRm,
           const Operand *OpRa, CondARM32::Cond Cond);

  void mul(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void orr(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void pop(const Operand *OpRt, CondARM32::Cond Cond);

  // Note: Registers is a bitset, where bit n corresponds to register Rn.
  void popList(const IValueT Registers, CondARM32::Cond Cond);

  void push(const Operand *OpRt, CondARM32::Cond Cond);

  // Note: Registers is a bitset, where bit n corresponds to register Rn.
  void pushList(const IValueT Registers, CondARM32::Cond Cond);

  void sbc(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void sdiv(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
            CondARM32::Cond Cond);

  void str(const Operand *OpRt, const Operand *OpAddress, CondARM32::Cond Cond,
           const TargetInfo &TInfo);

  void str(const Operand *OpRt, const Operand *OpAddress, CondARM32::Cond Cond,
           const TargetLowering *Lowering) {
    const TargetInfo TInfo(Lowering);
    str(OpRt, OpAddress, Cond, TInfo);
  }

  void sub(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
           bool SetFlags, CondARM32::Cond Cond);

  void tst(const Operand *OpRn, const Operand *OpSrc1, CondARM32::Cond Cond);

  void udiv(const Operand *OpRd, const Operand *OpRn, const Operand *OpSrc1,
            CondARM32::Cond Cond);

  void umull(const Operand *OpRdLo, const Operand *OpRdHi, const Operand *OpRn,
             const Operand *OpRm, CondARM32::Cond Cond);

  // Implements uxtb/uxth depending on type of OpSrc0.
  void uxt(const Operand *OpRd, const Operand *OpSrc0, CondARM32::Cond Cond);

  static bool classof(const Assembler *Asm) {
    return Asm->getKind() == Asm_ARM32;
  }

  void emitTextInst(const std::string &Text, SizeT InstSize);

private:
  // A vector of pool-allocated x86 labels for CFG nodes.
  using LabelVector = std::vector<Label *>;
  LabelVector CfgNodeLabels;
  // A vector of pool-allocated x86 labels for Local labels.
  LabelVector LocalLabels;
  // Number of bytes emitted by InstARM32::emit() methods, when run inside
  // InstARM32::emitUsingTextFixup().
  size_t EmitTextSize = 0;

  // Load/store multiple addressing mode.
  enum BlockAddressMode {
    // bit encoding P U W
    DA = (0 | 0 | 0) << 21,   // decrement after
    IA = (0 | 4 | 0) << 21,   // increment after
    DB = (8 | 0 | 0) << 21,   // decrement before
    IB = (8 | 4 | 0) << 21,   // increment before
    DA_W = (0 | 0 | 1) << 21, // decrement after with writeback to base
    IA_W = (0 | 4 | 1) << 21, // increment after with writeback to base
    DB_W = (8 | 0 | 1) << 21, // decrement before with writeback to base
    IB_W = (8 | 4 | 1) << 21  // increment before with writeback to base
  };

  Label *getOrCreateLabel(SizeT Number, LabelVector &Labels);

  void bindCfgNodeLabel(const CfgNode *Node) override;

  void emitInst(IValueT Value) { Buffer.emit<IValueT>(Value); }

  // Pattern cccctttoooosnnnnddddiiiiiiiiiiii where cccc=Cond, ttt=Type,
  // oooo=Opcode, nnnn=Rn, dddd=Rd, iiiiiiiiiiii=imm12 (See ARM section A5.2.3).
  void emitType01(CondARM32::Cond Cond, IValueT Type, IValueT Opcode,
                  bool SetCc, IValueT Rn, IValueT Rd, IValueT imm12);

  // List of possible checks to apply when calling emitType01() (below).
  enum Type01Checks {
    NoChecks,
    RdIsPcAndSetFlags,
  };

  // Converts appropriate representation on a data operation, and then calls
  // emitType01 above.
  void emitType01(IValueT Opcode, const Operand *OpRd, const Operand *OpRn,
                  const Operand *OpSrc1, bool SetFlags, CondARM32::Cond Cond,
                  Type01Checks RuleChecks = RdIsPcAndSetFlags);

  // Same as above, but the value for Rd and Rn have already been converted
  // into instruction values.
  void emitType01(IValueT Opcode, IValueT OpRd, IValueT OpRn,
                  const Operand *OpSrc1, bool SetFlags, CondARM32::Cond Cond,
                  Type01Checks RuleChecks = RdIsPcAndSetFlags);

  void emitType05(CondARM32::Cond COnd, int32_t Offset, bool Link);

  // Pattern ccccoooaabalnnnnttttaaaaaaaaaaaa where cccc=Cond, ooo=InstType,
  // l=isLoad, b=isByte, and aaa0a0aaaa0000aaaaaaaaaaaa=Address. Note that
  // Address is assumed to be defined by decodeAddress() in
  // IceAssemblerARM32.cpp.
  void emitMemOp(CondARM32::Cond Cond, IValueT InstType, bool IsLoad,
                 bool IsByte, uint32_t Rt, uint32_t Address);

  // Pattern cccc100aaaalnnnnrrrrrrrrrrrrrrrr where cccc=Cond,
  // aaaa<<21=AddressMode, l=IsLoad, nnnn=BaseReg, and
  // rrrrrrrrrrrrrrrr is bitset of Registers.
  void emitMultiMemOp(CondARM32::Cond Cond, BlockAddressMode AddressMode,
                      bool IsLoad, IValueT BaseReg, IValueT Registers);

  // Pattern cccc011100x1dddd1111mmmm0001nnn where cccc=Cond,
  // x=Opcode, dddd=Rd, nnnn=Rn, mmmm=Rm.
  void emitDivOp(CondARM32::Cond Cond, IValueT Opcode, IValueT Rd, IValueT Rn,
                 IValueT Rm);

  // Pattern ccccxxxxxxxfnnnnddddssss1001mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, ssss=Rs, f=SetCc, and xxxxxxx=Opcode.
  void emitMulOp(CondARM32::Cond Cond, IValueT Opcode, IValueT Rd, IValueT Rn,
                 IValueT Rm, IValueT Rs, bool SetCc);

  // Implements various forms of Unsigned extend value, using pattern
  // ccccxxxxxxxxnnnnddddrr000111mmmm where cccc=Cond, xxxxxxxx<<20=Opcode,
  // nnnn=Rn, dddd=Rd, rr=Rotation, and mmmm=Rm.
  void emitUxt(CondARM32::Cond, IValueT Opcode, IValueT Rd, IValueT Rn,
               IValueT Rm, RotationValue Rotation);

  // Pattern cccctttxxxxnnnn0000iiiiiiiiiiii where cccc=Cond, nnnn=Rn,
  // ttt=Instruction type (derived from OpSrc1), iiiiiiiiiiii is derived from
  // OpSrc1, and xxxx=Opcode.
  void emitCompareOp(IValueT Opcode, const Operand *OpRn, const Operand *OpSrc1,
                     CondARM32::Cond Cond);

  void emitBranch(Label *L, CondARM32::Cond, bool Link);

  // Encodes the given Offset into the branch instruction Inst.
  IValueT encodeBranchOffset(IOffsetT Offset, IValueT Inst);

  // Returns the offset encoded in the branch instruction Inst.
  static IOffsetT decodeBranchOffset(IValueT Inst);

  // Implements movw, generating pattern ccccxxxxxxxsiiiiddddiiiiiiiiiiii where
  // cccc=Cond, xxxxxxx<<21=Opcode, dddd=Rd, s=SetFlags, and
  // iiiiiiiiiiiiiiii=Imm16.
  void emitMovw(IValueT Opcode, IValueT Rd, IValueT Imm16, bool SetFlags,
                CondARM32::Cond Cond);
};

} // end of namespace ARM32
} // end of namespace Ice

#endif // SUBZERO_SRC_ICEASSEMBLERARM32_H
