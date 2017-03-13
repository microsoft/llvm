//===-- GraphColor/TargetRegisterAllocInfo.h --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Tiled register allocator target interface
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_TARGETREGISTERALLOCINFO_H
#define TILED_GRAPHCOLOR_TARGETREGISTERALLOCINFO_H

#include "../Cost.h"
#include "Graph.h"

#include "llvm/ADT/SparseBitVector.h"

namespace llvm
{
   class MachineFunction;
   class MachineBasicBlock;
   class MachineInstr;
   class MachineOperand;
   class TargetRegisterClass;
}

namespace Tiled
{

namespace VR
{
   class Info;
}

namespace RegisterAllocator
{
namespace GraphColor
{

class TargetRegisterAllocInfo
{
public:

   TargetRegisterAllocInfo
   (
      Tiled::VR::Info *       vri,
      llvm::MachineFunction * funcu
   );

   void
   PreferenceInstruction
   (
      llvm::MachineInstr *           instruction,
      GraphColor::PreferenceVector * preferenceVector
   );

   void
   Spill
   (
      llvm::MachineOperand * registerOperand,
      unsigned               srcReg,
      llvm::MachineOperand * spillOperand,
      llvm::MachineInstr *   insertAfterInstruction,
      bool                   isFinal,
      bool                   isTileSpill,
      bool                   isGlobal
   );

   void
   Reload
   (
      llvm::MachineOperand * registerOperand,
      unsigned               destReg,
      llvm::MachineOperand * reloadOperand,
      llvm::MachineInstr *   insertBeforeInstruction,
      bool                   isFinal,
      bool                   isTileReload,
      bool                   isGlobal
   );

   Cost
   Fold
   (
      llvm::MachineOperand * availableOperand,
      llvm::MachineOperand * scratchOperand
   );

   const llvm::TargetRegisterClass *
   GetBaseIntegerRegisterCategory();

   const llvm::TargetRegisterClass *
   GetBaseFloatRegisterCategory();

   void
   InitializeFunction();

   void
   InitializeCosts();

   void
   InitializeRegisterVectorCosts
   (
      GraphColor::CostVector * registerCostVector
   );

   unsigned
   CalleeSaveConservativeThreshold();

   unsigned
   EdgeProbabilityInstructionShareStopIteration();

   unsigned
   CrossCallInstructionShareStopIteration();

   unsigned
   InitialInstructionShareLimit();

   unsigned
   ConstrainedInstructionShareStartIteration();

   unsigned
   CallerSaveIterationLimit();

   unsigned
   IntegerPressureAdjustment();

   unsigned
   FloatPressureAdjustment();

   double
   InstructionShareDecayRateValue();

   Tiled::Cost
   IntegerMoveCost();

   Tiled::Cost
   EstimateInstructionCost
   (
      llvm::MachineInstr * instruction
   );

   Tiled::Cost
   EstimateMemoryCost
   (
      llvm::MachineOperand * /*TBD: a MemoryOperand type ?*/ foldOperand
   );

   bool
   IsLargeBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   CanReuseSpillSymbol
   (
      Graphs::FrameIndex frameSlot,  //was: Symbol
      bool               isGlobal,
      bool               mayBeParameter
   );
   //in Arm target it was defined as: [thunk("(!symbol->AsVariableSymbol->IsParameter)")]

   bool
   IsLegalToReplaceOperand
   (
      llvm::MachineOperand *     originalOperand,
      llvm::MachineOperand *     replaceOperand
   );

   bool
   CanCallerSaveSpill
   (
      llvm::MachineFunction * functionUnit,
      unsigned                reg
      // there was a Type argument
   );  // now return true

   bool
   IsCalleeSaveCategory
   (
      llvm::MachineFunction *     functionUnit,
      llvm::TargetRegisterClass * registerCategory
   )
   {
      // For an architecture with single register kind and no sub-registers always true
      return true;
   }

   void
   addRC
   (
      const llvm::TargetRegisterClass *RC
   );

   const llvm::TargetRegisterClass *
   GetRegisterCategory
   (
      unsigned Reg
   );

public:
   
   std::vector<unsigned> *              RegisterCategoryIdToAllocatableRegisterCount;
   GraphColor::SparseBitVectorVector *  RegisterCategoryIdToAllocatableAliasTagBitVector;
   unsigned                             MaximumRegisterCategoryId;
   unsigned                             MaximumRegisterId;
   llvm::SparseBitVector<> *            DoNotAllocateRegisterAliasTagBitVector;
   llvm::SparseBitVector<> *            CalleeSaveRegisterAliasTagSet;
   
private:

   Tiled::VR::Info *                 vrInfo;
   llvm::MachineFunction *           MF;
   const llvm::TargetRegisterInfo *  TRI;
   const llvm::TargetInstrInfo *     TII;
   Tiled::Cost                       integerMoveCost;

};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_TARGETREGISTERALLOCINFO_H
