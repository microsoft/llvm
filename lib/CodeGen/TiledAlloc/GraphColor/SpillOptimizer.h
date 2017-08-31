//===-- GraphColor/SpillOptimizer.h -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_SPILLOPTIMIZER_H
#define TILED_GRAPHCOLOR_SPILLOPTIMIZER_H

#include "AvailableExpressions.h"
#include <unordered_map>

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

class LiveRange;
class SpillOptimizer;
class Tile;
class SpillExtensionObject;

typedef std::map<std::pair<llvm::MachineInstr*,unsigned>, GraphColor::SpillExtensionObject*> OperandToSpillExtensionObjectMap;

enum class SpillKinds
{
   None          = 0,
   Fold          = 0x1,
   Recalculate   = 0x2,
   Reload        = 0x4,
   FoldedReload  = 0x8,
   Spill         = 0x10,
   FoldedSpill   = 0x20,
   Transfer      = 0x40,
   CalleeSave    = 0x80,
   CallerSave    = 0x100,
   NoReloadShare = 0x200,
   NoSpillShare  = 0x400
};

class SpillRecord
{

public:

   /*override*/ void Delete();

   static GraphColor::SpillRecord *
   New
   (
      GraphColor::SpillOptimizer * spillOptimizer,
      llvm::MachineOperand *       recalculateOperand,
      llvm::MachineOperand *       reloadOperand,
      llvm::MachineOperand *       spillOperand,
      llvm::MachineOperand *       valueOperand
   );

   static GraphColor::SpillRecord *
   New
   (
      GraphColor::SpillOptimizer * spillOptimizer
   );

public:

   void ClearFoldedSpill();

   void ClearReload();

   void ClearSpill();

   void SetFold();

   void SetFoldedReload();

   void SetFoldedSpill();

   void SetRecalculate();

   void SetReload();

   void SetSpill();

public:

   int                                AliasTag;
   llvm::MachineBasicBlock *          BasicBlock;
   llvm::MachineOperand *             FoldedReloadOperand;  //was IR::MemoryOperand*

   bool HasFoldedReload()
   {
      return (this->FoldedReloadOperand != nullptr);
   }
   
   bool                               HasPending;
   
   bool HasPendingInstruction()
   {
      return (this->PendingInstruction != nullptr);
   }
   
   bool HasRecalculate()
   {
      return (this->RecalculateOperand != nullptr);
   }
   
   bool HasReload()
   {
      return (this->ReloadOperand != nullptr);
   }
   
   bool HasSpill()
   {
      return (this->SpillOperand != nullptr);
   }
   
   bool HasValue()
   {
      return (this->ValueOperand != nullptr);
   }
   
   bool IsFold()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::Fold))
              != unsigned(SpillKinds::None));
   }
   
   bool IsFoldedReload()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::FoldedReload))
              != unsigned(SpillKinds::None));
   }
   
   bool IsFoldedSpill()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::FoldedSpill))
              != unsigned(SpillKinds::None));
   }
   
   bool IsNoReloadShare()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::NoReloadShare))
              != unsigned(SpillKinds::None));
   }
   
   bool IsNoSpillShare()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::NoSpillShare))
              != unsigned(SpillKinds::None));
   }
   
   bool IsRecalculate()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::Recalculate))
              != unsigned(SpillKinds::None));
   }
   
   bool IsReload()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::Reload))
              != unsigned(SpillKinds::None));
   }
   
   bool IsSpill()
   {
      return ((unsigned(this->SpillKinds) & unsigned(SpillKinds::Spill))
              != unsigned(SpillKinds::None));
   }

   bool
   IsMemoryOperand
   (
      llvm::MachineOperand * operand
   );

   bool                       IsOriginalDefinition;
   llvm::MachineInstr *       PendingInstruction;
   unsigned                   RecalculateInstructionCount;
   llvm::MachineOperand *     RecalculateOperand;
   unsigned                   ReloadInstructionCount;
   llvm::MachineOperand *     ReloadOperand;
   GraphColor::SpillKinds     SpillKinds;
   llvm::MachineOperand *     SpillOperand;  //was IR::MemoryOperand
   llvm::MachineOperand *     ValueOperand;
   GraphColor::SpillRecord *  Next;
   GraphColor::SpillRecord *  nextIterationHashItem; // The next hashItem for iteration purposes
   GraphColor::SpillRecord *  previousIterationHashItem; // The previous hashItem for iteration purposes
};

