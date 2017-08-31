//===-- GraphColor/Liveness.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Liveness.h"
#include "Allocator.h"
#include "LiveRange.h"
#include "SpillOptimizer.h"
#include "Tile.h"
#include "CostModel.h"

#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/Target/TargetMachine.h"

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
//    Construct a new register liveness object.
//
//-----------------------------------------------------------------------------

GraphColor::Liveness *
Liveness::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::Liveness * liveness = new GraphColor::Liveness();
   Graphs::FlowGraph *    functionUnit = allocator->FunctionUnit;
   Tiled::VR::Info *      vrInfo = functionUnit->vrInfo;

   liveness->Allocator = allocator;
   liveness->FunctionUnit = functionUnit;
   liveness->vrInfo = vrInfo;
   liveness->RegisterLivenessWalker = nullptr;
   liveness->RegisterDefinedWalker = nullptr;

   llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * genBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * killBitVector = new llvm::SparseBitVector<>();

   liveness->LiveBitVector = liveBitVector;
   liveness->GenerateBitVector = genBitVector;
   liveness->KillBitVector = killBitVector;

   std::vector<unsigned> * scratchPressureVector
      = new std::vector<unsigned>(allocator->MaximumRegisterCategoryId + 1, 0);

   liveness->ScratchPressureVector = scratchPressureVector;

   return liveness;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Delete this register liveness object.
//
//-----------------------------------------------------------------------------

void
Liveness::Delete()
{
   this->RegisterLivenessWalker->Delete();
   this->RegisterLivenessWalker = nullptr;

   if (this->RegisterDefinedWalker != nullptr) {
      this->RegisterDefinedWalker->Delete();
      this->RegisterDefinedWalker = nullptr;
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build global live ranges for a function.  
//
//-----------------------------------------------------------------------------

void
Liveness::BuildGlobalLiveRanges()
{
   llvm::SparseBitVector<> * globalLiveBitVector = Allocator->ScratchBitVector1;

   // Compute global register liveness (i.e. liveness that cross tile boundaries).
   this->ComputeGlobalRegisterLiveness(globalLiveBitVector);

   // Enumerate the global live ranges and calculate global live range weight costs.
   this->EnumerateGlobalLiveRanges(globalLiveBitVector);

   // Calculate global alias tag sets and weight for each tile.
   this->ComputeTileGlobalAliasTagSetsAndWeight();
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute global register liveness by summarizing all register liveness at 
//    all tile boundaries.
//
//-----------------------------------------------------------------------------

void
Liveness::ComputeGlobalRegisterLiveness
(
   llvm::SparseBitVector<> * globalLiveBitVector
)
{
   GraphColor::Allocator *   allocator = this->Allocator;
   GraphColor::TileGraph *   tileGraph = allocator->TileGraph;
   Dataflow::LivenessData *  registerLivenessData;
   llvm::SparseBitVector<> * doNotAllocateAliasTagSet = allocator->DoNotAllocateRegisterAliasTagBitVector;
   llvm::SparseBitVector<> * liveInBitVector;
   llvm::SparseBitVector<> * liveOutBitVector;

   globalLiveBitVector->clear();

   // Process every tile in the tile graph summarizing liveness at boundaries.

   GraphColor::TileList * postOrderTiles = tileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      // Add variables live in to this tile to the globally live set.

      Graphs::MachineBasicBlockList::iterator b;

      // foreach_BasicBlock_in_List
      for (b = tile->EntryBlockList->begin(); b != tile->EntryBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         registerLivenessData = this->GetRegisterLivenessData(block);
         liveInBitVector = registerLivenessData->LiveOutBitVector;
         *globalLiveBitVector |= *liveInBitVector;
      }

      // Add variables live out of this tile to the globally live set.

      // foreach_BasicBlock_in_List
      for (b = tile->ExitBlockList->begin(); b != tile->ExitBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         registerLivenessData = this->GetRegisterLivenessData(block);
         liveOutBitVector = registerLivenessData->LiveInBitVector;
         *globalLiveBitVector |= *liveOutBitVector;
      }

      llvm::SparseBitVector<> * globalGenerateAliasTagSet = tile->GlobalGenerateAliasTagSet;
      llvm::SparseBitVector<> * globalKillAliasTagSet = tile->GlobalKillAliasTagSet;

      assert(globalGenerateAliasTagSet->empty());
      assert(globalKillAliasTagSet->empty());

      Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;
      Graphs::MachineBasicBlockVector::iterator tb;

      // foreach_block_in_tile
      for (tb = mbbVector->begin(); tb != mbbVector->end(); ++tb)
      {
         llvm::MachineBasicBlock * block = *tb;

         registerLivenessData = this->GetRegisterLivenessData(block);
         llvm::SparseBitVector<> * generateBitVector = registerLivenessData->GenerateBitVector;
         llvm::SparseBitVector<> * killBitVector = registerLivenessData->KillBitVector;

         *globalGenerateAliasTagSet |= *generateBitVector;
         *globalKillAliasTagSet |= *killBitVector;
      }

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         *globalGenerateAliasTagSet |= *(nestedTile->GlobalGenerateAliasTagSet);
         *globalKillAliasTagSet |= *(nestedTile->GlobalKillAliasTagSet);
      }

      globalGenerateAliasTagSet->intersectWithComplement(*doNotAllocateAliasTagSet);
      globalKillAliasTagSet->intersectWithComplement(*doNotAllocateAliasTagSet);

      tile->GlobalGenerateAliasTagSet = globalGenerateAliasTagSet;
      tile->GlobalKillAliasTagSet = globalKillAliasTagSet;
   }

   globalLiveBitVector->intersectWithComplement(*doNotAllocateAliasTagSet);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute global alias tag sets and weights for every tile.
//
//-----------------------------------------------------------------------------

void
Liveness::ComputeTileGlobalAliasTagSetsAndWeight()
{
   GraphColor::Allocator *   allocator = this->Allocator;
   GraphColor::TileGraph *   tileGraph = allocator->TileGraph;
   GraphColor::LiveRange *   globalLiveRange;
   llvm::SparseBitVector<> * liveOutBitVector;
   llvm::SparseBitVector<> * liveInBitVector;
   Dataflow::LivenessData *  registerLivenessData;
   unsigned                  globalAliasTag;
   llvm::SparseBitVector<> * globalLiveBitVector = allocator->ScratchBitVector1;

   // Calculate global alias tags live in (live into tile == live out of entry blocks)

   GraphColor::TileList * postOrderTiles = tileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      llvm::SparseBitVector<> * globalAliasTagSet = tile->GlobalAliasTagSet;
      llvm::SparseBitVector<> * globalLiveInAliasTagSet = tile->GlobalLiveInAliasTagSet;
      llvm::SparseBitVector<> * globalLiveOutAliasTagSet = tile->GlobalLiveOutAliasTagSet;

      globalLiveBitVector->clear();

      Graphs::MachineBasicBlockList::iterator b;

      // foreach_BasicBlock_in_List
      for (b = tile->EntryBlockList->begin(); b != tile->EntryBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         registerLivenessData = this->GetRegisterLivenessData(block);
         liveOutBitVector = registerLivenessData->LiveOutBitVector;
         *globalLiveBitVector |= *liveOutBitVector;
      }

      llvm::SparseBitVector<>::iterator g;

      // foreach_sparse_bv_bit
      for (g = globalLiveBitVector->begin(); g != globalLiveBitVector->end(); ++g)
      {
         unsigned aliasTag = *g;

         globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);
         if (globalLiveRange == nullptr) {
            continue;
         }

         globalAliasTag = globalLiveRange->VrTag;
         globalLiveInAliasTagSet->set(globalAliasTag);
      }

      // Calculate global alias tags live out (live out of tile == live in to exit blocks)

      globalLiveBitVector->clear();

      // foreach_BasicBlock_in_List
      for (b = tile->ExitBlockList->begin(); b != tile->ExitBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         registerLivenessData = this->GetRegisterLivenessData(block);
         liveInBitVector = registerLivenessData->LiveInBitVector;
         *globalLiveBitVector |= *liveInBitVector;
      }

      llvm::SparseBitVector<>::iterator a;

      // foreach_sparse_bv_bit
      for (a = globalLiveBitVector->begin(); a != globalLiveBitVector->end(); ++a)
      {
         unsigned aliasTag = *a;

         globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);
         if (globalLiveRange == nullptr) {
            continue;
         }

         globalAliasTag = globalLiveRange->VrTag;
         globalLiveOutAliasTagSet->set(globalAliasTag);
      }

      // Calculate full set of global alias tags in tile.

      *globalAliasTagSet |= *globalLiveInAliasTagSet;
      *globalAliasTagSet |= *globalLiveOutAliasTagSet;

      // Add global alias tags live across nested tile boundaries.

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         *globalAliasTagSet |= *nestedTile->GlobalLiveInAliasTagSet;
         *globalAliasTagSet |= *nestedTile->GlobalLiveOutAliasTagSet;
      }

      // Summarize global weight cost on tile.

      Tiled::Cost tileGlobalWeightCost = allocator->ZeroCost;
      Tiled::Cost globalLiveRangeWeightCost;

      // foreach_sparse_bv_bit
      for (g = globalAliasTagSet->begin(); g != globalAliasTagSet->end(); ++g)
      {
         unsigned globalAliasTag = *g;

         globalLiveRange = allocator->GetGlobalLiveRange(globalAliasTag);
         globalLiveRangeWeightCost = globalLiveRange->WeightCost;
         tileGlobalWeightCost.IncrementBy(&globalLiveRangeWeightCost);
      }

      tile->GlobalWeightCost = tileGlobalWeightCost;
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Enumerate and construct the global live ranges for a function.  Update the global alias tag 
//    to live range map on the allocator.
//
// Arguments:
//
//    globalLiveBitVector - set of global alias tags determined live across tile boundaries
//
// Returns:
//
//    The number of live ranges constructed.
//
//-----------------------------------------------------------------------------

