//===-- GraphColor/LiveRange.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "LiveRange.h"
#include "Allocator.h"
#include "ConflictGraph.h"
#include "CostModel.h"
#include "PreferenceGraph.h"
#include "Tile.h"
#include "AvailableExpressions.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize live range in preparation for allocation.
//
//------------------------------------------------------------------------------

void
LiveRange::Initialize
(
   GraphColor::Allocator * allocator
)
{
   // Initialize allocator.

   this->Allocator = allocator;

   // Initialize costs.

   this->SpillCost = allocator->ZeroCost;
   this->CallerSaveCost = allocator->ZeroCost;
   this->AllocationCost = allocator->ZeroCost;
   this->RecalculateCost = allocator->ZeroCost;
#ifdef ARCH_WITH_FOLDS
   this->FoldCost = allocator->ZeroCost;
#endif
   this->WeightCost = allocator->ZeroCost;

   this->CalleeSaveValueRegister = VR::Constants::InvalidReg;
   this->IsCalleeSaveValue = false;
   this->SummaryRegister = VR::Constants::InvalidReg;
   this->FrameSlot = VR::Constants::InvalidFrameIndex;
   this->IsSecondChanceGlobal = false;

   // Set initial state for decision

   this->decision = GraphColor::Decision::Initial;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for local live range object.
//
// Arguments:
//
//    tile        - Tile containing the lifetime to allocate the live range in.
//    aliasTag    - Widest appearance alias tag for the live range.
//    liveRangeId - Tile allocated live range number.    
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
LiveRange::NewLocal
(
   GraphColor::Tile * tile,
   unsigned           liveRangeId,
   unsigned           vrTag
)
{
   GraphColor::Allocator * allocator = tile->Allocator;
   GraphColor::LiveRange * liveRange = new GraphColor::LiveRange();

   // Set id and owning tile for context.
   liveRange->Id = liveRangeId;

   // Initialize allocation state.

   liveRange->Initialize(allocator);

   // Use property, side effect required.

   liveRange->VrTag = vrTag;

   //Tiled::VR::Info *           vrInfo = allocator->VrInfo;
   //llvm::SparseBitVector<> * writeThroughAliasTagSet = allocator->WriteThroughAliasTagSet;

   //if (vrInfo->CommonMayPartialTags(vrTag, writeThroughAliasTagSet)) {
   //   liveRange->IsWriteThrough = true;
   //}

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for global live range object.
//
// Arguments:
//
//    allocator   - Allocator containing the lifetime to allocate the live range in.
//    aliasTag    - Widest appearance alias tag for the live range.
//    liveRangeId - Tile allocated live range number.    
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
LiveRange::NewGlobal
(
   GraphColor::Allocator * allocator,
   unsigned                liveRangeId,
   int                     vrTag
)
{
   GraphColor::LiveRange * liveRange = new GraphColor::LiveRange();

   // Bit vector for registers alias tags for preferences of a global live range.  
   // Used by preferencing code when coloring a tile to setup some basic preferences.

   llvm::SparseBitVector<> * globalPreferenceRegisterAliasTagSet = new llvm::SparseBitVector<>();
   
   liveRange->GlobalPreferenceRegisterAliasTagSet = globalPreferenceRegisterAliasTagSet;

   // Register preference vector.

   GraphColor::PhysicalRegisterPreferenceVector * globalRegisterPreferenceVector =
      new GraphColor::PhysicalRegisterPreferenceVector();

   liveRange->GlobalRegisterPreferenceVector = globalRegisterPreferenceVector;

   // Bit vector for registers alias tags for hard preferences of a global live range.  
   // Used by preferencing code when coloring a tile to setup some basic preferences.

   llvm::SparseBitVector<> * globalHardPreferenceRegisterAliasTagSet = new llvm::SparseBitVector<>();

   liveRange->GlobalHardPreferenceRegisterAliasTagSet = globalHardPreferenceRegisterAliasTagSet;

   // Bit vector for registers alias tags that conflict with global live range.  
   // Used by preferencing code when coloring a tile to setup some basic anti-preferences.

   llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet = new llvm::SparseBitVector<>();

   liveRange->GlobalConflictRegisterAliasTagSet = globalConflictRegisterAliasTagSet;

   // Bit vector for global live range alias tags that conflict with global live range.  
   // Used by preferencing code when coloring a tile to setup some basic anti-preferences.

   llvm::SparseBitVector<> * globalConflictAliasTagSet = new llvm::SparseBitVector<>();

   liveRange->GlobalConflictAliasTagSet = globalConflictAliasTagSet;

   // Set id and owning tile for context.

   liveRange->Id = liveRangeId;

   // Initialize allocation state.

   liveRange->Initialize(allocator);

   // Use property, side effect required.

   liveRange->VrTag = vrTag;

   // Make this a global live range by pointing it at itself.
   liveRange->GlobalLiveRange = liveRange;
   assert(liveRange->IsGlobal());

   //Tiled::VR::Info *           vrInfo = allocator->VrInfo;
   //llvm::SparseBitVector<> * writeThroughAliasTagSet = allocator->WriteThroughAliasTagSet;

   //if (vrInfo->CommonMayPartialTags(vrTag, writeThroughAliasTagSet)) {
   //   liveRange->IsWriteThrough = true;
   //}

   return liveRange;
}

//------------------------------------------------------------------------------
//
//   Description:
//
//      Create a new summary live range member
//
// Arguments:
//
//    tile - Tile containing the lifetime to allocate the summary live range member in.
//    name - register name associated with member
//
//------------------------------------------------------------------------------

GraphColor::SummaryLiveRangeMember *
SummaryLiveRangeMember::New
(
   GraphColor::Tile * tile,
   int                name
)
{
   GraphColor::SummaryLiveRangeMember * summaryLiveRangeMember;

   summaryLiveRangeMember = new GraphColor::SummaryLiveRangeMember();

   //TODO:  ?? summaryLiveRangeMember->MemberKind = UnionFind::MemberKind::SummaryLiveRange;
   summaryLiveRangeMember->Initialize(name);

   return summaryLiveRangeMember;
}

GraphColor::LiveRangeMember *
LiveRangeMember::New
(
   int                                name,
   const llvm::TargetRegisterClass *  registerCategory
)
{
   GraphColor::LiveRangeMember * liveRangeMember;

   liveRangeMember = new GraphColor::LiveRangeMember();

   //TODO:  ?? liveRangeMember->MemberKind = UnionFind::MemberKind::LiveRange;
   liveRangeMember->Initialize(name);
   liveRangeMember->RegisterCategory = registerCategory;

   return liveRangeMember;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for summary live range object.
//
// Arguments:
//
//    tile        - Tile containing the lifetime to allocate the live range in.
//    aliasTag    - Widest appearance alias tag for the live range.
//    liveRangeId - Tile allocated live range number.    
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
LiveRange::NewSummary
(
   GraphColor::Tile * tile,
   unsigned           liveRangeId,
   int                vrTag
)
{
   GraphColor::Allocator * allocator = tile->Allocator;
   GraphColor::LiveRange * liveRange = new GraphColor::LiveRange();

   // Set id and owning tile for context.
   liveRange->Id = liveRangeId;

   // Initialize allocation state.
   liveRange->Initialize(allocator);

   // Use property, side effect required.
   liveRange->VrTag = vrTag;

   // Initialize the alias tag set.
   liveRange->SummaryAliasTagSet = new llvm::SparseBitVector<>();

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Reset live range id.
//
// Arguments:
//
//    id - new live range id
//
// Remarks:
//
//    Currently only valid to reset id's on summary live ranges as part of pruning.
//
//------------------------------------------------------------------------------

void
LiveRange::ResetId
(
   unsigned id
)
{
   assert(this->IsSummary());
   assert(this->IsPhysicalRegister || !this->SummaryAliasTagSet->empty());

   this->Id = id;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for live range object.
//
// Arguments:
//
//    tile        - Tile containing the lifetime to allocate the live range in.
//    aliasTag    - Widest appearance alias tag for the live range.
//    liveRangeId - Tile allocated live range number.    
//
//------------------------------------------------------------------------------

void
LiveRange::SetAliasTag
(
   int vrTag
)
{
   GraphColor::Allocator * allocator = this->Allocator;

   // Should never be making a live range for a "do not allocate" register.
   assert(!allocator->DoNotAllocateRegisterAliasTagBitVector->test(vrTag));

   // <place for code supporting architectures with sub-registers>

   // Set the alias tag and corresponding type.

   this->VrTag = vrTag;
   //this->type = type;
   //this->field = field;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete routine for LiveRange
//
//------------------------------------------------------------------------------

void
LiveRange::Delete()
{
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate register to live range and update tile summary variable mappings
//
// Internal:
//
//   Check for conflict violations.
//
//------------------------------------------------------------------------------

void
LiveRange::Allocate
(
   unsigned reg
)
{
   // check basic conflict

   GraphColor::Allocator *     allocator = this->Allocator;
   GraphColor::Tile *          tile = this->Tile;
   assert(tile == allocator->ActiveTile);

   GraphColor::ConflictGraph * conflictGraph = tile->ConflictGraph;
   assert(conflictGraph != nullptr);

   GraphColor::GraphIterator iter;

   // foreach_conflict_liverange
   for (GraphColor::LiveRange * conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&iter, this);
        (conflictLiveRange != nullptr);
        conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&iter)
      )
   {
      assert(reg != conflictLiveRange->Register);
   }

   assert(VR::Info::IsPhysicalRegister(reg));
   this->Register = reg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Mark all live range costs as infinite.
//
// Notes:
//
//    Marked as infinite this live range becomes un-modifiable for spill
//    or recalculation.  Needed for point lifetimes and special cases.
//
//------------------------------------------------------------------------------

void
LiveRange::MarkAsInfinite()
{
   this->SpillCost = Tiled::Cost::InfiniteCost;
   this->RecalculateCost = Tiled::Cost::InfiniteCost;
#ifdef ARCH_WITH_FOLDS
   this->FoldCost = Tiled::Cost::InfiniteCost;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test if there is a valid spill strategy available for this live
//    range - or it must be allocated.
//
//------------------------------------------------------------------------------

bool
LiveRange::CanBeSpilled()
{
   return (!this->SpillCost.IsInfinity() || !this->RecalculateCost.IsInfinity()
#ifdef ARCH_WITH_FOLDS
          || !this->FoldCost.IsInfinity()
#endif
          );
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Mark live range as "spilled"
//
// Notes:
//
//    Marks a live range, typically a summary, as spilled when we don't want to edit the summary
//    conflict/preference graph in flight.  Live ranges marked as "spilled" are intended to be ignored.
//
//------------------------------------------------------------------------------

void
LiveRange::MarkSpilled()
{
   // Removing the candidate register for a live range removes it as a candidate.

   this->Register = VR::Constants::InvalidReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize live range costs preliminary to costing.
//
//------------------------------------------------------------------------------

void
LiveRange::InitializeCosts()
{
   GraphColor::Tile *                 tile = this->Tile;
   GraphColor::Allocator *            allocator = this->Allocator;
   GraphColor::AvailableExpressions * availableExpressions = tile->AvailableExpressions;
   int                                vrTag = this->VrTag;

   llvm::MachineOperand * availableOperand = availableExpressions->GetAvailable(vrTag);

   // If there's no available expression for the live range start the cost at infinity.
   if (availableOperand == nullptr) {
      this->RecalculateCost = allocator->InfiniteCost;
#ifdef ARCH_WITH_FOLDS
      this->FoldCost = allocator->InfiniteCost;
#endif
   } else if (this->IsGlobal()) {
      this->SpillCost = allocator->InfiniteCost;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute max of the live range preference edges
//
//------------------------------------------------------------------------------

Tiled::Cost
LiveRange::MaxPreferenceEdgeCost()
{
   GraphColor::Allocator *          allocator = this->Allocator;
   GraphColor::Tile *               tile = this->Tile;
   Tiled::Cost                      cost;
   Tiled::Cost                      maxCost = allocator->ZeroCost;
   GraphColor::AllocatorCostModel * costModel = tile->CostModel;

   RegisterAllocator::PreferenceConstraint preferenceConstraint;

   GraphColor::PreferenceGraph * graph = tile->PreferenceGraph;
   GraphColor::GraphIterator _iterator;
   GraphColor::LiveRange * preferenceLiveRange;

   for (preferenceLiveRange = graph->GetFirstPreferenceLiveRange(&_iterator, this, &cost, &preferenceConstraint);
        preferenceLiveRange != nullptr;
        preferenceLiveRange = graph->GetNextPreferenceLiveRange(&_iterator, &cost, &preferenceConstraint))
   {
      if (costModel->Compare(&maxCost, &cost) < 0) {
         maxCost = cost;
      }
   }

   return maxCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Reset the current spill decision.
//
// Notes:
//
//    Results from added constraints coming in during spilling. Such as replacement of an operand on an
//    instruction forces a switch from replace to spill decision for a live range associated on a given
//    instruction.
//
//------------------------------------------------------------------------------

void
LiveRange::ResetDecision()
{
   // force live range to remake the spilling type decision.

   this->decision = GraphColor::Decision::Initial;

   this->bestCost = Tiled::Cost::InfiniteCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    External API to allow the spill decision on a live range to be set to
//    memory.  Used by the fastpath allocator so that it can simply spill
//    live ranges directly to memory.
//
//------------------------------------------------------------------------------

void
LiveRange::ForceMemorySpillDecision()
{
   this->decision = GraphColor::Decision::Memory;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get best spill cost from among the live range alternatives
//
//------------------------------------------------------------------------------

Tiled::Cost
LiveRange::GetBestSpillCost()
{
   GraphColor::Decision decision = this->Decision();

   // Decision getter sets the best cost, if(){}else{} construct is an assert analog to make sure there is a
   // use of decision.  Note: decision can be 'Invalid' for the case "can't spill" case.

   if (decision != GraphColor::Decision::Initial) {
      return this->bestCost;
   } else {
      assert(false);

      return Tiled::Cost::InfiniteCost;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get best spill cost from among the live range alternatives
//
//------------------------------------------------------------------------------

// For bringup force Decision::Memory spill

Tiled::Cost
LiveRange::GetBestSpillCost
(
   GraphColor::AllocatorCostModel * costModel
)
{
   GraphColor::Allocator *          allocator = this->Allocator;
   Tiled::Cost                      bestCost = Tiled::Cost::InfiniteCost;

   this->decision = GraphColor::Decision::Invalid;

   if (this->IsTrivial ||
         (this->SpillCost.IsInfinity() && this->RecalculateCost.IsInfinity()
#ifdef ARCH_WITH_FOLDS
          && this->FoldCost.IsInfinity()
#endif
         )
      ) {
      // If marked as infinite we must be allocated and not spilled so spill decision stays 'invalid'.  

      this->bestCost = allocator->InfiniteCost;
      return this->bestCost;
   }

   Tiled::Cost spillCost = this->SpillCost;

   if (costModel->Compare(&bestCost, &spillCost) >= 0) {
      bestCost = spillCost;
      this->decision = GraphColor::Decision::Memory;
   }

   Tiled::Cost recalculateCost = this->RecalculateCost;

   if (costModel->Compare(&bestCost, &recalculateCost) >= 0) {
      bestCost = recalculateCost;
      this->decision = GraphColor::Decision::Recalculate;
   }

#ifdef ARCH_WITH_FOLDS
      Tiled::Cost foldCost = this->FoldCost;

      if (costModel->Compare(&bestCost, &foldCost) >= 0) {
         bestCost = foldCost;
         this->decision = GraphColor::Decision::Fold;
      }
#endif

#ifdef FUTURE_IMPL   // MULTITILE +
   Tiled::Cost callerSaveCost = this->CallerSaveCost;

   if (costModel->Compare(&bestCost, &callerSaveCost) >= 0) {
      bestCost = callerSaveCost;
      this->decision = GraphColor::Decision::CallerSave;
   }
#endif

   this->bestCost = bestCost;

   return this->bestCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get spill decision, implemented in a lazy approach.
//
//------------------------------------------------------------------------------

GraphColor::Decision
LiveRange::Decision()
{
   // if in initial state do comparison to determine decision

   if (this->decision == GraphColor::Decision::Initial) {
      GraphColor::Tile *               tile = this->Tile;
      GraphColor::AllocatorCostModel * costModel = tile->CostModel;

      this->GetBestSpillCost(costModel);
   }

   return this->decision;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get spill benefit.
//
// Notes:
//
//    This is a scalar which roughly represents the reduction in resource demand if this live range is
//    spilled. Currently this only shows for a fold which uniformly removes a resource needing allocation.
//    This term is added to the simplification cost in the denominator with degree.
//
//    SpillCost / (Degree + SpillBenefit)
//
//------------------------------------------------------------------------------

#ifdef ARCH_WITH_FOLDS
unsigned
LiveRange::SpillBenefit()
{
   return (this->decision == GraphColor::Decision::Fold) ? this->FoldBenefit : 0;
}
#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Add live range to summary live range.
//
//------------------------------------------------------------------------------

void
LiveRange::AddLiveRange
(
   GraphColor::LiveRange * liveRange
)
{
   assert(this->IsSummary());
   assert(!liveRange->IsSummary());

   unsigned aliasTag = liveRange->VrTag;

   this->SummaryAliasTagSet->set(aliasTag);

   liveRange->SummaryLiveRangeId = this->Id;

   this->UseCount += liveRange->UseCount;
   this->DefinitionCount += liveRange->DefinitionCount;
   //this->HasByteReference |= liveRange->HasByteReference;
   this->IsCallSpanning |= liveRange->IsCallSpanning;

   this->IsCalleeSaveValue |= liveRange->IsCalleeSaveValue;
   this->IsCalleeSaveValue = this->IsCalleeSaveValue && !this->HasReference();

   this->HasHardPreference |= liveRange->HasHardPreference;
   this->HasHardPreference |= liveRange->IsPreColored;

   if (liveRange->IsCalleeSaveValue) {
      if (this->CalleeSaveValueRegister == VR::Constants::InvalidReg) {
         this->CalleeSaveValueRegister = liveRange->CalleeSaveValueRegister;
      } else {
         // Ensure we don't get a silly case like two different callee save values being put together.
         assert(this->CalleeSaveValueRegister == liveRange->CalleeSaveValueRegister);
      }
   }

   // <place for code supporting architectures with sub-registers>

   Tiled::Cost weightCost = liveRange->WeightCost;
   Tiled::Cost summaryWeightCost = this->WeightCost;

   summaryWeightCost.IncrementBy(&weightCost);
   this->WeightCost = summaryWeightCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Remove live range (by alias tag) from summary live range.
//
//------------------------------------------------------------------------------

void
LiveRange::RemoveFromSummary
(
   unsigned vrTag
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   Tiled::VR::Info *       vrInfo = allocator->VrInfo;

   assert(this->IsSummary());

   vrInfo->MinusMayPartialTags(vrTag, this->SummaryAliasTagSet);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get first global live range covered by this summary live range.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
LiveRange::GetGlobalLiveRange()
{
   GraphColor::Tile *        tile = this->Tile;
   GraphColor::Allocator *   allocator = tile->Allocator;
   GraphColor::LiveRange *   globalLiveRange = nullptr;
   llvm::SparseBitVector<> * summaryAliasTagSet = this->SummaryAliasTagSet;
   int                       vrTag;

   assert(this->IsSummary());

   llvm::SparseBitVector<>::iterator iter;

   for (iter = summaryAliasTagSet->begin(); iter != summaryAliasTagSet->end(); ++iter)
   {
      vrTag = *iter;
      globalLiveRange = allocator->GetGlobalLiveRange(vrTag);
      if (globalLiveRange != nullptr) {
         break;
      }
   }

   return globalLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test is this live range is constrained by a conflict with a physical register.
//
//------------------------------------------------------------------------------

bool
LiveRange::HasPhysicalRegisterConflict()
{
   GraphColor::Tile *      tile = this->Tile;

   if (this->IsSummary()) {
      GraphColor::SummaryConflictGraph * summaryConflictGraph = tile->SummaryConflictGraph;
      assert(summaryConflictGraph != nullptr);

      GraphColor::GraphIterator _iterator;
      GraphColor::LiveRange * summaryConflictLiveRange;
      for (summaryConflictLiveRange = summaryConflictGraph->GetFirstConflictSummaryLiveRange(&_iterator, this);
           summaryConflictLiveRange != nullptr;
           summaryConflictLiveRange = summaryConflictGraph->GetNextConflictSummaryLiveRange(&_iterator))
      {
         if (summaryConflictLiveRange->IsPhysicalRegister || summaryConflictLiveRange->IsPreColored) {
            return true;
         }
      }

   } else {
      GraphColor::ConflictGraph * conflictGraph = tile->ConflictGraph;
      assert(conflictGraph != nullptr);

      GraphColor::GraphIterator _iterator;
      GraphColor::LiveRange * conflictLiveRange;
      for (conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&_iterator, this);
           conflictLiveRange != nullptr;
           conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&_iterator))
      {
         if (conflictLiveRange->IsPhysicalRegister || conflictLiveRange->IsPreColored) {
            return true;
         }
      }
   }

   return false;
}

const llvm::TargetRegisterClass *
LiveRange::GetRegisterCategory()
{
   unsigned Reg = this->Register;
   if (Reg == VR::Constants::InitialPseudoReg || Reg == VR::Constants::InvalidReg) {
      Reg = this->Allocator->VrInfo->GetRegister(this->VrTag);
   }

   return this->Allocator->GetRegisterCategory(Reg);
}

const llvm::TargetRegisterClass *
LiveRange::BaseRegisterCategory()
{
   unsigned Reg = this->Register;
   if (Reg == VR::Constants::InitialPseudoReg || Reg == VR::Constants::InvalidReg) {
      Reg = this->Allocator->VrInfo->GetRegister(this->VrTag);
   }

   return this->Allocator->GetRegisterCategory(Reg);
}

bool
LiveRange::IsAssigned()
{
   return (VR::Info::IsPhysicalRegister(this->Register));
}

} // GraphColor
} // RegisterAllocator
} // Tiled
