//===-- GraphColor/ConflictGraph.cpp ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ConflictGraph.h"
#include "Allocator.h"
#include "Liveness.h"
#include "LiveRange.h"
#include "Tile.h"
#include "TargetRegisterAllocInfo.h"

#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "tiled-conflicts"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Construct a new conflict graph object for this tile.
//
//-----------------------------------------------------------------------------

GraphColor::ConflictGraph *
ConflictGraph::New
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *     allocator = tile->Allocator;
   GraphColor::ConflictGraph * conflictGraph = new GraphColor::ConflictGraph();

   conflictGraph->Allocator = allocator;
   conflictGraph->Tile = tile;

   return conflictGraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Test if two live range ids conflict.
//
//-----------------------------------------------------------------------------

bool
ConflictGraph::HasConflict
(
   unsigned liveRangeId1,
   unsigned liveRangeId2
)
{
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;

   return bitGraph->TestEdge(liveRangeId1, liveRangeId2);

}

namespace {

inline bool
CanConflict
(
   GraphColor::Tile * tile,
   LiveRange *        liveRange1,
   LiveRange *        liveRange2
)
{
   unsigned subIndexA, subIndexB;    // Unused return arguments
   const llvm::TargetRegisterClass * liveRange1RC = liveRange1->BaseRegisterCategory();
   const llvm::TargetRegisterClass * liveRange2RC = liveRange2->BaseRegisterCategory();

   return tile->Allocator->TRI->getCommonSuperRegClass(
     liveRange1RC, 1, liveRange2RC, 1, subIndexA, subIndexB) == nullptr;
}

}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Determine whether a copy between the two types may legally be preferenced
//
//-----------------------------------------------------------------------------

//note: IsPreferentialCopy() is needed only for memory GC or vector architecture targets

#ifdef FUTURE_IMPL
bool
ConflictGraph::IsPreferentialCopy
(
   Types::Type * definitionType,
   Types::Type * sourceType
)
{
   if (definitionType->IsGCPointer || sourceType->IsGCPointer)
   {
      if (definitionType->IsObjectPointer && sourceType->IsObjectPointer)
      {
         // Combine if both instances are object pointer

         return true;
      }
      else if (definitionType->IsManagedPointer && sourceType->IsManagedPointer)
      {
         // Combine if both instances are managed pointer

         return true;
      }

      // Fall out case is if one is unmanaged or if one is object pointer and one is managed pointer.
      // Code is structured this way to favor the "both not a GC pointer" case since that is the
      // common case.
   }
   else
   {
      // If the source and destination sizes don't match,
      // and one or the other is an 8 or 16 bit size,
      // we may create bad code (one such example was seen in
      // yylex in spec2k\gcc with PGI).

      Tiled::BitSize sourceSize = sourceType->BitSize;
      Tiled::BitSize definitionSize = definitionType->BitSize;

      if (sourceSize < definitionSize)
      {
         return (sourceSize >= 32);
      }
      else if (definitionSize < sourceSize)
      {
         return (definitionSize >= 32);
      }

      return true;
   }
}
#endif

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add conflicts to global live ranges
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddGlobalConflicts
(
   GraphColor::Tile *        tile,
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * killBitVector,
   llvm::SparseBitVector<> * liveBitVector
)
{
   GraphColor::Allocator *   allocator = tile->Allocator;
   Tiled::VR::Info *         vrInfo = allocator->VrInfo;
   unsigned                  killGlobalAliasTag;
   unsigned                  liveGlobalAliasTag;
   unsigned                  killRegisterAliasTag;
   unsigned                  liveRegisterAliasTag;
   unsigned                  registerAliasTag;
   unsigned                  notAnAliasTag = VR::Constants::NoMoreBits;
                             //was:  Tiled::BitVector::Constants::NoMoreBits;
   llvm::SparseBitVector<> * doNotAllocateRegisterAliasTagBitVector;
   GraphColor::LiveRange *   killGlobalLiveRange;
   GraphColor::LiveRange *   liveGlobalLiveRange;
   GraphColor::LiveRange *   globalLiveRange;
   GraphColor::LiveRange *   conflictingGlobalLiveRange;
   llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet;
   GraphColor::LiveRange *   copySourceLiveRange = nullptr;

   // Get source live range of copy instruction.
   if ((instruction != nullptr) && (instruction->isCopy())) {
      const llvm::MachineOperand& sourceOperand(instruction->getOperand(1));
      assert(sourceOperand.isReg() && (instruction->getOperand(0)).isReg());
      unsigned regNum = sourceOperand.getReg();
      copySourceLiveRange = allocator->GetGlobalLiveRange(vrInfo->GetTag(regNum));
   }

   doNotAllocateRegisterAliasTagBitVector = allocator->DoNotAllocateRegisterAliasTagBitVector;

   llvm::SparseBitVector<>::iterator kiter;
   for (kiter = killBitVector->begin(); kiter != killBitVector->end(); ++kiter)
   {
      killGlobalAliasTag = *kiter;

      // Get def live range and register if they exist.

      killRegisterAliasTag = notAnAliasTag;

      if (vrInfo->IsPhysicalRegisterTag(killGlobalAliasTag)) {
         if (doNotAllocateRegisterAliasTagBitVector->test(killGlobalAliasTag)) {
            continue;
         }
         // allocatable physical register
         killRegisterAliasTag = killGlobalAliasTag;
      }

      killGlobalLiveRange = allocator->GetGlobalLiveRange(killGlobalAliasTag);

      // If we don't have a def of register or global live range then nothing to do.
      if ((killGlobalLiveRange == nullptr) && (killRegisterAliasTag == notAnAliasTag)) {
         continue;
      }

      llvm::SparseBitVector<>::iterator liter;
      for (liter = liveBitVector->begin(); liter != liveBitVector->end(); ++liter)
      {
         liveGlobalAliasTag = *liter;

         // Get use live range and register if they exist.

         liveRegisterAliasTag = notAnAliasTag;

         if (vrInfo->IsPhysicalRegisterTag(liveGlobalAliasTag)) {
            if (doNotAllocateRegisterAliasTagBitVector->test(liveGlobalAliasTag)) {
               continue;
            }
            // allocatable physical register
            liveRegisterAliasTag = liveGlobalAliasTag;
         }

         liveGlobalLiveRange = allocator->GetGlobalLiveRange(liveGlobalAliasTag);

         if ((liveGlobalLiveRange == nullptr) && (liveRegisterAliasTag == notAnAliasTag)) {
            continue;
         }

         // Handle def of physical reg conflicting with live range of another physical reg (don't do anything).
         if ((killRegisterAliasTag != notAnAliasTag) && (liveRegisterAliasTag != notAnAliasTag)) {
            continue;
         }

         // Determine what register we have and which global live ranges we have.

         globalLiveRange = nullptr;
         conflictingGlobalLiveRange = nullptr;
         registerAliasTag = notAnAliasTag;

         // Handle def of physical reg conflicting with live range.

         if ((killRegisterAliasTag != notAnAliasTag) && (liveGlobalLiveRange != nullptr)) {
            assert(liveRegisterAliasTag == notAnAliasTag);

            globalLiveRange = liveGlobalLiveRange;
            conflictingGlobalLiveRange = killGlobalLiveRange;
            registerAliasTag = killRegisterAliasTag;
         }

         // Handle def of live range conflicting with a physical reg.

         else if ((killGlobalLiveRange != nullptr) && (liveRegisterAliasTag != notAnAliasTag)) {
            assert(killRegisterAliasTag == notAnAliasTag);

            globalLiveRange = killGlobalLiveRange;
            conflictingGlobalLiveRange = liveGlobalLiveRange;
            registerAliasTag = liveRegisterAliasTag;
         }

         // Handle def of live range conflicting with another live range.

         else if ((killGlobalLiveRange != nullptr) && (liveGlobalLiveRange != nullptr)) {
            assert(killRegisterAliasTag == notAnAliasTag);
            assert(liveRegisterAliasTag == notAnAliasTag);

            globalLiveRange = liveGlobalLiveRange;
            conflictingGlobalLiveRange = killGlobalLiveRange;
            registerAliasTag = notAnAliasTag;
         }

         // Optimization to allow preferencing across copies (ignore conflict).

         if ((liveGlobalLiveRange == copySourceLiveRange)
            && (liveGlobalLiveRange != nullptr) && (killGlobalLiveRange != nullptr)
          /*&& ConflictGraph::IsPreferentialCopy(killGlobalLiveRange->Type, copySourceLiveRange->Type)*/ ) {
            continue;
         }

         // If we have a global live range and register, then add the register as a conflict.
         // We include all the overlapping register now and prune the set later.

         if (registerAliasTag != notAnAliasTag) {
            globalConflictRegisterAliasTagSet = (*tile->GlobalLiveRangeConflictsVector)[globalLiveRange->Id];

            if (globalConflictRegisterAliasTagSet == nullptr) {
               // lazy init global conflict info for this tile.
               globalConflictRegisterAliasTagSet = new llvm::SparseBitVector<>();
               (*(tile->GlobalLiveRangeConflictsVector))[globalLiveRange->Id] = globalConflictRegisterAliasTagSet;
            }

            vrInfo->OrMayPartialTags(registerAliasTag, globalConflictRegisterAliasTagSet);

            // Don't create conflicts with global live ranges of physregs.
            continue;
         }

         if ((globalLiveRange == nullptr) || (conflictingGlobalLiveRange == nullptr))
            continue;

         // Note: This test is only needed for architectures with > 1 number of register categories
         if (CanConflict(tile, globalLiveRange, conflictingGlobalLiveRange))
            continue;

         if (globalLiveRange->VrTag == conflictingGlobalLiveRange->VrTag)
            continue;

         // If we have a global live range and conflicting global live range, then add the
         // each global live range to the others conflicting live ranges.
         assert(!globalLiveRange->IsPhysicalRegister);
         assert(!conflictingGlobalLiveRange->IsPhysicalRegister);
         globalLiveRange->GlobalConflictAliasTagSet->set(conflictingGlobalLiveRange->VrTag);
         conflictingGlobalLiveRange->GlobalConflictAliasTagSet->set(globalLiveRange->VrTag);

      }

   }

}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add conflicts to global live ranges
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddGlobalRegisterConflicts
(
   GraphColor::Tile *      tile,
   llvm::MachineOperand *  operand
)
{
   GraphColor::Allocator *    allocator = tile->Allocator;
   Tiled::VR::Info *          vrInfo = allocator->VrInfo;
   assert(operand->isReg());
   unsigned                   aliasTag = vrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange *    globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);
   llvm::SparseBitVector<> *  allocatableRegisterAliasTagBitVector;
   llvm::SparseBitVector<> *  globalConflictRegisterAliasTagSet;

   if (globalLiveRange == nullptr) {
      return;
   }

   if (globalLiveRange->IsPhysicalRegister) {
      return;
   }

   globalConflictRegisterAliasTagSet = (*tile->GlobalLiveRangeConflictsVector)[globalLiveRange->Id];
   if (globalConflictRegisterAliasTagSet == nullptr) {
      // lazy init global conflict info for this tile.
      globalConflictRegisterAliasTagSet = new llvm::SparseBitVector<>();
      (*tile->GlobalLiveRangeConflictsVector)[globalLiveRange->Id] = globalConflictRegisterAliasTagSet;
   }

   const llvm::TargetRegisterClass * registerCategory = globalLiveRange->GetRegisterCategory();
   allocatableRegisterAliasTagBitVector = allocator->GetAllocatableRegisters(registerCategory);

   unsigned pseudoReg = operand->getReg();
   assert(VR::Info::IsVirtualRegister(pseudoReg));

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = allocatableRegisterAliasTagBitVector->begin(); r != allocatableRegisterAliasTagBitVector->end(); ++r)
   {
      unsigned registerAliasTag = *r;

      // <place for code supporting architectures with sub-registers>

      // Add the register as a conflict.   We include all the overlapping 
      // register now and prune the set later.

      vrInfo->OrMayPartialTags(registerAliasTag, globalConflictRegisterAliasTagSet);
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build global conflicts and annotate global live ranges with
//    them. Local tile data is built first and then summarized on
//    global live range data structures.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::BuildGlobalConflicts
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::Liveness *             liveness = allocator->Liveness;
   Dataflow::LivenessData *           registerLivenessData;

   // Create temporary bit vectors.
   llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * genBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * killBitVector = new llvm::SparseBitVector<>();

   // Process each block in function and add conflict registers to global live ranges.

   unsigned globalLiveRangeCount = allocator->GlobalLiveRangeCount();
   GraphColor::TileList * tilesPostOrder = allocator->TileGraph->PostOrderTileList;
   GraphColor::TileList::iterator titer;

   // foreach_tile_in_dfs_postorder
   for (titer = tilesPostOrder->begin(); titer != tilesPostOrder->end(); ++titer)
   {
      GraphColor::Tile * tile = *titer;

      // Set up tile live range conflict vector.

      GraphColor::SparseBitVectorVector * globalLiveRangeConflictsVector =
         new GraphColor::SparseBitVectorVector((globalLiveRangeCount + 1), nullptr);
      tile->GlobalLiveRangeConflictsVector = globalLiveRangeConflictsVector;

      Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;
      Graphs::MachineBasicBlockVector::reverse_iterator biter;

      // foreach_block_in_tile_backward
      for (biter = mbbVector->rbegin(); biter != mbbVector->rend(); ++biter)
      {
         llvm::MachineBasicBlock * block = *biter;

         registerLivenessData = liveness->GetRegisterLivenessData(block);
         *(liveBitVector) = *(registerLivenessData->LiveOutBitVector);

         // Process each instruction in the block moving backwards calculating liveness, building
         // conflict registers for global live ranges.

         llvm::MachineBasicBlock::reverse_instr_iterator rii;

         // foreach_instr_in_block_backward_editing
         for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
         {
            llvm::MachineInstr * instruction = &(*rii);

            liveness->TransferInstruction(instruction, genBitVector, killBitVector);

            // Add conflicts for global live ranges.
            ConflictGraph::AddGlobalConflicts(tile, instruction, killBitVector, liveBitVector);

            // Keep track of global live ranges spanning calls.
            if (instruction->isCall()) {
               ConflictGraph::MarkGlobalLiveRangesSpanningCall(allocator, liveBitVector);
            }

            // Add conflicts with physical registers based on constraints of appearance.

            // foreach_register_source_and_destination_opnd
            foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
               ConflictGraph::AddGlobalRegisterConflicts(tile, operand);

               next_source_and_destination_opnd_v2(operand, instruction, end_iter);
            }

            // Update liveness gens and kills.
            liveness->UpdateInstruction(instruction, liveBitVector, genBitVector, killBitVector);
         }

         //EH:  For now, EH flow not converted/implemented

         // If this block has a predecessor that ends in an EH instruction with
         // a dangling definition, we want to consider that definition part of
         // this block, so go find it here

         // <place for EH-flow-related code>

      }

      // Prune global live range register conflicts to be only registers in its allocatable category.

      GraphColor::LiveRangeVector * lrVector = allocator->GetGlobalLiveRangeEnumerator();

      // foreach_global_liverange_in_allocator
      for (unsigned i = 1; i < lrVector->size(); ++i) {
         GraphColor::LiveRange *   globalLiveRange = (*lrVector)[i];
         llvm::SparseBitVector<> * allocatableRegistersAliasTagSet;
         llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet;

         globalConflictRegisterAliasTagSet = (*globalLiveRangeConflictsVector)[globalLiveRange->Id];

         if (globalConflictRegisterAliasTagSet != nullptr) {
            allocatableRegistersAliasTagSet = allocator->GetCategoryRegisters(globalLiveRange);
            (*globalConflictRegisterAliasTagSet) &= (*allocatableRegistersAliasTagSet);
         }
      }

   }

   // Perform a full update of the global scope.

   ConflictGraph::UpdateGlobalConflicts(allocator->GlobalAliasTagSet, allocator);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Update global live range data structures with tile local data.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::UpdateGlobalConflicts