unsigned
Liveness::EnumerateGlobalLiveRanges
(
   llvm::SparseBitVector<> * globalLiveBitVector
)
{
   if (globalLiveBitVector->empty()) {
      assert(this->Allocator->GlobalLiveRangeCount() == 0);
      return 0;
   }

   GraphColor::Allocator *      allocator = this->Allocator;
   Tiled::VR::Info *            vrInfo = allocator->VrInfo;
   GraphColor::SpillOptimizer * spillOptimizer = allocator->SpillOptimizer;
   unsigned                     aliasTag;
   GraphColor::LiveRange *      liveRange;
   unsigned                     reg;
   llvm::SparseBitVector<> *    registerAliasTagBitVector = allocator->ScratchBitVector2;
   llvm::SparseBitVector<> *    doNotAllocateAliasTagBitVector =
      allocator->DoNotAllocateRegisterAliasTagBitVector;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = globalLiveBitVector->begin(); a != globalLiveBitVector->end(); ++a)
   {
      aliasTag = *a;

      // Extract any register tags that cross global boundaries and
      // add those live ranges since they don't overlap in the same
      // way as candidate live ranges.

      if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
         // Ensure that the parent gets initialized before the child.  If there is
         // an ordering issue and a child alias tag is smaller than the parent,
         // this can cause issues if the child register is not allocateable.

         reg = vrInfo->GetRegister(aliasTag);
         liveRange = allocator->AddGlobalRegisterLiveRange(reg);

         // <place for code supporting architectures with sub-registers>
         // foreach_may_partial_alias_of_tag(registerAliasTag, aliasTag, aliasInfo)
         //For architectures without sub=registers loop of the above type collapses to single
         //iteration (registerAliasTag = aliasTag), here the iteration can be skipped.
      }
   }

   Graphs::NodeFlowOrder * flowReversePostOrder = allocator->FlowReversePostOrder;

   // foreach_block_in_order
   for (unsigned i = 1; i <= flowReversePostOrder->NodeCount(); ++i)
   {
      llvm::MachineBasicBlock * block = flowReversePostOrder->Node(i);

      llvm::MachineInstr * instruction = nullptr;
      llvm::MachineBasicBlock::instr_iterator(ii);

      // foreach_instr_in_block
      for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
      {
         instruction = &*ii;

         // foreach_dataflow_source_and_destination_opnd => foreach_register_source_and_destination_opnd
         foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
            aliasTag = vrInfo->GetTag(operand->getReg());

            if (vrInfo->CommonMayPartialTags(aliasTag, globalLiveBitVector)) {
               reg = operand->getReg();
               liveRange = allocator->AddGlobalLiveRange(aliasTag, reg);

               if (liveRange != nullptr) {
                  Tiled::Cost weightCost = liveRange->WeightCost;
                  Tiled::Cost cost = spillOptimizer->ComputeWeight(operand);

                  weightCost.IncrementBy(&cost);
                  liveRange->WeightCost = weightCost;

                  if (operand->isUse()) {
                     liveRange->UseCount++;
                  } else {
                     liveRange->DefinitionCount++;

                     if (GraphColor::SpillOptimizer::IsCalleeSaveTransfer(instruction)) {
                        llvm::MachineOperand * sourceOperand = spillOptimizer->getSingleExplicitSourceOperand(instruction);
                        assert(Tiled::VR::Info::IsPhysicalRegister(sourceOperand->getReg()));

                        liveRange->IsCalleeSaveValue = true;
                        liveRange->CalleeSaveValueRegister = sourceOperand->getReg();
                     }
                  }

                  //liveRange->HasSymbol |= (symbol != nullptr);
                     //TODO: HasFrameSlot cannot replace it at liveness computation time (compute later?)
                  liveRange->IsTrivial = false;
               }
            }
 
            next_source_and_destination_opnd_v2(operand, instruction, end_iter);
         }
      }
   }

   // Build set of global alias tags used to denote global live ranges.

   llvm::SparseBitVector<> * globalAliasTagSet = allocator->GlobalAliasTagSet;
   GraphColor::LiveRangeEnumerator * globalLiveRanges = allocator->GetGlobalLiveRangeEnumerator();
   GraphColor::LiveRangeEnumerator::iterator lr;

   // foreach_global_liverange_in_allocator
   for (lr = globalLiveRanges->begin(), ++lr /*vector-base1*/; lr != globalLiveRanges->end(); ++lr)
   {
      GraphColor::LiveRange * globalLiveRange = *lr;

      unsigned globalAliasTag = globalLiveRange->VrTag;
      globalAliasTagSet->set(globalAliasTag);
   }

   // Return the number of live ranges constructed.
   unsigned liveRangeCount = allocator->GlobalLiveRangeCount();

   return liveRangeCount;
}


const llvm::TargetRegisterClass *
Liveness::getChildRegisterCategory
(
   const llvm::TargetRegisterClass * RC
)
{
   // for architectures with no sub-registers
   return nullptr;

   //NYI
   //an implementation should use:
   //   TargetRegisterClass::getSubClassMask(), TargetRegisterInfo::getRegClass(idx)
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Estimate block pressure
//
// Arguments:
//
//    block - Block to estimate pressure for.
//
// Notes:
//
//    Can be run before live ranges are computed - so is only an
//    estimate - allowing it to be used in tile formation.
//
// Returns:
//
//    Estimated count of overlapping live ranges.
//
//-----------------------------------------------------------------------------

#ifdef FUTURE_IMPL   // MULTITILE +  ?currently not used
void
Liveness::EstimateBlockPressure
(
   llvm::MachineBasicBlock *  block,
   std::vector<unsigned> *    registerPressureVector
)
{
   std::vector<unsigned> *  scratchPressureVector = this->ScratchPressureVector;

   std::fill(scratchPressureVector->begin(), scratchPressureVector->end(), 0);

   llvm::SparseBitVector<> *   genBitVector = this->GenerateBitVector;
   llvm::SparseBitVector<> *   killBitVector = this->KillBitVector;
   llvm::SparseBitVector<> *   liveBitVector = this->LiveBitVector;

   llvm::iterator_range<llvm::MachineBasicBlock::instr_iterator> instructionRange(block->instr_begin(), block->instr_end());

   UnionFind::Tree *  liveRangeTree = this->ComputeLiveRangeAliasTagSets(instructionRange, this->Allocator, false);

   genBitVector->clear();
   killBitVector->clear();
   liveBitVector->clear();

   Tiled::VR::Info *           vrInfo = this->vrInfo;

   llvm::MachineBasicBlock::reverse_instr_iterator rii;

   // foreach_instr_in_block_backward
   for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
   {
      llvm::MachineInstr * instruction = &(*rii);

      // Check pressure by category.

      this->TransferInstruction(instruction, genBitVector, killBitVector);

      // Decrement live destinations and adjust for found global definitions.

      llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(),
                                                                    instruction->defs().end());
      llvm::MachineInstr::mop_iterator destinationOperand;

      // foreach_register_destination_opnd
      for (destinationOperand = drange.begin(); destinationOperand != drange.end(); ++destinationOperand)
      {
         if (destinationOperand->isReg()) {
            unsigned destinationAliasTag = vrInfo->GetTag(destinationOperand->getReg());

            UnionFind::Member * rootMember = liveRangeTree->Find(destinationAliasTag);

            if (rootMember != nullptr) {
               // Root member has the largest register category.
               // Decrement register count for each category and allocatable subcategory

               const llvm::TargetRegisterClass * registerCategory
                  = static_cast<GraphColor::LiveRangeMember*>(rootMember)->RegisterCategory;

               if (!registerCategory->isAllocatable()) {
                  continue;
               }

               bool  isNotLocallyGenerated = !liveBitVector->test(destinationAliasTag);

               if (isNotLocallyGenerated) {
                  // Model the global gen at this def point so we can max correctly.

                  this->IncrementAllocatable(registerCategory, scratchPressureVector);

                  const llvm::TargetRegisterClass * currentRegisterCategory = registerCategory;

                  while (currentRegisterCategory != nullptr)
                  {
                     unsigned  registerCategoryId = currentRegisterCategory->getID();
                     unsigned  count = (*scratchPressureVector)[registerCategoryId];

                     if (count > (*registerPressureVector)[registerCategoryId]) {
                        (*registerPressureVector)[registerCategoryId] = count;
                     }

                     currentRegisterCategory = this->getChildRegisterCategory(currentRegisterCategory);
                  }
               }

               // Decrement for the kill
               this->DecrementAllocatable(registerCategory, scratchPressureVector);
            }
         }
      }

      // Remove killed bits from live set.

      *liveBitVector = (*liveBitVector - *killBitVector);

      // Increment by sources

      llvm::MachineInstr::mop_iterator sourceOperand;
      llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                  instruction->explicit_operands().end());
      // foreach_register_source_opnd
      for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
      {
         if (sourceOperand->isReg()) {
            unsigned sourceAliasTag = vrInfo->GetTag(sourceOperand->getReg());

            if (liveBitVector->test(sourceAliasTag)) {
               continue;
            }

            UnionFind::Member * rootMember = liveRangeTree->Find(sourceAliasTag);

            if (rootMember != nullptr) {
               // Root member has the largest register category.
               // Decrement register count for each category and allocatable subcategory

               const llvm::TargetRegisterClass * registerCategory
                  = static_cast<GraphColor::LiveRangeMember*>(rootMember)->RegisterCategory;

               if (!registerCategory->isAllocatable()) {
                  continue;
               }

               this->IncrementAllocatable(registerCategory, scratchPressureVector);

               while (registerCategory != nullptr)
               {
                  unsigned  registerCategoryId = registerCategory->getID();
                  unsigned  count = (*scratchPressureVector)[registerCategoryId];

                  // keep track of the max pressure count
                  if (count > (*registerPressureVector)[registerCategoryId]) {
                     (*registerPressureVector)[registerCategoryId] = count;
                  }

                  registerCategory = this->getChildRegisterCategory(registerCategory);
               }
            }
         }
      }

      // Update live with gen set.

      *liveBitVector |= *genBitVector;
   }

   delete scratchPressureVector;
   this->ScratchPressureVector = nullptr;
}
#endif

//-----------------------------------------------------------------------------
//
// Description:
//
//    Decrement allocatable register category starting at pseudo reg
//    and then the rest of the sub tree.
//
// Arguments:
//
//    pseudoReg      - Pseudo register representing the category to start
//                     the decrement at.
//    pressureVector - Pressure sum by category.
//
//-----------------------------------------------------------------------------

