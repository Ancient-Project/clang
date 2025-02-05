//== llvm/CodeGen/GlobalISel/LegalizerHelper.h ---------------- -*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file A pass to convert the target-illegal operations created by IR -> MIR
/// translation into ones the target expects to be able to select. This may
/// occur in multiple phases, for example G_ADD <2 x i8> -> G_ADD <2 x i16> ->
/// G_ADD <4 x i16>.
///
/// The LegalizerHelper class is where most of the work happens, and is
/// designed to be callable from other passes that find themselves with an
/// illegal instruction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_MACHINELEGALIZEHELPER_H
#define LLVM_CODEGEN_GLOBALISEL_MACHINELEGALIZEHELPER_H

#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/LowLevelType.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RuntimeLibcalls.h"

namespace llvm {
// Forward declarations.
class LegalizerInfo;
class Legalizer;
class MachineRegisterInfo;
class GISelChangeObserver;

class LegalizerHelper {
public:
  enum LegalizeResult {
    /// Instruction was already legal and no change was made to the
    /// MachineFunction.
    AlreadyLegal,

    /// Instruction has been legalized and the MachineFunction changed.
    Legalized,

    /// Some kind of error has occurred and we could not legalize this
    /// instruction.
    UnableToLegalize,
  };

  LegalizerHelper(MachineFunction &MF, GISelChangeObserver &Observer,
                  MachineIRBuilder &B);
  LegalizerHelper(MachineFunction &MF, const LegalizerInfo &LI,
                  GISelChangeObserver &Observer, MachineIRBuilder &B);

  /// Replace \p MI by a sequence of legal instructions that can implement the
  /// same operation. Note that this means \p MI may be deleted, so any iterator
  /// steps should be performed before calling this function. \p Helper should
  /// be initialized to the MachineFunction containing \p MI.
  ///
  /// Considered as an opaque blob, the legal code will use and define the same
  /// registers as \p MI.
  LegalizeResult legalizeInstrStep(MachineInstr &MI);

  /// Legalize an instruction by emiting a runtime library call instead.
  LegalizeResult libcall(MachineInstr &MI);

  /// Legalize an instruction by reducing the width of the underlying scalar
  /// type.
  LegalizeResult narrowScalar(MachineInstr &MI, unsigned TypeIdx, LLT NarrowTy);

  /// Legalize an instruction by performing the operation on a wider scalar type
  /// (for example a 16-bit addition can be safely performed at 32-bits
  /// precision, ignoring the unused bits).
  LegalizeResult widenScalar(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);

  /// Legalize an instruction by splitting it into simpler parts, hopefully
  /// understood by the target.
  LegalizeResult lower(MachineInstr &MI, unsigned TypeIdx, LLT Ty);

  /// Legalize a vector instruction by splitting into multiple components, each
  /// acting on the same scalar type as the original but with fewer elements.
  LegalizeResult fewerElementsVector(MachineInstr &MI, unsigned TypeIdx,
                                     LLT NarrowTy);

  /// Legalize a vector instruction by increasing the number of vector elements
  /// involved and ignoring the added elements later.
  LegalizeResult moreElementsVector(MachineInstr &MI, unsigned TypeIdx,
                                    LLT MoreTy);

  /// Expose MIRBuilder so clients can set their own RecordInsertInstruction
  /// functions
  MachineIRBuilder &MIRBuilder;

  /// Expose LegalizerInfo so the clients can re-use.
  const LegalizerInfo &getLegalizerInfo() const { return LI; }

private:
  /// Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  /// Use by extending the operand's type to \p WideTy using the specified \p
  /// ExtOpcode for the extension instruction, and replacing the vreg of the
  /// operand in place.
  void widenScalarSrc(MachineInstr &MI, LLT WideTy, unsigned OpIdx,
                      unsigned ExtOpcode);

  /// Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  /// Use by truncating the operand's type to \p NarrowTy using G_TRUNC, and
  /// replacing the vreg of the operand in place.
  void narrowScalarSrc(MachineInstr &MI, LLT NarrowTy, unsigned OpIdx);

  /// Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  /// Def by extending the operand's type to \p WideTy and truncating it back
  /// with the \p TruncOpcode, and replacing the vreg of the operand in place.
  void widenScalarDst(MachineInstr &MI, LLT WideTy, unsigned OpIdx = 0,
                      unsigned TruncOpcode = TargetOpcode::G_TRUNC);

  // Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  // Def by truncating the operand's type to \p NarrowTy, replacing in place and
  // extending back with \p ExtOpcode.
  void narrowScalarDst(MachineInstr &MI, LLT NarrowTy, unsigned OpIdx,
                       unsigned ExtOpcode);
  /// Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  /// Def by performing it with additional vector elements and extracting the
  /// result elements, and replacing the vreg of the operand in place.
  void moreElementsVectorDst(MachineInstr &MI, LLT MoreTy, unsigned OpIdx);

  /// Legalize a single operand \p OpIdx of the machine instruction \p MI as a
  /// Use by producing a vector with undefined high elements, extracting the
  /// original vector type, and replacing the vreg of the operand in place.
  void moreElementsVectorSrc(MachineInstr &MI, LLT MoreTy, unsigned OpIdx);

  LegalizeResult
  widenScalarMergeValues(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);
  LegalizeResult
  widenScalarUnmergeValues(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);
  LegalizeResult
  widenScalarExtract(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);
  LegalizeResult
  widenScalarInsert(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);

