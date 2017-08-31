//===-- GraphColor/TargetRegisterAllocInfo.cpp ------------------*- C++ -*-===//
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

#include "../Cost.h"
#include "../Graphs/Graph.h"
#include "Preference.h"
#include "TargetRegisterAllocInfo.h"

#include "llvm/ADT/Triple.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Target/TargetLowering.h"

#define DEBUG_TYPE "tiled-trai"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

TargetRegisterAllocInfo::TargetRegisterAllocInfo
(
   Tiled::VR::Info *       vri,
   llvm::MachineFunction * mf
) : vrInfo(vri), MF(mf), TRI(MF->getSubtarget().getRegisterInfo()),
    TII(MF->getSubtarget().getInstrInfo())
{
   const llvm::TargetRegisterInfo * const targetRegInfo = mf->getSubtarget().getRegisterInfo();

   this->MaximumRegisterCategoryId = 0;
   llvm::TargetRegisterInfo::regclass_iterator beginRegClass = targetRegInfo->regclass_begin();
   llvm::TargetRegisterInfo::regclass_iterator endRegClass = targetRegInfo->regclass_end();
   for (llvm::TargetRegisterInfo::regclass_iterator rc = beginRegClass; rc != endRegClass; ++rc)
   {
      this->MaximumRegisterCategoryId = std::max(this->MaximumRegisterCategoryId, (*rc)->getID());
   }

   this->MaximumRegisterId = targetRegInfo->getNumRegs();

   this->RegisterCategoryIdToAllocatableRegisterCount =
      new std::vector<unsigned>(this->MaximumRegisterCategoryId + 1, 0);

   this->RegisterCategoryIdToAllocatableAliasTagBitVector =
      new GraphColor::SparseBitVectorVector(this->MaximumRegisterCategoryId + 1, nullptr);


   // Allocatable registers: add for each used RC based on used virtual registers.
   unsigned numVirtualRegs = mf->getRegInfo().getNumVirtRegs();

   for (unsigned i = 0, e = numVirtualRegs; i != e; i++) {
      unsigned Reg = this->TRI->index2VirtReg(i);
      const llvm::TargetRegisterClass *rc = GetRegisterCategory(Reg);
      if ((*this->RegisterCategoryIdToAllocatableAliasTagBitVector)[rc->getID()] != nullptr)
         continue;

      addRC(rc);
   }

   // Reserved registers
   llvm::SparseBitVector<> * doNotAllocateRegisterAliasTagBitVector = new llvm::SparseBitVector<>();
   //This should include Frame Register (FP) unless this MF is "frameless", and also the scratch register (if any).
   llvm::BitVector reservedBitVector(this->TRI->getReservedRegs(*(this->MF)));
   for (int reg = reservedBitVector.find_first(); reg != -1; reg = reservedBitVector.find_next(reg))
   {
      unsigned tag = vrInfo->GetTag(reg);
      doNotAllocateRegisterAliasTagBitVector->set(tag);
   }

   this->DoNotAllocateRegisterAliasTagBitVector = doNotAllocateRegisterAliasTagBitVector;

   llvm::SparseBitVector<> * calleeSaveRegisterAliasTagSet = new llvm::SparseBitVector<>();
   const llvm::MCPhysReg *   calleeSavedRegs = nullptr;

   for (calleeSavedRegs = this->TRI->getCalleeSavedRegs(this->MF); *calleeSavedRegs != 0; ++calleeSavedRegs)
   {
      unsigned reg = *calleeSavedRegs;
      unsigned tag = vrInfo->GetTag(reg);
      calleeSaveRegisterAliasTagSet->set(tag);
   }
   this->CalleeSaveRegisterAliasTagSet = calleeSaveRegisterAliasTagSet;

   this->integerMoveCost.Initialize(1, 2);
}

void
TargetRegisterAllocInfo::Spill
(
   llvm::MachineOperand * registerOperand,
   unsigned               srcReg,
   llvm::MachineOperand * spillOperand,
   llvm::MachineInstr *   insertAfterInstruction,
   bool                   isFinal,
   bool                   isTileSpill,
   bool                   isGlobal
)
{
   // note that the last 3 arguments are currently not used
   DEBUG(llvm::dbgs() << "MBB#" << insertAfterInstruction->getParent()->getNumber() << "  ");
   DEBUG(llvm::dbgs() << "Spill:  vreg" << (registerOperand->getReg() & ~(1u << 31)) << "  slot=" << spillOperand->getIndex() << "\n");

   llvm::MachineBasicBlock *         MBB = insertAfterInstruction->getParent();
   const llvm::TargetRegisterClass * RC;
   if (TRI->isVirtualRegister(srcReg))
      RC = MF->getRegInfo().getRegClass(srcReg);
   else
      RC = TRI->getLargestLegalSuperClass(TRI->getMinimalPhysRegClass(srcReg), *MF);

   llvm::MachineBasicBlock::iterator MII;
   if (insertAfterInstruction->getNextNode()) {
      MII = insertAfterInstruction->getNextNode();
        // storeRegToStackSlot() inserts the new instruction *before* the MII argument
   } else {
      MII = MBB->instr_end();
   }

   int frameIndex = spillOperand->getIndex();
   if (MF->getFrameInfo().getObjectSize(frameIndex) < this->TRI->getRegSizeInBits(*RC)) {
      // The default slot size for spilling is 64bits.
     // Create the correctly sized stack slot (i.e. 80b for x86 fp)
      frameIndex = MF->getFrameInfo().CreateSpillStackObject(this->TRI->getRegSizeInBits(*RC) / 8,
                                                             this->TRI->getSpillAlignment(*RC));
      spillOperand->setIndex(frameIndex);
   }

   this->TII->storeRegToStackSlot(*MBB, MII, srcReg, false, frameIndex, RC, this->TRI);
   //TBD: implementing the isKill (4th) argument, if relevant to this RA, would require bringing in from
   //     clients an extra argument set from llvm::MachineOperand::isKill() of the operand being spilled
}