void
Liveness::DecrementAllocatable
(
   const llvm::TargetRegisterClass * registerCategory,
   std::vector<unsigned> *           pressureVector
)
{
   unsigned  registerCategoryId = registerCategory->getID();
   unsigned  count = (*pressureVector)[registerCategoryId];

   (*pressureVector)[registerCategoryId] = --count;

   // <place for code supporting architectures with sub-registers>

}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Increment allocatable register category starting at pseudo reg
//    and then the rest of the sub tree.
//
// Arguments:
//
//    pseudoReg      - Pseudo register representing the category to start
//                     the increment at.
//    pressureVector - Pressure sum by category.
//
//-----------------------------------------------------------------------------

void
Liveness::IncrementAllocatable
(
   const llvm::TargetRegisterClass * registerCategory,
   std::vector<unsigned> *           pressureVector
)
{
   unsigned  registerCategoryId = registerCategory->getID();
   unsigned  count = (*pressureVector)[registerCategoryId];

   (*pressureVector)[registerCategoryId] = ++count;

   // <place for code supporting architectures with sub-registers>

}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build live ranges for a tile.  
//
// Arguments:
//
//    tile - tile to build live ranges for.
//
//-----------------------------------------------------------------------------

void
Liveness::BuildLiveRanges
(
   GraphColor::Tile * tile
)
{
   int liveRangeCountBefore;
   int liveRangeCountAfter;

   // Enumerate live ranges.
   liveRangeCountBefore = this->EnumerateLiveRanges(tile);

   // If we are within a reasonable limit for performance, just return.
   if (liveRangeCountBefore < int(GraphColor::Allocator::Constants::LiveRangeLimit)) {
      return;
   }

   // Otherwise, allocate trivial live ranges to reduce the overall number of live ranges.

   GraphColor::Allocator * allocator = tile->Allocator;

   allocator->AllocateTrivialLiveRanges(tile);

   // Enumerate live ranges again.

   liveRangeCountAfter = this->EnumerateLiveRanges(tile);
   assert(liveRangeCountAfter <= liveRangeCountBefore);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Enumerate and construct the live ranges for global live ranges live at 
//    tile boundary 
//
// Arguments:
//
//    tile                - tile to build live ranges for.
//    globalLiveBitVector - liveness bit vector at boundary that is being 
//                          processed
//
// Returns:
//
//    The number of live ranges constructed.
//
//-----------------------------------------------------------------------------

unsigned
Liveness::EnumerateGlobalLiveRangesWithInTile
(
   GraphColor::Tile *        tile,
   GraphColor::Tile *        nestedTile,
   llvm::SparseBitVector<> * globalLiveBitVector
)
{
   GraphColor::Allocator *        allocator = this->Allocator;
   llvm::SparseBitVector<> *      globalSpillAliasTagSet = tile->GlobalSpillAliasTagSet;
   llvm::SparseBitVector<> *      globalIdSet = allocator->ScratchBitVector1;
   GraphColor::LiveRangeVector *  globalLiveRangeVector = allocator->GlobalLiveRangeVector;
   unsigned                       liveRangeCount = tile->LiveRangeCount();
   unsigned                       globalAliasTag;
   unsigned                       summaryAliasTag;
   GraphColor::LiveRange *        globalLiveRange;
   unsigned                       globalLiveRangeId;
   GraphColor::LiveRange *        liveRange;
   unsigned                       reg;

   // Process global liveness and create missing live ranges.

   globalIdSet->clear();

   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = globalLiveBitVector->begin(); g != globalLiveBitVector->end(); ++g)
   {
      globalAliasTag = *g;

      // Don't build live ranges for global live ranges that have been spilled in tile.
      if (globalSpillAliasTagSet->test(globalAliasTag)) {
         continue;
      }

      // If no global live range, ignore.
      if (allocator->GetGlobalLiveRange(globalAliasTag) == nullptr) {
         continue;
      }

      // Get the pre-existing global live range.
      assert(tile->GlobalAliasTagSet->test(globalAliasTag));

      globalLiveRangeId = allocator->GetGlobalLiveRangeId(globalAliasTag);
      globalIdSet->set(globalLiveRangeId);
   }

   // Compute the set of global live range IDs (above) and then create the live
   // ranges for this tile in that order (below) since iterating by alias tag
   // is not very deterministic.

   // foreach_sparse_bv_bit
   for (g = globalIdSet->begin(); g != globalIdSet->end(); ++g)
   {
      globalLiveRangeId = *g;

      globalLiveRange = (*globalLiveRangeVector)[globalLiveRangeId];
      globalAliasTag = globalLiveRange->VrTag;
      reg = globalLiveRange->Register;

      // Create local live range for global live range and mark accordingly, propagating
      // associated info.

      liveRange = tile->AddLiveRange(globalAliasTag, reg);
      liveRange->GlobalLiveRange = globalLiveRange;
      liveRange->FrameSlot = globalLiveRange->FrameSlot;
      liveRange->IsCalleeSaveValue = globalLiveRange->IsCalleeSaveValue;
      liveRange->CalleeSaveValueRegister = globalLiveRange->CalleeSaveValueRegister;

      // Keep track of number of global live ranges spanning call that pass through
      // or are referenced in this tile.

      bool wasCallSpanning = liveRange->IsCallSpanning;

      liveRange->IsCallSpanning |= globalLiveRange->IsCallSpanning;
      if (liveRange->IsCallSpanning && !wasCallSpanning) {
         tile->NumberCallSpanningLiveRanges++;
      }

      liveRange->IsTrivial = false;

      // Add summary live range from nested tile that covers this global live range.
      if (nestedTile != nullptr) {
         GraphColor::LiveRange * summaryLiveRange = nestedTile->GetSummaryLiveRange(globalAliasTag);

         if (summaryLiveRange != nullptr) {
            if (!summaryLiveRange->HasHardPreference && !summaryLiveRange->IsSecondChanceGlobal) {
               summaryAliasTag = summaryLiveRange->VrTag;
               tile->AddLiveRange(summaryAliasTag, reg);
            }
         }
      }

   }

   // Determine how many live ranges constructed during this process.
   liveRangeCount = tile->LiveRangeCount() - liveRangeCount;

   return liveRangeCount;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    This function returns the block along with 'aliasTag' is live if the block
//    is the only immediate successor of 'block' along which 'aliasTag' is live.
//
// Arguments:
//
//    block -     a basic block whose successor are going to be examined.
//    aliasTag -  the alias tag which we're going to check for liveness on 
//                block's successors
//
// Returns:
//
//    The successor block along which 'aliasTag' is live or
//    nullptr if 'aliasTag' is live into multiple successors.
//
//-----------------------------------------------------------------------------

llvm::MachineBasicBlock *
Liveness::FindSingleLiveSuccessorBlock
(
   llvm::MachineBasicBlock * block,
   unsigned                  aliasTag
)
{
   llvm::MachineBasicBlock * followBlock = nullptr;
   llvm::MachineBasicBlock::succ_iterator s;

   for (s = block->succ_begin(); s != block->succ_end(); ++s)
   {
      llvm::MachineBasicBlock * successorBlock = *s;

      //EH: if (successorEdge->IsException) {
      //       continue;
      //    }

      Dataflow::LivenessData *  successorBlockRegisterLivenessData =
         this->GetRegisterLivenessData(successorBlock);
      llvm::SparseBitVector<> * liveInBitVector = successorBlockRegisterLivenessData->LiveInBitVector;

      if (liveInBitVector->test(aliasTag)) {
         if (followBlock != nullptr) {
            // There is more than one successor block into which this alias tag is live.
            followBlock = nullptr;
            break;
         } else {
            // Found a successor block into which this alias tag is live.
            followBlock = successorBlock;
         }
      }
   }

   return followBlock;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    This function returns the first instruction in 'block' that births a
//    register.
//
// Arguments:
//
//    block - a basic block
//
// Returns:
//
//    The first instruction that does not birth a register. Nullptr if no such
//    instruction exists in the block.
//
//-----------------------------------------------------------------------------

llvm::MachineInstr *
Liveness::FindFirstRealInstruction
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineInstr * instruction = nullptr;
   llvm::MachineBasicBlock::instr_iterator(i);

   // foreach_instr_in_block
   for (i = block->instr_begin(); i != block->instr_end(); ++i)
   {
      instruction = &*i;
      if (instruction->isLabel()) {
         continue;
      }

      return instruction;
   }

   return instruction;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Enumerate and construct the live ranges for a tile.  Update the alias tag to live range map
//    on the allocator.
//
// Arguments:
//
//    tile - tile to build live ranges for.
//
// Returns:
//
//    The number of live ranges constructed.
//
//-----------------------------------------------------------------------------

unsigned
Liveness::EnumerateLiveRanges
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *       allocator = this->Allocator;
   Tiled::VR::Info *             vrInfo = this->vrInfo;
   GraphColor::SpillOptimizer *  spillOptimizer = allocator->SpillOptimizer;
   llvm::SparseBitVector<> *     allocatableRegisterAliasTagSet = tile->AllocatableRegisterAliasTagSet;
   llvm::SparseBitVector<> *     killedRegisterAliasTagSet = tile->KilledRegisterAliasTagSet;
   llvm::SparseBitVector<> *     categoryRegisterAliasTagSet;
   llvm::MachineFunction *       MF = allocator->FunctionUnit->machineFunction;
   const llvm::TargetRegisterInfo * TRI = MF->getSubtarget().getRegisterInfo();
   unsigned                      aliasTag;
   unsigned                      reg;
   GraphColor::LiveRange *       liveRange = nullptr;
   llvm::MachineOperand *        prevDestinationOperand = nullptr;
   llvm::SparseBitVector<> *     liveOutBitVector;
   llvm::SparseBitVector<> *     globalAliasTagSet = tile->GlobalAliasTagSet;
   llvm::SparseBitVector<> *     globalLiveInAliasTagSet = tile->GlobalLiveInAliasTagSet;
   llvm::SparseBitVector<> *     globalLiveOutAliasTagSet = tile->GlobalLiveOutAliasTagSet;
   llvm::SparseBitVector<> *     globalLiveBitVector;
   Dataflow::LivenessData *      registerLivenessData;
   GraphColor::LiveRange *       globalLiveRange;
   unsigned                      globalAliasTag;
   unsigned                      registerAliasTag;
   const llvm::TargetRegisterClass * registerCategory = nullptr;
   unsigned                      registerCategoryId;
   llvm::SparseBitVector<> *     processedAliasTagSet = allocator->ScratchBitVector1;
   llvm::SparseBitVector<> *     processedRegisterCategoryIdSet = allocator->ScratchBitVector2;

   // Reset (zero out) the live range map.
   allocator->ResetAliasTagToIdMap();

   // Reset the live range vector.
   tile->ResetLiveRangeVector();

   // Determine allocatable registers referenced by this tile

   processedRegisterCategoryIdSet->clear();
   processedAliasTagSet->clear();

   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;
   Graphs::MachineBasicBlockVector::iterator b;

   // foreach_block_in_tile_forward
   for (b = mbbVector->begin(); b != mbbVector->end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;

      llvm::MachineBasicBlock::instr_iterator(i);

      // foreach_instr_in_block
      for (i = block->instr_begin(); i != block->instr_end(); ++i)
      {
         llvm::MachineInstr * instruction = &(*i);

         if (Tile::IsTileBoundaryInstruction(instruction)) {

            for (unsigned operandIndex = 0; operandIndex < instruction->getNumOperands(); ++operandIndex)
            {
               llvm::MachineOperand * operand = &(instruction->getOperand(operandIndex));

               reg = operand->getReg();

               if (!(reg == VR::Constants::InitialPseudoReg || VR::Info::IsVirtualRegister(reg))) {
                  unsigned aliasTag = vrInfo->GetTag(operand->getReg());
                  assert(aliasTag != VR::Constants::InvalidTag);

                  if (operand->isDef()) {
                     if (!processedAliasTagSet->test(aliasTag)) {
                        processedAliasTagSet->set(aliasTag);

                        // Keep track of registers killed because of calls or other side-effects.
                        vrInfo->OrMayPartialTags(aliasTag, killedRegisterAliasTagSet);
                     }

                  } else if (Tile::IsEnterTileInstruction(instruction)) {
                     // Keep track of alias tags in live into nested tiles.
                     llvm::SparseBitVector<> * nestedTileAliasTagSet = tile->NestedTileAliasTagSet;

                     vrInfo->OrMayPartialTags(aliasTag, nestedTileAliasTagSet);
                  }
               }
            }

            continue;
         }

         if (instruction->isCall()) {
            if (allocator->callKilledRegBitVector->empty()) {
               unsigned vectorSize = ((TRI->getNumRegs() + 31) / 32) * 32;

               llvm::BitVector  callPreservedBitVector(vectorSize);
               callPreservedBitVector.setBitsInMask(TRI->getCallPreservedMask(*MF, llvm::CallingConv::Fast));

               llvm::BitVector  callKilledRegBitVector(vectorSize);
               callKilledRegBitVector = TRI->getAllocatableSet(*MF);
               callKilledRegBitVector.reset(Tiled::NoReg);

               callKilledRegBitVector.reset(callPreservedBitVector);  // X &= ~Y

               for (int reg = callKilledRegBitVector.find_first(); reg != -1; reg = callKilledRegBitVector.find_next(reg))
               {
                  unsigned tag = vrInfo->GetTag(reg);
                  allocator->callKilledRegBitVector->set(tag);
               }
            }

            vrInfo->OrMayPartialTags(allocator->callKilledRegBitVector, killedRegisterAliasTagSet);
         }

         // foreach_dataflow_source_and_destination_opnd => foreach_register_source_and_destination_opnd
         foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
            reg = operand->getReg();

            registerCategory = allocator->GetRegisterCategory(reg);
            if (reg == 0 || registerCategory == nullptr || !(registerCategory->isAllocatable())) {
               next_source_and_destination_opnd_v2(operand, instruction, end_iter);
               continue;
            }

            registerCategoryId = registerCategory->getID();

            if (!(reg == VR::Constants::InitialPseudoReg || VR::Info::IsVirtualRegister(reg))) {
               aliasTag = vrInfo->GetTag(reg);

               // Keep track of registers killed.
               vrInfo->OrMayPartialTags(aliasTag, killedRegisterAliasTagSet);
            }

            if (!processedRegisterCategoryIdSet->test(registerCategoryId)) {
               processedRegisterCategoryIdSet->set(registerCategoryId);

               categoryRegisterAliasTagSet = allocator->GetAllocatableRegisters(registerCategory);
               if (categoryRegisterAliasTagSet != nullptr) {
                  *allocatableRegisterAliasTagSet |= *categoryRegisterAliasTagSet;
               }
            }

            // Handle special case for S0,S1 => D0  (WinPhone -QRCE non-EABI, dead parameter issue).
            // <place for code supporting the special architecture sub-registers architecture>

            next_source_and_destination_opnd_v2(operand, instruction, end_iter);
         }
      }
   }

   // Trim register kills to only allocatable registers.

   llvm::SparseBitVector<> * doNotAllocateAliasTagSet = allocator->DoNotAllocateRegisterAliasTagBitVector;
   killedRegisterAliasTagSet->intersectWithComplement(*doNotAllocateAliasTagSet);

   // Determine allocatable registers referenced by live ranges passing through this tile.

   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = globalAliasTagSet->begin(); g != globalAliasTagSet->end(); ++g)
   {
      globalAliasTag = *g;

      globalLiveRange = allocator->GetGlobalLiveRange(globalAliasTag);
      reg = globalLiveRange->Register;

      registerCategory = globalLiveRange->GetRegisterCategory();
      registerCategoryId = registerCategory->getID();

      if (!processedRegisterCategoryIdSet->test(registerCategoryId)) {
         processedRegisterCategoryIdSet->set(registerCategoryId);

         categoryRegisterAliasTagSet = allocator->GetAllocatableRegisters(registerCategory);
         if (categoryRegisterAliasTagSet != nullptr) {
            *allocatableRegisterAliasTagSet |= *categoryRegisterAliasTagSet;
         }
      }
   }

   // Include killed registers to allocatable set since we must form live ranges for them too.
   *allocatableRegisterAliasTagSet |= *killedRegisterAliasTagSet;

   // Create live ranges for physical registers allocatable or killed in this tile.

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = allocatableRegisterAliasTagSet->begin(); r != allocatableRegisterAliasTagSet->end(); ++r)
   {
      registerAliasTag = *r;

      reg = vrInfo->GetRegister(registerAliasTag);
      liveRange = tile->AddRegisterLiveRange(reg);
   }

   // Create live ranges for global live ranges that are live in.

   this->EnumerateGlobalLiveRangesWithInTile(tile, nullptr, globalLiveInAliasTagSet);

   // Look at every register operand in the tile and ensure we have 
   // a live range that covers it.  We process the blocks in RDFO order
   // to enumerate the live ranges in forward flow order.

   processedAliasTagSet->clear();
   mbbVector = tile->BlockVector;

   // foreach_block_in_tile_forward
   for (b = mbbVector->begin(); b != mbbVector->end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;
#if defined(TILED_DEBUG_DUMPS)
      std::cout << "\nMBB#" << block->getNumber() << "   (Enumerate LRs)" << std::endl;
#endif

      bool nonEmptyBlock = !(block->empty());
      llvm::MachineInstr * firstInstruction = nonEmptyBlock? &(block->front()) : nullptr;
      llvm::MachineInstr * firstRealInstruction = nonEmptyBlock ? this->FindFirstRealInstruction(block) : nullptr;
      bool                 isRegionEntry = nonEmptyBlock ? (firstInstruction->isLabel()) : false;

      llvm::MachineBasicBlock::instr_iterator(i);

      // foreach_instr_in_block
      for (i = block->instr_begin(); i != block->instr_end(); ++i)
      {
         llvm::MachineInstr * instruction = &(*i);
#if defined(TILED_DEBUG_DUMPS)
         instruction->dump();
#endif

         // Keep track of tile boundaries.
         bool isTileBoundaryInstruction = GraphColor::Tile::IsTileBoundaryInstruction(instruction);

         // Create live ranges for global live ranges that are live across nested tile boundary.

         if (isTileBoundaryInstruction) {
            unsigned           tileId = allocator->GetTileId(instruction);
            GraphColor::Tile * nestedTile = allocator->TileGraph->GetTileByTileId(tileId);

            if (Tile::IsEnterTileInstruction(instruction)) {
               globalLiveBitVector = nestedTile->GlobalLiveInAliasTagSet;
            } else {
               assert(Tile::IsExitTileInstruction(instruction));
               globalLiveBitVector = nestedTile->GlobalLiveOutAliasTagSet;
            }
            this->EnumerateGlobalLiveRangesWithInTile(tile, nestedTile, globalLiveBitVector);
         }

         // Process each register operand and create live ranges, keep track of use and def information.

         // foreach_dataflow_source_and_destination_opnd => foreach_register_source_and_destination_opnd
         foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
            reg = operand->getReg();
            aliasTag = vrInfo->GetTag(reg);
            liveRange = tile->AddLiveRange(aliasTag, reg);

            if (liveRange != nullptr) {
               // Keep track of use/def information, but don't include tile boundary instructions because
               // they are summary nodes, not actual uses or defs.

               if (!isTileBoundaryInstruction) {

                  if (operand->isUse()) {
                     liveRange->UseCount++;
                  } else {

                     if (!(Tiled::VR::Info::IsPhysicalRegister(reg))) {
                        if (SpillOptimizer::IsCalleeSaveTransfer(instruction)) {
                           llvm::MachineOperand * sourceOperand = spillOptimizer->getSingleExplicitSourceOperand(instruction);
                           assert(Tiled::VR::Info::IsPhysicalRegister(sourceOperand->getReg()));

                           liveRange->IsCalleeSaveValue = true;
                           liveRange->CalleeSaveValueRegister = sourceOperand->getReg();
                        }
                     }

                     liveRange->DefinitionCount++;
                  }

                  //liveRange->HasSymbol |= (symbol != nullptr);
                     //TODO: HasFrameSlot cannot replace it at liveness computation time (compute later?)
               }
            }

            next_source_and_destination_opnd_v2(operand, instruction, end_iter);
         }


         // Mark trivial live ranges.
         //
         // Simple:
         //
         // t1 =
         //    = op t1, ...
         //
         // Compound:
         //
         // t1 =
         // t1 = op t1, ...
         // t1 = op t1, ...
         //    = t1

         GraphColor::LiveRange *  trivialLiveRange = nullptr;
         int sourceOperandCnt = 0;
         llvm::MachineInstr::mop_iterator sourceOperand;
         llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                     instruction->explicit_operands().end());
         // foreach_register_source_opnd
         for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
         {
            if (!sourceOperand->isReg()) {
               continue;
            }
            sourceOperandCnt++;;

            unsigned                sourceAliasTag = vrInfo->GetTag(sourceOperand->getReg());
            GraphColor::LiveRange * sourceLiveRange = tile->GetLiveRange(sourceAliasTag);

            if (sourceLiveRange != nullptr) {

               if ((trivialLiveRange == sourceLiveRange) && (sourceOperandCnt == 1)) {
                  unsigned  prevDestinationAliasTag = vrInfo->GetTag(prevDestinationOperand->getReg());

                  if (vrInfo->MustTotallyOverlap(prevDestinationAliasTag, sourceAliasTag)) {
                     trivialLiveRange->IsTrivial = (!trivialLiveRange->WasAnalyzed);
                  } else {
                     trivialLiveRange->IsTrivial = false;
                     trivialLiveRange->WasAnalyzed = true;
                  }

               } else {
                  // If this instruction is the first instruction in the block.

                  if (!isRegionEntry && (instruction == firstRealInstruction)
                     /* && !(allocator->IsX87SupportEnabled && sourceLiveRange->Type->IsFloat)*/ ) {
                     // If this block has a single predecessor then we may be 
                     // able to mark this live range as trivial.

                     if (block->pred_size() == 1) {
                        llvm::MachineBasicBlock * predecessorBlock = *(block->pred_begin());

                        llvm::MachineBasicBlock * followerBlock =
                           this->FindSingleLiveSuccessorBlock(predecessorBlock, sourceAliasTag);

                        // If the sourceAliasTag is live-out only on one of the 
                        // arms of the split and that arm is the same as that 
                        // of this source then we may be able to mark the 
                        // live-range as trivial.
                        if ((followerBlock != nullptr) && (followerBlock == block)) {
                           // Look backwards in the predecessor block for a 
                           // definition of 'sourceAliasTag'. If we find the 
                           // instruction that defines the 'sourceAliasTag' AND
                           // there were no other instructions after it that
                           // defined any register from the same register category, 
                           // then we can mark this live range as trivial.

                           bool isTrivial = false;

                           llvm::MachineBasicBlock::reverse_instr_iterator rii;

                           // foreach_instr_in_block_backward_editing
                           for (rii = predecessorBlock->instr_rbegin(); rii != predecessorBlock->instr_rend(); ++rii)
                           {
                              llvm::MachineInstr * predecessorInstruction = &(*rii);

                              // The only instructions that we want to skip past are branch instructions.
                              // Nothing should be marked as trivial across region-exits.
                              if (predecessorInstruction->isBranch()
                                 /*TBD: && !predecessorInstruction->AsBranchInstruction->IsRegionExit*/ ) {
                                 continue;
                              }

                              llvm::MachineInstr::mop_iterator predecessorOperand;
                              llvm::iterator_range<llvm::MachineInstr::mop_iterator> opnd_range(predecessorInstruction->defs().begin(),
                                                                                                predecessorInstruction->defs().end() );

                              // foreach_dataflow_destination_opnd  (previously, the 'dataflow' stood for register AND memory operands)
                              for (predecessorOperand = opnd_range.begin(); predecessorOperand != opnd_range.end(); ++predecessorOperand)
                              {
                                 if (predecessorOperand->isReg()) {
                                    // We want the predecessor instruction to define ONLY this live-range
                                    // and nothing else. Which is why there are no breaks in this loop.

                                    unsigned tag = vrInfo->GetTag(predecessorOperand->getReg());
                                    if (vrInfo->MustTotallyOverlap(tag, sourceLiveRange->VrTag)) {
                                       isTrivial = true;
                                    } else {
                                       // The other definitions on the instruction are to other register 
                                       // categories so ignore them.

                                       unsigned predReg = predecessorOperand->getReg();
                                       const llvm::TargetRegisterClass * operandRegisterCategory = allocator->GetBaseRegisterCategory(predReg);
                                       const llvm::TargetRegisterClass * liveRangeRegisterCategory = allocator->GetRegisterCategory(sourceOperand->getReg());
                                       //was: sourceLiveRange->RegisterCategory->BaseRegisterCategory

                                       // in a single register-type architecture the below condition will always be true
                                       if (operandRegisterCategory == liveRangeRegisterCategory) {
                                          isTrivial = false;
                                       }
                                    }
                                 }
                              }

                              break;
                           }

                           if (isTrivial) {
                              sourceLiveRange->IsTrivial = (!sourceLiveRange->WasAnalyzed);
                              sourceLiveRange->WasAnalyzed = true;

                              continue;
                           }
                        }
                     }
                  }

                  sourceLiveRange->IsTrivial = false;
                  sourceLiveRange->WasAnalyzed = true;
               }
            }
         }

         GraphColor::LiveRange * destinationLiveRange = nullptr;
         llvm::MachineOperand *  destinationOperand = nullptr;
         if (instruction->defs().begin() != instruction->defs().end()) {
            destinationOperand = instruction->defs().begin();
         }

         if (destinationOperand != nullptr) {
            if (destinationOperand->isReg()) {
               unsigned tag = vrInfo->GetTag(destinationOperand->getReg());
               destinationLiveRange = tile->GetLiveRange(tag);
               if (destinationLiveRange != nullptr) {
                  destinationLiveRange->IsTrivial = (!destinationLiveRange->WasAnalyzed);
               }
            }
         }

         if (trivialLiveRange == nullptr) {
            trivialLiveRange = destinationLiveRange;
            prevDestinationOperand = destinationOperand;
         } else if (trivialLiveRange == destinationLiveRange) {
            prevDestinationOperand = destinationOperand;
         } else {
            bool doTerminateTrivialLiveRange = false;

            if (destinationLiveRange == nullptr) {
               doTerminateTrivialLiveRange = true;
            } else if (!destinationLiveRange->IsPreColored) {
               doTerminateTrivialLiveRange = true;
            } else if (instruction->isCall()) {
               doTerminateTrivialLiveRange = true;
            } else if (Tile::IsEnterTileInstruction(instruction)) {
               doTerminateTrivialLiveRange = true;
            }

            if (doTerminateTrivialLiveRange) {
               trivialLiveRange->WasAnalyzed = true;
               trivialLiveRange = destinationLiveRange;
               prevDestinationOperand = destinationOperand;
            }
         }

      }  //next_instr_in_block


      // Mark any live ranges spanning blocks as not trivial.

      registerLivenessData = this->GetRegisterLivenessData(block);
      liveOutBitVector = registerLivenessData->LiveOutBitVector;

      llvm::SparseBitVector<>::iterator a;

      // foreach_sparse_bv_bit
      for (a = liveOutBitVector->begin(); a != liveOutBitVector->end(); ++a)
      {
         aliasTag = *a;

         liveRange = tile->GetLiveRange(aliasTag);
         if (liveRange != nullptr) {
            liveRange->IsTrivial = false;
         }
      }
     // end under: if (!useFastPath)

   }

   // Create live ranges for global live ranges that are live out.

   this->EnumerateGlobalLiveRangesWithInTile(tile, nullptr, globalLiveOutAliasTagSet);

   // Propagate live range info from summary variables of nested tiles.

   GraphColor::AllocatorCostModel * costModel = tile->CostModel;

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      GraphColor::LiveRangeVector * slrVector = nestedTile->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned i = 1; i < slrVector->size(); ++i)
      {
         GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[i];

         aliasTag = summaryLiveRange->VrTag;
         liveRange = tile->GetLiveRange(aliasTag);

         if (liveRange != nullptr) {
            //liveRange->HasByteReference |= summaryLiveRange->HasByteReference;

            if (summaryLiveRange->HasUse()) {
               liveRange->UseCount++;
            }

            if (summaryLiveRange->HasDefinition()) {
               liveRange->DefinitionCount++;
            }

            bool wasCallSpanning = liveRange->IsCallSpanning;

            liveRange->IsCallSpanning |= summaryLiveRange->IsCallSpanning;
            if (liveRange->IsCallSpanning && !wasCallSpanning) {
               tile->NumberCallSpanningLiveRanges++;
            }

            liveRange->IsCalleeSaveValue |= summaryLiveRange->IsCalleeSaveValue;
            liveRange->CalleeSaveValueRegister = summaryLiveRange->CalleeSaveValueRegister;

            // Mark summary proxies so we can cost them correctly - they must get registers.

            liveRange->IsSummaryProxy = true;
            liveRange->IsTrivial = false;
            liveRange->WasAnalyzed = true;

            liveRange->SummaryRegister = summaryLiveRange->Register;

            Tiled::Cost summaryCost = summaryLiveRange->WeightCost;
            Tiled::Cost zeroCost = allocator->ZeroCost;

            if (costModel->Compare(&summaryCost, &zeroCost) <= 0) {
               summaryCost = allocator->UnitCost;
            }

            liveRange->WeightCost = summaryCost;
         }
      }
   }

   // Mark block local live ranges.

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned idx = 1 /*vector-base1*/; idx < lrVector->size(); ++idx)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[idx];

      liveRange->IsBlockLocal = true;
   }

   llvm::SparseBitVector<> * nonBlockLocalAliasTagSet = tile->NonBlockLocalAliasTagSet;
   unsigned                  nonBlockLocalAliasTag;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = nonBlockLocalAliasTagSet->begin(); a != nonBlockLocalAliasTagSet->end(); ++a)
   {
      nonBlockLocalAliasTag = *a;

      liveRange = tile->GetLiveRange(nonBlockLocalAliasTag);
      if (liveRange != nullptr) {
         liveRange->IsBlockLocal = false;
      }
   }

   // Return the number of live ranges constructed.

   unsigned liveRangeCount = tile->LiveRangeCount();

   return liveRangeCount;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute register liveness for the entire function.
//
// Arguments:
//
//    functionUnit - function unit to compute register liveness on
//
//-----------------------------------------------------------------------------

void
Liveness::ComputeRegisterLiveness
(
   Graphs::FlowGraph * functionUnit
)
{
   assert(functionUnit == this->FunctionUnit);

   Dataflow::RegisterLivenessWalker * registerLivenessWalker;

   registerLivenessWalker = Dataflow::RegisterLivenessWalker::New();
   this->RegisterLivenessWalker = registerLivenessWalker;

   registerLivenessWalker->ComputeLiveness(functionUnit);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute register liveness for a tile.
//
// Arguments:
//
//    tile - tile to compute register liveness on
//
//-----------------------------------------------------------------------------

void
Liveness::ComputeRegisterLiveness
(
   GraphColor::Tile * tile
)
{
   // First time through, we already have liveness for the entire function.

   // if (tile->Iteration > 1)
   {
      // Clear all the liveness first. 

      Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;
      Graphs::MachineBasicBlockVector::iterator tb;

      // foreach_block_in_tile
      for (tb = mbbVector->begin(); tb != mbbVector->end(); ++tb)
      {
         llvm::MachineBasicBlock * block = *tb;
         Dataflow::LivenessData * livenessData = this->GetRegisterLivenessData(block);

         livenessData->GenerateBitVector->clear();
         livenessData->KillBitVector->clear();
         livenessData->LiveInBitVector->clear();
         livenessData->LiveOutBitVector->clear();

#if defined(TILED_DEBUG_DUMPS)
         block->dump();
#endif
      }

      // Initialize nested tile boundaries accounting for spills.

      llvm::SparseBitVector<> * globalSpillBitVector = tile->GlobalSpillAliasTagSet;
      llvm::SparseBitVector<> * globalLiveBitVector = Allocator->ScratchBitVector1;
      llvm::SparseBitVector<> * callerSaveBitVector = tile->CallerSaveAliasTagSet;
      llvm::SparseBitVector<> * originalGlobalLiveBitVector;
      Tiled::VR::Info *         vrInfo = Allocator->VrInfo;
      unsigned                  globalSpillTag;

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         Graphs::MachineBasicBlockList::iterator bi;

         // foreach_tile_entry_block
         for (bi = nestedTile->EntryBlockList->begin(); bi != nestedTile->EntryBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * block = *bi;

            llvm::MachineBasicBlock::succ_iterator s;

            // foreach_block_succ_block
            for (s = block->succ_begin(); s != block->succ_end(); ++s)
            {
               llvm::MachineBasicBlock * successorBlock = *s;

               Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(successorBlock);
               llvm::SparseBitVector<> * liveInBitVector = livenessData->LiveInBitVector;

               // Initialize boundary block live in.
               liveInBitVector->clear();
            }
         }


         // foreach_tile_exit_block
         for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * block = *bi;

            llvm::MachineBasicBlock::pred_iterator p;

            // foreach_block_pred_block
            for (p = block->pred_begin(); p != block->pred_end(); ++p)
            {
               llvm::MachineBasicBlock * predecessorBlock = *p;

               Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(predecessorBlock);
               llvm::SparseBitVector<> * liveOutBitVector = livenessData->LiveOutBitVector;

               // Initialize boundary block live out.
               liveOutBitVector->clear();
            }
         }
      }

      Tiled::VR::Info *  aliasInfo = this->Allocator->VrInfo;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         Graphs::MachineBasicBlockList::iterator bi;

         // foreach_tile_entry_block
         for (bi = nestedTile->EntryBlockList->begin(); bi != nestedTile->EntryBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * block = *bi;

            originalGlobalLiveBitVector = this->GetGlobalAliasTagSet(block);
            *globalLiveBitVector = *originalGlobalLiveBitVector;

            llvm::SparseBitVector<>::iterator a;

            // foreach_sparse_bv_bit
            for (a = globalSpillBitVector->begin(); a != globalSpillBitVector->end(); ++a)
            {
               unsigned globalSpillTag = *a;

               aliasInfo->MinusMayPartialTags(globalSpillTag, globalLiveBitVector);
            }

            // foreach_sparse_bv_bit
            for (a = callerSaveBitVector->begin(); a != callerSaveBitVector->end(); ++a)
            {
               unsigned callerSaveAliasTag = *a;

               if (!nestedTile->GlobalAliasTagSet->test(callerSaveAliasTag)) {
                  continue;
               }

               // CallerSaveBitVector holds the caller save tags for the current tile.  These will
               // have been spilled around any nested tiles where they in turn were spilled.
               // Check the nested tile and if spilled clear the alias tag from the live in set.

               GraphColor::LiveRange * nestedLiveRange = nestedTile->GetSummaryLiveRange(callerSaveAliasTag);

               if (nestedLiveRange == nullptr) {
                  aliasInfo->MinusMayPartialTags(callerSaveAliasTag, globalLiveBitVector);

                  // Since caller save cuts liveness of the original
                  // global due to removal of the global on both sides
                  // of the tile boundary we need to remove it from
                  // the original liveness.

                  aliasInfo->MinusMayPartialTags(callerSaveAliasTag, originalGlobalLiveBitVector);
               }
            }

            llvm::MachineBasicBlock::succ_iterator s;

            // foreach_block_succ_block
            for (s = block->succ_begin(); s != block->succ_end(); ++s)
            {
               llvm::MachineBasicBlock * successorBlock = *s;

               Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(successorBlock);
               llvm::SparseBitVector<> * liveInBitVector = livenessData->LiveInBitVector;

               // Sum up boundary block live in.
               *liveInBitVector |= *globalLiveBitVector;
            }
         }

         // foreach_tile_exit_block
         for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * block = *bi;

            originalGlobalLiveBitVector = this->GetGlobalAliasTagSet(block);
            *globalLiveBitVector = *originalGlobalLiveBitVector;

            llvm::SparseBitVector<>::iterator a;

            // foreach_sparse_bv_bit
            for (a = globalSpillBitVector->begin(); a != globalSpillBitVector->end(); ++a)
            {
               unsigned globalSpillTag = *a;

               aliasInfo->MinusMayPartialTags(globalSpillTag, globalLiveBitVector);
            }

            // foreach_sparse_bv_bit
            for (a = callerSaveBitVector->begin(); a != callerSaveBitVector->end(); ++a)
            {
               unsigned callerSaveAliasTag = *a;

               if (!nestedTile->GlobalAliasTagSet->test(callerSaveAliasTag)) {
                  continue;
               }

               // CallerSaveBitVector holds the caller save tags for the current tile.  These will
               // have been spilled around any nested tiles where they in turn were spilled.
               // Check the nested tile and if spilled clear the alias tag from the live in set.

               GraphColor::LiveRange * nestedLiveRange = nestedTile->GetSummaryLiveRange(callerSaveAliasTag);

               if (nestedLiveRange == nullptr) {
                  aliasInfo->MinusMayPartialTags(callerSaveAliasTag, globalLiveBitVector);

                  // Since caller save cuts liveness of the original
                  // global due to removal of the global on both sides
                  // of the tile boundary we need to remove it from
                  // the original liveness.

                  aliasInfo->MinusMayPartialTags(callerSaveAliasTag, originalGlobalLiveBitVector);
               }
            }

            llvm::MachineBasicBlock::pred_iterator p;

            // foreach_block_pred_block
            for (p = block->pred_begin(); p != block->pred_end(); ++p)
            {
               llvm::MachineBasicBlock * predecessorBlock = *p;

               Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(predecessorBlock);
               llvm::SparseBitVector<> * liveOutBitVector = livenessData->LiveOutBitVector;

               // Sum up boundary block live out.

               *liveOutBitVector |= *globalLiveBitVector;
            }
         }
      }

      // Modify tile boundaries for folds that have occurred in tile.

      llvm::SparseBitVector<> * foldAliasTagSet = tile->FoldAliasTagSet;
      llvm::SparseBitVector<> * recalculateAliasTagSet = tile->RecalculateAliasTagSet;

      if (!foldAliasTagSet->empty() || !recalculateAliasTagSet->empty()) {

         Graphs::MachineBasicBlockList::iterator b;

         // foreach_BasicBlock_in_List
         for (b = tile->ExitBlockList->begin(); b != tile->ExitBlockList->end(); ++b)
         {
            llvm::MachineBasicBlock * block = *b;
            Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(block);
            llvm::SparseBitVector<> * liveInBitVector = livenessData->LiveInBitVector;

            *globalLiveBitVector = *liveInBitVector;

            unsigned recalculateSpillTag;

            llvm::SparseBitVector<>::iterator r, f;

            // foreach_sparse_bv_bit
            for (r = recalculateAliasTagSet->begin(); r != recalculateAliasTagSet->end(); ++r)
            {
               recalculateSpillTag = *r;
               vrInfo->MinusMayPartialTags(recalculateSpillTag, globalLiveBitVector);
            }

            unsigned foldSpillTag;

            // foreach_sparse_bv_bit
            for (f = foldAliasTagSet->begin(); f != foldAliasTagSet->end(); ++f)
            {
               foldSpillTag = *f;
               vrInfo->MinusMayPartialTags(foldSpillTag, globalLiveBitVector);
            }

            *liveInBitVector = *globalLiveBitVector;
         }
      }

      // Call the liveness walker again on tile blocks.

      Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->RegisterLivenessWalker;
      mbbVector = tile->BlockVector;

      registerLivenessWalker->Traverse(Dataflow::TraversalKind::Iterative, mbbVector);

      // Calculate non local alias tag set.

      llvm::SparseBitVector<> * nonBlockLocalAliasTagSet = tile->NonBlockLocalAliasTagSet;

      // foreach_block_in_tile
      for (tb = mbbVector->begin(); tb != mbbVector->end(); ++tb)
      {
         llvm::MachineBasicBlock * block = *tb;

         Dataflow::LivenessData * livenessData = this->GetRegisterLivenessData(block);

         llvm::SparseBitVector<> * liveInBitVector = livenessData->LiveInBitVector;
         llvm::SparseBitVector<> * liveOutBitVector = livenessData->LiveOutBitVector;

         *nonBlockLocalAliasTagSet |= *liveInBitVector;
         *nonBlockLocalAliasTagSet |= *liveOutBitVector;
      }

   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute register liveness at a given instruction.