/*
comment SpillRecord::SpillOperand
{
   // Memory operand backing store for a live range
}

comment SpillRecord::ReloadOperand
{
   // Current reload for a given live range - if this is non-null it can be reused during spilling
}

comment SpillRecord::RecalculateOperand
{
   // Current recalculate for a given liveRange - if this is non-null it can be reused during spilling
}

comment SpillRecord::ValueOperand
{
   // Tile or global constant value for a live range
}

comment SpillRecord::IsFoldedReload
{
   // Reload is folded into the appearance instruction and will require hoisting to be reused.
   //
   // Note: IsFoldedReload is mutually exclusive with IsPendingSpill.  Setting one will clear the other.
   // (A folded reload becomes last operative def and supersedes a pending spill and vice versa)
}
*/

typedef std::unordered_map<int, GraphColor::SpillRecord* > AliasTagToSpillRecordMap;

class AliasTagToSpillRecordIterableMap : public AliasTagToSpillRecordMap
{
   GraphColor::SpillRecord *  firstIterationHashItem;

public:

   static GraphColor::AliasTagToSpillRecordIterableMap *
   New
   (
      unsigned size
   )
   {
      GraphColor::AliasTagToSpillRecordIterableMap * map = new AliasTagToSpillRecordIterableMap();
      return map;
   }

   void
   Remove
   (
      unsigned  key,
      bool      firstTime = true
   )
   {
      AliasTagToSpillRecordMap::iterator i = this->find(key);
      if (i == this->end()) {
         return;
      }

      if (firstTime) {
         GraphColor::SpillRecord * removedRecord = i->second;
         RemoveItem(removedRecord);
      }

      this->erase(i);
   }

   void
   Remove
   (
      unsigned                  key,
      GraphColor::SpillRecord * spillRecord
   )
   {
      Remove(key);
   }

   bool
   Insert
   (
      unsigned                  key,
      GraphColor::SpillRecord * record
   )
   {
      bool result = false;

      AliasTagToSpillRecordMap::iterator i = this->find(key);
      if (i != this->end()) {
         assert(i->second == record);
         if (i->second != record) {
            i->second = record;
         }
         return false;
      }

      if (record->nextIterationHashItem == nullptr && record->previousIterationHashItem == nullptr) {
         this->AddItem(record);
      }

      AliasTagToSpillRecordMap::value_type entry(key, record);
      this->insert(entry);

      return true;
   }

   GraphColor::SpillRecord *
   Lookup
   (
      unsigned key
   )
   {
         AliasTagToSpillRecordMap::iterator i = this->find(key);
         if (i == this->end()) {
            return nullptr;
         }

         return i->second;
   }

   void RemoveAll()
   {
      this->clear();

      GraphColor::SpillRecord *  spillRecord = this->firstIterationHashItem;
      while (spillRecord)
      {
         GraphColor::SpillRecord *  nextRecord = spillRecord->nextIterationHashItem;
         delete spillRecord;
         spillRecord = nextRecord;
      }

      this->firstIterationHashItem = nullptr;
   }

   GraphColor::SpillRecord *
   GetFirst() { return firstIterationHashItem; }

private:

   AliasTagToSpillRecordIterableMap()
      : GraphColor::AliasTagToSpillRecordMap()
   {
         this->firstIterationHashItem = nullptr;
   }

   void
   RemoveItem
   (
      GraphColor::SpillRecord * removedRecord
   )
   {
      GraphColor::SpillRecord * nextRecord = removedRecord->nextIterationHashItem;
      GraphColor::SpillRecord * previousRecord = removedRecord->previousIterationHashItem;

      if (nextRecord != nullptr) {
         nextRecord->previousIterationHashItem = previousRecord;
      }
      if (previousRecord != nullptr) {
         previousRecord->nextIterationHashItem = nextRecord;
      }

      if (removedRecord == this->firstIterationHashItem) {
         this->firstIterationHashItem = nextRecord;
         if (this->firstIterationHashItem != nullptr) {
            this->firstIterationHashItem->previousIterationHashItem = nullptr;
         }
      }
   }