void
TargetRegisterAllocInfo::Reload
(
   llvm::MachineOperand * registerOperand,
   unsigned               destReg,
   llvm::MachineOperand * reloadOperand,
   llvm::MachineInstr *   insertBeforeInstruction,
   bool                   isFinal,
   bool                   isTileReload,
   bool                   isGlobal
)
{
   // note that the last 3 arguments are currently not used

   llvm::MachineBasicBlock *         MBB = insertBeforeInstruction->getParent();
   const llvm::TargetRegisterClass * RC;
   if (TRI->isVirtualRegister(destReg))
      RC = MF->getRegInfo().getRegClass(destReg);
   else
      RC = TRI->getLargestLegalSuperClass(TRI->getMinimalPhysRegClass(destReg), *MF);

   llvm::MachineBasicBlock::iterator MII = insertBeforeInstruction;

   this->TII->loadRegFromStackSlot(*MBB, MII, destReg, reloadOperand->getIndex(), RC, this->TRI);
}

void
TargetRegisterAllocInfo::PreferenceInstruction
(
   llvm::MachineInstr *           instruction,
   GraphColor::PreferenceVector * preferenceVector
)
{
   Tiled::VR::Info * vrInfo = this->vrInfo;

   // Clear the preference vector.
   preferenceVector->clear();

   if (instruction->isCopy()) {
      llvm::MachineOperand * destinationOperand = (instruction->defs()).begin();
      llvm::MachineOperand * sourceOperand = instruction->uses().begin();
      if (destinationOperand->isReg() && sourceOperand->isReg() &&
         !destinationOperand->isImplicit() && !sourceOperand->isImplicit()) {
         unsigned                                destinationAliasTag;
         unsigned                                sourceAliasTag;
         Tiled::RegisterAllocator::Preference    preference;
         Tiled::Cost                             cost;

         destinationAliasTag = vrInfo->GetTag(destinationOperand->getReg());
         sourceAliasTag = vrInfo->GetTag(sourceOperand->getReg());
         cost.Copy(&this->integerMoveCost);

         preference.AliasTag1 = destinationAliasTag;
         preference.AliasTag2 = sourceAliasTag;
         preference.Cost = cost;
         preference.PreferenceConstraint = Tiled::RegisterAllocator::PreferenceConstraint::SameRegister;

         preferenceVector->push_back(preference);
      }
   }
}

Tiled::Cost
TargetRegisterAllocInfo::IntegerMoveCost()
{
   return this->integerMoveCost;
}

bool
TargetRegisterAllocInfo::IsLargeBlock
(
   llvm::MachineBasicBlock * block
)
{
   //TBD: replace 50 with a non-literal value
   return (block->size() > 50);
}

bool
TargetRegisterAllocInfo::CanCallerSaveSpill
(
   llvm::MachineFunction * functionUnit,
   unsigned                reg
   // there was a Type argument
)
{
   //TBD?: for now
   return true;
}

void
TargetRegisterAllocInfo::addRC
(
   const llvm::TargetRegisterClass *RC
)
{
   // Allocatable registers
   llvm::SparseBitVector<> * allocatableAliasTagBitVector = new llvm::SparseBitVector<>();
   llvm::BitVector registerBitVector(this->TRI->getAllocatableSet(*(this->MF), RC));
   for (int reg = registerBitVector.find_first(); reg != -1; reg = registerBitVector.find_next(reg))
   {
      unsigned tag = vrInfo->GetTag(reg);
      //Issue t-xiliu: temporary fix to get around the bug in register allocator.
      //The interference detection fails to detect interference with certain physical registers (e.g. LR in ARM32).
      //TODO: remove this so that RA can be allocated if possible.
      if (this->MF->getRegInfo().getTargetRegisterInfo()->getRARegister() == tag)
         continue;

      allocatableAliasTagBitVector->set(tag);
   }

   (*this->RegisterCategoryIdToAllocatableAliasTagBitVector)[RC->getID()] = allocatableAliasTagBitVector;
   (*this->RegisterCategoryIdToAllocatableRegisterCount)[RC->getID()] = RC->getNumRegs();
}

