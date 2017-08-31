//===-- GraphColor/LiveRange.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_LIVERANGE_H
#define TILED_GRAPHCOLOR_LIVERANGE_H

#include "llvm/ADT/SparseBitVector.h"
#include "Graph.h"
#include "../Alias/Alias.h"
#include "../Graphs/Graph.h"
#include "../Graphs/UnionFind.h"
#include "../Cost.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

class Tile;
enum class Decision;
class AllocatorCostModel;

class LiveRange
{

public:

   /*override*/ void Delete();

   static GraphColor::LiveRange *
   NewGlobal
   (
      GraphColor::Allocator * allocator,
      unsigned                liveRangeId,
      int                     tag
   );

   static GraphColor::LiveRange *
   NewLocal
   (
      GraphColor::Tile * tile,
      unsigned           liveRangeId,
      unsigned           tag
   );

   static GraphColor::LiveRange *
   NewSummary
   (
      GraphColor::Tile * tile,
      unsigned           liveRangeId,
      int                tag
   );

public:

   void
   AddLiveRange
   (
      GraphColor::LiveRange * liveRange
   );

   void
   Allocate
   (
      unsigned reg
   );

   void ForceMemorySpillDecision();

   Tiled::Cost GetBestSpillCost();

   GraphColor::LiveRange * GetGlobalLiveRange();

   void
   Initialize
   (
      GraphColor::Allocator * allocator
   );

   void InitializeCosts();

   void MarkAsInfinite();

   void MarkSpilled();

   Tiled::Cost MaxPreferenceEdgeCost();

   void
   RemoveFromSummary
   (
      unsigned tag
   );

   void ResetDecision();

   void
   ResetId
   (
      unsigned id
   );

public:

   bool CanBeSpilled();

   GraphColor::Decision Decision();

   bool HasPhysicalRegisterConflict();

   void                                            
   SetAliasTag
   (
      int tag
   );

   int
   GetAliasTag()
   {
      return VrTag;
   }

   unsigned SpillBenefit();

   // For VRs a biased index value is stored in VrTag, if ever needed, it can be unambiguously
   // converted back to the negative integer used by llvm. For PhysRegs it should not matter.
   unsigned                                        VrTag;

   Tiled::Cost                                     AllocationCost;
   GraphColor::Allocator *                         Allocator;
   unsigned                                        Area;

   const llvm::TargetRegisterClass *
   BaseRegisterCategory();

   unsigned                                        CalleeSaveValueRegister;
   Tiled::Cost                                     CallerSaveCost;
   unsigned                                        DefinitionCount;
   unsigned                                        Degree;
   unsigned                                        ConflictEdgeCount;
   unsigned                                        ConflictAdjacencyIndex;
   unsigned                                        PreferenceEdgeCount;
   unsigned                                        PreferenceAdjacencyIndex;

   // Types::Field *                                  Field get noset { field }

#ifdef ARCH_WITH_FOLDS
   unsigned                                        FoldBenefit; // default was 1
   Tiled::Cost                                     FoldCost;
#endif

   int
   GlobalAliasTag()
   {
      return this->GlobalLiveRange->VrTag;
   }

   llvm::SparseBitVector<> *                       GlobalConflictAliasTagSet;
   llvm::SparseBitVector<> *                       GlobalConflictRegisterAliasTagSet;
   llvm::SparseBitVector<> *                       GlobalHardPreferenceRegisterAliasTagSet;
   GraphColor::LiveRange *                         GlobalLiveRange;
   llvm::SparseBitVector<> *                       GlobalPreferenceRegisterAliasTagSet;
   GraphColor::PhysicalRegisterPreferenceVector *  GlobalRegisterPreferenceVector;

   //bool                                            HasByteReference;
   bool                                            HasCallSpanningPreference;

   bool                                    
   HasDefinition()
   {
      return (this->DefinitionCount != 0); 
   }

   bool                                            HasHardPreference;
 
   bool 
   HasReference() 
   { 
      return (this->HasUse() || this->HasDefinition()); 
   }

   bool                                            HasSpillPartition;  
   //bool                                          HasSymbol;  //HasFrameSlot cannot replace it at liveness computation time
   
   bool                                    
   HasUse()
   {
      return (this->UseCount != 0); 
   }

   unsigned                                        Id;

   bool
   IsAssigned
   (
   );


   bool                                            IsBlockLocal;
   bool                                            IsCallSpanning;
   bool                                            IsCalleeSaveValue;

   bool
   IsGlobal() 
   { 
      return (this->GlobalLiveRange != nullptr);
   }

   bool                                            IsPhysicalRegister;
   bool                                            IsPreColored;
   bool                                            IsSecondChanceGlobal;
   bool                                            IsSimplified;
   
   bool
   IsSpilled()
   { 
      return (this->Register == VR::Constants::InvalidReg);
   }

   bool 
   IsSummary()
   {
      return (this->SummaryAliasTagSet != nullptr);
   }

   bool
   IsNotAssignedVirtual
   (
      Tiled::VR::Info * aliasInfo
   )
   {
      unsigned reg = aliasInfo->GetRegister(this->VrTag);
      return (aliasInfo->IsVirtualRegister(reg) && !aliasInfo->IsPhysicalRegister(this->Register));
   }

   Tiled::Cost
   GetBestSpillCost
   (
      GraphColor::AllocatorCostModel * costModel
   );

   bool                                            IsSummaryProxy;
   bool                                            IsTrivial;
   //bool                                            IsWriteThrough;

   Tiled::CostValue                                Pressure;
   Tiled::Cost                                     RecalculateCost;

   unsigned                                        Register;

   const llvm::TargetRegisterClass *
   GetRegisterCategory();

   unsigned                                        SearchNumber;
   Tiled::Cost                                     SpillCost;
   llvm::SparseBitVector<> *                       SummaryAliasTagSet;
   unsigned                                        SummaryLiveRangeId;
   unsigned                                        SummaryRegister;

   Graphs::FrameIndex                              FrameSlot; //was: Symbol

   GraphColor::Tile *                              Tile;
   //Types::Type *                                   Type;
   unsigned                                        UseCount;
   bool                                            WasAnalyzed;
   Tiled::Cost                                     WeightCost;

private:

   Tiled::Cost                                     bestCost;
   GraphColor::Decision                            decision;

};

/*
comment LiveRange::AliasTag
{
   // Underlying widest appearance alias tag for the LiveRange
}

comment LiveRange::IsCallSpanning
{
   // If true, this live range spans a call.
}

comment LiveRange::IsTrivial
{
   //  If true, this is a trivial live range that spans only two adjacent 
   //  instructions, the def instruction immediately followed by the one
   //  and only use instruction (i.e. single-def/single-use).
}

comment LiveRange::Type
{
   // Type associated with widest appearance alias tag for the LiveRange
}

comment LiveRange::RegisterCategory
{
   // Register category required of the resource assigned to
   // these appearances
}

comment LiveRange::Register
{
   // Live range register.  One of candidate pseudo, physreg pseudo,
   // or physreg.
}

comment LiveRange::IsAssigned
{
   // True if live range has been assigned a register
}

comment LiveRange::AllocationCost
{
   // Cost of definition and resources for allocation of this live range
}

comment LiveRange::FoldCost
{
   // Cost to fold all appearances back to memory
}

comment LiveRange::SpillCost
{
   // Cost to spill this live range at all appearances
}

comment LiveRange::RecalculateCost
{
   // Cost to recalculate, if possible, for each appearance of the live
   // range - 
}

comment LiveRange::CallerSaveCost
{
   // cost to spill around any intervening calls if the register gets
   // a caller save register.
}
*/

class SummaryLiveRangeMember : public UnionFind::Member
{
public:
   static GraphColor::SummaryLiveRangeMember *
   New
   (
      GraphColor::Tile * tile,
      int                name
   );

public:
   unsigned         SummaryLiveRangeId;
};

class LiveRangeMember : public UnionFind::Member
{
public:
   static GraphColor::LiveRangeMember *
   New
   (
      int                                name,
      const llvm::TargetRegisterClass *  registerCategory
   );

public:
   unsigned                           LiveRangeId;
   const llvm::TargetRegisterClass *  RegisterCategory;
};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_LIVERANGE_H