   void
   AddItem
   (
      GraphColor::SpillRecord * record
   )
   {
      record->nextIterationHashItem = nullptr;
      record->previousIterationHashItem = nullptr;

      if (this->firstIterationHashItem != nullptr && this->firstIterationHashItem != record) {
         this->firstIterationHashItem->previousIterationHashItem = record;
         record->nextIterationHashItem = this->firstIterationHashItem;
      }

      this->firstIterationHashItem = record;
   }
};

class SlotWiseOptimizer
{

public:

   static GraphColor::SlotWiseOptimizer *
   New
   (
      GraphColor::SpillOptimizer * spillOptimizer
   );

public:

   Tiled::Cost
   ComputeCallerSave
   (
      GraphColor::LiveRange * liveRange,
      GraphColor::Tile *      tile,
      bool                    doInsert
   );

   void
   Initialize
   (
      GraphColor::Allocator * allocator
   );

public:
   
   GraphColor::Allocator *      Allocator;
   Tiled::Cost                  Cost;

   /*
   property Tiled::Boolean                 DoCallerSaveStrategy { "(this->SpillOptimizer->DoCallerSaveStrategy)" }
   property Tiled::Boolean                 HasSpillPoints { hasSpillPoints }
   property GraphColor::LiveRange ^        LiveRange { liveRange }
   property Tiled::Boolean                 MustRecalculate { mustRecalculate }
   */

   GraphColor::SpillOptimizer * SpillOptimizer;
   GraphColor::Tile *           Tile;

private:

   bool
   ComputeBlockData
   (
      llvm::MachineBasicBlock * block
   );

   bool
   ReloadForJoin
   (
      llvm::MachineBasicBlock * block,
      bool                      doInsert
   );

   bool
   ReloadForUse
   (
      llvm::MachineBasicBlock * block,
      bool                      doInsert
   );

   bool
   StoreForDefinition
   (
      llvm::MachineBasicBlock * block,
      bool                      doInsert
   );

   bool
   StoreForSplit
   (
      llvm::MachineBasicBlock * block,
      bool                      doInsert
   );

private:

   //GraphColor::BlockInfoVector *       BlockInfoVector;
   //unsigned                            CallCount;
   llvm::SparseBitVector<> *           GenerateBitVector;
   llvm::SparseBitVector<> *           KilledBitVector;
   llvm::SparseBitVector<> *           LiveBitVector;
};


class SpillOptimizer
{

public:

   static GraphColor::SpillOptimizer *
   New
   (
      GraphColor::Allocator * allocator
   );

public:

   void
   BeginBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      bool                      doInsert
   );

   void
   CalculateBlockArea
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   );

   void
   ClearMap
   (
      GraphColor::SpillRecord * spillRecord
   );

   static void
   ResetIrExtensions()
   {
      //temporary maps used instead of direct (InstructionId) field on each instruction
      instruction2SpillKind.clear();
      instruction2InstrCount.clear();
   }

   //TODO: this function is needed only because SpillKinds are not stored as an actual instruction field.
   static void
   ResetInstrSpillMarkings(llvm::MachineInstr *  instruction)
   {
      instruction2SpillKind.erase(instruction);
   }

   Tiled::Cost
   ComputeCallerSave
   (
      GraphColor::LiveRange * liveRange,
      GraphColor::Tile *      tile
   )
   {
      return this->ComputeCallerSave(liveRange, tile, false);
   }

   Tiled::Cost
   ComputeCallerSave
   (
      GraphColor::LiveRange * liveRange,
      GraphColor::Tile *      tile,
      bool                    doInsert
   )
   {
      return this->SlotWiseOptimizer->ComputeCallerSave(liveRange, tile, doInsert);
   }

   Tiled::Cost
   ComputeDefinition
   (
      llvm::MachineOperand * definitionOperand
   );

   Tiled::Cost
   ComputeEnterLoad
   (
      GraphColor::Tile *    memoryTile,
      unsigned              boundryTag,
      unsigned              reg,
      llvm::MachineInstr *  insertInstruction,
      bool                  doInsert
   );

   Tiled::Cost
   ComputeExitSave
   (
      GraphColor::Tile *    memoryTile,
      unsigned              boundryTag,
      unsigned              reg,
      llvm::MachineInstr *  insertInstruction,
      bool                  doInsert
   );

#ifdef ARCH_WITH_FOLDS
   Tiled::Cost
   ComputeFold
   (
      llvm::MachineOperand * operand,
      GraphColor::Tile *     tile
   )
   {
      return this->ComputeFold(operand, tile, false);
   }

   Tiled::Cost
   ComputeFold
   (
      llvm::MachineOperand * operand,
      GraphColor::Tile *     tile,
      bool                   doInsert
   );

   Tiled::Cost
   ComputeFoldedReload
   (
      llvm::MachineOperand *  operand,
      llvm::MachineOperand *  spillOperand,  //IR::MemoryOperand*
      llvm::MachineInstr *    insertInstruction,
      bool                    doInsert
   );

   Tiled::Cost
   ComputeFoldedSpill
   (
      unsigned                spillTag,
      llvm::MachineOperand *  operand,
      llvm::MachineOperand *  spillOperand,  //IR::MemoryOperand*
      llvm::MachineInstr *    insertInstruction,
      bool                    canMakePending,
      bool                    doInsert
   );
#endif // ARCH_WITH_FOLDS

   Tiled::Cost
   ComputeLoad
   (
      GraphColor::Tile *      memoryTile,
      GraphColor::LiveRange * liveRange,
      unsigned                reg,
      llvm::MachineInstr *    insertInstruction,
      bool                    doInsert
   );

   Tiled::Cost
   ComputePendingSpill
   (
      llvm::MachineOperand * /*IR::VariableOperand* */ operand,
      unsigned                                         spillAliasTag,
      llvm::MachineOperand * /*IR::MemoryOperand* */   spillOperand,
      llvm::MachineInstr *                             insertInstruction,
      bool                                             doInsert
   );

   Tiled::Cost
   ComputeRecalculate
   (
      llvm::MachineOperand** registerOperand,
      llvm::MachineOperand * replaceOperand,
      GraphColor::Tile *     tile
   )
   {
      return this->ComputeRecalculate(registerOperand, replaceOperand, tile, false);
   }

   Tiled::Cost
   ComputeRecalculate
   (
      llvm::MachineOperand**  registerOperand,
      llvm::MachineOperand *  replaceOperand,
      GraphColor::Tile *      tile,
      const bool              doInsert
   );

   Tiled::Cost
   ComputeReload
   (
      llvm::MachineOperand**  registerOperand,
      llvm::MachineOperand *  sourceOperand,
      GraphColor::Tile *      tile
   )
   {
      return this->ComputeReload(registerOperand, sourceOperand, tile, false);
   }

   Tiled::Cost
   ComputeReload
   (
      llvm::MachineOperand**  registerOperand,
      llvm::MachineOperand *  sourceOperand,
      GraphColor::Tile *      tile,
      bool                    doInsert
   );

   Tiled::Cost
   ComputeSave
   (
      GraphColor::Tile *      memoryTile,
      GraphColor::LiveRange * liveRange,
      unsigned                reg,
      llvm::MachineInstr *    insertInstruction,
      bool                    doInsert
   );

   Tiled::Cost
   ComputeSpill
   (
      llvm::MachineOperand ** /*IR::VariableOperand& */    registerOperand,
      llvm::MachineOperand *  /*IR::VariableOperand* */    destinationOperand,
      GraphColor::Tile *                                   tile
   )
   {
      return this->ComputeSpill(registerOperand, destinationOperand, tile, false);
   }

   Tiled::Cost
   ComputeSpill
   (
      llvm::MachineOperand ** /*IR::VariableOperand& */    registerOperand,
      llvm::MachineOperand *  /*IR::VariableOperand* */    destinationOperand,
      GraphColor::Tile *                                   tile,
      bool                                                 doInsert
   );

   Tiled::Cost
   ComputeTransfer
   (
      GraphColor::Tile *        otherTile,
      unsigned                  boundryTag,
      llvm::MachineBasicBlock * block
   )
   {
      return this->ComputeTransfer(otherTile, boundryTag, block, false);
   }

   Tiled::Cost
   ComputeTransfer
   (
      GraphColor::Tile *           otherTile,
      unsigned                     boundaryTag,
      llvm::MachineBasicBlock *    block,
      bool                         doInsert
   );

   Tiled::Cost
   ComputeWeight
   (
      llvm::MachineOperand * operand
   );

   void
   Cost
   (
      llvm::MachineOperand * operand,
      llvm::MachineInstr *   instruction,
      GraphColor::Tile *     tile
   );

   void
   CostCallerSave
   (
      GraphColor::LiveRange * liveRange,
      GraphColor::Tile *      tile
   );

   static void
   DeleteInstruction
   (
      llvm::MachineInstr *    instruction,
      llvm::MachineFunction * MF
   );

   bool
   DoSpillSharing
   (
      llvm::MachineOperand * operand
   );

   void
   EndBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      bool                      doInsert
   );

   llvm::MachineOperand *  //IR::MemoryOperand*
   FindSpillMemory
   (
      llvm::MachineInstr * spillInstruction
   );

   llvm::MachineOperand *  //IR::MemoryOperand*
   FindReloadMemory
   (
      llvm::MachineInstr * reloadInstruction
   );

   bool
   IsMemoryOperand
   (
      llvm::MachineOperand * operand
   );

   void
   Finish
   (
      llvm::MachineInstr *  instruction,
      GraphColor::Tile *    tile
   )
   {
      this->Finish(instruction, tile, false);
   }

   void
   Finish
   (
      llvm::MachineInstr *  instruction,
      GraphColor::Tile *    tile,
      bool                  doInsert
   );

   llvm::MachineOperand *
   GetAvailable
   (
      llvm::MachineOperand * appearanceOperand
   )
   {
      return this->AvailableExpressions->GetAvailable(appearanceOperand);
   }

   llvm::MachineOperand *
   GetAvailable
   (
      unsigned aliasTag
   )
   {
      return this->AvailableExpressions->GetAvailable(aliasTag);
   }

   llvm::MachineOperand *
   GetDefinition
   (
      unsigned             aliasTag,
      llvm::MachineInstr * instruction
   );

   GraphColor::SpillRecord *
   GetDominating
   (
      unsigned             aliasTag,
      llvm::MachineInstr * instruction
   );

   GraphColor::SpillRecord *
   GetDominating
   (
      unsigned               aliasTag,
      llvm::MachineInstr *   instruction,
      GraphColor::SpillKinds spillKinds
   );

   llvm::MachineOperand *
   GetLastDefinitionTemporary
   (
      llvm::MachineOperand * appearanceOperand
   );

   GraphColor::LiveRange *
   GetLiveRange
   (
      unsigned aliasTag
   );

   GraphColor::SpillRecord *
   GetPending
   (
      GraphColor::SpillRecord * spillRecord,
      GraphColor::SpillKinds    spillKinds
   );

   GraphColor::LiveRange *
   GetPendingLiveRange
   (
      unsigned aliasTag
   );

   llvm::MachineOperand *
   GetPendingSpill
   (
      unsigned                 spillTag,
      llvm::MachineOperand **  spillOperand,  //was:  IR::MemoryOperand
      llvm::MachineInstr *     instruction
   );

   llvm::MachineOperand *
   GetRecalculate
   (
      llvm::MachineOperand * operand
   );

   llvm::MachineOperand *
   GetRegisterOperand
   (
      unsigned  aliasTag,
      unsigned  reg
   );

   llvm::MachineOperand *
   GetReload
   (
      llvm::MachineOperand * operand,
      Tiled::Cost&             cost,
      bool                   doInsert
   );

   static unsigned
   GetSpillAliasTag
   (
      llvm::MachineOperand * operand
   );

   llvm::MachineOperand *
   GetSpillAppearance
   (
      unsigned                                                spillTag,
      llvm::iterator_range<llvm::MachineInstr::mop_iterator>  operandList
   );

   static GraphColor::SpillKinds
   GetSpillKinds
   (
      llvm::MachineInstr * instruction
   );

   llvm::MachineOperand *  //was: IR::MemoryOperand *
   GetSpillOperand
   (
      llvm::MachineOperand *     operand,
      GraphColor::LiveRange *    liveRange,
      GraphColor::Tile *         tile,
      bool&                      isValidInMemory
   );

   GraphColor::SpillRecord *
   GetSpillRecord
   (
      unsigned                  appearanceTag,
      llvm::MachineBasicBlock * basicBlock
   );

   GraphColor::SpillRecord *
   GetSpillRecord
   (
      llvm::MachineOperand *        appearanceOperand,
      llvm::MachineBasicBlock *     basicBlock
   )
   {
      assert(appearanceOperand->isReg());

      unsigned appearanceOperandTag = this->VrInfo->GetTag(appearanceOperand->getReg());

      return this->GetSpillRecord(appearanceOperandTag, basicBlock);
   }

   unsigned
   GetSpillRegister
   (
      llvm::MachineOperand *  registerOperand,
      llvm::MachineOperand * /*IR::MemoryOperand* */ spillOperand
   );

   llvm::MachineOperand *
   GetSpillTemporary
   (
      llvm::MachineOperand * replaceOperand,
      llvm::MachineOperand * spillOperand,  /*IR::MemoryOperand* */
      bool                   doInsert
   );

   unsigned
   GetInstrCount
   (
      llvm::MachineInstr * instruction
   );

   bool
   HasSpill
   (
      llvm::MachineOperand * operand
   )
   {
      assert(operand->isReg());
      unsigned tag = this->VrInfo->GetTag(operand->getReg());

      return this->IsTracked(tag);
   }

   void
   Initialize
   (
      GraphColor::Allocator * allocator
   );

   void
   Initialize
   (
      GraphColor::Tile * tile
   );

   void
   InsertCommittedSpill
   (
      llvm::MachineOperand *                          definitionOperand,
      unsigned                                        appearanceTag,
      llvm::MachineOperand * /*IR::MemoryOperand* */  spillOperand,
      llvm::MachineInstr *                            pendingInstruction
   );

   void
   InsertDefinition
   (
      llvm::MachineOperand *     newInstanceOperand,
      llvm::MachineOperand *     appearanceOperand,
      llvm::MachineInstr *       pendingInstruction
   )
   {
      this->InsertReloadInternal(newInstanceOperand, appearanceOperand, pendingInstruction, true);
   }

   void
   InsertFoldedReload
   (
      llvm::MachineOperand *  newInstanceOperand,
      llvm::MachineOperand *  appearanceOperand,
      llvm::MachineOperand *  spillOperand,  /*IR::MemoryOperand* */
      llvm::MachineInstr *    pendingInstruction
   );

   void
   InsertFoldedSpill
   (
      llvm::MachineOperand *       newInstanceOperand,
      llvm::MachineOperand *       appearanceOperand,
      llvm::MachineOperand * /*IR::MemoryOperand* */ spillOperand,
      llvm::MachineInstr *         pendingInstruction
   );

   void
   InsertPendingSpill
   (
      llvm::MachineOperand *                          definitionOperand,
      unsigned                                        appearanceTag,
      llvm::MachineOperand * /*IR::MemoryOperand* */  spillOperand,
      llvm::MachineInstr *                            pendingInstruction
   );

   void
   InsertRecalculate
   (
      llvm::MachineOperand *  newInstanceOperand,
      llvm::MachineOperand *  appearanceOperand,
      llvm::MachineInstr *    pendingInstruction
   );

   void
   InsertReload
   (
      llvm::MachineOperand *  newInstanceOperand,
      llvm::MachineOperand *  appearanceOperand,
      llvm::MachineInstr *    pendingInstruction
   )
   {
      this->InsertReloadInternal(newInstanceOperand, appearanceOperand, pendingInstruction, false);
   }

   GraphColor::SpillRecord *
   InsertSpillInternal
   (
      llvm::MachineOperand *                          definitionOperand,
      unsigned                                        appearanceTag,
      llvm::MachineOperand * /*IR::MemoryOperand* */  spillOperand,
      llvm::MachineInstr *                            pendingInstruction
   );

   static bool
   IsCalleeSaveTransfer
   (
      llvm::MachineInstr * instruction
   );

   bool
   IsLegalToFoldOperand
   (
      llvm::MachineOperand * definitionOperand,
      llvm::MachineOperand * useOperand
   );

   bool
   IsLegalToReplaceOperand
   (
      llvm::MachineOperand * originalOperand,
      llvm::MachineOperand * replaceOperand
   );

   bool
   IsReloaded
   (
      unsigned             aliasTag,
      llvm::MachineInstr * instruction
   );

   static bool
   IsSpillKind
   (
      llvm::MachineInstr *   instruction,
      GraphColor::SpillKinds spillKinds
   );

   bool
   IsTracked
   (
      unsigned aliasTag
   );

   void
   Map
   (
      GraphColor::SpillRecord * spillRecord
   );

   GraphColor::SpillRecord *
   Pop
   (
      GraphColor::SpillRecord * spillRecord
   );

   void
   ProcessPendingSpill
   (
      unsigned             spillAliasTag,
      llvm::MachineInstr * instruction,
      bool                 doInsert
   );

   void
   ProcessPendingSpills
   (
      llvm::MachineInstr * instruction,
      bool                 doInsert
   );

   void
   ProcessPendingSpills
   (
      llvm::MachineInstr *      lastInstruction,
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      bool                      doInsert
   );

   void RemoveAll();

   static void
   RemoveInstruction
   (
      llvm::MachineInstr * instruction
   );

   llvm::MachineOperand *
   ReplaceSpillAppearance
   (
      llvm::MachineOperand *  temporaryOperand,
      llvm::MachineOperand ** replaceOperand,
      bool                    doInsert
   );

   void Reset();

   static void
   SetCalleeSaveTransfer
   (
      llvm::MachineInstr * instruction
   );

   Tiled::Cost
   Spill
   (
      llvm::MachineOperand * operand,
      llvm::MachineInstr *   instruction,
      GraphColor::Tile *     tile
   );

   Tiled::Cost
   SpillDestination
   (
      llvm::MachineOperand * destinationOperand,
      llvm::MachineInstr *   instruction,
      unsigned               spillTag,
      GraphColor::Tile *     tile,
      GraphColor::Decision  decision
   );

   void
   SpillFromSummary
   (
      unsigned                globalAliasTag,
      GraphColor::LiveRange * summaryLiveRange
   );

   Tiled::Cost
   SpillSource
   (
      llvm::MachineOperand * sourceOperand,
      llvm::MachineInstr *   instruction,
      unsigned               aliasTag,
      GraphColor::Tile *     tile,
      GraphColor::Decision   decision
   );

   void
   TileBoundary
   (
      llvm::MachineInstr * instruction
   )
   {
      this->TileBoundary(instruction, nullptr, false);
   }

   void
   TileBoundary
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * spillAliasTagBitVector,
      bool                      doInsert
   );

   bool
   UpdatePendingSpills
   (
      unsigned                spillTag,
      llvm::MachineOperand *  inheritedSpillOperand,
      llvm::MachineInstr *    instruction
   );

   llvm::MachineOperand *
   CopyOperand
   (
      const llvm::MachineOperand * operand
   );

   llvm::MachineOperand *
   replaceAndUnlinkOperand
   (
      llvm::MachineInstr *    instruction,
      llvm::MachineOperand ** oldOperand,
      llvm::MachineOperand *  newOperand
   );

   int
   getOperandIndex
   (
      llvm::MachineInstr *   instruction,
      llvm::MachineOperand * operand
   );

   llvm::MachineOperand *
   getSingleExplicitSourceOperand
   (
      llvm::MachineInstr * instruction
   );

   llvm::MachineOperand *
   getSingleExplicitDestinationOperand
   (
      llvm::MachineInstr * instruction
   );