const llvm::TargetRegisterClass *
TargetRegisterAllocInfo::GetRegisterCategory
(
   unsigned Reg
)
{
   // FIXME: currently TiledRA can not handle registers that are in more than one RCs.
   // So for regs that can potentially be conflicted, return identical RC!
   if (!Reg) {
      //issue - reg0 is noreg, return something here (refer to llvm/lib/CodeGen/TargetRegisterInfo.cpp).
      return TRI->getRegClass(0); //the returned valued can't be nullptr but is not really used
   }
   else if (this->TRI->isVirtualRegister(Reg)) {
      return MF->getRegInfo().getRegClass(Reg);
   }
   else if (this->TRI->isPhysicalRegister(Reg)) {
      return TRI->getLargestLegalSuperClass(TRI->getMinimalPhysRegClass(Reg), *MF);
   }
   else {
      llvm_unreachable("Unimplemented register type.");
      return nullptr;
   }
}

bool
TargetRegisterAllocInfo::CanReuseSpillSymbol
(
   Graphs::FrameIndex frameSlot,  //was: Symbol
   bool               isGlobal,
   bool               mayBeParameter
)
{
   return !mayBeParameter;
}

Cost
TargetRegisterAllocInfo::Fold
(
   llvm::MachineOperand * availableOperand,
   llvm::MachineOperand * scratchOperand
)
{
   //TBD
   return Cost::InfiniteCost;
}

const llvm::TargetRegisterClass *
TargetRegisterAllocInfo::GetBaseIntegerRegisterCategory()
{
   // QWordRegisters for amd64 and DWordRegisters for x86
   const llvm::TargetLowering *TLI = MF->getSubtarget().getTargetLowering();
   if (MF->getTarget().getTargetTriple().isArch64Bit())
      return TLI->getRegClassFor(llvm::MVT::i64);
   else
      return TLI->getRegClassFor(llvm::MVT::i32);
}

const llvm::TargetRegisterClass *
TargetRegisterAllocInfo::GetBaseFloatRegisterCategory()
{
   // XmmOWordRegisters for amd64
   const llvm::TargetLowering *TLI = MF->getSubtarget().getTargetLowering();
   if (MF->getTarget().getTargetTriple().isArch64Bit())
      return TLI->getRegClassFor(llvm::MVT::f64);
   else
      return TLI->getRegClassFor(llvm::MVT::f32);
}

void
TargetRegisterAllocInfo::InitializeFunction()
{
   //TBD
}

void
TargetRegisterAllocInfo::InitializeCosts()
{
   //TBD
}

void
TargetRegisterAllocInfo::InitializeRegisterVectorCosts
(
   GraphColor::CostVector * registerCostVector
)
{
   //TBD
}

unsigned
TargetRegisterAllocInfo::CalleeSaveConservativeThreshold()
{
   unsigned              threshold;
   //Tiled::Optimization::Goal optimizationGoal = MF->OptimizationGoal;
   //switch (optimizationGoal)
   //  case Tiled::Optimization::Goal::FavorSpeed:
      threshold = 6;
   //TBD: set the threshold for other  Optimization::Goal-s
   return threshold;
}

unsigned
TargetRegisterAllocInfo::EdgeProbabilityInstructionShareStopIteration()
{
   return 2;
}

unsigned
TargetRegisterAllocInfo::CrossCallInstructionShareStopIteration()
{
   return 4;
}

unsigned
TargetRegisterAllocInfo::InitialInstructionShareLimit()
{
   return 256;
}

unsigned
TargetRegisterAllocInfo::ConstrainedInstructionShareStartIteration()
{
   return 2;
}

unsigned
TargetRegisterAllocInfo::CallerSaveIterationLimit()
{
   // Bump this up for the higher optimization goal (/O?).

   return 1;
}

unsigned
TargetRegisterAllocInfo::IntegerPressureAdjustment()
{
   //TBD
   return 0;
}

unsigned
TargetRegisterAllocInfo::FloatPressureAdjustment()
{
   //TBD
   return 0;
}

double
TargetRegisterAllocInfo::InstructionShareDecayRateValue()
{
   return 0.25;
}

Tiled::Cost
TargetRegisterAllocInfo::EstimateInstructionCost
(
   llvm::MachineInstr * instruction
)
{
   //for now, general approximation
   Tiled::Cost instrCost;
   if (instruction->mayLoad())
      instrCost.Initialize(2, 4);
   else  // This includes mayStore()
      instrCost.Initialize(1, 4);

   return instrCost;
}

Tiled::Cost
TargetRegisterAllocInfo::EstimateMemoryCost
(
   llvm::MachineOperand * foldOperand
)
{
   Tiled::Cost memOperandCost;
   memOperandCost.Initialize(2, 2);

   return memOperandCost;
}

bool
TargetRegisterAllocInfo::IsLegalToReplaceOperand
(
   llvm::MachineOperand * originalOperand,
   llvm::MachineOperand * replacingOperand
)
{
   return (originalOperand->isReg() && replacingOperand->isReg());
   //For RISC architectures there are no valid folding replacements with memory replaceOperand
}

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled
