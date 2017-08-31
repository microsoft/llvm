//===-- Alias/Alias.h - Alias Information -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_ALIAS_ALIAS_H
#define TILED_ALIAS_ALIAS_H

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"

namespace Tiled
{
   static const unsigned NoReg = 0;

namespace VR
{
   
class Constants
{
public:
   static const unsigned InvalidTag = ~0; //(-1)
   static const unsigned InvalidReg = ~0;
   static const unsigned InitialPseudoReg = (~0 & ~1);
   static const int      InvalidFrameIndex = -1;
   static const unsigned NoMoreBits = ~0;
};
   
enum class StorageClass
{
   IllegalSentinel = 0, // illegal storage class
   Auto, // function local variable
   Parameter, // function parameter
   Static, // global variable
   Argument, // function output argument
   CalleeSave // Callee save location
};

enum class AliasType
{
   InvalidAliasType = 0,
   Must, //in case of (virtual) register aliasing means definite complete overlap
   May   //in case of (virtual) register aliasing means at least partial overlap
};

enum class SideEffectSummaryOptions
{
   None = 0, // No restrictions on the summary
   IgnoreLocalTemporaries = 1  // Ignore temporaries defined in the set of instructions to summarize
};


class Info
{
public:
   
   Info
   (
      llvm::MachineFunction * mf
   )
   {
      // ISSUE-TODO-aasmith-2015/11/23: This is initializing a static!
      // Changed to make independent.
      if (HighPhysicalRegister == 0) {
         HighPhysicalRegister = mf->getSubtarget().getRegisterInfo()->getNumRegs() - 1;
      }
   }
   
   void
   OrMustTotalTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );
   
   void
   OrMayTotalTags
   (
      unsigned int             tag,
      llvm::SparseBitVector<>* tagsBitVector
   );
   
   void
   OrMayPartialTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );

   void
   OrMayPartialTags
   (
      llvm::SparseBitVector<> * tagsBitVector,
      llvm::SparseBitVector<> * resultBitVector
   );

   void
   MinusMustTotalTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );
   
   //for register, or location without fields, just clear the tag's bit
   void
   MinusMayPartialTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );
   
   void
   OrMemberTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );
   
   bool
   CommonMayPartialTags
   (
      unsigned                 tag,
      llvm::SparseBitVector<>* tagsBitVector
   );

   bool
   CommonMayPartialTags
   (
      llvm::SparseBitVector<>* tagsBitVector1,
      llvm::SparseBitVector<>* tagsBitVector2
   );

   bool
   MustTotallyOverlap
   (
      llvm::MachineOperand * opnd1,
      llvm::MachineOperand * opnd2
   )
   {
      unsigned tag1 = VR::Constants::InvalidTag;
      unsigned tag2 = VR::Constants::InvalidTag;
      if (opnd1->isReg() && opnd2->isReg()) {
         tag1 = opnd1->getReg();
         tag2 = opnd2->getReg();
      }
      else if (opnd1->isFI() && opnd2->isFI()) {
         tag1 = static_cast<unsigned>(opnd1->getIndex());
         tag2 = static_cast<unsigned>(opnd2->getIndex());
      }
      else {
         return false;
      }
      return MustTotallyOverlap(tag1, tag2);
   }
   
   // On an architecture without sub-registers, a register always totally overlaps itself
   bool
   MustTotallyOverlap
   (
      unsigned tag1,
      unsigned tag2
   );
   
   //register/location always overlaps itself
   bool
   MayPartiallyOverlap
   (
      unsigned tag1,
      unsigned tag2
   );

   llvm::MachineOperand*
   DestinationMayPartiallyOverlap
   (
      llvm::MachineInstr *   instruction,
      llvm::MachineOperand * operand
   );

   llvm::MachineOperand*
   SourceMayPartiallyOverlap
   (
      llvm::MachineInstr *   instruction,
      llvm::MachineOperand * operand
   );

   unsigned
   GetTag
   (
      unsigned Reg
   )
   {
      //TBD, for now:
      if (Reg == VR::Constants::InvalidReg || Reg == VR::Constants::InitialPseudoReg)
         return VR::Constants::InvalidTag;

      return reg2Index(Reg);
   }
   
   unsigned
   GetRegister
   (
      unsigned Index
   )
   {
      if (Index == VR::Constants::InvalidTag)
         return VR::Constants::InvalidReg;

      return index2Reg(Index);
   }
   
   static unsigned
   index2Reg
   (
      unsigned index
   )
   {
      if (index > HighPhysicalRegister) {
         if (index < (1u << 30)) {
            //virtual register
            return llvm::TargetRegisterInfo::index2VirtReg(index - (HighPhysicalRegister + 1));
         } else {
            //this is a StackSlot
            assert(index >= (1u << 30) && index < (1u << 31));
            return index;
         }
      } else {
         return index;
      }
   }
   
   static unsigned
   reg2Index
   (
      unsigned reg
   )
   {
      if ((reg & (1u << 31)) != 0) {
         return (HighPhysicalRegister + 1 + llvm::TargetRegisterInfo::virtReg2Index(reg));
      } else if (reg <= HighPhysicalRegister) {
         //this is a physical register
         return (reg);
      } else {
         //this is a stack slot
         assert(reg >= (1u << 30));
         return reg;
      }
   }
   
   static bool
   IsRegisterTag
   (
      unsigned tag
   )
   {
      // Tiled::Alias::IsRegisterTag checked if an Alias::Tag stands for a 'physical', or 'virtual'/IsPseudo, register:
      //   IsLocationTag(tag) =>  IdMap(tag) => IsRegisterId(id): Unit->GetRegister(id) => (reg->IsTarget || reg->IsPseudo)
      //
      if (tag <= HighPhysicalRegister || (tag > HighPhysicalRegister && tag < (1u << 30))) {
         return true;
      }
      
      return false;
   }
   
   static bool
   IsPhysicalRegisterTag
   (
      unsigned tag
   )
   {
      // physical registers => tag is the identity mapping
      return IsPhysicalRegister(tag);
   }
   
   static bool
   IsPhysicalRegister
   (
      unsigned reg
   )
   {
      return (reg <= HighPhysicalRegister);
   }
   
   static bool
   IsVirtualRegister
   (
      unsigned reg
   )
   {
      return ((reg != VR::Constants::InvalidReg) && (reg != VR::Constants::InitialPseudoReg) && ((reg & (1u << 31)) != 0));
   }
   
   bool
   IsSingletonTag
   (
      unsigned tag
   )
   {
      // For an architecture with no sub-registers
      return true;
   }

   bool
   CheckTag
   (
      unsigned tag
   );

   llvm::SparseBitVector<>*
   SummarizeDefinitionTagsInRange
   (
      llvm::MachineInstr *            firstInstruction,
      llvm::MachineInstr *            lastInstruction,
      VR::AliasType                   type,
      VR::SideEffectSummaryOptions    summaryOptions,
      llvm::SparseBitVector<> *       definitionTagSet
   );

   llvm::MachineOperand *
   DependencyForward
   (
      llvm::MachineInstr *  instruction,
      llvm::MachineInstr *  fromInstruction,
      llvm::MachineInstr *  toInstruction
   );

   llvm::MachineOperand *
   DependencyBackward
   (
      llvm::MachineInstr *  instruction,
      llvm::MachineInstr *  fromInstruction,
      llvm::MachineInstr *  toInstruction
   );

   llvm::MachineOperand *
   PartialDefinitionForward
   (
      llvm::MachineOperand *  operand,
      llvm::MachineInstr *    fromInstruction,
      llvm::MachineInstr *    toInstruction
   );

   llvm::MachineOperand *
   PartialDefinitionBackward
   (
      llvm::MachineOperand *  operand,
      llvm::MachineInstr *    fromInstruction,
      llvm::MachineInstr *    toInstruction
   );

   llvm::MachineOperand *
   PartialUseOrDefinitionForward
   (
      llvm::MachineOperand *  operand,
      llvm::MachineInstr *    fromInstruction,
      llvm::MachineInstr *    toInstruction
   );

   llvm::MachineOperand *
   PartialUseOrDefinitionBackward
   (
      llvm::MachineOperand *  operand,
      llvm::MachineInstr *    fromInstruction,
      llvm::MachineInstr *    toInstruction
   );

   static unsigned HighPhysicalRegister;
};

}  // namespace VR
   
} // namespace Tiled

#endif // TILED_ALIAS_ALIAS_H