public:

   Tiled::VR::Info *       VrInfo;
   GraphColor::Allocator * Allocator;
   unsigned                CallerSaveIterationLimit;
   unsigned                ConservativeControlThreshold;
   unsigned                ConstrainedInstructionShareStartIteration;
   unsigned                CrossCallInstructionShareStopIteration;
   bool                    DoCalculateBlockArea;
   bool                    DoCallerSaveStrategy;
   bool                    DoShareAcrossCalls;
   bool                    DoShareByEdgeProbability;
   bool                    EBBShare;
   unsigned                EdgeProbabilityInstructionShareStopIteration;
   unsigned                InitialInstructionShareLimit;
   unsigned                InstructionCount;
   double                  InstructionShareDecayRateValue;
   unsigned                InstructionShareLimit;
   unsigned                inFunctionSpilledLRs;

   static OperandToSpillExtensionObjectMap OperandToSpillExtension;

   bool
   IsGlobalScope
   (
   )
   {
      return (this->Tile == nullptr);
   }
   
   llvm::SparseBitVector<> * RegionTrackedBitVector;
   bool                      ShareSpills;
   unsigned                  SpillShareIterationLimit;

   llvm::SparseBitVector<> * SpilledTagBitVector;
   GraphColor::Tile *        Tile;

private:

   static void
   AddSpillKinds
   (
      llvm::MachineInstr *   instruction,
      GraphColor::SpillKinds spillKinds
   );

   bool
   CanShareSpill
   (
      llvm::MachineOperand *                           destinationOperand,
      llvm::MachineOperand * /*IR::MemoryOperand * */  spillOperand,
      llvm::MachineInstr *                             instruction
   );

   void
   ClearPendingSpill
   (
      unsigned             aliasTag,
      llvm::MachineInstr * instruction
   );

   void
   ClearReload
   (
      unsigned             aliasTag,
      llvm::MachineInstr * instruction
   );

   llvm::MachineInstr *
   GetCopyTransferInsertionPoint
   (
      llvm::MachineOperand *        destinationOperand,
      llvm::MachineOperand *        sourceOperand,
      llvm::MachineBasicBlock *     block
   );

   llvm::MachineInstr *
   GetCopyTransferInsertionPoint
   (
      llvm::MachineInstr * topBlockingInstruction,
      llvm::MachineInstr * bottomBlockingInstruction
   );

   unsigned
   GetCopyTransferScratchRegister
   (
      unsigned                   targetRegister,
      llvm::MachineBasicBlock *  compensationBlock,
      GraphColor::Tile *         otherTile
   );

   bool
   InRange
   (
      GraphColor::SpillRecord * spillRecord,
      GraphColor::SpillKinds    spillKinds
   );

   void
   Insert
   (
      GraphColor::SpillRecord * spillRecord
   );

   GraphColor::SpillRecord *
   InsertReloadInternal
   (
      llvm::MachineOperand *  newInstanceOperand,
      llvm::MachineOperand *  appearanceOperand,
      llvm::MachineInstr *    pendingInstruction,
      bool                    isOriginal
   );

   void
   SetSpillAliasTag
   (
      llvm::MachineOperand * operand,
      unsigned               spillAliasTag
   );

   static void
   SetSpillKinds
   (
      llvm::MachineInstr *   instruction,
      GraphColor::SpillKinds spillKinds
   );

   void
   SetInstrCount
   (
      llvm::MachineInstr * instruction,
      unsigned             instrCount
   );

   Graphs::OperandToSlotMap::iterator
   NewSlot
   (
      Graphs::OperandToSlotMap * idToSlotMap,
      unsigned                   id
   );