//
// Arguments:
//
//    instruction     - Instruction to find liveness at.
//    block           - enclosing basic block.
//    outputBitVector - out vector for live alias tags.
//
//  Returns:
//
//    outputBitVector with live alias tags at passed instruction.
//
//-----------------------------------------------------------------------------

llvm::SparseBitVector<> *
Liveness::ComputeRegisterLiveness
(
   llvm::MachineInstr *      instruction,
   llvm::MachineBasicBlock * block,
   llvm::SparseBitVector<> * outputBitVector
)
{
   llvm::MachineInstr *      lastInstruction = &(block->instr_back());
   Dataflow::LivenessData *  livenessData = this->GetRegisterLivenessData(block);
   llvm::SparseBitVector<> * liveOutBitVector = livenessData->LiveOutBitVector;

   if (instruction == lastInstruction) {
      *outputBitVector = *liveOutBitVector;
   } else {
      llvm::SparseBitVector<> * liveBitVector = this->LiveBitVector;
      llvm::SparseBitVector<> * generateBitVector = this->GenerateBitVector;
      llvm::SparseBitVector<> * killBitVector = this->KillBitVector;

      *liveBitVector = *liveOutBitVector;
      generateBitVector->clear();
      killBitVector->clear();

      llvm::MachineBasicBlock::reverse_instr_iterator rii;
      llvm::MachineBasicBlock::reverse_instr_iterator iter_last;
      if (instruction == nullptr || instruction == &(block->front())) {
         iter_last = block->instr_rend();
      }
      else {
         iter_last = static_cast<llvm::MachineBasicBlock::reverse_instr_iterator>(instruction);
         ++iter_last;
      }

      // foreach_instr_in_range_backward
      for (rii = block->instr_rbegin(); rii != iter_last; ++rii)
      {
         llvm::MachineInstr * currentInstruction = &(*rii);
         assert(currentInstruction->getParent() == block);

         this->TransferInstruction(currentInstruction, generateBitVector, killBitVector);

        // <place for EH-flow-related code (DanglingInstruction)>, else
         if (currentInstruction != instruction) {
            this->UpdateInstruction(currentInstruction, liveBitVector, generateBitVector, killBitVector);
         } else {
            *liveBitVector = *liveBitVector - *killBitVector;
         }
      }

      *outputBitVector = *liveBitVector;
   }

   return outputBitVector;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute defined registers for the function unit.
//
// Arguments:
//
//    functionUnit - function to process.
//
//-----------------------------------------------------------------------------

void
Liveness::ComputeDefinedRegisters
(
   Graphs::FlowGraph * functionUnit
)
{
   // Determine if register liveness pruning is required.

   GraphColor::Allocator *            allocator = this->Allocator;
   Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->RegisterLivenessWalker;
   llvm::MachineFunction *            MF = functionUnit->machineFunction;
   llvm::MachineBasicBlock *          firstBlock(&(*(MF->begin())));
   const llvm::TargetRegisterInfo *   TRI = MF->getSubtarget().getRegisterInfo();

   Dataflow::LivenessData * firstBlockLive =
      static_cast<Dataflow::LivenessData *>(registerLivenessWalker->GetBlockData(firstBlock));  //dynamic_cast needed if > 1 derived class
   assert(firstBlockLive);

   llvm::SparseBitVector<> * undefinedRegisterAliasTagBitVector = allocator->UndefinedRegisterAliasTagBitVector;

   *undefinedRegisterAliasTagBitVector = *(firstBlockLive->LiveInBitVector);

   // Make sure we don't look at the frame tag as upward exposed.

   Tiled::VR::Info * vrInfo = functionUnit->vrInfo;
   unsigned        frameTag = vrInfo->GetTag(TRI->getFrameRegister(*MF));

   undefinedRegisterAliasTagBitVector->reset(frameTag);

   // Determine if we have any undefined registers live into the function.
   Tiled::Boolean isPruningRequired = !undefinedRegisterAliasTagBitVector->empty();

   // Early out if we don't have any undefined at the entry.
   if (!isPruningRequired) {
      return;
   }

   // Create new defined register walker.
   assert(this->RegisterDefinedWalker == nullptr);

   Dataflow::RegisterDefinedWalker * registerDefinedWalker =
      Dataflow::RegisterDefinedWalker::New();

   this->RegisterDefinedWalker = registerDefinedWalker;

   // Provide liveness walker in order to prune defined register sets as we go.
   registerDefinedWalker->LivenessWalker = registerLivenessWalker;

   // Compute defined registers
   registerDefinedWalker->Initialize(Dataflow::Direction::Forward, functionUnit);
   registerDefinedWalker->Traverse(Dataflow::TraversalKind::Iterative, functionUnit);

   // Mark liveness as requiring pruning.
   this->IsRegisterLivenessPruningRequired = true;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Prune register liveness for the tile (if required)
//
// Arguments:
//
//    liveBitVector - live register bit vector.
//    definedBitVector - defined register bit vector.
//
//-----------------------------------------------------------------------------

void
Liveness::PruneRegisterLiveness
(
   llvm::SparseBitVector<> * liveBitVector,
   llvm::SparseBitVector<> * definedBitVector
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   llvm::SparseBitVector<> *  undefinedBitVector = allocator->ScratchBitVector1;
   llvm::SparseBitVector<> *  undefinedRegisterAliasTagBitVector = allocator->UndefinedRegisterAliasTagBitVector;

   // Get copy of undefined on function entry (this is the maximal prune set).

   *undefinedBitVector = *undefinedRegisterAliasTagBitVector;

   // Limit pruning to only registers found to be undefined on function entry and
   // not defined subsequently.

   llvm::SparseBitVector<>::iterator iter;

   // foreach_sparse_bv_bit
   for (iter = definedBitVector->begin(); iter != definedBitVector->end(); ++iter)
   {
      // Count as defined any partial over lap from the defined set.
      // This handles the partial or piecewise def case.
      unsigned definedTag = *iter;

      vrInfo->MinusMayPartialTags(definedTag, undefinedBitVector);
   }

   // Prune live in vector by removing registers with no reaching definition.

   *liveBitVector = *liveBitVector - *undefinedBitVector;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Prune register liveness for the tile (if required)
//
// Arguments:
//
//    tile - tile to prune.
//
//-----------------------------------------------------------------------------

void
Liveness::PruneRegisterLiveness
(
   GraphColor::Tile * tile
)
{
   // If no pruning required, skip it.
   if (!this->IsRegisterLivenessPruningRequired) {
      return;
   }

   assert(this->RegisterDefinedWalker != nullptr);

   // Prune register liveness based on register definition information.
   Graphs::MachineBasicBlockVector *  mbbVector = tile->BlockVector;
   Graphs::MachineBasicBlockVector::iterator tb;

   // foreach_block_in_tile
   for (tb = mbbVector->begin(); tb != mbbVector->end(); ++tb)
   {
      llvm::MachineBasicBlock * block = *tb;

      Dataflow::DefinedData *  blockDefined   = this->GetRegisterDefinedData(block);
      Dataflow::LivenessData * blockLiveness = this->GetRegisterLivenessData(block);

      llvm::SparseBitVector<> * liveInBitVector = blockLiveness->LiveInBitVector;
      llvm::SparseBitVector<> * liveOutBitVector = blockLiveness->LiveOutBitVector;
      llvm::SparseBitVector<> * definedInBitVector = blockDefined->InBitVector;
      llvm::SparseBitVector<> * definedOutBitVector = blockDefined->OutBitVector;

      this->PruneRegisterLiveness(liveInBitVector, definedInBitVector);
      this->PruneRegisterLiveness(liveOutBitVector, definedOutBitVector);
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Prune register liveness for the function unit. (if required)
//
// Arguments:
//
//    functionUnit - function to prune.
//
//-----------------------------------------------------------------------------

void
Liveness::PruneRegisterLiveness
(
   Graphs::FlowGraph * functionUnit
)
{
   assert(functionUnit != nullptr);
   (functionUnit);

   // If no pruning required, skip it.

   if (!this->IsRegisterLivenessPruningRequired)
      return;

   assert(this->RegisterDefinedWalker != nullptr);

   // Prune register liveness based on register definition information.

   GraphColor::Allocator * allocator = this->Allocator;
   Graphs::NodeFlowOrder * flowReversePostOrder = allocator->FlowReversePostOrder;

   // foreach_block_in_order
   for (unsigned i = 1; i <= flowReversePostOrder->NodeCount(); ++i)
   {
      llvm::MachineBasicBlock * block = flowReversePostOrder->Node(i);

      Dataflow::DefinedData *  blockDefined   = this->GetRegisterDefinedData(block);
      Dataflow::LivenessData * blockLiveness = this->GetRegisterLivenessData(block);

      llvm::SparseBitVector<> * liveInBitVector = blockLiveness->LiveInBitVector;
      llvm::SparseBitVector<> * liveOutBitVector = blockLiveness->LiveOutBitVector;
      llvm::SparseBitVector<> * definedInBitVector = blockDefined->InBitVector;
      llvm::SparseBitVector<> * definedOutBitVector = blockDefined->OutBitVector;

      this->PruneRegisterLiveness(liveInBitVector, definedInBitVector);
      this->PruneRegisterLiveness(liveOutBitVector, definedOutBitVector);
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get global alias tag set associated with a block (original liveness of
//    global variables before optimization/allocation).
//
// Arguments:
//
//    block - block from flow graph
//
// Returns:
//
//    bit vector containing global alias tags live at across block boundary.
//
//-----------------------------------------------------------------------------

//TBD: define a separate class w/ static GetExtensionObject, or just put a map in Allocator?

llvm::SparseBitVector<> *
Liveness::GetGlobalAliasTagSet
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileExtensionObject * tileExtensionObject = TileExtensionObject::GetExtensionObject(block);
   assert(tileExtensionObject != nullptr);

   llvm::SparseBitVector<> * globalAliasTagSet = tileExtensionObject->GlobalAliasTagSet;

   return globalAliasTagSet;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get register defined data for a given block.
//
// Arguments:
//
//    block - block from flow graph
//
// Returns:
//
//    Register liveness data.
//
//-----------------------------------------------------------------------------

Dataflow::DefinedData *
Liveness::GetRegisterDefinedData
(
   llvm::MachineBasicBlock * block
)
{
   Dataflow::DefinedData * definedData;

   definedData = static_cast<Dataflow::DefinedData *>(this->RegisterDefinedWalker->GetBlockData(block->getNumber()));  //dynamic_cast needed if > 1 derived class
   assert(definedData || this->RegisterDefinedWalker->GetBlockData(block->getNumber()) == nullptr);

   return definedData;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get register liveness data for a given block.
//
// Arguments:
//
//    block - block from flow graph
//
// Returns:
//
//    Register liveness data.
//
//-----------------------------------------------------------------------------

Tiled::Dataflow::LivenessData *
Liveness::GetRegisterLivenessData
(
   llvm::MachineBasicBlock * block
)
{
   Tiled::Dataflow::LivenessData * livenessData;

   livenessData = static_cast<Tiled::Dataflow::LivenessData *>(this->RegisterLivenessWalker->GetBlockData(block->getNumber()));  //dynamic_cast needed if > 1 derived class
   assert(livenessData || this->RegisterLivenessWalker->GetBlockData(block->getNumber()) == nullptr);

   return livenessData;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Transfer liveness for a single instruction setting the gen and kill sets
//    appropriately. Only non-dangling definitions are processed.
//
//-----------------------------------------------------------------------------

void
Liveness::TransferInstruction
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * generateBitVector,
   llvm::SparseBitVector<> * killBitVector
)
{
   Tiled::Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->RegisterLivenessWalker;

   // Reset instruction level gen and kill sets.

   killBitVector->clear();
   generateBitVector->clear();

   // Process instruction kills.
   // Don't transfer dangling definitions on the last instruction in the
   // block; these are considered part of the unique non-EH successor.

   registerLivenessWalker->TransferDestinations(instruction, generateBitVector, killBitVector);

   // Process instructions gens.
   // Always transfer source liveness

   registerLivenessWalker->TransferSources(instruction, generateBitVector, killBitVector);
}


bool
IsValueInstruction
(
   llvm::MachineInstr * instruction
)
{
   if (instruction->defs().begin() == instruction->defs().end()) {
      return false;
   }

   llvm::MachineInstr::mop_iterator destOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(),
                                                                 instruction->defs().end() );
   // foreach_register_destination_opnd
   for (destOperand = drange.begin(); destOperand != drange.end(); ++destOperand)
   {
      if (!destOperand->isReg()) {
         return false;
      }
   }

   llvm::MachineInstr::mop_iterator srcOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                               instruction->operands().end());
   bool hasUses = false;
   // foreach_source_opnd
   for (srcOperand = uses.begin(); srcOperand != uses.end(); ++srcOperand)
   {
      if (srcOperand->isMetadata() || srcOperand->isRegMask() || (srcOperand->isReg() && (/*impl*/ srcOperand->isDef() || srcOperand->isUndef()))) {
         return false;
      }
      hasUses = true;
   }

   return hasUses;
}

bool
IsFunctionArgumentCopy
(
   llvm::MachineInstr * instruction
)
{
   if (instruction->isCopy()) {
      const llvm::MachineOperand * source(instruction->uses().begin());
      const llvm::MachineOperand * destination(instruction->defs().begin());
      
      if ((destination->isReg() && Tiled::VR::Info::IsPhysicalRegister(destination->getReg()))
          || (source->isReg() && Tiled::VR::Info::IsPhysicalRegister(source->getReg()))) {
         return true;
      }
   }

   return false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Remove dead code introduced by folding and rematerialization.
//
//-----------------------------------------------------------------------------

void
Liveness::RemoveDeadCode
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *            allocator = this->Allocator;
   Tiled::VR::Info *                  vrInfo = allocator->VrInfo;
   Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->RegisterLivenessWalker;

   llvm::SparseBitVector<> * liveBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * genBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * killBitVector = new llvm::SparseBitVector<>();

   // Process each block and remove dead code as we find it.

   Graphs::MachineBasicBlockVector::reverse_iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile_backward
   for (biter = mbbVector->rbegin(); biter != mbbVector->rend(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;
      if (block->empty()) continue;

      Dataflow::LivenessData * registerLivenessData = this->GetRegisterLivenessData(block);
      Dataflow::Data *         temporaryRegisterLivenessData;
      bool                     removedAny = false;

      // Merge incoming dataflow and initialize liveness.

      temporaryRegisterLivenessData = registerLivenessWalker->Merge(block, registerLivenessData);
      registerLivenessData->Update(temporaryRegisterLivenessData);
      *liveBitVector = *(registerLivenessData->LiveOutBitVector);

      // Process each instruction in the block moving backwards calculating liveness and removing dead code.

      llvm::MachineInstr * instruction;
      llvm::MachineInstr * prev_instruction;

      // foreach_instr_in_block_backward_editing
      for (instruction = &(*block->instr_rbegin()); instruction != nullptr; instruction = prev_instruction)
      {
         prev_instruction = instruction->getPrevNode();

         this->TransferInstruction(instruction, genBitVector, killBitVector);

         // Remove dead code created by folding and rematerialization

         if (IsValueInstruction(instruction) && !IsFunctionArgumentCopy(instruction)) {
            bool isDead = true;
            bool doUnlink = false;

            // Walk every register destination and see if they are all dead.

            llvm::iterator_range<llvm::MachineInstr::mop_iterator> range(instruction->operands().begin(), 
                                                                         instruction->operands().end()  );
            llvm::MachineInstr::mop_iterator destinationOperand;

            // foreach_destination_opnd
            for (destinationOperand = range.begin(); destinationOperand != range.end(); ++destinationOperand)
            {
               if (!destinationOperand->isReg() || !destinationOperand->isDef())
                  continue;

               unsigned aliasTag = vrInfo->GetTag(destinationOperand->getReg());

               if (vrInfo->CommonMayPartialTags(aliasTag, liveBitVector)) {
                  isDead = false;
                  break;
               }

               GraphColor::AvailableExpressions * globalAvailableExpressions
                  = allocator->GlobalAvailableExpressions;

               llvm::MachineOperand * occurrenceOperand
                  = globalAvailableExpressions->LookupOccurrenceOperand(destinationOperand);

               if (occurrenceOperand != nullptr) {
                  doUnlink = true;
               }
            }

            // If instruction is dead and has no side-effects then remove it and 
            // bypass updating liveness.

            if (isDead) {

               if (!instruction->hasUnmodeledSideEffects()) {
                  // Remove instruction.
                  removedAny = true;
                  allocator->Indexes->removeMachineInstrFromMaps(*instruction);
                  block->erase_instr(instruction);
 
                  continue;
               }
            }
         }

         // Update liveness gens and kills.
         this->UpdateInstruction(instruction, liveBitVector, genBitVector, killBitVector);
      }

      // <place for EH-flow-related code (DanglingInstruction)>

      // Update liveness on block.

      *(registerLivenessData->LiveInBitVector) = *liveBitVector;

      if (removedAny) {
         allocator->Indexes->repairIndexesInRange(block, block->begin(), block->end());
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Update liveness for a single instruction using the previously calculated
//    gen and kill sets. 
//
//-----------------------------------------------------------------------------

void
Liveness::UpdateInstruction
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * liveBitVector,
   llvm::SparseBitVector<> * generateBitVector,
   llvm::SparseBitVector<> * killBitVector
)
{
   // Update liveness with gens and kills.

   liveBitVector->intersectWithComplement(*killBitVector);
   (*liveBitVector) |= *(generateBitVector);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build the life range tree of all pseudo registers for the range.
//
// Arguments:
//
//    instructionRange - range of instructions to consider.
//    onlyRegistersWithOverlap - only include pseudo registers with overlap
//        relationships to other pseudos.
//
// Returns:
//    
//    The life range tree.
//
// Remarks:
//    
//    Walk over each operand and create a union find tree based on alias tag.
//    Compare all alias tags and union all nodes which alias. The result is
//    a union-find tree where each root node is a unique life range.
//
//-----------------------------------------------------------------------------

UnionFind::Tree *
Liveness::ComputeLiveRangeAliasTagSets
(
   llvm::iterator_range<llvm::MachineBasicBlock::instr_iterator>& instructionRange,
   GraphColor::Allocator *                                        allocator,
   bool                                                           onlyRegistersWithOverlap
)
{
   Tiled::VR::Info *  aliasInfo = allocator->VrInfo;
   UnionFind::Tree *  liveRangeTree = UnionFind::Tree::New();

   // Insert all register operands

   llvm::MachineBasicBlock::instr_iterator instr;

   // foreach_instr_in_range
   for (instr = instructionRange.begin(); instr != instructionRange.end(); ++instr)
   {
      llvm::MachineInstr * instruction = &(*instr);

      // foreach_register_source_and_destination_opnd
      foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
         if (Tiled::VR::Info::IsVirtualRegister(operand->getReg())) {
            unsigned registerAliasTag = aliasInfo->GetTag(operand->getReg());

            if (onlyRegistersWithOverlap && aliasInfo->IsSingletonTag(registerAliasTag)) {
               // If the tag is the only member of its layout there can be no overlap.
               continue;
            }

            UnionFind::Member * member = liveRangeTree->Lookup(registerAliasTag);

            if (member == nullptr) {
               const llvm::TargetRegisterClass * registerCategory = allocator->GetRegisterCategory(operand->getReg());
               GraphColor::LiveRangeMember * liveRangeMember =
                  GraphColor::LiveRangeMember::New(registerAliasTag, registerCategory);

               liveRangeTree->Insert(liveRangeMember);
            }
         }

         next_source_and_destination_opnd_v2(operand, instruction, end_iter);
      }
   }

   // Process any live ranges not based on a primary tag and combine
   // overlapping appearances.

   // <place for code supporting architectures with sub-registers>


   return liveRangeTree;
}

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled
