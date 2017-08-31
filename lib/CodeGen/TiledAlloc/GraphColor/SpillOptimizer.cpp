//===-- GraphColor/SpillOptimizer.cpp ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SpillOptimizer.h"
#include "Allocator.h"
#include "Liveness.h"
#include "LiveRange.h"
#include "../Graphs/NodeWalker.h"
#include "TargetRegisterAllocInfo.h"

#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/IR/DebugLoc.h"

#define DEBUG_TYPE "tiled-spill"

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
//    Static constructor for spill record.
//
// Arguments:
//
//    spillOptimizer     - Owning spill optimizer.
//    recalculateOperand - Recalculate operand being tracked (or nullptr).
//    reloadOperand      - Reload operand being tracked (or nullptr).
//    spillDefinitionOperand - Definition operand of a pending spill (or nullptr).
//    spillOperand       - Backing memory of the spill (or nullptr).
//    valueOperand       - constant value operand for this context (or nullptr)
//
// Returns:
//
//    New spill optimizer object.
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillRecord::New
(
   GraphColor::SpillOptimizer * spillOptimizer,
   llvm::MachineOperand *       recalculateOperand,
   llvm::MachineOperand *       reloadOperand,
   llvm::MachineOperand *       spillOperand,
   llvm::MachineOperand *       valueOperand
)
{
   GraphColor::SpillRecord * spillRecord = GraphColor::SpillRecord::New(spillOptimizer);

   spillRecord->RecalculateOperand = recalculateOperand;
   spillRecord->ReloadOperand = reloadOperand;
   spillRecord->SpillOperand = spillOperand;
   spillRecord->ValueOperand = valueOperand;

   return spillRecord;
}

GraphColor::SpillRecord *
SpillRecord::New
(
   GraphColor::SpillOptimizer * spillOptimizer
)
{
   GraphColor::SpillRecord * spillRecord;

   if (spillOptimizer->IsGlobalScope()) {
      //GraphColor::Allocator * allocator = spillOptimizer->Allocator;

      spillRecord = new GraphColor::SpillRecord();
   } else {
      //GraphColor::Tile * tile = spillOptimizer->Tile;
      
      spillRecord = new GraphColor::SpillRecord();
   }

   spillRecord->nextIterationHashItem = nullptr;
   spillRecord->previousIterationHashItem = nullptr;

   return spillRecord;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Delete this spill-record. This will delete scratch IR that isn't linked into the IR.
//
//------------------------------------------------------------------------------

void
SpillRecord::Delete()
{
   if (this->HasRecalculate()) {
      llvm::MachineOperand *  recalculateOperand = this->RecalculateOperand;
      llvm::MachineInstr *    recalculateInstruction = recalculateOperand->getParent();

      // The recalculate operand is not linked into the instruction, or instruction into block.
      if ((recalculateInstruction) && (recalculateInstruction->getParent() == nullptr)) {
         assert(this->BasicBlock);
         llvm::MachineFunction * MF = this->BasicBlock->getParent();
         SpillOptimizer::DeleteInstruction(recalculateInstruction, MF);
      }

      this->RecalculateOperand = nullptr;
   }

   if (this->HasReload()) {
      llvm::MachineOperand *  reloadOperand = this->ReloadOperand;
      llvm::MachineInstr *    reloadInstruction = reloadOperand->getParent();

      // The reload operand is not linked into the instruction.
      if ((reloadInstruction) && (reloadInstruction->getParent() == nullptr)) {
         assert(this->BasicBlock);
         llvm::MachineFunction * MF = this->BasicBlock->getParent();
         SpillOptimizer::DeleteInstruction(reloadInstruction, MF);
      }

      this->ReloadOperand = nullptr;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record of a fold
//
//------------------------------------------------------------------------------

void
SpillRecord::SetFold()
{
#ifdef ARCH_WITH_FOLDS
   unsigned result = unsigned(this->SpillKinds) | unsigned(GraphColor::SpillKinds::Fold);
   this->SpillKinds = GraphColor::SpillKinds(result);
#endif
   assert(0 && "nyi");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record of a recalculate
//
//------------------------------------------------------------------------------

void
SpillRecord::SetRecalculate()
{
   // Recalculate should only have the recalculateOperand set.
   assert(this->RecalculateOperand != nullptr);

   unsigned result = unsigned(this->SpillKinds) | unsigned(GraphColor::SpillKinds::Recalculate);
   this->SpillKinds = GraphColor::SpillKinds(result);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear spill record of a spill
//
//------------------------------------------------------------------------------

void
SpillRecord::ClearSpill()
{
   assert(this->SpillOperand == nullptr);

   unsigned result = unsigned(this->SpillKinds) & ~(unsigned(GraphColor::SpillKinds::Spill));
   this->SpillKinds = GraphColor::SpillKinds(result);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear spill record of a folded spill
//
//------------------------------------------------------------------------------

void
SpillRecord::ClearFoldedSpill()
{
#ifdef ARCH_WITH_FOLDS
   unsigned result = unsigned(this->SpillKinds) &  ~(unsigned(GraphColor::SpillKinds::FoldedSpill));
   this->SpillKinds = GraphColor::SpillKinds(result);
#endif
   assert(0 && "nyi");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record for spill
//
// Notes:
//
//    Spill kind clears any prior reload/recalculate state.
//
//------------------------------------------------------------------------------

void
SpillRecord::SetSpill()
{
   // Spills have
   // - spill operand
   // - spill definition

   assert(this->SpillOperand != nullptr);

#ifdef ARCH_WITH_FOLDS
   // Remove folded reload/spill
   unsigned result1 = ~(unsigned(GraphColor::SpillKinds::FoldedReload) | unsigned(GraphColor::SpillKinds::FoldedSpill));
   unsigned result2 = (unsigned(this->SpillKinds) & result1);*/
   // Add Spill
   this->SpillKinds = GraphColor::SpillKinds(result2 | unsigned(GraphColor::SpillKinds::Spill));
#else
   // Add Spill
   this->SpillKinds = GraphColor::SpillKinds(unsigned(this->SpillKinds) | unsigned(GraphColor::SpillKinds::Spill));
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record for folded spill
//
// Notes:
//
//    FoldedSpill and Spill are mutually exclusive - once a folded spill is sunk then its at can only be
//    a spill.
//
//------------------------------------------------------------------------------

void
SpillRecord::SetFoldedSpill()
{
#ifdef ARCH_WITH_FOLDS
   // Folded spill (clear all except folded reload and add folded spill)
   unsigned result1 = unsigned(this->SpillKinds) & unsigned(GraphColor::SpillKinds::FoldedReload);
   unsigned result2 = unsigned(GraphColor::SpillKinds::FoldedSpill) | result1;
   this->SpillKinds = GraphColor::SpillKinds(result2);
#endif
   assert(0 && "nyi");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear reload record of a spill
//
//------------------------------------------------------------------------------

void
SpillRecord::ClearReload()
{
   assert(this->ReloadOperand == nullptr);

   unsigned result = (unsigned(this->SpillKinds) & ~(unsigned(GraphColor::SpillKinds::Reload)));
   this->SpillKinds = GraphColor::SpillKinds(result);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record for reload
//
// Notes:
//
//    FoldedReload and Reload are mutually exclusive - once a folded reload is
//    hoisted then its at can only be a reload.
//
//------------------------------------------------------------------------------

void
SpillRecord::SetReload()
{
    assert(this->ReloadOperand != nullptr);

#ifdef ARCH_WITH_FOLDS
    // remove fold reload bit if any
    unsigned result = (unsigned(this->SpillKinds) & ~(unsigned(GraphColor::SpillKinds::FoldedReload)));
    // Add Reload
    this->SpillKinds = GraphColor::SpillKinds(result | unsigned(GraphColor::SpillKinds::Reload));
#else
    // Add Reload
    this->SpillKinds = GraphColor::SpillKinds(unsigned(this->SpillKinds) | unsigned(GraphColor::SpillKinds::Reload));
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill record for folded reload
//
// Notes:
//
//    FoldedReload and Reload are mutually exclusive - once a folded reload
//    is hoisted then its at can only be a reload.
//
//------------------------------------------------------------------------------

void
SpillRecord::SetFoldedReload()
{
#ifdef ARCH_WITH_FOLDS
   Assert(this->ReloadOperand != nullptr);
    Assert(this->PendingInstruction != nullptr);

    // remove reload bit if any

    this->SpillKinds &= ~(GraphColor::SpillKinds::Reload);

    this->SpillKinds |= GraphColor::SpillKinds::FoldedReload;
#endif // ARCH_WITH_FOLDS
   assert(0 && "nyi");
}

llvm::MachineInstr *
getNextInstrAcrossBlockEnd
(
   llvm::MachineInstr * instruction
)
{
   llvm::MachineBasicBlock * mbb = instruction->getParent();
   assert(mbb != nullptr);
   llvm::MachineBasicBlock::succ_iterator succ = mbb->succ_begin();
   assert(succ != mbb->succ_end());
   if (!(*succ)->empty()) {
      return &((*succ)->instr_front());
   }

   return nullptr;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for spill optimizer.
//
// Arguments:
//
//    allocator - The allocator creating the spill optimizer.
//
// Returns:
//
//    New spill optimizer object.
//
//------------------------------------------------------------------------------

GraphColor::SpillOptimizer *
SpillOptimizer::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::SpillOptimizer * spillOptimizer = new GraphColor::SpillOptimizer();
   GraphColor::AliasTagToSpillRecordIterableMap * spillMap
      = GraphColor::AliasTagToSpillRecordIterableMap::New(256);

   llvm::SparseBitVector<> * regionTrackedBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * spilledTagBitVector = new llvm::SparseBitVector<>();

    // Operands equal scratch bit vector - tracks linked operands as
    // dictated by the legalize.

   llvm::SparseBitVector<> * operandsEqualOperandBitVector = new llvm::SparseBitVector<>();

   spillOptimizer->Allocator = allocator;
   spillOptimizer->VrInfo = allocator->VrInfo;
   spillOptimizer->RegionTrackedBitVector = regionTrackedBitVector;
   spillOptimizer->SpillMap = spillMap;
   spillOptimizer->SpilledTagBitVector = spilledTagBitVector;
   spillOptimizer->OperandsEqualOperandBitVector = operandsEqualOperandBitVector;

   // default spill iteration limit, can be overridden in debug using "SpillShareIterationLimit" control

   spillOptimizer->SpillShareIterationLimit = 10;

   spillOptimizer->EBBShare = true;

   // Initialize sub-optimization for slot wise transformations.
   GraphColor::SlotWiseOptimizer * slotWiseOptimizer = GraphColor::SlotWiseOptimizer::New(spillOptimizer);

   spillOptimizer->SlotWiseOptimizer = slotWiseOptimizer;

   // Initialize the spill optimizer local scratch bit vectors.

   llvm::SparseBitVector<> * scratchBitVector1 = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * scratchBitVector2 = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();

   spillOptimizer->ScratchBitVector1 = scratchBitVector1;
   spillOptimizer->ScratchBitVector2 = scratchBitVector2;
   spillOptimizer->LiveBitVector = liveBitVector;
   spillOptimizer->inFunctionSpilledLRs = 0;

   return spillOptimizer;
}

//TODO: temporary maps in place of an unsigned field associated with each llvm::MachineInstr
std::map<llvm::MachineInstr*, GraphColor::SpillKinds>  SpillOptimizer::instruction2SpillKind;
std::map<llvm::MachineInstr*, unsigned>  SpillOptimizer::instruction2InstrCount;

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize spill optimizer for a walk of the global scope.
//
// Arguments:
//
//    allocator - Global register allocator scope.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Initialize
(
   GraphColor::Allocator * allocator
)
{
   this->Tile = nullptr;

   this->AvailableExpressions = allocator->GlobalAvailableExpressions;

   this->InstructionShareLimit = Tiled::TypeConstants::MaxUInt;

   this->InstructionCount = 1;

   GraphColor::SlotWiseOptimizer * slotWiseOptimizer = this->SlotWiseOptimizer;

   slotWiseOptimizer->Initialize(allocator);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize spill optimizer for a given tile.
//
// Arguments:
//
//    tile - Tile context.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Initialize
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator * allocator = tile->Allocator;

   this->Tile = tile;

   this->AvailableExpressions = tile->AvailableExpressions;

   unsigned constrainedInstructionShareStartIteration = this->ConstrainedInstructionShareStartIteration;
   unsigned edgeProbabilityInstructionShareStopIteration = this->EdgeProbabilityInstructionShareStopIteration;
   unsigned crossCallInstructionShareStopIteration = this->CrossCallInstructionShareStopIteration;

   double  decayRate = this->InstructionShareDecayRateValue;

   unsigned iteration = tile->Iteration;

   if (allocator->Pass == GraphColor::Pass::Allocate) {
      // Set instruction share limit.

      if (iteration < constrainedInstructionShareStartIteration) {
         if (tile->ExtendedBasicBlockCount > 1) {
            this->InstructionShareLimit = Tiled::TypeConstants::MaxUInt;
         } else {
            // If we allocating a single EBB we're working without natural split points.  So reduce the share
            // window and disallow sharing across calls.  This will ensure that we converge quickly.
            // (case is likely on the smaller side so a more conservative approach is more warranted)

            unsigned singleEBBHeuristicLimit = (tile->InstructionCount / 2);
            this->InstructionShareLimit = ((singleEBBHeuristicLimit > 1)? singleEBBHeuristicLimit : 2);

            // set up this iteration to stop sharing cross calls since we have no other natural split points.

            crossCallInstructionShareStopIteration = iteration;
         }

      } else if (iteration == constrainedInstructionShareStartIteration) {
         assert(this->InitialInstructionShareLimit > 0);

         if (tile->InstructionCount > this->InitialInstructionShareLimit) {
            this->InstructionShareLimit = this->InitialInstructionShareLimit;
         } else {
            // small function opt out, reduces useless iterations.

            unsigned smallFunctionHeuristicLimit = (tile->InstructionCount / 3);
            this->InstructionShareLimit = ((smallFunctionHeuristicLimit > 1)? smallFunctionHeuristicLimit : 2);
         }

      } else if (this->InstructionShareLimit <= 2) {
         this->InstructionShareLimit = 2;

      } else {
         // Drop by half each subsequent iteration stopping at 2

         unsigned limit = static_cast<unsigned>(this->InstructionShareLimit * decayRate);
         this->InstructionShareLimit = ((limit < 2) ? 2 : limit);
      }

      if ((iteration >= constrainedInstructionShareStartIteration)
          && (iteration < edgeProbabilityInstructionShareStopIteration)) {
         this->DoShareByEdgeProbability = true;
      } else {
         this->DoShareByEdgeProbability = false;
      }


      if (iteration >= crossCallInstructionShareStopIteration) {
         this->DoShareAcrossCalls = false;
      } else {
         this->DoShareAcrossCalls = true;
      }

      this->InstructionCount = 1;
   }

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete instruction safely.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::DeleteInstruction
(
   llvm::MachineInstr * instruction,
   llvm::MachineFunction * MF
)
{
    assert(MF);

    SpillOptimizer::ResetInstrSpillMarkings(instruction);
    MF->DeleteMachineInstr(instruction);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Remove instruction safely.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::RemoveInstruction
(
   llvm::MachineInstr * instruction
)
{
   SpillOptimizer::ResetInstrSpillMarkings(instruction);
   if (instruction->getParent()) {
      instruction->eraseFromParent();
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Remove all current spill optimizer state.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::RemoveAll()
{
    GraphColor::AliasTagToSpillRecordIterableMap * spillMap = this->SpillMap;

    assert(this->RegionTrackedBitVector->empty());

    // Clear any state from the map;
    spillMap->RemoveAll();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Reset all spill context
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Reset()
{
   this->RegionTrackedBitVector->clear();
   this->SpilledTagBitVector->clear();

   // Actually delete the spill records in the map.

   GraphColor::SpillRecord *  spillRecord = this->SpillMap->GetFirst();

   // foreach_record_in_spillmap
   for (; spillRecord != nullptr; spillRecord = spillRecord->nextIterationHashItem)
   {
      GraphColor::SpillRecord * nextRecord = spillRecord;

      while (nextRecord != nullptr)
      {
         GraphColor::SpillRecord * nextToNextRecord = nextRecord->Next;
         nextRecord->Delete();
         nextRecord = nextToNextRecord;
      }
   }

   this->RemoveAll();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Note completed instruction.
//
// Notes:
//
//    Called from both cost and spill loops.  Used to count instructions and reset sharing.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Finish
(
   llvm::MachineInstr *  instruction,
   GraphColor::Tile *    tile,
   bool                  doInsert
)
{
   if (instruction->isCall() && !this->DoShareAcrossCalls) {
      // push all pending defs 'back' far enough that they can't be shared.
      this->InstructionCount += this->InstructionShareLimit;
   } else {
      this->InstructionCount++;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear spill record from spill map.
//
// Notes:
//
//    Function has a funny-ish name because of some fxcop/managed problem.  This name avoids the problem.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ClearMap
(
   GraphColor::SpillRecord * spillRecord
)
{
   Tiled::VR::Info *                                vrInfo = this->Allocator->VrInfo;
   unsigned                                         spillTag = spillRecord->AliasTag;
   GraphColor::AliasTagToSpillRecordIterableMap *   spillMap = this->SpillMap;

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_may_partial_alias_of_tag(tag, spillTag, aliasInfo)
   //{
        spillMap->Remove(spillTag);
   //}
   //next_may_partial_alias_of_tag;

   if (spillRecord->HasRecalculate()) {
      llvm::MachineOperand * recalculateOperand = spillRecord->RecalculateOperand;
      assert(recalculateOperand->isReg());
      unsigned recalcTag = vrInfo->GetTag(recalculateOperand->getReg());

      spillMap->Remove(recalcTag, false);
   }

   if (spillRecord->HasReload()) {
      llvm::MachineOperand * reloadOperand = spillRecord->ReloadOperand;
      assert(reloadOperand->isReg());
      unsigned reloadTag = vrInfo->GetTag(reloadOperand->getReg());

      spillMap->Remove(reloadTag, false);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add spill record in the  spill map.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Map
(
   GraphColor::SpillRecord * spillRecord
)
{
   Tiled::VR::Info *                                vrInfo = this->Allocator->VrInfo;
   unsigned                                         spillTag = spillRecord->AliasTag;
   GraphColor::AliasTagToSpillRecordIterableMap *   spillMap = this->SpillMap;

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_may_partial_alias_of_tag(tag, spillTag, aliasInfo)
   //{
        spillMap->Insert(spillTag, spillRecord);
   //}
   //next_may_partial_alias_of_tag;

   if (spillRecord->HasRecalculate()) {
      llvm::MachineOperand * recalculateOperand = spillRecord->RecalculateOperand;
      assert(recalculateOperand->isReg());
      unsigned recalcTag = vrInfo->GetTag(recalculateOperand->getReg());

      spillMap->Insert(recalcTag, spillRecord);
   }

   if (spillRecord->HasReload()) {
      llvm::MachineOperand * reloadOperand = spillRecord->ReloadOperand;
      assert(reloadOperand->isReg());
      unsigned reloadTag = vrInfo->GetTag(reloadOperand->getReg());

      spillMap->Insert(reloadTag, spillRecord);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert spill record.
//
// Arguments:
//
//    spillRecord - Spill record being inserted.
//
// Notes:
//
//    Record is pushed on the appropriate list if needed and mapped into the spill map.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Insert
(
   GraphColor::SpillRecord * spillRecord
)
{
    int spillTag = spillRecord->AliasTag;

    // Add new record superseding a dominating record if present.

    GraphColor::AliasTagToSpillRecordIterableMap *  spillMap = this->SpillMap;
    GraphColor::SpillRecord *                       currentRecord = spillMap->Lookup(spillTag);

    if (currentRecord != nullptr) {
        this->ClearMap(currentRecord);

        spillRecord->Next = currentRecord;
    }

    this->Map(spillRecord);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get prior recalculation if one is available.
//
// Arguments:
//
//    operand - Operand being recalculated.
//
// Returns:
//
//    Available recalculate operand
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetRecalculate
(
   llvm::MachineOperand * operand
)
{
   assert(operand->isReg());
   unsigned   aliasTag = this->VrInfo->GetTag(operand->getReg());

   if (!this->DoSpillSharing(operand)) {
      return nullptr;
   }

   llvm::MachineInstr *  instruction = operand->getParent();
   assert(instruction != nullptr);

   GraphColor::SpillRecord *  spillRecord =
      this->GetDominating(aliasTag, instruction, GraphColor::SpillKinds::Recalculate);

   if ((spillRecord != nullptr) && this->InRange(spillRecord, GraphColor::SpillKinds::Recalculate)) {
      return spillRecord->RecalculateOperand;
   } else {
      return nullptr;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a new definition from a recalculate and track the new def.
//
// Arguments:
//
//    apperanceOperand - Appearance the new definition is for. May be use or def.
//    instruction      - Instruction with the new definition.
//    tile             - Tile context.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::InsertRecalculate
(
   llvm::MachineOperand *  newInstanceOperand,
   llvm::MachineOperand *  appearanceOperand,
   llvm::MachineInstr *    pendingInstruction
)
{
   llvm::MachineBasicBlock *  basicBlock = pendingInstruction->getParent();
   assert(basicBlock != nullptr);

   GraphColor::SpillRecord *  spillRecord = this->GetSpillRecord(appearanceOperand, basicBlock);

   spillRecord->RecalculateOperand = newInstanceOperand;
   spillRecord->PendingInstruction = pendingInstruction;

   spillRecord->SetRecalculate();

   spillRecord->RecalculateInstructionCount = this->InstructionCount;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get current available reload for this operand if any.
//
// Arguments:
//
//    operand - Appearance being reloaded.
//
// Returns:
//
//    Available reload if any.
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetReload
(
   llvm::MachineOperand *  operand,
   Tiled::Cost&            cost,
   bool                    doInsert
)
{
    if (!this->DoSpillSharing(operand)) {
        return nullptr;
    }

    llvm::MachineInstr * instruction = operand->getParent();
    assert(instruction != nullptr);
    assert(operand->isReg());
    unsigned             aliasTag = this->VrInfo->GetTag(operand->getReg());


    if (SpillOptimizer::IsCalleeSaveTransfer(instruction)) {
        return nullptr;
    }

    GraphColor::SpillKinds kinds = GraphColor::SpillKinds( unsigned(GraphColor::SpillKinds::Reload)
#ifdef ARCH_WITH_FOLDS
                                                         | unsigned(GraphColor::SpillKinds::FoldedSpill)
                                                         | unsigned(GraphColor::SpillKinds::FoldedReload)
#endif
                                                         );

    GraphColor::SpillRecord * spillRecord = this->GetDominating(aliasTag, instruction, kinds);

    if ((spillRecord != nullptr) && this->InRange(spillRecord, GraphColor::SpillKinds::Reload)) {

        if (spillRecord->HasReload() && !spillRecord->IsNoReloadShare()) {
            llvm::MachineOperand *  reloadOperand = spillRecord->ReloadOperand;
            //Types::Type ^         reloadType = reloadOperand->Type;
            //Types::Type ^         useType = operand->Type;
            unsigned                useRegister = operand->getReg();
            //Tiled::BitSize          reloadBitSize = reloadType->BitSize;

            //Assert(reloadBitSize <= reloadOperand->Register->BitSize);

            //Tiled::BitOffset useOffset = useRegister->BitOffset;
            //Tiled::BitSize   useBitSize = useType->BitSize + useOffset;
            Tiled::Cost      hoistCost = this->Allocator->ZeroCost;

#ifdef ARCH_WITH_FOLDS
            if (spillRecord->IsFoldedReload()) {
                Tiled::Cost reloadHoistCost;

                assert(spillRecord->HasFoldedReload());
                llvm::MachineOperand * /* ::MemoryOperand ^ */ spillOperand = spillRecord->FoldedReloadOperand;

                assert((spillRecord->PendingInstruction != nullptr)
                    /* && (spillRecord->PendingInstruction != spillRecord->PendingInstruction->DummyInstruction) ???*/ );

                llvm::MachineInstr * insertInstruction = spillRecord->PendingInstruction;

                // Hoist the folded reload and compute the cost.
                reloadHoistCost = this->ComputeFoldedReload(reloadOperand, spillOperand, insertInstruction, doInsert);
                hoistCost.IncrementBy(&reloadHoistCost);

                spillRecord->SetReload();
            }

            if (spillRecord->IsFoldedSpill()) {
                Tiled::Cost spillHoistCost;

                assert(spillRecord->HasSpill());
                llvm::MachineOperand * /*was: ::MemoryOperand ^ */ spillOperand = spillRecord->SpillOperand;

                assert((spillRecord->PendingInstruction != nullptr)
                   /* && (spillRecord->PendingInstruction != spillRecord->PendingInstruction->DummyInstruction) ???*/ );

                llvm::MachineInstr * insertInstruction = spillRecord->PendingInstruction;

                // If we're hoisting a fold in our own block we can use the normal processing for pending spills
                // otherwise we have to go conservative and insert the spill directly.

                bool canMakePending = (instruction->getParent() == insertInstruction->getParent());

                // Hoist the folded spill and compute the cost.
                spillHoistCost = this->ComputeFoldedSpill(aliasTag, reloadOperand, spillOperand, insertInstruction,
                   canMakePending, doInsert);
                hoistCost.IncrementBy(&spillHoistCost);

                spillRecord->SetSpill();
            }
#endif // ARCH_WITH_FOLDS

            // If the definition does not totally overlap the use then we can be left in the state where there is
            // liveness stretching back to the entry.  This can block convergence if the number of conflict edges
            // grows to large so omit sharing in this case.  (We still will get one shot in the first iteration
            // to get a color for these appearances but go conservative immediately once we start spilling) Check
            // the basic extent of the definition versus the use and see if the overlap.  Note: this is called
            // during spilling so the def and use may have different tags (def already replaced with a spill
            // temporary).

            // <place for code supporting architectures with sub-registers>

            cost = hoistCost;

            assert(reloadOperand->getParent() != nullptr);
            return reloadOperand;
        }
    }

    // Default return value of nullptr
    return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a new definition from a reload/spill and track the new def for the spill region.
//
// Arguments:
//
//    apperanceOperand - Appearance the new definition is for. May be use or def.
//    instruction      - Instruction with the new definition.
//    tile             - Tile context.
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::InsertReloadInternal
(
   llvm::MachineOperand *     newInstanceOperand,
   llvm::MachineOperand *     appearanceOperand,
   llvm::MachineInstr *       pendingInstruction,
   bool                       isOriginal
)
{
    llvm::MachineBasicBlock * basicBlock = pendingInstruction->getParent();
    assert(basicBlock != nullptr);
    GraphColor::SpillRecord * spillRecord = this->GetSpillRecord(appearanceOperand, basicBlock);

    // track whether this is a definition in the original program or one that was synthesized by the allocator.

    spillRecord->ReloadOperand = newInstanceOperand;
    spillRecord->IsOriginalDefinition = isOriginal;
    spillRecord->PendingInstruction = pendingInstruction;

    // If we've inserted a new reload - we've committed to previous folds.

#ifdef ARCH_WITH_FOLDS
    spillRecord->ClearFoldedSpill();
#endif

    spillRecord->SetReload();
    spillRecord->ReloadInstructionCount = this->InstructionCount;

    return spillRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a folded reload record
//
// Arguments:
//
//    newInstanceOperand - The hoist temporary with appropriate register and type
//    appearanceOperand  - Original appearance operand 
//    spillOperand       - Current spill operand on the given instruction.
//
// Notes:
//
//    Inserts a reload record tracking a current folded reload together with the needed info to hoist it back
//    out for reuse of we discover a subsequent use.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::InsertFoldedReload
(
   llvm::MachineOperand *       newInstanceOperand,
   llvm::MachineOperand *       appearanceOperand,
   llvm::MachineOperand *       spillOperand,
   llvm::MachineInstr *         pendingInstruction
)
{
#ifdef ARCH_WITH_FOLDS
   GraphColor::Tile *        tile = this->Tile;
    unsigned                  appearanceTag = this->VrInfo->GetTag(appearanceOperand->getReg());
    GraphColor::LiveRange *   liveRange = tile->GetLiveRange(appearanceTag);
    unsigned                  liveRangeTag = liveRange->VrTag;
    llvm::SparseBitVector<> * regionTrackedBitVector = this->RegionTrackedBitVector;
    GraphColor::SpillRecord * spillRecord =
       this->InsertReloadInternal(newInstanceOperand, appearanceOperand, pendingInstruction, false);

    // Mark record as pending and add the spill memory operand of the
    // current folded reload

    spillRecord->FoldedReloadOperand = spillOperand;

    //?? Assert(pendingInstruction != pendingInstruction->DummyInstruction);
    spillRecord->PendingInstruction = pendingInstruction;

    spillRecord->SetFoldedReload();

    // Track spills using the single live range tracking tag.
    regionTrackedBitVector->set(liveRangeTag);
#endif
    assert(0 && "nyi");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a folded spill record
//
// Arguments:
//
//    newInstanceOperand - The hoist temporary with appropriate register and type
//    appearanceOperand  - Original appearance operand 
//    spillOperand       - Current spill operand on the given instruction.
//
// Notes:
//
//    Inserts a reload record tracking a current folded reload together with the needed info to hoist it back
//    out for reuse of we discover a subsequent use.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::InsertFoldedSpill
(
   llvm::MachineOperand *       newInstanceOperand,
   llvm::MachineOperand *       appearanceOperand,
   llvm::MachineOperand *       spillOperand,
   llvm::MachineInstr *         pendingInstruction
)
{
#ifdef ARCH_WITH_FOLDS
   assert(pendingInstruction != nullptr);

   GraphColor::Tile *          tile = this->Tile;
   Tiled::VR::Info *           vrInfo = this->VrInfo;
   assert(appearanceOperand->isReg());
   unsigned                    appearanceTag = vrInfo->GetTag(appearanceOperand->getReg());
   GraphColor::LiveRange *     liveRange = tile->GetLiveRange(appearanceTag);
   assert(liveRange != nullptr);
   unsigned                    liveRangeTag = liveRange->VrTag;
   llvm::MachineBasicBlock *   basicBlock = pendingInstruction->getParent();
   assert(basicBlock != nullptr);
   llvm::SparseBitVector<> *   regionTrackedBitVector = this->RegionTrackedBitVector;

   // BitVector::Sparse ^     spilledTagBitVector = this->SpilledTagBitVector;

   GraphColor::SpillRecord *  spillRecord = this->GetSpillRecord(appearanceOperand, basicBlock);

   // Track last definition and the backing store.

   spillRecord->ReloadOperand = newInstanceOperand;
   spillRecord->SpillOperand = spillOperand;
   spillRecord->PendingInstruction = pendingInstruction;

   spillRecord->SetFoldedSpill();
   spillRecord->HasPending = true;

   // Force a remapping since we've added info
   this->Map(spillRecord);

   // Track spills using the single live range tracking tag.
   regionTrackedBitVector->set(liveRangeTag);

   spillRecord->ReloadInstructionCount = this->InstructionCount;
#endif
   assert(0 && "nyi");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get pending spill definition point for the passed spill.
//
// Arguments:
//
//    spillTag     - spilling live range tag.
//    spillOperand - Backing memory for this spill.
//
// Note:
//
//    Pending spills are tracked in a bit vector by their live range alias tag.
//
// Returns:
//
//    Spill point definition.
//
//------------------------------------------------------------------------------

llvm::MachineOperand *  //IR::VariableOperand*
SpillOptimizer::GetPendingSpill
(
   unsigned                                            spillTag,
   llvm::MachineOperand **                             spillOperand,
   llvm::MachineInstr *                                instruction
)
{
   GraphColor::SpillRecord * spillRecord = this->GetDominating(spillTag, instruction, GraphColor::SpillKinds::Spill);

   // walk back and find a spill marked pending if any
   GraphColor::SpillKinds kinds = GraphColor::SpillKinds(unsigned(GraphColor::SpillKinds::Spill)
#ifdef ARCH_WITH_FOLDS
                                                       | unsigned(GraphColor::SpillKinds::FoldedSpill)
#endif
                                                        );
  
   spillRecord = this->GetPending(spillRecord, kinds);

   assert(spillRecord != nullptr && spillRecord->HasSpill() && spillRecord->HasPending);

   llvm::MachineOperand *  reloadOperand = spillRecord->ReloadOperand;
   *spillOperand = spillRecord->SpillOperand;

   return reloadOperand;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get spill record for the appearance.
//
// Arguments:
//
//    apperanceOperand - Appearance about which we want to record some spill information.
//
// Notes:
//
//    May return a currently mapped spill record or create a new one.  There can be at most one spill record
//    for a given tag in a given block.  Different blocks require different spill records.
//
// Returns:
//
//    Appearance spill record (unmapped)
//
//------------------------------------------------------------------------------


GraphColor::SpillRecord *
SpillOptimizer::GetSpillRecord
(
   unsigned                  appearanceTag,
   llvm::MachineBasicBlock * basicBlock
)
{
   assert(basicBlock != nullptr);

   // Get current record or create new.
   GraphColor::SpillRecord * spillRecord = this->SpillMap->Lookup(appearanceTag);

   if ((spillRecord == nullptr) || (spillRecord->BasicBlock != basicBlock)) {
      unsigned liveRangeTag;

      if (spillRecord != nullptr) {
         // Reuse live range tag from previous spill record if we just need a new record for a new block.
         // This avoids the lookup.

         liveRangeTag = spillRecord->AliasTag;
      } else {
         GraphColor::LiveRange * liveRange = this->GetLiveRange(appearanceTag);
         assert(liveRange != nullptr);

         liveRangeTag = liveRange->VrTag;
      }

      spillRecord = GraphColor::SpillRecord::New(this);

      spillRecord->AliasTag = liveRangeTag;
      spillRecord->BasicBlock = basicBlock;

      this->Insert(spillRecord);
   }

   return spillRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a new definition as a spill point
//
// Arguments:
//
//    newInstanceOperand - New inserted definition of the point lifetime of the spill.
//    apperanceOperand   - Appearance the new definition is for.
//    spillOperand       - Backing memory for this spill.
//    pendingInstruction - Definition instruction.
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::InsertSpillInternal
(
   llvm::MachineOperand *                          definitionOperand,
   unsigned                                        appearanceTag,
   llvm::MachineOperand *                          spillOperand,
   llvm::MachineInstr *                            pendingInstruction
)
{
   GraphColor::Tile *         tile = this->Tile;
   GraphColor::LiveRange *    liveRange = tile->GetLiveRange(appearanceTag);
   unsigned                   liveRangeTag = liveRange->VrTag;
   assert(liveRange != nullptr);
   llvm::SparseBitVector<> *  regionTrackedBitVector = this->RegionTrackedBitVector;

   llvm::MachineBasicBlock *  basicBlock = pendingInstruction->getParent();
   assert(basicBlock != nullptr);

   GraphColor::SpillRecord *  spillRecord = this->GetSpillRecord(appearanceTag, basicBlock);

   // Track last definition and the backing store.

   spillRecord->ReloadOperand = definitionOperand;
   spillRecord->SpillOperand = spillOperand;

   spillRecord->SetSpill();
   spillRecord->SetReload(); // definition may be used as a reload point.

   // Force a remapping since we've added info
   this->Map(spillRecord);

   // Track spills using the single live range tracking tag.

   regionTrackedBitVector->set(liveRangeTag);

   spillRecord->ReloadInstructionCount = this->InstructionCount;

   return spillRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a new definition as a committed spill point
//
// Arguments:
//
//    newInstanceOperand - New inserted definition of the point lifetime of the spill.
//    apperanceOperand   - Appearance the new definition is for.
//    spillOperand       - Backing memory for this spill.
//    pendingInstruction - Definition instruction o
//
//------------------------------------------------------------------------------

void
SpillOptimizer::InsertCommittedSpill
(
   llvm::MachineOperand *                          definitionOperand,
   unsigned                                        appearanceTag,
   llvm::MachineOperand *                          spillOperand,
   llvm::MachineInstr *                            pendingInstruction
)
{
   GraphColor::SpillRecord * spillRecord
      = this->InsertSpillInternal(definitionOperand, appearanceTag, spillOperand, pendingInstruction);

   this->SpilledTagBitVector->set(spillRecord->AliasTag);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert a new definition as a pending spill point
//
// Arguments:
//
//    newInstanceOperand - New inserted definition of the point lifetime of the spill.
//    apperanceOperand   - Appearance the new definition is for.
//    spillOperand       - Backing memory for this spill.
//    pendingInstruction - Definition instruction o
//
//------------------------------------------------------------------------------

void
SpillOptimizer::InsertPendingSpill
(
   llvm::MachineOperand *                          definitionOperand,
   unsigned                                        appearanceTag,
   llvm::MachineOperand *                          spillOperand,
   llvm::MachineInstr *                            pendingInstruction
)
{
   GraphColor::SpillRecord * spillRecord
      = this->InsertSpillInternal(definitionOperand, appearanceTag, spillOperand, pendingInstruction);

   // Mark as pending
   spillRecord->HasPending = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Process a pending spill for given spill location and an instruction point.
//
// Arguments:
//
//    spillAliasTag - Tag for location being spilled.
//    instruction   - Instruction that pending spill must dominate.
//    doInsert      - Do inserts or not for costing/spilling
//
// Notes:
//
//    Directly updates live range costs rather than returning a cost.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ProcessPendingSpill
(
   unsigned             spillAliasTag,
   llvm::MachineInstr * instruction,
   bool                 doInsert
)
{
   if (!this->IsTracked(spillAliasTag)) {
      return;
   }

   GraphColor::SpillKinds kinds = GraphColor::SpillKinds(unsigned(GraphColor::SpillKinds::Spill)
#ifdef ARCH_WITH_FOLDS
                                                       | unsigned(GraphColor::SpillKinds::FoldedSpill)
#endif
                                                        );

   GraphColor::SpillRecord * spillRecord = this->GetDominating(spillAliasTag, instruction, kinds);
   if ((spillRecord == nullptr) || (spillRecord->HasPending == false)) {
      return;
   }

   spillRecord = this->GetPending(spillRecord, kinds);
   if (spillRecord == nullptr) {
      return;
   }

   assert(spillRecord->HasPending);

   unsigned liveRangeAliasTag = spillRecord->AliasTag;

   if (spillRecord->IsSpill()) {
      GraphColor::Tile *      tile = this->Tile;
      GraphColor::LiveRange * liveRange = tile->GetLiveRange(liveRangeAliasTag);

      assert(liveRangeAliasTag == liveRange->VrTag);

      llvm::MachineOperand *  spillOperand;
      llvm::MachineOperand *  registerOperand
         = this->GetPendingSpill(liveRangeAliasTag, &spillOperand, instruction);
      llvm::MachineInstr *       spillInstruction = registerOperand->getParent();
      Tiled::Cost                totalSpillCost;

      assert(spillInstruction != nullptr);

      Tiled::Cost spillCost = this->ComputePendingSpill(registerOperand, liveRangeAliasTag, spillOperand,
         spillInstruction, doInsert);

      totalSpillCost = liveRange->SpillCost;
      totalSpillCost.IncrementBy(&spillCost);
      liveRange->SpillCost = totalSpillCost;
   }

   // Folded spills are already set up and just need the bit cleared.
   this->ClearPendingSpill(liveRangeAliasTag, instruction);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Process any pending spills for given instruction point.
//
// Arguments:
//
//    instruction   - Instruction that the pending spills must dominate.
//    doInsert      - Do inserts or not for costing/spilling
//
// Notes:
//
//    Directly updates live range costs rather than returning a cost.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ProcessPendingSpills
(
   llvm::MachineInstr * instruction,
   bool                 doInsert
)
{
   llvm::SparseBitVector<> * regionTrackedBitVector = this->RegionTrackedBitVector;

   llvm::SparseBitVector<>::iterator t;

   // foreach_sparse_bv_bit
   for (t = regionTrackedBitVector->begin(); t != regionTrackedBitVector->end(); ++t)
   {
      unsigned spillTag = *t;
      this->ProcessPendingSpill(spillTag, instruction, doInsert);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Process pending spills at an extended basic bock exit.
//
// Arguments:
//
//    lastInstruction  - Last instruction in the block with the exit
//    block            - Entry block of new EBB
//    doInsert         - Perform the insert
//
// Notes:
//
//    Process any pending spills that are live at the entry of the EBB we're exiting to.  This gives a much
//    smaller set.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ProcessPendingSpills
(
   llvm::MachineInstr *      lastInstruction,
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   bool                      doInsert
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   Tiled::VR::Info *          aliasInfo = this->VrInfo;
   llvm::SparseBitVector<> *  regionTrackedBitVector = this->RegionTrackedBitVector;
   Dataflow::LivenessData *   registerLivenessData = allocator->Liveness->GetRegisterLivenessData(block);
   llvm::SparseBitVector<> *  liveInBitVector = registerLivenessData->LiveInBitVector;
   unsigned                   spillTag;

   assert(!this->EBBShare || Graphs::ExtendedBasicBlockWalker::IsEntry(block)
          || tile->IsExitBlock(block) || tile->IsNestedExitBlock(block));

   // <place for an EH-related code (dangling instructions)>

   llvm::SparseBitVector<> *  idVector = this->ScratchBitVector1;
   idVector->clear();

   llvm::SparseBitVector<>::iterator t;

   // foreach_sparse_bv_bit
   for (t = regionTrackedBitVector->begin(); t != regionTrackedBitVector->end(); ++t)
   {
      spillTag = *t;

      // Process any pending spills that reach out of this block. (in pending spill set and has a spill in
      // this block.

      if (aliasInfo->CommonMayPartialTags(spillTag, liveInBitVector)) {
         GraphColor::LiveRange * liveRange = this->Tile->GetLiveRange(spillTag);

         if (liveRange != nullptr) {
            idVector->set(liveRange->Id);
         }
      }
   }

   // Compute the set of live range IDs (above) and then spill the live
   // ranges for this tile in that order (below) since iterating by alias tag
   // is not very deterministic.

   llvm::SparseBitVector<>::iterator id;

   // foreach_sparse_bv_bit
   for (id = idVector->begin(); id != idVector->end(); ++id)
   {
      unsigned spillRangeId = *id;

      GraphColor::LiveRange * liveRange = this->Tile->GetLiveRangeById(spillRangeId);

      spillTag = liveRange->VrTag;
      this->ProcessPendingSpill(spillTag, lastInstruction, doInsert);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    UpdatePendingSpills - Update current pending spill records to
//    reflect the updated spill location via inheritance.
//
// Arguments:
//
//    spillTag                 - Alias tag for the spill
//    inheritedSpillOperand    - Inherited memory backing store for spill/reload.
//    instruction              - Instruction from which the update is occuring.
//
// Returns:
//
//    true if updated
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::UpdatePendingSpills
(
   unsigned                spillTag,
   llvm::MachineOperand *  inheritedSpillOperand,
   llvm::MachineInstr *    instruction
)
{
   bool hasUpdate = false;

   // Make sure we haven't commited to a spill.
   assert(!this->SpilledTagBitVector->test(spillTag));

   // Walk the spill records asserting that all spills are pending and
   // making sure they are using the inheritedSpillOperand.

   GraphColor::SpillKinds kinds = GraphColor::SpillKinds(unsigned(SpillKinds::Spill)
#ifdef ARCH_WITH_FOLDS
                                                       | unsigned(SpillKinds::FoldedSpill)
#endif
                                                        );
   GraphColor::SpillRecord * spillRecord = this->GetDominating(spillTag, instruction, kinds);

   while (spillRecord != nullptr)
   {
      assert(spillRecord->IsSpill() /*ARCH_WITH_FOLDS: || spillRecord->IsFoldedSpill()*/);
      assert(spillRecord->HasPending);

      llvm::MachineOperand *  spillOperand = spillRecord->SpillOperand;
      assert(spillOperand != nullptr);

      if (!(inheritedSpillOperand->isIdenticalTo(*spillOperand))) {
         hasUpdate = true;

         if (spillOperand->getParent() == nullptr) {
            delete spillOperand;  //TODO: spillOperand->Delete()
         }

         // Prior pending spill relying on a previous spill operand.
         // Update in case we need to use this instruction on a
         // subsequent path.

         assert(this->IsMemoryOperand(inheritedSpillOperand));
         spillRecord->SpillOperand = this->CopyOperand(inheritedSpillOperand);
      }

      // Walk back through spills records.
      spillRecord = spillRecord->Next;
   }

   return hasUpdate;
}

llvm::MachineOperand *
SpillOptimizer::CopyOperand
(
   const llvm::MachineOperand * operand
)
{
   llvm::MachineOperand * newOperand = new llvm::MachineOperand(*operand);
   newOperand->clearParent();

   return newOperand;
}

llvm::MachineOperand *
SpillOptimizer::replaceAndUnlinkOperand
(
   llvm::MachineInstr *     instruction,
   llvm::MachineOperand **  oldOperand,
   llvm::MachineOperand *   newOperand
)
{
   //precondition: *oldOperand points to an element of instruction's Operands array
   int oldOperandIndex = this->getOperandIndex(instruction, *oldOperand);
   if (oldOperandIndex < 0) {
      assert(0 && "operand to be replaced not found on the instruction");
      return nullptr;
   }

   assert((*oldOperand)->isReg() && newOperand->isReg());

   llvm::MachineOperand * oldOperandCopy = this->CopyOperand(*oldOperand);
   (*oldOperand)->setReg(newOperand->getReg());
   *oldOperand = oldOperandCopy;

   return &(instruction->getOperand(oldOperandIndex));
}

int
SpillOptimizer::getOperandIndex
(
   llvm::MachineInstr *   instruction,
   llvm::MachineOperand * operand
)
{
   int idx = 0;
   llvm::MachineInstr::mop_iterator iter;

   for (iter = instruction->operands_begin(); iter != instruction->operands_end(); ++iter, ++idx)
   {
      if (iter == operand) {
         return idx;
      }
   }

   return -1;
}



//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeReload
//
// Arguments:
//
//    registerOperand - Reload register target operand, this is the new point lifetime.
//    sourceOperand   - Source appearance being reloaded.
//    spillOperand    - Memory backing store being reloaded from.
//    tile            - Tile context.
//    doInsert        - Leave reload IR if true.
//
// Returns:
//
//    Cost of reload code.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeReload
(
   llvm::MachineOperand **    registerOperand,
   llvm::MachineOperand *     sourceOperand,
   GraphColor::Tile *         tile,
   bool                       doInsert
)
{
   assert(sourceOperand->isReg());
   GraphColor::Allocator *     allocator = this->Allocator;
   Tiled::Cost                 cost = allocator->ZeroCost;
   Tiled::VR::Info *           vrInfo = allocator->VrInfo;

   TargetRegisterAllocInfo *   targetRegisterAllocator = allocator->TargetRegisterAllocator;
   llvm::MachineInstr *        endInstruction = sourceOperand->getParent();
   llvm::MachineInstr *        startInstruction = endInstruction->getPrevNode();
   llvm::SparseBitVector<> *   doNotSpillAliasTagBitVector = allocator->DoNotSpillAliasTagBitVector;
   bool                        isValidInMemory = false;

   unsigned                    sourceOperandTag = vrInfo->GetTag(sourceOperand->getReg());
   GraphColor::LiveRange *     liveRange = tile->GetLiveRange(sourceOperandTag);
   unsigned                    liveRangeTag = liveRange->VrTag;
   assert(liveRange != nullptr);
 
   llvm::MachineOperand *  spillOperand =
      this->GetSpillOperand(sourceOperand, liveRange, tile, isValidInMemory);

#ifdef ARCH_WITH_FOLDS
   llvm::MachineOperand * /* IR::MemoryOperand* */ foldOperand = spillOperand;
   bool                        isFoldOperandCopied = false;

   const llvm::TargetRegisterClass * sourceOperandRegisterCategory = allocator->GetRegisterCategory(sourceOperand->getReg());

   if (targetRegisterAllocator->GetBaseIntegerRegisterCategory() == sourceOperandRegisterCategory) {
      foldOperand = this->CopyOperand(spillOperand);
      //assumption/constraint: this RA spills only to stack frame slots

      isFoldOperandCopied = true;

      // keep pointer-ness when folding.  We don't do this generally because of a bad interaction with
      // memory address forms for xmm instructions

      //foldOperand->ResetField(sourceOperand->Field);
      //The above appears not available/needed in context of llvm::MachineOperand that isReg.
   }

   // See if there is a reload we can share - separate from available since we need limit the scope as well as
   // allow for speculation in the costing pass.

   // If we haven't reload and it's legal to fold do so.

   if (!this->IsReloaded(liveRangeTag, endInstruction) && this->IsLegalToReplaceOperand(sourceOperand, foldOperand)) {
      llvm::MachineInstr * pendingInstruction = sourceOperand->getParent();

      // Replace appearance with spill operand and insert pending reload (not yet hoisted)

      // Create spill temporary
      llvm::MachineOperand * temporaryOperand = this->GetSpillTemporary(sourceOperand, spillOperand, doInsert);

      if (doInsert) {
         // Replace appearance

         this->SetSpillKinds(endInstruction, SpillKinds::FoldedReload);

         foldOperand = this->replaceAndUnlinkOperand(endInstruction, &sourceOperand, foldOperand);

         if (tile->Pass == GraphColor::Pass::Allocate) {
            // Shortened lifetime of the underlying live range - mark the live range as partitioned
            liveRange->HasSpillPartition = true;
         }
      }

      // If pending spill, handle it.
      unsigned appearanceTag = vrInfo->GetTag(sourceOperand->getReg());
      this->ProcessPendingSpill(appearanceTag, pendingInstruction, doInsert);

      // Insert pending reload
      this->InsertFoldedReload(temporaryOperand, sourceOperand, foldOperand, pendingInstruction);

      Tiled::Cost foldCost = targetRegisterAllocator->EstimateMemoryCost(foldOperand);
      allocator->ScaleCyclesByFrequency(&foldCost, pendingInstruction);
      cost.IncrementBy(&foldCost);

      return cost;
   }

   if (isFoldOperandCopied && foldOperand->getParent() == nullptr) {
      delete foldOperand;
   }
#endif // ARCH_WITH_FOLDS

   // Cost the sharing of a reload

   llvm::MachineOperand * reloadOperand = this->GetReload(sourceOperand, cost, doInsert);

   if (reloadOperand != nullptr) {

      if (this->GetSpillKinds(endInstruction) == GraphColor::SpillKinds::Spill) {
         // If this is a spill inserted on a prior iteration, if the spill symbols are the same remove any
         // pending spill within the same block.

         unsigned               spillTag = vrInfo->GetTag(sourceOperand->getReg());
         llvm::MachineOperand * /*IR::MemoryOperand* */ pendingSpillOperand = nullptr;
         llvm::MachineOperand * destinationOperand = this->FindSpillMemory(endInstruction);

         assert(destinationOperand != nullptr);
         llvm::MachineOperand * destinationOperandNext = ++destinationOperand;
         bool c1 = this->IsMemoryOperand(destinationOperand);
         bool c2 = this->IsMemoryOperand(destinationOperandNext);
         assert((c1 && destinationOperandNext == (destinationOperand->getParent())->defs().end()) || !c2);

         GraphColor::SpillRecord * spillRecord = this->GetDominating(spillTag, endInstruction);

         if (spillRecord != nullptr) {
            spillRecord = this->GetPending(spillRecord, GraphColor::SpillKinds::Spill);

            if (spillRecord != nullptr) {
               unsigned sourceOperandTag = vrInfo->GetTag(sourceOperand->getReg());
               llvm::MachineOperand *  pendingOperand
                  = this->GetPendingSpill(sourceOperandTag, &pendingSpillOperand, endInstruction);

               if (pendingOperand != nullptr) {
                  llvm::MachineInstr * pendingInstruction = pendingOperand->getParent();

                  if ((pendingOperand == reloadOperand)
                     && spillOperand->isIdenticalTo(*destinationOperand)
                     && (pendingInstruction->getParent() == endInstruction->getParent())) {
                     this->ClearPendingSpill(spillTag, endInstruction);
                  }
               }
            }
         }
      }

      if (doInsert) {
         llvm::MachineOperand * reloadVariableOperand = reloadOperand;

         // Insert spill temporary in the original instruction
         this->ReplaceSpillAppearance(reloadVariableOperand, &sourceOperand, true);

         unsigned instructionShareDelta = this->InstructionShareDelta;
         if (instructionShareDelta > 1) {
            Tiled::VR::Info * vrInfo = this->VrInfo;
            assert(reloadOperand->isReg());
            unsigned reloadOperandTag = vrInfo->GetTag(reloadOperand->getReg());

            // clear doNotSpill since we just stretched the liverange of the reuse
            vrInfo->MinusMayPartialTags(reloadOperandTag, doNotSpillAliasTagBitVector);
         }

         // Set out parameter with available

         *registerOperand = reloadVariableOperand;
      }

      return cost;
   }


   llvm::MachineOperand * temporaryOperand = this->GetSpillTemporary(sourceOperand, spillOperand, doInsert);
   *registerOperand = temporaryOperand;

   // compute reload code via target allocator, cost, and insert if requested.

   //TODO: For now spills are full-word, there is no need for the 'narrowing' in GetSpillRegister
   unsigned reg = (*registerOperand)->getReg();

   unsigned appearanceTag = vrInfo->GetTag(sourceOperand->getReg());
   this->ProcessPendingSpill(appearanceTag, endInstruction, doInsert);

   if (tile->Pass == GraphColor::Pass::Allocate) {
      // Shortened lifetime of the underlying live range - mark the live range as partitioned
      liveRange->HasSpillPartition = true;
   }

   llvm::MachineOperand * destinationOperand = this->getSingleExplicitDestinationOperand(endInstruction);

   if (this->IsSpillKind(endInstruction, GraphColor::SpillKinds::Spill)
       && this->IsSpillKind(endInstruction, GraphColor::SpillKinds::CallerSave)
       && !this->IsSpillKind(endInstruction->getPrevNode(), GraphColor::SpillKinds::Reload)
       && (!doInsert || vrInfo->MustTotallyOverlap(spillOperand, destinationOperand))) {
      // This is the reload of a prior spill - so we can remove it.

      Tiled::Cost removedSpillCost = targetRegisterAllocator->EstimateInstructionCost(endInstruction);
      allocator->ScaleCyclesByFrequency(&removedSpillCost, endInstruction);
      cost.DecrementBy(&removedSpillCost);

      if (doInsert) {
         assert(destinationOperand != nullptr);
         // Remove spill
         SpillOptimizer::RemoveInstruction(endInstruction);
      }

      return cost;
   }
      
   // use a scratch copy of the register operand and spill operand when generating the spill for the no
   // insert case.  callers expect to preserve the operands they pass in.

   llvm::MachineOperand * scratchOperand = nullptr;
   llvm::MachineOperand * /*was: IR::MemoryOperand* */ scratchSpill;

   if (doInsert) {
      // Insert spill temporary in the original instruction
      this->ReplaceSpillAppearance(*registerOperand, &sourceOperand, true);

      scratchSpill = spillOperand;
   } else {
      scratchSpill = this->CopyOperand(spillOperand);
   }

   bool isTileReload = false;

   targetRegisterAllocator->Reload(scratchOperand, reg, scratchSpill, endInstruction,
      doInsert, isTileReload, liveRange->IsGlobal());
   if (doInsert) this->inFunctionSpilledLRs++;

   // Reload generates one or more instructions, get the start and end of that sequence.

   llvm::MachineInstr * firstReloadInstruction =
      (startInstruction)? startInstruction->getNextNode() : &(*(endInstruction->getParent())->instr_begin());
   llvm::MachineInstr * lastReloadInstruction = endInstruction->getPrevNode();
   llvm::MachineInstr * instruction;
   llvm::MachineInstr * nextInstr = nullptr;

   // foreach_instr_in_range_editing
   for (instruction = firstReloadInstruction; instruction != endInstruction; instruction = nextInstr)
   {
      nextInstr = instruction->getNextNode();

      // check added reload code is legal
      //instruction->MarkAsLegalized();  //under TILED_DEBUG_CHECKS

      Tiled::Cost instructionCost = targetRegisterAllocator->EstimateInstructionCost(instruction);
      allocator->ScaleCyclesByFrequency(&instructionCost, instruction);
      cost.IncrementBy(&instructionCost);

      this->SetSpillKinds(instruction, GraphColor::SpillKinds::Reload);

      if (!doInsert) {
         SpillOptimizer::ResetInstrSpillMarkings(instruction);
         instruction->removeFromParent();
      } else {
         allocator->Indexes->insertMachineInstrInMaps(*instruction);
      }

      // Insert new reload instruction into the reload table
      if (instruction == lastReloadInstruction) {
         this->InsertReload((instruction->defs()).begin() /*DestinationOperand*/, sourceOperand, endInstruction);
      }
   }

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeRecalculate
//
// Arguments:
//
//    appearanceOperand - Recalculate register target operand, this is the new point lifetime.
//    replaceOperand    - Source appearance being recalculated.
//    tile              - Tile context.
//    doInsert          - Leave reload IR if true.
//
// Returns:
//
//    Cost of reload code.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeRecalculate
(
   llvm::MachineOperand **    registerOperand,
   llvm::MachineOperand *     replaceOperand,
   GraphColor::Tile *         tile,
   const bool                 doInsert
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::Cost              cost = allocator->ZeroCost;

   TargetRegisterAllocInfo *  targetRegisterAllocator = allocator->TargetRegisterAllocator;
   llvm::MachineInstr *       instruction = replaceOperand->getParent();

   // Look up for an available recalculation as well as any current recalculate definition

   llvm::MachineOperand *  availableOperand = this->GetAvailable(replaceOperand);
   llvm::MachineOperand *  recalculateOperand = this->GetRecalculate(replaceOperand);

   // check if there is any recalculate available

   if (availableOperand == nullptr) {
      assert(!doInsert);

      // no recalculate instruction available, early out.
      return allocator->InfiniteCost;
   }

   if (replaceOperand->isDef()) {
      // Recalculate being removed - cost of this goes to zero (it's being removed)

      cost = allocator->ZeroCost;

   } else {

      if ((recalculateOperand != nullptr) && this->IsLegalToReplaceOperand(replaceOperand, recalculateOperand)) {
         // Reuse already manifest recalculate. Cost of reuse is zero.

         cost = allocator->ZeroCost;

         if (doInsert) {
            llvm::SparseBitVector<> *  doNotSpillAliasTagBitVector = allocator->DoNotSpillAliasTagBitVector;

            // Any reuse must be from a dominating instruction.
            assert(allocator->FunctionUnit->Dominates(recalculateOperand->getParent(), instruction));

            //if (recalculateOperand->Field != replaceOperand->Field) {
            // <place for code supporting architectures with sub-registers>

            this->replaceAndUnlinkOperand(instruction, &replaceOperand, recalculateOperand);
            //the old replaceOperand is no longer needed below
            if (replaceOperand->getParent() == nullptr)
               delete replaceOperand;

            int instructionShareDelta = this->InstructionShareDelta;

            if (instructionShareDelta > 1) {
               Tiled::VR::Info *  vrInfo = allocator->VrInfo;
               unsigned           aliasTag = vrInfo->GetTag(recalculateOperand->getReg());
               vrInfo->MinusMayPartialTags(aliasTag, doNotSpillAliasTagBitVector);
            }

            *registerOperand = recalculateOperand;
         }

      } else {

         assert(availableOperand != nullptr);
         llvm::MachineInstr *    availableInstruction = availableOperand->getParent();
         llvm::MachineOperand *  temporaryOperand = this->GetSpillTemporary(replaceOperand, nullptr, doInsert);

         cost = targetRegisterAllocator->EstimateInstructionCost(availableInstruction);
         allocator->ScaleCyclesByFrequency(&cost, instruction);

         // Copy tracked recalculate expression to a new instruction.

         llvm::MachineFunction * machineFunction = allocator->FunctionUnit->machineFunction;
         llvm::MachineInstr *    newInstruction = machineFunction->CloneMachineInstr(availableInstruction);

         llvm::MachineOperand *  newDestinationOperand = (newInstruction->defs()).begin();
         assert(newDestinationOperand != nullptr);
         llvm::MachineOperand *  temporaryDestinationOperand;

         // <place for code supporting architectures with sub-registers>

         temporaryDestinationOperand = temporaryOperand;

         //TODO:  newDestinationOperand->SetCannotMakeExpressionTemporary();

         newDestinationOperand->setReg(temporaryDestinationOperand->getReg());
         this->SetSpillKinds(newInstruction, SpillKinds::Recalculate);

         if (tile->Pass == GraphColor::Pass::Allocate) {
            GraphColor::LiveRange *  liveRange = tile->GetLiveRange(replaceOperand);

            // Shortened lifetime of underlying live range - mark as partitioned.
            liveRange->HasSpillPartition = true;
         }

         if (doInsert) {
            assert(replaceOperand->isUse());
            this->ReplaceSpillAppearance(temporaryOperand, &replaceOperand, true);

            // insert the recalculation
            llvm::MachineBasicBlock * mbb = instruction->getParent();
            mbb->insert(llvm::MachineBasicBlock::iterator(instruction), newInstruction);

            this->Allocator->Indexes->insertMachineInstrInMaps(*newInstruction);

            *registerOperand = temporaryOperand;
         }

         // Set up new recalculate for reuse
         this->InsertRecalculate(newDestinationOperand, replaceOperand, instruction);
      }
   }

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Tag is tracked in region.
//
// Arguments:
//
//    aliasTag - Tag to check.
//
// Returns:
//
//    True if the alias tag is being tracked for this live range in this region.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::IsTracked
(
   unsigned aliasTag
)
{
   llvm::SparseBitVector<> * regionTrackedBitVector = this->RegionTrackedBitVector;
   GraphColor::LiveRange *   liveRange = this->GetPendingLiveRange(aliasTag);

   if (liveRange == nullptr)
      return false;

   return regionTrackedBitVector->test(liveRange->VrTag);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Check if the last dominating record had this location in memory
//
// Arguments:
//
//    aliasTag - Tag to check.
//
// Returns:
//
//    True if the alias tag is in memory.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::IsReloaded
(
   unsigned             aliasTag,
   llvm::MachineInstr * instruction
)
{
   if (!this->IsTracked(aliasTag)) {
      return false;
   }

   GraphColor::SpillRecord * spillRecord = this->GetDominating(aliasTag, instruction);

   if ((spillRecord != nullptr) && !spillRecord->IsNoReloadShare()
      && this->InRange(spillRecord, GraphColor::SpillKinds::Reload)) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    GetPendingLiveRange
//
// Arguments:
//
//    aliasTag - Alias tag to check for pending live range.
//
// Returns:
//
//    liveRange or nullptr if none.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
SpillOptimizer::GetPendingLiveRange
(
   unsigned aliasTag
)
{
   GraphColor::Tile *      tile = this->Tile;
   GraphColor::LiveRange * liveRange = tile->GetLiveRange(aliasTag);

   if (liveRange == nullptr) {
      // Look up live range via spill map.  This handles inserted spill temporaries.
      GraphColor::AliasTagToSpillRecordIterableMap *  spillMap = this->SpillMap;
      GraphColor::SpillRecord *                       spillRecord = spillMap->Lookup(aliasTag);

      if (spillRecord == nullptr) {
         // If we're not tracking this live ranges at all return false.  We're done.
         return nullptr;
      }

      unsigned liveRangeTag = spillRecord->AliasTag;
      liveRange = tile->GetLiveRange(liveRangeTag);
   }

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ClearPendingSpill
//
// Arguments:
//
//    aliasTag - Tag to clear pending spills on.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ClearPendingSpill
(
   unsigned             aliasTag,
   llvm::MachineInstr * instruction
)
{
   GraphColor::SpillKinds kinds = GraphColor::SpillKinds(unsigned(GraphColor::SpillKinds::Spill)
#ifdef ARCH_WITH_FOLDS
                                                       | unsigned(GraphColor::SpillKinds::FoldedSpill)
#endif
                                                        );

   GraphColor::SpillRecord * spillRecord = this->GetDominating(aliasTag, instruction, kinds);
   spillRecord = this->GetPending(spillRecord, kinds);

   assert(this->IsTracked(aliasTag));

   if (spillRecord != nullptr) {
      // Track the fact that we've committed this tag to memory.
      this->SpilledTagBitVector->set(spillRecord->AliasTag);

#ifdef ARCH_WITH_FOLDS
      // Leave spill operand for hoisting in the folded case.
      if (!spillRecord->IsFoldedSpill()) {
         spillRecord->SpillOperand = nullptr;
      }
#else
      spillRecord->SpillOperand = nullptr;
#endif

      spillRecord->HasPending = false;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ClearReload
//
// Arguments:
//
//    aliasTag - Tag to clear pending spills on.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::ClearReload
(
   unsigned             aliasTag,
   llvm::MachineInstr * instruction
)
{
   GraphColor::SpillRecord * spillRecord = this->GetDominating(aliasTag, instruction, GraphColor::SpillKinds::Reload);

   if (spillRecord != nullptr) {
      spillRecord->ReloadOperand = nullptr;
      spillRecord->ClearReload();
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    DoSpillSharing
//
// Arguments:
//
//    operand - Operand to consider sharing for.
//
// Notes:
//
//    Function takes operand but this is more a per type/liverange decision.  A series of operands in a live
//    range for a given tile should all return the same answer to this question.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::DoSpillSharing
(
   llvm::MachineOperand * operand
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   GraphColor::Tile *      tile = this->Tile;

   // Opt out of spill sharing if we're compiling /Od or we're passed the iteration limit
   if (!this->IsGlobalScope() && (tile->Iteration > this->SpillShareIterationLimit)) {
      return false;
   }

   // do simple opt out for FP on x87  (TODO if needed)

   return this->ShareSpills;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    CanShareSpill
//
// Arguments:
//
//    destinationOperand - Definition operand to be spilled.
//    spillOperand       - Current spill backing memory. 
//
// Notes:
//
//    Requires a spill to be "pending", tracked, in the spill map for this to be true.
//
// Returns:
//
//    True if the definition can be folded into the current backing memory
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::CanShareSpill
(
   llvm::MachineOperand *                           destinationOperand,
   llvm::MachineOperand * /*IR::MemoryOperand* */   spillOperand,
   llvm::MachineInstr *                             instruction
)
{
   Tiled::VR::Info *  vrInfo = this->VrInfo;
   unsigned           appearanceTag = vrInfo->GetTag(destinationOperand->getReg());

   if (SpillOptimizer::IsCalleeSaveTransfer(instruction)) {
      return false;
   }

   if (!this->IsTracked(appearanceTag)) {
      //  No prior state, go ahead and share this def.
      return true;
   }

   GraphColor::SpillRecord * spillRecord = this->GetDominating(appearanceTag, instruction);

   if ((spillRecord != nullptr) && spillRecord->IsNoSpillShare()) {
      return false;
   }

   if ((spillRecord == nullptr) || !spillRecord->IsSpill()) {
      // If no prior state or not tracking a spill we can't sure (need to insert a store.)
      return true;
   }

   // Don't share processed spills 

   if (!spillRecord->HasPending) {
      return false;
   }

   // Try and detect cases where we could be losing information

   // If prior spill was to a larger backing location so this can't be shared.
   // <place for code supporting architectures with sub-registers>

   // No prior state was contrary to this def sharing a spill so return true.

   return true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeSpill
//
// Arguments:
//
//    registerOperand    - Reload register target operand, this is the new point lifetime.
//    destinationOperand - destination appearance being spilled.
//    spillOperand       - Memory backing store being stored to.
//    tile               - Tile context.
//    doInsert           - Leave reload IR if true.
//
// Returns:
//
//    Cost of spill code.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeSpill
(
   llvm::MachineOperand ** /*IR::VariableOperand*& */    registerOperand,
   llvm::MachineOperand *  /*IR::VariableOperand * */    destinationOperand,
   GraphColor::Tile *                                    tile,
   bool                                                  doInsert
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   Tiled::Cost               cost = allocator->ZeroCost;

   llvm::MachineOperand *   definitionOperand;
   llvm::MachineOperand *   scratchOperand;
   llvm::MachineOperand *   scratchSpill;
   bool                     isValidInMemory = false;

   Tiled::VR::Info *       vrInfo = this->VrInfo;
   assert(destinationOperand->isReg());
   unsigned                destinationTag = vrInfo->GetTag(destinationOperand->getReg());
   GraphColor::LiveRange * liveRange = tile->GetLiveRange(destinationTag);

   llvm::MachineOperand *  spillOperand
      = this->GetSpillOperand(destinationOperand, liveRange, tile, isValidInMemory);
   llvm::MachineOperand *  temporaryOperand = this->GetSpillTemporary(destinationOperand, spillOperand, doInsert);

#ifdef ARCH_WITH_FOLDS
   // Only for architectures with instructions (other than LOAD/STORE) capable of writing results directly to memory
   if (this->DoReplaceOperandsEqual) {
      TargetRegisterAllocInfo * targetRegisterAllocator = allocator->TargetRegisterAllocator;
      llvm::MachineInstr *      pendingInstruction = destinationOperand->getParent();
      assert(pendingInstruction != nullptr);

      cost = targetRegisterAllocator->EstimateInstructionCost(pendingInstruction);
      Tiled::Cost sourceOperandCost = targetRegisterAllocator->EstimateMemoryCost(spillOperand);

      // Avoid the double count.
      cost.DecrementBy(&sourceOperandCost);

      // Clear cycle count for this def since it does not manifest it's value in a register.  The graph color
      // model uses definition cost (cycles) as equivalent to the cost of using a physical register.  In this
      // case no register is used so the execution cycles should be omitted.  When using code size as an
      // objective we have a different case.  Regardless of register cost or not we get the code size.  This
      // dichotomy causes the different treatment here between cycles and bytes.

      cost.SetExecutionCycles(0);

      if (doInsert) {
         // Replace appearance
         const llvm::TargetRegisterClass * destinationOperandRegisterCategory
            = allocator->GetRegisterCategory(destinationOperand->getReg());

         if (targetRegisterAllocator->GetBaseIntegerRegisterCategory() == destinationOperandRegisterCategory) {
            // keep pointer-ness when folding.  We don't do this generally because of a bad interaction with
            // memory address forms for xmm instructions
            //spillOperand->ResetField(destinationOperand->Field);
         }

         this->replaceAndUnlinkOperand(pendingInstruction, &destinationOperand, spillOperand);
         this->SetSpillKinds(pendingInstruction, SpillKinds::FoldedSpill);
      }

      // Insert folded spill
      this->InsertFoldedSpill(temporaryOperand, destinationOperand, spillOperand, pendingInstruction);

      // Cost computed on the reload side, no change for folded spill.
      return cost;
   }
#endif  //ARCH_WITH_FOLDS

   *registerOperand = temporaryOperand;

   // Use a scratch copy of the register operand and spill operand when generating the spill for the no insert
   // case.  callers expect to preserve the operands they pass in.

   if (doInsert) {
      definitionOperand = this->ReplaceSpillAppearance(*registerOperand, &destinationOperand, doInsert);
      scratchOperand = *registerOperand;
      scratchSpill = spillOperand;
   } else {
      definitionOperand = destinationOperand;
      scratchOperand = this->CopyOperand((*registerOperand));
      scratchSpill = this->CopyOperand(spillOperand);
   }

   llvm::MachineInstr *  insertInstruction = definitionOperand->getParent();
   assert(insertInstruction != nullptr);

   if (this->DoSpillSharing(destinationOperand)) {
      // Add tracking of definition being spilled

      // Check if any prior pending spill can be shared with this def.
      // 
      // Cases where this isn't true:
      //  - if the pending spill is larger
      //  - if the pending spill is assigned to a larger candidate register

      Tiled::VR::Info * vrInfo = this->VrInfo;
      unsigned          spillTag = vrInfo->GetTag(destinationOperand->getReg());

      if (!this->CanShareSpill(destinationOperand, scratchSpill, insertInstruction)) {
         this->ProcessPendingSpill(spillTag, insertInstruction, doInsert);
      }

      if (isValidInMemory) {
         // Insert the inherited spill for tracking
         this->InsertCommittedSpill(definitionOperand, spillTag, scratchSpill, insertInstruction);
      } else {
         // Note this spill location for later insertion if this is ultimately the last definition for some path
         // out of the window we're tracking.
         this->InsertPendingSpill(definitionOperand, spillTag, scratchSpill, insertInstruction);
      }
   } else if (!isValidInMemory) {
      // Model spill

      this->InsertSpillInternal(scratchOperand, liveRange->VrTag, scratchSpill, insertInstruction);

      cost = this->ComputePendingSpill(scratchOperand, liveRange->VrTag, spillOperand, insertInstruction, doInsert);
   }

   // Add the cost of this definition. This ensures that we're proportional with allocation cost (sum of
   // definition instruction costs)
   Tiled::Cost definitionCost = this->ComputeDefinition(definitionOperand);
   cost.IncrementBy(&definitionCost);

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Find largest appearance of of the given tag on the passed operand list
//
// Arguments:
// 
//    spillTag    - tag to find max operand for
//    operandList - operand list to search
//
// Returns:
//
//    max sized operand or nullptr.
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetSpillAppearance
(
   unsigned                                                spillTag,
   llvm::iterator_range<llvm::MachineInstr::mop_iterator>  operandList
)
{
   llvm::MachineOperand *  maxOperand = nullptr;
   Tiled::VR::Info *         vrInfo = this->VrInfo;

   llvm::MachineInstr::mop_iterator operand;

   // foreach_opnd
   for (operand = operandList.begin(); operand != operandList.end(); ++operand)
   {
      if (operand->isReg()) {
         unsigned operandTag = vrInfo->GetTag(operand->getReg());

         if (vrInfo->MustTotallyOverlap(spillTag, operandTag) && (maxOperand == nullptr)) {
            maxOperand = operand;
         }
      }
   }

   return maxOperand;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeFoldedReload
//
// Arguments:
//
//    operand           - Appearance being spilled.
//    spillOperand      - Spill backing memory.
//    insertInstruction - instruction to be inserted after.
//    doInsert          - Insert spill code if true.
//
// Notes:
//
//    Computes the cost of hoisting a previously folded reload - i.e. memory on an instruction - to a register
//    for reuse.  By product of this is a new load in the program and a temp holding the reload value.
//
// Returns:
//
//    Cost of hoisting the reload for sharing.
//
//------------------------------------------------------------------------------

#ifdef ARCH_WITH_FOLDS
Tiled::Cost
SpillOptimizer::ComputeFoldedReload
(
   llvm::MachineOperand *  operand,
   llvm::MachineOperand *  spillOperand,  //IR::MemoryOperand*
   llvm::MachineInstr *    insertInstruction,
   bool                    doInsert
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   Tiled::Cost             cost = allocator->ZeroCost;

   Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;
   IR::Instruction ^       endInstruction = insertInstruction;
   IR::Instruction ^       startInstruction = endInstruction->Previous;

   // compute reload code via target allocator, cost, and insert if requested.

   Registers::Register ^ reg = this->GetSpillRegister(operand, spillOperand);

   // use a scratch copy of the register operand and spill operand when generating the spill for the no
   // insert case.  callers expect to preserve the operands they pass in.

   IR::VariableOperand ^ scratchOperand;
   IR::MemoryOperand ^   scratchSpill;

   if (doInsert)
   {
      Assert(spillOperand->Instruction == endInstruction);

      // Insert spill temporary in the original instruction

      this->ReplaceSpillAppearance(operand, spillOperand, true);

      GraphColor::SpillKinds spillKinds = this->GetSpillKinds(endInstruction);

      // Remove folded reload

      spillKinds = (spillKinds & ~SpillKinds::FoldedReload);

      this->SetSpillKinds(endInstruction, spillKinds);
   }

   scratchOperand = operand;
   scratchSpill = spillOperand;

   GraphColor::LiveRange ^ liveRange = this->Tile->GetLiveRange(operand->AliasTag);
   Tiled::Boolean            isTileReload = false;
   Tiled::Boolean            isGlobalLiveRange = (liveRange == nullptr) ? false : liveRange->IsGlobal;

   targetRegisterAllocator->Reload(scratchOperand, reg, scratchSpill, endInstruction,
      doInsert, isTileReload, isGlobalLiveRange);

   // Reload generates one or more instructions, get the start and end of that sequence.

   IR::Instruction ^ firstReloadInstruction = startInstruction->Next;
   IR::Instruction ^ lastReloadInstruction = endInstruction->Previous;

   foreach_instr_in_range_editing(instruction, firstReloadInstruction, lastReloadInstruction)
   {
      // check added reload code is legal

      instruction->MarkAsLegalized();

      Tiled::Cost instructionCost = targetRegisterAllocator->EstimateInstructionCost(instruction);

      allocator->ScaleCyclesByFrequency(&instructionCost, instruction);

      cost.IncrementBy(&instructionCost);

      this->SetSpillKinds(instruction, SpillKinds::Reload);

      if (!doInsert)
      {
         instruction->Unlink();
      }

      instruction->MarkAsLegalized();
   }
   next_instr_in_range_editing;

   return cost;
}
#endif // ARCH_WITH_FOLDS

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeFoldedSpill
//
// Arguments:
//
//    operand           - Appearance being spilled.
//    spillOperand      - Spill backing memory.
//    insertInstruction - instruction to be inserted after.
//    doInsert          - Insert spill code if true.
//
// Notes:
//
//    Computes the cost of hoisting a previously folded spill - i.e. memory on an instruction - to a register
//    for reuse.  By product of this is a new load in the program and a temp holding the reload value.
//
// Returns:
//
//    Cost of hoisting the reload for sharing.
//
//------------------------------------------------------------------------------

#ifdef ARCH_WITH_FOLDS
Tiled::Cost
SpillOptimizer::ComputeFoldedSpill
(
   unsigned                spillTag,
   llvm::MachineOperand *  operand,
   llvm::MachineOperand *  spillOperand,  //IR::MemoryOperand*
   llvm::MachineInstr *    insertInstruction,
   bool                    canMakePending,
   bool                    doInsert
)
{

   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::Cost              cost = allocator->ZeroCost;

   Alias::Info ^                               aliasInfo = this->AliasInfo;
   Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;
   IR::Operand ^ definitionOperand;

   if (doInsert)
   {
      Assert(insertInstruction == spillOperand->Instruction);

      // Insert spill temporary in the original instruction

      definitionOperand = this->ReplaceSpillAppearance(operand, spillOperand, true);

      GraphColor::SpillKinds spillKinds = this->GetSpillKinds(insertInstruction);

      spillKinds = (spillKinds & ~SpillKinds::FoldedSpill);

      this->SetSpillKinds(insertInstruction, spillKinds);
   }
   else
   {
      definitionOperand = aliasInfo->DestinationMustTotallyOverlap(insertInstruction, spillTag);
   }

   Assert(definitionOperand != nullptr);

   if (canMakePending)
   {
      this->InsertPendingSpill(definitionOperand, spillTag, spillOperand, insertInstruction);

      cost = targetRegisterAllocator->EstimateMemoryCost(spillOperand);
      allocator->ScaleCyclesByFrequency(&cost, insertInstruction);

      cost.Negate();
   }
   else
   {
      Graphs::BasicBlock ^ basicBlock = insertInstruction->BasicBlock;

      GraphColor::SpillRecord ^ spillRecord = this->GetSpillRecord(spillTag, basicBlock);

      Assert(spillRecord->IsFoldedSpill);

      cost = this->ComputePendingSpill(definitionOperand->AsVariableOperand, spillTag, spillOperand,
         insertInstruction, doInsert);

      // set up new definition as the last "reload" value.

      spillRecord->ReloadOperand = definitionOperand;
   }

   return cost;
}
#endif // ARCH_WITH_FOLDS


llvm::MachineOperand *
getMemorySpillOperand
(
   llvm::MachineInstr * instruction
)
{
   llvm::MachineInstr::mop_iterator o;
   for (o = instruction->operands_begin(); o != instruction->operands_end(); ++o)
      if (o->isFI()) {
         return &(*o);
      }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputePendingSpill
//
// Arguments:
//
//    operand           - Appearance being spilled (VariableOperand)
//    spillAliasTag     - Alias tag for the live range being spilled (maybe scratch in some cases)
//    spillOperand      - Spill backing memory (MemoryOperand)
//    insertInstruction - Instruction to be inserted after.
//    doInsert          - Insert spill code if true.
//
// Returns:
//
//    Cost of spill code.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputePendingSpill
(
   llvm::MachineOperand * operand,
   unsigned               spillAliasTag,
   llvm::MachineOperand * spillOperand,
   llvm::MachineInstr *   insertInstruction,
   bool                   doInsert
)
{
   GraphColor::Allocator *   allocator = this->Allocator;
   Tiled::Cost               cost = allocator->ZeroCost;
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   TargetRegisterAllocInfo * targetRegisterAllocator = allocator->TargetRegisterAllocator;
   unsigned                  reg = this->GetSpillRegister(operand, spillOperand);
   llvm::MachineInstr *      startInstruction = insertInstruction;
   llvm::MachineInstr *      endInstruction = startInstruction->getNextNode();
   //bool                    isDanglingInstruction = false;
   llvm::MachineOperand *    reloadMemoryOperand = getMemorySpillOperand(insertInstruction);

   if (this->IsSpillKind(insertInstruction, GraphColor::SpillKinds::Reload)
      && (!doInsert || vrInfo->MustTotallyOverlap(reloadMemoryOperand, spillOperand))) {

      // Clear reload so this will not be further shared
      this->ClearReload(spillAliasTag, insertInstruction);

      // Return zero cost since we're avoiding a spill in this case (value is already in memory).
      return cost;
   }

   // If we are doing insertion, then keep track if we are spilling a dangling definition
   // of and instruction with an EH edge.

   /*EH:
   if (doInsert) {
        isDanglingInstruction = startInstruction->IsDanglingInstruction;
   }*/

   GraphColor::LiveRange * liveRange = this->Tile->GetLiveRange(spillAliasTag);

   bool isTileSpill = false;
   llvm::MachineOperand * sourceOperand = nullptr;

   targetRegisterAllocator->Spill(sourceOperand, reg, spillOperand,
      startInstruction, doInsert, isTileSpill, liveRange->IsGlobal());
   if (doInsert) this->inFunctionSpilledLRs++;

   // Spill generates one or more instructions, get the start and end of that sequence.

   llvm::MachineBasicBlock::instr_iterator(iter);
   llvm::MachineBasicBlock::instr_iterator(nextIter);
   llvm::MachineBasicBlock::instr_iterator firstSpillInstrIter =
      llvm::MachineBasicBlock::instr_iterator(startInstruction->getNextNode());
   llvm::MachineBasicBlock::instr_iterator(endIter);
   if (endInstruction != nullptr) {
      endIter = llvm::MachineBasicBlock::instr_iterator(endInstruction);
   } else {
      endIter = (startInstruction->getParent())->instr_end();
   }

   // foreach_instr_in_range_editing
   for (iter = firstSpillInstrIter; iter != endIter; iter = nextIter)
   {
      llvm::MachineInstr * instruction = &(*iter);
      nextIter = ++iter;

      Tiled::Cost instructionCost = targetRegisterAllocator->EstimateInstructionCost(instruction);
      allocator->ScaleCyclesByFrequency(&instructionCost, instruction);
      cost.IncrementBy(&instructionCost);

      this->SetSpillKinds(instruction, SpillKinds::Spill);

      if (!doInsert) {
         SpillOptimizer::ResetInstrSpillMarkings(instruction);
         instruction->removeFromParent();
      } else {
         this->Allocator->Indexes->insertMachineInstrInMaps(*instruction);
      }
   }

   // If we are spilling the dangling definition of an instruction with and EH edge
   // then move the spill code into the next block.

   // <place for EH-flow-related code>

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute cost of folding a definition into the use
//
// Arguments:
//
//    definitionOperand - definition of available instruction
//    useOperand        - use to fold into
//    tile              - tile context
//    doInsert          - true if code to be inserted into the IR stream
//
// Return
//
//    Cost of fold or infinity if no fold possible.
//
//------------------------------------------------------------------------------

#ifdef ARCH_WITH_FOLDS
Tiled::Cost
SpillOptimizer::ComputeFold
(
   llvm::MachineOperand * operand,
   GraphColor::Tile *     tile,
   bool                   doInsert
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::Cost              foldCost = allocator->ZeroCost;

   Tiled::Cost originalCost;
   Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;
   IR::Instruction ^                           instruction = operand->Instruction;

   if (operand->IsDefinition)
   {
      // look up specific occurrence

      IR::Operand ^ availableOperand = this->GetAvailable(operand);

      if (availableOperand != nullptr)
      {
         // definition is available

         foldCost = allocator->ZeroCost;
      }
      else
      {
         AssertM((doInsert == false), "trying to delete a definition not in the table!");

         foldCost = allocator->InfiniteCost;
      }
   }
   else
   {
      IR::Operand ^ availableOperand = this->GetAvailable(operand);

      // fold cost starts at infinity, not foldable, and turns into a real number once we find we can fold.

      foldCost.SetInfinity();

      // If the live range in question was marked as "Fold" during costing then try and do the fold.  If
      // ongoing spilling caused the live range appearance in question to no longer be foldable, a rare case,
      // the fix up code that does the recalculate needs to fire.  So we don't do the early out via
      // IsLegalToFoldOperand.  This is a byproduct of the optimistic view we take during costing.
      // (i.e. ignoring possible spill side effects on live ranges that have appearances in the same
      // instruction)

      if ((availableOperand != nullptr) && (doInsert || this->IsLegalToFoldOperand(availableOperand, operand)))
      {
         IR::Instruction ^ scratchInstruction;
         IR::Operand ^     scratchOperand;

         if (doInsert)
         {
            scratchInstruction = instruction;
            scratchOperand = operand;
         }
         else
         {
            scratchInstruction = instruction->Copy();

            scratchOperand = this->GetSpillAppearance(operand->AliasTag,
               (operand->IsUse ?
                  scratchInstruction->SourceOperandList : scratchInstruction->DestinationOperandList));

            Assert(IR::Operand::Compare(scratchOperand, operand));
         }

         if (tile->Pass == GraphColor::Pass::Allocate)
         {
            // Mark underlying live range as having a partition.

            GraphColor::LiveRange ^ liveRange = tile->GetLiveRange(operand);

            liveRange->HasSpillPartition = true;
         }

         if (!doInsert || this->IsLegalToFoldOperand(availableOperand, operand))
         {
            foldCost = targetRegisterAllocator->Fold(availableOperand, scratchOperand);
         }
         else
         {
            Assert(doInsert);
            Assert(foldCost.IsInfinity);
         }

         if (doInsert && foldCost.IsInfinity)
         {
            // We're inserting and we failed to fold via the target. (other spilling decisions might have
            // blocked this) Convert this instance to a recalculate.

            IR::VariableOperand ^ registerOperand = nullptr;

            foldCost = this->ComputeRecalculate(&registerOperand, operand, tile, doInsert);
         }

         allocator->ScaleCyclesByFrequency(&foldCost, instruction);

         if (doInsert)
         {
            this->SetSpillKinds(instruction, SpillKinds::Fold);
         }
         else
         {
            // Remove scratch for non-insert case.

            SpillOptimizer::DeleteInstruction(scratchInstruction);
         }
      }
      else
      {
         Assert(doInsert == false);
         Assert(foldCost.IsInfinity);
      }
   }

   return foldCost;
}
#endif // ARCH_WITH_FOLDS

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute definition cost
//
// Argument:
//
//    definitionOperand - definition to cost
//
// Notes:
//
//    Sum of definition costs is the allocation cost of a given live range.  Used in conjunction with global
//    register cost to determine if we should allocate a register at all.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeDefinition
(
   llvm::MachineOperand * definitionOperand
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   llvm::MachineInstr *       definitionInstruction = definitionOperand->getParent();
   assert(definitionInstruction != nullptr);

   TargetRegisterAllocInfo *  targetRegisterAllocator = allocator->TargetRegisterAllocator;

   Tiled::Cost definitionCost = targetRegisterAllocator->EstimateInstructionCost(definitionInstruction);

   allocator->ScaleCyclesByFrequency(&definitionCost, definitionInstruction);

   return definitionCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeWeight
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeWeight
(
   llvm::MachineOperand * operand
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   llvm::MachineInstr *    instruction = operand->getParent();
   Tiled::Cost             weightCost;

   weightCost.Initialize(1, 1);
   allocator->ScaleCyclesByFrequency(&weightCost, instruction);

   return weightCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeTransfer - Insert appropriate copy or transfer to or from memory for the transition of the boundary
//    tag from one tile to another.
//
// Arguments:
//
//    otherTile  - The tile to compute transfer from/to
//    boundaryTag - Tag of the allocation being transferred.
//    block      - Block where to locate the transfer code.
//    doInsert   - true to insert transfer code, false to cost with out side effect.
//
// Returns:
//
//    Cost of the inserted transfer. 
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeTransfer
(
   GraphColor::Tile *           otherTile,
   unsigned                     boundaryTag,
   llvm::MachineBasicBlock *    block,
   bool                         doInsert
)
{
   GraphColor::Tile *       tile = this->Tile;
   GraphColor::Allocator *  allocator = this->Allocator;
   Graphs::FlowGraph *      flowGraph = allocator->FunctionUnit;
   llvm::MachineFunction *  MF = allocator->MF;
   Tiled::Cost              cost = allocator->ZeroCost;
   unsigned                 tileRegister = tile->GetSummaryRegister(boundaryTag);
   unsigned                 otherRegister = otherTile->GetSummaryRegister(boundaryTag);
   llvm::MachineOperand *   registerOperand;

   bool  otherIsParent = (tile->ParentTile == otherTile);
   bool  isEntry = otherIsParent ?
      (std::find(tile->EntryBlockList->begin(), tile->EntryBlockList->end(), block) != tile->EntryBlockList->end()) :
      (std::find(otherTile->EntryBlockList->begin(), otherTile->EntryBlockList->end(), block) != otherTile->EntryBlockList->end());
   //   bool            isValidInMemory = false;
   llvm::MachineBasicBlock * uniqueSuccessor = nullptr;

   TargetRegisterAllocInfo *  targetRegisterAllocator = allocator->TargetRegisterAllocator;

   if (tileRegister == VR::Constants::InvalidReg && otherRegister == VR::Constants::InvalidReg) {
      // Early out.  Spilled both sides.  Call into ComputeTransfer was due to processing of a second pass
      // spilled summary.
      return cost;
   }

   // If a global didn't get a register in the parent, check this entry to see if a parent spill was extended
   // to the EnterTile.

   if (otherRegister == VR::Constants::InvalidReg) {
      llvm::MachineInstr * enterInstruction = flowGraph->FindNextInstructionInBlock(llvm::TargetOpcode::ENTERTILE, &(*block->instr_begin()));

      if (enterInstruction != nullptr) {
         GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(boundaryTag);

         llvm::MachineInstr::mop_iterator sourceOperand;
         llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(enterInstruction->explicit_operands());
         // foreach_register_source_opnd
         for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
         {
            assert(globalLiveRange->VrTag != VR::Constants::InvalidTag);

            unsigned  spillAliasTag = this->GetSpillAliasTag(sourceOperand);
            unsigned  reg = sourceOperand->getReg();

            // Opt out of the pseudo register case.  On x86 we can see FP stack pseudos and these should never
            // be extended especially across flow.

            if (spillAliasTag == globalLiveRange->VrTag && !this->VrInfo->IsVirtualRegister(reg)) {
               // Spill available, get the register and use that.

               otherRegister = sourceOperand->getReg();
               break;
            }
         }
      }
   }

   assert(allocator->GetGlobalLiveRange(boundaryTag) != nullptr);

   // Check neither register is the tile spill register

   if (tileRegister != VR::Constants::InvalidReg && otherRegister != VR::Constants::InvalidReg) {
      // Resize registers so that they match.  This avoids a "convert"
      // case and allows for a simpiler identity check ("no copy case")

      // <place for code supporting architectures with sub-registers>

      if (otherRegister != tileRegister) {
         // Transfer by copy

         llvm::MachineOperand * sourceOperand = nullptr;
         llvm::MachineOperand * destinationOperand = nullptr;

         // Use source operand for both calls to get subregister,
         // source and destination are copies.

         // <place for code supporting architectures with sub-registers>

         if ((otherIsParent && isEntry) || (!otherIsParent && !isEntry)) {
            destinationOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(tileRegister, true)); // this allocation
            sourceOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(otherRegister, false)); // other allocation
         } else {
            assert((otherIsParent && !isEntry) || (!otherIsParent && isEntry));
            sourceOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(tileRegister, false)); // this allocation
            destinationOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(otherRegister, true)); // other allocation
         }

         unsigned  sourceRegister = sourceOperand->getReg();
         unsigned  destinationRegister = destinationOperand->getReg();

         llvm::MachineInstr * insertInstruction = this->GetCopyTransferInsertionPoint(destinationOperand, sourceOperand, block);

         // InsertCopy

         if (insertInstruction != nullptr) {
            // insert moveInstruction before insertInstruction
            llvm::MachineBasicBlock * mbb = insertInstruction->getParent();
            llvm::MachineBasicBlock::instr_iterator I(insertInstruction);
            BuildMI(*mbb, I, I->getDebugLoc(), allocator->MCID_MOVE, destinationOperand->getReg())
               .addReg(sourceOperand->getReg());
            llvm::MachineInstr * moveInstruction = I->getPrevNode();
            delete sourceOperand; delete destinationOperand;
            assert(moveInstruction && moveInstruction->isCopy());

            // Make sure we've only inserted a single instruction (costs are wrong otherwise).

            this->SetSpillKinds(moveInstruction, GraphColor::SpillKinds::Transfer);

            cost = targetRegisterAllocator->EstimateInstructionCost(moveInstruction);
            allocator->ScaleCyclesByFrequency(&cost, moveInstruction);

            if (doInsert) {
               allocator->Indexes->insertMachineInstrInMaps(*moveInstruction);

               GraphColor::LiveRange * summaryLiveRange = tile->GetSummaryLiveRange(boundaryTag);
               if (summaryLiveRange && !summaryLiveRange->HasReference()) {
                  GraphColor::Tile::BlockToCopysMap::iterator bc = tile->TransparentBoundaryCopys.find(block->getNumber());
                  if (bc == tile->TransparentBoundaryCopys.end()) {
                     GraphColor::Tile::BlockToCopysMap::value_type entry(block->getNumber(), GraphColor::Tile::CopyVector());
                     std::pair<GraphColor::Tile::BlockToCopysMap::iterator, bool> res = tile->TransparentBoundaryCopys.insert(entry);
                     bc = res.first;
                  }
                  bc->second.push_back(moveInstruction);
               }

            } else {
               SpillOptimizer::RemoveInstruction(moveInstruction);
            }

         } else {
            unsigned scratchRegister = VR::Constants::InvalidReg;

            scratchRegister = this->GetCopyTransferScratchRegister(tileRegister, block, otherTile);

            if (scratchRegister != VR::Constants::InvalidReg) {
               // Insert transfer through scratch with two moves.

               assert(!block->empty());
               llvm::MachineInstr * afterInstruction = tile->FindEnterTileInstruction(block);
               llvm::MachineInstr * beforeInstruction = tile->FindExitTileInstruction(block);

               if (afterInstruction == nullptr) {
                  afterInstruction = &(block->front());
               }

               if (beforeInstruction == nullptr) {
                  beforeInstruction = &(block->back());
               }

               llvm::MachineBasicBlock::instr_iterator I(afterInstruction->getNextNode());
               BuildMI(*block, I, I->getDebugLoc(), allocator->MCID_MOVE, scratchRegister)
                  .addReg(sourceOperand->getReg());
               llvm::MachineInstr * moveOneInstruction = afterInstruction->getNextNode();

               llvm::MachineBasicBlock::instr_iterator II(beforeInstruction);
               BuildMI(*block, II, II->getDebugLoc(), allocator->MCID_MOVE, destinationOperand->getReg())
                  .addReg(scratchRegister);
               llvm::MachineInstr * moveTwoInstruction = beforeInstruction->getPrevNode();

               delete sourceOperand; delete destinationOperand;

               this->SetSpillKinds(moveOneInstruction, GraphColor::SpillKinds::Transfer);
               this->SetSpillKinds(moveTwoInstruction, GraphColor::SpillKinds::Transfer);

               Tiled::Cost copyCost = targetRegisterAllocator->EstimateInstructionCost(moveOneInstruction);
               cost.IncrementBy(&copyCost);

               copyCost = targetRegisterAllocator->EstimateInstructionCost(moveTwoInstruction);
               cost.IncrementBy(&copyCost);

               allocator->ScaleCyclesByFrequency(&cost, moveOneInstruction);

               if (doInsert) {
                  allocator->Indexes->insertMachineInstrInMaps(*moveOneInstruction);
                  allocator->Indexes->insertMachineInstrInMaps(*moveTwoInstruction);
               } else {
                  SpillOptimizer::RemoveInstruction(moveOneInstruction);
                  SpillOptimizer::RemoveInstruction(moveTwoInstruction);
               }

            } else {
               // Insert spill through mem.

               assert(doInsert);

               if (otherIsParent == isEntry) {
                  destinationRegister = tileRegister; // this allocation
                  sourceRegister = otherRegister; // other allocation
               } else {
                  assert((otherIsParent && !isEntry) || (!otherIsParent && isEntry));

                  sourceRegister = tileRegister; // this allocation
                  destinationRegister = otherRegister; // other allocation
               }

               llvm::MachineInstr * firstInstruction = tile->FindEnterTileInstruction(block);
               llvm::MachineInstr * lastInstruction = tile->FindExitTileInstruction(block);

               if (firstInstruction == nullptr) {
                  firstInstruction = &(block->front());
               }

               if (lastInstruction == nullptr) {
                  lastInstruction = &(block->back());
               }

               Tiled::Cost exitSaveCost = this->ComputeExitSave(otherTile, boundaryTag, sourceRegister, firstInstruction, doInsert);
               cost.IncrementBy(&exitSaveCost);

               Tiled::Cost enterLoadCost = this->ComputeEnterLoad(tile, boundaryTag, destinationRegister, lastInstruction, doInsert);
               cost.IncrementBy(&enterLoadCost);
            }
         }

         if (doInsert) {
            block->addLiveIn(sourceRegister);
            if ((uniqueSuccessor = flowGraph->uniqueSuccessorBlock(block))) {
               uniqueSuccessor->addLiveIn(destinationRegister);
            }
         }
      }

   } else {

      GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(boundaryTag);
      unsigned                globalLiveRangeTag = globalLiveRange->VrTag;

      if (tileRegister == VR::Constants::InvalidReg) {
         // This arm only valid on pass 2.

         assert(allocator->Pass == GraphColor::Pass::Assign);
         assert(otherIsParent);
         assert(otherRegister != VR::Constants::InvalidReg);

         // Spilled in the local tile but parent got a register.

         if (isEntry) {
            llvm::MachineInstr * firstInstruction = tile->FindEnterTileInstruction(block);
            if (firstInstruction == nullptr) {
               firstInstruction = &(block->front());
            }

            // If spilled in the local tile, and an entry block insert ExitSave.
            if (tile->IsMemory(globalLiveRangeTag)) {
               cost = ComputeExitSave(tile, boundaryTag, otherRegister, firstInstruction, doInsert);
               if (doInsert) {
                  block->addLiveIn(otherRegister);
               }
            }

         } else {
            llvm::MachineInstr * lastInstruction = tile->FindExitTileInstruction(block);
            if (lastInstruction == nullptr) {
               lastInstruction = &(block->back());
            }

            cost = ComputeEnterLoad(tile, boundaryTag, otherRegister, lastInstruction, doInsert);
            if (doInsert && (uniqueSuccessor = flowGraph->uniqueSuccessorBlock(block))) {
               uniqueSuccessor->addLiveIn(otherRegister);
            }
         }

      } else {
         assert(otherRegister == VR::Constants::InvalidReg);
         assert(tileRegister != VR::Constants::InvalidReg);

         if (otherIsParent == isEntry) {
            llvm::MachineInstr * lastInstruction = tile->FindExitTileInstruction(block);
            if (lastInstruction == nullptr) {
               lastInstruction = &(block->back());
            }

            if (!otherIsParent || tile->GlobalTransitiveUsedAliasTagSet->test(globalLiveRangeTag)
                || tile->GlobalTransitiveDefinedAliasTagSet->test(globalLiveRangeTag)) {
               cost = ComputeEnterLoad(otherTile, boundaryTag, tileRegister, lastInstruction, doInsert);
               if (doInsert && (uniqueSuccessor = flowGraph->uniqueSuccessorBlock(block))) {
                  uniqueSuccessor->addLiveIn(tileRegister);
               }
            }
         } else {
            assert((otherIsParent && !isEntry) || (!otherIsParent && isEntry));

            llvm::MachineInstr * firstInstruction = tile->FindEnterTileInstruction(block);
            if (firstInstruction == nullptr) {
              firstInstruction = &(block->front());
            }

            // Add opt out case if the other tile isn't memory

            if (otherTile->IsMemory(globalLiveRangeTag)
                && (!otherIsParent || tile->GlobalTransitiveDefinedAliasTagSet->test(globalLiveRangeTag))) {
               cost = ComputeExitSave(otherTile, boundaryTag, tileRegister, firstInstruction, doInsert);
               if (doInsert) {
                  block->addLiveIn(tileRegister);
               }
            }
         }
      }
   }

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute a enter load of the live range from a source tile.
//
// Argument:
//
//    tile              - Tile where the live range is modified.
//    boundaryTag       - Alias tag of live range at boundary.
//    reg               - Target register of the load.
//    insertInstruction - IR point to insert the load.
//    doInsert          - true to insert load, false to cost with no side effect.
//
// Return:
//
//    Cost of enter load.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeEnterLoad
(
   GraphColor::Tile *    spillTile,
   unsigned              boundaryTag,
   unsigned              reg,
   llvm::MachineInstr *  insertInstruction,
   bool                  doInsert
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   GraphColor::LiveRange *  liveRange = allocator->GetGlobalLiveRange(boundaryTag);
   assert(liveRange != nullptr);
   Tiled::VR::Info *        aliasInfo = this->VrInfo;
   unsigned                 liveRangeAliasTag = liveRange->VrTag;
   llvm::MachineInstr *     transferInstruction;
   Tiled::Cost              cost;

   unsigned liveRangeRegister = liveRange->Register;
   bool     isValidInMemory;

   TargetRegisterAllocInfo *  targetRegisterAllocator = allocator->TargetRegisterAllocator;

   GraphColor::AvailableExpressions * globalAvailableExpressions = allocator->GlobalAvailableExpressions;
   llvm::MachineOperand * availableOperand = globalAvailableExpressions->GetAvailable(liveRangeAliasTag);

   if (availableOperand != nullptr) {

      llvm::MachineInstr *  availableInstruction = availableOperand->getParent();
      cost = targetRegisterAllocator->EstimateInstructionCost(availableInstruction);

      allocator->ScaleCyclesByFrequency(&cost, insertInstruction);

      // <place for code supporting architectures with sub-registers>

      // Copy tracked recalculate expression to a new instruction.

      llvm::MachineFunction *  MF = allocator->MF;
      transferInstruction = MF->CloneMachineInstr(availableInstruction);

      assert(transferInstruction->defs().begin() != transferInstruction->defs().end());
      llvm::MachineOperand * destinationOperand = transferInstruction->defs().begin();
      assert(destinationOperand != nullptr);

      // Hammer alias tag to ensure if we're inserting before pass 2 that the tags are consistent.
      // <place for code supporting architectures with sub-registers>

      // Resize the allocated register to the recalculate definition (might be smaller if size opts has been
      // messing with the field)
      // <place for code supporting architectures with sub-registers>
      unsigned recalculateRegister = reg;
      destinationOperand->setReg(recalculateRegister);

      this->SetSpillKinds(transferInstruction, GraphColor::SpillKinds::Recalculate);

      if (doInsert) {
         llvm::MachineBasicBlock * mbb = insertInstruction->getParent();
         mbb->insert(insertInstruction, transferInstruction);
         allocator->Indexes->insertMachineInstrInMaps(*transferInstruction);
      } else {
         SpillOptimizer::RemoveInstruction(transferInstruction);
      }

   } else {

      // <place for code supporting architectures with sub-registers>

      assert(reg != VR::Constants::InvalidReg);
      if (reg == VR::Constants::InitialPseudoReg) {
         reg = aliasInfo->GetRegister(liveRangeAliasTag);
      }
      llvm::MachineOperand *  registerOperand = this->GetRegisterOperand(liveRangeAliasTag, reg);
      llvm::MachineOperand *  spillOperand =
         this->GetSpillOperand(registerOperand, liveRange, spillTile, isValidInMemory);
      assert(spillOperand->isFI());
      bool                    isTileReload = true;
      bool                    isGlobalLiveRange = true;

      targetRegisterAllocator->Reload(registerOperand, reg, spillOperand, insertInstruction, doInsert, isTileReload, isGlobalLiveRange);
      if (doInsert) this->inFunctionSpilledLRs++;

      llvm::MachineInstr * transferInstruction = insertInstruction->getPrevNode();
      this->SetSpillKinds(transferInstruction, GraphColor::SpillKinds::Reload);

      cost = targetRegisterAllocator->EstimateInstructionCost(transferInstruction);
      allocator->ScaleCyclesByFrequency(&cost, transferInstruction);

      if (doInsert) {
         allocator->Indexes->insertMachineInstrInMaps(*transferInstruction);
      } else {
         SpillOptimizer::RemoveInstruction(transferInstruction);
      }
   }

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute a load of the live range from a source tile.
//
// Argument:
//
//    tile              - Tile where the live range is modified.
//    liveRange         - Live range to load.
//    reg               - Target register of the load.
//    insertInstruction - IR point to insert the load.
//    doInsert          - true to insert load, false to cost with no side effect.
//
// Return:
//
//    Cost of load.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeLoad
(
   GraphColor::Tile *      tile,
   GraphColor::LiveRange * liveRange,
   unsigned                reg,
   llvm::MachineInstr *    insertInstruction,
   bool                    doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^ allocator = this->Allocator;
    Alias::Tag              liveRangeAliasTag = liveRange->AliasTag;
    IR::Instruction ^       transferInstruction;
    Tiled::Cost               cost;

    Assert(liveRange != nullptr);

    Assert(tile->Pass == GraphColor::Pass::Allocate);

    Registers::Register ^ liveRangeRegister = liveRange->Register;

    Tiled::Boolean isValidInMemory;

    Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;

#if defined(TILED_DEBUG_DUMPS)
    Tiled::FunctionUnit ^         functionUnit = allocator->FunctionUnit;
    Targets::Runtimes::Lister ^ lister = functionUnit->Lister;
#endif

    IR::Operand ^ availableOperand = nullptr;

    availableOperand = this->GetAvailable(liveRangeAliasTag);

    if (availableOperand != nullptr)
    {
        IR::Instruction ^       availableInstruction = availableOperand->Instruction;

        cost = targetRegisterAllocator->EstimateInstructionCost(availableInstruction);

        allocator->ScaleCyclesByFrequency(&cost, insertInstruction);

        reg = reg->GetSubRegister(liveRangeRegister->BitSize, 0);

        // Copy tracked recalculate expression to a new instruction.

        transferInstruction = availableInstruction->Copy();

        IR::Operand ^ destinationOperand = transferInstruction->DestinationOperand;

        Assert(destinationOperand != nullptr);

        // Hammer alias tag to ensure if we're inserting before pass 2 that the tags are consistent.

        destinationOperand->AliasTag = liveRangeAliasTag;

        // Resize the allocated register to the recalculate definition (might be smaller if size opts has been
        // messing with the field)

        Tiled::BitSize          recalculateBitSize = destinationOperand->Register->BitSize;
        Registers::Register ^ recalculateRegister = reg->GetSubRegister(recalculateBitSize, 0);

        destinationOperand->Register = recalculateRegister;

        transferInstruction->DebugTag = insertInstruction->DebugTag;

        this->SetSpillKinds(transferInstruction,
            (GraphColor::SpillKinds::Recalculate | GraphColor::SpillKinds::CallerSave));

        if (doInsert)
        {
            insertInstruction->InsertBefore(transferInstruction);
        }
        else
        {
            SpillOptimizer::RemoveInstruction(transferInstruction);
        }
    }
    else
    {
        reg = reg->GetSubRegister(liveRangeRegister->BitSize, 0);

        Assert(reg != nullptr);

        IR::VariableOperand ^ registerOperand = this->GetRegisterOperand(liveRangeAliasTag, reg);
        IR::MemoryOperand ^   spillOperand = this->GetSpillOperand(registerOperand, liveRange, tile,
            &isValidInMemory);
        Tiled::Boolean          isTileReload = false;

        targetRegisterAllocator->Reload(registerOperand, reg, spillOperand, insertInstruction,
            doInsert, isTileReload, liveRange->IsGlobal);

        IR::Instruction ^ transferInstruction = insertInstruction->Previous;

        this->SetSpillKinds(transferInstruction,
            (GraphColor::SpillKinds::Reload | GraphColor::SpillKinds::CallerSave));

        cost = targetRegisterAllocator->EstimateInstructionCost(transferInstruction);

        allocator->ScaleCyclesByFrequency(&cost, transferInstruction);

        if (doInsert)
        {

#if defined(TILED_DEBUG_DUMPS)
            this->CountReloads++;
#endif // TILED_DEBUG_DUMPS

        }
        else
        {
            SpillOptimizer::RemoveInstruction(transferInstruction);
        }
    }

    return cost;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute a exit save of the live range.
//
// Arguments:
//
//    spillTile         - Tile where the live range is spilled.
//    boundaryTag       - Alias tag of live range at boundary.
//    reg               - Source register of the store.
//    insertInstruction - Location to insert the store.
//    doInsert          - true to insert store, false to cost with no side effect.
//
// Return:
//
//    Cost of exit save.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeExitSave
(
   GraphColor::Tile *    spillTile,
   unsigned              boundaryTag,
   unsigned              reg,
   llvm::MachineInstr *  insertInstruction,
   bool                  doInsert
)
{
   Tiled::VR::Info *        vrInfo = this->VrInfo;
   GraphColor::Allocator *  allocator = this->Allocator;
    GraphColor::LiveRange * liveRange = allocator->GetGlobalLiveRange(boundaryTag);
    assert(liveRange != nullptr);
    unsigned                liveRangeAliasTag = liveRange->VrTag;

    unsigned  liveRangeRegister = liveRange->Register;
    bool      isValidInMemory;

    TargetRegisterAllocInfo *  targetRegisterAllocator = allocator->TargetRegisterAllocator;

    /* code relevant only to architectures with sub-registers:
    if (reg->BitSize > liveRangeRegister->BitSize) {
        reg = reg->GetSubRegister(liveRangeRegister->BitSize, 0);
    }*/

    assert(reg != VR::Constants::InvalidReg);
    if (reg == VR::Constants::InitialPseudoReg) {
       reg = vrInfo->GetRegister(liveRange->VrTag);
    }

    llvm::MachineOperand *  registerOperand = this->GetRegisterOperand(liveRangeAliasTag, reg);
    llvm::MachineOperand * /*IR::MemoryOperand* */  spillOperand =
       this->GetSpillOperand(registerOperand, liveRange, spillTile, isValidInMemory);
    assert(spillOperand->isFI());

    bool  isTileSpill = true;
    bool  isGlobalLiveRange = true;

    targetRegisterAllocator->Spill(registerOperand, reg, spillOperand, insertInstruction, doInsert, isTileSpill, isGlobalLiveRange);
    if (doInsert) this->inFunctionSpilledLRs++;

    llvm::MachineInstr *  transferInstruction = insertInstruction->getNextNode();

    this->SetSpillKinds(transferInstruction, GraphColor::SpillKinds::Spill);

    Tiled::Cost cost = targetRegisterAllocator->EstimateInstructionCost(transferInstruction);
    allocator->ScaleCyclesByFrequency(&cost, transferInstruction);

    if (doInsert) {
       allocator->Indexes->insertMachineInstrInMaps(*transferInstruction);
#if defined(TILED_DEBUG_DUMPS)
       this->CountStores++;
#endif
    } else {
       SpillOptimizer::RemoveInstruction(transferInstruction);
    }

    return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute a save of the live range.
//
// Arguments:
//
//    spillTile         - Tile where the live range is spilled.
//    liveRange         - Live range to store.
//    reg               - Source register of the store.
//    insertInstruction - Location to insert the store.
//    doInsert          - true to insert store, false to cost with no side effect.
//
// Return:
//
//    Cost of save.
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::ComputeSave
(
   GraphColor::Tile *      tile,
   GraphColor::LiveRange * liveRange,
   unsigned                reg,
   llvm::MachineInstr *    insertInstruction,
   bool                    doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^ allocator = this->Allocator;
    Alias::Tag              liveRangeAliasTag = liveRange->AliasTag;

    Assert(liveRange != nullptr);
    Assert(tile->Pass == GraphColor::Pass::Allocate);

    Registers::Register ^   liveRangeRegister = liveRange->Register;

    Tiled::Boolean isValidInMemory;

    Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;

#if defined(TILED_DEBUG_DUMPS)
    Tiled::FunctionUnit ^         functionUnit = allocator->FunctionUnit;
    Targets::Runtimes::Lister ^ lister = functionUnit->Lister;
#endif

    reg = reg->GetSubRegister(liveRangeRegister->BitSize, 0);

    Assert(reg != nullptr);

    IR::VariableOperand ^ registerOperand = this->GetRegisterOperand(liveRangeAliasTag, reg);
    IR::MemoryOperand ^   spillOperand = this->GetSpillOperand(registerOperand, liveRange, tile,
        &isValidInMemory);

    Tiled::Boolean isTileSpill = false;

    targetRegisterAllocator->Spill(registerOperand, reg, spillOperand, insertInstruction,
        doInsert, isTileSpill, liveRange->IsGlobal);

    IR::Instruction ^ transferInstruction = insertInstruction->Next;

    this->SetSpillKinds(transferInstruction,
        (GraphColor::SpillKinds::Spill | GraphColor::SpillKinds::CallerSave));

    Tiled::Cost cost = targetRegisterAllocator->EstimateInstructionCost(transferInstruction);

    allocator->ScaleCyclesByFrequency(&cost, transferInstruction);

    if (doInsert)
    {

#if defined(TILED_DEBUG_DUMPS)
        this->CountStores++;
#endif // TILED_DEBUG_DUMPS

    }
    else
    {
        SpillOptimizer::RemoveInstruction(transferInstruction);
    }

    return cost;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    FindSpillMemory - scan through the IR of a generated spill to
//    locate the memory operand
//
// Arguments:
//
//    spillInstruction - Spill instruction to start with.
//
// Notes:
//
//    All marked spills must result in a memory operand.  This code
//    will assert if that invariant is violated.
//
// Returns:
//
//    Existing spill memory operand. 
//
//------------------------------------------------------------------------------

llvm::MachineOperand *  //IR::MemoryOperand*
SpillOptimizer::FindSpillMemory
(
   llvm::MachineInstr * spillInstruction
)
{
   llvm::MachineInstr *   nextInstruction = spillInstruction;
   llvm::MachineOperand * destinationOperand = (nextInstruction->defs()).begin();

   // advance the next pointer to the last spill instruction
   // that defines the spill location.

   while (!nextInstruction->mayStore())
   {
      llvm::MachineInstr * searchInstruction = nextInstruction->getNextNode();

#if defined (TILED_DEBUG_CHECKS)
      llvm::MachineOperand * singleExplicitSource = this->getSingleExplicitSourceOperand(searchInstruction);
      assert(singleExplicitSource != nullptr);
      assert(this->GetSpillKinds(searchInstruction) == GraphColor::SpillKinds::Spill);
      Tiled::VR::Info *  aliasInfo = this->VrInfo;
      assert(aliasInfo->MustTotallyOverlap(destinationOperand, singleExplicitSource));
#endif // TILED_DEBUG_CHECKS

      nextInstruction = searchInstruction;
      destinationOperand = (nextInstruction->defs()).begin();
   }
   
   llvm::MachineInstr::mop_iterator o;
   for (o = nextInstruction->operands_begin(); o != nextInstruction->operands_end(); ++o)
      if (o->isFI()) break;

   assert(o != nextInstruction->operands_end());
   return &(*o);
}

llvm::MachineOperand *  //IR::MemoryOperand*
SpillOptimizer::FindReloadMemory
(
   llvm::MachineInstr * reloadInstruction
)
{
   llvm::MachineInstr *   nextInstruction = reloadInstruction;

   // advance the next pointer to the last spill instruction
   // that defines the spill location.

   while (!nextInstruction->mayLoad())
   {
      llvm::MachineInstr * searchInstruction = nextInstruction->getNextNode();

#if defined (TILED_DEBUG_CHECKS)
      llvm::MachineOperand * destinationOperand = (nextInstruction->defs()).begin();
      llvm::MachineOperand * singleExplicitSource = this->getSingleExplicitSourceOperand(searchInstruction);
      assert(singleExplicitSource != nullptr);
      assert(this->GetSpillKinds(searchInstruction) == GraphColor::SpillKinds::Reload);
      Tiled::VR::Info *  aliasInfo = this->VrInfo;
      assert(aliasInfo->MustTotallyOverlap(destinationOperand, singleExplicitSource));
#endif // TILED_DEBUG_CHECKS

      nextInstruction = searchInstruction;
   }

   llvm::MachineInstr::mop_iterator o;
   for (o = nextInstruction->operands_begin(); o != nextInstruction->operands_end(); ++o)
      if (o->isFI()) break;

   assert(o != nextInstruction->operands_end());
   return &(*o);
}

Graphs::OperandToSlotMap::iterator
SpillOptimizer::NewSlot
(
   Graphs::OperandToSlotMap * idToSlotMap,
   unsigned                   id
)
{
   llvm::MachineFunction *   functionUnit = this->Allocator->FunctionUnit->machineFunction;
   llvm::MachineFrameInfo&   MFI = functionUnit->getFrameInfo();
   Graphs::FrameIndex     frameSlot = MFI.CreateSpillStackObject(8, 8);

   Graphs::SlotEntry entry(frameSlot);
   Graphs::OperandToSlotMap::value_type tuple(id, entry);
   std::pair<Graphs::OperandToSlotMap::iterator, bool> result = idToSlotMap->insert(tuple);
   if (!result.second) {
      (result.first)->second = entry;
   }

   return result.first;
}

llvm::MachineOperand *
getRegisterSourceOperand
(
   llvm::MachineInstr *    nextInstruction,
   llvm::MachineOperand *  operand,
   Tiled::VR::Info *       vrInfo
)
{
   llvm::MachineInstr::mop_iterator iter;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(nextInstruction->uses().begin(), nextInstruction->explicit_operands().end());

   // foreach_register_source_opnd
   for (iter = uses.begin(); iter != uses.end(); ++iter)
   {
      llvm::MachineOperand * srcOperand = &(*iter);

      if (srcOperand->isReg() && vrInfo->MustTotallyOverlap(srcOperand, operand)) {
         return srcOperand;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Generate the spill operand for spilling this live range
//
// Arguments:
//
//    operand         - live range appearance operand
//    liveRange       - Spill operand live range
//    tile            - tile context
//    isValidInMemory - out parameter for valid value already in memory
//
// Remarks:
//
//    Create and cache (or use) live range backing symbol
//
//    Note: spilling back to a global or shared location must be proven before
//    spill costing and the symbol added to the live range.
//
// Returns:
//
//    New spill operand using backing symbol.
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetSpillOperand
(
   llvm::MachineOperand *  operand,
   GraphColor::LiveRange * liveRange,
   GraphColor::Tile *      tile,
   bool&                   isValidInMemory
)
{
   assert(operand->isReg());
  
   GraphColor::Allocator *   allocator = this->Allocator;
   TargetRegisterAllocInfo * targetRegisterAllocator = allocator->TargetRegisterAllocator;
   Graphs::FlowGraph *       functionUnit = allocator->FunctionUnit;
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   unsigned                  baseReg = liveRange->Register;
   Graphs::FrameIndex        frameSlot = liveRange->FrameSlot;
   Tiled::VR::StorageClass   storageClass = Tiled::VR::StorageClass::Auto;
   bool                      doUpdate = true;

   // ISSUE-TODO-aasmith-2017/02/17: For now all memory spills are
   // full-register-size, no need for operand vs. register size computations
   //Types::Type *           baseType = liveRange->Type;
   //Types::Type *           type;
   //type = targetRegisterAllocator->GetMachineSpillType(functionUnit, operand->getReg(), operand->Type);

   // If we are doing costing, use the dummy spill symbol.

   if (frameSlot == VR::Constants::InvalidFrameIndex) {
      frameSlot = allocator->CostSpillSymbol;

      // Avoid live range updates if we're just working with the CostSpillSymbol;
      doUpdate = (frameSlot == VR::Constants::InvalidFrameIndex);
   }

   llvm::MachineInstr * instruction = operand->getParent();

   if ((frameSlot == VR::Constants::InvalidFrameIndex)
      && (this->GetSpillKinds(instruction) == GraphColor::SpillKinds::Reload)) {

      // Reload.  So if we haven't established a spill symbol yet inherit this one.
      llvm::MachineOperand * reloadSourceOperand = this->FindReloadMemory(instruction);
      assert((reloadSourceOperand != nullptr) && (reloadSourceOperand->isFI()));
      Graphs::FrameIndex subsumeSlot = reloadSourceOperand->getIndex();

      // Don't subsume a symbol that isn't large enough or if the reload has its address
      // calculation hoisted by the target

      //if ((subsumeSymbol != nullptr) && (type->BitSize <= subsumeSymbol->Type->BitSize))
      //{
      frameSlot = subsumeSlot;
      liveRange->FrameSlot = frameSlot;

      // Since we are inheriting tell the caller that the value is already valid in memory.
      isValidInMemory = true;
      //}

   } else if (operand->isDef() && this->ShareSpills && !liveRange->IsGlobal()) {
      llvm::MachineInstr * nextInstruction = instruction->getNextNode();
      if (nextInstruction == nullptr)
         nextInstruction = getNextInstrAcrossBlockEnd(instruction);
      unsigned             liveRangeTag = liveRange->VrTag;

      if (nextInstruction != nullptr) {
         assert(!nextInstruction->isLabel());

         llvm::MachineOperand * spillDestinationOperand = nullptr;
         // If we're processing a def and the next instruction is a spill and we haven't spilled this tag yet to
         // memory (committed to a spill slot already) subsume the spill location.

         if (!GraphColor::Tile::IsTileBoundaryInstruction(nextInstruction)
            && (this->GetSpillKinds(nextInstruction) == GraphColor::SpillKinds::Spill)
            && getRegisterSourceOperand(nextInstruction, operand, vrInfo) != nullptr) {
            // advance the next pointer to the last spill instruction
            // that defines the spill location.

            spillDestinationOperand = this->FindSpillMemory(nextInstruction);
         } else {
            nextInstruction = nullptr;
         }

         if (nextInstruction != nullptr) {
            // We have a spill.  So if compatible subsume this one.
            // It's possible that the spill might have more than one destination.

            assert(spillDestinationOperand != nullptr && spillDestinationOperand->isFI());

            Graphs::FrameIndex   subsumeSlot = VR::Constants::InvalidFrameIndex;
            if (spillDestinationOperand->isFI()) {
               subsumeSlot = spillDestinationOperand->getIndex();
            }

            // The symbol may be null if the address calculation has been hoisted by the target
            if (subsumeSlot != VR::Constants::InvalidFrameIndex) {
               // Don't subsume a symbol that isn't large enough.
               bool subsumedSize = true; //was:  (type->BitSize <= subsumeSymbol->Type->BitSize)
               bool lrWasSpilled = this->SpilledTagBitVector->test(liveRangeTag);

               if ((subsumeSlot == frameSlot) || (!lrWasSpilled && subsumedSize)) {

                  if (doUpdate) {
                     // Update prior pending spills to account for inheritance.
                     if (subsumeSlot != frameSlot) {
                        this->UpdatePendingSpills(liveRangeTag, spillDestinationOperand, instruction);
                     }

                     frameSlot = subsumeSlot;

                     // Only update if we're really spill mode.
                     liveRange->FrameSlot = frameSlot;
                  }

                  // Since we are inheriting tell the caller that the value is already valid in memory.
                  isValidInMemory = true;
               }
            }
         }
      }
   }

   // If we are doing actual spilling, get spill symbol for live range.

   if (frameSlot == VR::Constants::InvalidFrameIndex) {

      Graphs::OperandToSlotMap *  localIdToSlotMap = tile->LocalIdMap;
      assert(operand->isReg());
      unsigned                    localId = (operand->isReg())? operand->getReg() : 0;

      //baseType = targetRegisterAllocator->GetMachineSpillType(functionUnit, baseReg, baseType);
      //symbol = operand->Symbol;

      Graphs::OperandToSlotMap::iterator idToSlotIter = localIdToSlotMap->find(localId);
      if (idToSlotIter != localIdToSlotMap->end()) {
         frameSlot = (idToSlotIter->second).frameSlot;
      }

      // If we haven't found a symbol on the operand and or in the map, or if the found symbol is too small to
      // hold the base size of the live range make a new backing symbol

      if ( (frameSlot == VR::Constants::InvalidFrameIndex)
      // || (symbol->Type->BitSize < baseType->BitSize)
         || !(targetRegisterAllocator->CanReuseSpillSymbol(frameSlot, liveRange->IsGlobal(), liveRange->IsPreColored))) {

         // Make a backing symbol at least as big as the widest resource appearance
         //was: symbol = Symbols::VariableSymbol::NewTemporary(symbolTable, baseType, localId);
         idToSlotIter = SpillOptimizer::NewSlot(localIdToSlotMap, localId);
         frameSlot = (idToSlotIter->second).frameSlot;

         if (liveRange->IsCalleeSaveValue) {
            // Make a special local symbol of callee save storage class to hold the live through value from
            // parent.
            storageClass = Tiled::VR::StorageClass::CalleeSave;
            (idToSlotIter->second).storageClass = storageClass;
         }
      }

      // Update live range with spill symbol.
      liveRange->FrameSlot = frameSlot;

      // Update global live range with spill symbol if necessary.
      if (liveRange->IsGlobal()) {
         GraphColor::LiveRange * globalLiveRange = liveRange->GlobalLiveRange;
         assert((globalLiveRange == liveRange) || (globalLiveRange->FrameSlot == VR::Constants::InvalidFrameIndex));

         globalLiveRange->FrameSlot = frameSlot;
      }
   }


   //  In LLVM an abstract FrameIndex (isFI) MachineOperand is sufficient, no explicit frame register
   //  MachineOperand is needed as base to build MachineInstr accessing stack frame.
   llvm::MachineOperand * spillOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateFI(frameSlot));

   return spillOperand;
}

llvm::MachineOperand *
SpillOptimizer::getSingleExplicitSourceOperand
(
   llvm::MachineInstr * instruction
)
{
   llvm::MachineOperand * result = nullptr;

   //The code below emulates the SingleExplicitSourceOperand property of IR::Instruction in Phoenix
   if ((instruction->explicit_operands().end() - instruction->uses().begin()) == 1) {
      llvm::MachineOperand * tmp = instruction->uses().begin();
      if (!tmp->isReg() || !tmp->isImplicit()) {
         result = tmp;
      }
   }

   return result;
}

llvm::MachineOperand *
SpillOptimizer::getSingleExplicitDestinationOperand
(
   llvm::MachineInstr * instruction
)
{
   llvm::MachineOperand * result = nullptr;

   if ((instruction->defs().end() - instruction->defs().begin()) == 1) {
      llvm::MachineOperand * tmp = instruction->defs().begin();
      if (!tmp->isReg() || !tmp->isImplicit()) {
         result = tmp;
      }
   }

   return result;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Replace the spill temporary register that is the point target for spilling this live range
//
// Arguments:
//
//    temporaryOperand - current temporary register
//    replaceOperand   - operand to replace
//    doInsert         - flag controlling whether changes should be made manifest in the IR
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::ReplaceSpillAppearance
(
   llvm::MachineOperand *  temporaryOperand,
   llvm::MachineOperand ** replaceOperand,
   bool                    doInsert
)
{
   // **replaceOperand because some of the clients still use it after the call,
   // and a valid pointer to (dangling) operand is needed.
   //TBD:  Does temporaryOperand have the same requirement?

   if (doInsert) {

      //Types::Field ^        originalField = replaceOperand->Field;
      llvm::MachineInstr *  instruction = (*replaceOperand)->getParent();
      unsigned              originalRegister = VR::Constants::InvalidReg;

      // If we are dealing with a variable operand register appearance keep the original register
      if ((*replaceOperand)->isReg()) {
         originalRegister = (*replaceOperand)->getReg();
      }

      //TBD:  temporaryOperand->SetCannotMakeExpressionTemporary();

      llvm::MachineOperand * insertOperand = temporaryOperand;
      if (temporaryOperand->getParent()) {
         insertOperand = this->CopyOperand(temporaryOperand);
      }

      insertOperand = this->replaceAndUnlinkOperand(instruction, replaceOperand, insertOperand);

      if ((*replaceOperand)->isReg()) {
         assert(originalRegister != VR::Constants::InvalidReg);

         // This resets a "high" pseudo register to the base pseudo if we see that spilling
         // has cut up the partial defs into disjoint lifetimes.  We see this by looking at the
         // size of the reload represented by the temporaryOperand.

         // <place for code supporting architectures with sub-registers>
      }

      return insertOperand;

   } else {

      return *replaceOperand;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get last reload, folded reload, or recalculate definition temporary
//
// Arguments:
//
//    operand - Operand to find last definition temporary.
//
// Note:
//
//   This API will return the definition of a folded reload that requires hoisting to be manifest. This logic
//   is need for spill temporary consistency.  For the last "real" definition call GetDefinition().
//
// Returns:
//
//    Last definition
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetLastDefinitionTemporary
(
   llvm::MachineOperand * appearanceOperand
)
{
   unsigned             appearanceOperandTag = this->VrInfo->GetTag(appearanceOperand->getReg());
   llvm::MachineInstr * instruction = appearanceOperand->getParent();

   //Assert(instruction != instruction->DummyInstruction);
   assert(instruction != nullptr);

   GraphColor::SpillKinds kinds = GraphColor::SpillKinds( unsigned(GraphColor::SpillKinds::Reload)
#ifdef ARCH_WITH_FOLDS
                                                        | unsigned(GraphColor::SpillKinds::FoldedReload)
#endif
                                                        | unsigned(GraphColor::SpillKinds::Recalculate) );

   GraphColor::SpillRecord * spillRecord = this->GetDominating(appearanceOperandTag, instruction, kinds);

   if (spillRecord != nullptr) {
      if (spillRecord->HasReload()) {
         return spillRecord->ReloadOperand;
      } else if (spillRecord->HasRecalculate()) {
         return spillRecord->RecalculateOperand;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get last reload, or recalculate definition.
//
// Arguments:
//
//    aliasTag - Tag to search for.
//    instruction - Location.
//
// Returns:
//
//    Dominating definition
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetDefinition
(
   unsigned             aliasTag,
   llvm::MachineInstr * instruction
)
{
   assert(instruction != nullptr);

   GraphColor::SpillKinds acceptableSpillKinds
      = GraphColor::SpillKinds(unsigned(SpillKinds::Reload)
                             | unsigned(SpillKinds::Recalculate)
#ifdef ARCH_WITH_FOLDS
                             | unsigned(SpillKinds::FoldedReload)
#endif
                              );

   GraphColor::SpillRecord * spillRecord = this->GetDominating(aliasTag, instruction, acceptableSpillKinds);

   if (spillRecord != nullptr) {
      // Opt out of hoisting folded reloads.  We can do better than this if
      // we are willing to hoist out the folded reload at this point.

#ifdef ARCH_WITH_FOLDS
      if ((unsigned(spillRecord->SpillKinds) & unsigned(SpillKinds::FoldedReload)) != unsigned(GraphColor::SpillKinds::None)) {
         return nullptr;
      }
#endif

      if (spillRecord->HasReload()) {
         return (this->InRange(spillRecord, SpillKinds::Reload))? spillRecord->ReloadOperand : nullptr;
      } else if (spillRecord->HasRecalculate()) {
         return (this->InRange(spillRecord, SpillKinds::Recalculate))? spillRecord->RecalculateOperand : nullptr;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Generate the spill temporary register that is the point target for spilling this live range
//
// Arguments:
//
//    replaceOperand   - operand to replace
//    spillOperand     - operand representing the backing value (either memory or register)
//    doInsert         - flag controlling whether changes should be made manifest in the IR
//
// Returns:
//
//    New spill temporary
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetSpillTemporary
(
   llvm::MachineOperand * replaceOperand,
   llvm::MachineOperand * spillOperand,  //IR::MemoryOperand*
   bool                   doInsert
)
{
    GraphColor::Allocator *   allocator = this->Allocator;
    llvm::SparseBitVector<> * doNotSpillAliasTagBitVector = allocator->DoNotSpillAliasTagBitVector;
    llvm::MachineFunction *   functionUnit = this->Allocator->FunctionUnit->machineFunction;
    Tiled::VR::Info *         vrInfo = this->VrInfo;
    unsigned                  replaceOperandTag = vrInfo->GetTag(replaceOperand->getReg());
    GraphColor::LiveRange *   liveRange = this->GetLiveRange(replaceOperandTag);
    assert(liveRange != nullptr);
    llvm::MachineOperand *    definitionOperand = this->GetLastDefinitionTemporary(replaceOperand);
    llvm::MachineOperand *    temporaryOperand;

    // Share if the def is the prior instruction (linked register) - for the general case of sharing a def use
    // GetReload or GetRecalculate.  This logic is strictly to support operands equal constraints.

    llvm::MachineInstr * instruction = replaceOperand->getParent();
    bool                 isAdjacentDefinition = (definitionOperand != nullptr)?
                            (definitionOperand->getParent() == instruction->getPrevNode()) : false;

    if ((definitionOperand != nullptr) && replaceOperand->isDef() && (isAdjacentDefinition || this->ShareSpills)) {
        // Reuse this temporary if it is immediately prior (needed for opeq) or we are sharing spills.

        //TBD:  definitionOperand->SetCannotMakeExpressionTemporary();
        temporaryOperand = this->CopyOperand(definitionOperand);

        if (!isAdjacentDefinition) {
            unsigned tempOperandTag = vrInfo->GetTag(temporaryOperand->getReg());
            vrInfo->MinusMayPartialTags(tempOperandTag, doNotSpillAliasTagBitVector);
        }

    } else {

        // Allocate new virtual register
        llvm::MachineRegisterInfo& MRI = allocator->MF->getRegInfo();
        unsigned replaceRegister = replaceOperand->getReg();
        const llvm::TargetRegisterClass * regClass = MRI.getRegClass(replaceRegister);
        unsigned temporaryRegister = MRI.createVirtualRegister(regClass);

        // Create simple spill appearance based on the type of the liveRange.  This let's us widen the appearance
        // if needed to satisfy a register link.

        temporaryOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(temporaryRegister, true));

        //TODO:  temporaryOperand->SetCannotMakeExpressionTemporary();

        unsigned tempOperandTag = vrInfo->GetTag(temporaryOperand->getReg());
        doNotSpillAliasTagBitVector->set(tempOperandTag);

        if (doInsert) {
            // set up shared register operand to refer to the spill symbol

            if (spillOperand != nullptr) {
                Graphs::OperandToSlotMap *  localIdToSlotMap = this->Allocator->LocalIdMap;
                Graphs::FrameIndex frameSlot;
                Tiled::VR::StorageClass  storageClass;

                //temporaryOperand->ChangeToSymbol(spillOperand->Symbol);
                if (spillOperand->isFI()) {
                    frameSlot = spillOperand->getIndex();
                    storageClass = Tiled::VR::StorageClass::Auto;
                } else {
                    assert(spillOperand->isReg());
                    Graphs::OperandToSlotMap::iterator iter = localIdToSlotMap->find(spillOperand->getReg());
                    assert(iter != localIdToSlotMap->end());
                    frameSlot = (iter->second).frameSlot;
                    storageClass = (iter->second).storageClass;
                }
                Graphs::SlotEntry entry(frameSlot, storageClass);
                Graphs::OperandToSlotMap::value_type tuple(temporaryRegister, entry);
                localIdToSlotMap->insert(tuple);

                // Ensure the storage classes match (auto vs. callee save)  (TBD?)
            }
        }
    }

    return temporaryOperand;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Generate the spill resource that is compatible with the spill memory access size
//
// Arguments:
//
//    registerOperand - temporary register operand being inserted on the spilling instruction
//    spillOperand    - operand representing the backing value
//
// Internal:
//
//    This should be in the MD target function but we need to mirror some of the logic from priority order to
//    resize resources to meet LIR opcode constraints.  This work around can be removed when the MD code is
//    refactored after deprecating priority order.
//
// Returns:
//
//    Spill register resource.
//
//------------------------------------------------------------------------------

unsigned
SpillOptimizer::GetSpillRegister
(
   llvm::MachineOperand *  registerOperand,
   llvm::MachineOperand *  spillOperand
)
{
   assert(registerOperand->isReg());
   assert(spillOperand->isFI());

   unsigned reg = registerOperand->getReg();

   // <place for code supporting architectures with sub-registers>

   return reg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get closest pending spill kind spill record in the list
//
// Arguments:
//
//    spillRecord - spillRecord to start looking from.
//    spillKinds  - Spill kinds to look for.
//
// Notes:
//
//    spill kinds may be a mask of kinds, search will stop at the first record containing a kind in the mask.
//
// Returns:
//
//    Pending spill record
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::GetPending
(
   GraphColor::SpillRecord * spillRecord,
   GraphColor::SpillKinds    spillKinds
)
{
    unsigned kinds = unsigned(spillKinds);
    unsigned nullKind = unsigned(GraphColor::SpillKinds::None);

   Graphs::FlowGraph * flowGraph = this->Allocator->FunctionUnit;

    while ((spillRecord != nullptr) &&
           (((unsigned(spillRecord->SpillKinds) & kinds) == nullKind) || !spillRecord->HasPending))
    {
        assert((spillRecord->Next == nullptr)
            || flowGraph->Dominates(spillRecord->Next->BasicBlock, spillRecord->BasicBlock));

        spillRecord = spillRecord->Next;
    }

    return spillRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get dominating spill record for a given kind
//
// Arguments:
//
//    aliasTag    - Tag for the location (or proxy) we are spilling.
//    instruction - Instruction that must be dominated by the spill record.
//    spillKinds  - Spill kinds to look for.
//
// Notes:
//
//    spill kinds may be a mask of kinds, search will stop at the first record containing a kind in the mask.
//
// Returns:
//
//    Dominating spill record
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::GetDominating
(
   unsigned               vrTag,
   llvm::MachineInstr *   instruction,
   GraphColor::SpillKinds spillKinds
)
{
    Tiled::VR::Info *         vrInfo = this->VrInfo;
    GraphColor::SpillRecord * dominatingRecord = this->GetDominating(vrTag, instruction);
    unsigned                  kinds = static_cast<unsigned>(spillKinds);
    unsigned                  nullKind = static_cast<unsigned>(GraphColor::SpillKinds::None);

   Graphs::FlowGraph * flowGraph = this->Allocator->FunctionUnit;

    // During costing we need to walk back through the list of records
    // to handle the case where 1) we cost the recalc, and 2) cost the
    // reload.  Since the recalc is in this block and the prior reload
    // potentially in the previous block we look back through.

    while ( (dominatingRecord != nullptr)
         && ((static_cast<unsigned>(dominatingRecord->SpillKinds) & kinds) == nullKind)
         && (vrInfo->MustTotallyOverlap(dominatingRecord->AliasTag, vrTag)))
    {
        assert((dominatingRecord->Next == nullptr)
            || (flowGraph->Dominates(dominatingRecord->Next->BasicBlock, dominatingRecord->BasicBlock)));

        dominatingRecord = dominatingRecord->Next;
    }

    return dominatingRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get dominating spill record.
//
// Arguments:
//
//    aliasTag    - Tag for the location (or proxy) we are spilling.
//    instruction - Instruction that must be dominated by the spill record.
//
// Notes:
//
//    Returns closing dominating record for a given tag.  This function is used to find the right point to
//    start searching for a given spill kind.  Non-dominating records are popped out of the spill map since
//    they can never reach the use we're processing.
//
// Returns:
//
//    Dominating spill record
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::GetDominating
(
   unsigned             vrTag,
   llvm::MachineInstr * instruction
)
{
   GraphColor::AliasTagToSpillRecordIterableMap * spillMap = this->SpillMap;
   llvm::MachineBasicBlock *                      appearanceBlock = instruction->getParent();
   assert(appearanceBlock != nullptr);

   GraphColor::SpillRecord * spillRecord = spillMap->Lookup(vrTag);

   if (spillRecord != nullptr) {

#if defined (TILED_DEBUG_CHECKS)
        // Assert that we're always using the live range tag or a sub piece.  Legacy we'd also tracked the spill
        // temps as needed.  

        // Alias::Info ^ aliasInfo = this->Allocator->AliasInfo;

        // Assert(aliasInfo->MustTotallyOverlap(spillRecord->AliasTag, aliasTag));
#endif // TILED_DEBUG_CHECKS

     Graphs::FlowGraph * flowGraph = this->Allocator->FunctionUnit;

      while (!flowGraph->Dominates(spillRecord->BasicBlock, appearanceBlock))
      {
         spillRecord = this->Pop(spillRecord);

         if (spillRecord == nullptr) {
            break;
         }
      }
   }

   return spillRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Pop spill record.
//
// Arguments:
//
//    spillRecord - Spill record to pop.
//
// Returns:
//
//    Popped spill record.
//
//------------------------------------------------------------------------------

GraphColor::SpillRecord *
SpillOptimizer::Pop
(
   GraphColor::SpillRecord * spillRecord
)
{
    Tiled::VR::Info *         vrInfo = this->Allocator->VrInfo;
    GraphColor::SpillRecord * nextRecord = spillRecord->Next;
    unsigned                  spillTag = spillRecord->AliasTag;

    //For architectures without sub-registers the commented below loop collapses to single iteration
    //foreach_must_total_alias_of_tag(tag, spillTag, aliasInfo)
    //{
        this->SpillMap->Remove(spillTag, spillRecord);
    //}
    //next_must_total_alias_of_tag;

    if (nextRecord != nullptr) {
        unsigned nextTag = nextRecord->AliasTag;

        //For architectures without subregisters no overlapping tags
        //foreach_may_partial_alias_of_tag(tag, nextTag, aliasInfo)
        //{
            this->SpillMap->Insert(nextTag, nextRecord);
        //}
        //next_may_partial_alias_of_tag;
    }

    spillRecord->Delete();

    return nextRecord;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill kinds mask for a given instruction
//
// Arguments:
//
//    instruction - Instruction to mark.
//    spillKinds  - Spill kinds mask to set.
//
// Notes:
//
//    This writes the passed spill kinds into the instruction rather
//    than 'or' them in.  For add semantics see AddSpillKinds
//
//------------------------------------------------------------------------------

void
SpillOptimizer::SetSpillKinds
(
   llvm::MachineInstr *   instruction,
   GraphColor::SpillKinds spillKinds
)
{
   // Mark instruction with the SpillKind that inserted it.

   //was: instruction->InstructionId = static_cast<unsigned>(spillKinds);
   //temporary map in place of a direct, unsigned field (InstructionId) on each instruction
   //TBD: if possible, implement as an unsigned data member in the Cascade extention to MachineOperand (?)

   std::map<llvm::MachineInstr*, GraphColor::SpillKinds>::iterator i = SpillOptimizer::instruction2SpillKind.find(instruction);

   if (i == SpillOptimizer::instruction2SpillKind.end()) {
      std::map<llvm::MachineInstr*, GraphColor::SpillKinds>::value_type entry(instruction, spillKinds);
      SpillOptimizer::instruction2SpillKind.insert(entry);
   } else {
      //overwrite
      i->second = spillKinds;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add spill kinds mask for a given instruction
//
// Arguments:
//
//    instruction - Instruction to mark.
//    spillKinds  - Spill kinds mask to set.
//
// Notes:
//
//    This includes the passed spill kinds into the instruction rather
//    than over write.  For over write semantics see SetSpillKinds
//
//------------------------------------------------------------------------------

void
SpillOptimizer::AddSpillKinds
(
   llvm::MachineInstr *   instruction,
   GraphColor::SpillKinds spillKinds
)
{
   // Mark instruction with the SpillKind that inserted it.

   //was:  instruction->InstructionId |= static_cast<unsigned>(spillKinds);
   //temporary map in place of a direct, unsigned field (InstructionId) on each instruction
   //TBD: if possible, implement as an unsigned data member in the Cascade extention to MachineOperand (?)

   unsigned spill_kinds = static_cast<unsigned>(spillKinds);
   std::map<llvm::MachineInstr*, GraphColor::SpillKinds>::iterator i = SpillOptimizer::instruction2SpillKind.find(instruction);

   if (i != SpillOptimizer::instruction2SpillKind.end()) {
      unsigned previous_spill_kinds = static_cast<unsigned>(i->second);
      i->second = static_cast<GraphColor::SpillKinds>(previous_spill_kinds | spill_kinds);
   } else {
      std::map<llvm::MachineInstr*, GraphColor::SpillKinds>::value_type entry(instruction, spillKinds);
      SpillOptimizer::instruction2SpillKind.insert(entry);
   }

}

void
SpillOptimizer::SetInstrCount
(
   llvm::MachineInstr * instruction,
   unsigned             instrCount
)
{
   //TBD: llvm::MachineInstr does not have an ID field
   std::map<llvm::MachineInstr*, unsigned>::value_type entry(instruction, instrCount);

   std::pair<std::map<llvm::MachineInstr*, unsigned>::iterator, bool> result = SpillOptimizer::instruction2InstrCount.insert(entry);
   if (!result.second) {
      //TBD: is the override valid to happen, or should there be an assertion?
      (result.first)->second = instrCount;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get spill kinds mask for a given instruction
//
// Arguments:
//
//    instruction - Instruction to mark.
//
// Returns:
//
//    Instruction spill kinds mask.
//
//------------------------------------------------------------------------------

GraphColor::SpillKinds
SpillOptimizer::GetSpillKinds
(
   llvm::MachineInstr * instruction
)
{
   // Interpret any instruction as a SpillKind

   //was:  return static_cast<GraphColor::SpillKinds>(instruction->InstructionId);
   //temporary map in place of a direct, unsigned field (InstructionId) on each instruction
   //TBD: if possible, implement as an unsigned data member in the Cascade extention to MachineOperand (?)

   std::map<llvm::MachineInstr*, GraphColor::SpillKinds>::iterator i = SpillOptimizer::instruction2SpillKind.find(instruction);

   if (i != SpillOptimizer::instruction2SpillKind.end()) {
      return i->second;
   }

   return GraphColor::SpillKinds::None;
}

unsigned
SpillOptimizer::GetInstrCount
(
   llvm::MachineInstr * instruction
)
{
   //TBD: llvm::MachineInstr does not have an ID field
   std::map<llvm::MachineInstr*, unsigned>::iterator i = SpillOptimizer::instruction2InstrCount.find(instruction);
   if (i == SpillOptimizer::instruction2InstrCount.end())
      return 0;

   return i->second;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Checks if passed record is in range to be reused.
//
// Arguments:
//
//    spillRecord - Spill record to test.
//
// Notes:
//
//    Range is computed based on current flow based instruction count minus the spill records count at
//    insertion.  If this range is less than the instruction sharing limit then the record can be reused. 
//
// Returns:
//
//    TRUE if record is in range.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::InRange
(
   GraphColor::SpillRecord * spillRecord,
   GraphColor::SpillKinds    spillKinds
)
{
    unsigned iCount = this->InstructionCount;
    unsigned recordCount;

    if (spillKinds == GraphColor::SpillKinds::Reload) {
        recordCount = spillRecord->ReloadInstructionCount;
    } else {
        assert(spillKinds == GraphColor::SpillKinds::Recalculate);
        recordCount = spillRecord->RecalculateInstructionCount;
    }

    assert(!(iCount < recordCount));

    unsigned  delta = iCount - recordCount;
    bool      isInRange = (delta <= this->InstructionShareLimit);

    if (isInRange) {
        this->InstructionShareDelta = delta;
    } else {
        this->InstructionShareDelta = 0;
    }

    return isInRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate area for live ranges live in/live out of this block.
//
// Arguments:
//
//    block - Block being analyzed.
//    tile  - Current tile
//
//------------------------------------------------------------------------------

void
SpillOptimizer::CalculateBlockArea
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile
)
{
    GraphColor::Allocator *    allocator = this->Allocator;
    Tiled::VR::Info *            vrInfo = allocator->VrInfo;
    Dataflow::LivenessData *   registerLivenessData =
        allocator->Liveness->GetRegisterLivenessData(block);
   llvm::SparseBitVector<> *  liveInBitVector = registerLivenessData->LiveInBitVector;
   llvm::SparseBitVector<> *  liveOutBitVector = registerLivenessData->LiveOutBitVector;
   llvm::SparseBitVector<> *  liveInOutBitVector = this->ScratchBitVector1;
   llvm::SparseBitVector<> *  seenBitVector = this->ScratchBitVector2;

    // If we're not in an allocate/assign pass for the tile then this is pre-computing pass for global cost of
    // the whole function. In that case use the global context.

    bool  useGlobalLiveRangeContext = (tile->Pass == GraphColor::Pass::Ready);

    liveInOutBitVector->clear();
    seenBitVector->clear();

    *liveInOutBitVector = *liveInBitVector;
    *liveInOutBitVector |= *liveOutBitVector;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = liveInOutBitVector->begin(); a != liveInOutBitVector->end(); ++a)
   {
        unsigned aliasTag = *a;
        if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
            continue;
        }

        if (!seenBitVector->test(aliasTag)) {
            GraphColor::LiveRange * liveRange;

            if (useGlobalLiveRangeContext) {
                liveRange = allocator->GetGlobalLiveRange(aliasTag);
            } else {
                liveRange = tile->GetLiveRange(aliasTag);
            }

            if (liveRange != nullptr) {
                // Increment live range area for crossing one of the block
                // boundaries.
                liveRange->Area++;
            }

            vrInfo->OrMayPartialTags(aliasTag, seenBitVector);
        }
    }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Begin block for costing/spill
//
// Arguments:
//
//    block - Block being analyzed.
//    tile  - Current tile
//
//------------------------------------------------------------------------------

void
SpillOptimizer::BeginBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   bool                      doInsert
)
{
    // Reset an reusable new instances at appropriate points:
    // - at join points during iteration 1 (EBB)
    // - at blocks there after.
    //
    // Note: individual new instances are blocked from being added after iteration 2 through the
    //  "InsertNew{Reload}Definition" functions this forces the spill every where conservative spilling after
    //  two shots at coloring.

   if (tile->Iteration <= this->SpillShareIterationLimit) {
      this->ShareSpills = true;
   } else {
      assert(this->ShareSpills != true);
   }

   llvm::MachineInstr * firstInstruction = (!block->empty()) ? &(block->instr_front()) : nullptr;

   if (firstInstruction) {
      /* && firstInstruction->isLabel()  //was to exclude exception-not-generated-fall-through blocks (?) */
      this->InstructionCount = this->GetInstrCount(firstInstruction);
      //was:  this->InstructionCount = firstInstruction->InstructionId;
   }

   if (!this->EBBShare || Graphs::ExtendedBasicBlockWalker::IsEntry(block) || tile->IsNestedEntryBlock(block)) {
      this->Reset();
   }

   if (!doInsert && this->DoCalculateBlockArea) {
      this->CalculateBlockArea(block, tile);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End block for costing/spill
//
// Arguments:
//
//    block - Block being analyzed.
//    tile  - Current tile
//
//------------------------------------------------------------------------------

void
SpillOptimizer::EndBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   bool                      doInsert
)
{
    // Reset current context memory, context remembers prior spills/reloads and
    // tries to share the load and store instructions.

#if defined (TILED_DEBUG_CHECKS)
   // ensure that the just completed block pass did not corrupt availability or spill record consistency
   tile->AvailableExpressions->CheckAvailable();
   this->CheckRecordConsistency();
#endif

   this->ShareSpills = false;

   unsigned              instructionShareLimit = this->InstructionShareLimit;
   Profile::Probability  highProbability = 0.0;

   llvm::MachineBasicBlock::succ_iterator s;

   if (this->DoShareByEdgeProbability) {
      // Compute the high probability edge of the fan out.

      // foreach_block_succ_edge
      for (s = block->succ_begin(); s != block->succ_end(); ++s)
      {
         Graphs::FlowEdge successorEdge(*s, block);

         Tiled::Profile::Probability   edgeProbability = successorEdge.getProfileProbability();

         if (edgeProbability > highProbability) {
            highProbability = edgeProbability;
         }
      }
   }

   // foreach_block_succ_edge
   for (s = block->succ_begin(); s != block->succ_end(); ++s)
   {
      Graphs::FlowEdge successorEdge(*s, block);

      llvm::MachineBasicBlock * successorBlock = *s;
      unsigned                  edgeDelta = 1;

      if (this->DoShareByEdgeProbability) {
         Profile::Probability   probabilityDelta = 0.0;
         Profile::Probability   edgeProbability = successorEdge.getProfileProbability();
         assert(highProbability >= edgeProbability);

         probabilityDelta = highProbability - edgeProbability;
         edgeDelta += static_cast<unsigned>(instructionShareLimit * probabilityDelta);
      }

      bool isSuccessorTileExit = (tile->IsExitBlock(successorBlock) || tile->IsNestedExitBlock(successorBlock));

      if (!this->EBBShare || Graphs::ExtendedBasicBlockWalker::IsEntry(successorBlock) || isSuccessorTileExit) {

         if (!block->empty()) {
            //TBD: may need to change the lastInstruction argument in ProcessPendingSpills (and its call subtree)
            //     with block directly, as the argument is used to get the parent block anyway
            llvm::MachineInstr * lastInstruction = &(block->instr_back());
            // Successor block is a join this is an exit.
            this->ProcessPendingSpills(lastInstruction, successorBlock, tile, doInsert);
         }
      }

      llvm::MachineInstr * firstInstruction = (!successorBlock->empty())? &(successorBlock->instr_front()) : nullptr;

      if (firstInstruction) {
         /* && firstInstruction->isLabel()  //was to exclude exception-not-generated-fall-through blocks (?) */
         // This will be the instruction count we start with when we process successor block.

         this->SetInstrCount(firstInstruction, this->InstructionCount + edgeDelta);
         //was: firstInstruction->InstructionId = this->InstructionCount + edgeDelta;
      }
   }

   Graphs::FlowGraph * flowGraph = this->Allocator->FunctionUnit;
   if (block == flowGraph->EndNode) {
      this->Reset();
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Cost or do spill of the given operand for the target
//    instruction, tile.
//
// Arguments:
//
//    operand     - Operand to spill
//    instruction - Instruction to spill through
//    tile        - Tile context
//
//------------------------------------------------------------------------------

void
SpillOptimizer::Cost
(
   llvm::MachineOperand * operand,
   llvm::MachineInstr *   instruction,
   GraphColor::Tile *     tile
)
{
   GraphColor::Allocator * allocator = tile->Allocator;
   Tiled::Cost             totalAllocationCost = allocator->ZeroCost;
   Tiled::Cost             totalSpillCost = allocator->ZeroCost;
   Tiled::Cost             totalRecalculateCost = allocator->ZeroCost;
#ifdef ARCH_WITH_FOLDS
   Tiled::Cost             totalFoldCost = allocator->ZeroCost;
#endif
   Tiled::Cost             totalWeightCost = allocator->ZeroCost;
   unsigned                vrTag = this->VrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange * liveRange = tile->GetLiveRange(vrTag);
   unsigned                spillTag = liveRange->GetAliasTag();
   bool                    doCostStress = false;

   // Set up live range costs.

    Tiled::Cost allocationCost = liveRange->AllocationCost;
    Tiled::Cost spillCost = liveRange->SpillCost;
    Tiled::Cost recalculateCost = liveRange->RecalculateCost;
#ifdef ARCH_WITH_FOLDS
    Tiled::Cost foldCost = liveRange->FoldCost;
#endif
    Tiled::Cost weightCost = liveRange->WeightCost;
    Tiled::Cost definitionCost;
    Tiled::Cost reloadCost;
    Tiled::Cost bestCost;

    totalAllocationCost.IncrementBy(&allocationCost);
    totalSpillCost.IncrementBy(&spillCost);
    totalRecalculateCost.IncrementBy(&recalculateCost);
#ifdef ARCH_WITH_FOLDS
    totalFoldCost.IncrementBy(&foldCost);
#endif
    totalWeightCost.IncrementBy(&weightCost);

    this->DoReplaceOperandsEqual = false;

    // Only compute cost if the cost is significant. (once infinity, always infinity)

    llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(), instruction->explicit_operands().end());
    llvm::MachineOperand * sourceVariableOperand = this->GetSpillAppearance(spillTag, uses);

    if (sourceVariableOperand != nullptr) {
        llvm::MachineOperand *  registerOperand = nullptr;

        weightCost = this->ComputeWeight(sourceVariableOperand);
        totalWeightCost.IncrementBy(&weightCost);

        if (!totalRecalculateCost.IsInfinity() || doCostStress) {
            recalculateCost = this->ComputeRecalculate(&registerOperand, sourceVariableOperand, tile);
            totalRecalculateCost.IncrementBy(&recalculateCost);
        }
        assert(registerOperand == nullptr);

#ifdef ARCH_WITH_FOLDS
        if (!totalFoldCost.IsInfinity() || doCostStress) {
            foldCost = this->ComputeFold(sourceVariableOperand, tile);
            totalFoldCost.IncrementBy(&foldCost);
        }
#endif

        if (!totalSpillCost.IsInfinity() || doCostStress) {
            reloadCost = this->ComputeReload(&registerOperand, sourceVariableOperand, tile);
            totalSpillCost.IncrementBy(&reloadCost);
        }
    }

    llvm::MachineOperand * destinationVariableOperand
        = this->GetSpillAppearance(spillTag, instruction->defs());

    if (destinationVariableOperand != nullptr) {
        llvm::MachineOperand *  registerOperand = nullptr;

        // Definition cost can't go to infinity unless this is a callee save value.
        assert(!totalAllocationCost.IsInfinity() || liveRange->IsCalleeSaveValue);

        definitionCost = this->ComputeDefinition(destinationVariableOperand);
        totalAllocationCost.IncrementBy(&definitionCost);

        weightCost = this->ComputeWeight(destinationVariableOperand);
        totalWeightCost.IncrementBy(&weightCost);

        if (!totalRecalculateCost.IsInfinity() || doCostStress) {
            recalculateCost = this->ComputeRecalculate(&registerOperand, destinationVariableOperand, tile);
            totalRecalculateCost.IncrementBy(&recalculateCost);
        }
        assert(registerOperand == nullptr);

#ifdef ARCH_WITH_FOLDS
        if (!totalFoldCost.IsInfinity() || doCostStress) {
            foldCost = this->ComputeFold(destinationVariableOperand, tile);
            totalFoldCost.IncrementBy(&foldCost);
        }
#endif

        if (!totalSpillCost.IsInfinity() || doCostStress) {
            spillCost = this->ComputeSpill(&registerOperand, destinationVariableOperand, tile);
            totalSpillCost.IncrementBy(&spillCost);
        }
    }

   // update live range costs with additional info.
   liveRange->AllocationCost = totalAllocationCost;
   liveRange->SpillCost = totalSpillCost;
   liveRange->RecalculateCost = totalRecalculateCost;
#ifdef ARCH_WITH_FOLDS
   liveRange->FoldCost = totalFoldCost;
#endif
   liveRange->WeightCost = totalWeightCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Cost caller save strategy for the live range
//
// Arguments:
//
//    liveRange - liveRange
//    tile      - Tile context
//
//------------------------------------------------------------------------------

void
SpillOptimizer::CostCallerSave
(
   GraphColor::LiveRange * liveRange,
   GraphColor::Tile *      tile
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   Tiled::Cost             infiniteCost = allocator->InfiniteCost;
   Tiled::UInt             iteration = tile->Iteration;

   // Pre-set caller save cost so we can compute the right "best spill cost".

   liveRange->CallerSaveCost = infiniteCost;

   if (tile->IsFat())
   {
      // No caller save splits for fat tiles.  (all intra block
      // splitting not supported by the current CallerSaveStrategy)

      return;
   }

#ifdef FUTURE_IMPL   // MULTITILE +
    if (liveRange->IsCallSpanning
        && liveRange->HasReference()
        && !liveRange->IsCalleeSaveValue
        && !liveRange->IsSummaryProxy
        && !liveRange->IsBlockLocal
        //&& !liveRange->IsWriteThrough
        && (iteration <= this->CallerSaveIterationLimit))
    {
        GraphColor::AllocatorCostModel ^ costModel = tile->CostModel;
        Tiled::Cost                        allocationCost = liveRange->AllocationCost;
        Tiled::Cost                        bestSpillCost = liveRange->GetBestSpillCost();

        if (costModel->Compare(&bestSpillCost, &allocationCost) > 0)
        {
            liveRange->ResetDecision();

            Tiled::Cost callerSaveCost = this->ComputeCallerSave(liveRange, tile);

            // This addition requires that allocation cost has been computed for this live range before caller save
            // cost is computed.

            callerSaveCost.IncrementBy(&allocationCost);

            liveRange->CallerSaveCost = callerSaveCost;
        }
    }
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Do spill of the given source operand for the target
//    instruction, tile.
//
// Arguments:
//
//    sourceOperand   - widest source operand to spill
//    instruction     - Instruction to spill through
//    spillTag        - live range object tag
//    tile            - Tile context
//    decision        - spill decision to implement
//
// Notes:
//
//    This function is used exclusively from the main
//    ::Spill(operand...) routine and should be considered as local to
//    it.
//
// Returns:
//
//    Cost of spilling the appearance for the instruction
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::SpillSource
(
   llvm::MachineOperand * sourceOperand,
   llvm::MachineInstr *   instruction,
   unsigned               spillTag,
   GraphColor::Tile *     tile,
   GraphColor::Decision   decision
)
{
   GraphColor::Allocator * allocator = tile->Allocator;
   Tiled::VR::Info *       vrInfo = this->VrInfo;
   Tiled::Cost             cost = allocator->ZeroCost;
   const bool              doInsert = true;

   switch (decision)
   {
      case GraphColor::Decision::Recalculate:
      {
         llvm::MachineOperand * registerOperand = nullptr;

         cost = this->ComputeRecalculate(&registerOperand, sourceOperand, tile, doInsert);
         assert(registerOperand != nullptr);

         llvm::MachineOperand * operand = (instruction->uses()).begin();
         unsigned srcIndex = instruction->getOperandNo(operand);

         // foreach_register_source_opnd_editing
         while (srcIndex < instruction->getNumExplicitOperands())
         {
            operand = &(instruction->getOperand(srcIndex));

            if (operand->isReg()) {
               unsigned operandTag = vrInfo->GetTag(operand->getReg());

               if (vrInfo->MustTotallyOverlap(spillTag, operandTag)) {
                  assert(registerOperand != nullptr);
                  llvm::MachineOperand *  oldOperand = &(*operand);

                  this->ReplaceSpillAppearance(registerOperand, &oldOperand, doInsert);
               } else {
                  assert(!vrInfo->MayPartiallyOverlap(spillTag, operandTag));
               }
            }

            ++srcIndex;
         }
      }
      break;

      case GraphColor::Decision::Memory:
      {
         llvm::MachineOperand * registerOperand = nullptr;

         cost = this->ComputeReload(&registerOperand, sourceOperand, tile, doInsert);

         llvm::MachineOperand * operand = (instruction->uses()).begin();
         unsigned srcIndex = instruction->getOperandNo(operand);

         // foreach_register_source_opnd_editing
         while (srcIndex < instruction->getNumExplicitOperands())
         {
            operand = &(instruction->getOperand(srcIndex));

            if (operand->isReg()) {
               unsigned operandTag = vrInfo->GetTag(operand->getReg());

               if (vrInfo->MustTotallyOverlap(spillTag, operandTag)) {
                  assert(registerOperand != nullptr);
                  llvm::MachineOperand *  oldOperand = &(*operand);

                  this->ReplaceSpillAppearance(registerOperand, &oldOperand, doInsert);
               } else {
                  assert(!vrInfo->MayPartiallyOverlap(spillTag, operandTag));
               }
            }

            ++srcIndex;
         }
      }
      break;

#ifdef ARCH_WITH_FOLDS
      case GraphColor::Decision::Fold:
      {
         Tiled::Cost foldCost = allocator->ZeroCost;

         llvm::MachineInstr::mop_iterator operand;
         llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                     instruction->explicit_operands().end());
         // foreach_register_source_opnd_editing
         for (operand = uses.begin(); operand != uses.end(); ++operand)
         {
            if (operand->isReg()) {
               unsigned operandTag = vrInfo->GetTag(operand->getReg());

               if (vrInfo->MustTotallyOverlap(spillTag, operandTag)) {
                  foldCost = this->ComputeFold(operand, tile, doInsert);
                  cost.IncrementBy(&foldCost);
               } else {
                  assert(!vrInfo->MayPartiallyOverlap(spillTag, operandTag));
               }
            }
         }
      }
      break;
#endif

   default:
      assert(false && "unimplemented decision");
   }

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Do spill of the given source operand for the target
//    instruction, tile.
//
// Arguments:
//
//    sourceOperand   - widest source operand to spill
//    instruction     - Instruction to spill through
//    spillTag        - live range object tag
//    tile            - Tile context
//    decision        - Spill decision to implement
//
// Notes:
//
//    This function is used exclusively from the main
//    ::Spill(operand...) routine and should be considered as local to
//    it.
//
// Returns:
//
//    Cost of spilling the appearance for the instruction
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::SpillDestination
(
   llvm::MachineOperand * destinationOperand,
   llvm::MachineInstr *   instruction,
   unsigned               spillTag,
   GraphColor::Tile *     tile,
   GraphColor::Decision   decision
)
{
   GraphColor::Allocator * allocator = tile->Allocator;
   Tiled::VR::Info *       vrInfo = this->VrInfo;
   Tiled::Cost             cost = allocator->ZeroCost;
   const bool              doInsert = true;

   switch (decision)
   {
      case GraphColor::Decision::Recalculate:
      {
         llvm::MachineOperand * registerOperand = nullptr;

         GraphColor::AvailableExpressions * globalAvailableExpressions = allocator->GlobalAvailableExpressions;
         globalAvailableExpressions->RemoveAvailable(destinationOperand);

         // appearance and operand being replaced are the same for recalculate destinations
         cost = this->ComputeRecalculate(&registerOperand, destinationOperand, tile, doInsert);
      }
      break;

      case GraphColor::Decision::Memory:
      {
         llvm::MachineOperand * registerOperand = nullptr;

         // spilling a definition - remove all availability
         tile->RemoveAvailable(destinationOperand);

         cost = this->ComputeSpill(&registerOperand, destinationOperand, tile, doInsert);

         // Only update definitions in fold/reload case.
         // Recalculate does not require it (and this will break it)

         llvm::MachineOperand * dstOperand;
         unsigned dstIndex = 0;

         // foreach_register_destination_opnd_editing
         while (dstIndex < instruction->getNumOperands() &&
                (dstOperand = &(instruction->getOperand(dstIndex)))->isReg() &&
                dstOperand->isDef())
         {
            unsigned operandTag = vrInfo->GetTag(dstOperand->getReg());

            if (vrInfo->MustTotallyOverlap(spillTag, operandTag)) {
               assert(registerOperand != nullptr);
               registerOperand = this->ReplaceSpillAppearance(registerOperand, &dstOperand, doInsert);
            } else {
               assert(!vrInfo->MayPartiallyOverlap(spillTag, operandTag));
            }

            ++dstIndex;
         }

         if (registerOperand->getParent() == nullptr) {
            delete registerOperand;
         }
      }
      break;

#ifdef ARCH_WITH_FOLDS
      case GraphColor::Decision::Fold:
      {
         Tiled::Cost foldCost;

         llvm::MachineInstr::mop_iterator operand;
         llvm::iterator_range<llvm::MachineInstr::mop_iterator> range(instruction->defs().begin(),
                                                                      instruction->defs().end() );
         // foreach_register_destination_opnd_editing
         for (operand = range.begin(); operand != range.end(); ++operand)
         {
            if (operand->isReg()) {
               unsigned operandTag = vrInfo->GetTag(operand->getReg());

               if (vrInfo->MustTotallyOverlap(spillTag, operandTag)) {
                  foldCost = this->ComputeFold(operand, tile, doInsert);
                  cost.IncrementBy(&foldCost);
               } else {
                  assert(!vrInfo->MayPartiallyOverlap(spillTag, operandTag));
               }
            }
         }

         // Keep track of definitions folded.  We need these to modify liveness at tile boundaries.
         assert(doInsert);
      }
      break;
#endif

      default:
        assert(false && "unimplemented decision!");
   };

   return cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Do spill of the given operand for the target
//    instruction, tile.
//
// Arguments:
//
//    operand     - Operand to spill
//    instruction - Instruction to spill through
//    tile        - Tile context
//
// Returns:
//
//    Cost of spilling the appearance for the instruction
//
//------------------------------------------------------------------------------

Tiled::Cost
SpillOptimizer::Spill
(
   llvm::MachineOperand * operand,
   llvm::MachineInstr *   instruction,
   GraphColor::Tile *     tile
)
{
    GraphColor::Allocator *      allocator = tile->Allocator;
    Tiled::VR::Info *            vrInfo = this->VrInfo;
    unsigned                     aliasTag = vrInfo->GetTag(operand->getReg());
    GraphColor::LiveRange *      liveRange = tile->GetLiveRange(aliasTag);
    Tiled::Cost                  cost;
    Tiled::Cost                  totalCost = allocator->ZeroCost;
    unsigned                     spillTag = liveRange->VrTag;
    GraphColor::Decision         decision = liveRange->Decision();

#if defined (TILED_DEBUG_SUPPORT)
    llvm::MachineInstr *  beforeInstruction = instruction->getPrevNode();
    llvm::MachineInstr *  afterInstruction = instruction->getNextNode();
#endif

    // Clear replace flag until proven otherwise
    this->DoReplaceOperandsEqual = false;

    // look through the instruction source and destination sides and find the widest spill appearance of each.

    llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                instruction->explicit_operands().end());
    llvm::MachineOperand * sourceOperand = this->GetSpillAppearance(spillTag, uses);

    llvm::MachineOperand * destinationOperand = this->GetSpillAppearance(spillTag, instruction->defs());

    if (sourceOperand != nullptr) {
        // perform spill of source side. Insert spill code for widest appearance and wire up all smaller
        // appearances.
        cost = this->SpillSource(sourceOperand, instruction, spillTag, tile, decision);
        totalCost.IncrementBy(&cost);
    }

    if (destinationOperand != nullptr) {
       cost = this->SpillDestination(destinationOperand, instruction, spillTag, tile, decision);
       totalCost.IncrementBy(&cost);
    }

#if defined (TILED_DEBUG_SUPPORT)
    Tiled::Cost insertedSpillCost = liveRange->InsertedSpillCost;
    insertedSpillCost.IncrementBy(&totalCost);
    liveRange->InsertedSpillCost = insertedSpillCost;
#endif

    return totalCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Spill a global tag from a tile summary live range on pass 2.
//
// Arguments:
//
//    globalAliasTag - Tag representing the global being spilled down in this context.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::SpillFromSummary
(
   unsigned                aliasTag,
   GraphColor::LiveRange * summaryLiveRange
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   GraphColor::Tile *         tile = this->Tile;
   llvm::SparseBitVector<> *  globalSpillAliasTagSet = tile->GlobalSpillAliasTagSet;

   assert(summaryLiveRange->Tile == tile);

   // Update tile summary live range map and spill from the summary live range.

   tile->ClearAliasTagToSummaryIdMap(aliasTag);
   summaryLiveRange->RemoveFromSummary(aliasTag);

   // Spilling global, mark on tile.

   GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);

   if (globalLiveRange != nullptr) {
      GraphColor::Decision decision = globalLiveRange->Decision();

      switch (decision)
      {
        case GraphColor::Decision::Memory:
        default:
        {
         tile->MemoryAliasTagSet->set(aliasTag);
        }
        break;

        case GraphColor::Decision::Recalculate:
        {
         tile->RecalculateAliasTagSet->set(aliasTag);
        }
        break;

#ifdef ARCH_WITH_FOLDS
        case GraphColor::Decision::Fold:
        {
         tile->FoldAliasTagSet->set(aliasTag);
        }
#endif
      };

      globalSpillAliasTagSet->set(aliasTag);
    }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Note the tile boundary instructions during cost/spill processing. 
//
// Arguments:
//
//    instruction            - Tile boundary instruction (EnterTile|ExitTile)
//    spillAliasTagBitVector - Set of tags being spilled
//    doInsert               - Do code inserts
//
//------------------------------------------------------------------------------

void
SpillOptimizer::TileBoundary
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * spillAliasTagBitVector,
   bool                      doInsert
)
{
   if (spillAliasTagBitVector == nullptr) return;

   GraphColor::Tile *        tile = this->Tile;
   GraphColor::TileGraph *   tileGraph = tile->TileGraph;
   GraphColor::Allocator *   allocator = this->Allocator;
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   llvm::SparseBitVector<> * doNotSpillAliasTagBitVector = allocator->DoNotSpillAliasTagBitVector;

   // Process tile boundary instructions for any direct spills:
   GraphColor::Tile * nestedTile = tileGraph->GetNestedTile(instruction->getParent());

   if (Tile::IsEnterTileInstruction(instruction)) {
      llvm::MachineOperand * dstOperand;
      unsigned dstIndex = instruction->getNumExplicitOperands();
      unsigned numOperands = instruction->getNumOperands();

      // foreach_destination_opnd_editing
      while (dstIndex < numOperands && (dstOperand = &(instruction->getOperand(dstIndex))) && dstOperand->isDef())
      {
         unsigned destinationTag = vrInfo->GetTag(dstOperand->getReg());

         if (vrInfo->CommonMayPartialTags(destinationTag, spillAliasTagBitVector)) {
            instruction->RemoveOperand(dstIndex);

            GraphColor::LiveRange * nestedSummaryLiveRange = nestedTile->GetSummaryLiveRange(destinationTag);
            // if the nested summary live range is still mapped, spill it.
            if (nestedSummaryLiveRange != nullptr) {
               assert(!nestedSummaryLiveRange->HasReference());
               nestedSummaryLiveRange->IsSecondChanceGlobal = true;
            }
            continue;
         }

         ++dstIndex;
      }

   } else {
      assert(Tile::IsExitTileInstruction(instruction));

      llvm::MachineOperand * srcOperand;
      unsigned numExplicitOperands = instruction->getNumExplicitOperands();
      unsigned srcIndex = 0;

      // foreach_source_opnd_editing
      while (srcIndex < numExplicitOperands && (srcOperand = &(instruction->getOperand(srcIndex))) && srcOperand->isUse())
      {
         unsigned sourceTag = vrInfo->GetTag(srcOperand->getReg());
         GraphColor::LiveRange * nestedSummaryLiveRange = nestedTile->GetSummaryLiveRange(sourceTag);

         if (vrInfo->CommonMayPartialTags(sourceTag, spillAliasTagBitVector)) {
            instruction->RemoveOperand(srcIndex);
            continue;
         }

         ++srcIndex;
      }
   }

   // For EnterTiles, when we have spill tags, and we're inserting
   // code to stretch spill live ranges if in "range" for reuse in pass 2.

   if (doInsert) {
      llvm::SparseBitVector<>::iterator sp;

      // foreach_sparse_bv_bit
      for (sp = spillAliasTagBitVector->begin(); sp != spillAliasTagBitVector->end(); ++sp)
      {
         unsigned spillTag = *sp;
         GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(spillTag);

         if (globalLiveRange != nullptr) {
            if (globalLiveRange->IsCalleeSaveValue) {
               // Don't extend callee save values (spilled one place in a tile is enough - a kill is a
               // kill). Once a "switch" spill is in then the value can not be extended past it. 
               continue;
            }

            // Boundary instructions are marked with the tile they Enter/Exit in the instructionId

            unsigned tileId = allocator->GetTileId(instruction);
            GraphColor::Tile * childTile = tileGraph->GetTileByTileId(tileId);

            assert(childTile->ParentTile == tile);
            assert(childTile->EntryBlockSet->test(instruction->getParent()->getNumber())
                  || childTile->ExitBlockSet->test(instruction->getParent()->getNumber()));

            GraphColor::LiveRange * summaryLiveRange = childTile->GetSummaryLiveRange(spillTag);

            if ((summaryLiveRange != nullptr) && !summaryLiveRange->IsSecondChanceGlobal) {
               // Append copy of last spill def for this tag if in range and associate
               // it with the global live range tag.

               llvm::MachineOperand * availableDefinition = this->GetDefinition(spillTag, instruction);

               if ((availableDefinition != nullptr) && (Tile::IsEnterTileInstruction(instruction))) {
                  //&& (availableDefinition->Field->BitSize >= globalLiveRangeBitSize)
                  llvm::MachineOperand  operand = llvm::MachineOperand::CreateReg(availableDefinition->getReg(), false);
                  instruction->addOperand(operand);
                  //std::cout << " ENTER#" << instruction->getParent()->getNumber() << ": availableOperand = " << operand.getReg() << std::endl;

                  unsigned lastExplicitOperandIndex = instruction->getNumExplicitOperands() - 1;
                  llvm::MachineOperand * extendedUse = &(instruction->getOperand(lastExplicitOperandIndex));

                  this->SetSpillAliasTag(extendedUse, globalLiveRange->VrTag);
                  unsigned extendedUseTag = vrInfo->GetTag(extendedUse->getReg());
                  vrInfo->MinusMayPartialTags(extendedUseTag, doNotSpillAliasTagBitVector);
               }
            }

         } else {
            // Do any spills of operands left from prior iterations.

            llvm::MachineOperand * srcOperand;
            unsigned srcIndex = (instruction->getNumExplicitOperands() > 0) ? 0 : 1;

            // foreach_register_source_opnd_editing
            while (srcIndex < instruction->getNumExplicitOperands())
            {
               srcOperand = &(instruction->getOperand(srcIndex));

               unsigned sourceTag = vrInfo->GetTag(srcOperand->getReg());

               if (vrInfo->MayPartiallyOverlap(spillTag, sourceTag)) {
                  llvm::MachineOperand * availableDefinition = this->GetDefinition(spillTag, instruction);

                  if (availableDefinition != nullptr) {
                     unsigned globalLiveRangeTag = this->GetSpillAliasTag(srcOperand);
                     assert(globalLiveRangeTag != VR::Constants::InvalidTag);

                     //was: llvm::MachineOperand * extendedUse = instruction->ReplaceOperand(srcOperand, availableDefinition);
                     //** In LLVM implementation there is no actual replacement here.
                     srcOperand->setReg(availableDefinition->getReg());
                     llvm::MachineOperand * extendedUse = srcOperand;

                     this->SetSpillAliasTag(extendedUse, globalLiveRangeTag);

                     unsigned extendedUseTag = vrInfo->GetTag(extendedUse->getReg());
                     vrInfo->MinusMayPartialTags(extendedUseTag, doNotSpillAliasTagBitVector);
                  } else {
                     instruction->RemoveOperand(srcIndex);
                     continue;
                  }
               }

               ++srcIndex;
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Check whether a definition may be folded with a use operand without affecting the
//    machine-dependent legality of the instruction form.
//
// Arguments:
//
//    definitionOperand - definition to fold
//    useOperand - use operand to fold into
//
// Returns:
//
//    True if the fold is legal.
//
//------------------------------------------------------------------------------

#ifdef ARCH_WITH_FOLDS
bool
SpillOptimizer::IsLegalToFoldOperand
(
   llvm::MachineOperand *     definitionOperand,
   llvm::MachineOperand *     useOperand
)
{
   GraphColor::Allocator ^ allocator = this->Allocator;
    Tiled::FunctionUnit ^     functionUnit = allocator->FunctionUnit;
    IR::Instruction ^       definitionInstruction = definitionOperand->Instruction;

    Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;

    if (!definitionInstruction->IsSafetyPure)
    {
        // Don't duplicate impure expressions.

        TILED_VERBOSE_TRACE(GraphColor::Allocator::SpillControl, functionUnit,
            Output::WriteLine(L"  impure definition");
        );

        return false;
    }

    if (definitionInstruction->HasHandlerLabelOperand)
    {
        TILED_VERBOSE_TRACE(GraphColor::Allocator::SpillControl, functionUnit,
            Output::WriteLine(L"  use exception flow");
        );

        return false;
    }

    if (!targetRegisterAllocator->IsFoldableDefinition(definitionOperand))
    {
        return false;
    }

    IR::Operand ^ sourceOperand = definitionInstruction->SingleExplicitSourceOperand;

    if (sourceOperand == nullptr)
    {
        TILED_VERBOSE_TRACE(GraphColor::Allocator::SpillControl, functionUnit,
            Output::WriteLine(L"  multiple sources on definition");
        );

        return false;
    }

    Types::Type ^ useType = useOperand->MachineType;
    Types::Type ^ sourceType = sourceOperand->MachineType;

    if (!functionUnit->Architecture->TypesAreCompatible(useType, sourceType))
    {
        TILED_VERBOSE_TRACE(GraphColor::Allocator::SpillControl, functionUnit,
            Output::WriteLine(L"  incompatible types");
        );

        return false;
    }

    if (!useOperand->IsSafetyPure)
    {
        // Don't duplicate into impure appearances.

        TILED_VERBOSE_TRACE(GraphColor::Allocator::SpillControl, functionUnit,
            Output::WriteLine(L"  impure use");
        );

        return false;
    }

    if (definitionInstruction->IsCommon)
    {
        // At this point there are a few straggling COMMON opcodes but their operands can't promoted but
        // the Legalizer will allow to be promoted.  The down stream lowering might not do the legalization
        // correctly. (LOADEFFECTIVEADDRESS*) left by the lowering of alloca is one example

        return false;
    }

    if (definitionInstruction->HasHandlerLabelOperand)
    {
        // Can't call the legalizer in this case, because we may insert expression
        // temporaries across exception edges to represent that the definition happens
        // only in the EH fall-through block.

        return false;
    }

    if (useOperand->IsAddressModeOperand)
    {
        return targetRegisterAllocator->CanFoldAddressMode(definitionInstruction, useOperand);
    }

    // Don't fold into call instructions if this operand is not the call target.
    // Such folding opportunities can only occur when the register parameters
    // to a function are not fixed as is the case with custom write barrier calling.

    else if (useOperand->Instruction->IsCallInstruction
        && (useOperand != useOperand->Instruction->AsCallInstruction->CallTargetOperand))
    {
        return false;
    }

    // Remaining checks are deferred to target

    return true;
}
#endif // ARCH_WITH_FOLDS

//------------------------------------------------------------------------------
//
// Description:
//
//    Check whether an existing operand may be replaced with a different operand without affecting the
//    machine-dependent legality of the instruction form.
//
// Arguments:
//
//    originalOperand - the original operand
//    replaceOperand - the new operand to check for replacement legality
//
// Returns:
//
//    True if the replacement is legal.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::IsLegalToReplaceOperand
(
   llvm::MachineOperand *     originalOperand,
   llvm::MachineOperand *     replaceOperand
)
{
    llvm::MachineInstr *      instruction = originalOperand->getParent();
    assert(instruction != nullptr);

    //if (originalOperand->Type->BitSize > replaceOperand->Type->BitSize) {
    //   // Don't handle widening the object at this point.
    //   return false;
    //}

    if (SpillOptimizer::IsCalleeSaveTransfer(instruction)) {
       return false;
    }

    if (instruction->isCall() && originalOperand->isReg()) {
       // Illegal to replace operands on a call, since this would change the calling convention.
       // This is largely for ALSA, since we put a use of the ALSA spill on calls for dataflow, and
       // we want the allocator to reload rather than fold.

       return false;
    }

    bool canReplace = false;

    //if (replaceOperand->Register->BitSize < originalOperand->Register->BitSize)) {
    //   Legalize doesn't reason about the register size mismatch - omit replacing in this case.
    //   return false;
    //}

    TargetRegisterAllocInfo *  targetRegisterAllocator = this->Allocator->TargetRegisterAllocator;
    canReplace = targetRegisterAllocator->IsLegalToReplaceOperand(originalOperand, replaceOperand);

    if (originalOperand->isUse()) {
       //canReplace = functionUnit->Legalize->IsLegalToReplaceOperandsEqual(originalOperand,
       //   replaceOperand, operandsEqualOperandBitVector);
       //this->DoReplaceOperandsEqual = (canReplace && !operandsEqualOperandBitVector->IsEmpty);
       this->DoReplaceOperandsEqual = false;  //operandsEqualOperandBitVector is not computed
    }

    return canReplace;

#ifdef ARCH_WITH_FOLDS
    Tiled::FunctionUnit ^     functionUnit = instruction->FunctionUnit;

    if (instruction->HasHandlerLabelOperand)
    {
        // Can't call the legalizer in this case, because we may insert expression
        // temporaries across exception edges to represent that the definition happens
        // only in the EH fall-through block.

        return false;
    }

    // Two appearances can't be made memory on a single instruction.

    Alias::Info ^ aliasInfo = functionUnit->AliasInfo;

    foreach_register_source_opnd(sourceOperand, instruction)
    {
        if (originalOperand == sourceOperand)
        {
            continue;
        }

        if (aliasInfo->MayPartiallyOverlap(sourceOperand, originalOperand))
        {
            return false;
        }
    }
    next_register_source_opnd;

    Tiled::Boolean canReplace = false;
    Tiled::Boolean doOperandsEqualFold = true;

    if (originalOperand->IsUse && doOperandsEqualFold)
    {
        BitVector::Sparse ^ operandsEqualOperandBitVector = this->OperandsEqualOperandBitVector;

        operandsEqualOperandBitVector->ClearAll();

        canReplace = functionUnit->Legalize->IsLegalToReplaceOperandsEqual(originalOperand,
            replaceOperand, operandsEqualOperandBitVector);

        this->DoReplaceOperandsEqual = (canReplace && !operandsEqualOperandBitVector->IsEmpty);

        return canReplace;
    }
    else
    {
        return functionUnit->Legalize->IsLegalToReplaceOperand(originalOperand, replaceOperand);
    }
#endif // ARCH_WITH_FOLDS
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get insertion point for a copy transfer given destination and source operands.
//
// Arguments:
//
//    destinationOperand - Register operand being copied to.
//    sourceOperand      - Register operand being copied from.
//    block              - Entry/Exit block.
//
// Notes:
//
//    Search for insertion point may reorder the instructions in the airlock block to make a insertion point
//    available.  This routine may swap instructions in order to attempt to find an insertion point.
//
// Returns:
//
//    Insert before instruction or nullptr if no insertion point.
//
//------------------------------------------------------------------------------

llvm::MachineInstr *
SpillOptimizer::GetCopyTransferInsertionPoint
(
   llvm::MachineOperand *        destinationOperand,
   llvm::MachineOperand *        sourceOperand,
   llvm::MachineBasicBlock *     block
)
{
   GraphColor::Tile *      tile = this->Tile;
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   llvm::MachineInstr *    insertAfterInstruction;
   llvm::MachineInstr *    insertBeforeInstruction;
   llvm::MachineInstr *    topBlockingInstruction;
   llvm::MachineInstr *    bottomBlockingInstruction;

   for (int pass = 1; pass <= 2; pass++)
   {
      insertAfterInstruction = tile->FindEnterTileInstruction(block);
      insertBeforeInstruction = tile->FindExitTileInstruction(block);
      topBlockingInstruction = nullptr;
      bottomBlockingInstruction = nullptr;

      if (insertAfterInstruction == nullptr) {
         insertAfterInstruction = &(block->front());
      }

      if (insertBeforeInstruction == nullptr) {
         insertBeforeInstruction = &(block->back());
      }

      // Scan forward checking interference and looking for insertion point.

      while (insertAfterInstruction != insertBeforeInstruction)
      {
         if (!Tile::IsEnterTileInstruction(insertAfterInstruction)) {
            if (aliasInfo->DestinationMayPartiallyOverlap(insertAfterInstruction, sourceOperand) != nullptr) {
               topBlockingInstruction = insertAfterInstruction;
               break;
            }
         }

         insertAfterInstruction = insertAfterInstruction->getNextNode();
      }

      insertAfterInstruction = insertAfterInstruction->getPrevNode();

      // Scan backward checking interference and looking for insertion point.

      while (insertBeforeInstruction != insertAfterInstruction)
      {
         if (!Tile::IsExitTileInstruction(insertBeforeInstruction)) {
            if (aliasInfo->SourceMayPartiallyOverlap(insertBeforeInstruction, destinationOperand) != nullptr) {
               bottomBlockingInstruction = insertBeforeInstruction;
               break;
            }
         }

         insertBeforeInstruction = insertBeforeInstruction->getPrevNode();
      }

      if (insertBeforeInstruction != nullptr) {
         insertBeforeInstruction = insertBeforeInstruction->getNextNode();
      } else {
         insertBeforeInstruction = &(block->front());
         return insertBeforeInstruction;
      }

      llvm::MachineInstr *  insertAfterNext = (insertAfterInstruction ? insertAfterInstruction->getNextNode() : &(block->front()));
      if (insertAfterNext == insertBeforeInstruction) {
         return insertBeforeInstruction;
      } else if (topBlockingInstruction == bottomBlockingInstruction) {
         return nullptr;
      }

      // Attempt to swap blocking instructions in hopes of creating insertion point.

      this->GetCopyTransferInsertionPoint(topBlockingInstruction, bottomBlockingInstruction);
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get insertion point for a copy transfer given two blocking instructions.
//
// Arguments:
//
//    topBlockingInstruction    - top blocking instruction
//    bottomBlockingInstruction - bottom blocking instruction
//    block                     - Entry/Exit block.
//
// Notes:
//
//    Attempt to reorder the instructions in the airlock block to make a insertion point
//    available.
//
// Returns:
//
//    Insert before instruction or nullptr if no insertion point.
//
//------------------------------------------------------------------------------

llvm::MachineInstr *
SpillOptimizer::GetCopyTransferInsertionPoint
(
   llvm::MachineInstr * topBlockingInstruction,
   llvm::MachineInstr * bottomBlockingInstruction
)
{
   Graphs::FlowGraph *    flowGraph = this->Allocator->FunctionUnit;
   Tiled::VR::Info *      aliasInfo = this->VrInfo;
   llvm::MachineInstr *   insertBeforeInstruction = nullptr;
   llvm::MachineOperand * dependentOperand = nullptr;

   // Try moving top blocking instruction below bottom blocking instruction.

   dependentOperand = aliasInfo->DependencyForward(topBlockingInstruction, topBlockingInstruction->getNextNode(), bottomBlockingInstruction);

   if (dependentOperand == nullptr) {
        flowGraph->MoveAfter(bottomBlockingInstruction, topBlockingInstruction);
        insertBeforeInstruction = topBlockingInstruction;
   }

   // Try moving bottom blocking instruction above top blocking instruction.

   else
   {
      dependentOperand = aliasInfo->DependencyBackward(bottomBlockingInstruction, bottomBlockingInstruction->getPrevNode(), topBlockingInstruction);
      if (dependentOperand == nullptr) {
         flowGraph->MoveBefore(topBlockingInstruction, bottomBlockingInstruction);
         insertBeforeInstruction = topBlockingInstruction;
      }
   }

   return insertBeforeInstruction;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get register operand for load or store.
//
// Arguments:
//
//    aliasTag - alias tag of register variable.
//
//------------------------------------------------------------------------------

unsigned
SpillOptimizer::GetCopyTransferScratchRegister
(
   unsigned                   targetRegister,
   llvm::MachineBasicBlock *  compensationBlock,
   GraphColor::Tile *         otherTile
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   GraphColor::Liveness *     liveness = allocator->Liveness;
   VR::Info *                 aliasInfo = this->VrInfo;
   llvm::SparseBitVector<> *  liveBitVector = liveness->GetGlobalAliasTagSet(compensationBlock);

   // Get allocatable registers 

   llvm::SparseBitVector<> * allocatableRegisterAliasTagSet
       = allocator->GetAllocatableRegisters(allocator->GetRegisterCategory(targetRegister));
   llvm::SparseBitVector<> * scratchRegisterAliasTagSet = this->ScratchBitVector1;
   *scratchRegisterAliasTagSet = *allocatableRegisterAliasTagSet;

   GraphColor::Tile *  tile = this->Tile;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = liveBitVector->begin(); a != liveBitVector->end(); ++a)
   {
      unsigned liveAliasTag = *a;

      // Handle live physreg case.
      if (VR::Info::IsPhysicalRegisterTag(liveAliasTag)) {
         aliasInfo->MinusMayPartialTags(liveAliasTag, scratchRegisterAliasTagSet);
         continue;
      }

      // Remove the child tile contribution
      GraphColor::LiveRange * summaryLiveRange = tile->GetSummaryLiveRange(liveAliasTag);
      if (summaryLiveRange != nullptr) {
         unsigned tileRegister = summaryLiveRange->Register;
         unsigned registerTag = aliasInfo->GetTag(tileRegister);

         aliasInfo->MinusMayPartialTags(registerTag, scratchRegisterAliasTagSet);
      }

      // Remove the parent tile contribution
      GraphColor::LiveRange * parentSummaryLiveRange = otherTile->GetSummaryLiveRange(liveAliasTag);
      if (parentSummaryLiveRange != nullptr) {
         unsigned parentRegister = parentSummaryLiveRange->Register;
         unsigned registerTag = aliasInfo->GetTag(parentRegister);

         aliasInfo->MinusMayPartialTags(registerTag, scratchRegisterAliasTagSet);
      }
   }

   llvm::MachineInstr * firstInstruction = tile->FindEnterTileInstruction(compensationBlock);
   llvm::MachineInstr * lastInstruction = tile->FindExitTileInstruction(compensationBlock);

   if (firstInstruction != nullptr) {
      // Remove any extended spill operands since these won't be reflected by the globals above.

      llvm::MachineInstr::mop_iterator sourceOperand;
      llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(firstInstruction->uses().begin(),
                                                                  firstInstruction->explicit_operands().end());
      // foreach_register_source_opnd
      for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
      {
         if (sourceOperand->isReg()) {
            unsigned sourceTag = aliasInfo->GetTag(sourceOperand->getReg());
            aliasInfo->MinusMayPartialTags(sourceTag, scratchRegisterAliasTagSet);
         }
      }

      firstInstruction = firstInstruction->getNextNode();
   } else {
      firstInstruction = &(compensationBlock->front());
   }

   if (lastInstruction != nullptr) {
      lastInstruction = lastInstruction->getPrevNode();
   } else {
      lastInstruction = &(compensationBlock->back());
   }

   // Remove defined in block (this pulls out any other scratch regs that have been defined for prior inserts.

   llvm::SparseBitVector<> * definitionTagsBitVector = this->ScratchBitVector2;
   definitionTagsBitVector->clear();

   aliasInfo->SummarizeDefinitionTagsInRange(firstInstruction, lastInstruction,
      Tiled::VR::AliasType::May,
      Tiled::VR::SideEffectSummaryOptions::IgnoreLocalTemporaries,
      definitionTagsBitVector);

   llvm::SparseBitVector<>::iterator d;

   // foreach_sparse_bv_bit
   for (d = definitionTagsBitVector->begin(); d != definitionTagsBitVector->end(); ++d)
   {
      unsigned definitionAliasTag = *d;
      aliasInfo->MinusMayPartialTags(definitionAliasTag, scratchRegisterAliasTagSet);
   }

   // Iterate through the available registers and try and find one at a global cost.

   GraphColor::CostVector *  registerCostVector = allocator->RegisterCostVector;
   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = scratchRegisterAliasTagSet->begin(); r != scratchRegisterAliasTagSet->end(); ++r)
   {
      unsigned     scratchRegisterTag = *r;
      unsigned     scratchRegister = aliasInfo->GetRegister(scratchRegisterTag);
      Tiled::Cost  registerCost = (*registerCostVector)[scratchRegister];

      // incur no new execution cycles to take this scratch register.
      if (Tiled::CostValue::CompareEQ(registerCost.GetExecutionCycles(), 0)) {
         return scratchRegister;
      }
   }

   return VR::Constants::InvalidReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get register operand for load or store.
//
// Arguments:
//
//    aliasTag - alias tag of register variable.
//
//------------------------------------------------------------------------------

llvm::MachineOperand *
SpillOptimizer::GetRegisterOperand
(
   unsigned  aliasTag,
   unsigned  reg
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   llvm::MachineOperand *   regOperand;

   /* obsoleted
    Alias::Info ^           aliasInfo = this->AliasInfo;
    Symbols::LocalIdMap ^   localIdMap = allocator->LocalIdMap;
    Tiled::FunctionUnit ^   functionUnit = allocator->FunctionUnit;
    Types::Field ^          field;
    Symbols::Symbol ^       symbol;
    unsigned                localId;

    // Get register operand.

    aliasInfo->GetLocationId(aliasTag, &localId, &field);

    // Size opts may widen things.   In that case, we need to find the field of the widened tag.

    if (field == nullptr) {
        Alias::Location ^ location = aliasInfo->LocationMap(aliasTag);
        field = location->Field;
    }

    Types::Type ^ type = reg->GetType(functionUnit);
    regOperand = IR::VariableOperand::NewTemporary(functionUnit, type->PrimaryField, localId);
    regOperand->Register = reg->RegisterCategory->PseudoRegister;

    Types::Type ^ targetType = functionUnit->Architecture->TargetRegisterType(field->Type);
    Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;
    Registers::RegisterCategory ^ registerCategory = targetRegisterAllocator->GetRegisterCategory(targetType);

    // Reset to global field if compatible with the register.
    if ((field->BitSize <= reg->BitSize) && (reg->BaseRegisterCategory == registerCategory->BaseRegisterCategory)) {
       regOperand->ResetField(field);
    }

    // Propagate symbol if present.
    symbol = localIdMap->Lookup(localId);
    if (symbol != nullptr) {
        regOperand->ChangeToSymbol(symbol);
    }
    */

    llvm::MachineRegisterInfo& MRI = allocator->MF->getRegInfo();
    const llvm::TargetRegisterClass *  regClass = allocator->GetRegisterCategory(reg);
    unsigned vrtRegister = MRI.createVirtualRegister(regClass);

    regOperand = new llvm::MachineOperand(llvm::MachineOperand::CreateReg(vrtRegister, true));

    return regOperand;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get spill alias tag this operand descends from if available in
//    the extension object.
//
// Arguments:
//
//    operand - Spill operand to check
//
// Returns:
//
//    Prior spill alias tag.
//
//------------------------------------------------------------------------------

unsigned
SpillOptimizer::GetSpillAliasTag
(
   llvm::MachineOperand * operand
)
{
   if (operand->getSpillTag() != 0) {
      return operand->getSpillTag();
   }

    return VR::Constants::InvalidTag;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get live range based on spill optimizer scope.  When running globally get live ranges from the allocator
//    live range vector, otherwise from the appropriate tile.
//
// Arguments:
//
//    aliasTag - Location to get live range for.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
SpillOptimizer::GetLiveRange
(
   unsigned aliasTag
)
{
    GraphColor::LiveRange * liveRange;

    if (this->IsGlobalScope()) {
        GraphColor::Allocator * allocator = this->Allocator;
        liveRange = allocator->GetGlobalLiveRange(aliasTag);
    } else {
        GraphColor::Tile * tile = this->Tile;
        liveRange = tile->GetLiveRange(aliasTag);
    }

    return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set spill alias tag this operand descends from if available in
//    the extension object.
//
// Arguments:
//
//    operand - Spill operand to check
//
//------------------------------------------------------------------------------

void
SpillOptimizer::SetSpillAliasTag
(
   llvm::MachineOperand * operand,
   unsigned               spillAliasTag
)
{
   operand->setSpillTag(spillAliasTag);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set callee save spill kind on the passed instruction.
//
// Arguments:
//
//    instruction - Instruction to mark.
//
//------------------------------------------------------------------------------

void
SpillOptimizer::SetCalleeSaveTransfer
(
   llvm::MachineInstr * instruction
)
{
#ifdef FUTURE_IMPL    // MULTITILE +
    Assert(instruction->IsCopy);
    SpillOptimizer::SetSpillKinds(instruction, GraphColor::SpillKinds::CalleeSave);
#endif
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test for instruction being callee save transfer.
//
// Arguments:
//
//    instruction - Instruction to test.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::IsCalleeSaveTransfer
(
   llvm::MachineInstr * instruction
)
{
   if (instruction->isLabel() || instruction->isBranch()) {
      return false;
   }

   if (GraphColor::Tile::IsTileBoundaryInstruction(instruction)) {
      return false;
   }

   GraphColor::SpillKinds spillKinds = SpillOptimizer::GetSpillKinds(instruction);

   if ((unsigned(spillKinds) & unsigned(SpillKinds::CalleeSave)) != unsigned(SpillKinds::None)) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test for spill kind bits set on a particular instruction.
//
// Arguments:
//
//    instruction - Instruction to test.
//    spillKinds  - Spill kinds mask.
//
// Notes:
//
//    If any of the spill kinds bits are set on the instruction return
//    true.
//
// Returns:
//
//    True of any of the spill kinds bits from the passed mask are set.
//
//------------------------------------------------------------------------------

bool
SpillOptimizer::IsSpillKind
(
   llvm::MachineInstr *   instruction,
   GraphColor::SpillKinds spillKinds
)
{
    GraphColor::SpillKinds kinds = SpillOptimizer::GetSpillKinds(instruction);
    if ((unsigned(kinds) & unsigned(spillKinds)) != unsigned(GraphColor::SpillKinds::None)) {
        return true;
    }
    return false;
}

bool
SpillOptimizer::IsMemoryOperand
(
   llvm::MachineOperand * operand
)
{
   if (operand == nullptr) {
      return false;
   }
   llvm::MachineInstr * parent = operand->getParent();

   if (parent->mayStore() || parent->mayLoad()) {
      return (operand->isFI() /*TBD: || operand->isGlobal() || operand->isSymbol() || operand->isTargetIndex()*/);
   }

   return false;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for spill optimizer.
//
// Arguments:
//
//    allocator - The allocator creating the spill optimizer.
//
// Returns:
//
//    New spill optimizer object.
//
//------------------------------------------------------------------------------

GraphColor::SlotWiseOptimizer *
SlotWiseOptimizer::New
(
  GraphColor::SpillOptimizer * spillOptimizer
)
{
  GraphColor::Allocator * allocator = spillOptimizer->Allocator;
  GraphColor::SlotWiseOptimizer * slotWiseOptimizer = new GraphColor::SlotWiseOptimizer();

  llvm::SparseBitVector<> * generateBitVector = new llvm::SparseBitVector<>();
  llvm::SparseBitVector<> * killedBitVector = new llvm::SparseBitVector<>();
  llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();

  slotWiseOptimizer->SpillOptimizer = spillOptimizer;
  slotWiseOptimizer->GenerateBitVector = generateBitVector;
  slotWiseOptimizer->KilledBitVector = killedBitVector;
  slotWiseOptimizer->LiveBitVector = liveBitVector;

  slotWiseOptimizer->Allocator = allocator;

  return slotWiseOptimizer;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute slot wise data for a given live range for each basic block where it's live.
//
// Arguments:
//
//    block - Block to compute data for
//
// Returns:
//
//    Returns true if all data computes cleanly.  If a un-analyzable case is detected false is returned.
//
//------------------------------------------------------------------------------

bool
SlotWiseOptimizer::ComputeBlockData
(
   llvm::MachineBasicBlock * block
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    GraphColor::Tile ^             tile = this->Tile;
    GraphColor::LiveRange ^        liveRange = this->LiveRange;
    Alias::Info ^                  aliasInfo = allocator->AliasInfo;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    Alias::Tag                     liveRangeAliasTag = liveRange->AliasTag;
    Tiled::Id                        blockId = block->Id;
    GraphColor::Liveness ^         liveness = allocator->Liveness;
    Dataflow::LivenessData ^       registerLivenessData = liveness->GetRegisterLivenessData(block);
    Tiled::Boolean                   isIntegerClass = liveRange->RegisterCategory->IsInt;
    Alias::Tag                     alsaRegisterAliasTag = Alias::Constants::InvalidTag;

    if (allocator->DoInsertAlsaCode)
    {
        // Initialize alsa state if we've inserted alsa code.

        Registers::Register ^  alsaRegister = allocator->FunctionUnit->Frame->AsynchronousFramePointerRegister;

        Assert(alsaRegister != nullptr);

        alsaRegisterAliasTag = aliasInfo->GetTag(alsaRegister);
    }

    BitVector::Sparse ^ integerMaxKillBlockBitVector = tile->IntegerMaxKillBlockBitVector;
    BitVector::Sparse ^ floatMaxKillBlockBitVector = tile->FloatMaxKillBlockBitVector;

    GraphColor::BlockInfo blockInfo;

    blockInfo.ClearAll();

    BitVector::Sparse ^ liveInBitVector = registerLivenessData->LiveInBitVector;
    BitVector::Sparse ^ liveOutBitVector = registerLivenessData->LiveOutBitVector;

    Tiled::Boolean isLiveIn = aliasInfo->CommonMayPartialTags(liveRangeAliasTag, liveInBitVector);
    Tiled::Boolean isLiveOut = aliasInfo->CommonMayPartialTags(liveRangeAliasTag, liveOutBitVector);

    blockInfo.IsLiveIn = isLiveIn;
    blockInfo.IsLiveOut = isLiveOut;

    Tiled::Boolean hasDanglingDefIn = false;

    Graphs::BasicBlock ^ uniqueNonEHPredecessorBlock = block->UniqueNonEHPredecessorBlock;

    if (uniqueNonEHPredecessorBlock != nullptr)
    {
        IR::Instruction ^ lastInstruction = uniqueNonEHPredecessorBlock->LastInstruction;

        if (lastInstruction->IsDanglingInstruction)
        {
            if (aliasInfo->DestinationMayPartiallyOverlap(lastInstruction, liveRangeAliasTag) != nullptr)
            {
                // Treat dangling defs out of predecessors as live into
                // the successor block for purposes of this analysis.

                hasDanglingDefIn = true;
            }
        }
    }

    BitVector::Sparse ^ generateBitVector = this->GenerateBitVector;
    BitVector::Sparse ^ killedBitVector = this->KilledBitVector;
    BitVector::Sparse ^ liveBitVector = this->LiveBitVector;

    Tiled::Boolean hasBackEdgePredecessor = false;
    Tiled::Boolean hasBackEdgeSuccessor = false;
    Tiled::Boolean isEntrySuccessor = false;
    Tiled::Boolean isExitPredecessor = false;

    if (isLiveIn)
    {
        foreach_block_pred_edge(predecessorEdge, block)
        {
            if (predecessorEdge->IsBack)
            {
                hasBackEdgePredecessor = true;
            }

            Graphs::BasicBlock ^ predecessorBlock = predecessorEdge->PredecessorNode;

            if (tile->IsEntryBlock(predecessorBlock))
            {
                isEntrySuccessor = true;
            }
        }
        next_block_pred_edge;

        blockInfo.IsEntry = hasBackEdgePredecessor | isEntrySuccessor;
    }

    if (hasBackEdgePredecessor && !isEntrySuccessor)
    {
        return false;
    }

    if (isLiveOut)
    {
        foreach_block_succ_edge(successorEdge, block)
        {
            if (successorEdge->IsBack)
            {
                hasBackEdgeSuccessor = true;
            }

            Graphs::BasicBlock ^ successorBlock = successorEdge->SuccessorNode;

            if (tile->IsExitBlock(successorBlock))
            {
                isExitPredecessor = true;
            }
        }
        next_block_succ_edge;

        blockInfo.IsExit = hasBackEdgeSuccessor | isExitPredecessor;
    }

    Tiled::Boolean doAnalyze = isLiveIn | isLiveOut | hasDanglingDefIn;

    blockInfo.IsAnalyzed = doAnalyze;

    // If live range being analyzed evaluate the block.

    if (doAnalyze)
    {
        // Reject any blocks with liveness that are unreached from
        // entry.  RPO is unreliable for unreached blocks and we
        // don't want liveness problems.

        if (!block->HasPathFromStart)
        {
            return false;
        }

        liveBitVector->CopyBits(liveOutBitVector);

        Tiled::Boolean isDefinition = false;
        Tiled::Boolean isUse = false;
        Tiled::Boolean isKill = false;

        // ALSA doesn't benefit from the pressure kill logic.

        if (!liveRange->IsAlsaValue)
        {
            if (isIntegerClass)
            {
                if (integerMaxKillBlockBitVector->GetBit(blockId))
                {
                    isKill = true;
                }
            }
            else
            {
                if (floatMaxKillBlockBitVector->GetBit(blockId))
                {
                    isKill = true;
                }
            }
        }

        foreach_instr_in_block_backward(instruction, block)
        {
            liveness->TransferInstruction(instruction, generateBitVector, killedBitVector);

            if (aliasInfo->CommonMayPartialTags(liveRangeAliasTag, killedBitVector))
            {
                // TODO - cover the partial def case - will be extended to insert the appropriate store size.

                if (!aliasInfo->DestinationMustTotallyOverlap(instruction, liveRangeAliasTag))
                {
                    return false;
                }

                isDefinition = true;
                isUse = false;
            }

            if (aliasInfo->CommonMayPartialTags(liveRangeAliasTag, generateBitVector))
            {
                if (!aliasInfo->SourceMustTotallyOverlap(instruction, liveRangeAliasTag))
                {
                    return false;
                }

                // if we have a kill (below) and hit a use it would require a fill before the use and leave the
                // live range live across the kill point.  This is not allowed and so is rejected.

                if (isKill)
                {
                    return false;
                }

                isUse = true;
            }

            // If live before the instruction and not killed by it this is an appearance

            if (instruction->IsCallInstruction)
            {
                IR::CallInstruction ^ callInstruction = instruction->AsCallInstruction;

                if (!liveRange->IsAlsaValue || (!callInstruction->IsAwaitsCall
                    && !callInstruction->IsStackNeutralCall))
                {
                    if (aliasInfo->CommonMayPartialTags(liveRangeAliasTag, liveBitVector))
                    {
                        if (isUse)
                        {

                            return false;
                        }

                        isKill = true;
                        this->CallCount++;
                    }
                }
            }
            else if (liveRange->IsAlsaValue  && !GraphColor::Tile::IsTileBoundaryInstruction(instruction))
            {
                // Handle general ALSA register kills.  We may have blocks
                // where there isn't a call kill but there are still
                // kills.

                Assert(alsaRegisterAliasTag != Alias::Constants::InvalidTag);

                if (aliasInfo->CommonMayPartialTags(liveRangeAliasTag, liveBitVector)
                    && (aliasInfo->DestinationMayPartiallyOverlap(instruction, alsaRegisterAliasTag) != nullptr))
                {
                    isKill = true;
                    this->CallCount++;
                }
            }

            GraphColor::TileGraph ^ tileGraph = tile->TileGraph;

            if (GraphColor::Tile::IsEnterTileInstruction(instruction))
            {
                GraphColor::Tile ^ nestedTile = tileGraph->GetNestedTile(instruction->BasicBlock);

                Assert(nestedTile != nullptr);

                GraphColor::LiveRange ^ summaryLiveRange = nestedTile->GetSummaryLiveRange(liveRangeAliasTag);

                // Check if the live range being analyzed was allocated in the nested tile.  If it was force it to
                // register here, other wise treat this point as a kill.

                if (summaryLiveRange != nullptr)
                {
                    isUse = true;
                }
                else
                {
                    isKill = true;
                }
            }

            if (GraphColor::Tile::IsExitTileInstruction(instruction))
            {
                GraphColor::Tile ^ nestedTile = tileGraph->GetNestedTile(instruction->BasicBlock);

                Assert(nestedTile != nullptr);

                GraphColor::LiveRange ^ summaryLiveRange = nestedTile->GetSummaryLiveRange(liveRangeAliasTag);

                // Check if the live range being analyzed was allocated in the nested tile.  If it was force it to
                // register here, other wise treat this point as a kill.

                if (summaryLiveRange != nullptr)
                {
                    isDefinition = true;
                }
                else
                {
                    isKill = true;
                }
            }

            // If any block has a kill in addition to a def then block boundary spilling around the
            // kill won't work.  Return false.

            if (isKill && isDefinition)
            {
                return false;
            }

            liveness->UpdateInstruction(instruction, liveBitVector, generateBitVector, killedBitVector);
        }
        next_instr_in_block_backward;

        // Process dangling definitions if any.

        Graphs::BasicBlock ^ uniqueNonEHPredecessorBlock = block->UniqueNonEHPredecessorBlock;

        if ((uniqueNonEHPredecessorBlock != nullptr)
            && uniqueNonEHPredecessorBlock->HasDanglingInstruction)
        {
            IR::Instruction ^ danglingInstruction = uniqueNonEHPredecessorBlock->LastInstruction;

            Assert(danglingInstruction->IsDanglingInstruction);

            // Process dangling instruction kills

            Dataflow::RegisterLivenessWalker ^ registerLivenessWalker = liveness->RegisterLivenessWalker;

            registerLivenessWalker->TransferDestinations(danglingInstruction, generateBitVector, killedBitVector);

            if (aliasInfo->CommonMayPartialTags(liveRangeAliasTag, killedBitVector))
            {

                if (!aliasInfo->DestinationMustTotallyOverlap(danglingInstruction, liveRangeAliasTag))
                {
                    return false;
                }

                isDefinition = true;
                isUse = false;
            }
        }

        // Update block info

        blockInfo.IsDefinition = isDefinition;
        blockInfo.IsKill = isKill;
        blockInfo.IsUse = isUse;
    }

    blockInfoVector->Item[blockId] = blockInfo;

    return true;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ReloadForUse
//
// Arguments:
//
//    block     - Block to process for reload. 
//    doInsert  - Do inserts or analyze.
//
// Notes:
//
//    This function inserts any needed reloads for an upward exposed use that is fully available. 
//    (dominated by a hazard)
//
//    Cost is communicated via the SlotWiseOptimizer->Cost field.
//
// Returns:
//
//   True if reload is legal, false otherwise.
//
//------------------------------------------------------------------------------

bool
SlotWiseOptimizer::ReloadForUse
(
   llvm::MachineBasicBlock * block,
   bool                      doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    GraphColor::Tile ^             tile = this->Tile;
    GraphColor::LiveRange ^        liveRange = this->LiveRange;
    Alias::Info ^                  aliasInfo = allocator->AliasInfo;
    GraphColor::SpillOptimizer ^   spillOptimizer = this->SpillOptimizer;
    Tiled::Id                        blockId = block->Id;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    GraphColor::BlockInfo          blockInfo = blockInfoVector->Item[blockId];
    Alias::Tag                     liveRangeAliasTag = liveRange->AliasTag;

    Assert((blockInfo.IsAvailableIn && (blockInfo.IsUse || blockInfo.IsExit))
        || (blockInfo.IsKill && !blockInfo.IsAvailableOut));

    // This code relies on the first use encountered being the one that needs spilling. By implication
    // that means no def/use after use within the block.

    IR::Instruction ^ insertInstruction = nullptr;
    IR::Instruction ^ startInstruction;
    IR::Instruction ^ lastInstruction = block->LastInstruction;

    if (blockInfo.IsKill && !blockInfo.IsAvailableOut)
    {
        // Find last kill as the instruction to start from.

        startInstruction = block->FindPreviousInstructionInBlock(IR::InstructionKind::CallInstruction,
            lastInstruction);

        if (startInstruction == nullptr)
        {
            // If we didn't find a call instruction make sure that we're dealing with a 'fat' block.

            Assert(tile->IntegerMaxKillBlockBitVector->GetBit(blockId)
                || tile->FloatMaxKillBlockBitVector->GetBit(blockId)
                || tile->IsNestedEntryBlock(block) || tile->IsNestedExitBlock(block));

            if (blockInfo.IsUse)
            {
                return false;
            }
            else
            {
                startInstruction = block->LastInstruction;
            }
        }
    }
    else
    {
        startInstruction = block->FirstInstruction;
    }

    foreach_instr_in_range(instruction, startInstruction, lastInstruction)
    {
        if (GraphColor::Tile::IsTileBoundaryInstruction(instruction)
            || aliasInfo->SourceMayPartiallyOverlap(instruction, liveRangeAliasTag))
        {
            insertInstruction = instruction;
            break;
        }
    }
    next_instr_in_range;

    if (insertInstruction == nullptr)
    {
        insertInstruction = block->LastInstruction;

        if (!insertInstruction->IsBranchInstruction)
        {
            return false;
        }
    }

    this->HasSpillPoints = true;

    // Cost of reload from stack
    Tiled::Cost reloadCost = spillOptimizer->ComputeLoad(tile, liveRange, liveRange->Register, insertInstruction,
        doInsert);
    // Increment global cost.

    Tiled::Cost cost = this->Cost;

    cost.IncrementBy(&reloadCost);

    this->Cost = cost;

    return true;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ReloadForJoin
//
// Arguments:
//
//    block     - Block to process for reload. 
//    doInsert  - Do inserts or analyze.
//
// Notes:
//
//    This function inserts any needed reloads for joins.
//
//    If this block available from a hazard (post dominates a call or fat point) and any successor blocks do not
//    - i.e. a join block that is partial available from a hazard on only one if it's incoming arcs - then a 
//    reload is costed/inserted.
//
//    Cost is communicated via the SlotWiseOptimizer->Cost field.
//
// Returns:
//
//   True if reload is legal, false otherwise.
//
//------------------------------------------------------------------------------

bool
SlotWiseOptimizer::ReloadForJoin
(
   llvm::MachineBasicBlock * block,
   bool                      doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    GraphColor::Tile ^             tile = this->Tile;
    GraphColor::SpillOptimizer ^   spillOptimizer = this->SpillOptimizer;
    GraphColor::LiveRange ^        liveRange = this->LiveRange;
    Tiled::Id                        blockId = block->Id;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    GraphColor::BlockInfo          blockInfo = blockInfoVector->Item[blockId];
    Tiled::Cost                      reloadCost = allocator->ZeroCost;

    Tiled::Boolean isSuccessorAvailable = true;
    Tiled::Boolean isSuccessorPartiallyAvailable = false;
    Tiled::Boolean isSuccessorPartiallyAnticipated = false;
    Tiled::Boolean hasExitBlockSuccessor = false;

    foreach_block_succ_block(successorBlock, block)
    {
        Tiled::Boolean isExitBlock = tile->IsExitBlock(successorBlock);

        hasExitBlockSuccessor |= isExitBlock;

        if (!tile->IsBodyBlockExclusive(successorBlock) && !isExitBlock)
        {
            continue;
        }

        Tiled::Id               successorBlockId = successorBlock->Id;
        GraphColor::BlockInfo successorBlockInfo = blockInfoVector->Item[successorBlockId];

        if (successorBlockInfo.IsLiveIn)
        {
            isSuccessorAvailable &= successorBlockInfo.IsAvailableIn;
            isSuccessorPartiallyAvailable |= successorBlockInfo.IsAvailableIn;
            isSuccessorPartiallyAnticipated |= successorBlockInfo.IsAnticipatedIn;
        }
    }
    next_block_succ_block;

    if (!isSuccessorAvailable && blockInfo.IsUpExposedOut)
    {
        // restore code.

        this->HasSpillPoints = true;

        if (isSuccessorPartiallyAvailable || isSuccessorPartiallyAnticipated)
        {
            // split successor edge to non-available blocks and do restore there.

            // Go through successors and split edges to non-available blocks inserting the reloads there.

            foreach_block_succ_edge(successorEdge, block)
            {
                Graphs::BasicBlock ^ successorBlock = successorEdge->SuccessorNode;

                Tiled::Boolean isExitBlock = tile->IsExitBlock(successorBlock);

                if (!tile->IsBodyBlockExclusive(successorBlock) && !isExitBlock)
                {
                    continue;
                }

                Tiled::Id               successorBlockId = successorBlock->Id;
                GraphColor::BlockInfo successorBlockInfo = blockInfoVector->Item[successorBlockId];

                if (successorBlockInfo.IsUpExposedIn
                    && !successorBlockInfo.IsAvailableIn)
                {
                    if (!successorEdge->IsSplittable || successorEdge->IsCritical || isExitBlock)
                    {
                        // We should never hit this bail out when we're doing inserts
                        Assert(!doInsert);

                        return false;
                    }

                    IR::Instruction ^ insertInstruction = successorBlock->FirstInstruction;

                    if (insertInstruction->IsLabelInstruction)
                    {
                        insertInstruction = insertInstruction->Next;
                    }

                    Tiled::Cost edgeCost = spillOptimizer->ComputeLoad(tile, liveRange, liveRange->Register,
                        insertInstruction, doInsert);

                    reloadCost.IncrementBy(&edgeCost);
                }
            }
            next_block_succ_edge;
        }
        else
        {
            IR::Instruction ^ insertInstruction = block->LastInstruction;

            if (insertInstruction->IsDanglingInstruction)
            {
                return false;
            }

            // Cost of reload from stack

            Tiled::Cost tempCost = spillOptimizer->ComputeLoad(tile, liveRange, liveRange->Register,
                insertInstruction,
                doInsert);

            reloadCost.IncrementBy(&tempCost);
        }
    }

    // Increment global cost.

    Tiled::Cost cost = this->Cost;

    cost.IncrementBy(&reloadCost);

    this->Cost = cost;

    return true;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    StoreForDefinition
//
// Arguments:
//
//    block     - Block to process for store. 
//    doInsert  - Do inserts or analyze.
//
// Notes:
//
//    This function inserts any needed stores for an downward visible that is fully anticipated. 
//    (post dominated by a hazard)
//
//    Cost is communicated via the SlotWiseOptimizer->Cost field.
//
// Returns:
//
//   True if store is legal, false otherwise.
//
//------------------------------------------------------------------------------

bool
SlotWiseOptimizer::StoreForDefinition
(
   llvm::MachineBasicBlock * block,
   bool                      doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    GraphColor::Tile ^             tile = this->Tile;
    Alias::Info ^                  aliasInfo = allocator->AliasInfo;
    GraphColor::SpillOptimizer ^   spillOptimizer = this->SpillOptimizer;
    GraphColor::LiveRange ^        liveRange = this->LiveRange;
    Tiled::Id                        blockId = block->Id;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    GraphColor::BlockInfo          blockInfo = blockInfoVector->Item[blockId];
    Alias::Tag                     liveRangeAliasTag = liveRange->AliasTag;
    Tiled::Cost                      storeCost;
    Tiled::Cost                      infiniteCost = allocator->InfiniteCost;

    // Setup has spill

    this->HasSpillPoints = true;

    // This code relies on the first def encountered being the one that needs spilling by implication
    // that means no use after def within the block.

    Tiled::Boolean      swapWithFirst = false;
    Tiled::Boolean      isDanglingInstruction = false;
    IR::Instruction ^ insertInstruction = nullptr;
    IR::Instruction ^ lastInstruction;
    IR::Instruction ^ startInstruction = block->FirstInstruction;

    if (blockInfo.IsKill && !blockInfo.IsAnticipatedIn)
    {
        lastInstruction = block->FindNextInstructionInBlock(IR::InstructionKind::CallInstruction,
            startInstruction);

        if (lastInstruction == nullptr)
        {
            // If we didn't find a call instruction make sure that we're
            // dealing with a 'fat' block or a tile boundary.

            Assert((allocator->DoInsertAlsaCode && (tile->Iteration == 1))
                || tile->IntegerMaxKillBlockBitVector->GetBit(blockId)
                || tile->FloatMaxKillBlockBitVector->GetBit(blockId)
                || tile->IsNestedEntryBlock(block)
                || tile->IsNestedExitBlock(block));

            lastInstruction = block->LastInstruction;
        }
    }
    else
    {
        lastInstruction = block->LastInstruction;
    }

    foreach_instr_in_range_backward(instruction, lastInstruction, startInstruction)
    {
        if (GraphColor::Tile::IsTileBoundaryInstruction(instruction)
            || aliasInfo->DestinationMayPartiallyOverlap(instruction, liveRangeAliasTag))
        {
            insertInstruction = instruction;

            isDanglingInstruction = insertInstruction->IsDanglingInstruction;

            break;
        }
    }
    next_instr_in_range_backward;

    if (insertInstruction == nullptr)
    {
        insertInstruction = block->FirstInstruction;

        if (insertInstruction->IsBranchInstruction)
        {
            // Insert a temporary instruction to have something to
            // spill "after" - this will be removed at the end of
            // allocation.

            IR::ValueInstruction ^ placeholderInstruction =
                IR::ValueInstruction::New(allocator->FunctionUnit, Common::Opcode::TemporaryPlaceholder);

            insertInstruction->InsertBefore(placeholderInstruction);

            insertInstruction = placeholderInstruction;
        }
        else if (!insertInstruction->IsLabelInstruction)
        {
            swapWithFirst = true;
        }
    }

    // Cost store to the stack.

    storeCost = spillOptimizer->ComputeSave(tile, liveRange, liveRange->Register, insertInstruction,
        doInsert);

    if (isDanglingInstruction && doInsert)
    {
        AssertM(!swapWithFirst, "Incompatible 'isDanglingInstruction' with 'swapWithFirst'");

        IR::Instruction ^ storeInstruction = insertInstruction->Next;

        storeInstruction->Unlink();

        Graphs::BasicBlock ^ successorBlock = block->UniqueNonEHSuccessorBlock;

        Assert(successorBlock != nullptr);

        IR::Instruction ^    firstInstruction = successorBlock->FirstInstruction;

        if (firstInstruction->IsLabelInstruction)
        {
            firstInstruction->InsertAfter(storeInstruction);
        }
        else
        {
            firstInstruction->InsertBefore(storeInstruction);
        }
    }
    else if (swapWithFirst && doInsert)
    {
        IR::Instruction ^ storeInstruction = insertInstruction->Next;

        storeInstruction->Unlink();

        insertInstruction->InsertBefore(storeInstruction);
    }

    // Increment global cost.

    Tiled::Cost cost = this->Cost;

    cost.IncrementBy(&storeCost);

    this->Cost = cost;

    return true;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    StoreForSplit
//
// Arguments:
//
//    block     - Block to process for store. 
//    doInsert  - Do inserts or analyze.
//
// Notes:
//
//    This function inserts any needed stores for splits.
//
//    If this block anticipates a hazard (dominates a call or fat point) and any predecessor block do not
//    - i.e. a split block that anticipates a hazard on only one if it's outgoing arcs - then a store is 
//    costed/inserted.
//
//    Cost is communicated via the SlotWiseOptimizer->Cost field.
//
// Returns:
//
//   True if store is legal, false otherwise.
//
//------------------------------------------------------------------------------

bool
SlotWiseOptimizer::StoreForSplit
(
   llvm::MachineBasicBlock *       block,
   bool                            doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    GraphColor::Tile ^             tile = this->Tile;
    GraphColor::SpillOptimizer ^   spillOptimizer = this->SpillOptimizer;
    GraphColor::LiveRange ^        liveRange = this->LiveRange;
    Tiled::Id                        blockId = block->Id;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    GraphColor::BlockInfo          blockInfo = blockInfoVector->Item[blockId];

    Tiled::Cost    storeCost = allocator->ZeroCost;
    Tiled::Boolean isPredecessorAnticipated = true;
    Tiled::Boolean isPredecessorPartiallyAnticipated = false;
    Tiled::Boolean isPredecessorPartiallyAvailable = false;

    Assert(blockInfo.IsAnticipatedIn);

    // Compute 1 - if all predecessors anticipate a hazard.
    // Compute 2 - if some predecessors anticipate a hazard.
    // Compute 3 - if some predecessors are available from a hazard.

    foreach_block_pred_block(predecessorBlock, block)
    {
        if (!tile->IsBodyBlockExclusive(predecessorBlock) && !tile->IsEntryBlock(predecessorBlock))
        {
            continue;
        }

        Tiled::Id               predecessorBlockId = predecessorBlock->Id;
        GraphColor::BlockInfo predecessorBlockInfo = blockInfoVector->Item[predecessorBlockId];

        if (predecessorBlockInfo.IsLiveOut)
        {
            isPredecessorAnticipated &= predecessorBlockInfo.IsAnticipatedOut;
            isPredecessorPartiallyAnticipated |= predecessorBlockInfo.IsAnticipatedOut;
            isPredecessorPartiallyAvailable |= predecessorBlockInfo.IsAvailableOut;
        }
    }
    next_block_pred_block;

    // If the current block anticipates a hazard and all predecessor are not also antipated and the
    // current block is not available in (no definition to store) we will need to do a store.

    if (!isPredecessorAnticipated && blockInfo.IsDownDefinedIn)
    {
        // store code.

        // If we have a partial case 

        if (isPredecessorPartiallyAnticipated || isPredecessorPartiallyAvailable)
        {
            Tiled::Boolean insertedSpills = false;

            // Go through predecessors and insert saves on edges from non-anticipated blocks (critical
            // edges are split).

            foreach_block_pred_edge(predecessorEdge, block)
            {
                Graphs::BasicBlock ^ predecessorBlock = predecessorEdge->PredecessorNode;

                if (!tile->IsBodyBlockExclusive(predecessorBlock) && !tile->IsEntryBlock(predecessorBlock))
                {
                    continue;
                }

                Tiled::Id               predecessorBlockId = predecessorBlock->Id;
                GraphColor::BlockInfo predecessorBlockInfo = blockInfoVector->Item[predecessorBlockId];

                if (predecessorBlockInfo.IsDownDefinedOut
                    && !predecessorBlockInfo.IsAnticipatedOut)
                {
                    if (!predecessorEdge->IsSplittable || predecessorEdge->IsCritical)
                    {
                        // We should never hit this bail out when we're doing inserts

                        Assert(!doInsert);

                        return false;
                    }

                    IR::Instruction ^ insertInstruction = predecessorBlock->LastInstruction;

                    if (insertInstruction->IsBranchInstruction || insertInstruction->IsDanglingInstruction)
                    {
                        insertInstruction = insertInstruction->Previous;

                        if (insertInstruction->IsDanglingInstruction)
                        {
                            Assert(!doInsert);

                            return false;
                        }
                    }

                    insertedSpills = true;

                    Tiled::Cost edgeCost = spillOptimizer->ComputeSave(tile, liveRange, liveRange->Register,
                        insertInstruction, doInsert);

                    storeCost.IncrementBy(&edgeCost);
                }
            }
            next_block_pred_edge;

            this->HasSpillPoints |= insertedSpills;
        }
        else
        {
            this->HasSpillPoints = true;

            IR::Instruction ^ insertInstruction = block->FirstInstruction;

            if (insertInstruction->IsLabelInstruction
                && (insertInstruction->Opcode == Common::Opcode::EnterUserFilter)
                && EH::EHLower::IsConstantFilter(insertInstruction, 1))
            {
                // If this could be a no encode filter, reject it.
                // Today sections are 

                return false;
            }

            if (insertInstruction->IsBranchInstruction)
            {
                // Insert a temporary instruction to have something to
                // spill "after" - this will be removed at the end of
                // allocation.

                IR::ValueInstruction ^ placeholderInstruction =
                    IR::ValueInstruction::New(allocator->FunctionUnit,
                    Common::Opcode::TemporaryPlaceholder);

                insertInstruction->InsertBefore(placeholderInstruction);

                insertInstruction = placeholderInstruction;
            }

            // Cost store to the stack.

            storeCost = spillOptimizer->ComputeSave(tile, liveRange, liveRange->Register,
                insertInstruction, doInsert);

            if (doInsert && !insertInstruction->IsLabelInstruction)
            {
                // Special case, save code typically goes below the target instruction 
                // (saving a definition) but in this case it goes above (saving before a block/hazzard).

                IR::Instruction ^ storeInstruction = insertInstruction->Next;

                storeInstruction->Unlink();

                insertInstruction->InsertBefore(storeInstruction);
            }
        }
    }

    Tiled::Cost cost = this->Cost;

    cost.IncrementBy(&storeCost);

    this->Cost = cost;

    return true;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeCallerSave
//
// Arguments:
//
//    liveRange - Live range to compute a caller save cost for.
//    tile      - Tile to analyze.
//    doInsert  - Do inserts or analyze.
//
// Returns:
//
//   Cost of doing a caller save split of this live range.
//
//------------------------------------------------------------------------------

Tiled::Cost
SlotWiseOptimizer::ComputeCallerSave
(
   GraphColor::LiveRange * liveRange,
   GraphColor::Tile *      tile,
   bool                    doInsert
)
{
#ifdef FUTURE_IMPL
    GraphColor::Allocator ^        allocator = this->Allocator;
    Tiled::Cost                      infiniteCost = allocator->InfiniteCost;
    Tiled::Cost                      cost = allocator->ZeroCost;
    Collections::BlockInfoVector ^ blockInfoVector = this->BlockInfoVector;
    Alias::Tag                     liveRangeAliasTag = liveRange->AliasTag;
    Tiled::Boolean                   callerSaveEnable = this->DoCallerSaveStrategy;

    if (!callerSaveEnable)
    {
        return infiniteCost;
    }

    Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = allocator->TargetRegisterAllocator;

    if (!targetRegisterAllocator->CanCallerSaveSpill(
        allocator->FunctionUnit, liveRange->Register, liveRange->Type))
    {
        return infiniteCost;
    }

    // Set current live range and tile for analysis.  These must be set before calling any of the private
    // functions implementing the slotwise analysis.

    this->LiveRange = liveRange;
    this->Tile = tile;
    this->Cost = allocator->ZeroCost;

    // Evaluate blocks for the appearance condition
    // 
    // - For callee save an 'appearance' is a call that will kill
    // - For live on EH exit an 'appearance' is a unsafe instruction
    //   with an EH label operand

    // Initialize each tile body block with liveIn, liveOut, use, def, and kill.

    foreach_block_in_tile_by_ebb_forward(block, tile)
    {
        if (!this->ComputeBlockData(block))
        {
            return infiniteCost;
        }

        // Forward walk to propagate down defined and available.

        Tiled::Id               blockId = block->Id;
        GraphColor::BlockInfo blockInfo = blockInfoVector->Item[blockId];

        if (blockInfo.IsAnalyzed)
        {
            Tiled::Boolean isAvailableIn = false;
            Tiled::Boolean isDownDefinedIn = false;
            Tiled::Boolean isFirstPredecessor = true;

            if (blockInfo.IsLiveIn && !blockInfo.IsEntry)
            {
                foreach_block_pred_edge(predecessorEdge, block)
                {
                    Graphs::BasicBlock ^ predecessorBlock = predecessorEdge->PredecessorNode;

                    if (!tile->IsBodyBlockExclusive(predecessorBlock))
                    {
                        continue;
                    }

                    GraphColor::BlockInfo predecessorBlockInfo = blockInfoVector->Item[predecessorBlock->Id];

                    if (predecessorBlockInfo.IsLiveOut)
                    {
                        if (isFirstPredecessor)
                        {
                            isAvailableIn = predecessorBlockInfo.IsAvailableOut;

                            // Processed first predecessor so clear the bit.

                            isFirstPredecessor = false;
                        }
                        else
                        {
                            isAvailableIn &= predecessorBlockInfo.IsAvailableOut;
                        }

                        isDownDefinedIn |= predecessorBlockInfo.IsDownDefinedOut;
                    }
                }
                next_block_pred_edge;
            }

            blockInfo.IsDownDefinedIn = isDownDefinedIn || blockInfo.IsEntry;
            blockInfo.IsDownDefinedOut =
                (!blockInfo.IsKill && (blockInfo.IsDownDefinedIn | blockInfo.IsDefinition));

            blockInfo.IsAvailableIn = blockInfo.IsLiveIn && isAvailableIn;
            blockInfo.IsAvailableOut =
                blockInfo.IsLiveOut && !blockInfo.IsExit && (blockInfo.IsKill || (!blockInfo.IsUse
                && blockInfo.IsAvailableIn));
        }

        blockInfoVector->Item[blockId] = blockInfo;
    }
    next_block_in_tile_by_ebb_forward;

    if (this->CallCount == 0)
    {
        // Special case of a block local call crossing live ranges.  (we couldn't detect the call) Leave for EBB
        // spilling.

        return infiniteCost;
    }

    // Backwards walk to propagate up exposed and anticipated. 

    foreach_block_in_tile_by_ebb_backward(block, tile)
    {
        Tiled::Id               blockId = block->Id;
        GraphColor::BlockInfo blockInfo = blockInfoVector->Item[blockId];

        if (blockInfo.IsAnalyzed)
        {
            Tiled::Boolean isAnticipatedOut = false;
            Tiled::Boolean isUpExposedOut = false;
            Tiled::Boolean isFirstSuccessor = true;

            if (blockInfo.IsLiveOut && !blockInfo.IsExit)
            {
                foreach_block_succ_edge(successorEdge, block)
                {
                    Graphs::BasicBlock ^ successorBlock = successorEdge->SuccessorNode;

                    // Edge to unwind or EH outside the tile (only allowed when there is no liveness)

                    if (!tile->IsBodyBlockExclusive(successorBlock))
                    {
                        continue;
                    }

                    GraphColor::BlockInfo successorBlockInfo = blockInfoVector->Item[successorBlock->Id];

                    if (successorBlockInfo.IsLiveIn)
                    {
                        if (isFirstSuccessor)
                        {
                            isAnticipatedOut = successorBlockInfo.IsAnticipatedIn;

                            // Processed first successor so clear the bit

                            isFirstSuccessor = false;
                        }
                        else
                        {
                            isAnticipatedOut &= successorBlockInfo.IsAnticipatedIn;
                        }

                        isUpExposedOut |= successorBlockInfo.IsUpExposedIn;
                    }
                }
                next_block_succ_edge;
            }

            blockInfo.IsUpExposedOut = isUpExposedOut || blockInfo.IsExit;
            blockInfo.IsUpExposedIn = (!blockInfo.IsKill && blockInfo.IsUpExposedOut) | blockInfo.IsUse;

            blockInfo.IsAnticipatedOut = blockInfo.IsLiveOut && isAnticipatedOut;
            blockInfo.IsAnticipatedIn =
                blockInfo.IsLiveIn && !blockInfo.IsEntry
                && (blockInfo.IsKill || (!blockInfo.IsDefinition && blockInfo.IsAnticipatedOut));
        }

        blockInfoVector->Item[blockId] = blockInfo;
    }
    next_block_in_tile_by_ebb_backward;

    // Initialize spill point tracking for slot wise pass.

    this->HasSpillPoints = false;

    GraphColor::AvailableExpressions ^ globalAvailableExpressions = allocator->GlobalAvailableExpressions;
    IR::Operand ^                      availableOperand
        = globalAvailableExpressions->GetAvailable(liveRangeAliasTag);

    this->MustRecalculate = (availableOperand != nullptr);

    foreach_block_in_tile_by_ebb_forward(block, tile)
    {
        Tiled::Id               blockId = block->Id;
        GraphColor::BlockInfo blockInfo = blockInfoVector->Item[blockId];

        if (!this->MustRecalculate)
        {
            // Case: Definition directly dominating a hazard, insert save at
            // the end of the block.

            if ((blockInfo.IsAnticipatedOut && (blockInfo.IsDefinition || blockInfo.IsEntry))
                || (blockInfo.IsKill && !blockInfo.IsAnticipatedIn))
            {
                IR::Instruction ^ startInstruction = block->FirstInstruction;

                if (startInstruction->IsLabelInstruction
                    && (startInstruction->Opcode == Common::Opcode::EnterUserFilter))
                {
                    return infiniteCost;
                }

                this->StoreForDefinition(block, doInsert);
            }

            // Case: Block is anticipated in if any of it's predecessors are not anticipated out then a save is
            // needed here either at the beginning of the block or on edges leading to this block based on whether
            // they are anticipated and !available.  (def needing to be saved)

            else if (blockInfo.IsAnticipatedIn)
            {
                if (!this->StoreForSplit(block, doInsert))
                {
                    return infiniteCost;
                }
            }
        }

        //  Now the load side.

        if ((blockInfo.IsAvailableIn && (blockInfo.IsUse || blockInfo.IsExit))
            || (blockInfo.IsKill && !blockInfo.IsAvailableOut))
        {
            if (!this->ReloadForUse(block, doInsert))
            {
                return infiniteCost;
            }
        }
        else if (blockInfo.IsAvailableOut)
        {
            if (!this->ReloadForJoin(block, doInsert))
            {
                return infiniteCost;
            }
        }
    }
    next_block_in_tile_by_ebb_forward;

    return (this->HasSpillPoints) ? this->Cost : infiniteCost;
#endif // FUTURE_IMPL
    llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize
//
// Arguments:
//
//    allocator - Allocator context.
//
//------------------------------------------------------------------------------

void
SlotWiseOptimizer::Initialize
(
   GraphColor::Allocator * allocator
)
{
#ifdef FUTURE_IMPL
    // final node count will be defined - do init of block info vector;

    Graphs::FlowGraph ^            flowGraph = allocator->FlowGraph;
    Tiled::UInt                      nodeCount = flowGraph->NodeCount;
    Collections::BlockInfoVector ^ blockInfoVector
        = Collections::BlockInfoVector::New((nodeCount + 1));

    this->BlockInfoVector = blockInfoVector;
#endif // FUTURE_IMPL
}

} // GraphColor
} // RegisterAllocator
} // Tiled
