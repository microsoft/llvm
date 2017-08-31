//===-- GraphColor/Tile.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Tile.h"
#include "Allocator.h"
#include "AvailableExpressions.h"
#include "ConflictGraph.h"
#include "Liveness.h"
#include "LiveRange.h"
#include "PreferenceGraph.h"
#include "SpillOptimizer.h"
#include "../Graphs/NodeWalker.h"
#include "TargetRegisterAllocInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetOpcodes.h"
#include "llvm/CodeGen/SlotIndexes.h"

#define DEBUG_TYPE "tiled"

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
//    Construct a new tile graph object
//
//------------------------------------------------------------------------------

GraphColor::TileGraph *
TileGraph::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::TileGraph *   tileGraph = new GraphColor::TileGraph();
   GraphColor::TileVector *  tileVector;
   Graphs::FlowGraph * flowGraph;

   tileGraph->Allocator = allocator;
   tileGraph->TileCount = 0;
   tileGraph->FunctionFlowGraph = flowGraph = allocator->FunctionUnit;

   tileVector = new GraphColor::TileVector();
   tileVector->reserve(64);
   tileGraph->TileVector = tileVector;
   tileVector->push_back(nullptr);

   // Create tile vectors for mapping block ids and tile ids to tiles.

   int                       blockCount = flowGraph->NodeCount;
   GraphColor::TileVector *  blockIdToTileVector = new GraphColor::TileVector(blockCount + 1, nullptr);
   tileGraph->BlockIdToTileVector = blockIdToTileVector;

   llvm::SparseBitVector<> * visitedBitVector = new llvm::SparseBitVector<>();
   tileGraph->VisitedBitVector = visitedBitVector;

   tileGraph->edgesToPatch = new BoundaryBlockPatchMap();

   tileGraph->ShrinkWrapTile = nullptr;

   return tileGraph;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Can we split this edge for a tile boundary?
//
//------------------------------------------------------------------------------