(
   llvm::SparseBitVector<> *  globalLiveRangeUpdateAliasTagSet,
   GraphColor::Allocator *    allocator
)
{
   // Walk global live ranges and collect global conflicts.

   GraphColor::TileList * tilesPreOrder = allocator->TileGraph->PreOrderTileList;
   GraphColor::TileList::iterator titer;

   // foreach_tile_in_dfs_preorder
   for (titer = tilesPreOrder->begin(); titer != tilesPreOrder->end(); ++titer)
   {
      GraphColor::Tile * tile = *titer;

      llvm::SparseBitVector<>::iterator b;

      // foreach_sparse_bv_bit
      for (b = globalLiveRangeUpdateAliasTagSet->begin(); b != globalLiveRangeUpdateAliasTagSet->end(); ++b)
      {
         unsigned globalLiveRangeAliasTag = *b;

         GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(globalLiveRangeAliasTag);
         assert(globalLiveRange != nullptr);

         llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet =
            globalLiveRange->GlobalConflictRegisterAliasTagSet;

         if (tile->IsRoot()) {
            globalConflictRegisterAliasTagSet->clear();
         }

         llvm::SparseBitVector<> * tileGlobalConflictRegisterAliasTagSet =
            (*tile->GlobalLiveRangeConflictsVector)[globalLiveRange->Id];

         if (tileGlobalConflictRegisterAliasTagSet != nullptr) {
            (*globalConflictRegisterAliasTagSet) |= (*tileGlobalConflictRegisterAliasTagSet);
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize the conflict graph bit graph 
//
//-----------------------------------------------------------------------------

void
ConflictGraph::InitializeBitGraph()
{
   GraphColor::Tile *              tile = this->Tile;
   int                             bitGraphSize = (tile->LiveRangeCount() + 1);
   BitGraphs::UndirectedBitGraph * bitGraph = BitGraphs::UndirectedBitGraph::New(bitGraphSize);

   this->BitGraph = bitGraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build the conflict graph bit graph 
//
//-----------------------------------------------------------------------------

void
ConflictGraph::BuildBitGraph()
{
   this->InitializeBitGraph();

   GraphColor::Tile *       tile = this->Tile;
   GraphColor::Allocator *  allocator = this->Allocator;
   GraphColor::Liveness *   liveness = allocator->Liveness;
   Dataflow::LivenessData * registerLivenessData;

   // Create temporary bit vectors.
   llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * genBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * killBitVector = new llvm::SparseBitVector<>();

   // Process each block and add conflict edges at definition points.

   Graphs::MachineBasicBlockVector::reverse_iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile_backward
   for (biter = mbbVector->rbegin(); biter != mbbVector->rend(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;

      registerLivenessData = liveness->GetRegisterLivenessData(block);
      (*liveBitVector) = (*registerLivenessData->LiveOutBitVector);
      
      DEBUG({
         llvm::dbgs() << "\nMBB#" << block->getNumber() << "   (Conflict/liveOutBitVector)\n";
         llvm::SparseBitVector<>::iterator a;
         llvm::dbgs() << "  LiveOut:   {";
         for (a = liveBitVector->begin(); a != liveBitVector->end(); ++a) {
            llvm::dbgs() << *a << ", ";
         }
         llvm::dbgs() << "}\n";
      });

      // Process each instruction in the block moving backwards calculating liveness, building
      // conflict graph and removing dead stores.

      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward_editing
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);

         liveness->TransferInstruction(instruction, genBitVector, killBitVector);

         // Exclude tile entries - we get their info from summary conflict graph.

         if (!Tile::IsEnterTileInstruction(instruction)) {

            // Add conflict edges for (def, def) and (def, live) pairs.
            this->AddDefinitionLiveConflicts(instruction, killBitVector, liveBitVector);

            // Note live ranges that span calls.
            if (instruction->isCall()) {
               this->MarkLiveRangesSpanningCall(liveBitVector);
            }
         }

         // Update liveness gens and kills.
         liveness->UpdateInstruction(instruction, liveBitVector, genBitVector, killBitVector);

         // Add conflicts with physical registers based on constraints of appearance.

         // foreach_register_source_and_destination_opnd
         foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
            this->AddConflictEdges(operand);

            next_source_and_destination_opnd_v2(operand, instruction, end_iter);
         }
      }

      //EH:  For now, EH flow not converted/implemented

      // If this block has a predecessor that ends in an EH instruction with
      // a dangling definition, we want to consider that definition part of
      // this block, so go find it here

      // <place for EH-flow-related code>

   }

   // Add conflicts coming from nested tiles.

   GraphColor::TileList * nestedTiles = tile->NestedTileList;
   GraphColor::TileList::iterator ntiter;

   // foreach_nested_tile_in_tile
   for (ntiter = nestedTiles->begin(); ntiter != nestedTiles->end(); ++ntiter)
   {
      GraphColor::Tile * nestedTile = *ntiter;

      // Add conflicts coming from nested summary live ranges in nested tiles.
      GraphColor::LiveRangeVector * lrVector = nestedTile->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned i = 1; i < lrVector->size(); ++i)
      {
         GraphColor::LiveRange *  nestedSummaryLiveRange = (*lrVector)[i];

         if (!nestedSummaryLiveRange->IsPhysicalRegister && !nestedSummaryLiveRange->IsSecondChanceGlobal) {
            this->AddConflictEdges(nestedTile, nestedSummaryLiveRange);
         }
      }
   }

   // Add conflicts coming between global live ranges.

   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   llvm::SparseBitVector<> *       globalAliasTagSet = tile->GlobalAliasTagSet;
   GraphColor::LiveRange *         globalLiveRange;
   GraphColor::LiveRange *         liveRange;
   GraphColor::LiveRange *         conflictLiveRange;
   unsigned                        aliasTag;
   unsigned                        conflictAliasTag;
   llvm::SparseBitVector<> *       globalLiveInAliasTagSet = tile->GlobalLiveInAliasTagSet;
   llvm::SparseBitVector<> *       globalLiveOutAliasTagSet = tile->GlobalLiveOutAliasTagSet;

   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = globalAliasTagSet->begin(); g != globalAliasTagSet->end(); ++g)
   {
      aliasTag = *g;

      liveRange = tile->GetLiveRange(aliasTag);
      if (liveRange == nullptr) {
         continue;
      }

      globalLiveRange = liveRange->GlobalLiveRange;
      assert(globalLiveRange != nullptr);

      llvm::SparseBitVector<> * globalConflictAliasTagSet = globalLiveRange->GlobalConflictAliasTagSet;
      llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet =
         globalLiveRange->GlobalConflictRegisterAliasTagSet;

      // If global is live in track global conflicts on entry.

      if (globalLiveInAliasTagSet->test(aliasTag)) {
         llvm::SparseBitVector<>::iterator c;

         // foreach_sparse_bv_bit
         for (c = globalConflictAliasTagSet->begin(); c != globalConflictAliasTagSet->end(); ++c)
         {
            conflictAliasTag = *c;

            if (globalLiveInAliasTagSet->test(conflictAliasTag)) {
               conflictLiveRange = tile->GetLiveRange(conflictAliasTag);

               if (conflictLiveRange == nullptr) {
                  continue;
               }

               bitGraph->AddEdge(liveRange->Id, conflictLiveRange->Id);
            }
         }
      }

      // If global is live out track global conflicts on exit.

      if (globalLiveOutAliasTagSet->test(aliasTag)) {
         llvm::SparseBitVector<>::iterator c;

         // foreach_sparse_bv_bit
         for (c = globalConflictAliasTagSet->begin(); c != globalConflictAliasTagSet->end(); ++c)
         {
            conflictAliasTag = *c;

            if (globalLiveOutAliasTagSet->test(conflictAliasTag)) {
               conflictLiveRange = tile->GetLiveRange(conflictAliasTag);

               if (conflictLiveRange == nullptr) {
                  continue;
               }

               bitGraph->AddEdge(liveRange->Id, conflictLiveRange->Id);
            }
         }
      }

      // Handle global conflicts coming out of nested tiles.

      // foreach_nested_tile_in_tile
      for (ntiter = nestedTiles->begin(); ntiter != nestedTiles->end(); ++ntiter)
      {
         GraphColor::Tile * nestedTile = *ntiter;

         llvm::SparseBitVector<> * nestedGlobalLiveOutAliasTagSet = nestedTile->GlobalLiveOutAliasTagSet;

         if (nestedGlobalLiveOutAliasTagSet->test(aliasTag)) {
            llvm::SparseBitVector<>::iterator c;

            // foreach_sparse_bv_bit
            for (c = globalConflictAliasTagSet->begin(); c != globalConflictAliasTagSet->end(); ++c)
            {
               conflictAliasTag = *c;

               if (nestedGlobalLiveOutAliasTagSet->test(conflictAliasTag)) {
                  conflictLiveRange = tile->GetLiveRange(conflictAliasTag);

                  if (conflictLiveRange == nullptr) {
                     // Handle spilled in prior iteration case.
                     continue;
                  }

                  bitGraph->AddEdge(liveRange->Id, conflictLiveRange->Id);
               }
            }

            // Handle conflicts between global candidates and global
            // register live ranges.  Physregs live out of tiles will not
            // have a live on def conflict so we look structurally at what
            // is live out of nested (live into parent) tiles.  

            unsigned conflictRegisterAliasTag;

            // foreach_sparse_bv_bit
            for (c = globalConflictRegisterAliasTagSet->begin(); c != globalConflictRegisterAliasTagSet->end(); ++c)
            {
               conflictRegisterAliasTag = *c;

               if (nestedGlobalLiveOutAliasTagSet->test(conflictRegisterAliasTag)) {
                  GraphColor::LiveRange * conflictRegisterLiveRange =
                     tile->GetLiveRange(conflictRegisterAliasTag);
                  assert(conflictRegisterLiveRange != nullptr);

                  bitGraph->AddEdge(liveRange->Id, conflictRegisterLiveRange->Id);
               }
            }
         }
      }

      // Physical registers don't track direct conflicts to reduce the
      // number of elements in the list so a separate walk on live in
      // and live out is done here to add conflicts.

      if (globalLiveRange->IsPhysicalRegister) {
         globalConflictAliasTagSet = tile->GlobalLiveInAliasTagSet;

         while (globalConflictAliasTagSet != nullptr)
         {
            if (globalConflictAliasTagSet->test(aliasTag)) {
               llvm::SparseBitVector<>::iterator c;

               // foreach_sparse_bv_bit
               for (c = globalConflictAliasTagSet->begin(); c != globalConflictAliasTagSet->end(); ++c)
               {
                  conflictAliasTag = *c;

                  conflictLiveRange = tile->GetLiveRange(conflictAliasTag);
                  if (conflictLiveRange == nullptr) {
                     continue;
                  }

                  if (liveRange == conflictLiveRange) {
                     continue;
                  }

                  if (CanConflict(tile, liveRange, conflictLiveRange))
                     continue;

                  bitGraph->AddEdge(liveRange->Id, conflictLiveRange->Id);
               }
            }

            globalConflictAliasTagSet = (globalConflictAliasTagSet == tile->GlobalLiveInAliasTagSet)
               ?  globalConflictAliasTagSet = tile->GlobalLiveOutAliasTagSet
               : nullptr;
         }
      }

      if (globalLiveRange->IsCalleeSaveValue) {

         Tiled::VR::Info *                 vrInfo = allocator->VrInfo;
         unsigned                          registerAliasTag;
         const llvm::TargetRegisterClass * registerCategory = globalLiveRange->GetRegisterCategory();
         llvm::SparseBitVector<> *         allocatableRegisterAliasTagBitVector =
                                              allocator->GetAllocatableRegisters(registerCategory);
         unsigned                          calleeSaveValueRegister = globalLiveRange->CalleeSaveValueRegister;
         assert(calleeSaveValueRegister != VR::Constants::InvalidReg);

         unsigned globalLiveRangeCalleeSaveSpillTag = vrInfo->GetTag(calleeSaveValueRegister);

         llvm::SparseBitVector<>::iterator r;

         // foreach_sparse_bv_bit
         for (r = allocatableRegisterAliasTagBitVector->begin(); r != allocatableRegisterAliasTagBitVector->end(); ++r)
         {
            registerAliasTag = *r;

            if (!vrInfo->MustTotallyOverlap(registerAliasTag, globalLiveRangeCalleeSaveSpillTag)) {
               GraphColor::LiveRange * registerLiveRange = tile->GetLiveRange(registerAliasTag);
               if (registerLiveRange != nullptr) {
                  assert(liveRange->Id != registerLiveRange->Id);
                  bitGraph->AddEdge(liveRange->Id, registerLiveRange->Id);
               }
            }

         }
      }
   }

   delete liveBitVector;
   delete genBitVector;
   delete killBitVector;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add conflict edges between:
//       1) all definitions - they cannot occupy the same register;
//       2) all definitions and all currently live registers.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddDefinitionLiveConflicts
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * definitionBitVector,
   llvm::SparseBitVector<> * liveBitVector
)
{
   if (definitionBitVector->empty()) {
      return;
   }

   // Add conflict edges between all definitions as they can't occupy the same register.
   // Ignore on call for TP reasons unless we annotating this for Alsa, in which case we need
   // to create conflicts.

   bool doIgnoreDefinitionConflicts = instruction->isCall();

   if (!doIgnoreDefinitionConflicts) {
      this->AddConflictEdges(instruction, definitionBitVector, definitionBitVector);
   } else {
      // Definition set for the call instruction contains physical registers only, and
      // they cannot conflict with each other.
      //
      // The same thing goes for outline instructions summarizing inline ASM.
   }

   // Add conflict edges between all definitions and all currently live registers.
   this->AddConflictEdges(instruction, definitionBitVector, liveBitVector);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add edges in the conflict graph between new definitions and existing 
//    live ranges.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddConflictEdges
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * definitionBitVector,
   llvm::SparseBitVector<> * liveBitVector
)
{
   GraphColor::Allocator *         allocator = this->Allocator;
   Tiled::VR::Info *               vrInfo = allocator->VrInfo;
   GraphColor::Tile *              tile = this->Tile;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   unsigned                        definitionAliasTag;
   unsigned                        liveAliasTag;
   GraphColor::LiveRange *         copySourceLiveRange = nullptr;
   llvm::SparseBitVector<> *       intLiveRangeIdBitVector = allocator->ScratchBitVector1;
   llvm::SparseBitVector<> *       floatLiveRangeIdBitVector = allocator->ScratchBitVector2;
   unsigned                        basicBlockId = (instruction->getParent())->getNumber();
   llvm::SparseBitVector<> *       integerMaxKillBlockBitVector = tile->IntegerMaxKillBlockBitVector;
   llvm::SparseBitVector<> *       floatMaxKillBlockBitVector = tile->FloatMaxKillBlockBitVector;
   bool                            isMaxIntegerKill = integerMaxKillBlockBitVector->test(basicBlockId);
   bool                            isMaxFloatKill = floatMaxKillBlockBitVector->test(basicBlockId);
   bool                            doPressureCalculation
      = ((definitionBitVector != liveBitVector) && this->DoPressureCalculation
         && (!isMaxIntegerKill || !isMaxFloatKill));
   bool                            isFirstPass = true;
   unsigned                        maxIntegerRegisterCount = this->MaxIntegerRegisterCount;
   unsigned                        maxFloatRegisterCount = this->MaxFloatRegisterCount;

   if (doPressureCalculation) {
      intLiveRangeIdBitVector->clear();
      floatLiveRangeIdBitVector->clear();
   }

   // Get source live range of copy instruction.

   if ((instruction != nullptr) && (instruction->isCopy())) {
      const llvm::MachineOperand& sourceOperand(instruction->getOperand(1));
      if (sourceOperand.isReg() && (instruction->getOperand(0)).isReg()) {
         unsigned regNum = sourceOperand.getReg();
         copySourceLiveRange = tile->GetLiveRange(vrInfo->GetTag(regNum));
      }
   }

   DEBUG({
      if (instruction->isCall()) {
         llvm::dbgs() << "++++++ CALL instruction:\n";
         instruction->dump();

         llvm::dbgs() << "\ndefinitionBitVector = { ";
         for (llvm::SparseBitVector<>::iterator d = definitionBitVector->begin();
              d != definitionBitVector->end(); ++d)
         {
            llvm::dbgs() << *d << ", ";
         }
         llvm::dbgs() << "}\n";

         llvm::dbgs() << "liveBitVector = { ";
         for (llvm::SparseBitVector<>::iterator l = liveBitVector->begin();
              l != liveBitVector->end(); ++l)
         {
            llvm::dbgs() << *l << ", ";
         }
         llvm::dbgs() << "}\n";
      }
   });

   // Loop through definition live ranges and currently live live ranges and
   // create conflicts as appropriate.

   llvm::SparseBitVector<>::iterator d;

   // foreach_sparse_bv_bit
   for (d = definitionBitVector->begin(); d != definitionBitVector->end(); ++d)
   {
      definitionAliasTag = *d;
      //DEBUG(llvm::dbgs() << "Def:" << definitionAliasTag << "  conflicts w/ Live:" << liveAliasTag << "\n");

      GraphColor::LiveRange * definitionLiveRange = tile->GetLiveRange(definitionAliasTag);
      if (definitionLiveRange == nullptr) {
         continue;
      }

      llvm::SparseBitVector<>::iterator l;

      // foreach_sparse_bv_bit
      for (l = liveBitVector->begin(); l != liveBitVector->end(); ++l)
      {
         liveAliasTag = *l;

         GraphColor::LiveRange * liveRange = tile->GetLiveRange(liveAliasTag);
         if (liveRange == nullptr)
            continue;

         if (definitionLiveRange == liveRange)
            continue;

         // Optimization to allow preferencing across copies (ignore conflict).

         if (liveRange == copySourceLiveRange)
            /*&& ConflictGraph::IsPreferentialCopy(definitionLiveRange->Type, copySourceLiveRange->Type)*/ {
            // IsPreferentialCopy needed only for memory GC or vector architecture targets
            continue;
         }

         if (CanConflict(tile, liveRange, definitionLiveRange))
            continue;

         if (doPressureCalculation && isFirstPass) {
            const llvm::TargetRegisterClass * baseRegisterCategory = liveRange->BaseRegisterCategory();

            //TODO: For architectures with separate register classes to hold integer and floating point
            // values the 1st term of this condition is not needed.
            if (/*baseRegisterCategory->IsInt &&*/ !isMaxIntegerKill) {
               intLiveRangeIdBitVector->set(liveRange->Id);
            } else if (!isMaxFloatKill) {
               floatLiveRangeIdBitVector->set(liveRange->Id);
            }
         }

         bitGraph->AddEdge(definitionLiveRange->Id, liveRange->Id);
      }

      if (isFirstPass && doPressureCalculation) {
         unsigned intLiveRangeCount = intLiveRangeIdBitVector->count();

         if (intLiveRangeCount > maxIntegerRegisterCount) {
            isMaxIntegerKill = true;
            integerMaxKillBlockBitVector->set(basicBlockId);
         }

         unsigned floatLiveRangeCount = floatLiveRangeIdBitVector->count();

         if (floatLiveRangeCount > maxFloatRegisterCount) {
            isMaxFloatKill = true;
            floatMaxKillBlockBitVector->set(basicBlockId);
         }

         isFirstPass = false;
      }
   }
}



//-----------------------------------------------------------------------------
//
// Description:
//
//    Add edges in the conflict graph between this summary variable and all
//    other summary variables coming from nested tile.  Remove edges for
//    live ranges summarized by this summary variable inside the nested tile.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddConflictEdges
(
   GraphColor::Tile *      nestedTile,
   GraphColor::LiveRange * summaryLiveRange
)
{
   GraphColor::Tile *                 tile = this->Tile;
   Tiled::VR::Info *                  vrInfo = tile->VrInfo;
   GraphColor::SummaryConflictGraph * summaryConflictGraph = nestedTile->SummaryConflictGraph;
   BitGraphs::UndirectedBitGraph *    bitGraph = this->BitGraph;
   unsigned                           summaryAliasTag = summaryLiveRange->VrTag;
   llvm::SparseBitVector<> *          summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
   GraphColor::LiveRange *            liveRange = tile->GetLiveRange(summaryAliasTag);

   GraphColor::GraphIterator citer;
   GraphColor::LiveRange * conflictSummaryLiveRange;

   if (liveRange != nullptr) {
      unsigned                liveRangeId = liveRange->Id;
      unsigned                conflictAliasTag;
      GraphColor::LiveRange * conflictLiveRange;

      // foreach_conflict_summaryliverange
      for (conflictSummaryLiveRange = summaryConflictGraph->GetFirstConflictSummaryLiveRange(&citer, summaryLiveRange);
           conflictSummaryLiveRange != nullptr;
           conflictSummaryLiveRange = summaryConflictGraph->GetNextConflictSummaryLiveRange(&citer))
      {
         if (conflictSummaryLiveRange->IsSecondChanceGlobal || conflictSummaryLiveRange->IsSpilled()) {
            continue;
         }

         if (conflictSummaryLiveRange->HasHardPreference) {
            conflictAliasTag = vrInfo->GetTag(conflictSummaryLiveRange->Register);
         } else {
            conflictAliasTag = conflictSummaryLiveRange->VrTag;
         }

         conflictLiveRange = tile->GetLiveRange(conflictAliasTag);
         assert(conflictLiveRange != nullptr);

         unsigned conflictLiveRangeId = conflictLiveRange->Id;
         if (liveRangeId != conflictLiveRangeId) {
            // Add conflict edges between summary variables
            bitGraph->AddEdge(liveRangeId, conflictLiveRangeId);
         }
      }

   } else {
      assert(summaryLiveRange->HasHardPreference || summaryLiveRange->IsSpilled());
   }

   // Add conflict edges for constituents of summary variables that appear as live ranges in
   // current tile.

   {
      unsigned                aliasTag;
      GraphColor::LiveRange * liveRange;
      unsigned                liveRangeId;
      unsigned                conflictAliasTag;
      GraphColor::LiveRange * conflictLiveRange;
      unsigned                conflictLiveRangeId;

      // foreach_conflict_summaryliverange
      for (conflictSummaryLiveRange = summaryConflictGraph->GetFirstConflictSummaryLiveRange(&citer, summaryLiveRange);
           conflictSummaryLiveRange != nullptr;
           conflictSummaryLiveRange = summaryConflictGraph->GetNextConflictSummaryLiveRange(&citer))
      {
         // get all constituents of the conflicting summary LR
         llvm::SparseBitVector<> * conflictSummaryAliasTagSet = conflictSummaryLiveRange->SummaryAliasTagSet;

         llvm::SparseBitVector<>::iterator s;

         // foreach_sparse_bv_bit:  iterate over the constituents of the input argument summary LR
         for (s = summaryAliasTagSet->begin(); s != summaryAliasTagSet->end(); ++s)
         {
            aliasTag = *s;
            if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            liveRange = tile->GetLiveRange(aliasTag);
            if (liveRange == nullptr) {
               continue;
            }

            liveRangeId = liveRange->Id;

            llvm::SparseBitVector<>::iterator c;

            // foreach_sparse_bv_bit:  iterate over the constituents of the conflicting summary LR
            for (c = conflictSummaryAliasTagSet->begin(); c != conflictSummaryAliasTagSet->end(); ++c)
            {
               conflictAliasTag = *c;

               if (vrInfo->IsPhysicalRegisterTag(conflictAliasTag)) {
                  continue;
               }

               conflictLiveRange = tile->GetLiveRange(conflictAliasTag);
               if (conflictLiveRange == nullptr) {
                  continue;
               }

               conflictLiveRangeId = conflictLiveRange->Id;
               if (liveRangeId != conflictLiveRangeId) {
                  bitGraph->AddEdge(liveRangeId, conflictLiveRange->Id);
               }
            }
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add edges in the conflict graph between this live range and any physical
//    register in its allocation category that cannot be allocated/rewritten to 
//    this operand.  This prunes allocation set based on constraints like
//    not-pairable, no-subregisters, ...
//
// Remarks:
//
//    See AddGlobalRegisterConflicts for global version.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::AddConflictEdges
(
   llvm::MachineOperand * operand
)
{
   assert(operand->isReg());

   GraphColor::Tile *                tile = this->Tile;
   GraphColor::Allocator *           allocator = this->Allocator;
   Tiled::VR::Info *                 vrInfo = allocator->VrInfo;  
   unsigned                          vrTag = vrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange *           liveRange = tile->GetLiveRange(vrTag);
   unsigned                          pseudoReg;
   unsigned                          reg;
   //Registers::Register *             subRegister;

   if (liveRange == nullptr) {
      return;
   }

   if (liveRange->IsPhysicalRegister) {
      return;
   }

   const llvm::TargetRegisterClass * registerCategory = this->Allocator->GetRegisterCategory(operand->getReg());
   llvm::SparseBitVector<> *         allocatableRegisterAliasTagBitVector =
      allocator->GetAllocatableRegisters(registerCategory);

   unsigned              liveRangeCalleeSaveRegisterTag = VR::Constants::InvalidTag;
   unsigned              calleeSaveValueRegister = liveRange->CalleeSaveValueRegister;

   if (calleeSaveValueRegister != VR::Constants::InvalidReg) {
      liveRangeCalleeSaveRegisterTag = vrInfo->GetTag(calleeSaveValueRegister);
   }

   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;

   pseudoReg = operand->getReg();
   assert(vrInfo->IsVirtualRegister(pseudoReg));

   GraphColor::LiveRange *           registerLiveRange;
   unsigned                          registerAliasTag;

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = allocatableRegisterAliasTagBitVector->begin(); r != allocatableRegisterAliasTagBitVector->end(); ++r)
   {
      registerAliasTag = *r;
      reg = vrInfo->GetRegister(registerAliasTag);

      // <place for code supporting architectures with sub-registers>

      if (!liveRange->IsGlobal() && liveRange->IsCalleeSaveValue) {
         assert(liveRangeCalleeSaveRegisterTag != VR::Constants::InvalidTag);

         if (!vrInfo->MustTotallyOverlap(registerAliasTag, liveRangeCalleeSaveRegisterTag)) {
            registerLiveRange = tile->GetLiveRange(registerAliasTag);

            if (registerLiveRange != nullptr) {
               assert(liveRange->Id != registerLiveRange->Id);
               bitGraph->AddEdge(liveRange->Id, registerLiveRange->Id);
            }
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Mark live ranges spanning call instruction.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::MarkLiveRangesSpanningCall
(
   llvm::SparseBitVector<> * liveBitVector
)
{
   GraphColor::Tile * tile = this->Tile;

   tile->NumberCalls++;

   unsigned  liveAliasTag;

   llvm::SparseBitVector<>::iterator l;

   // foreach_sparse_bv_bit
   for (l = liveBitVector->begin(); l != liveBitVector->end(); ++l)
   {
      liveAliasTag = *l;

      GraphColor::LiveRange * liveRange = tile->GetLiveRange(liveAliasTag);

      if ((liveRange != nullptr) && !liveRange->IsPreColored) {
         if (!liveRange->IsCallSpanning) {
            tile->NumberCallSpanningLiveRanges++;
            liveRange->IsCallSpanning = true;
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Mark live ranges spanning call instruction.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::MarkGlobalLiveRangesSpanningCall
(
   GraphColor::Allocator *     allocator,
   llvm::SparseBitVector<> *   liveBitVector
)
{
   // Increment global number of calls.

   allocator->NumberCalls++;

   unsigned liveAliasTag;

   llvm::SparseBitVector<>::iterator liter;

   // foreach_sparse_bv_bit
   for (liter = liveBitVector->begin(); liter != liveBitVector->end(); ++liter)
   {
      liveAliasTag = *liter;

      GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(liveAliasTag);
      if ((globalLiveRange != nullptr) && !globalLiveRange->IsPreColored) {
         if (!globalLiveRange->IsCallSpanning) {
            globalLiveRange->IsCallSpanning = true;
            allocator->NumberGlobalCallSpanningLiveRanges++;
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize empty adjacency vector.
//
// Remarks:
//
//     Also initialize each live ranges degree and conflict edge count.
//
//-----------------------------------------------------------------------------

void
ConflictGraph::InitializeAdjacencyVector()
{
   GraphColor::Tile *              tile = this->Tile;
   GraphColor::Allocator *         allocator = tile->Allocator;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   unsigned                        liveRangeCount = tile->LiveRangeCount();
   unsigned                        edgeCount = 0;

   for (unsigned liveRangeId1 = 1; liveRangeId1 <= liveRangeCount; liveRangeId1++)
   {
      GraphColor::LiveRange * liveRange1 = tile->GetLiveRangeById(liveRangeId1);

      for (unsigned liveRangeId2 = (liveRangeId1 + 1); liveRangeId2 <= liveRangeCount; liveRangeId2++)
      {
         if (bitGraph->TestEdge(liveRangeId1, liveRangeId2)) {
            GraphColor::LiveRange * liveRange2 = tile->GetLiveRangeById(liveRangeId2);
            unsigned                degree;

            degree = allocator->CalculateDegree(liveRange1, liveRange2);
            liveRange1->Degree += degree;
            liveRange1->ConflictEdgeCount++;

            degree = allocator->CalculateDegree(liveRange2, liveRange1);
            liveRange2->Degree += degree;
            liveRange2->ConflictEdgeCount++;

            edgeCount += 2;
         }
      }
   }

   this->NodeCount = liveRangeCount;
   this->EdgeCount = edgeCount;

   unsigned size = edgeCount;

   GraphColor::IdVector * adjacencyVector = new GraphColor::IdVector(size, 0);
   this->AdjacencyVector = adjacencyVector;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build adjacency vector after bit graph has been built.
//
// Remarks:
//
//    Set the ConflictEdgeCount and ConflictAdjacencyIndex on live range.
//    Also, set the initial "Degree" on live range.  
//
//    AdjacencyVector layout is as follows:
//
//                                            AdjacenceyVector
//                                            +------------------------------+
//    liveRange1->ConflictAdjacenceyIndex --> |conflictingLiveRangeId1       |
//                                            |conflictingLiveRangeId2       |
//                                            |...                           |
//    liveRange1->ConflictEdgeCount == n      |conflictingLiveRangeId(n)     |
//                                            +------------------------------+
//    liveRange2->ConflictAdjacenceyIndex --> |conflictingLiveRangeId1       |
//                                            |conflictingLiveRangeId2       |
//                                            |...                           |
//    liveRange2->ConflictEdgeCount == m      |conflictingLiveRangeId(m)     |
//                                            +------------------------------+
//                                            ....
//
//-----------------------------------------------------------------------------

void
ConflictGraph::BuildAdjacencyVector()
{
   this->InitializeAdjacencyVector();
   GraphColor::Tile *              tile = this->Tile;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::IdVector *          adjacencyVector = this->AdjacencyVector;
   unsigned                        liveRangeCount = tile->LiveRangeCount();

   unsigned  index = 0;
   for (unsigned liveRangeId1 = 1; liveRangeId1 <= liveRangeCount; liveRangeId1++)
   {
      GraphColor::LiveRange * liveRange1 = tile->GetLiveRangeById(liveRangeId1);

      liveRange1->ConflictAdjacencyIndex = index;

      for (unsigned liveRangeId2 = 1; liveRangeId2 <= liveRangeCount; liveRangeId2++)
      {
         if (bitGraph->TestEdge(liveRangeId1, liveRangeId2)) {
            assert(liveRangeId1 != liveRangeId2);

            (*adjacencyVector)[index++] = liveRangeId2;
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//   Get the first conflicting live range for a given live range
//
//-----------------------------------------------------------------------------

GraphColor::LiveRange *
ConflictGraph::GetFirstConflictLiveRange
(
   GraphColor::GraphIterator * iterator,
   GraphColor::LiveRange *     liveRange
)
{
   GraphColor::IdVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                index = liveRange->ConflictAdjacencyIndex;

   iterator->Count = liveRange->ConflictEdgeCount;
   iterator->Index = index;

   if (iterator->Count == 0) {
      return nullptr;
   }

   GraphColor::Tile *      tile = this->Tile;
   unsigned                conflictLiveRangeId = (*adjacencyVector)[index];
   GraphColor::LiveRange * conflictLiveRange = tile->GetLiveRangeById(conflictLiveRangeId);

   iterator->Index++;
   iterator->Count--;

   return conflictLiveRange;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//   Get the next conflicting live range for a given live range
//
//-----------------------------------------------------------------------------

GraphColor::LiveRange *
ConflictGraph::GetNextConflictLiveRange
(
   GraphColor::GraphIterator * iterator
)
{
   if (iterator->Count == 0) {
      return nullptr;
   }

   unsigned                index = iterator->Index;
   GraphColor::Tile *      tile = this->Tile;
   GraphColor::IdVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                conflictLiveRangeId = (*adjacencyVector)[index];
   GraphColor::LiveRange * conflictLiveRange = tile->GetLiveRangeById(conflictLiveRangeId);

   iterator->Index++;
   iterator->Count--;

   return conflictLiveRange;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Construct a new conflict graph object for this tile.
//
//-----------------------------------------------------------------------------

GraphColor::SummaryConflictGraph *
SummaryConflictGraph::New
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *            allocator = tile->Allocator;
   GraphColor::SummaryConflictGraph * conflictGraph = new GraphColor::SummaryConflictGraph();

   conflictGraph->Allocator = allocator;
   conflictGraph->Tile = tile;

   return conflictGraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//   Get the first conflicting summary live range for a given summary live range
//
//-----------------------------------------------------------------------------

GraphColor::LiveRange *
SummaryConflictGraph::GetFirstConflictSummaryLiveRange
(
   GraphColor::GraphIterator * iterator,
   GraphColor::LiveRange *     liveRange
)
{
   GraphColor::IdVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                index = liveRange->ConflictAdjacencyIndex;

   iterator->Count = liveRange->ConflictEdgeCount;
   iterator->Index = index;

   if (iterator->Count == 0) {
      return nullptr;
   }

   GraphColor::Tile *      tile = this->Tile;
   unsigned                conflictLiveRangeId = (*adjacencyVector)[index];
   GraphColor::LiveRange * conflictLiveRange = tile->GetSummaryLiveRangeById(conflictLiveRangeId);

   iterator->Index++;
   iterator->Count--;

   return conflictLiveRange;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//   Get the next conflicting summary live range for a given summary live range
//
//-----------------------------------------------------------------------------

GraphColor::LiveRange *
SummaryConflictGraph::GetNextConflictSummaryLiveRange
(
   GraphColor::GraphIterator * iterator
)
{
   if (iterator->Count == 0) {
      return nullptr;
   }

   unsigned                index = iterator->Index;
   GraphColor::Tile *      tile = this->Tile;
   GraphColor::IdVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                conflictLiveRangeId = (*adjacencyVector)[index];
   GraphColor::LiveRange * conflictLiveRange = tile->GetSummaryLiveRangeById(conflictLiveRangeId);

   iterator->Index++;
   iterator->Count--;

   return conflictLiveRange;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize the summary conflict graph bit graph
//
//-----------------------------------------------------------------------------

void
SummaryConflictGraph::InitializeBitGraph()
{
   GraphColor::Tile *              tile = this->Tile;
   int                             summaryLiveRangeCount = tile->SummaryLiveRangeCount();
   int                             globalTransparentSpillCount = tile->GlobalTransparentSpillCount;
   int                             bitGraphSize = (summaryLiveRangeCount + globalTransparentSpillCount + 1);
   BitGraphs::UndirectedBitGraph * bitGraph = BitGraphs::UndirectedBitGraph::New(bitGraphSize);

   this->BitGraph = bitGraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build the summary conflict graph bit graph
//
//-----------------------------------------------------------------------------

void
SummaryConflictGraph::BuildBitGraph()
{
   this->InitializeBitGraph();

   // Process live ranges, for every conflict between a pair of live ranges that have been mapped to 
   // summary live ranges, create a conflict between the corresponding pair of summary live ranges.

   GraphColor::Tile *              tile = this->Tile;
   Tiled::VR::Info *               vrInfo = tile->VrInfo;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::ConflictGraph *     conflictGraph = tile->ConflictGraph;

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned l = 1; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];

      unsigned summaryLiveRangeId = liveRange->SummaryLiveRangeId;
      if (summaryLiveRangeId != 0) {
         GraphColor::GraphIterator citer;
         GraphColor::LiveRange * conflictLiveRange;

         // foreach_conflict_liverange
         for (conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&citer, liveRange);
              conflictLiveRange != nullptr;
              conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&citer))
         {
            unsigned conflictSummaryLiveRangeId = conflictLiveRange->SummaryLiveRangeId;

            if (conflictSummaryLiveRangeId != 0) {
               if (summaryLiveRangeId != conflictSummaryLiveRangeId) {
                  bitGraph->AddEdge(summaryLiveRangeId, conflictSummaryLiveRangeId);
               }
            }
         }
      }
   }

   // Walk the summary live ranges and add conflicts for second chance globals

   GraphColor::LiveRangeVector * slrVector = tile->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned i = 1; i < slrVector->size(); ++i)
   {
      GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[i];

      if (summaryLiveRange->IsPhysicalRegister) {
         continue;
      }

      if (summaryLiveRange->IsSecondChanceGlobal) {

         GraphColor::Allocator * allocator = tile->Allocator;
         GraphColor::LiveRangeVector * clrVector = tile->GetSummaryLiveRangeEnumerator();

         // foreach_summaryliverange_in_tile
         for (unsigned j = 1; j < clrVector->size(); ++j)
         {
            GraphColor::LiveRange *  conflictLiveRange = (*clrVector)[j];

            if (conflictLiveRange->IsPhysicalRegister) {
               unsigned registerTag = vrInfo->GetTag(conflictLiveRange->Register);

               if (!summaryLiveRange->IsCalleeSaveValue
                  && !vrInfo->CommonMayPartialTags(registerTag, tile->KilledRegisterAliasTagSet)
                  && !vrInfo->CommonMayPartialTags(registerTag, tile->GlobalLiveInAliasTagSet)
                  && !vrInfo->CommonMayPartialTags(registerTag, tile->GlobalLiveOutAliasTagSet)) {
                  continue;
               }

               if (summaryLiveRange->IsCalleeSaveValue
                  && (summaryLiveRange->CalleeSaveValueRegister == conflictLiveRange->Register)) {
                  continue;
               }
            }

            if (conflictLiveRange->Id == summaryLiveRange->Id) {
               break;
            }

            if (conflictLiveRange->IsSecondChanceGlobal) {
               GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(summaryLiveRange);
               GraphColor::LiveRange * globalConflictLiveRange = allocator->GetGlobalLiveRange(conflictLiveRange);

               // Skip adding a tile conflict if the global conflict analysis says there is no conflict.
               if (!globalLiveRange->GlobalConflictAliasTagSet->test(globalConflictLiveRange->VrTag)) {
                  continue;
               }
            }

            if (CanConflict(tile, summaryLiveRange, conflictLiveRange)) {
               bitGraph->AddEdge(conflictLiveRange->Id, summaryLiveRange->Id);
            }
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize empty summary adjacency vector.
//
// Remarks:
//
//     Also initialize each summary live range degree and conflict edge count.
//
//-----------------------------------------------------------------------------

void
SummaryConflictGraph::InitializeAdjacencyVector()
{
   GraphColor::Tile *              tile = this->Tile;
   GraphColor::Allocator *         allocator = tile->Allocator;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   unsigned                        edgeCount = 0;
   unsigned                        summaryLiveRangeCount = tile->SummaryLiveRangeCount();

   // Add extra slots for possible reintroduction of spilled globals.
   // Extra space is spillCount * maxConfict * 2, where maxConflict is liveRange + spillCount.
   //
   // Each possible live range added for a spilled global can at most conflict with every live range and newly
   // introduced global spill live range * 2 (edge going each way)

   for (unsigned summaryLiveRangeId1 = 1; summaryLiveRangeId1 <= summaryLiveRangeCount; summaryLiveRangeId1++)
   {
      GraphColor::LiveRange * summaryLiveRange1 = tile->GetSummaryLiveRangeById(summaryLiveRangeId1);

      for (unsigned summaryLiveRangeId2 = (summaryLiveRangeId1 + 1);
           summaryLiveRangeId2 <= summaryLiveRangeCount;
           summaryLiveRangeId2++)
      {
         if (bitGraph->TestEdge(summaryLiveRangeId1, summaryLiveRangeId2)) {
            GraphColor::LiveRange * summaryLiveRange2 = tile->GetSummaryLiveRangeById(summaryLiveRangeId2);
            unsigned                degree;

            degree = allocator->CalculateDegree(summaryLiveRange1, summaryLiveRange2);
            summaryLiveRange1->Degree += degree;
            summaryLiveRange1->ConflictEdgeCount++;

            degree = allocator->CalculateDegree(summaryLiveRange2, summaryLiveRange1);
            summaryLiveRange2->Degree += degree;
            summaryLiveRange2->ConflictEdgeCount++;

            edgeCount += 2;
         }
      }
   }

   this->NodeCount = summaryLiveRangeCount;
   this->EdgeCount = edgeCount;

   unsigned size = edgeCount;

   GraphColor::IdVector * adjacencyVector = new GraphColor::IdVector(size, 0);
   this->AdjacencyVector = adjacencyVector;

   // Cache privately the initial computed size for use in the checker.

   this->AdjacencyVectorInitialSize = size;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build adjacency vector after bit graph has been built.
//
// Remarks:
//
//    Set the ConflictEdgeCount and ConflictAdjacencyIndex on summary variable.
//    Also, set the initial "Degree" on summary variable.  
//
//    AdjacencyVector layout is as follows:
//
//                                                    AdjacenceyVector
//                                                   +--------------------------------+
//    summaryLiveRange1->ConflictAdjacenceyIndex --> |conflictingSummaryLiveRangeId1  |
//                                                   |conflictingSummaryLiveRangeId2  |
//                                                   |...                             |
//    summaryLiveRange1->ConflictEdgeCount == n      |conflictingSummaryLiveRangeId(n)|
//                                                   +--------------------------------+
//    summaryLiveRange2->ConflictAdjacenceyIndex --> |conflictingSummaryLiveRangeId1  |
//                                                   |conflictingSummaryLiveRangeId2  |
//                                                   |...                             |
//    summaryLiveRange2->ConflictEdgeCount == m      |conflictingSummaryLiveRangeId(m)|
//                                                   +--------------------------------+
//                                                   ....
//
//-----------------------------------------------------------------------------

void
SummaryConflictGraph::BuildAdjacencyVector()
{
   this->InitializeAdjacencyVector();

   GraphColor::IdVector *          adjacencyVector = this->AdjacencyVector;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::Tile *              tile = this->Tile;
   unsigned                        summaryLiveRangeCount = tile->SummaryLiveRangeCount();

   // Offset index by global spill count to allow for re-injection of globals pass 2 if needed.

   unsigned  index = 0;
   for (unsigned summaryLiveRangeId1 = 1; summaryLiveRangeId1 <= summaryLiveRangeCount; summaryLiveRangeId1++)
   {
      GraphColor::LiveRange * summaryLiveRange1 = tile->GetSummaryLiveRangeById(summaryLiveRangeId1);

      summaryLiveRange1->ConflictAdjacencyIndex = index;

      for (unsigned summaryLiveRangeId2 = 1; summaryLiveRangeId2 <= summaryLiveRangeCount; summaryLiveRangeId2++)
      {
         if (bitGraph->TestEdge(summaryLiveRangeId1, summaryLiveRangeId2)) {
            assert(summaryLiveRangeId1 != summaryLiveRangeId2);

            (*adjacencyVector)[index++] = summaryLiveRangeId2;
         }
      }
   }
}

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