  /// Helper function to split a wide generic register into bitwise blocks with
  /// the given Type (which implies the number of blocks needed). The generic
  /// registers created are appended to Ops, starting at bit 0 of Reg.
  void extractParts(Register Reg, LLT Ty, int NumParts,
                    SmallVectorImpl<Register> &VRegs);

  /// Version which handles irregular splits.
  bool extractParts(Register Reg, LLT RegTy, LLT MainTy,
                    LLT &LeftoverTy,
                    SmallVectorImpl<Register> &VRegs,
                    SmallVectorImpl<Register> &LeftoverVRegs);

  /// Helper function to build a wide generic register \p DstReg of type \p
  /// RegTy from smaller parts. This will produce a G_MERGE_VALUES,
  /// G_BUILD_VECTOR, G_CONCAT_VECTORS, or sequence of G_INSERT as appropriate
  /// for the types.
  ///
  /// \p PartRegs must be registers of type \p PartTy.
  ///
  /// If \p ResultTy does not evenly break into \p PartTy sized pieces, the
  /// remainder must be specified with \p LeftoverRegs of type \p LeftoverTy.
  void insertParts(Register DstReg, LLT ResultTy,
                   LLT PartTy, ArrayRef<Register> PartRegs,
                   LLT LeftoverTy = LLT(), ArrayRef<Register> LeftoverRegs = {});

  /// Perform generic multiplication of values held in multiple registers.
  /// Generated instructions use only types NarrowTy and i1.
  /// Destination can be same or two times size of the source.
  void multiplyRegisters(SmallVectorImpl<Register> &DstRegs,
                         ArrayRef<Register> Src1Regs,
                         ArrayRef<Register> Src2Regs, LLT NarrowTy);

public:
  LegalizeResult fewerElementsVectorImplicitDef(MachineInstr &MI,
                                                unsigned TypeIdx, LLT NarrowTy);

  /// Legalize a simple vector instruction where all operands are the same type
  /// by splitting into multiple components.
  LegalizeResult fewerElementsVectorBasic(MachineInstr &MI, unsigned TypeIdx,
                                          LLT NarrowTy);

  /// Legalize a instruction with a vector type where each operand may have a
  /// different element type. All type indexes must have the same number of
  /// elements.
  LegalizeResult fewerElementsVectorMultiEltType(MachineInstr &MI,
                                                 unsigned TypeIdx, LLT NarrowTy);

  LegalizeResult fewerElementsVectorCasts(MachineInstr &MI, unsigned TypeIdx,
                                          LLT NarrowTy);

  LegalizeResult
  fewerElementsVectorCmp(MachineInstr &MI, unsigned TypeIdx, LLT NarrowTy);

  LegalizeResult
  fewerElementsVectorSelect(MachineInstr &MI, unsigned TypeIdx, LLT NarrowTy);

  LegalizeResult fewerElementsVectorPhi(MachineInstr &MI,
                                        unsigned TypeIdx, LLT NarrowTy);

  LegalizeResult moreElementsVectorPhi(MachineInstr &MI, unsigned TypeIdx,
                                       LLT MoreTy);

  LegalizeResult fewerElementsVectorUnmergeValues(MachineInstr &MI,
                                                  unsigned TypeIdx,
                                                  LLT NarrowTy);
  LegalizeResult
  reduceLoadStoreWidth(MachineInstr &MI, unsigned TypeIdx, LLT NarrowTy);

  LegalizeResult narrowScalarShiftByConstant(MachineInstr &MI, const APInt &Amt,
                                             LLT HalfTy, LLT ShiftAmtTy);

  LegalizeResult narrowScalarShift(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult narrowScalarMul(MachineInstr &MI, LLT Ty);
  LegalizeResult narrowScalarExtract(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult narrowScalarInsert(MachineInstr &MI, unsigned TypeIdx, LLT Ty);

  LegalizeResult narrowScalarBasic(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult narrowScalarSelect(MachineInstr &MI, unsigned TypeIdx, LLT Ty);

  LegalizeResult lowerBitCount(MachineInstr &MI, unsigned TypeIdx, LLT Ty);

  LegalizeResult lowerU64ToF32BitOps(MachineInstr &MI);
  LegalizeResult lowerUITOFP(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult lowerSITOFP(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult lowerMinMax(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult lowerFCopySign(MachineInstr &MI, unsigned TypeIdx, LLT Ty);
  LegalizeResult lowerFMinNumMaxNum(MachineInstr &MI);
  LegalizeResult lowerUnmergeValues(MachineInstr &MI);
  LegalizeResult lowerShuffleVector(MachineInstr &MI);
  LegalizeResult lowerDynStackAlloc(MachineInstr &MI);

private:
  MachineRegisterInfo &MRI;
  const LegalizerInfo &LI;
  /// To keep track of changes made by the LegalizerHelper.
  GISelChangeObserver &Observer;
};

/// Helper function that creates the given libcall.
LegalizerHelper::LegalizeResult
createLibcall(MachineIRBuilder &MIRBuilder, RTLIB::Libcall Libcall,
              const CallLowering::ArgInfo &Result,
              ArrayRef<CallLowering::ArgInfo> Args);

/// Create a libcall to memcpy et al.
LegalizerHelper::LegalizeResult createMemLibcall(MachineIRBuilder &MIRBuilder,
                                                 MachineRegisterInfo &MRI,
                                                 MachineInstr &MI);

} // End namespace llvm.

#endif