bool
TileGraph::CanSplitEdge
(
   Graphs::FlowEdge& edge
)
{
   // Really not splittable.

   if (!edge.isSplittable())
      return false;

   // Handle special cases (why are these splittable?)
   /* <place for EH-flow-related code: exclude the fall-through edge after exception generating instruction> */

   return true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test whether a block has an unsplittible successor edge using the allocator definition of spilittable. 
//
// Arguments:
//
//    block - Block to test.
//
// Returns:
//
//    True if block has an unsplittable successor edge.  False otherwise.
//
//------------------------------------------------------------------------------

bool
TileGraph::CanAllSuccessorsBeTileExitEdge
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineBasicBlock::succ_iterator succ_iter;

   for (succ_iter = block->succ_begin(); succ_iter != block->succ_end(); ++succ_iter)
   {
      Graphs::FlowEdge successorEdge(block, *succ_iter);

      if (!this->CanBeTileExitEdge(successorEdge)) {
         return false;
      }
   }

   return (!block->succ_empty());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test whether a block has an unsplittible predecessor edge using the allocator definition of spilittable. 
//
// Arguments:
//
//    block - Block to test.
//
// Returns:
//
//    True if block has an unsplittable predecessor edge.  False otherwise.
//
//------------------------------------------------------------------------------

bool
TileGraph::CanAllPredecessorsBeTileEntryEdge
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineBasicBlock::pred_iterator pred_iter;

   for (pred_iter = block->pred_begin(); pred_iter != block->pred_end(); ++pred_iter)
   {
      Graphs::FlowEdge predecessorEdge(*pred_iter, block);

      if (!this->CanBeTileEntryEdge(predecessorEdge)) {
         return false;
      }
   }

   return (!block->pred_empty());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Can this be a valid tile entry edge?
//
//------------------------------------------------------------------------------

bool
TileGraph::CanBeTileEntryEdge
(
   Graphs::FlowEdge& edge
)
{
   // If edge can be split, then we definitely have a good place for enter tile.

   if (this->CanSplitEdge(edge)) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Can this be a valid tile exit edge?
//
//------------------------------------------------------------------------------

bool
TileGraph::CanBeTileExitEdge
(
   Graphs::FlowEdge& edge
)
{
   // If edge can be split, then we definitely have a good place for exit tile.
   if (this->CanSplitEdge(edge)) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Can we build a tile for this loop?
//
//------------------------------------------------------------------------------

bool
TileGraph::CanBuildTileForLoop
(
   llvm::MachineLoop * loop
)
{
   // Punt on irreducible loops.

   // given the definition of getLoopPredecessor() there is only 1 predecessor, not included in the loop,
   // of the header block.
   llvm::MachineBasicBlock * predecessorBlock = loop->getLoopPredecessor();
   // The more conservative  getLoopPreheader() could be used instead.

   if (predecessorBlock == nullptr) {
      return false;
   }

   bool isLoopSuitableForTile = true;

   // Process loop enters looking for situations where we need to punt on loop.

   llvm::MachineBasicBlock * headerBlock = loop->getHeader();

   // Punt on loops if we can't inject an airlock at every loop enter where needed based on liveness.

   llvm::MachineLoopInfo * MLI = this->Allocator->LoopInfo;
   llvm::MachineLoop * loopOfHeader = MLI->getLoopFor(headerBlock);  // the innermost one
   llvm::MachineLoop * loopOfPredecessor = MLI->getLoopFor(predecessorBlock);

   if (!loopOfHeader->contains(loopOfPredecessor)) {
      Graphs::FlowEdge predecessorEdge(predecessorBlock, headerBlock);
      if (!this->CanBeTileEntryEdge(predecessorEdge)) {
         isLoopSuitableForTile = false;
      }
   }

   // Process loop exits looking for situations where we need to punt on loop.

   if (isLoopSuitableForTile) {
      llvm::SmallVector<llvm::MachineBasicBlock*,8> exitBlocks;
      loop->getExitingBlocks(exitBlocks);

      llvm::SmallVector<llvm::MachineBasicBlock*, 8>::iterator e;

      for (e = exitBlocks.begin(); e != exitBlocks.end(); ++e)
      {
         llvm::MachineBasicBlock * block = *e;

         llvm::MachineLoop * exitBlockLoop = MLI->getLoopFor(block);

         // Punt on loops formed by inline assembler.
         if (!block->empty() && (block->instr_back()).isInlineAsm()) {
            isLoopSuitableForTile = false;
            break;
         }

         llvm::MachineBasicBlock::succ_iterator s;
         for (s = block->succ_begin(); s != block->succ_end(); ++s)
         {
            llvm::MachineBasicBlock * successorBlock = *s;

            // Punt on loops if we can't inject and airlock at every loop exit where needed based
            // on liveness.

            // get respective innermost loops
            llvm::MachineLoop * successorLoop = MLI->getLoopFor(successorBlock);

            if ((exitBlockLoop != successorLoop) && (successorLoop != nullptr) && !exitBlockLoop->contains(successorLoop)) {
               Graphs::FlowEdge successorEdge(block, successorBlock);
               if (!this->CanBeTileExitEdge(successorEdge)) {
                  isLoopSuitableForTile = false;
                  break;
               }
            }
         }

         if (!isLoopSuitableForTile) {
            break;
         }
      }
   }

   return isLoopSuitableForTile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Specialized version of 'SplitEdge' for tile entry. This is specialized to ensure that the original edge
//    object is the one leading to the new block. Maintaining this allows for simpler maintenance of the
//    initial tile structure since no outer tiles entry/exit edges need to be updated.
//
// Arguments:
//
//   entryEdge - Tile entry edge to split.
//
// Returns:
//
//   New tile entry block.
//
//------------------------------------------------------------------------------

llvm::MachineBasicBlock *
TileGraph::SplitEntryEdge
(
   //note: no explicit edge objects in the Machine CFG
   Graphs::FlowEdge& entryEdge
)
{
   assert(!entryEdge.isUninitialized() && entryEdge.predecessorBlock != nullptr);

   llvm::MachineBasicBlock * successorBlock = entryEdge.successorBlock;
   Graphs::FlowGraph *       flowGraph = this->FunctionFlowGraph;
   llvm::MachineBasicBlock * boundaryBlock = flowGraph->machineFunction->CreateMachineBasicBlock();
  
   flowGraph->machineFunction->insert(llvm::MachineFunction::iterator(successorBlock), boundaryBlock);
   flowGraph->NodeCount++;
   if (boundaryBlock->getNumber() > flowGraph->MaxNodeId)
      flowGraph->MaxNodeId = boundaryBlock->getNumber();

   flowGraph->ChangeEdgeSuccessorBlock(entryEdge, boundaryBlock);

   flowGraph->NewEdge(boundaryBlock, successorBlock);
   llvm::MachineInstr * gotoInstruction = flowGraph->AppendGotoToBlock(boundaryBlock);
   assert(gotoInstruction && gotoInstruction->isUnconditionalBranch());

   return boundaryBlock;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Specialized version of 'SplitEdge' for tile exit. This is specialized to
//   ensure that the original edge object is the one leading from the new block.
//   Maintaining this allows for simpler maintenance of the initial tile
//   structure since no outer tiles entry/exit edges need to be updated.
//
// Arguments:
//
//   exitEdge - Tile exit edge to split.
//
// Returns:
//
//   New tile exit block.
//
//------------------------------------------------------------------------------

llvm::MachineBasicBlock *
TileGraph::SplitExitEdge
(
   //note: no explicit edge objects in the Machine CFG
   Graphs::FlowEdge& exitEdge
)
{
   llvm::MachineBasicBlock * predecessorBlock = exitEdge.predecessorBlock;
   llvm::MachineBasicBlock * successorBlock = exitEdge.successorBlock;

   GraphColor::Allocator * allocator = this->Allocator;
   Graphs::FlowGraph *     flowGraph = this->FunctionFlowGraph;

   llvm::MachineBasicBlock * boundaryBlock = flowGraph->machineFunction->CreateMachineBasicBlock();
   flowGraph->machineFunction->insert(llvm::MachineFunction::iterator(successorBlock), boundaryBlock);
   flowGraph->NodeCount++;
   if (boundaryBlock->getNumber() > flowGraph->MaxNodeId)
      flowGraph->MaxNodeId = boundaryBlock->getNumber();

   flowGraph->ChangeEdgePredecessorBlock(exitEdge, boundaryBlock);
   llvm::MachineInstr * gotoInstruction = flowGraph->AppendGotoToBlock(boundaryBlock);
   assert(gotoInstruction && gotoInstruction->isUnconditionalBranch());

   flowGraph->NewEdge(predecessorBlock, boundaryBlock);
   flowGraph->RedirectBranchInBlock(successorBlock, predecessorBlock, boundaryBlock);

   return boundaryBlock;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build tiles for fat blocks.
//
// Arguments:
//
//    parentTile - Tile to create fat blocks in.
//
// Returns:
//
//    true if a tile was formed.
//
//------------------------------------------------------------------------------

bool
TileGraph::BuildTilesForFat
(
   GraphColor::Tile * parentTile
)
{
   bool hasFatTile = false;

   if (parentTile->BodyBlockCount() > 1) {

      GraphColor::Allocator *          allocator = this->Allocator;
      llvm::MachineFunction *          MF = allocator->MF;
      Graphs::MachineBasicBlockList    workList;
      Tiled::Profile::Count            entryProfileCount = allocator->EntryProfileCount;

      llvm::SparseBitVector<> * bodyBlocksSet = parentTile->BodyBlockSet;
      llvm::SparseBitVector<>::iterator b;

      // foreach_body_block_in_tile_exclusive
      for (b = bodyBlocksSet->begin(); b != bodyBlocksSet->end(); ++b)
      {
         unsigned blockId = *b;
         llvm::MachineBasicBlock * block = MF->getBlockNumbered(blockId);

         if (parentTile->IsBodyBlockExclusive(block)) {

            Profile::Count blockProfileCount = allocator->getProfileCount(block);

            // Check for greater than entry profile. Any fat blocks that are cold will be handled by the cold tile
            // heuristics.

            if (CostValue::CompareGE(blockProfileCount, entryProfileCount) && allocator->IsFatBlock(block)) {
               llvm::MachineInstr * firstInstruction = &(block->front());

               if (!firstInstruction->isLabel() && block->pred_size() > 0) {
                  // previously was to catch exception-not-generated-fall-through blocks

                  llvm::MachineBasicBlock * headBlock = nullptr;
                  this->ComputeNonEHBlockSize(block, &headBlock);
                  assert(headBlock != nullptr);
                  assert(parentTile->IsBodyBlockExclusive(headBlock));

                  if (std::find(workList.begin(), workList.end(), headBlock) == workList.end()) {
                     workList.push_back(headBlock);
                  }
               } else {
                  if (std::find(workList.begin(), workList.end(), block) == workList.end()) {
                     workList.push_back(block);
                  }
               }
            }

         }
      }

      while (!workList.empty())
      {
         llvm::MachineBasicBlock * fatBlock = workList.front();
         workList.pop_front();

         hasFatTile |= this->BuildTilesForFat(parentTile, fatBlock);
      }
   }

   return hasFatTile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute NonEH block size.  This collects size info from a series of connected blocks that are only split
//    by EH handlers.
//
// Arguments:
//
//    block     - Start block to test from.
//    headBlock - Initial block of the series, will begin with a label.
//
// Returns:
//
//    NonEH block instruction count.
//
//------------------------------------------------------------------------------

unsigned
TileGraph::ComputeNonEHBlockSize
(
   llvm::MachineBasicBlock *  block,
   llvm::MachineBasicBlock ** headBlock
)
{
   unsigned                  blockCount = 0;
   llvm::MachineBasicBlock * lastBlock = block;

   // since this implementation doesn't analyze/support EH-flows blocks are not split on EH-generating instructions,
   // i.e. each block is 'maximal', there is no need to merge this block with EH-flow predecessor(s):

   blockCount += lastBlock->size();
   *headBlock = lastBlock;

   return blockCount;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build a tile for a fat block.
//
// Arguments:
//
//    parentTile - Tile to partition for fat regions.
//    block      - Starting block for fat tile formation.
//
// Returns:
//
//    true if a tile was formed.
//
//------------------------------------------------------------------------------

bool
TileGraph::BuildTilesForFat
(
   GraphColor::Tile *        parentTile,
   llvm::MachineBasicBlock * block
)
{
   Graphs::FlowGraph *  flowGraph = this->FunctionFlowGraph;

   if (block->pred_size() == 0 /*TBD: || block->succ_size() == 0*/) {
      return false;
   }

   bool  hasFat = true;

   GraphColor::Tile * fatTile = GraphColor::Tile::New(parentTile, block, TileKind::Fat);

   // If this is an EH split for a call then continue to add the blocks until we reach a fan out/merge or a
   // goto.
   /* <place for EH-flow-related code> */

   if (!this->IsValidTile(fatTile) || !this->ShouldMakeFatTile(fatTile)) {
      this->RemoveTile(fatTile);

      hasFat = false;
   } else {
      //place for debug dump of constructed Fat tile
   }

   return hasFat;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build tiles for loop list. Currently, we only support reducible loops.
//
//    See regress\crypto\zinflate.cpp:
//
//       ?DecodeBody@Inflator@CryptoPP@@AEAA_NXZ
//       private: bool __cdecl CryptoPP::Inflator::DecodeBody(void) __ptr64
//
//    For example of irreducible nested loops that yield bad loop graph.
//
//------------------------------------------------------------------------------

bool
TileGraph::BuildTilesForLoops
(
   GraphColor::Tile *      parentTile,
   GraphColor::LoopList *  loopList
)
{
   bool                      wasTileAdded = false;
   GraphColor::Tile *        tile = nullptr;

   assert(this->Allocator->IsTilingEnabled);

   // Process every loop in list.
   GraphColor::LoopList::iterator l;

   for (l = loopList->begin(); l != loopList->end(); ++l)
   {
      llvm::MachineLoop * loop = *l;

      // Make tiles for loops that are suitable.
      if (this->CanBuildTileForLoop(loop)) {
         tile = GraphColor::Tile::New(parentTile, loop);
         wasTileAdded = true;
      } else {
         // If we fail to make a loop for the current loop, reset the
         // to the parent and try the next level.
         tile = parentTile;
      }

      GraphColor::LoopList nestedLoopList;
      llvm::MachineLoopInfo::iterator nl;
      for (nl = llvm::GraphTraits<const llvm::MachineLoop*>::child_begin(loop);
           nl != llvm::GraphTraits<const llvm::MachineLoop*>::child_end(loop);
           ++nl)
      {
         nestedLoopList.push_back(*nl);
      }

      // Process loops recursively.
      wasTileAdded |= this->BuildTilesForLoops(tile, &nestedLoopList);
   }

   return wasTileAdded;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Find early out path from start block, through successor true or false "block" to end block.
//    Keep track of last block before end block.
//    Don't allow flow merge and ignore EH out edges.
//
// Parameters:
//
//    startBlock - start block, start of search
//    block      - first block after start block
//    lastBlock  - last  block before end block
//    endBlock   - end block, end of search
//
// Returns:
//
//    Number of blocks along path 0, 1, ... n or -1 if path not found.
//
//------------------------------------------------------------------------------

int
TileGraph::FindEarlyOutPath
(
   llvm::MachineBasicBlock *  startBlock,
   llvm::MachineBasicBlock *  block,
   llvm::MachineBasicBlock ** lastBlock,
   llvm::MachineBasicBlock *  endBlock,
   llvm::SparseBitVector<> *  blockSet
)
{
   GraphColor::Allocator *   allocator = this->Allocator;
   int                       pathBlockCount = 1;
   llvm::MachineBasicBlock * successorBlock = nullptr;
   llvm::MachineBasicBlock * previousBlock = startBlock;

   blockSet->set(startBlock->getNumber());
   blockSet->set(endBlock->getNumber());

   while (block != endBlock)
   {
      // If we find our path leads back into the early out block set we have a loop. (since we're following
      // only a single exit at each block) This test blocks the infinite loop.

      bool hasBackEdge = !(blockSet->test_and_set(block->getNumber()));

      if (hasBackEdge && (block != startBlock)) {
         return -1;
      }

      if (pathBlockCount > 1) {
         // We currently don't allow flow merges down simple path except at
         // the first block.

         llvm::MachineBasicBlock::pred_iterator p;
         for (p = block->pred_begin(); p != block->pred_end(); ++p)
         {
            llvm::MachineBasicBlock * predecessorBlock = *p;
            if (previousBlock != predecessorBlock) {
               return -1;
            }
         }
      }

      if (block->succ_empty()) {
         return -1;
      }

      llvm::MachineBasicBlock::succ_iterator s;
      // foreach_block_succ_edge
      for (s = block->succ_begin(); s != block->succ_end(); ++s)
      {
         // Is this a fall thru or goto edge?

         if (block->isLayoutSuccessor(*s) || (!(*s)->empty() /*previously  ((*s)->front()).isLabel()) was to exclude exception-not-generated-fall-through blocks*/)) {
            if (successorBlock != nullptr) {
               // Don't have unique successor.
               return -1;
            }

            // Get unique successor.
            successorBlock = *s;
         } else if ((*s)->isEHPad()) {
            ; // Ignore EH edges to unwind.
         } else {
            // Don't have unique successor with simple fall through.
            return -1;
         }
      }

      // Compute effective path length.

      unsigned instructionCount = block->size();

      if (instructionCount > 0 && (block->front()).isLabel()) {
         instructionCount--;
      }

      llvm::MachineInstr * lastInstruction = &(block->instr_back());

      if (lastInstruction->isUnconditionalBranch()) {
         assert(instructionCount != 0);
         instructionCount--;
      }

      if (instructionCount > 0) {
         // If the block is too "fat" by instruction count reject the path

         if (instructionCount > allocator->EarlyOutTraceBlockSizeLimit) {
            return -1;
         }

         // If the block contains a rare or unlikely call, reject the path.

         llvm::MachineBasicBlock::instr_iterator(i);
         for (i = block->instr_begin(); i != block->instr_end(); ++i)
         {
            llvm::MachineInstr * instruction = &(*i);
            if (instruction->isCall()) {
               llvm::MachineInstr * callInstruction = instruction;

               //TBD: exclude if the called function has 'cold' profile, if the info is available
            }
         }

         // if block is not a trivial airlock, like inserted for critical edge, add to evaluated block count.
         pathBlockCount++;
      }

      // Advance search forward.

      previousBlock = block;
      block = successorBlock;
      successorBlock = nullptr;
   }

   if (block == endBlock) {
      *lastBlock = previousBlock;
      return pathBlockCount;
   } else {
      *lastBlock = nullptr;
      return -1;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the tiles for shrink wrapping.
//
// Remarks:
//
//    Currently only building a single tile around "actual" function body 
//    to enable early out from root tile.
//
//    We are only handling this very simple case right now to enable basic
//    shrink wrap support.
//
//    We don't shrink wrap in the presence of EH either.
//
//    Currently looking for pattern:
//
//    L1:
//       START
//    foo:
//       ENTERFUNCTION
//    L2:
//       ENTERBODY
//       ...
//       jcc L_TRUE, L_FALSE
//    L_TRUE:
//       ... <fall through blocks, no merges> ...
//       <assign return value if any>
//       goto L3
//    ---------------------------------------
//    L_FALSE:                               \
//       <actual function body>               +- This would be actual function body tile.
//       goto L3                             /   (also works if L_TRUE/L_FALSE are reversed).
//    ---------------------------------------
//    L3:
//       EXITBODY
//    L4:
//       EXITFUNCTION
//    L5:
//       STOP
//
//------------------------------------------------------------------------------

bool
TileGraph::BuildTilesForShrinkWrapping
(
   GraphColor::Tile * parentTile
)
{
#ifdef FUTURE_IMPL   // MULTITILE +
   GraphColor::Allocator ^ allocator = this->Allocator;
   Tiled::Boolean            wasTileAdded = false;

   if (allocator->IsTilingEnabled)
   {
      if (allocator->IsShrinkWrappingEnabled)
      {
         Tiled::FunctionUnit ^             functionUnit = allocator->FunctionUnit;

         //         Profile::Info ^                 profileInfo = functionUnit->ProfileInfo;

         Graphs::FlowGraph ^             flowGraph = allocator->FlowGraph;
         Tiled::Id                       blockId;
         Graphs::BasicBlock ^            block;
         Graphs::BasicBlock ^            startBlock;
         Graphs::BasicBlock ^            endBlock;
         Graphs::BasicBlock ^            enterFunctionBlock;
         Graphs::BasicBlock ^            exitFunctionBlock;
         Graphs::BasicBlock ^            unwindBlock;
         Graphs::BasicBlock ^            enterBodyBlock;
         Graphs::BasicBlock ^            exitBodyBlock;
         Graphs::BasicBlock ^            trueBlock;
         Graphs::BasicBlock ^            falseBlock;
         Graphs::BasicBlock ^            trueLastBlock;
         Graphs::BasicBlock ^            falseLastBlock;
         Tiled::Int                        truePathBlockCount;
         Tiled::Int                        falsePathBlockCount;
         Graphs::BasicBlock ^            earlyOutBlock;
         Graphs::BasicBlock ^            earlyOutLastBlock;

         //         Graphs::BasicBlock ^            enterTileBlock;
         //         Graphs::BasicBlock ^            exitTileBlock;

         Collections::FlowEdgeVector ^   enterActualBodyEdgeVector;
         Collections::FlowEdgeVector ^   exitActualBodyEdgeVector;
         IR::Instruction ^               startInstruction;
         IR::Instruction ^               endInstruction;
         IR::Instruction ^               enterFunctionInstruction;
         IR::Instruction ^               exitFunctionInstruction;
         IR::Instruction ^               enterBodyInstruction;
         IR::Instruction ^               exitBodyInstruction;
         IR::BranchInstruction ^         branchInstruction;
         IR::LabelInstruction ^          trueLabelInstruction;
         IR::LabelInstruction ^          falseLabelInstruction;

         //         IR::ValueInstruction ^          enterTileInstruction;
         //         IR::ValueInstruction ^          exitTileInstruction;

         IR::Instruction ^               instruction;

         //         IR::Instruction ^               targetInstruction;

         BitVector::Sparse ^             earlyOutTileBlockSet;
         BitVector::Sparse ^             truePathBlockSet;
         BitVector::Sparse ^             falsePathBlockSet;

         // Don't currently handle EH.

         if (functionUnit->HasAnyEH)
         {
            goto LABEL_NoShrinkWrapping;
         }

         // Get start and end instructions.

         instruction = functionUnit->FirstInstruction;
         startInstruction = instruction->FindNextInstruction(Common::Opcode::Start);
         instruction = functionUnit->LastInstruction;
         endInstruction = instruction->FindPreviousInstruction(Common::Opcode::End);

         // Get start and end blocks

         startBlock = startInstruction->BasicBlock;
         endBlock = endInstruction->BasicBlock;

         // Get unwind block.

         unwindBlock = nullptr;
         instruction = functionUnit->UnwindInstruction;
         if (instruction != nullptr)
         {
            unwindBlock = instruction->BasicBlock;
         }

         // Get function entry and exit instructions.

         instruction = functionUnit->FirstInstruction;
         enterFunctionInstruction = instruction->FindNextInstruction(Common::Opcode::EnterFunction);
         instruction = functionUnit->LastInstruction;
         exitFunctionInstruction = instruction->FindPreviousInstruction(Common::Opcode::EnterExitFunction);

         if ((enterFunctionInstruction == nullptr) || (exitFunctionInstruction == nullptr))
         {
            goto LABEL_NoShrinkWrapping;
         }

         // Get function entry and exit blocks

         enterFunctionBlock = enterFunctionInstruction->BasicBlock;
         exitFunctionBlock = exitFunctionInstruction->BasicBlock;

         // Get function body entry and exit instructions.

         instruction = functionUnit->FirstInstruction;
         enterBodyInstruction = instruction->FindNextInstruction(Common::Opcode::EnterBody);
         instruction = functionUnit->LastInstruction;
         exitBodyInstruction = instruction->FindPreviousInstruction(Common::Opcode::ExitBody);

         if ((enterBodyInstruction == nullptr) || (exitBodyInstruction == nullptr))
         {
            goto LABEL_NoShrinkWrapping;
         }

         // Get function body entry and exit blocks

         enterBodyBlock = enterBodyInstruction->BasicBlock;
         exitBodyBlock = exitBodyInstruction->BasicBlock;

         BitVector::Sparse ^ skippedEnterBodyBlockSet = TILED_NEW_SPARSE_BITVECTOR(lifetime);

         // Get conditional branch at end of entry block.
         //
         // Allow for a chain of blocks that end in function calls that have been created because of EH
         // edges.   Walk to the conditional branch at the end of a chain of function calls.

         instruction = enterBodyBlock->LastInstruction;
         while (instruction->IsCallInstruction)
         {
            foreach_block_succ_edge(successorEdge, enterBodyBlock)
            {
               if (!Tile::IsExceptionEdge(successorEdge))
               {
                  skippedEnterBodyBlockSet->SetBit(enterBodyBlock->Id);

                  enterBodyBlock = successorEdge->SuccessorNode;
               }
            }
            next_block_succ_edge;
            Assert(instruction != enterBodyBlock->LastInstruction);
            instruction = enterBodyBlock->LastInstruction;
         }

         if (!instruction->IsBranchInstruction)
         {
            goto LABEL_NoShrinkWrapping;
         }

         branchInstruction = instruction->AsBranchInstruction;
         if (!branchInstruction->IsConditional)
         {
            goto LABEL_NoShrinkWrapping;
         }

         // Get true and false blocks.

         trueLabelInstruction = branchInstruction->TrueLabelInstruction;
         falseLabelInstruction = branchInstruction->FalseLabelInstruction;
         trueBlock = trueLabelInstruction->BasicBlock;
         falseBlock = falseLabelInstruction->BasicBlock;

         if (trueBlock == falseBlock)
         {
            goto LABEL_NoShrinkWrapping;
         }

         // Determine if either path (true or false) leads directly to exit body.

         earlyOutBlock = nullptr;
         earlyOutLastBlock = nullptr;
         trueLastBlock = nullptr;
         falseLastBlock = nullptr;

         // Keep track of blocks in early out tile.

         earlyOutTileBlockSet = TILED_NEW_SPARSE_BITVECTOR(lifetime);
         truePathBlockSet = TILED_NEW_SPARSE_BITVECTOR(lifetime);
         falsePathBlockSet = TILED_NEW_SPARSE_BITVECTOR(lifetime);

         truePathBlockCount =
            this->FindEarlyOutPath(enterBodyBlock, trueBlock, &trueLastBlock, exitBodyBlock, truePathBlockSet);
         falsePathBlockCount =
            this->FindEarlyOutPath(enterBodyBlock, falseBlock, &falseLastBlock, exitBodyBlock,
               falsePathBlockSet);

         // We should be looking at profile data when more than one option
         // for early out tile exists.  Right now we use shortest path to exit.

         if ((truePathBlockCount == -1) && (falsePathBlockCount == -1))
         {
            goto LABEL_NoShrinkWrapping;
         }
         else if ((truePathBlockCount != -1) && (falsePathBlockCount != -1))
         {
            if (truePathBlockCount <= falsePathBlockCount)
            {
               earlyOutBlock = trueBlock;
               earlyOutLastBlock = trueLastBlock;
               earlyOutTileBlockSet->Or(truePathBlockSet);
            }
            else
            {
               earlyOutBlock = falseBlock;
               earlyOutLastBlock = falseLastBlock;
               earlyOutTileBlockSet->Or(falsePathBlockSet);
            }
         }
         else if (truePathBlockCount != -1)
         {
            Assert(falsePathBlockCount == -1);
            earlyOutBlock = trueBlock;
            earlyOutLastBlock = trueLastBlock;
            earlyOutTileBlockSet->Or(truePathBlockSet);
         }
         else if (falsePathBlockCount != -1)
         {
            Assert(truePathBlockCount == -1);
            earlyOutBlock = falseBlock;
            earlyOutLastBlock = falseLastBlock;
            earlyOutTileBlockSet->Or(falsePathBlockSet);
         }
         else
         {
            Assert(false);
            goto LABEL_NoShrinkWrapping;
         }

#if 0

         // Try to grow the early out tile by including any compound conditional expression at the 
         // the enter body.
         // 
         // Example:
         //
         //     L1:
         //          ENTERBODY
         //          ...
         //          jcc L2, L3  ; automatically in early out
         //     L3:
         //          ...
         //          jcc L2, L4  ; include in early out if one branch leads to early out
         //
         //     L4:
         //          <actual function body>
         //          jmp L5
         //     L2:
         //          <early out>
         //          jmp L5
         //     L5:
         //          EXITBODY

         GraphColor::Liveness ^   liveness = this->Liveness;
         Dataflow::LivenessData ^ registerLivenessData;
         BitVector::Sparse ^      liveInEntryBitVector;
         BitVector::Sparse ^      liveInBitVector;

         registerLivenessData = liveness->GetRegisterLivenessData(enterBodyBlock);
         liveInEntryBitVector = registerLivenessData->LiveOutBitVector;

         block = enterBodyBlock;
         while (block != nullptr)
         {
            // Block must end in branch.

            instruction = block->LastInstruction;
            if (!instruction->IsBranchInstruction)
            {
               break;
            }

            // Block must end in conditional branch.

            branchInstruction = instruction->AsBranchInstruction;
            if (!branchInstruction->IsConditional)
            {
               break;
            }

            if (block != enterBodyBlock)
            {
               // Block must be small

               if (block->CompareInstructionCount(8) > 0)
               {
                  break;
               }

               // Block must have same liveness as original enter body block.

               registerLivenessData = liveness->GetRegisterLivenessData(block);
               liveInBitVector = registerLivenessData->LiveInBitVector;
               if (!BitVector::Sparse::Equals(liveInEntryBitVector, liveInBitVector))
               {
                  break;
               }
            }

            // Find which of the two edges leads back to the early out block set (if any).
            // And if a block is found, add it to the early out block set and continue searching.

            trueLabelInstruction = branchInstruction->TrueLabelInstruction;
            falseLabelInstruction = branchInstruction->FalseLabelInstruction;
            trueBlock = trueLabelInstruction->BasicBlock;
            falseBlock = falseLabelInstruction->BasicBlock;

            Tiled::Boolean isEarlyOutTrueBlock = earlyOutTileBlockSet->GetBit(trueBlock->Id);
            Tiled::Boolean isEarlyOutFalseBlock = earlyOutTileBlockSet->GetBit(falseBlock->Id);

            if (isEarlyOutTrueBlock && !isEarlyOutFalseBlock)
            {
               earlyOutTileBlockSet->SetBit(block->Id);
               block = falseBlock;
            }
            else if (!isEarlyOutTrueBlock && isEarlyOutFalseBlock)
            {
               earlyOutTileBlockSet->SetBit(block->Id);
               block = trueBlock;
            }
            else
            {
               block = nullptr;
            }
         }

#endif

         // Determine edge that enters "actual" function body.  We currently only handle
         // a single enter.  For the purposes of this algorithm, include the unwind block
         // in the early out block set.

         if (unwindBlock != nullptr)
         {
            earlyOutTileBlockSet->SetBit(unwindBlock->Id);
         }
         enterActualBodyEdgeVector = Collections::FlowEdgeVector::New(lifetime, 5);
         exitActualBodyEdgeVector = Collections::FlowEdgeVector::New(lifetime, 5);
         foreach_sparse_bv_bit(blockId, earlyOutTileBlockSet)
         {
            block = flowGraph->Block(blockId);

            if (block == unwindBlock)
            {
               continue;
            }

            // Determine actual function body entry edges.

            if (block != exitBodyBlock)
            {
               foreach_block_succ_edge(successorEdge, block)
               {
                  Graphs::BasicBlock ^ successorBlock = successorEdge->SuccessorNode;

                  if (!earlyOutTileBlockSet->GetBit(successorBlock->Id))
                  {
                     if (!this->CanSplitEdge(successorEdge))
                     {
                        goto LABEL_NoShrinkWrapping;
                     }

                     enterActualBodyEdgeVector->Push(successorEdge);
                  }
               }
               next_block_succ_edge;
            }
            // Determine actual function body exit edges.

            if (block != enterBodyBlock)
            {
               foreach_block_pred_edge(predecessorEdge, block)
               {
                  Graphs::BasicBlock ^ predecessorBlock = predecessorEdge->PredecessorNode;

                  if (!earlyOutTileBlockSet->GetBit(predecessorBlock->Id))
                  {
                     if (!this->CanSplitEdge(predecessorEdge))
                     {
                        goto LABEL_NoShrinkWrapping;
                     }

                     exitActualBodyEdgeVector->Push(predecessorEdge);
                  }
               }
               next_block_pred_edge;
            }
         }
         next_sparse_bv_bit;

         // We must have at least one entry and exit to actual function body.

         if ((enterActualBodyEdgeVector->Count() == 0) || (exitActualBodyEdgeVector->Count() == 0))
         {
            goto LABEL_NoShrinkWrapping;
         }

         BitVector::Sparse ^ actualBodySet = TILED_NEW_SPARSE_BITVECTOR(lifetime);

         Tiled::Id parentBlockId;

         foreach_sparse_bv_bit(parentBlockId, parentTile->BodyBlockSet)
         {
            if ((parentBlockId == startBlock->Id) || (parentBlockId == endBlock->Id))
            {
               continue;
            }

            if ((parentBlockId == enterFunctionBlock->Id) || (parentBlockId == exitFunctionBlock->Id))
            {
               continue;
            }

            if (!earlyOutTileBlockSet->GetBit(parentBlockId)
               && !skippedEnterBodyBlockSet->GetBit(parentBlockId))
            {
               actualBodySet->SetBit(parentBlockId);
            }
         }
         next_sparse_bv_bit;

         GraphColor::Tile ^ shrinkWrapTile = nullptr;

         foreach_nested_tile_in_tile_editing(nestedTile, parentTile)
         {
            // Move nested tiles to new shrinkwrap tile

            actualBodySet->Minus(nestedTile->BodyBlockSet);

            if (shrinkWrapTile == nullptr)
            {
               shrinkWrapTile = GraphColor::Tile::New(parentTile, nestedTile, GraphColor::TileKind::ShrinkWrap);
            }
            else if (nestedTile != shrinkWrapTile)
            {
               shrinkWrapTile->AddChild(nestedTile);
            }
         }
         next_nested_tile_in_tile_editing;

         Graphs::NodeFlowOrder ^  flowExtendedBasicBlockReversePostOrder
            = allocator->FlowExtendedBasicBlockReversePostOrder;

         // If we didn't create a tile to cover any parent tile child
         // tiles then create a tile with the first block now.

         if (shrinkWrapTile == nullptr)
         {
            BitVector::SparsePosition bitPosition;
            Tiled::Id                   firstBlockId = actualBodySet->GetFirstBit(&bitPosition);
            Tiled::Id                   orderPosition
               = flowExtendedBasicBlockReversePostOrder->Position(firstBlockId);
            Graphs::Node ^            firstBlock = flowExtendedBasicBlockReversePostOrder->Node(orderPosition);

            // Remove initial block

            actualBodySet->ClearBit(firstBlockId);

            shrinkWrapTile
               = GraphColor::Tile::New(parentTile, firstBlock->AsBasicBlock, GraphColor::TileKind::ShrinkWrap);
         }

         foreach_sparse_bv_bit(blockId, actualBodySet)
         {
            Graphs::BasicBlock ^ block = flowGraph->Block(blockId);

            if (!block->HasPredecessor && !block->HasSuccessor)
            {
               // Avoid spare blocks like data blocks.

               continue;
            }

            shrinkWrapTile->AddBlock(block);
         }
         next_sparse_bv_bit;

         this->ShrinkWrapTile = shrinkWrapTile;

#if defined (TILED_DEBUG_CHECKS)
         // Make sure the same edges are entry/exit. 

         foreach_FlowEdge_in_Vector(edge, enterActualBodyEdgeVector)
         {
            Assert(shrinkWrapTile->EntryEdgeList->Contains(edge));
         }
         next_FlowEdge_in_Vector;

         foreach_FlowEdge_in_Vector(edge, exitActualBodyEdgeVector)
         {
            Assert(shrinkWrapTile->ExitEdgeList->Contains(edge));
         }
         next_FlowEdge_in_Vector;

         // Make sure we have the same number of entry and exit.

         Tiled::UInt exceptionEdgeCount = 0;

         foreach_FlowEdge_in_List(edge, shrinkWrapTile->ExitEdgeList)
         {
            if (edge->IsException) {
               exceptionEdgeCount++;
            }
         }
         next_FlowEdge_in_List;

         Assert(enterActualBodyEdgeVector->Count() == shrinkWrapTile->EntryEdgeList->Count());
         Assert(exitActualBodyEdgeVector->Count() == (shrinkWrapTile->ExitEdgeList->Count() - exceptionEdgeCount));
#endif // TILED_DEBUG_CHECKS

         wasTileAdded = true;

LABEL_NoShrinkWrapping:

         ;
      }
   }

   return wasTileAdded;
#else
    return false;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test for special case blocks that should be excluded from the allocators definition of an EBB
//
// Arguments:
//
//    block - Block to test.
//
// Returns:
//
//    True if block may be included in an RA EBB tile.  False otherwise.
//
//------------------------------------------------------------------------------

bool
TileGraph::CanMakeEBBBlock
(
   llvm::MachineBasicBlock * block
)
{
   if (block->isEHPad())
      return false;
   if (block->isReturnBlock())
      return false;
    return true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build tiles to partition large functions.  Tiles add acyclic EBBs together until limited by flow
//    constraints or a size limit (0.1 total function size).
//
//------------------------------------------------------------------------------

bool
TileGraph::BuildTilesForSize
(
   GraphColor::Tile * parentTile
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   llvm::MachineFunction * MF = this->FunctionFlowGraph->machineFunction;
   bool                    hasSizeTile = false;
   unsigned                blockCount = 0;
   unsigned                tileBlockThreshold
      = static_cast<unsigned>(double(parentTile->BodyBlockSet->count()) * 0.1);
   bool                    isOverThreshold = false;

   if (!allocator->IsTilingEnabled /*|| HasAnyEH(MF)*/) {
      return hasSizeTile;
   }

   // floor the threshold at 12.
   tileBlockThreshold = (tileBlockThreshold < 12) ? 12 : tileBlockThreshold;

   GraphColor::Tile *                currentTile = nullptr;

   Graphs::MachineBasicBlockVector   regionStack;
   regionStack.reserve(32);
   Graphs::MachineBasicBlockList     workList;

   llvm::SparseBitVector<> * bodyBlockSet = parentTile->BodyBlockSet;
   llvm::SparseBitVector<>::iterator b;

   // foreach_body_block_in_tile_exclusive(block, parentTile)
   for (b = bodyBlockSet->begin(); b != bodyBlockSet->end(); ++b)
   {
      unsigned blockId = *b;
      llvm::MachineBasicBlock * block = MF->getBlockNumbered(blockId);

      if (parentTile->IsBodyBlockExclusive(block)) {
         if (block == parentTile->HeadBlock) {
            continue;
         }

         if (!this->CanMakeEBBBlock(block)) {
            continue;
         }

         // Form a work list of EBB entries we want to process.

         if (Graphs::ExtendedBasicBlockWalker::IsEntry(block)
            && this->CanAllSuccessorsBeTileExitEdge(block)
            && this->CanAllPredecessorsBeTileEntryEdge(block)) {
            workList.push_back(block);
         }
      }
   }

   // After this next loop runs the node order will be invalid and will have to be rebuilt.

   while (!workList.empty())
   {
      llvm::MachineBasicBlock * headBlock = workList.front();
      workList.pop_front();
      hasSizeTile = true;

      currentTile = GraphColor::Tile::New(parentTile, headBlock, GraphColor::TileKind::EBB);

      regionStack.push_back(headBlock);
      blockCount = 1;

      while (!regionStack.empty())
      {
         bool isEntryUniquePredecessorEBB = false;

         llvm::MachineBasicBlock * ebbBlock = (regionStack.back());
         regionStack.pop_back();

         llvm::MachineBasicBlock::succ_iterator s;

         // foreach_block_succ_edge
         for (s = ebbBlock->succ_begin(); s != ebbBlock->succ_end(); ++s)
         {
            llvm::MachineBasicBlock * successorBlock = *s;

            if (parentTile->IsBodyBlockExclusive(successorBlock)) {
               Graphs::FlowEdge successorEdge(ebbBlock, successorBlock);
               assert(this->CanBeTileExitEdge(successorEdge));

               bool isEntry = Graphs::ExtendedBasicBlockWalker::IsEntry(successorBlock);

               if (isEntry) {
                  bool isFirst = true;

                  llvm::MachineBasicBlock::pred_iterator p;

                  // foreach_block_pred_edge
                  for (p = successorBlock->pred_begin(); p != successorBlock->pred_end(); ++p)
                  {
                     Graphs::FlowEdge predecessorEdge(*p, successorBlock);
                     //note: the comparison below only checks equality of the 2 endpoints.
                     if (predecessorEdge == successorEdge) {
                        // Skip the edge we're coming in on.  It won't yet be in the exit set but is an exit.
                        continue;
                     }

                     if (isFirst) {
                        isEntryUniquePredecessorEBB = currentTile->IsExitEdge(predecessorEdge);
                        isFirst = false;
                     } else {
                        isEntryUniquePredecessorEBB &= currentTile->IsExitEdge(predecessorEdge);
                     }
                  }
               }

               isOverThreshold = (blockCount > tileBlockThreshold);

               if ((!isEntry || (isEntryUniquePredecessorEBB && !isOverThreshold))
                  && this->CanMakeEBBBlock(successorBlock)
                  && this->CanAllSuccessorsBeTileExitEdge(successorBlock)) {
                  assert(std::find(regionStack.begin(), regionStack.end(), successorBlock) == regionStack.end());

                  if (isEntry) {
                     //workList->Remove(successorBlock);
                     Graphs::MachineBasicBlockList::iterator t;
                     t = std::find(workList.begin(), workList.end(), successorBlock);
                     if (t != workList.end()) {
                        workList.erase(t);
                     }
                  }

                  regionStack.push_back(successorBlock);
                  blockCount++;
               }
            }
         }
      }

      // Add entry to EBB region

   }

   return hasSizeTile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the tile for function.
//
//------------------------------------------------------------------------------

void
TileGraph::BuildTileForFunction()
{
   Graphs::FlowGraph *  flowGraph = this->FunctionFlowGraph;
   GraphColor::Tile *   rootTile = GraphColor::Tile::New(this, flowGraph);

   this->RootTile = rootTile;
}

bool isJoin(llvm::MachineBasicBlock * node)
{
   llvm::MachineBasicBlock::pred_iterator p = node->pred_begin();
   if (p == node->pred_end())
      return false;

   llvm::MachineBasicBlock * firstPred = *p;

   while (++p != node->pred_end())
   {
      if (*p != firstPred) {
         return true;
      }
   }

   return false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the tile graph (i.e. tree) for the function.
//
// Notes:
//
//    New tile build that builds the tile tree before inserting the
//    airlock blocks.  This makes adding/subdividing/removing tiles easier.
//
//------------------------------------------------------------------------------

void
TileGraph::BuildGraph()
{
   GraphColor::Allocator *  allocator = this->Allocator;
   Graphs::FlowGraph *      flowGraph = this->FunctionFlowGraph;
   llvm::MachineLoopInfo *  loopInfo = flowGraph->LoopInfo;

   // Make initial tile based on function.

   this->BuildTileForFunction();

//#ifdef TILED_OFF   // if uncommented, blocks execution of multi-tile code

   if (allocator->IsTilingEnabled) {
      // Form initial tree of tiles on the loop graph

      GraphColor::LoopList     topLoopList(loopInfo->begin(), loopInfo->end());
      GraphColor::Tile *       rootTile = this->RootTile;

      this->BuildTilesForLoops(rootTile, &topLoopList);

      // Partition root tile for early out if it exits.

      this->BuildTilesForShrinkWrapping(rootTile);

      bool  enableSizeTileStress = false;

      GraphColor::TileVector *  tileVector = this->TileVector;
      size_t  t = 0;

      // foreach_tile_in_tilegraph
      for (++t /*vector-base1*/; t < tileVector->size(); ++t)
      {
         GraphColor::Tile *  tile = (*tileVector)[t];

         unsigned  bodyBlockCount = tile->BodyBlockCount();
         bool      doSplitForSize = (bodyBlockCount > allocator->NodeCountThreshold) || enableSizeTileStress;

         if ((tile->IsRoot() || tile->IsLoop()) && doSplitForSize) {
            // Cut up any potentially large tiles
            this->BuildTilesForSize(tile);
         }

         if (!tile->IsFat() && !(tile->IsRoot() && this->ShrinkWrapTile != nullptr)) {
            this->BuildTilesForFat(tile);
         }
      }

      this->MergeAdjacentTiles();
   }

//#endif   // if uncommented, blocks execution of multi-tile code

   this->Initialize();

   // Now that tile graph is initialized and boundary airlock blocks
   // are manifest go through the remaining edges and split acyclic
   // critical edges.

//#ifdef TILED_OFF   // if uncommented, blocks execution of multi-tile code

   if (allocator->SpillOptimizer->DoCallerSaveStrategy) {
      llvm::MachineFunction * MF = flowGraph->machineFunction;

      // Compute depth first numbering to identify back edges.
      flowGraph->BuildDepthFirstNumbers();

      Graphs::FlowEdgeList  criticalEdgeList;

      // Split any critical edges for caller save strategy computation.

      llvm::MachineFunction::iterator b;

      // foreach_block_in_func
      for (b = MF->begin(); b != MF->end(); ++b)
      {
         llvm::MachineBasicBlock * block = &*b;
         if (block->empty()) continue;

         if (isJoin(block)) {
            llvm::MachineBasicBlock::pred_iterator p;

            // foreach_block_pred_edge
            for (p = block->pred_begin(); p != block->pred_end(); ++p)
            {
               Graphs::FlowEdge predecessorEdge(*p, block);
               bool doSplitExitEdge = false;

               if (!predecessorEdge.isSplittable() || predecessorEdge.isBack(loopInfo)) {
                  continue;
               }

               if (predecessorEdge.isCritical()) {

                  llvm::MachineBasicBlock * predecessorBlock = predecessorEdge.predecessorBlock;
                  GraphColor::Tile *        tile = this->GetTile(predecessorBlock);
                  llvm::MachineBasicBlock * successorBlock = predecessorEdge.successorBlock;
                  GraphColor::Tile *        sinkTile = this->GetTile(successorBlock);

                  if ((doSplitExitEdge || !tile->IsExitBlock(successorBlock)) && !sinkTile->IsEntryBlock(predecessorBlock)) {
                     this->HasSplitCriticalEdges = true;

                     criticalEdgeList.push_back(predecessorEdge);
                  }
               }
            }
         }

      }

      GraphColor::TileVector * blockIdToTileVector = this->BlockIdToTileVector;
      blockIdToTileVector->resize(blockIdToTileVector->capacity() + criticalEdgeList.size());

      while (!criticalEdgeList.empty())
      {
         Graphs::FlowEdge flowEdge = criticalEdgeList.front();
         criticalEdgeList.pop_front();
         assert(flowEdge.isCritical());

         llvm::MachineBasicBlock * predecessorBlock = flowEdge.predecessorBlock;
         GraphColor::Tile *        tile = this->GetTile(predecessorBlock);

         llvm::MachineBasicBlock * airlockBlock = predecessorBlock->SplitCriticalEdge(flowEdge.successorBlock, flowGraph->pass);

         tile->AddNewBlock(airlockBlock);
      }
   }
//#endif   // if uncommented, blocks execution of multi-tile code

   // Build tile order and tile/loop/block maps.
   this->BuildTileOrders();
}

bool
lessProfileCount
(
   const Graphs::FlowEdge& edge1,
   const Graphs::FlowEdge& edge2
)
{
   return (Graphs::FlowEdge::CompareProfileCounts(edge1, edge2) == -1);
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Box up sibling tiles connected by an edge by profile order.
//
// Notes:
//
//    To allow for more flexible split points this function identifies
//    adjacent tiles (tiles connected exit to entry) and creates a new
//    parent tile that overlaps them both.  This is repeated for each
//    edge with this property with those with the highest profile
//    weight being processed first.  In this way we provide a good
//    nesting that avoids some of the problems with inserting
//    compensation code flow insensitively.
//
//                  |
//              +-------+
//      |       |  (3)  |
//    +---+     | +---+ |
//    |(1)|     | |(1)| |
//    +---+     | +---+ |
//      |   ==> |   |   |
//    +---+     | +---+ |
//    |(2)|     | |(2)| |
//    +---+     | +---+ |
//      |       +-------+
//                  |
//
//     The simple case above shows how the new tile allows the
//     allocator to split around the pair of tiles rather than
//     store/reload along a single edge.
//
//------------------------------------------------------------------------------

void
TileGraph::MergeAdjacentTiles()
{
   Graphs::FlowEdgeVector     adjacentTileEdgeVector;
   GraphColor::TileList       workList;
   llvm::SparseBitVector<> *  visitedBitVector = this->VisitedBitVector;

   GraphColor::TileVector *  tileVector = this->TileVector;
   GraphColor::TileVector::iterator  t;

   // foreach_tile_in_tilegraph
   for (t = tileVector->begin(), ++t /*vector-base1*/; t != tileVector->end(); ++t)
   {
      GraphColor::Tile *  tile = *t;

      if (tile->NestedTileSet->count() >= 2) {
         workList.push_back(tile);
      }
   }

   while (!workList.empty())
   {
      GraphColor::Tile * tile = workList.front();
      workList.pop_front();

      visitedBitVector->clear();

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         // if there is an edge connecting an exit/entry and tile is
         // the immediate parent, add to the adjacent worklist.

         if (nestedTile->ExitEdgeList->size() == 1) {
            Graphs::FlowEdgeList * exitEdgeList = nestedTile->ExitEdgeList;
            Graphs::FlowEdgeList::iterator ee;

            for (ee = exitEdgeList->begin(); ee != exitEdgeList->end(); ++ee)
            {
               Graphs::FlowEdge exitEdge = *ee;

               llvm::MachineBasicBlock * successorBlock = exitEdge.successorBlock;
               GraphColor::Tile *        adjacentTile = this->GetTile(successorBlock);
               int                       adjacentTileId = adjacentTile->Id;

               if (tile->NestedTileSet->test(adjacentTileId)) {
                  adjacentTileEdgeVector.push_back(exitEdge);
               }
            }
         }
      }

      // Sort by profile weight to favor hot entry/exit pairs.

      std::sort(adjacentTileEdgeVector.begin(), adjacentTileEdgeVector.end(), lessProfileCount);

      while (!adjacentTileEdgeVector.empty())
      {
         Graphs::FlowEdge           edge = adjacentTileEdgeVector.back();
         adjacentTileEdgeVector.pop_back();
         llvm::MachineBasicBlock *  predecessorBlock = edge.predecessorBlock;
         GraphColor::Tile *         predecessorTile = this->GetTile(predecessorBlock);

         while (predecessorTile->ParentTile != tile)
         {
            predecessorTile = predecessorTile->ParentTile;
         }

         assert(predecessorTile != nullptr);

         llvm::MachineBasicBlock *  successorBlock = edge.successorBlock;
         GraphColor::Tile *         successorTile = this->GetTile(successorBlock);

         while (successorTile->ParentTile != tile)
         {
            successorTile = successorTile->ParentTile;
         }

         assert(successorTile != nullptr);

         int  predecessorTileId = predecessorTile->Id;
         int  successorTileId = successorTile->Id;

         if (!visitedBitVector->test(predecessorTileId) && !visitedBitVector->test(successorTileId)) {
            visitedBitVector->set(predecessorTileId);
            visitedBitVector->set(successorTileId);

            GraphColor::Tile * newTile = GraphColor::Tile::New(tile, predecessorTile, GraphColor::TileKind::Acyclic);

            newTile->AddChild(successorTile);

            if ((tile->NestedTileSet->count() >= 2) &&
                (std::find(workList.begin(), workList.end(), tile) == workList.end())) {
               // read the parent tile to find any other adjacency
               // opportunities.

               workList.push_back(tile);
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test a fat tile for benefit.
//
// Notes:
//
//    - No fat tiles for entry or exit edges that are back.  It's always
//    a bad idea to make the bet that compensation code should go on a
//    back edge.
//
// Returns:
//
//  True if should make fat tile.
//
//------------------------------------------------------------------------------

bool
TileGraph::ShouldMakeFatTile
(
   GraphColor::Tile * tile
)
{
   llvm::MachineLoopInfo * MLI = this->Allocator->LoopInfo;
   Graphs::FlowEdgeList::iterator e;

   // foreach_tile_entry_edge
   for (e = tile->EntryEdgeList->begin(); e != tile->EntryEdgeList->end(); ++e)
   {
      Graphs::FlowEdge entryEdge = *e;

      if (entryEdge.isBack(MLI)) {
         // Don't allow tile boundaries on back edges as it's pretty
         // much always a bad place to put compensation code.
         return false;
      }
   }

   // foreach_tile_exit_edge
   for (e = tile->ExitEdgeList->begin(); e != tile->ExitEdgeList->end(); ++e)
   {
      Graphs::FlowEdge exitEdge = *e;

      if (exitEdge.isBack(MLI)) {
         // Don't allow tile boundaries on back edges as it's pretty
         // much always a bad place to put compensation code.
         return false;
      }
   }

   return true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test if this is a valid tile.  
//
// Arguments:
//
//    tile - Tile to test.
//
// Returns:
//
//    true if this can be initialized a tile, false otherwise.
//
//------------------------------------------------------------------------------

bool
TileGraph::IsValidTile
(
   GraphColor::Tile * tile
)
{
   assert(!tile->IsInitialized);
   llvm::MachineLoopInfo * MLI = this->Allocator->LoopInfo;

   Graphs::FlowEdgeList::iterator e;

   // foreach_tile_entry_edge
   for (e = tile->EntryEdgeList->begin(); e != tile->EntryEdgeList->end(); ++e)
   {
      Graphs::FlowEdge entryEdge = *e;

      if (entryEdge.isBack(MLI)) {
         // Don't allow tile boundaries on back edges as it's pretty
         // much always a bad place to put compensation code.
         return false;
      }

      if (!this->CanBeTileEntryEdge(entryEdge)) {
         return false;
      }
   }

   // foreach_tile_exit_edge
   for (e = tile->ExitEdgeList->begin(); e != tile->ExitEdgeList->end(); ++e)
   {
      Graphs::FlowEdge exitEdge = *e;

      if (exitEdge.isBack(MLI)) {
         // Don't allow tile boundaries on back edges as it's pretty
         // much always a bad place to put compensation code.
         return false;
      }

      if (!this->CanBeTileExitEdge(exitEdge)) {
         return false;
      }
   }

   //TODO: false exit code for unreachable lastInstruction

   return true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize TileGraph after initial tile hierarchy has been defined.  This finalizes the sizes of the
//    TileGraph block vector, initializes tiles with the data structures directly used by the allocator, and
//    manifests Tile entry and exit blocks.
//
//------------------------------------------------------------------------------

void
TileGraph::Initialize()
{
   unsigned                boundaryEdgeCount = 0;
   GraphColor::TileList    workList;

   //if uncommented, blocks execution of multi-tile code:  this->HasSplitCriticalEdges = false;

   // Initialize pre and post DFS order lists.

   GraphColor::TileList *   preOrderTileList = new GraphColor::TileList();
   this->PreOrderTileList = preOrderTileList;

   GraphColor::TileList *   postOrderTileList = new GraphColor::TileList();
   this->PostOrderTileList = postOrderTileList;

   // Compute a simple post and pre order for tiles.  This is used for
   // init to ensure that we build entry/exit blocks in the right
   // order.  The order will be recomputed during 'BuildTileOrders'
   // along with the block order so we more closely match the EBB post 
   // order used generally.

   GraphColor::Tile * rootTile = this->RootTile;

   workList.push_back(rootTile);

   while (!workList.empty())
   {
      GraphColor::Tile * currentTile = workList.back();
      workList.pop_back();

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = currentTile->NestedTileList->begin(); nt != currentTile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;
         workList.push_back(nestedTile);
      }

      boundaryEdgeCount += currentTile->EntryEdgeList->size();
      boundaryEdgeCount += currentTile->ExitEdgeList->size();

      preOrderTileList->push_back(currentTile);
   }

   // Build post order tile list.

   GraphColor::TileList::iterator t;

   // foreach_Tile_in_List
   for (t = preOrderTileList->begin(); t != preOrderTileList->end(); ++t)
   {
      GraphColor::Tile *tile = *t;
      postOrderTileList->push_front(tile);
   }

   // Make sure that we have enough space for the new blocks that will
   // be inserted.

   if (boundaryEdgeCount > 0) {
      this->BlockIdToTileVector->resize(this->BlockIdToTileVector->capacity() + boundaryEdgeCount, nullptr);
   }

   // Initialize rest of data structures 

   // foreach_tile_in_dfs_preorder
   for (t = preOrderTileList->begin(); t != preOrderTileList->end(); ++t)
   {
      GraphColor::Tile *tile = *t;
      tile->Initialize();
   }

   // Add airlocks for entry and exit.

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTileList->begin(); t != postOrderTileList->end(); ++t)
   {
      GraphColor::Tile *tile = *t;
      tile->InsertEntryAndExit();
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build allocator tile orderings based on extended basic block order.  It is required that TileGraph
//    initialization has occurred before this because no further blocks are assumed to be added to the flow
//    graph.
//
//------------------------------------------------------------------------------

void
TileGraph::BuildTileOrders()
{
   GraphColor::Allocator * allocator = this->Allocator;

   // Initialize pre and post DFS order lists.

   GraphColor::TileList * preOrderTileList = this->PreOrderTileList;
   GraphColor::TileList * postOrderTileList = this->PostOrderTileList;

   preOrderTileList->clear();
   postOrderTileList->clear();

   if ((this->TileCount > 1) || this->HasSplitCriticalEdges) {
      // Rebuild flow orders if we've inserted new blocks for tile entry/exit
      allocator->BuildFlowGraphOrder();
   }

   GraphColor::Tile * rootTile = this->RootTile;

   preOrderTileList->push_back(rootTile);
   rootTile->IsMapped = true;

   // Flow graph in rpo and set up tile orders.

   Graphs::NodeFlowOrder * flowExtendedBasicBlockReversePostOrder = allocator->FlowExtendedBasicBlockReversePostOrder;

   // foreach_block_in_order
   for (unsigned i = 1; i <= flowExtendedBasicBlockReversePostOrder->NodeCount(); ++i)
   {
      llvm::MachineBasicBlock * block = flowExtendedBasicBlockReversePostOrder->Node(i);
      GraphColor::Tile * tile = (*this->BlockIdToTileVector)[block->getNumber()];
      assert(tile != nullptr);

      if (tile->IsNestedExitBlock(block)) {
         GraphColor::Tile * childTile = this->GetNestedTile(block);
         assert(childTile->IsEntryBlock(block));

         if (!childTile->IsMapped) {
            this->PreOrderTileList->push_back(childTile);

            childTile->IsMapped = true;
         }
      }

      // Compute block instruction size.

      unsigned instructionCount = block->size();
      tile->InstructionCount += instructionCount;

      // Keep a count the number of extended basic blocks in the tile.

      //llvm::MachineInstr * firstInstruction = &(block->front());

      if (Graphs::ExtendedBasicBlockWalker::IsEntry(block)
         && !block->succ_empty()
         && !block->isEHPad()                            ) {

         // Extended basic block count is the same as the number of EBB entry blocks in the tile.
         tile->ExtendedBasicBlockCount++;
      }

      // Push blocks in to tile block vector in rpo order

      tile->BlockVector->push_back(block);
   }

   // Build post order tile list.

   GraphColor::TileList::iterator t;

   // foreach_Tile_in_List
   for (t = this->PreOrderTileList->begin(); t != this->PreOrderTileList->end(); ++t)
   {
     GraphColor::Tile *tile = *t;

      this->PostOrderTileList->push_front(tile);

      // Update total instruction count.
      this->InstructionCount += tile->InstructionCount;
   }
}

void
TileGraph::storePatchRecord
(
   const std::pair<unsigned, unsigned>&  key
)
{
   BoundaryBlockPatchMap * table = this->edgesToPatch;

   BoundaryBlockPatchMap::iterator e = table->find(key);
   std::pair<llvm::MachineBasicBlock*, bool> record(nullptr, false);

   if (e == table->end()) {
      BoundaryBlockPatchMap::value_type  entry(key, record);
      std::pair<BoundaryBlockPatchMap::iterator, bool> result = table->insert(entry);
   } else {
      e->second = record;
   }
}

bool
TileGraph::definePatch
(
   const std::pair<unsigned, unsigned>&  key,
   llvm::MachineBasicBlock*              boundaryBlock,
   bool                                  isEnterTile
)
{
   BoundaryBlockPatchMap * table = this->edgesToPatch;

   BoundaryBlockPatchMap::iterator e = table->find(key);
   if (e != table->end()) {
      e->second.first = boundaryBlock;
      e->second.second = isEnterTile;
      return true;
   } else {
      std::pair<llvm::MachineBasicBlock*, bool> record(boundaryBlock, isEnterTile);
      BoundaryBlockPatchMap::value_type  entry(key, record);
      std::pair<BoundaryBlockPatchMap::iterator, bool>  result = table->insert(entry);
      return false;
   }
}

llvm::MachineBasicBlock*
TileGraph::getPatch
(
   const std::pair<unsigned, unsigned>&  key,
   bool& wasEnterTile
)
{
   BoundaryBlockPatchMap * table = this->edgesToPatch;

   BoundaryBlockPatchMap::iterator e = table->find(key);
   if (e == table->end()) {
      return nullptr;
   }

   wasEnterTile = e->second.second;
   return e->second.first;
}


GraphColor::BlockToTileExtensionObjectMap TileGraph::BlockToTileExtension;

GraphColor::TileExtensionObject *
TileExtensionObject::GetExtensionObject
(
   llvm::MachineBasicBlock * block
)
{
   assert(block->getNumber() >= 0);

   GraphColor::BlockToTileExtensionObjectMap::iterator i = TileGraph::BlockToTileExtension.find(block->getNumber());
   //This map can probably be speeded up.
   if (i != TileGraph::BlockToTileExtension.end()) {
      return i->second;
   }

   return nullptr;
}

void
TileExtensionObject::AddExtensionObject
(
   llvm::MachineBasicBlock *          block,
   GraphColor::TileExtensionObject *  object
)
{
   GraphColor::BlockToTileExtensionObjectMap::value_type entry(block->getNumber(), object);
   std::pair<GraphColor::BlockToTileExtensionObjectMap::iterator, bool> result;
   result = TileGraph::BlockToTileExtension.insert(entry);

   if (!result.second) {
      (result.first)->second = object;
   }
}

void
TileExtensionObject::RemoveExtensionObject
(
   llvm::MachineBasicBlock *          block,
   GraphColor::TileExtensionObject *  object
)
{
   GraphColor::BlockToTileExtensionObjectMap::iterator i = TileGraph::BlockToTileExtension.find(block->getNumber());

   if (i != TileGraph::BlockToTileExtension.end()) {
      assert(i->second == object);

      if (object->GlobalAliasTagSet) {
         delete object->GlobalAliasTagSet;
      }
      delete object;

      TileGraph::BlockToTileExtension.erase(i);
   }
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Compute tile frequency for the given tile based on the TileKind. 
//
//------------------------------------------------------------------------------

void
Tile::ComputeTileFrequency()
{
   GraphColor::Allocator * allocator = this->Allocator;
   bool                    useHotModel = allocator->DoFavorSpeed;
   Profile::Count          frequency;

   assert(this->HeadBlock != nullptr);

   switch(this->TileKind)
   {
      case GraphColor::TileKind::Root:
      {
         llvm::MachineInstr * firstInstruction;
         if (!this->HeadBlock->empty()) {
            firstInstruction = &(this->HeadBlock->front());
         } else {
            llvm::MachineBasicBlock * uniqueSuccessor = allocator->FunctionUnit->uniqueSuccessorBlock(this->HeadBlock);
            assert(uniqueSuccessor != nullptr);
            firstInstruction = &(uniqueSuccessor->front());
         }
         llvm::MachineInstr * enterFunctionInstruction = firstInstruction;

         frequency = allocator->GetBiasedInstructionFrequency(enterFunctionInstruction);
         assert(Tiled::CostValue::IsGreaterThanZero(frequency));

         if (allocator->DoFavorSpeed) {
            useHotModel = true;
         }
      }
      break;

      case GraphColor::TileKind::Acyclic:
      case GraphColor::TileKind::Fat:
      case GraphColor::TileKind::EBB:
      {
         llvm::MachineInstr * firstInstruction = &(this->HeadBlock->front());

         frequency = allocator->GetBiasedInstructionFrequency(firstInstruction);
         assert(Tiled::CostValue::IsGreaterThanZero(frequency));

         if (allocator->DoFavorSpeed) {
            useHotModel = true;
         }
      }
      break;

      case GraphColor::TileKind::Loop:
      {

         if (this->HeadBlock->empty()) {
            frequency = allocator->GetBiasedBlockFrequency(this->HeadBlock);
         } else {
            llvm::MachineInstr * loopHeaderInstruction = &(this->HeadBlock->front());
            frequency = allocator->GetBiasedInstructionFrequency(loopHeaderInstruction);
         }

         assert(Tiled::CostValue::IsGreaterThanZero(frequency));

         Tiled::Profile::Count biasedEntryCount = allocator->BiasedEntryCount;
#ifdef FUTURE_IMPL   // MULTITILE +
         Tiled::Profile::Count weightedCount = Tiled::CostValue::Multiply(biasedEntryCount,
            Tiled::CostValue::ConvertFromSigned(allocator->CycleMargin));

         if (allocator->DoFavorSpeed || Tiled::CostValue::CompareGT(frequency, weightedCount)) {
            useHotModel = true;
         }
#endif
      }
      break;

   }

   this->Frequency = frequency;

   if (useHotModel) {
      this->CostModel = allocator->HotCostModel;
   } else {
      this->CostModel = allocator->ColdCostModel;
   }
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Construct a new tile object                                                                             
//                                                                                                             
// Remarks                                                                     
//
//    Initial light weight construction covering block set, entry and
//    exit edges, and tile hierarchy.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Tile::New
(
   GraphColor::TileGraph * tileGraph
)
{
   GraphColor::Allocator *     allocator = tileGraph->Allocator;
   GraphColor::Tile *          tile = new GraphColor::Tile();
   GraphColor::TileList *      tileList = new GraphColor::TileList();
   GraphColor::TileVector *    tileVector = tileGraph->TileVector;
   llvm::SparseBitVector<> *   tileSet = new llvm::SparseBitVector<>();
   Graphs::FlowEdgeList *      entryEdgeList = new Graphs::FlowEdgeList();
   Graphs::FlowEdgeList *      exitEdgeList = new Graphs::FlowEdgeList();

   // Initialize minimal data structures that capture the tile
   // structure.  Liveness and alias tag info is added via Tile::Initialize()

   tile->TileGraph = tileGraph;
   tile->Allocator = allocator;

   tile->FunctionFlowGraph = allocator->FunctionUnit;
   tile->MF = allocator->MF;
   tile->NestedTileList = tileList;
   tile->NestedTileSet = tileSet;

   tile->EntryEdgeList = entryEdgeList;
   tile->ExitEdgeList = exitEdgeList;

   llvm::SparseBitVector<> * bodyBlockSet = new llvm::SparseBitVector<>();

   tile->BodyBlockSet = bodyBlockSet;

   tileGraph->TileCount++;
   tile->Id = tileGraph->TileCount;
   tileVector->push_back(tile);

   return tile;
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Initialize Tile skeleton with the rest of the data structures needed for allocation.
//                                                                                                             
//------------------------------------------------------------------------------

void
Tile::Initialize()
{
   GraphColor::TileGraph *        tileGraph = this->TileGraph;
   GraphColor::Allocator *        allocator = tileGraph->Allocator;
   llvm::SparseBitVector<> *      tileAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      spilledAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalSpillAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalLiveInAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalLiveOutAliasTagSet = new llvm::SparseBitVector<>();
   GraphColor::LiveRangeVector *  summaryLiveRangeVector;
   llvm::SparseBitVector<> *      killedRegisterAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      registersOverwrittentInTile = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      allocatableRegisterAliasTagSet = new llvm::SparseBitVector<>();

   // Track decisions on tile segments of global live ranges

   llvm::SparseBitVector<> *      foldAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      recalculateAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      memoryAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      callerSaveAliasTagSet = new llvm::SparseBitVector<>();

   // Track defs and uses of global live ranges allocated transitively within the tile (includes nested tiles).

   llvm::SparseBitVector<> *      globalUsedAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalDefinedAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalTransitiveUsedAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalTransitiveDefinedAliasTagSet = new llvm::SparseBitVector<>();

   // Track gens and kills of globals in the original program

   llvm::SparseBitVector<> *      globalGenerateAliasTagSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      globalKillAliasTagSet = new llvm::SparseBitVector<>();

   // Track liveness at block boundaries to determine whether a live range is block local or not.

   llvm::SparseBitVector<> *      nonBlockLocalAliasTagSet = new llvm::SparseBitVector<>();

   // Track block that include a max kill point for one of the register classes.

   llvm::SparseBitVector<> *      integerMaxKillBlockBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *      floatMaxKillBlockBitVector = new llvm::SparseBitVector<>();

   // Block sets

   llvm::SparseBitVector<> * entryBlockSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * exitBlockSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * nestedEntryBlockSet = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> * nestedExitBlockSet = new llvm::SparseBitVector<>();

   this->EntryBlockSet = entryBlockSet;
   this->ExitBlockSet = exitBlockSet;
   this->NestedEntryBlockSet = nestedEntryBlockSet;
   this->NestedExitBlockSet = nestedExitBlockSet;

   // Information for tile scheduling.

   Tiled::Cost globalWeightCost = allocator->ZeroCost;

   unsigned  blockCount = this->BodyBlockSet->count();

   Graphs::MachineBasicBlockVector * blockVector = new Graphs::MachineBasicBlockVector();
   blockVector->reserve(blockCount);
   this->BlockVector = blockVector;
   this->LocalIdMap = allocator->LocalIdMap;

   //TODO: see notes in the static constructor Allocator::New
   this->IdToSlotMap = allocator->IdToSlotMap;

   this->VrInfo = allocator->VrInfo;
   this->DummySpillSymbol = allocator->DummySpillSymbol;
   this->ExtendedBasicBlockCount = 0;
   this->NestedTileAliasTagSet = tileAliasTagSet;
   this->SpilledAliasTagSet = spilledAliasTagSet;
   this->GlobalAliasTagSet = globalAliasTagSet;
   this->GlobalLiveInAliasTagSet = globalLiveInAliasTagSet;
   this->GlobalLiveOutAliasTagSet = globalLiveOutAliasTagSet;
   this->GlobalSpillAliasTagSet = globalSpillAliasTagSet;
   this->KilledRegisterAliasTagSet = killedRegisterAliasTagSet;
   this->RegistersOverwrittentInTile = registersOverwrittentInTile;
   this->AllocatableRegisterAliasTagSet = allocatableRegisterAliasTagSet;

   this->NonBlockLocalAliasTagSet = nonBlockLocalAliasTagSet;

   this->FoldAliasTagSet = foldAliasTagSet;
   this->RecalculateAliasTagSet = recalculateAliasTagSet;
   this->MemoryAliasTagSet = memoryAliasTagSet;
   this->CallerSaveAliasTagSet = callerSaveAliasTagSet;

   // Used and Defined vectors are exclusive to the tile and only
   // contain globals that have been allocated.

   this->GlobalUsedAliasTagSet = globalUsedAliasTagSet;
   this->GlobalDefinedAliasTagSet = globalDefinedAliasTagSet;
   this->GlobalTransitiveUsedAliasTagSet = globalTransitiveUsedAliasTagSet;
   this->GlobalTransitiveDefinedAliasTagSet = globalTransitiveDefinedAliasTagSet;

   // Generate and Killed are transitive with nested tiles (as you'd
   // expect) and contain every global from the original liveness.

   this->GlobalGenerateAliasTagSet = globalGenerateAliasTagSet;
   this->GlobalKillAliasTagSet = globalKillAliasTagSet;

   this->GlobalWeightCost = globalWeightCost;

   // Push an null ref so zero doesn't map to a real summary

   summaryLiveRangeVector = new GraphColor::LiveRangeVector();
   summaryLiveRangeVector->reserve(32);
   summaryLiveRangeVector->push_back(nullptr);
   this->SummaryLiveRangeVector = summaryLiveRangeVector;
   GraphColor::AliasTagToIdMap * summaryAliasTagToIdMap = new GraphColor::AliasTagToIdMap();
   this->SummaryAliasTagToIdMap = summaryAliasTagToIdMap;

   this->EntryBlockList = new Graphs::MachineBasicBlockList();
   this->ExitBlockList = new Graphs::MachineBasicBlockList();

   this->IntegerMaxKillBlockBitVector = integerMaxKillBlockBitVector;
   this->FloatMaxKillBlockBitVector = floatMaxKillBlockBitVector;

   // If there's no headblock set, test whether there is a single head
   // block (all entries lead to the same block) and set it.

   if (this->HeadBlock == nullptr) {
      // root tile has empty EntryEdgeList but it uses getEntryNode(MF) to set HeadBlock
      Graphs::FlowEdge maxEdge;

      Graphs::FlowEdgeList::iterator e;

      // foreach_tile_entry_edge
      for (e = this->EntryEdgeList->begin(); e != this->EntryEdgeList->end(); ++e)
      {
         Graphs::FlowEdge edge = *e;

         if (maxEdge.isUninitialized()) {
            maxEdge = edge;
         } else if (Graphs::FlowEdge::CompareProfileCounts(edge, maxEdge) > 0) {
            maxEdge = edge;
         }
      }

      assert(!maxEdge.isUninitialized());

      this->HeadBlock = maxEdge.successorBlock;
   }

   this->ComputeTileFrequency();

#ifdef FUTURE_IMPL   // MULTITILE +
   if (allocator->IsShrinkWrappingEnabled) {
      // Detect the case where a tile exit does not have a path to function exit - or can be said to
      // dangle. For functions that we shrink wrap, if they also contain dangling exits, we can end up in a
      // case where we have incorrect liveness/restores (this case covered by asserts) and unwind info can
      // later fail to be generated correctly.  So we detect dangling exits here and than opt out of shrink
      // wrapping later if warranted.

      IR::Instruction ^    exitInstruction = functionUnit->LastExitInstruction;
      Graphs::BasicBlock ^ exitBlock = exitInstruction->BasicBlock;
      IR::Instruction ^    unwindInstruction = functionUnit->UnwindInstruction;
      Graphs::BasicBlock ^ unwindBlock = unwindInstruction->BasicBlock;

      Assert((exitBlock != nullptr) && (unwindBlock != nullptr));

      foreach_tile_exit_edge(edge, this)
      {
         Graphs::BasicBlock ^ sinkBlock = edge->SuccessorNode;

         // Note: we do not consider exits that go directly to unwind to be dangling.  This is an exception to
         // the above rule because of the special nature of unwind (there is special handling in the frame
         // tree for this case).

         if ((sinkBlock != unwindBlock) && !sinkBlock->HasPathToNode(exitBlock))
         {
            // Mark tile as having a dangling exit.

            this->HasDanglingExit = true;
         }
      }
      next_tile_exit_edge;
   }
#endif

   this->Pass = GraphColor::Pass::Ready;
   this->HasAssignedRegisters = false;

   this->IsInitialized = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Construct a new tile object starting with a single block
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Tile::New
(
   GraphColor::Tile *        parentTile,
   llvm::MachineBasicBlock * block,
   GraphColor::TileKind      tileKind
)
{
   GraphColor::TileGraph * tileGraph = parentTile->TileGraph;
   GraphColor::Tile *      tile = Tile::New(tileGraph);

   tile->TileKind = tileKind;

   parentTile->AddChild(tile);

   tile->AddBlock(block);

   return tile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Construct a new tile object starting with a single nested tile
//    taken from the passed parent.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Tile::New
(
   GraphColor::Tile *   parentTile,
   GraphColor::Tile *   nestedTile,
   GraphColor::TileKind tileKind
)
{
   GraphColor::TileGraph * tileGraph = parentTile->TileGraph;
   GraphColor::Tile *      tile = Tile::New(tileGraph);

   tile->TileKind = tileKind;

   tile->AddChild(nestedTile);

   parentTile->AddChild(tile);

   return tile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Construct a new tile object from function unit
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Tile::New
(
   GraphColor::TileGraph * tileGraph,
   Graphs::FlowGraph *     flowGraph
)
{
   GraphColor::Tile * tile = Tile::New(tileGraph);

   tile->TileKind = GraphColor::TileKind::Root;

   llvm::MachineFunction * MF = flowGraph->machineFunction;
   tile->HeadBlock = llvm::GraphTraits<llvm::MachineFunction*>::getEntryNode(MF);

   // Function tile is special in that it has no entry or exit blocks

   llvm::MachineFunction::iterator b;

   // foreach_block_in_func
   for (b = MF->begin(); b != MF->end(); ++b)
   {
      llvm::MachineBasicBlock * block = &*b;

      // This is a special case of "Tile::AddBlock()".  Because the function unit tile can have no entry/exit
      // edges we can avoid the logic tracking boundaries.  This is only valid for constructing the initial
      // function unit tile which is why the code is inlined rather than factored into a separate
      // function. (there should be no reuse)

      bool wasSet = !(tile->BodyBlockSet->test_and_set(block->getNumber()));
      assert(!wasSet && "Should never double add a block");

      tileGraph->SetTile(block, tile);
   }

   return tile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Construct a new tile object from loop
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Tile::New
(
   GraphColor::Tile *  parentTile,
   llvm::MachineLoop * loop
)
{
   GraphColor::TileGraph *  tileGraph = parentTile->TileGraph;
   GraphColor::Tile *       tile = Tile::New(tileGraph);

   tile->TileKind = GraphColor::TileKind::Loop;
   tile->HeadBlock = loop->getHeader();

#if defined (TILED_DEBUG_CHECKS)
   // Stash the loop for checks.
   tile->Loop = loop;
#endif

   llvm::MachineLoop::block_iterator b;

   // foreach_inclusive_block_in_loop
   for (b = loop->block_begin(); b != loop->block_end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;

      tile->AddBlock(block);
   }

   parentTile->AddChild(tile);

   return tile;
}

struct equalEdges : public std::binary_function<Graphs::FlowEdge, Graphs::FlowEdge, bool>
{
   bool operator()
   (
      const Graphs::FlowEdge&  edge1,
      const Graphs::FlowEdge&  edge2
   ) const  { return (edge1 == edge2); }
};


//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add child tile.
//
// Arguments:
//
//    tile - Tile to be made a child of the receiver tile.
//                                                                                                             
//------------------------------------------------------------------------------

void
Tile::AddChild
(
   GraphColor::Tile * tile
)
{
   GraphColor::Tile * originalParentTile = tile->ParentTile;

   // Make sure we're not passed ourselves.
   assert(tile != this);

   // Reset parent relationship
   tile->ParentTile = this;

   if (originalParentTile != nullptr) {
      // remove child from old parent

      originalParentTile->NestedTileSet->reset(tile->Id);

      GraphColor::TileList * parentNestedTileList = originalParentTile->NestedTileList;
      GraphColor::TileList::iterator t;
      t = std::find(parentNestedTileList->begin(), parentNestedTileList->end(), tile);
      if (t != parentNestedTileList->end()) {
         parentNestedTileList->erase(t);
      }
   }

   // Fix up entry/exit if this was a sibling.

   Graphs::FlowEdgeList::iterator e, next_e;

   // foreach_tile_entry_edge
   for (e = this->EntryEdgeList->begin(); e != this->EntryEdgeList->end(); e = next_e)
   {
      Graphs::FlowEdge entryEdge = *e;
      next_e = e; ++next_e;

      if (tile->IsExitEdge(entryEdge)) {
         llvm::MachineBasicBlock * successorBlock = entryEdge.successorBlock;

         this->RemoveEntryEdge(entryEdge);

         if (this == this->TileGraph->GetTile(successorBlock)) {
            // If this origin of the exit then remove it.

            tile->RemoveExitEdge(entryEdge);
         }
      }
   }

   // foreach_tile_exit_edge
   for (e = this->ExitEdgeList->begin(); e != this->ExitEdgeList->end(); e = next_e)
   {
      Graphs::FlowEdge exitEdge = *e;
      next_e = e; ++next_e;

      if (tile->IsEntryEdge(exitEdge)) {
         llvm::MachineBasicBlock * predecessorBlock = exitEdge.predecessorBlock;

         this->RemoveExitEdge(exitEdge);

         if (this == this->TileGraph->GetTile(predecessorBlock)) {
            // If this origin of the entry then remove it.

            tile->RemoveEntryEdge(exitEdge);
         }
      }
   }

   // Add nested tile blocks to inclusive body block set.

   *(this->BodyBlockSet) |= *(tile->BodyBlockSet);

   // Add any entry/exit edges that lead from outside blocks to now
   // nested tiles to the new parent tile.

   // foreach_tile_entry_edge
   for (e = tile->EntryEdgeList->begin(); e != tile->EntryEdgeList->end(); ++e)
   {
      Graphs::FlowEdge entryEdge = *e;

      llvm::MachineBasicBlock * predecessorBlock = entryEdge.predecessorBlock;

      Graphs::FlowEdgeList::const_iterator f;
      f = std::find_if(this->EntryEdgeList->begin(), this->EntryEdgeList->end(), std::bind2nd(equalEdges(), entryEdge));

      if (f != this->EntryEdgeList->end()) {
         std::pair<unsigned, unsigned>  key(entryEdge.predecessorBlock->getNumber(), entryEdge.successorBlock->getNumber());
         this->TileGraph->storePatchRecord(key);
      }

      if (!this->IsBodyBlockInclusive(predecessorBlock) && f == this->EntryEdgeList->end()) {
         this->AddEntryEdge(entryEdge);
      }
   }

   // foreach_tile_exit_edge
   for (e = tile->ExitEdgeList->begin(); e != tile->ExitEdgeList->end(); ++e)
   {
      Graphs::FlowEdge exitEdge = *e;

      llvm::MachineBasicBlock * successorBlock = exitEdge.successorBlock;


      Graphs::FlowEdgeList::const_iterator f;
      f = std::find_if(this->ExitEdgeList->begin(), this->ExitEdgeList->end(), std::bind2nd(equalEdges(), exitEdge));

      if (f != this->ExitEdgeList->end()) {
         std::pair<unsigned, unsigned>  key(exitEdge.predecessorBlock->getNumber(), exitEdge.successorBlock->getNumber());
         this->TileGraph->storePatchRecord(key);
      }

      if (!this->IsBodyBlockInclusive(successorBlock) && f == this->ExitEdgeList->end()) {
         this->AddExitEdge(exitEdge);
      }
   }

   // Add tile to nested set.

   this->NestedTileSet->set(tile->Id);
   this->NestedTileList->push_back(tile);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add child tile.
//
// Arguments:
//
//    tile - Tile to be made a child of the receiver tile.
//                                                                                                             
//------------------------------------------------------------------------------

void
Tile::RemoveChild
(
   GraphColor::Tile * tile
)
{
   // Make sure we're not passed ourselves.
   assert(tile != this);

   // Make sure we're a child.
   assert(this == tile->ParentTile);

   // Validate the parent/child data.
   llvm::SparseBitVector<> * nestedTileSet = this->NestedTileSet;
   GraphColor::TileList *    nestedTileList = this->NestedTileList;
   assert(nestedTileSet->test(tile->Id));

   GraphColor::TileList::iterator containedTile = std::find(nestedTileList->begin(), nestedTileList->end(), tile);
   assert(containedTile != nestedTileList->end());

   // Remove tile to nested set.

   nestedTileSet->reset(tile->Id);
   nestedTileList->erase(containedTile);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add tile block
//
// Arguments:
//
//    block - New block to add to tile.
//
// Returns:
//
//   'true' if block not currently part of tile.
//                                                                                                             
//------------------------------------------------------------------------------

bool
Tile::AddBlock
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;

   bool wasSet = !(this->BodyBlockSet->test_and_set(block->getNumber()));
   assert(!wasSet  /*"Should never double add a block"*/);

   if (!wasSet) {

      tileGraph->SetTile(block, this);

      if (!this->IsInitialized || (!this->IsNestedEntryBlock(block) && !this->IsNestedExitBlock(block))) {

       llvm::MachineBasicBlock::pred_iterator p;

       // foreach_block_pred_edge
       for (p = block->pred_begin(); p != block->pred_end(); ++p)
       {
          Graphs::FlowEdge predecessorEdge(*p, block);
          llvm::MachineBasicBlock * predecessorBlock = predecessorEdge.predecessorBlock;

            // Self edge always is pulled into the tile.
            if (predecessorBlock != block) {
               if (this->IsBodyBlockInclusive(predecessorBlock)) {
                  bool wasRemoved = false;

                  wasRemoved |= this->RemoveExitEdge(predecessorEdge);
                  wasRemoved |= this->RemoveEntryEdge(predecessorEdge);

                  assert(wasRemoved /*"Malformed arc to new block"*/);
               } else {
                  this->AddEntryEdge(predecessorEdge);
               }
            }
         }

       llvm::MachineBasicBlock::succ_iterator s;
       // foreach_block_succ_edge
       for (s = block->succ_begin(); s != block->succ_end(); ++s)
       {
          Graphs::FlowEdge successorEdge(block, *s);
          llvm::MachineBasicBlock * successorBlock = successorEdge.successorBlock;

            // Self edge always is pulled into the tile.
            if (successorBlock != block) {
               if (this->IsBodyBlockInclusive(successorBlock)) {
                  bool wasRemoved = false;

                  // For a backedge we will be expecting a entry edge
                  wasRemoved |= this->RemoveEntryEdge(successorEdge);
                  wasRemoved |= this->RemoveExitEdge(successorEdge);

                  assert(wasRemoved /*"Malformed arc from new block"*/);
               } else {
                  this->AddExitEdge(successorEdge);
               }
            }
         }
      }
   }

   return !wasSet;
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add a newly created block to tile.
//
// Arguments:
//
//    block - New block to add to tile.
//
// Notes:
//
//    As a newly created block the tile graph will not be tracking the
//    entry/exit edges so omit all typical checks but add the stronger
//    assert that the block successor and predecessor blocks are in
//    the same tile.
//                                                                                                             
//------------------------------------------------------------------------------

void
Tile::AddNewBlock
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;

   bool wasSet = !(this->BodyBlockSet->test_and_set(block->getNumber()));
   assert(!wasSet && "Should be a new block");

   tileGraph->SetTile(block, this);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add entry edge.
//
// Arguments:
//
//    edge - New entry edge.
//
//------------------------------------------------------------------------------

void
Tile::AddEntryEdge
(
   Graphs::FlowEdge& edge
)
{
   Graphs::FlowEdgeList::const_iterator f;
   assert(std::find_if(this->EntryEdgeList->begin(), this->EntryEdgeList->end(), std::bind2nd(equalEdges(), edge))
         == this->EntryEdgeList->end());

   this->EntryEdgeList->push_back(edge);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Remove entry edge.
//
// Arguments:
//
//    edge - Entry edge to remove.
//
// Returns:
//
//    'true' if entry edge was removed (existed prior).
//
//------------------------------------------------------------------------------

bool
Tile::RemoveEntryEdge
(
   Graphs::FlowEdge& edge
)
{
   Graphs::FlowEdgeList::iterator f;
   f = std::find_if(this->EntryEdgeList->begin(), this->EntryEdgeList->end(), std::bind2nd(equalEdges(), edge));
   if (f != this->EntryEdgeList->end()) {
      this->EntryEdgeList->erase(f);
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add entry block.
//
// Arguments:
//
//    block - New entry block.
//
//------------------------------------------------------------------------------

void
Tile::AddEntryBlock
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::Tile * parentTile = this->ParentTile;

   assert(parentTile != nullptr);

   this->EntryBlockSet->set(block->getNumber());
   this->EntryBlockList->push_back(block);

   parentTile->AddNestedExitBlock(block);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add exit edge.
//
// Arguments:
//
//    edge - New exit edge.
//
//------------------------------------------------------------------------------

void
Tile::AddExitEdge
(
   Graphs::FlowEdge& edge
)
{
   assert(std::find_if(this->ExitEdgeList->begin(), this->ExitEdgeList->end(), std::bind2nd(equalEdges(), edge))
         == this->ExitEdgeList->end());

   this->ExitEdgeList->push_back(edge);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Remove exit edge.
//
// Arguments:
//
//    edge - Exit edge to remove.
//
// Returns:
//
//    'true' if exit edge was removed (existed prior).
//
//------------------------------------------------------------------------------

bool
Tile::RemoveExitEdge
(
   Graphs::FlowEdge& edge
)
{
   Graphs::FlowEdgeList::iterator f;
   f = std::find_if(this->ExitEdgeList->begin(), this->ExitEdgeList->end(), std::bind2nd(equalEdges(), edge));
   if (f != this->ExitEdgeList->end()) {
      this->ExitEdgeList->erase(f);
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add exit block.
//
// Arguments:
//
//    block - New exit block.
//
//------------------------------------------------------------------------------

void
Tile::AddExitBlock
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::Tile *  parentTile = this->ParentTile;
   assert(parentTile != nullptr);

   this->ExitBlockSet->set(block->getNumber());
   this->ExitBlockList->push_back(block);

   parentTile->AddNestedEntryBlock(block);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add nested entry block.
//
// Arguments:
//
//    block - New nested entry block.
//
//------------------------------------------------------------------------------

void
Tile::AddNestedEntryBlock
(
   llvm::MachineBasicBlock * block
)
{
   this->NestedEntryBlockSet->set(block->getNumber());
   this->AddBlock(block);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Add nested exit block.
//
// Arguments:
//
//    block - New nested exit block.
//
//------------------------------------------------------------------------------

void
Tile::AddNestedExitBlock
(
   llvm::MachineBasicBlock * block
   )
{
   this->NestedExitBlockSet->set(block->getNumber());
   this->AddBlock(block);
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Remove tile from tile graph.
//
// Arguments:
//
//    tile - Tile to remove.
//
//------------------------------------------------------------------------------

void
TileGraph::RemoveTile
(
   GraphColor::Tile * tile
)
{
   GraphColor::Tile * parentTile = tile->ParentTile;
   assert(parentTile != nullptr);

   GraphColor::TileList * nestedTileList = tile->NestedTileList;
   GraphColor::TileList::iterator nt, next_nt;

   // foreach_nested_tile_in_tile_editing
   for (nt = nestedTileList->begin(); nt != nestedTileList->end(); nt = next_nt)
   {
      GraphColor::Tile * nestedTile = *nt;
      next_nt = nt; ++next_nt;

      parentTile->AddChild(nestedTile);
   }

   llvm::MachineFunction *  MF = this->Allocator->MF;

   llvm::SparseBitVector<> * bodyBlockSet = tile->BodyBlockSet;
   llvm::SparseBitVector<>::iterator b;

   // foreach_body_block_in_tile_exclusive
   for (b = bodyBlockSet->begin(); b != bodyBlockSet->end(); ++b)
   {
      unsigned blockId = *b;
      llvm::MachineBasicBlock * block = MF->getBlockNumbered(blockId);
      if (tile->IsBodyBlockExclusive(block)) {
         this->SetTile(block, parentTile);
      }
   }

   parentTile->RemoveChild(tile);
   tile->ParentTile = nullptr;

   delete nestedTileList;
   delete tile->NestedTileSet;
   delete bodyBlockSet;
   delete tile->EntryEdgeList;
   delete tile->ExitEdgeList;

   GraphColor::TileVector *  tileVector = this->TileVector;
   assert(tileVector->back() == tile);
   tileVector->pop_back();
   this->TileCount--;

   delete tile;
}


llvm::MachineBasicBlock *
uniqueSuccessorBlock(llvm::MachineBasicBlock * block)
{
   if (block->succ_size() == 1)
      return *(block->succ_begin());

   return nullptr;
}

llvm::MachineBasicBlock *
uniquePredecessorBlock(llvm::MachineBasicBlock * block)
{
   if (block->pred_size() == 1)
      return *(block->pred_begin());

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get liveness that can be used at tile boundary insertion points.
//
// Arguments:
//
//    edge        - Edge to split for tile boundary.
//    doSuccessor - direction to compute liveness for.  True is successor direction.
//
// Note:
//
//    The returned bit vector is pointing into live data so it should not be modified.  It is not a copy.
//
// Returns:
//
//    BitVector for the livenss at the first valid block edge found forward or back.
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
TileGraph::GetLiveness
(
   Graphs::FlowEdge&  edge,
   bool               doSuccessor
)
{
   GraphColor::Allocator * allocator = this->Allocator;
   GraphColor::Liveness *  liveness = allocator->Liveness;

   if (doSuccessor) {
      llvm::MachineBasicBlock * sinkBlock = edge.successorBlock;

      do
      {
         Dataflow::LivenessData * sinkLivenessData = liveness->GetRegisterLivenessData(sinkBlock);

         if (sinkLivenessData != nullptr) {
            return sinkLivenessData->LiveInBitVector;
         } else {
            sinkBlock = uniqueSuccessorBlock(sinkBlock);
         }
      }
      while (sinkBlock != nullptr);

      // If we drop through to here we found no unique sink liveness.  In this case we conservativly return
      // nullptr.

      return nullptr;

   } else {
      llvm::MachineBasicBlock *  sourceBlock = edge.predecessorBlock;

      do
      {
         Dataflow::LivenessData * sourceLivenessData = liveness->GetRegisterLivenessData(sourceBlock);

         if (sourceLivenessData != nullptr) {
            return sourceLivenessData->LiveOutBitVector;
         } else {
            sourceBlock = uniquePredecessorBlock(sourceBlock);
         }

      }
      while (sourceBlock != nullptr);

      // If we drop through to here we found no unique source liveness.  In this case we conservativly return
      // nullptr.

      return nullptr;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert tile boundary block.
//
// Arguments:
//
//    edge - Edge to split for tile boundary.
//    tile - Tile this 
//
// Returns:
//
//    True if block is a tile exit, false otherwise.
//
//------------------------------------------------------------------------------

llvm::MachineBasicBlock *
TileGraph::InsertTileBoundaryBlock
(
   Graphs::FlowEdge&  edge,
   GraphColor::Tile * tile,
   bool               isEntry
)
{
   assert(!edge.isUninitialized());
   assert(tile != nullptr);

   llvm::MachineBasicBlock * boundaryBlock;
   llvm::MachineInstr *      boundaryInstruction;
   llvm::MachineFunction *   MF = this->FunctionFlowGraph->machineFunction;
   GraphColor::Allocator *   allocator = this->Allocator;

   if (isEntry) {

      llvm::SparseBitVector<> * scratchLiveBitVector = this->VisitedBitVector;
      scratchLiveBitVector->clear();

      llvm::SparseBitVector<> * liveOutBitVector = this->GetPredecessorLiveness(edge);
      llvm::SparseBitVector<> * liveInBitVector = this->GetSuccessorLiveness(edge);
      assert(liveOutBitVector != nullptr);
      assert(liveInBitVector != nullptr);

      *scratchLiveBitVector = *liveOutBitVector;
      *scratchLiveBitVector &= *liveInBitVector;

      llvm::SparseBitVector<> * doNotAllocateRegisterAliasTagBitVector
        = allocator->DoNotAllocateRegisterAliasTagBitVector;
      scratchLiveBitVector->intersectWithComplement(*doNotAllocateRegisterAliasTagBitVector);

      llvm::SparseBitVector<> * allRegisterTags = allocator->AllRegisterTags;

      assert(allRegisterTags != nullptr);
      *scratchLiveBitVector &= *allRegisterTags;

      DEBUG({
         if (this->Allocator->FunctionUnit->machineFunction->getName().str() == "main") {
            llvm::dbgs() << " **** T#" << tile->Id << " (kind=" << unsigned(tile->TileKind) << ")\n";
            llvm::dbgs() << "      SplitEntryEdge:  <mbb#" << edge.predecessorBlock->getNumber()
                      << ", mbb#" << edge.successorBlock->getNumber() << ">\n";
         }
      });

      boundaryBlock = this->SplitEntryEdge(edge);

      boundaryInstruction = MF->CreateMachineInstr(allocator->MCID_ENTERTILE, llvm::DebugLoc(), true);

      Tiled::VR::Info * vrInfo = allocator->VrInfo;

      // Create an alias operand of allocatable physregs on the EnterTile.  This maintains liveness into the
      // tile even where hard preferences are set on the definition side.

      if (!scratchLiveBitVector->empty()) {
         llvm::SparseBitVector<>::iterator r;

         // foreach_sparse_bv_bit
         for (r = scratchLiveBitVector->begin(); r != scratchLiveBitVector->end(); ++r)
         {
            unsigned tileBoundaryLiveRegisterTag = *r;
            unsigned tileBoundaryLiveRegister = vrInfo->GetRegister(tileBoundaryLiveRegisterTag);
            llvm::MachineOperand tileBoundaryLiveRegisterOperand(llvm::MachineOperand::CreateReg(tileBoundaryLiveRegister, false));

            boundaryInstruction->addOperand(*MF, tileBoundaryLiveRegisterOperand);
            //llvm::dbgs() << " ENTER#" << boundaryBlock->getNumber() << ": tileBoundaryLiveRegisterOperand = " << tileBoundaryLiveRegister << "\n";
         }
      }

   } else {

      DEBUG({
         if (this->Allocator->FunctionUnit->machineFunction->getName().str() == "main") {
            llvm::dbgs() << " **** T#" << tile->Id << " (kind=" << unsigned(tile->TileKind) << ")\n";
            llvm::dbgs() << "      SplitExitEdge:  <mbb#" << edge.predecessorBlock->getNumber()
                         << ", mbb#" << edge.successorBlock->getNumber() << ">\n";
         }
      });

      boundaryBlock = this->SplitExitEdge(edge);

      boundaryInstruction = MF->CreateMachineInstr(allocator->MCID_EXITTILE, llvm::DebugLoc(), true);
   }

   // boundaryBlock has a <goto> instruction appended in the above SplitEntry[Exit]Edge()
   llvm::MachineBasicBlock::instr_iterator I(boundaryBlock->instr_back());
   boundaryBlock->insert(I, boundaryInstruction);

   DEBUG({
      if (this->Allocator->FunctionUnit->machineFunction->getName().str() == "****") {
         llvm::dbgs() << "      inserted BoundaryBlock#" << boundaryBlock->getNumber() << "\n";
      }
   });

   allocator->Indexes->insertMBBInMaps(boundaryBlock);
   allocator->Indexes->insertMachineInstrInMaps(*boundaryInstruction);

   allocator->SetTileId(boundaryInstruction, tile->Id);

   assert(TileExtensionObject::GetExtensionObject(boundaryBlock) == nullptr);
   GraphColor::TileExtensionObject * tileExtensionObject = TileExtensionObject::New();
   tileExtensionObject->Tile = tile;
   TileExtensionObject::AddExtensionObject(boundaryBlock, tileExtensionObject);

   return boundaryBlock;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert tile boundary block.
//
// Arguments:
//
//    block - Block to split to make tile entry
//    tile - Tile this
//    isEntry - true if insert is entry false otherwise. 
//
//------------------------------------------------------------------------------

void
TileGraph::InsertTileBoundaryBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   bool                      isEntry
)
{
   assert(block != nullptr);
   assert(tile != nullptr);

   GraphColor::Allocator * allocator = this->Allocator;
   Graphs::FlowGraph *     flowGraph = allocator->FunctionUnit;

   llvm::MachineBasicBlock * boundaryBlock;

   if (isEntry) {
      // Find entry edge to split, then redirect all others to it.

      llvm::MachineBasicBlock::pred_iterator pred_iter = block->pred_begin();
      assert(pred_iter != block->pred_end());
      Graphs::FlowEdge splitEdge(*pred_iter, block);

      boundaryBlock = this->InsertTileBoundaryBlock(splitEdge, tile, isEntry);

      llvm::MachineBasicBlock::pred_iterator p;

      // foreach_block_pred_edge_editing
      for (p = block->pred_begin(); p != block->pred_end(); ++p)
      {
         Graphs::FlowEdge          predecessorEdge(*p, block);
         llvm::MachineBasicBlock * predecessorBlock = predecessorEdge.predecessorBlock;

         if (predecessorBlock != boundaryBlock) {
            flowGraph->ChangeEdgeSuccessorBlock(predecessorEdge, boundaryBlock);

            llvm::MachineInstr * lastInstruction = &(boundaryBlock->instr_back());
         }
      }

   } else {
      llvm::MachineBasicBlock * succBlock = flowGraph->UniqueNonEHSuccessorBlock(block);

      if (succBlock != nullptr) {
         Graphs::FlowEdge splitEdge(block, succBlock);

         // Find an exit edge to split, then redirect all others to
         // it.  (unique successor)

         boundaryBlock = this->InsertTileBoundaryBlock(splitEdge, tile, isEntry);

         llvm::MachineBasicBlock::succ_iterator s;
         for (s = block->succ_begin(); s != block->succ_end(); ++s)
         {
            llvm::MachineBasicBlock * successorBlock = *s;

            if (successorBlock != boundaryBlock) {
               block->replaceSuccessor(successorBlock, boundaryBlock);
            }
         }

      } else {
         // Split block

         llvm::MachineInstr * lastInstruction = &(block->instr_back());
         assert(lastInstruction->isBranch());

         llvm::MachineBasicBlock * bottomBlock = flowGraph->SplitBlock(block, lastInstruction);

         llvm::MachineBasicBlock::pred_iterator pred_iter = bottomBlock->pred_begin();
         assert(pred_iter != bottomBlock->pred_end() && *pred_iter == block);
         Graphs::FlowEdge splitEdge(*pred_iter, bottomBlock);

         boundaryBlock = this->InsertTileBoundaryBlock(splitEdge, tile, isEntry);
      }
   }
}

//------------------------------------------------------------------------------
//                                                                                                             
// Description:                                                                                                
//                                                                                                             
//    Insert enter and exit blocks into the flow graph.  This makes explicit all tile transition points
//    (validated via TileGraph::CheckGraphFlow()). All tracked entry/exit edges are split via the allocators
//    version of 'split{entry,exit}edge' and the resulting blocks are added to the appropriate tiles.
//
//------------------------------------------------------------------------------

void
Tile::InsertEntryAndExit()
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;

   DEBUG({
      llvm::dbgs() << "\n *** Tile# " << this->Id << "\n";
      llvm::dbgs() << "      { ";
      llvm::SparseBitVector<>::iterator b;
      // foreach_body_block_in_tile_exclusive
      for (b = this->BodyBlockSet->begin(); b != this->BodyBlockSet->end(); ++b)
         llvm::dbgs() << " #" << *b;
      llvm::dbgs() << " }\n";
   });

   // Process entry edge list work list style.  After this point must
   // refer to the entry blocks.
   Graphs::FlowEdgeList *  entryEdgeList = this->EntryEdgeList;

   while (!entryEdgeList->empty())
   {
      Graphs::FlowEdge  entryEdge = entryEdgeList->front();
      entryEdgeList->pop_front();

      // patch the edge if needed
      bool wasEnterTile = true;
      llvm::MachineBasicBlock*  patchBlock = nullptr;
      std::pair<unsigned, unsigned>  key(entryEdge.predecessorBlock->getNumber(), entryEdge.successorBlock->getNumber());

      do
      {
         patchBlock = this->TileGraph->getPatch(key, wasEnterTile);
         if (patchBlock) {
            if (wasEnterTile) {
               entryEdge.successorBlock = patchBlock;
               key.second = patchBlock->getNumber();
            } else {
               entryEdge.predecessorBlock = patchBlock;
               key.first = patchBlock->getNumber();
            }
         }
      } while (patchBlock);

      if (entryEdge.isSplittable()) {
         llvm::MachineBasicBlock *  enterBlock = tileGraph->InsertTileEnterBlock(entryEdge, this);
         this->TileGraph->definePatch(key, enterBlock, true);

         this->AddEntryBlock(enterBlock);
      }
   }

   Graphs::FlowEdgeList * exitEdgeList = this->ExitEdgeList;

   while (!exitEdgeList->empty())
   {
      Graphs::FlowEdge  exitEdge = exitEdgeList->front();
      exitEdgeList->pop_front();

      // patch the edge if needed
      bool wasEnterTile = false;
      llvm::MachineBasicBlock*  patchBlock = nullptr;
      std::pair<unsigned, unsigned>  key(exitEdge.predecessorBlock->getNumber(), exitEdge.successorBlock->getNumber());

      do
      {
         patchBlock = this->TileGraph->getPatch(key, wasEnterTile);
         if (patchBlock) {
            if (!wasEnterTile) {
               exitEdge.predecessorBlock = patchBlock;
               key.first = patchBlock->getNumber();
            } else {
               exitEdge.successorBlock = patchBlock;
               key.second = patchBlock->getNumber();
            }
         }
      } while (patchBlock);

      if (exitEdge.isSplittable()) {
         llvm::MachineBasicBlock * originalSuccessorBlock = exitEdge.successorBlock;
         llvm::MachineBasicBlock * exitBlock = tileGraph->InsertTileExitBlock(exitEdge, this);
         this->TileGraph->definePatch(key, exitBlock, false);

         if (this->IsShrinkWrap() && !exitEdgeList->empty()) {

            GraphColor::Allocator * allocator = tileGraph->Allocator;
            Graphs::FlowGraph * flowGraph = allocator->FunctionUnit;
            //TBD: Profile::Info ^         profileInfo = functionUnit->ProfileInfo;

            // Do exit redirection to the outer shrink wrap tile - this may be extended but the fact that
            // multiple regions may hold a given edge and so need multiple exits means that it's only safe for
            // 'shrink wrap' today.
            Graphs::FlowEdgeList::iterator e, next_e;

            // foreach_FlowEdge_in_List_editing
            for (e = exitEdgeList->begin(); e != exitEdgeList->end(); e = next_e)
            {
               Graphs::FlowEdge otherExitEdge = *e;
               next_e = e; next_e++;

               if (otherExitEdge.successorBlock == originalSuccessorBlock) {  //TODO: which original?
                  // If other exits target the same block redirect to share the exit block.
                  exitEdgeList->erase(e);

                  flowGraph->ChangeEdgeSuccessorBlock(otherExitEdge, exitBlock);

                  llvm::MachineInstr * lastInstruction = &(exitBlock->instr_back());
                  //TBD: profileInfo->PropagateData(lastInstruction);
               }
            }
         }

         this->AddExitBlock(exitBlock);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a block in the body of the tile?
//
// Note:
//
//    This is the inclusive set.  Body blocks are blocks who reside in
//    the given tile or a nested tile.
//
//------------------------------------------------------------------------------

bool
Tile::IsBodyBlockInclusive
(
   llvm::MachineBasicBlock * block
)
{
   unsigned                  blockId = block->getNumber();
   llvm::SparseBitVector<> * blockSet = this->BodyBlockSet;

   return blockSet->test(blockId);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a block in the exclusive body of the tile?
//
// Note:
//
//    This excludes blocks that belong to nested tiles.
//
//------------------------------------------------------------------------------

bool
Tile::IsBodyBlockExclusive
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;
   bool                    isInclusiveBlock = this->IsBodyBlockInclusive(block);

   if (isInclusiveBlock) {
      return (tileGraph->GetTile(block) == this);
   } else {
      return false;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a block an entry block of the tile?
//
//------------------------------------------------------------------------------

bool
Tile::IsEntryBlock
(
   llvm::MachineBasicBlock * block
)
{
   unsigned                  blockId = block->getNumber();
   llvm::SparseBitVector<> * blockSet = this->EntryBlockSet;

   return blockSet->test(blockId);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given edge and entry edge of the tile?
//
//------------------------------------------------------------------------------

bool
Tile::IsEntryEdge
(
   Graphs::FlowEdge& edge
)
{
   Graphs::FlowEdgeList * entryEdgeList = this->EntryEdgeList;

   Graphs::FlowEdgeList::const_iterator f;
   f = std::find_if(entryEdgeList->begin(), entryEdgeList->end(), std::bind2nd(equalEdges(), edge));
   return (f != entryEdgeList->end());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a block an exit block of the tile?
//
//------------------------------------------------------------------------------

bool
Tile::IsExitBlock
(
   llvm::MachineBasicBlock * block
)
{
   unsigned                   blockId = block->getNumber();
   llvm::SparseBitVector<> *  blockSet = this->ExitBlockSet;

   return blockSet->test(blockId);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given edge and entry edge of the tile?
//
//------------------------------------------------------------------------------

bool
Tile::IsExitEdge
(
   Graphs::FlowEdge& edge
)
{
   Graphs::FlowEdgeList * exitEdgeList = this->ExitEdgeList;

   Graphs::FlowEdgeList::const_iterator f;
   f = std::find_if(exitEdgeList->begin(), exitEdgeList->end(), std::bind2nd(equalEdges(), edge));
   return (f != exitEdgeList->end());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a nested entry block of the tile?
//
//------------------------------------------------------------------------------

bool
Tile::IsNestedEntryBlock
(
   llvm::MachineBasicBlock * block
)
{
   unsigned                  blockId = block->getNumber();
   llvm::SparseBitVector<> * blockSet = this->NestedEntryBlockSet;

   return blockSet->test(blockId);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Is the given block a block an nested exit block of the tile?
//
//------------------------------------------------------------------------------

Tiled::Boolean
Tile::IsNestedExitBlock
(
   llvm::MachineBasicBlock * block
)
{
   unsigned                  blockId = block->getNumber();
   llvm::SparseBitVector<> * blockSet = this->NestedExitBlockSet;

   return blockSet->test(blockId);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add live range to tile
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::AddLiveRange
(
   int       aliasTag,
   unsigned  reg
)
{
   GraphColor::Allocator *           allocator = this->Allocator;
   Tiled::VR::Info *                 vrInfo = this->VrInfo;
   GraphColor::LiveRangeVector *     liveRangeVector = this->LiveRangeVector;
   GraphColor::LiveRange *           liveRange;
   unsigned                          liveRangeId;

   // Check to see if live range already exists
   liveRange = this->GetLiveRange(aliasTag);

   // If it doesn't see we have previously mapped alias tag and update
   // the live range.   Or, generate a new live range.

   if (liveRange == nullptr) {
      if (reg == VR::Constants::InitialPseudoReg || reg == VR::Constants::InvalidReg) {
         reg = this->Allocator->VrInfo->GetRegister(aliasTag);
      }
      const llvm::TargetRegisterClass * registerCategory = allocator->GetRegisterCategory(reg);
      if (!allocator->GetAllocatableRegisters(registerCategory)) {
         return nullptr;
      }

      if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
         // Don't create live ranges for things we can't allocate.
         if (allocator->DoNotAllocateRegisterAliasTagBitVector->test(aliasTag)) {
            //This eliminates Frame Register (FP) unless this MF is "frameless",
            //it also eliminates the scratch register (if any).
            return nullptr;
         }
      }

      // Determine if we have mapped this alias tag or any tags it overlaps already.  
      // If so, we just found a larger appearance so use existing live range and 
      // widen the alias tag on the live range.

      liveRangeId = allocator->GetLiveRangeIdWithOverlap(aliasTag);

      if (liveRangeId != 0) {
         // if this is an overlap case we could also have to handle a
         // merge (to disjoint live ranges that feed a single larger use).

         // This code will be relevant only in future implementation supporting architectures with physical registers having sub-registers.
         // Currently, there will be NO mergeable (in terms of the code below) live ranges, i.e. mergedLiveRangeId == 0.

         unsigned mergedLiveRangeId = allocator->GetLiveRangeIdWithMerge(aliasTag, liveRangeId);

         liveRange = (*liveRangeVector)[liveRangeId];
         assert(liveRange != nullptr);

         assert(vrInfo->MustTotallyOverlap(aliasTag, liveRange->VrTag));
         liveRange->VrTag = aliasTag;

         if (mergedLiveRangeId != 0) {
            // Handle n-way merge.  In practice this is very rare (2-
            // way is rare - more than 2 is super rare).

            do
            {
               allocator->MapAliasTagToLiveRangeIdMerged(aliasTag, liveRangeId, mergedLiveRangeId);

               GraphColor::LiveRange * mergedLiveRange = (*liveRangeVector)[mergedLiveRangeId];
               assert(mergedLiveRange != nullptr);

               liveRangeVector->erase(std::remove(liveRangeVector->begin(), liveRangeVector->end(), mergedLiveRange),
                                      liveRangeVector->end());

               // fix up liverange ids. (so the rest of the code doesn't
               // have to skip nullptrs everywhere).

               for (unsigned index = mergedLiveRangeId; index < liveRangeVector->size(); index++)
               {
                  GraphColor::LiveRange * adjustedLiveRange = (*liveRangeVector)[index];
                  assert(adjustedLiveRange->Id == (index + 1));
                  adjustedLiveRange->Id = index;

                  allocator->MapAliasTagToLiveRangeIdOverlappedRemap(adjustedLiveRange->VrTag, index);
               }

               // Update liveRangeId to account for the case where the above loop adjusted the id as part of
               // the merge.

               liveRangeId = allocator->GetLiveRangeIdWithOverlap(aliasTag);

               mergedLiveRangeId = allocator->GetLiveRangeIdWithMerge(aliasTag, liveRangeId);
            }
            while (mergedLiveRangeId != 0);

         } else {
            allocator->MapAliasTagToLiveRangeIdOverlappedExclusive(aliasTag, liveRangeId);
         }

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;
      }

      // Construct a new live range for this alias tag and map it in.
      else {
         liveRangeId = liveRangeVector->size();
         liveRange  = GraphColor::LiveRange::NewLocal(this, liveRangeId, aliasTag);
         liveRangeVector->push_back(liveRange);

         allocator->MapAliasTagToLiveRangeIdOverlappedExclusive(aliasTag, liveRangeId);

         if (!((reg == VR::Constants::InitialPseudoReg) || vrInfo->IsVirtualRegister(reg))) {
            liveRange->IsPreColored = true;
            if (Tiled::VR::Info::IsPhysicalRegisterTag(aliasTag)) {
               liveRange->IsPhysicalRegister = true;
            }
            liveRange->Register = reg;

            // Do we know the difference?  When are these not the same?
            assert(liveRange->IsPhysicalRegister == liveRange->IsPreColored);
         } else {
            liveRange->Register = VR::Constants::InitialPseudoReg;
         }

         liveRange->Tile = this;
      }
   }

   // Track whether we have any byte references to this live range.  This
   // will further constrain register selection.

   // <place for code supporting architectures with sub-registers>

   // It is possible to see a larger pseudo register request because of
   // machine constraints (example: X86 push tv22(reg32).i8.   Keep track
   // of largest pseudo register associated with live range.

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add summary live range to tile
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::AddSummaryLiveRange
(
   unsigned   aliasTag,
   unsigned   reg
)
{
   GraphColor::LiveRangeVector *  liveRangeVector = this->SummaryLiveRangeVector;
   unsigned                       liveRangeId;
   GraphColor::LiveRange *        liveRange;

   liveRangeId = liveRangeVector->size();
   liveRange  = GraphColor::LiveRange::NewSummary(this, liveRangeId, aliasTag);

   liveRange->Register = reg;

   liveRange->Tile = this;

   liveRangeVector->push_back(liveRange);

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Map alias tag to summary live range id for this tile.
//
//------------------------------------------------------------------------------

void
Tile::MapAliasTagToSummaryId
(
   unsigned aliasTag,
   unsigned summaryLiveRangeId
)
{
   Tiled::VR::Info * vrInfo = this->VrInfo;
   GraphColor::AliasTagToIdMap::iterator s;
   s = this->SummaryAliasTagToIdMap->find(aliasTag);
   if (s != this->SummaryAliasTagToIdMap->end()) {
     // already mapped
     return;
   }

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_must_total_alias_of_tag(totallyOverlappedAliasTag, aliasTag, aliasInfo)
   //{
      GraphColor::AliasTagToIdMap::value_type entry(/*totallyOverlappedAliasTag*/ aliasTag, summaryLiveRangeId);
      this->SummaryAliasTagToIdMap->insert(entry);
   //}
   //next_must_total_alias_of_tag;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear mapping of alias tag to summary id.
//
// Notes:
//
//    Used when spilling globals pass 2 from summaries to keep map up
//    to date.
//
//------------------------------------------------------------------------------

void
Tile::ClearAliasTagToSummaryIdMap
(
   unsigned aliasTag
)
{
   Tiled::VR::Info *              aliasInfo = this->VrInfo;
   GraphColor::AliasTagToIdMap *  summaryAliasTagToIdMap = this->SummaryAliasTagToIdMap;

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_may_partial_alias_of_tag(partialTag, aliasTag, aliasInfo)
   //{
      summaryAliasTagToIdMap->erase(aliasTag);
   //}
   //next_may_partial_alias_of_tag;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add register live range to tile
//
// Note: 
//
//    Each distinct physical register gets their own live range this
//    allows for full preferencing and register constraints.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::AddRegisterLiveRange
(
   unsigned reg
)
{
   GraphColor::Allocator *       allocator = this->Allocator;
   Tiled::VR::Info *             vrInfo = this->VrInfo;
   GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;
   unsigned                      aliasTag = vrInfo->GetTag(reg);
   unsigned                      liveRangeId;
   GraphColor::LiveRange *       liveRange = this->GetLiveRange(aliasTag);

   if (liveRange == nullptr) {
      unsigned originalLiveRangeId = allocator->GetLiveRangeIdWithOverlap(aliasTag);

      if (originalLiveRangeId == 0) {
         // New, write overlapped

         liveRangeId = liveRangeVector->size();
         liveRange  = GraphColor::LiveRange::NewLocal(this, liveRangeId, aliasTag);
         liveRangeVector->push_back(liveRange);

         allocator->MapAliasTagToRegisterLiveRangeIdOverlapped(aliasTag, liveRangeId);

         assert(Tiled::VR::Info::IsPhysicalRegister(reg));
         liveRange->IsPreColored = true;
         liveRange->IsPhysicalRegister = true;

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;

         liveRange->Tile = this;
      }
      //else if (reg->HasSubRegister && (reg->AllocatableChildCount > 1))
      // <place for code supporting architectures with sub-registers>
      else {
         GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;
         assert(originalLiveRangeId != 0);

         // Widen

         liveRange = (*liveRangeVector)[originalLiveRangeId];
         assert(liveRange != nullptr);

         assert(vrInfo->MustTotallyOverlap(aliasTag, liveRange->VrTag));
         liveRange->VrTag = aliasTag;

         allocator->MapAliasTagToLiveRangeIdOverlappedExclusive(aliasTag, originalLiveRangeId);

         //assert(liveRange->BaseRegisterCategory == reg->BaseRegisterCategory);
         //assert(liveRange->Register->BitSize <= reg->BitSize);

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;
      }

   } else {
      //if (reg->HasSubRegister && (reg->AllocatableChildCount > 1)) {
      // <place for code supporting architectures with sub-registers>
      // New, write overlapped
   }

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile scope decision for a global live range that crosses the
//    tile boundary.
//
//------------------------------------------------------------------------------

GraphColor::Decision
Tile::GetGlobalDecision
(
   unsigned globalLiveRangeAliasTag
)
{
   // Ensure that the global in question has a live range within this tile.
   assert(this->GlobalAliasTagSet->test(globalLiveRangeAliasTag));

   if (this->MemoryAliasTagSet->test(globalLiveRangeAliasTag)) {
      return GraphColor::Decision::Memory;
   }

   if (this->RecalculateAliasTagSet->test(globalLiveRangeAliasTag)) {
      return GraphColor::Decision::Recalculate;
   }

#ifdef ARCH_WITH_FOLDS
   if (this->FoldAliasTagSet->test(globalLiveRangeAliasTag)) {
      return GraphColor::Decision::Fold;
   }
#endif

   // Note that while we do track CallerSave decision we return "Allocate" for
   // global live ranges. The CallerSave decision is only tracked for use in the tile
   // version of ComputeRegisterLiveness() to aid when computing tile liveness.

   assert(this->GetSummaryLiveRange(globalLiveRangeAliasTag) != nullptr);
   assert(!this->GlobalSpillAliasTagSet->test(globalLiveRangeAliasTag));

   return GraphColor::Decision::Allocate;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile scope decision for a global live range that crosses the
//    tile boundary.
//
//------------------------------------------------------------------------------

void
Tile::SetGlobalDecision
(
   unsigned              globalLiveRangeAliasTag,
   GraphColor::Decision  decision
)
{
   // Ensure that the global in question has a live range within this tile.
   assert(this->GlobalAliasTagSet->test(globalLiveRangeAliasTag));

   switch(decision)
   {
      case GraphColor::Decision::Memory:
      {
         this->MemoryAliasTagSet->set(globalLiveRangeAliasTag);
      }
      break;

      case GraphColor::Decision::Recalculate:
      {
         this->RecalculateAliasTagSet->set(globalLiveRangeAliasTag);
      }
      break;

#ifdef ARCH_WITH_FOLDS
      case GraphColor::Decision::Fold:
      {
         this->FoldAliasTagSet->set(globalLiveRangeAliasTag);
      }
      break;
#endif

      case GraphColor::Decision::CallerSave:

         // Caller save decision doesn't modify the register candidates at tile boundaries and so
         // looks like allocation to any other tile.
         // CallerSave is noted here for use in building tile liveness but will not be returned by
         // GetGlobalDecision() so that CallerSave continues to look like "Allocation"

         this->CallerSaveAliasTagSet->set(globalLiveRangeAliasTag);

         break;

      default:
         llvm_unreachable("No spill decision!");
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile liverange by alias tag.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetLiveRange
(
   unsigned aliasTag
)
{
   // Don't be calling this when there isn't an active tile with a current 
   // aliasTag to liveRange mapping.

   assert(this->LiveRangeVector != nullptr);

   if (aliasTag < 1) {
      return nullptr;
   }

   GraphColor::Allocator *        allocator = this->Allocator;
   unsigned                       liveRangeId  = allocator->GetLiveRangeId(aliasTag);
   GraphColor::LiveRangeVector *  liveRangeVector = this->LiveRangeVector;
   GraphColor::LiveRange *        liveRange = (*liveRangeVector)[liveRangeId];

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile liverange from operand.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetLiveRange
(
   llvm::MachineOperand * operand
)
{
   assert(operand->isReg());
   unsigned                aliasTag = this->VrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange * liveRange = this->GetLiveRange(aliasTag);

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile allocated register by alias tag
//
//------------------------------------------------------------------------------

unsigned
Tile::GetAllocatedRegister
(
   unsigned aliasTag
)
{
   // Tile points to a allocator initialized alias tag vector that
   // maps alias tag to tile summary live ranges
   assert(this->AliasTagToIdVector != nullptr);

   // Ensure we're asking questions only of the active tile since that
   // will be what initialized the AliasTagToIdVector.

   assert(this->Allocator->ActiveTile == this);
   //"GetAllocatedRegister can only be called on the active tile."

   unsigned summaryLiveRangeId = (*this->AliasTagToIdVector)[aliasTag];

   GraphColor::LiveRange * summaryLiveRange = (*this->SummaryLiveRangeVector)[summaryLiveRangeId];

   if (summaryLiveRange != nullptr) {
      return summaryLiveRange->Register;
   }

   return VR::Constants::InvalidReg;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile live range by live range id
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetLiveRangeById
(
   unsigned liveRangeId
)
{
   GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;
   GraphColor::LiveRange *       liveRange = (*liveRangeVector)[liveRangeId];

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile LiveRange enumerator
//
// Remarks:
//
//     Currently we just use the vector as the basis for enumeration.
//
//------------------------------------------------------------------------------

GraphColor::LiveRangeEnumerator *
Tile::GetLiveRangeEnumerator
(
)
{
   GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;

   return liveRangeVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile LiveRange vector suitable for ordering.
//
// Remarks:
//
//     Currently we just use the, the default is reverse source order
//
// Returns:
//
//     Ordered live range vector.
//
//------------------------------------------------------------------------------

GraphColor::LiveRangeVector *
Tile::GetLiveRangeOrder()
{
   GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;
   GraphColor::LiveRangeVector * orderedLiveRangeVector;
   unsigned                      count = liveRangeVector->size();

   orderedLiveRangeVector = new GraphColor::LiveRangeVector(count, nullptr);

   // foreach_LiveRange_in_Vector
   GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();
   for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
   {
      GraphColor::LiveRange * liveRange = *lr;
      count--;
      (*orderedLiveRangeVector)[count] = liveRange;
   }

   return orderedLiveRangeVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile summary live range by summary live range id
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetSummaryLiveRangeById
(
   unsigned summaryLiveRangeId
)
{
   GraphColor::LiveRangeVector * summaryLiveRangeVector = this->SummaryLiveRangeVector;
   GraphColor::LiveRange *       summaryLiveRange = (*summaryLiveRangeVector)[summaryLiveRangeId];

   return summaryLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile summary live range by alias tag
//
// Arguments:
//
//    aliasTag - Alias tag to test for membership in a summary.
//
// Notes:
//
//    This API is separate from the more general "GetLiveRange" because that relies on an initialized global
//    aliasTagToLiveRangeId.  This API can be called on a nested or parent tile when that tile is not the
//    initialized tile. Though look up is slower.
//
// Returns:
//
//    Summary live range or nullptr
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetSummaryLiveRange
(
   unsigned aliasTag
)
{
   GraphColor::AliasTagToIdMap::iterator i = this->SummaryAliasTagToIdMap->find(aliasTag);

   if (i != this->SummaryAliasTagToIdMap->end()) {
      unsigned summaryLiveRangeId = i->second;

      if (summaryLiveRangeId != 0) {
         GraphColor::LiveRange *  summaryLiveRange = (*this->SummaryLiveRangeVector)[summaryLiveRangeId];

         if (!summaryLiveRange->IsSpilled()) {
            return summaryLiveRange;
         }
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile register summary live range by alias tag
//
// Arguments:
//
//    aliasTag - Register alias tag to test for membership in a summary.
//
// Notes:
//
//    This API is separate from the more general "GetLiveRange" because that relies on an initialized global
//    aliasTagToLiveRangeId.  This API can be called on a nested or parent tile when that tile is not the
//    initialized tile. Though look up is slower.
//
// Returns:
//
//    Summary live range or nullptr
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Tile::GetRegisterSummaryLiveRange
(
   unsigned aliasTag
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::VR::Info *          vrInfo = allocator->VrInfo;

   assert(vrInfo->IsRegisterTag(aliasTag));

   GraphColor::LiveRangeVector * slrVector = this->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];
      assert(summaryLiveRange->IsSummary());

      if (vrInfo->MustTotallyOverlap(summaryLiveRange->VrTag, aliasTag)) {
         return summaryLiveRange;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get summary degree for the live range.
//
// Arguments:
//
//    liveRange - live range needing degree
//
// Notes:
//
//    The summary conflict graph may have duplicate edges in it representing physregs as well as hard
//    preferences. This function factors the edges to return the true degree count.  
//
// Returns:
//
//    degree
//
//------------------------------------------------------------------------------

unsigned
Tile::GetSummaryDegree
(
   GraphColor::LiveRange * liveRange
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   Tiled::VR::Info *          aliasInfo = this->VrInfo;
   llvm::SparseBitVector<> *  registerConflictsBitVector = allocator->ScratchBitVector1;
   unsigned                   registerAliasTag;

   assert(liveRange->IsSummary());
   assert(!liveRange->HasHardPreference);
   assert(!liveRange->IsSpilled());

   unsigned degree = 0;

   if (liveRange->IsSecondChanceGlobal) {
      return degree;
   }

   registerConflictsBitVector->clear();

   GraphColor::ConflictGraph * conflictGraph = this->ConflictGraph;
   GraphColor::GraphIterator iter;

   // foreach_conflict_liverange
   for (GraphColor::LiveRange * conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&iter, liveRange);
        conflictLiveRange != nullptr;
        conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&iter)
   )
   {
      if (conflictLiveRange->IsSecondChanceGlobal || conflictLiveRange->IsSpilled()) {
         continue;
      }

      registerAliasTag = aliasInfo->GetTag(conflictLiveRange->Register);

      if (!aliasInfo->CommonMayPartialTags(registerAliasTag, registerConflictsBitVector)) {
         registerConflictsBitVector->set(registerAliasTag);
         degree++;
      }
   }

   return degree;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile summary live range register by alias tag
//
// Arguments:
//
//    aliasTag - Alias tag to test for membership in a summary.
//
// Notes:
//
//    This API is separate from the more general "GetAllocatedRegister" because that relies on an initialized
//    global aliasTagToLiveRangeId.  This API can be called on a nested or parent tile when that tile is
//    not the initialized tile. Though look up is slower.
//
// Returns:
//
//    Summary register or nullptr for none
//
//------------------------------------------------------------------------------

unsigned
Tile::GetSummaryRegister
(
   unsigned aliasTag
)
{
   GraphColor::LiveRange * summaryLiveRange = this->GetSummaryLiveRange(aliasTag);

   if (summaryLiveRange != nullptr) {
      llvm::SparseBitVector<> *  recalculateAliasTagSet = this->RecalculateAliasTagSet;
      //llvm::SparseBitVector<> *  foldAliasTagSet = this->RecalculateAliasTagSet;
      GraphColor::LiveRange *    globalLiveRange = this->Allocator->GetGlobalLiveRange(aliasTag);

      // Check to see if this is a global that has had a different action than allocate.  

      if (globalLiveRange != nullptr) {
         unsigned globalAliasTag = globalLiveRange->VrTag;

         if (recalculateAliasTagSet->test(globalAliasTag) /*|| foldAliasTagSet->test(globalAliasTag)*/) {
            return VR::Constants::InvalidReg;
         }
      }

      return summaryLiveRange->Register;

   } else {
      return VR::Constants::InvalidReg;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get tile summary live range enumerator.
//
// Remarks:
//
//     Currently we just use the vector as the basis for enumeration.
//
//------------------------------------------------------------------------------

GraphColor::LiveRangeEnumerator *
Tile::GetSummaryLiveRangeEnumerator
(
)
{
   GraphColor::LiveRangeVector * summaryLiveRangeVector = this->SummaryLiveRangeVector;

   return summaryLiveRangeVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Begin pass on a tile.
//
// Arguments:
//
//    pass - Operative pass in the allocator
//
// Remarks:
//
//    Pass::Allocate - Create all the necessary data structures to for the allocator to
//    run on the tile.
//
// Note:
//
//    Pass passed as parameter to improve 'self documentation'
//
//------------------------------------------------------------------------------

void
Tile::BeginPass
(
   GraphColor::Pass pass
)
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;
   GraphColor::Allocator * allocator = tileGraph->Allocator;
   Graphs::FlowGraph *     functionUnit = allocator->FunctionUnit;

   assert(this->Pass == GraphColor::Pass::Ready);

   this->Pass = pass;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End allocation pass on a tile.
//
// Remarks
//
//    Tear down all the data structures used to by the allocator to 
//    perform allocation on the tile.
//
//------------------------------------------------------------------------------

void
Tile::EndPass
(
   GraphColor::Pass pass
)
{
   assert(this->Pass == pass);

   this->Pass = GraphColor::Pass::Ready;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Begin allocation iteration on a tile.
//
// Remarks
//
//    Create all the necessary data structures to for the allocator to
//    run a single iteration the tile.
//
//------------------------------------------------------------------------------

void
Tile::BeginIteration
(
   GraphColor::Pass pass
)
{
   // Set the lifetime for this iteration.  Set this before building any per iteration
   // data structure for tile.

   // All passes

   GraphColor::TileGraph * tileGraph = this->TileGraph;
   GraphColor::Allocator * allocator = tileGraph->Allocator;
   Graphs::FlowGraph *     functionUnit = allocator->FunctionUnit;

   GraphColor::LiveRangeVector *  liveRangeVector = new GraphColor::LiveRangeVector();
   liveRangeVector->reserve(256);
   this->LiveRangeVector = liveRangeVector;

   UnionFind::Tree *              coloredRegisterTree = UnionFind::Tree::New();
   this->ColoredRegisterTree = coloredRegisterTree;

   GraphColor::LiveRangeVector *  preferenceLiveRangeVector = new GraphColor::LiveRangeVector();
   preferenceLiveRangeVector->reserve(256);
   this->PreferenceLiveRangeVector = preferenceLiveRangeVector;

   // Reset search controls.

   allocator->SearchNumber = 0;
   allocator->SearchLiveRangeId = 0;

   if (pass == GraphColor::Pass::Allocate) {
      // Build the empty live range vector, conflict graph and preference graph.

      GraphColor::ConflictGraph *          conflictGraph = GraphColor::ConflictGraph::New(this);
      GraphColor::SummaryConflictGraph *   summaryConflictGraph = GraphColor::SummaryConflictGraph::New(this);
      GraphColor::PreferenceGraph *        preferenceGraph = nullptr;
      GraphColor::SummaryPreferenceGraph * summaryPreferenceGraph = nullptr;
      GraphColor::AvailableExpressions *   availableExpressions = GraphColor::AvailableExpressions::New(this);

      // Only generate preference graph when optimizations are enabled.

      preferenceGraph = GraphColor::PreferenceGraph::New(this);
      summaryPreferenceGraph = GraphColor::SummaryPreferenceGraph::New(this);

      TargetRegisterAllocInfo *     targetRegisterAllocator = allocator->TargetRegisterAllocator;
      GraphColor::SpillOptimizer *  spillOptimizer = allocator->SpillOptimizer;

      if (this->Iteration <= spillOptimizer->CallerSaveIterationLimit) {
         std::vector<unsigned> *      registerCategoryIdToAllocatableRegisterCount
            = allocator->RegisterCategoryIdToAllocatableRegisterCount;
         const llvm::TargetRegisterClass * baseIntegerRegisterCategory = allocator->BaseIntegerRegisterCategory;
         const llvm::TargetRegisterClass * baseFloatRegisterCategory = allocator->BaseFloatRegisterCategory;

         conflictGraph->DoPressureCalculation = true;
         conflictGraph->MaxIntegerRegisterCount
            = ((*registerCategoryIdToAllocatableRegisterCount)[baseIntegerRegisterCategory->getID()]
               + targetRegisterAllocator->IntegerPressureAdjustment());
         conflictGraph->MaxFloatRegisterCount
            = ((*registerCategoryIdToAllocatableRegisterCount)[baseFloatRegisterCategory->getID()]
               + targetRegisterAllocator->FloatPressureAdjustment());
      }

      this->ConflictGraph = conflictGraph;
      this->SummaryConflictGraph = summaryConflictGraph;
      this->PreferenceGraph = preferenceGraph;
      this->SummaryPreferenceGraph = summaryPreferenceGraph;
      this->AvailableExpressions = availableExpressions;

      // Initialize the available expression component.
      this->AvailableExpressions->Initialize(this);

   } else {
      assert(pass == GraphColor::Pass::Assign);

      assert(this->ConflictGraph == nullptr);
      assert(this->PreferenceGraph == nullptr);
      assert(this->SummaryConflictGraph != nullptr);
      assert(this->SummaryPreferenceGraph != nullptr);

      this->ConflictGraph = this->SummaryConflictGraph;
      this->PreferenceGraph = this->SummaryPreferenceGraph;
   }

   // Reset the live range vector.
   this->ResetLiveRangeVector();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Reset the live range vector.  
//
//------------------------------------------------------------------------------

void
Tile::ResetLiveRangeVector()
{
   GraphColor::LiveRangeVector * liveRangeVector = this->LiveRangeVector;

   // Reserve live range id 0, start live range allocation at 1.
   liveRangeVector->clear();
   liveRangeVector->push_back(nullptr);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Free memory associated with tile for a given iteration.
//
//------------------------------------------------------------------------------

void
Tile::FreeMemory()
{
   delete this->LiveRangeVector;
   this->LiveRangeVector = nullptr;

   this->ConflictGraph->Delete();
   this->ConflictGraph = nullptr;

   this->PreferenceGraph->Delete();
   this->PreferenceGraph = nullptr;

   delete this->PreferenceLiveRangeVector;
   this->PreferenceLiveRangeVector = nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End allocation iteration on a tile.
//
// Remarks
//
//    Tear down all the data structures used to by the allocator to 
//    perform allocation iteration on the tile.
//
//------------------------------------------------------------------------------

void
Tile::EndIteration
(
   GraphColor::Pass pass
)
{
   // Reset (zero out) the live range map before building.

   GraphColor::Allocator * allocator = this->Allocator;

   allocator->ResetAliasTagToIdMap();
   this->FreeMemory();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Test for assignment of a given operand appearance
//
// Arguments:
//
//    operand - Operand to test.
//
// Returns:
//
//    Returns true if the live range was allocated a register
//
//------------------------------------------------------------------------------

bool
Tile::IsAssigned
(
   llvm::MachineOperand * operand
)
{
   if (!operand->isReg()) {
      return false;
   }

   unsigned tag = this->VrInfo->GetTag(operand->getReg());
      //the getReg() call still returns the virtual register

   GraphColor::LiveRange * liveRange = this->GetLiveRange(tag);
   assert(liveRange != nullptr);

   if (liveRange->IsAssigned()) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
// Description:
//
//    Allocate a tile live range a given register resource.  
//
// Arguments:
//
//    liveRange - live range to allocate to
//    reg       - resource to allocate
//
// Notes:
//
//    Track resource in a tile union find tree out of which summary live range will be computed.
//
//------------------------------------------------------------------------------

void
Tile::Allocate
(
   GraphColor::LiveRange * liveRange,
   unsigned                reg
)
{
   // Allocate resource to live range
   liveRange->Allocate(reg);

   // Update tile allocated resource tracking

   Tiled::VR::Info *   vrInfo = this->VrInfo;
   unsigned            registerAliasTag = vrInfo->GetTag(reg);
   UnionFind::Tree *   tree = this->ColoredRegisterTree;
   UnionFind::Member * member = tree->Lookup(registerAliasTag);

   if (member != nullptr) {
      // If a prior insert has added this register to the tree then we
      // don't need to repeat it.  (because of the fact that alias
      // total tags are not ordered we can't do this same opt out below)
      return;
   }

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_must_total_alias_of_tag(totalTag, registerAliasTag, aliasInfo)
   //{
      GraphColor::SummaryLiveRangeMember * summaryLiveRangeMember;

      summaryLiveRangeMember = GraphColor::SummaryLiveRangeMember::New(this, /*totalTag*/ registerAliasTag);

      tree->Insert(summaryLiveRangeMember);
   //}
   //next_must_total_alias_of_tag;
}

//------------------------------------------------------------------------------
// Description:
//
//    Merge allocated resources into minimal number of disjoint sets.
//
//------------------------------------------------------------------------------

void
Tile::UnionAllocated()
{
   // Make sure we have union find members for all registers given live ranges in this tile.

   //Tiled::VR::Info *                 vrInfo = this->VrInfo;
   GraphColor::Allocator *           allocator = this->Allocator;
   const llvm::TargetRegisterInfo *  TRI = allocator->TRI;
   GraphColor::LiveRangeVector *     lrVector = this->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned lr = 1; lr < lrVector->size(); ++lr)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[lr];

     if (liveRange->IsPhysicalRegister) {
         unsigned reg = liveRange->Register;

         if (reg != TRI->getFrameRegister(*(allocator->MF))) {
            this->Allocate(liveRange, reg);
         }
      }
   }

   //UnionFind::Tree *  tree = this->ColoredRegisterTree;
   //Tiled::VR::Info *  aliasInfo = this->VrInfo;

   unsigned allocatedTag1;
   unsigned allocatedTag2;
   UnionFind::IntToMemberMap *  memberMap = this->ColoredRegisterTree->MemberMap;
   UnionFind::IntToMemberMap::iterator mb;

   // foreach_root_member_in_unionfind
   for (mb = memberMap->begin(); mb != memberMap->end(); ++mb)
   {
      UnionFind::Member * member1 = mb->second;
      allocatedTag1 = member1->Name;

      UnionFind::IntToMemberMap::iterator nmb;

      // foreach_root_member_in_unionfind
      for (nmb = memberMap->begin(); nmb != memberMap->end(); ++nmb)
      {
         UnionFind::Member * member2 = nmb->second;
         allocatedTag2 = member2->Name;

         if (allocatedTag2 == allocatedTag1) {
            break;
         }

         // For architectures without sub-registers this conditions for tree->Union() will never be true.
         //if (aliasInfo->MayPartiallyOverlap(allocatedTag2, allocatedTag1)) {
         // <place for code supporting architectures with sub-registers>

      }
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Prune summary live ranges that have not actually summarized any live ranges.  Reset summary 
//    live range ids as appropriate.
//
//------------------------------------------------------------------------------

void
Tile::PruneSummaryLiveRanges()
{
   GraphColor::LiveRangeVector * summaryLiveRangeVector = this->SummaryLiveRangeVector;
   unsigned                      maxId = 1;

   // foreach_LiveRange_in_Vector
   GraphColor::LiveRangeVector::iterator slr = summaryLiveRangeVector->begin();

   for (++slr /*vector-base1*/; slr != summaryLiveRangeVector->end(); ++slr)
   {
      GraphColor::LiveRange * summaryLiveRange = *slr;
      if (summaryLiveRange == nullptr) {
         continue;
      }

      llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;

      if (summaryLiveRange->IsPhysicalRegister || !summaryAliasTagSet->empty()) {
         (*summaryLiveRangeVector)[maxId] = summaryLiveRange;

         if (summaryLiveRange->Id != maxId) {
            summaryLiveRange->ResetId(maxId);

            if (!summaryLiveRange->IsSecondChanceGlobal) {
               llvm::SparseBitVector<>::iterator a;

               // foreach_sparse_bv_bit
               for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
               {
                  unsigned aliasTag = *a;

                  this->MapAliasTagToSummaryId(aliasTag, maxId);

                  GraphColor::LiveRange * liveRange = this->GetLiveRange(aliasTag);
                  liveRange->SummaryLiveRangeId = maxId;
               }
            }
         }

         maxId++;
      }
   }

   summaryLiveRangeVector->resize(maxId);

   // Map pruned summary live ranges into summary live range hash.

   // foreach_LiveRange_in_Vector
   slr = summaryLiveRangeVector->begin();

   for (++slr /*vector-base1*/; slr != summaryLiveRangeVector->end(); ++slr)
   {
      GraphColor::LiveRange * summaryLiveRange = *slr;
      unsigned summaryLiveRangeId = summaryLiveRange->Id;

      this->MapAliasTagToSummaryId(summaryLiveRange->VrTag, summaryLiveRangeId);

      llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
      llvm::SparseBitVector<>::iterator a;

      // foreach_sparse_bv_bit
      for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
      {
         unsigned aliasTag = *a;

         this->MapAliasTagToSummaryId(aliasTag, summaryLiveRangeId);
      }
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Propagate definition and use information up from nested tiles.  Run during summarization we collect
//    global definition and use information for pass 2 and then add in any information from nested tiles. 
//
//------------------------------------------------------------------------------

void
Tile::PropagateGlobalAppearances()
{
   GraphColor::TileList * nestedTiles = this->NestedTileList;
   GraphColor::TileList::iterator ntiter;

   llvm::SparseBitVector<> * globalUsedAliasTagSet = this->GlobalUsedAliasTagSet;
   llvm::SparseBitVector<> * globalDefinedAliasTagSet = this->GlobalDefinedAliasTagSet;
   llvm::SparseBitVector<> * globalTransitiveUsedAliasTagSet = this->GlobalTransitiveUsedAliasTagSet;
   llvm::SparseBitVector<> * globalTransitiveDefinedAliasTagSet = this->GlobalTransitiveDefinedAliasTagSet;

   *globalTransitiveDefinedAliasTagSet = *globalDefinedAliasTagSet;
   *globalTransitiveUsedAliasTagSet = *globalUsedAliasTagSet;

   // foreach_nested_tile_in_tile
   for (ntiter = nestedTiles->begin(); ntiter != nestedTiles->end(); ++ntiter)
   {
      GraphColor::Tile * nestedTile = *ntiter;

      *globalTransitiveUsedAliasTagSet |= *(nestedTile->GlobalTransitiveUsedAliasTagSet);
      *globalTransitiveDefinedAliasTagSet |= *(nestedTile->GlobalTransitiveDefinedAliasTagSet);
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Reuse local live ranges as summary for tile allocated resources.
//
//------------------------------------------------------------------------------

void
Tile::ReuseLiveRangesAsSummary()
{
   // Assign the live range vector as the summary live range vector.
   // This allows 'RewriteRegisters' to be runable in pass 1 to short
   // circuit reallocation.

   this->SummaryLiveRangeVector = this->LiveRangeVector;
}

//------------------------------------------------------------------------------
// Description:
//
//    Build up summary live range for tile allocated resources.
//
//------------------------------------------------------------------------------

void
Tile::BuildSummaryLiveRanges()
{
   Tiled::VR::Info *                    vrInfo = this->VrInfo;
   GraphColor::Allocator *              allocator = this->Allocator;
   unsigned                             maxRegisterAliasTag = VR::Constants::InvalidTag;
   unsigned                             registerAliasTag = VR::Constants::InvalidTag;
   unsigned                             reg;
   GraphColor::LiveRange *              summaryLiveRange;
   unsigned                             summaryLiveRangeId;
   GraphColor::SummaryLiveRangeMember * summaryLiveRangeMember;

   // Create summary live ranges for physical registers referenced or allocatable by this tile.

   GraphColor::LiveRangeVector * lrVector = this->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned lr = 1; lr < lrVector->size(); ++lr)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[lr];

      if (liveRange->IsPhysicalRegister) {
         registerAliasTag = liveRange->VrTag;
         reg              = liveRange->Register;
         assert(registerAliasTag == vrInfo->GetTag(reg));

         summaryLiveRange = this->AddSummaryLiveRange(registerAliasTag, reg);
         summaryLiveRange->IsPhysicalRegister = true;
         summaryLiveRange->IsPreColored = true;
      }
   }

   llvm::MachineRegisterInfo&  MRI = this->MF->getRegInfo();

   // Create a summary live range for each union find root.  Union-find tree has done the merge.

   UnionFind::IntToMemberMap *  memberMap = this->ColoredRegisterTree->MemberMap;
   UnionFind::IntToMemberMap::iterator mb;

   // foreach_root_member_in_unionfind
   for (mb = memberMap->begin(); mb != memberMap->end(); ++mb)
   {
      UnionFind::Member * rootMember = mb->second;
      if (!rootMember->IsRoot()) continue;

      // Find the max overlapping member/register associated with summary live range.
      summaryLiveRangeMember = static_cast<GraphColor::SummaryLiveRangeMember *>(rootMember);
      maxRegisterAliasTag = summaryLiveRangeMember->Name;

      // foreach_equivalent_unionfind_member loop finding biggest/parent register
      // needs to be added here for architectures with sub-registers.

      // Create new temporary associated with summary live range

      unsigned  reg = vrInfo->GetRegister(maxRegisterAliasTag);

      const llvm::TargetRegisterClass *  regClass = allocator->GetRegisterCategory(reg);
      unsigned  vrtReg = MRI.createVirtualRegister(regClass);
      unsigned  aliasTag = vrInfo->GetTag(vrtReg);

      // Create new summary live range and add it to tile.
      summaryLiveRange = this->AddSummaryLiveRange(aliasTag, reg);

      // Mark as hard preference if register is killed in tile
      if (vrInfo->CommonMayPartialTags(maxRegisterAliasTag, this->KilledRegisterAliasTagSet)) {
         summaryLiveRange->HasHardPreference = true;
      }

      // Assign summary live range id to summary live range member.
      summaryLiveRangeId = summaryLiveRange->Id;
      summaryLiveRangeMember->SummaryLiveRangeId = summaryLiveRangeId;

   }

   // Add second chance globals based on the the transparent globals spilled in this tile.

   unsigned                   globalAliasTag;
   llvm::SparseBitVector<> *  globalSpillAliasTagSet = this->GlobalSpillAliasTagSet;
   llvm::SparseBitVector<> *  secondChanceGlobalBitVector = allocator->ScratchBitVector1;

   secondChanceGlobalBitVector->clear();
   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = globalSpillAliasTagSet->begin(); g != globalSpillAliasTagSet->end(); ++g)
   {
      unsigned globalAliasTag = *g;

      if (this->IsTransparent(globalAliasTag)) {
         GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(globalAliasTag);
         assert(globalLiveRange != nullptr);

         //a skip/continue section needed for LRs of X87-Floats

         // Don't create second chance global candidates for callee save values where the callee save value
         // register is killed in the tile.

         if (globalLiveRange->IsCalleeSaveValue) {
            unsigned calleeSaveValueRegister = globalLiveRange->CalleeSaveValueRegister;
            unsigned calleeSaveValueRegisterAliasTag = vrInfo->GetTag(calleeSaveValueRegister);

            if (vrInfo->CommonMayPartialTags(calleeSaveValueRegisterAliasTag,this->KilledRegisterAliasTagSet)) {
               continue;
            }
         }

         secondChanceGlobalBitVector->set(globalAliasTag);

         // Add the new summary live range

         unsigned  reg = globalLiveRange->Register;

         unsigned  Reg = reg;
         if (Reg == VR::Constants::InitialPseudoReg || Reg == VR::Constants::InvalidReg) {
            Reg = this->Allocator->VrInfo->GetRegister(globalLiveRange->VrTag);
         }
         const llvm::TargetRegisterClass *  regClass = allocator->GetRegisterCategory(Reg);

         unsigned  vrtReg = MRI.createVirtualRegister(regClass);
         unsigned  aliasTag = vrInfo->GetTag(vrtReg);

         // Create new summary live range and add it to tile.
         GraphColor::LiveRange * secondChanceLiveRange = this->AddSummaryLiveRange(aliasTag, reg);
         secondChanceLiveRange->AddLiveRange(globalLiveRange);

         secondChanceLiveRange->IsCalleeSaveValue = globalLiveRange->IsCalleeSaveValue;
         secondChanceLiveRange->UseCount = 0;
         secondChanceLiveRange->DefinitionCount = 0;
         secondChanceLiveRange->CalleeSaveValueRegister = globalLiveRange->CalleeSaveValueRegister;

         // Mark live range as zero weight. (no local appearances/cost)
         secondChanceLiveRange->WeightCost = allocator->ZeroCost;

         secondChanceLiveRange->IsSecondChanceGlobal = true;
      }
   }

   // Remove from global spill set those globals we're going to reintroduce for allocation pass 2.
   globalSpillAliasTagSet->intersectWithComplement(*secondChanceGlobalBitVector);

}

//------------------------------------------------------------------------------
// Description:
//
//    Map live ranges to summary live ranges for tile allocated resources.
//
//------------------------------------------------------------------------------

void
Tile::MapLiveRangesToSummaryLiveRanges
(
   llvm::SparseBitVector<> * rewriteAliasTagBitVector
)
{
   Tiled::VR::Info *                    vrInfo = this->VrInfo;
   UnionFind::Tree *                    tree = this->ColoredRegisterTree;
   unsigned                             registerAliasTag;
   unsigned                             summaryLiveRangeId;
   GraphColor::SummaryLiveRangeMember * summaryLiveRangeMember;
   GraphColor::LiveRange *              summaryLiveRange;
   llvm::SparseBitVector<> *            boundarySpillAliasTagSet = this->Allocator->ScratchBitVector2;

   rewriteAliasTagBitVector->clear();
   boundarySpillAliasTagSet->clear();

   // Map allocated live ranges (excepting callee save values - these
   // need to be filtered out if they've been allocated as a proxy)

   GraphColor::LiveRangeVector * lrVector = this->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned lr = 1; lr < lrVector->size(); ++lr)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[lr];

      if (liveRange->IsSpilled() || (liveRange->IsCalleeSaveValue && liveRange->IsSummaryProxy)) {
         continue;
      }

      unsigned liveRangeRegister = liveRange->Register;
      registerAliasTag = vrInfo->GetTag(liveRangeRegister);

      if (liveRange->IsPhysicalRegister) {
         summaryLiveRange = this->GetRegisterSummaryLiveRange(registerAliasTag);
         assert(summaryLiveRange != nullptr);

         summaryLiveRangeId = summaryLiveRange->Id;
      } else {
         UnionFind::Member * root = tree->Find(registerAliasTag);
         assert(root != nullptr);

         summaryLiveRangeMember = static_cast<GraphColor::SummaryLiveRangeMember*>(root);
         summaryLiveRangeId = summaryLiveRangeMember->SummaryLiveRangeId;
         summaryLiveRange = this->GetSummaryLiveRangeById(summaryLiveRangeId);
      }

      unsigned liveRangeAliasTag = liveRange->VrTag;

      if (summaryLiveRange->Register != liveRangeRegister) {
         // <place for code supporting architectures with sub-registers>
      }

      // Update global preferences

      if (liveRange->IsGlobal()) {

         GraphColor::LiveRange * globalLiveRange = liveRange->GlobalLiveRange;
         unsigned                globalLiveRangeAliasTag = globalLiveRange->VrTag;

         if (liveRange->HasUse()) {
            this->GlobalUsedAliasTagSet->set(globalLiveRangeAliasTag);
         }

         if (liveRange->HasDefinition()) {
            this->GlobalDefinedAliasTagSet->set(globalLiveRangeAliasTag);
         }

         // Keep track of hard preferences

         if (summaryLiveRange->HasHardPreference && !globalLiveRange->IsCalleeSaveValue) {
            llvm::SparseBitVector<> * globalHardPreferenceRegisterAliasTagSet =
               globalLiveRange->GlobalHardPreferenceRegisterAliasTagSet;
            globalHardPreferenceRegisterAliasTagSet->set(registerAliasTag);
         }
      }

      summaryLiveRange->AddLiveRange(liveRange);

      assert(liveRange->SummaryLiveRangeId == summaryLiveRangeId);
   }

   // Map in callee save values where appropriate.

   // foreach_liverange_in_tile
   for (unsigned lr = 1; lr < lrVector->size(); ++lr)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[lr];

      if (!(liveRange->IsCalleeSaveValue && liveRange->IsSummaryProxy)) {
         continue;
      }

      unsigned liveRangeRegister = liveRange->Register;
      registerAliasTag = vrInfo->GetTag(liveRangeRegister);
      assert(!liveRange->IsPhysicalRegister && registerAliasTag != VR::Constants::InvalidTag);

      UnionFind::Member * root = tree->Find(registerAliasTag);
      assert(root != nullptr);

      summaryLiveRangeMember = static_cast<GraphColor::SummaryLiveRangeMember*>(root);
      summaryLiveRangeId = summaryLiveRangeMember->SummaryLiveRangeId;
      summaryLiveRange = this->GetSummaryLiveRangeById(summaryLiveRangeId);

      unsigned liveRangeAliasTag = liveRange->VrTag;

      if (summaryLiveRange->Register != liveRangeRegister) {
         // <place for code supporting architectures with sub-registers>
      }

      assert(!liveRange->IsGlobal());

      if (summaryLiveRange->IsCalleeSaveValue || this->IsRoot()) {
         summaryLiveRange->AddLiveRange(liveRange);

         assert(liveRange->SummaryLiveRangeId == summaryLiveRangeId);
      } else {
         boundarySpillAliasTagSet->set(liveRangeAliasTag);
      }
   }

   // After we've fully mapped in live ranges and determined what will be propagated as callee save force all
   // callee saves to have no hard preference.  Since callee saves can always be spilled and already conflict
   // with all other registers but the one they can be allocated hard preference can just cause problems.

   GraphColor::LiveRangeVector * slrVector = this->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];
      if (summaryLiveRange->IsCalleeSaveValue) {
         summaryLiveRange->HasHardPreference = false;
      }
   }

   // Do any needed spilling.
   if (!boundarySpillAliasTagSet->empty()) {
      this->Allocator->SpillAtBoundary(this, boundarySpillAliasTagSet);
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Rewrite operands to denote when constrained registers are required (e.g. high registers of register pair)
//
//------------------------------------------------------------------------------

void
Tile::RewriteConstrainedRegisters
(
   llvm::SparseBitVector<> * rewriteAliasTagBitVector
)
{
   // <place for code supporting architectures with sub-registers>
}

//------------------------------------------------------------------------------
// Description:
//
//    Update enter tile and exit tile instructions to have operands for summary live ranges and appropriate
//    register set side-effects.
//
//------------------------------------------------------------------------------

void
Tile::UpdateEnterTileAndExitTileInstructions()
{
   Tiled::VR::Info *                              aliasInfo = this->VrInfo;
   GraphColor::Allocator *                        allocator = this->Allocator;
   llvm::MachineInstr *                           enterTileInstruction;
   llvm::MachineInstr *                           exitTileInstruction;
   unsigned                                       reg;
   llvm::SparseBitVector<> *                      killedRegisterAliasTagSet = this->KilledRegisterAliasTagSet;
   llvm::MachineOperand * /*AliasOperand*/        killedRegisterAliasOperand = nullptr;
   llvm::MachineRegisterInfo&                     MRI = this->MF->getRegInfo();

   // Build up destination lists on enter tile instructions.
   Graphs::MachineBasicBlockList * entryBlockList = this->EntryBlockList;
   Graphs::MachineBasicBlockList::iterator b;

   // foreach_BasicBlock_in_List
   for (b = entryBlockList->begin(); b != entryBlockList->end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;
      enterTileInstruction = this->FindNextInstruction(block, block->instr_begin(), llvm::TargetOpcode::ENTERTILE);

      GraphColor::LiveRangeVector * slrVector = this->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned slr = 1; slr < slrVector->size(); ++slr)
      {
         GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];

         if (summaryLiveRange->IsPhysicalRegister || summaryLiveRange->IsSecondChanceGlobal) {
            continue;
         }

         if (!summaryLiveRange->HasHardPreference) {
            //TODO: reg = targetRegisterAllocator->GetPseudoRegister(reg);
            reg = aliasInfo->GetRegister(summaryLiveRange->VrTag);
         } else {
            reg = summaryLiveRange->Register;
         }

         // creating and appending an IsDef & IsImpl[icit] operand
         enterTileInstruction->addRegisterDefined(reg);

         //TODO: operand->CannotMakeExpressionTemporary = true;
      }

      if (!killedRegisterAliasTagSet->empty()) {
         // Create register side-effect operand (if required);

         // append one operand per tag in killedRegisterAliasTagSet.
         llvm::SparseBitVector<>::iterator a;

         // foreach_sparse_bv_bit
         for (a = killedRegisterAliasTagSet->begin(); a != killedRegisterAliasTagSet->end(); ++a)
         {
            enterTileInstruction->addRegisterDefined(aliasInfo->GetRegister(*a));
            //llvm::dbgs() << " ENTER#" << enterTileInstruction->getParent()->getNumber() << ": registerDefined = " << aliasInfo->GetRegister(*a) << "\n";
         }
      }
   }

   // Build up source lists on exit tile instructions.
   Graphs::MachineBasicBlockList * exitBlockList = this->ExitBlockList;

   // foreach_BasicBlock_in_List
   for (b = exitBlockList->begin(); b != exitBlockList->end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;
      exitTileInstruction = this->FindNextInstruction(block, block->instr_begin(), llvm::TargetOpcode::EXITTILE);

      GraphColor::LiveRangeVector * slrVector = this->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned slr = 1; slr < slrVector->size(); ++slr)
      {
         GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];

         if (summaryLiveRange->IsPhysicalRegister || summaryLiveRange->IsSecondChanceGlobal) {
            continue;
         }

         if (!summaryLiveRange->HasHardPreference) {
            reg = aliasInfo->GetRegister(summaryLiveRange->VrTag);
         } else {
            reg = summaryLiveRange->Register;
         }

         //TODO: operand->CannotMakeExpressionTemporary = true;

         exitTileInstruction->addOperand((*this->MF), llvm::MachineOperand::CreateReg(reg, false));
     }
   }
}


llvm::MachineInstr *
Tile::FindNextInstruction
(
   llvm::MachineBasicBlock *                block,
   llvm::MachineBasicBlock::instr_iterator  i,
   unsigned                                 opcode
)
{
   for (; i != block->instr_end(); ++i)
   {
      if (i->getOpcode() == opcode) {
         return &*i;
      }
   }

   return nullptr;
}

llvm::MachineInstr *
Tile::FindPreviousInstruction
(
   llvm::MachineBasicBlock *                        block,
   llvm::MachineBasicBlock::reverse_instr_iterator  i,
   unsigned                                         opcode
)
{
   for (; i != block->instr_rend(); ++i)
   {
      if (i->getOpcode() == opcode) {
         return &*i;
      }
   }

   return nullptr;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Update global conflicts for globals crossing this tile.
//
//------------------------------------------------------------------------------

void
Tile::UpdateGlobalConflicts()
{
   GraphColor::Allocator * allocator = this->Allocator;
   bool                    changedLiveness = false;

   // for each global crossing this tile update tile conflicts and
   // then update the global scope.

   llvm::SparseBitVector<> * globalAliasTagSet = this->GlobalAliasTagSet;
   llvm::SparseBitVector<> * globalSpillAliasTagSet = this->GlobalSpillAliasTagSet;
   //unsigned globalLiveRangeAliasTag;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = globalAliasTagSet->begin(); a != globalAliasTagSet->end(); ++a)
   {
      unsigned globalLiveRangeAliasTag = *a;

      if (globalSpillAliasTagSet->test(globalLiveRangeAliasTag)) {
         GraphColor::LiveRange * liveRange = this->GetLiveRange(globalLiveRangeAliasTag);

         GraphColor::LiveRange * globalLiveRange = allocator->GetGlobalLiveRange(globalLiveRangeAliasTag);
         unsigned                globalLiveRangeId = globalLiveRange->Id;

         llvm::SparseBitVector<> * globalLiveRangeConflicts
            = (*this->GlobalLiveRangeConflictsVector)[globalLiveRangeId];

         if (liveRange == nullptr) {
            // full spill, the global no longer is live in the tile.
            // Remove all conflicts.

            if (globalLiveRangeConflicts != nullptr) {
               delete globalLiveRangeConflicts;
               (*this->GlobalLiveRangeConflictsVector)[globalLiveRangeId] = nullptr;

               changedLiveness = true;
            }

         } else if (globalLiveRangeConflicts != nullptr) {
            globalLiveRangeConflicts->clear();

            // Rebuild conflicts from local conflict set.
            GraphColor::ConflictGraph * conflictGraph = this->ConflictGraph;
            GraphColor::GraphIterator iter;

            // foreach_conflict_liverange
            for (GraphColor::LiveRange * conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&iter, liveRange);
                 conflictLiveRange != nullptr;
                 conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&iter)
            )
            {
               if (conflictLiveRange->IsPhysicalRegister) {
                  globalLiveRangeConflicts->set(conflictLiveRange->VrTag);
               }
            }

            changedLiveness = true;
         }
      }
   }

   if (changedLiveness) {
      // If there have been any changes update globals

      GraphColor::ConflictGraph::UpdateGlobalConflicts(globalAliasTagSet, allocator);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build summary conflicts
//
//------------------------------------------------------------------------------

void
Tile::BuildSummaryConflicts()
{
   GraphColor::SummaryConflictGraph * summaryConflictGraph = this->SummaryConflictGraph;

   summaryConflictGraph->Build();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build summary preferences
//
//------------------------------------------------------------------------------

void
Tile::BuildSummaryPreferences()
{
   GraphColor::PreferenceGraph * preferenceGraph = this->PreferenceGraph;

   // Remove costing used to guide global live range allocation.

   if (preferenceGraph != nullptr) {
      preferenceGraph->SubtractCostForGlobalPreferenceEdges();
   }

   // Map remaining preferences.

   GraphColor::SummaryPreferenceGraph * summaryPreferenceGraph = this->SummaryPreferenceGraph;

   if (summaryPreferenceGraph != nullptr) {
      summaryPreferenceGraph->Build();
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear tile live ranges assignments.
//
// Notes:
//
//   Remove pass one assignments and turn them back to pseudos in preparation for pass to reallocation.
//
//------------------------------------------------------------------------------

void
Tile::ClearAllocations()
{
   assert(this->Allocator->Pass == GraphColor::Pass::Assign);
   assert(this->SummaryLiveRangeVector != nullptr);

   // Mark any over constrained live ranges as 'hard preference'
   // before clearing allocations.

   GraphColor::LiveRangeVector * slrVector = this->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *  liveRange = (*slrVector)[slr];

      if (liveRange->IsPhysicalRegister || liveRange->HasHardPreference
         || liveRange->IsSpilled() || liveRange->IsCalleeSaveValue) {
         continue;
      }

      // Check that the live range we're setting up for reallocation is guaranteed colorable.  In the case of
      // byteable registers on x86 different allocation order in pass 2 can make us uncolorable.

      // Pessimistically uncolorable case rely on Allocation passes decision.

      unsigned allocatableRegisterCount = this->Allocator->NumberAllocatableRegisters(liveRange);

      unsigned degree = this->GetSummaryDegree(liveRange);

      if (!(degree + 1 < allocatableRegisterCount)) {
         liveRange->HasHardPreference = true;
      }
   }

   // foreach_summaryliverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *  liveRange = (*slrVector)[slr];

      if (liveRange->IsPreColored || liveRange->HasHardPreference || liveRange->IsSpilled()) {
         continue;
      }

      // if the architecture has multiple register types, the type would have to be retrieved from member LRs
      liveRange->Register = VR::Constants::InitialPseudoReg;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Rebuild tile for second pass.
//
// Notes:
//
//    Recreates the live range vector, alias tag mapping, conflict graph, and preference graph from summary
//    information.
//
//------------------------------------------------------------------------------

void
Tile::Rebuild()
{
   GraphColor::LiveRangeVector *  liveRangeVector = this->LiveRangeVector;
   unsigned                       summaryCount = this->SummaryLiveRangeCount();

   // Initialize a new live range vector from summary for second pass.

   assert(liveRangeVector != nullptr);
   assert(liveRangeVector->size() == 1);

   liveRangeVector->resize(summaryCount + 1, nullptr);

   GraphColor::LiveRangeVector * lrVector = this->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned i = 1; i < lrVector->size(); ++i)
   {
      GraphColor::LiveRange *  summaryLiveRange = (*lrVector)[i];

      unsigned summaryLiveRangeId = summaryLiveRange->Id;
      assert(summaryLiveRangeId != 0);
      (*liveRangeVector)[summaryLiveRangeId] = summaryLiveRange;
   }

   assert(this->VrInfo != nullptr);
   assert(this->AliasTagToIdVector != nullptr);

   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->AliasTagToIdVector;

   lrVector = this->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned l = 1; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];
      assert(liveRange->IsSummary());

      unsigned aliasTag;

      llvm::SparseBitVector<>::iterator a;
      llvm::SparseBitVector<> * liveRangeBitVector = liveRange->SummaryAliasTagSet;

      // foreach_sparse_bv_bit
      for (a = liveRangeBitVector->begin(); a != liveRangeBitVector->end(); ++a)
      {
         aliasTag = *a;

         //For architectures without sub-registers the commented below loop collapses to single iteration
         //foreach_may_partial_alias_of_tag(totalTag, aliasTag, aliasInfo)
         //{
            (*aliasTagToIdVector)[aliasTag] = liveRange->Id;
         //}
         //next_may_partial_alias_of_tag;
      }
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Remove availability for this operand.
//
//------------------------------------------------------------------------------

void
Tile::RemoveAvailable
(
   llvm::MachineOperand * operand
)
{
   GraphColor::Allocator *             allocator = this->Allocator;
   GraphColor::AvailableExpressions *  localAvailableExpressions = this->AvailableExpressions;
   GraphColor::AvailableExpressions *  globalAvailableExpressions = allocator->GlobalAvailableExpressions;

   localAvailableExpressions->RemoveAvailable(operand);
   globalAvailableExpressions->RemoveAvailable(operand);
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if the instruction is a tile boundary instruction (EnterTile, or ExitTile)
//
//------------------------------------------------------------------------------

bool
Tile::IsTileBoundaryInstruction
(
   llvm::MachineInstr * instruction
)
{
   assert(instruction != nullptr);

   if (Tile::IsEnterTileInstruction(instruction) || Tile::IsExitTileInstruction(instruction)) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if the instruction is an EnterTile boundary instruction.
//
//------------------------------------------------------------------------------

bool
Tile::IsEnterTileInstruction
(
   llvm::MachineInstr * instruction
)
{
   assert(instruction != nullptr);

   if (instruction->getOpcode() == llvm::TargetOpcode::ENTERTILE) {
      return true;
   }

   return false;
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if the instruction is an ExitTile boundary instruction.
//
//------------------------------------------------------------------------------

bool
Tile::IsExitTileInstruction
(
   llvm::MachineInstr * instruction
)
{
   assert(instruction != nullptr);

   if (instruction->getOpcode() == llvm::TargetOpcode::EXITTILE) {
      return true;
   }

   return false;
}

llvm::MachineInstr *
Tile::FindEnterTileInstruction
(
   llvm::MachineBasicBlock * enterTileBlock
)
{
   Graphs::FlowGraph *  flowGraph = this->FunctionFlowGraph;

   llvm::MachineInstr * enterTileInstruction
      = flowGraph->FindNextInstructionInBlock(llvm::TargetOpcode::ENTERTILE, &(*enterTileBlock->instr_begin()));

   return enterTileInstruction;
}

llvm::MachineInstr *
Tile::FindExitTileInstruction
(
   llvm::MachineBasicBlock * exitTileBlock
)
{
   Graphs::FlowGraph *  flowGraph = this->FunctionFlowGraph;

   llvm::MachineInstr * exitTileInstruction
      = flowGraph->FindPreviousInstructionInBlock(llvm::TargetOpcode::EXITTILE, &(*exitTileBlock->instr_rbegin()));

   return exitTileInstruction;
}

//------------------------------------------------------------------------------
// Description:
//
//    Set tile for basic block.
//
//------------------------------------------------------------------------------

void
TileGraph::SetTile
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile
)
{
   GraphColor::TileVector *  blockIdToTileVector = this->BlockIdToTileVector;
   unsigned                  blockId = block->getNumber();

   (*blockIdToTileVector)[blockId] = tile;
}

//------------------------------------------------------------------------------
// Description:
//
//    Get tile for basic block.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
TileGraph::GetTile
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileVector *  blockIdToTileVector = this->BlockIdToTileVector;
   unsigned                  blockId = block->getNumber();
   GraphColor::Tile *        tile;

   tile = (*blockIdToTileVector)[blockId];

   return tile;
}

//------------------------------------------------------------------------------
// Description:
//
//    Get nested tile for basic block.   Only applies to Enter/Exit Tile blocks the tile
//    the block is entering or leaving, not the tile the block is contained in.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
TileGraph::GetNestedTile
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileExtensionObject * tileExtensionObject = TileExtensionObject::GetExtensionObject(block);
   assert(tileExtensionObject != nullptr);

   GraphColor::Tile * tile = tileExtensionObject->Tile;

   return tile;
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if a global has an allocated use or def in this tile (exclusive).
//
//------------------------------------------------------------------------------

bool
Tile::HasGlobalReference
(
   GraphColor::LiveRange * globalLiveRange
)
{
   unsigned globalLiveRangeTag = globalLiveRange->VrTag;

   return this->HasGlobalReference(globalLiveRangeTag);
}

bool
Tile::HasGlobalReference
(
   unsigned globalAliasTag
)
{
   llvm::SparseBitVector<> *  globalUsedAliasTagSet = this->GlobalUsedAliasTagSet;
   llvm::SparseBitVector<> *  globalDefinedAliasTagSet = this->GlobalDefinedAliasTagSet;

   return (globalUsedAliasTagSet->test(globalAliasTag) || globalDefinedAliasTagSet->test(globalAliasTag));
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if a global has an nested or otherwise allocated use or def in this tile.
//
//------------------------------------------------------------------------------

bool
Tile::HasTransitiveGlobalReference
(
   GraphColor::LiveRange * globalLiveRange
)
{
   unsigned globalLiveRangeTag = globalLiveRange->VrTag;

   return this->HasTransitiveGlobalReference(globalLiveRangeTag);
}

bool
Tile::HasTransitiveGlobalReference
(
   unsigned globalAliasTag
)
{
   llvm::SparseBitVector<> * globalTransitiveUsedAliasTagSet = this->GlobalTransitiveUsedAliasTagSet;
   llvm::SparseBitVector<> * globalTransitiveDefinedAliasTagSet = this->GlobalTransitiveDefinedAliasTagSet;

   return (globalTransitiveUsedAliasTagSet->test(globalAliasTag)
      || globalTransitiveDefinedAliasTagSet->test(globalAliasTag));
}

//------------------------------------------------------------------------------
// Description:
//
//    Test if a global is transparent through the tile (includes nested tiles).
//
//------------------------------------------------------------------------------

bool
Tile::IsTransparent
(
   unsigned globalAliasTag
)
{
   llvm::SparseBitVector<> * globalGenerateAliasTagSet = this->GlobalGenerateAliasTagSet;
   llvm::SparseBitVector<> * globalKillAliasTagSet = this->GlobalKillAliasTagSet;
   Tiled::VR::Info *         vrInfo = this->VrInfo;

   return (!vrInfo->CommonMayPartialTags(globalAliasTag, globalGenerateAliasTagSet)
        && !vrInfo->CommonMayPartialTags(globalAliasTag, globalKillAliasTagSet));
}

//------------------------------------------------------------------------------
// Description:
//
//    Count transparent globals spilled in this tile.  This gives us a upper bound on the number of live
//    ranges that can be injected in pass 2.
//
//------------------------------------------------------------------------------

void
Tile::CountGlobalTransparentSpills()
{
   unsigned count = 0;
   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = this->GlobalSpillAliasTagSet->begin(); a != this->GlobalSpillAliasTagSet->end(); ++a)
   {
      unsigned globalSpillAliasTag = *a;
      if (this->IsTransparent(globalSpillAliasTag)) {
         count++;
      }
   }

   this->GlobalTransparentSpillCount = count;
}

void
Tile::pruneCompensatingCopysOfTile()
{
   llvm::SparseBitVector<> overwrittenOnBoundaryRegs;
   Graphs::MachineBasicBlockList::iterator bi;
   llvm::MachineBasicBlock::instr_iterator i, next_i;

   // foreach_tile_entry_block
   for (bi = this->EntryBlockList->begin(); bi != this->EntryBlockList->end(); ++bi)
   {
      llvm::MachineBasicBlock * entryBlock = *bi;

      BlockToCopysMap::iterator bc = this->TransparentBoundaryCopys.find(entryBlock->getNumber());
      bool boundaryBlockHasTransparentCopys = (bc != this->TransparentBoundaryCopys.end());

      if (boundaryBlockHasTransparentCopys) {
         llvm::SparseBitVector<> locallyCopiedRegs;

         // foreach_instr_in_block
         for (i = entryBlock->instr_begin(); i != entryBlock->instr_end(); ++i)
         {
            llvm::MachineInstr * instruction = &(*i);

            if (instruction->isCopy()) {
               assert(i->getNumOperands() == 2);
               unsigned destReg = (instruction->getOperand(0)).getReg();
               if (locallyCopiedRegs.test(destReg)) {
                  overwrittenOnBoundaryRegs.set(destReg);
               }
               locallyCopiedRegs.set((instruction->getOperand(1)).getReg());
            }
         }

         // foreach_instr_in_block_editing
         for (i = entryBlock->instr_begin(); i != entryBlock->instr_end(); i = next_i)
         {
            llvm::MachineInstr * instruction = &(*i);
            next_i = i; ++next_i;

            if (instruction->isCopy()) {
               CopyVector::iterator ci = std::find(bc->second.begin(), bc->second.end(), instruction);
               unsigned sourceReg = (instruction->getOperand(1)).getReg();

               if (ci != bc->second.end() &&
                  !this->RegistersOverwrittentInTile->test(sourceReg) && !overwrittenOnBoundaryRegs.test(sourceReg)) {
                  instruction->eraseFromParent();
               }

            }
         }
      }
   }

   // foreach_tile_entry_block
   for (bi = this->ExitBlockList->begin(); bi != this->ExitBlockList->end(); ++bi)
   {
      llvm::MachineBasicBlock * exitBlock = *bi;

      BlockToCopysMap::iterator bc = this->TransparentBoundaryCopys.find(exitBlock->getNumber());
      bool boundaryBlockHasTransparentCopys = (bc != this->TransparentBoundaryCopys.end());

      if (boundaryBlockHasTransparentCopys) {

         // foreach_instr_in_block_editing
         for (i = exitBlock->instr_begin(); i != exitBlock->instr_end(); i = next_i)
         {
            llvm::MachineInstr * instruction = &(*i);
            next_i = i; ++next_i;

            if (instruction->isCopy()) {
               assert(i->getNumOperands() == 2);
               CopyVector::iterator ci = std::find(bc->second.begin(), bc->second.end(), instruction);
               llvm::MachineOperand& destination = instruction->getOperand(0);
               unsigned destReg = (instruction->getOperand(0)).getReg();

               if (ci != bc->second.end() &&
                  !this->RegistersOverwrittentInTile->test(destReg) && !overwrittenOnBoundaryRegs.test(destReg)) {
                  instruction->eraseFromParent();
               }

            }
         }
      }
   }
}

//------------------------------------------------------------------------------
// Description:
//
//    Get tile for basic block id.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
TileGraph::GetTileByBlockId
(
   Tiled::Id blockId
)
{
   GraphColor::Tile * tile = (*this->BlockIdToTileVector)[blockId];

   return tile;
}

//------------------------------------------------------------------------------
// Description:
//
//    Get tile for tile id.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
TileGraph::GetTileByTileId
(
   unsigned tileId
)
{
   assert(tileId < this->TileVector->size());
   GraphColor::Tile * tile = (*this->TileVector)[tileId];

   return tile;
}

} // GraphColor
} // RegisterAllocator
} // Tiled