private:

   GraphColor::AvailableExpressions *              AvailableExpressions;
   bool                                            DoReplaceOperandsEqual;
   unsigned                                        InstructionShareDelta;
   llvm::SparseBitVector<> *                       LiveBitVector;
   llvm::SparseBitVector<> *                       OperandsEqualOperandBitVector;
   llvm::SparseBitVector<> *                       ScratchBitVector1;
   llvm::SparseBitVector<> *                       ScratchBitVector2;
   GraphColor::SlotWiseOptimizer *                 SlotWiseOptimizer;
   GraphColor::AliasTagToSpillRecordIterableMap *  SpillMap;

   //temporary map in place of an unsigned field associated with each llvm::MachineInstr
   static std::map<llvm::MachineInstr*,GraphColor::SpillKinds>  instruction2SpillKind;
   static std::map<llvm::MachineInstr*,unsigned>                instruction2InstrCount;
};

//------------------------------------------------------------------------------
//
// Description:
//
//    Extension object placed on operands that can map it back to the original tag.
//
//------------------------------------------------------------------------------

class SpillExtensionObject
{
public:

   unsigned SpillAliasTag;

public:

   static SpillExtensionObject * New()
   {
      SpillExtensionObject * seo = new SpillExtensionObject();
      return seo;
   }

   static SpillExtensionObject *
   GetExtensionObject
   (
      llvm::MachineInstr * instruction,
      unsigned             srcOperandIdx
   );

   static void
   RemoveExtensionObject
   (
      llvm::MachineInstr *   instruction,
      unsigned               srcOperandIdx,
      SpillExtensionObject * object
   );

};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_SPILLOPTIMIZER_H
