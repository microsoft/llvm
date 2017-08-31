//===-- GraphColor/PreferenceGraph.cpp -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PreferenceGraph.h"
#include "Allocator.h"
#include "ConflictGraph.h"
#include "CostModel.h"
#include "../Dataflow/Dataflow.h"
#include "../Graphs/BitGraph.h"
#include "Liveness.h"
#include "LiveRange.h"
#include "Tile.h"
#include "TargetRegisterAllocInfo.h"
#include "SpillOptimizer.h"
#include "llvm/Target/TargetOpcodes.h"

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
//    Construct a new preference graph object for this tile.
//
//-----------------------------------------------------------------------------

GraphColor::PreferenceGraph *
PreferenceGraph::New
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *       allocator = tile->Allocator;
   GraphColor::PreferenceGraph * preferenceGraph = new GraphColor::PreferenceGraph;
   preferenceGraph->Allocator = allocator;
   preferenceGraph->Tile = tile;
   preferenceGraph->PreferenceVector = new GraphColor::PreferenceVector();
   preferenceGraph->PreferenceVector->reserve(16);   //elements pushed back in PreferenceInstruction() below

   return preferenceGraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Test if two live range ids preference each other
//
//-----------------------------------------------------------------------------

bool
PreferenceGraph::HasPreference
(
   unsigned liveRangeId1,
   unsigned liveRangeId2
)
{
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;

   return bitGraph->TestEdge(liveRangeId1, liveRangeId2);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Build the bit graph representation of the register allocation 
//    preference graph.
//
//-----------------------------------------------------------------------------

void
PreferenceGraph::BuildBitGraph()
{
   // Initialize empty bit graph.
   this->InitializeBitGraph();

   GraphColor::Tile * tile = this->Tile;

   // Process each instruction in tile looking for preferences and adding edges to the graph.

   Graphs::MachineBasicBlockVector::iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile
   for (biter = mbbVector->begin(); biter != mbbVector->end(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;
      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);

         if (PreferenceGraph::HasPreference(instruction)) {
            this->AddInstructionPreferenceEdges(instruction);
         } else if (tile->IsTileBoundaryInstruction(instruction)) {
            this->AddBoundaryPreferenceEdges(instruction);
            this->AddSummaryPreferenceEdges(instruction);
         }
      }
   }

   // Process each nested tile looking for preferences edges to 
   // summary live ranges coming up from nested tile.

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      GraphColor::LiveRangeVector * slrVector = nestedTile->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned slr = 1; slr < slrVector->size(); ++slr)
      {
         GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];
         this->AddSummaryPreferenceEdges(summaryLiveRange);
      }
   }

   // Process each global in tile looking for preferences and adding edges to the graph.
   this->AddGlobalPreferenceEdges();
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize empty bit graph representation of the register allocation 
//    preference graph.
//
//-----------------------------------------------------------------------------

void
PreferenceGraph::InitializeBitGraph()
{
   GraphColor::Tile *              tile = this->Tile;
   unsigned                        bitGraphSize = (tile->LiveRangeCount() + 1);
   BitGraphs::UndirectedBitGraph * bitGraph = BitGraphs::UndirectedBitGraph::New(bitGraphSize);

   this->BitGraph = bitGraph;
}

//---------------------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edge between two live ranges connected by a preference instruction.
//
//---------------------------------------------------------------------------------------------

void
PreferenceGraph::AddInstructionPreferenceEdges
(
   llvm::MachineInstr * instruction
)
{
   assert(PreferenceGraph::HasPreference(instruction));
#if defined(TILED_DEBUG_DUMPS)
   instruction->dump();
#endif

   GraphColor::Tile *              tile = this->Tile;
   GraphColor::Allocator *         allocator = this->Allocator;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::PreferenceVector *  preferenceVector = this->PreferenceVector;
   TargetRegisterAllocInfo *       targetRegisterAllocator = allocator->TargetRegisterAllocator;
   GraphColor::ConflictGraph *     conflictGraph = tile->ConflictGraph;

   // Calculate preference operand and costs.

   targetRegisterAllocator->PreferenceInstruction(instruction, preferenceVector);

   // Add edges in the preference bit graph

   for (unsigned i = 0, n = preferenceVector->size(); i < n; i++)
   {
      RegisterAllocator::Preference  preference = (*preferenceVector)[i];

      unsigned                definitionAliasTag = preference.AliasTag1;
      GraphColor::LiveRange * definitionLiveRange = tile->GetLiveRange(definitionAliasTag);
      if (definitionLiveRange == nullptr) {
         continue;
      }
     
      unsigned                definitionLiveRangeId = definitionLiveRange->Id;
      bool                    definitionIsRegister = definitionLiveRange->IsPhysicalRegister;
      unsigned                useAliasTag = preference.AliasTag2;
      GraphColor::LiveRange * useLiveRange = tile->GetLiveRange(useAliasTag);
      if (useLiveRange == nullptr) {
         continue;
      }
     
      unsigned     useLiveRangeId = useLiveRange->Id;
      bool         useIsRegister = useLiveRange->IsPhysicalRegister;

      if (definitionLiveRangeId != useLiveRangeId) {
         if (!conflictGraph->HasConflict(definitionLiveRangeId, useLiveRangeId)) {
            if (!definitionIsRegister || !useIsRegister) {
               bitGraph->AddEdge(definitionLiveRangeId, useLiveRangeId);
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edges for connected spill live ranges left by the
//    spill optimizer.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddBoundaryPreferenceEdges
(
   llvm::MachineInstr * instruction
)
{
   if (!Tile::IsEnterTileInstruction(instruction)) {
      return;
   }

   GraphColor::Tile *              tile = this->Tile;
   GraphColor::Allocator *         allocator = this->Allocator;
   GraphColor::ConflictGraph *     conflictGraph = tile->ConflictGraph;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   Tiled::VR::Info *               vrInfo = tile->VrInfo;

   // For each enter tile source that can be mapped back to a global add an edge between the summary variable
   // and the source live range.

   llvm::MachineInstr::mop_iterator sourceOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->explicit_operands());

   // foreach_register_source_opnd
   for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
   {
      assert(sourceOperand->isReg());
      unsigned reg = sourceOperand->getReg();

      if (!(reg == VR::Constants::InitialPseudoReg || reg == VR::Constants::InvalidTag)) {
         unsigned mappedTag = SpillOptimizer::GetSpillAliasTag(sourceOperand);
         assert(mappedTag != VR::Constants::InvalidTag);

         GraphColor::Tile *       childTile = allocator->GetTile(instruction);
         GraphColor::LiveRange *  spillLiveRange = tile->GetLiveRange(vrInfo->GetTag(sourceOperand->getReg()));
         assert(spillLiveRange != nullptr);
         GraphColor::LiveRange *  childLiveRange = childTile->GetSummaryLiveRange(mappedTag);


         if ((childLiveRange != nullptr) && !childLiveRange->IsSecondChanceGlobal) {
            assert(childLiveRange->IsSummary());
            unsigned summaryLiveRangeAliasTag = childLiveRange->VrTag;

            GraphColor::LiveRange * liveRange = tile->GetLiveRange(summaryLiveRangeAliasTag);

            if (liveRange == nullptr) {
               unsigned registerAliasTag = vrInfo->GetTag(childLiveRange->Register);
               liveRange = tile->GetLiveRange(registerAliasTag);
            }

            assert(liveRange != nullptr);

            unsigned liveRangeId = liveRange->Id;
            unsigned spillLiveRangeId = spillLiveRange->Id;

            if (!conflictGraph->HasConflict(liveRangeId, spillLiveRangeId)) {
               bitGraph->AddEdge(liveRangeId, spillLiveRangeId);
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edges between global live ranges live at nested tile
//    boundary and summary live ranges they are associated with the nested tile.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddSummaryPreferenceEdges
(
   llvm::MachineInstr * instruction
)
{
   GraphColor::Tile *              tile = this->Tile;
   assert(tile->IsTileBoundaryInstruction(instruction));

   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::Allocator *         allocator = this->Allocator;
   GraphColor::Tile *              nestedTile = allocator->GetTile(instruction);
   GraphColor::Liveness *          liveness = allocator->Liveness;
   llvm::MachineBasicBlock *       block = instruction->getParent();
   Dataflow::LivenessData *        registerLivenessData = liveness->GetRegisterLivenessData(block);
   llvm::SparseBitVector<> *       liveBitVector;
   unsigned                        globalAliasTag;
   GraphColor::ConflictGraph *     conflictGraph = tile->ConflictGraph;
   Tiled::VR::Info *               aliasInfo = allocator->VrInfo;
   unsigned                        summaryAliasTag;
   llvm::SparseBitVector<> *       summaryAliasTagSet;
   unsigned                        registerAliasTag;
   unsigned                        reg;
   GraphColor::LiveRange *         globalLiveRange;
   GraphColor::LiveRange *         summaryLiveRange;
   GraphColor::LiveRange *         preferenceLiveRange;
   unsigned                        globalLiveRangeId;
   unsigned                        preferenceLiveRangeId;

   if (Tile::IsEnterTileInstruction(instruction)) {
      liveBitVector = registerLivenessData->LiveInBitVector;
   } else {
      assert(Tile::IsExitTileInstruction(instruction));
      liveBitVector = registerLivenessData->LiveOutBitVector;
   }

   llvm::SparseBitVector<>::iterator glr;

   // foreach_sparse_bv_bit
   for (glr = liveBitVector->begin(); glr != liveBitVector->end(); ++glr)
   {
      unsigned globalAliasTag = *glr;

      // Get global live range if it exists in current tile.
      globalLiveRange = tile->GetLiveRange(globalAliasTag);
      if (globalLiveRange == nullptr) {
         continue;
      }

      if (globalLiveRange->IsPhysicalRegister) {
         continue;
      }

      // Get summary live range associated with global live range if it exits.
      summaryLiveRange = nestedTile->GetSummaryLiveRange(globalAliasTag);
      if (summaryLiveRange == nullptr || summaryLiveRange->IsSecondChanceGlobal) {
         continue;
      }

      // Get live range in current tile representing summary live range.  If it has been
      // hard preferenced, use the live range representing the register.

      summaryAliasTag = summaryLiveRange->VrTag;
      preferenceLiveRange = tile->GetLiveRange(summaryAliasTag);

      if (preferenceLiveRange == nullptr) {
         assert(summaryLiveRange->HasHardPreference);

         reg = summaryLiveRange->Register;
         registerAliasTag = aliasInfo->GetTag(reg);
         preferenceLiveRange = tile->GetLiveRange(registerAliasTag);
         assert(preferenceLiveRange != nullptr);
      }

      // If no conflict exits in current tile, then create preference edge.

      globalLiveRangeId = globalLiveRange->Id;
      preferenceLiveRangeId = preferenceLiveRange->Id;

      if (!conflictGraph->HasConflict(globalLiveRangeId, preferenceLiveRangeId)) {
         bitGraph->AddEdge(globalLiveRangeId, preferenceLiveRangeId);
      }

      // Create preference edges between this global live range and all other global live ranges 
      // summarized by this summary live range.

      summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;

      llvm::SparseBitVector<>::iterator s;

      // foreach_sparse_bv_bit
      for (s = summaryAliasTagSet->begin(); s != summaryAliasTagSet->end(); ++s)
      {
         unsigned summaryAliasTag = *s;

         preferenceLiveRange = tile->GetLiveRange(summaryAliasTag);

         if (preferenceLiveRange != nullptr) {
            globalLiveRangeId = globalLiveRange->Id;
            preferenceLiveRangeId = preferenceLiveRange->Id;

            if (globalLiveRangeId != preferenceLiveRangeId) {
               if (!conflictGraph->HasConflict(globalLiveRangeId, preferenceLiveRangeId)) {
                  bitGraph->AddEdge(globalLiveRangeId, preferenceLiveRangeId);
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edges for live ranges connected by a summary live range.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddSummaryPreferenceEdges
(
   GraphColor::LiveRange * summaryLiveRange
)
{
   // Skip physical registers.
   if (summaryLiveRange->IsPhysicalRegister) {
      return;
   }

   GraphColor::Tile *               tile = this->Tile;
   BitGraphs::UndirectedBitGraph *  bitGraph = this->BitGraph;
   GraphColor::ConflictGraph *      conflictGraph = tile->ConflictGraph;
   unsigned                         summaryLiveRangeAliasTag = summaryLiveRange->VrTag;
   GraphColor::LiveRange *          liveRange = tile->GetLiveRange(summaryLiveRangeAliasTag);

   // If there is no live range covering summary variable, it must be because it
   // is hard preferenced already or it's a second chance global or it's been marked as a spill.

   if (liveRange == nullptr) {
      assert(summaryLiveRange->HasHardPreference || summaryLiveRange->IsSecondChanceGlobal || summaryLiveRange->IsSpilled());
      return;
   }

   // Preference physical register preferenced by summary live range.

   unsigned                                 liveRangeId = liveRange->Id;
   GraphColor::Tile *                       nestedTile = summaryLiveRange->Tile;
   GraphColor::SummaryPreferenceGraph *     summaryPreferenceGraph = nestedTile->SummaryPreferenceGraph;
   GraphColor::LiveRange *                  summaryPreferenceLiveRange;
   Tiled::Cost                              summaryCost;
   RegisterAllocator::PreferenceConstraint  summaryPreferenceConstraint;

   GraphColor::GraphIterator piter;

   // foreach_preference_summaryliverange
   for (summaryPreferenceLiveRange =
           summaryPreferenceGraph->GetFirstPreferenceSummaryLiveRange(&piter,
              summaryLiveRange, &summaryCost, &summaryPreferenceConstraint);
        (summaryPreferenceLiveRange != nullptr);
        summaryPreferenceLiveRange =
           summaryPreferenceGraph->GetNextPreferenceSummaryLiveRange(&piter,
              &summaryCost, &summaryPreferenceConstraint)
        )
   {
      unsigned                preferenceAliasTag = summaryPreferenceLiveRange->VrTag;
      GraphColor::LiveRange * preferenceLiveRange = tile->GetLiveRange(preferenceAliasTag);

      if (preferenceLiveRange != nullptr) {
         if (preferenceLiveRange->IsPhysicalRegister) {
            unsigned  preferenceLiveRangeId = preferenceLiveRange->Id;

            if (liveRangeId != preferenceLiveRangeId) {
               if (!conflictGraph->HasConflict(liveRangeId, preferenceLiveRangeId)) {
                  bitGraph->AddEdge(liveRangeId, preferenceLiveRangeId);
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add Global preference edges for tile.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddGlobalPreferenceEdges()
{
   GraphColor::Tile * tile = this->Tile;
   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned lr = 1; lr < lrVector->size(); ++lr)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[lr];

      if (liveRange->IsGlobal()) {
         this->AddGlobalPreferenceEdges(liveRange);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edges for connected spill live ranges left by the
//    spill optimizer.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForBoundaryPreferenceEdges
(
   llvm::MachineInstr * instruction
)
{
   if (!Tile::IsEnterTileInstruction(instruction)) {
      return;
   }

   GraphColor::Tile *      tile = this->Tile;
   GraphColor::Allocator * allocator = this->Allocator;
   GraphColor::Tile *      childTile = allocator->GetTile(instruction);
   Tiled::VR::Info *       aliasInfo = allocator->VrInfo;
   Tiled::Cost             moveCost = allocator->IntegerMoveCost;

   // For each enter tile source that can be mapped back to a global add an edge between the summary variable
   // and the source live range.

   llvm::MachineInstr::mop_iterator sourceOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->explicit_operands());

   // foreach_register_source_opnd
   for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
   {
      assert(sourceOperand->isReg());
      unsigned reg = sourceOperand->getReg();

      if (!(reg == VR::Constants::InitialPseudoReg || VR::Info::IsVirtualRegister(reg))) {
         unsigned mappedTag = SpillOptimizer::GetSpillAliasTag(sourceOperand);
         assert(mappedTag != VR::Constants::InvalidTag);

         GraphColor::LiveRange * spillLiveRange = tile->GetLiveRange(aliasInfo->GetTag(sourceOperand->getReg()));
         GraphColor::LiveRange * childLiveRange = childTile->GetSummaryLiveRange(mappedTag);
         assert(spillLiveRange != nullptr);

         if (childLiveRange != nullptr && !childLiveRange->IsSecondChanceGlobal) {
            assert(childLiveRange->IsSummary());

            unsigned summaryLiveRangeAliasTag = childLiveRange->VrTag;
            GraphColor::LiveRange * liveRange = tile->GetLiveRange(summaryLiveRangeAliasTag);
            if (liveRange == nullptr) {
               unsigned registerAliasTag = aliasInfo->GetTag(childLiveRange->Register);

               liveRange = tile->GetLiveRange(registerAliasTag);
            }
            assert(liveRange != nullptr);

            unsigned liveRangeId = liveRange->Id;
            unsigned spillLiveRangeId = spillLiveRange->Id;

            if (tile->ConflictGraph->HasConflict(liveRangeId, spillLiveRangeId)) {
               continue;
            }

            assert(this->BitGraph->TestEdge(liveRangeId, spillLiveRangeId));

            // Calculate and add edge cost.

            Tiled::Cost cost = moveCost;

            allocator->ScaleCyclesByFrequency(&cost, instruction);
            this->AddCostForEdges(liveRange, spillLiveRange, &cost);
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add preference edges for a global live range.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddGlobalPreferenceEdges
(
   GraphColor::LiveRange * liveRange
)
{
   assert(liveRange->IsGlobal());

   GraphColor::Tile *              tile = this->Tile;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::ConflictGraph *     conflictGraph = tile->ConflictGraph;
   Tiled::Id                         liveRangeId = liveRange->Id;
   GraphColor::LiveRange *         globalLiveRange = liveRange->GlobalLiveRange;
   GraphColor::LiveRange *         registerLiveRange;
   unsigned                        registerAliasTag;
   unsigned                        registerLiveRangeId;
   llvm::SparseBitVector<> *       preferenceRegisterAliasTagSet;

   assert(globalLiveRange != nullptr);

   // Get preferred register set for global live range.
   preferenceRegisterAliasTagSet = this->GetGlobalRegisterPreferences(globalLiveRange);

   // Add edges for preferred register set for global live range.

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = preferenceRegisterAliasTagSet->begin(); r != preferenceRegisterAliasTagSet->end(); ++r)
   {
      registerAliasTag = *r;

      registerLiveRange = tile->GetLiveRange(registerAliasTag);
      if (registerLiveRange == nullptr) {
         continue;
      }

      registerLiveRangeId = registerLiveRange->Id;

      assert(liveRange->BaseRegisterCategory() == registerLiveRange->BaseRegisterCategory());

      if (!conflictGraph->HasConflict(liveRangeId, registerLiveRangeId)) {
         bitGraph->AddEdge(liveRangeId, registerLiveRangeId);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize adjacency vector after bit graph has been built.
//    Each entry starts as zero cost.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::InitializeAdjacencyVector()
{
   GraphColor::Tile *                       tile = this->Tile;
   GraphColor::Allocator *                  allocator = this->Allocator;
   BitGraphs::UndirectedBitGraph *          bitGraph = this->BitGraph;
   GraphColor::LiveRangePreferenceVector *  adjacencyVector;
   unsigned                                 edgeCount = 0;
   unsigned                                 liveRangeCount = tile->LiveRangeCount();

   for (unsigned liveRangeId1 = 1; liveRangeId1 <= liveRangeCount; liveRangeId1++)
   {
      GraphColor::LiveRange * liveRange1 = tile->GetLiveRangeById(liveRangeId1);

      for (unsigned liveRangeId2 = (liveRangeId1 + 1); liveRangeId2 <= liveRangeCount; liveRangeId2++)
      {
         GraphColor::LiveRange * liveRange2 = tile->GetLiveRangeById(liveRangeId2);

         if (bitGraph->TestEdge(liveRangeId1, liveRangeId2)) {
            if (!liveRange1->IsPhysicalRegister) {
               liveRange1->PreferenceEdgeCount++;
               edgeCount++;
            }

            //TODO ?: assert(!liveRange2->IsPhysicalRegister);
            if (!liveRange2->IsPhysicalRegister) {
               liveRange2->PreferenceEdgeCount++;
               edgeCount++;
            }
         }
      }
   }

   this->NodeCount = liveRangeCount;
   this->EdgeCount = edgeCount;

   unsigned size = edgeCount;

   adjacencyVector = new GraphColor::LiveRangePreferenceVector(size);
   this->AdjacencyVector = adjacencyVector;

   unsigned index = 0;

   Tiled::Cost zeroCost = allocator->ZeroCost;

   for (unsigned liveRangeId1 = 1; liveRangeId1 <= liveRangeCount; liveRangeId1++)
   {
      GraphColor::LiveRange * liveRange1 = tile->GetLiveRangeById(liveRangeId1);

      liveRange1->PreferenceAdjacencyIndex = index;

      if (!liveRange1->IsPhysicalRegister) {
         for (unsigned liveRangeId2 = 1; liveRangeId2 <= liveRangeCount; liveRangeId2++)
         {
            if (bitGraph->TestEdge(liveRangeId1, liveRangeId2)) {
               assert(liveRangeId1 != liveRangeId2);

               ((*adjacencyVector)[index]).LiveRangeId = liveRangeId2;
               ((*adjacencyVector)[index]).Cost = zeroCost;
               ((*adjacencyVector)[index]).PreferenceConstraint =
                  RegisterAllocator::PreferenceConstraint::SameRegister;
               index++;
               assert(size > 0);
               --size;
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build adjacency vector after bit graph has been built.
//
// Remarks:
//
//    Set the PreferenceEdgeCount and PreferenceAdjacencyIndex on live range.
//    Also, set the initial "Degree" on live range.  
//
//    AdjacencyVector layout is as follows:
//
//                                             AdjacenceyVector
//                                             +---------------------------------------------+
//    liveRange1->PreferenceAdjacenceyIndex -> |preference(cost, liveRangeId1, constraint)   |
//                                             |preference(cost, liveRangeId2, constraint)   |
//                                             |...                                          |
//    liveRange1->PreferenceEdgeCount == n     |preference(cost, liveRangeId(n), constraint) |
//                                             +---------------------------------------------+
//    liveRange2->PreferenceAdjacenceyIndex -> |preference(cost, liveRangeId1, constraint)   |
//                                             |preference(cost, liveRangeId2, constraint)   |
//                                             |...                                          |
//    liveRange2->PreferenceEdgeCount == m     |preference(cost, liveRangeId(m), constraint) |
//                                             +---------------------------------------------+
//                                             ....
//
//------------------------------------------------------------------------------

void
PreferenceGraph::BuildAdjacencyVector()
{
   // Initialize empty (zero cost) adjacency vector.

   this->InitializeAdjacencyVector();

   // Process each instruction in tile looking for preferences and calculating/adding the 
   // cost for them.

   GraphColor::Tile * tile = this->Tile;

   Graphs::MachineBasicBlockVector::iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile
   for (biter = mbbVector->begin(); biter != mbbVector->end(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;
      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);
         if (PreferenceGraph::HasPreference(instruction)) {
            this->AddCostForInstructionPreferenceEdge(instruction);
         } else if (tile->IsTileBoundaryInstruction(instruction)) {
            this->AddCostForBoundaryPreferenceEdges(instruction);
            this->AddCostForSummaryPreferenceEdges(instruction);
         }
      }
   }

   // Process each nested tile looking for preferences edges to summary live ranges

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      GraphColor::LiveRangeVector * slrVector = nestedTile->GetSummaryLiveRangeEnumerator();

      // foreach_summaryliverange_in_tile
      for (unsigned slr = 1; slr < slrVector->size(); ++slr)
      {
         GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];
         this->AddCostForSummaryPreferenceEdges(summaryLiveRange);
      }
   }

   // Process each global live range and cost for this tile.

   Tiled::Boolean doPositiveCost = true;

   this->AddCostForGlobalPreferenceEdges(doPositiveCost);

   // Make sure we have costed every edge appropriately.
#if defined(TILED_DEBUG_CHECKS)
   this->CheckAdjacencyVector();
#endif
}


#if defined(TILED_DEBUG_CHECKS)
//---------------------------------------------------------------------------------------------
//
// Description:
//
//    Check adjacency vector and make sure we have costed every entry.
//
//---------------------------------------------------------------------------------------------

void
PreferenceGraph::CheckAdjacencyVector()
{
   GraphColor::Tile ^               tile = this->Tile;
   Tiled::Id                          liveRangeId;
   GraphColor::LiveRange ^          liveRange;
   Tiled::UInt32                      index;
   Tiled::UInt32                      limit;
   Tiled::UInt32                      size;
   Tiled::Cost                        edgeCost;

   liveRangeId = 1;
   size = adjacencyVector->Count();
   index = 0;

   while (index < size)
   {
      liveRange = tile->GetLiveRangeById(liveRangeId);
      Assert(index == liveRange->PreferenceAdjacencyIndex);

      limit = liveRange->PreferenceEdgeCount + index;
      while (index < limit)
      {
         edgeCost = adjacencyVector->Item[index].Cost;
         index++;
      }

      liveRangeId++;
   }

   Assert(index == adjacencyVector->Count());
}

#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Add cost for global live ranges.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForGlobalPreferenceEdges
(
   bool doPositiveCost
)
{
   GraphColor::Tile * tile = this->Tile;

   // Process global live ranges and add costs coming from their global preferences and conflicts.

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned l = 1; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];

      if (liveRange->IsGlobal() && !liveRange->IsSpilled()) {
         this->AddCostForGlobalPreferenceEdges(liveRange, doPositiveCost);
      }
   }

   // Process nested tile boundary instructions and global live ranges that span them.
   // Add cost coming from register conflicts in nested tiles forcing moves at tile boundaries.

   GraphColor::Allocator *   allocator = this->Allocator;
   GraphColor::Liveness *    liveness = allocator->Liveness;
   Dataflow::LivenessData *  registerLivenessData;
   llvm::SparseBitVector<> * liveBitVector;

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

         llvm::MachineInstr * instruction = nestedTile->FindNextInstruction(block, block->instr_begin(), llvm::TargetOpcode::ENTERTILE);
         assert(instruction != nullptr);

         registerLivenessData = liveness->GetRegisterLivenessData(block);
         liveBitVector = registerLivenessData->LiveInBitVector;

         this->AddCostForGlobalPreferenceEdges(instruction, liveBitVector, doPositiveCost);
      }

      // foreach_tile_exit_block
      for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * block = *bi;

         llvm::MachineInstr * instruction = nestedTile->FindNextInstruction(block, block->instr_begin(), llvm::TargetOpcode::EXITTILE);
         assert(instruction != nullptr);

         registerLivenessData = liveness->GetRegisterLivenessData(block);
         liveBitVector = registerLivenessData->LiveOutBitVector;

         this->AddCostForGlobalPreferenceEdges(instruction, liveBitVector, doPositiveCost);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the preference register alias tag set for the global live range.
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
PreferenceGraph::GetGlobalRegisterPreferences
(
   GraphColor::LiveRange * globalLiveRange
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   llvm::SparseBitVector<> *  allocatableRegisterAliasTagSet;
   llvm::SparseBitVector<> *  globalPreferenceRegisterAliasTagSet;
   llvm::SparseBitVector<> *  globalHardPreferenceRegisterAliasTagSet;
   llvm::SparseBitVector<> *  globalConflictRegisterAliasTagSet;
   llvm::SparseBitVector<> *  preferenceRegisterAliasTagSet = allocator->ScratchBitVector2;

   preferenceRegisterAliasTagSet->clear();

   if (globalLiveRange->IsPhysicalRegister) {
      return preferenceRegisterAliasTagSet;
   }

   globalConflictRegisterAliasTagSet = globalLiveRange->GlobalConflictRegisterAliasTagSet;
   globalPreferenceRegisterAliasTagSet = globalLiveRange->GlobalPreferenceRegisterAliasTagSet;
   globalHardPreferenceRegisterAliasTagSet = globalLiveRange->GlobalHardPreferenceRegisterAliasTagSet;

   // If there is nothing to help guide the global live range allocation just return.
   if ((globalPreferenceRegisterAliasTagSet->empty())
      && (globalConflictRegisterAliasTagSet->empty())
      && (globalHardPreferenceRegisterAliasTagSet->empty())) {
      return preferenceRegisterAliasTagSet;
   }

   // Calculate preference register set.
   if (!globalConflictRegisterAliasTagSet->empty()) {
      allocatableRegisterAliasTagSet = allocator->GetCategoryRegisters(globalLiveRange);
      *preferenceRegisterAliasTagSet = *allocatableRegisterAliasTagSet;
      preferenceRegisterAliasTagSet->intersectWithComplement(*globalConflictRegisterAliasTagSet);
   } else {
      *preferenceRegisterAliasTagSet |= *globalPreferenceRegisterAliasTagSet;

      // Preference info is the full pre-allocation set which is updated as allocation progresses.  So to get
      // the correct set we take initial minus the ongoing updated global conflict set.

      preferenceRegisterAliasTagSet->intersectWithComplement(*globalConflictRegisterAliasTagSet);
      *preferenceRegisterAliasTagSet |= *globalHardPreferenceRegisterAliasTagSet;
   }

   return preferenceRegisterAliasTagSet;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add cost for global live range.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForGlobalPreferenceEdges
(
   GraphColor::LiveRange * liveRange,
   bool                    doPositiveCost
)
{
   assert(liveRange->IsGlobal());

   GraphColor::Tile *                              tile = this->Tile;
   GraphColor::Allocator *                         allocator = this->Allocator;
   Tiled::VR::Info *                               vrInfo = allocator->VrInfo;
   BitGraphs::UndirectedBitGraph *                 bitGraph = this->BitGraph;
   unsigned                                        liveRangeId = liveRange->Id;
   GraphColor::LiveRange *                         globalLiveRange = liveRange->GlobalLiveRange;
   GraphColor::LiveRange *                         registerLiveRange;
   unsigned                                        registerAliasTag;
   unsigned                                        registerLiveRangeId;
   llvm::SparseBitVector<> *                       globalPreferredRegisterAliasTagSet;
   Tiled::Cost                                     globalPreferredCost;
   unsigned                                        globalPreferredReg;
   GraphColor::PhysicalRegisterPreferenceVector *  globalRegisterPreferenceVector;

   assert(globalLiveRange != nullptr);

   // Get preferred register set for global live range.
   globalPreferredRegisterAliasTagSet = this->GetGlobalRegisterPreferences(globalLiveRange);

   // Calculate unit cost.
   globalPreferredCost = allocator->UnitCost;
   if (!doPositiveCost) {
      allocator->ScaleBy(&globalPreferredCost, -1);
   }

   // Cost preferred register set for global live range.

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = globalPreferredRegisterAliasTagSet->begin(); r != globalPreferredRegisterAliasTagSet->end(); ++r)
   {
      registerAliasTag = *r;
      registerLiveRange = tile->GetLiveRange(registerAliasTag);
      if (registerLiveRange == nullptr) {
         continue;
      }

      registerLiveRangeId = registerLiveRange->Id;

      assert(liveRange->BaseRegisterCategory() == registerLiveRange->BaseRegisterCategory());

      if (bitGraph->TestEdge(liveRangeId, registerLiveRangeId)) {
         this->AddCostForEdges(liveRange, registerLiveRange, &globalPreferredCost);
      }
   }

   // Add costs for actual physical register preferenced by global live range.

   llvm::SparseBitVector<> * globalConflictRegisterAliasTagSet = globalLiveRange->GlobalConflictRegisterAliasTagSet;

   globalRegisterPreferenceVector = globalLiveRange->GlobalRegisterPreferenceVector;

   unsigned preferenceEdgeCount = globalRegisterPreferenceVector->size();

   for (unsigned preferenceIndex = 0; preferenceIndex < preferenceEdgeCount; preferenceIndex++)
   {
      globalPreferredReg = ((*globalRegisterPreferenceVector)[preferenceIndex]).Register;

      registerAliasTag = vrInfo->GetTag(globalPreferredReg);
      registerLiveRange = tile->GetLiveRange(registerAliasTag);
      registerLiveRangeId = registerLiveRange->Id;

      if (bitGraph->TestEdge(liveRangeId, registerLiveRangeId)
         && !globalConflictRegisterAliasTagSet->test(registerAliasTag)) {

         // Get preference cost.
         globalPreferredCost = ((*globalRegisterPreferenceVector)[preferenceIndex]).Cost;
         if (!doPositiveCost) {
            allocator->ScaleBy(&globalPreferredCost, -1);
         }

         // Add edge cost 
         this->AddCostForEdges(liveRange, registerLiveRange, &globalPreferredCost);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Undo the above costing.  Used before summarization.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::SubtractCostForGlobalPreferenceEdges()
{
   this->AddCostForGlobalPreferenceEdges(false);
}

//------------------------------------------------------------------------------
//
// Description
//
//    Add edge cost to adjacency lists in graph. Optionally scale cost by
//    instruction frequency. Exclude costing edges on physical register
//    adjacency lists since those lists are intentionally left empty.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForEdges
(
   GraphColor::LiveRange * liveRange1,
   GraphColor::LiveRange * liveRange2,
   Tiled::Cost *             cost
)
{
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 liveRange1Id = liveRange1->Id;
   unsigned                                 liveRange2Id = liveRange2->Id;
   unsigned                                 index;
   unsigned                                 limit;
   Tiled::Cost                                edgeCost;

   // Add edge cost for lr1 => lr2 edge

   if (!liveRange1->IsPhysicalRegister) {
      index = liveRange1->PreferenceAdjacencyIndex;
      limit = liveRange1->PreferenceEdgeCount + index;

      for (; index < limit; index++)
      {
         if (((*adjacencyVector)[index]).LiveRangeId == liveRange2Id) {
            break;
         }
      }

      assert(index < limit);

      edgeCost = ((*adjacencyVector)[index]).Cost;
      edgeCost.IncrementBy(cost);
      ((*adjacencyVector)[index]).Cost = edgeCost;
   }

   // Add edge cost for lr2 => lr1 edge

   if (!liveRange2->IsPhysicalRegister) {
      index = liveRange2->PreferenceAdjacencyIndex;
      limit = liveRange2->PreferenceEdgeCount + index;

      for (; index < limit; index++)
      {
         if (((*adjacencyVector)[index]).LiveRangeId == liveRange1Id) {
            break;
         }
      }

      assert(index < limit);

      edgeCost = ((*adjacencyVector)[index]).Cost;
      edgeCost.IncrementBy(cost);
      ((*adjacencyVector)[index]).Cost = edgeCost;
   }
}

#pragma warning(disable:4238)

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate the cost of the instruction and add it to the preference graph
//    for the appropriate edge
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForInstructionPreferenceEdge
(
   llvm::MachineInstr * instruction
)
{
   assert(PreferenceGraph::HasPreference(instruction));

   GraphColor::Tile *                          tile = this->Tile;
   GraphColor::Allocator *                     allocator = this->Allocator;
   BitGraphs::UndirectedBitGraph *             bitGraph = this->BitGraph;
   GraphColor::PreferenceVector *              preferenceVector = this->PreferenceVector;

   TargetRegisterAllocInfo * targetRegisterAllocator = allocator->TargetRegisterAllocator;

   // Calculate preference operand and costs.
   targetRegisterAllocator->PreferenceInstruction(instruction, preferenceVector);

   // Add costs to edges in the preference graph adjacency vector.

   for (unsigned i = 0, n = preferenceVector->size(); i < n; i++)
   {
      RegisterAllocator::Preference  preference = (*preferenceVector)[i];
      unsigned                       definitionAliasTag = preference.AliasTag1;
      GraphColor::LiveRange *        definitionLiveRange = tile->GetLiveRange(definitionAliasTag);
      if (definitionLiveRange == nullptr) {
         continue;
      }
      unsigned                definitionLiveRangeId = definitionLiveRange->Id;

      unsigned                useAliasTag = preference.AliasTag2;
      GraphColor::LiveRange * useLiveRange = tile->GetLiveRange(useAliasTag);
      if (useLiveRange == nullptr) {
         continue;
      }
      unsigned                useLiveRangeId = useLiveRange->Id;

      if (bitGraph->TestEdge(definitionLiveRangeId, useLiveRangeId)) {
         Tiled::Cost cost = preference.Cost;

         allocator->ScaleCyclesByFrequency(&cost, instruction);
         this->AddCostForEdges(definitionLiveRange, useLiveRange, &cost);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate preference cost for edges between global live ranges live at nested tile boundary 
//    and the summary live ranges they are associated with in the nested tile.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForSummaryPreferenceEdges
(
   llvm::MachineInstr * instruction
)
{
   GraphColor::Tile *              tile = this->Tile;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::Allocator *         allocator = this->Allocator;
   GraphColor::Tile *              nestedTile = allocator->GetTile(instruction);
   GraphColor::Liveness *          liveness = allocator->Liveness;
   llvm::MachineBasicBlock *       block = instruction->getParent();
   Dataflow::LivenessData *        registerLivenessData = liveness->GetRegisterLivenessData(block);
   llvm::SparseBitVector<> *       liveBitVector;
   unsigned                        globalAliasTag;
   VR::Info *                      aliasInfo = allocator->VrInfo;
   unsigned                        summaryAliasTag;
   llvm::SparseBitVector<> *       summaryAliasTagSet;
   unsigned                        registerAliasTag;
   unsigned                        reg;
   GraphColor::LiveRange *         globalLiveRange;
   GraphColor::LiveRange *         summaryLiveRange;
   Tiled::Cost                     compensationCost;

   assert(GraphColor::Tile::IsTileBoundaryInstruction(instruction));

   compensationCost = allocator->IntegerMoveCost;
   allocator->ScaleCyclesByFrequency(&compensationCost, instruction);

   if (Tile::IsEnterTileInstruction(instruction)) {
      liveBitVector = registerLivenessData->LiveInBitVector;
   } else {
      assert(Tile::IsExitTileInstruction(instruction));
      liveBitVector = registerLivenessData->LiveOutBitVector;
   }

   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = liveBitVector->begin(); g != liveBitVector->end(); ++g)
   {
      unsigned globalAliasTag = *g;

      // Get global live range if it exists in current tile.
      globalLiveRange = tile->GetLiveRange(globalAliasTag);
      if (globalLiveRange == nullptr) {
         continue;
      }

      // Get summary live range associate with global live range if it exits.
      summaryLiveRange = nestedTile->GetSummaryLiveRange(globalAliasTag);
      if (summaryLiveRange == nullptr || summaryLiveRange->IsSecondChanceGlobal) {
         continue;
      }

      // Get live range in current tile representing summary live range.  If it has been
      // hard preferenced, use the live range representing the register.

      summaryAliasTag = summaryLiveRange->VrTag;

      GraphColor::LiveRange * preferenceLiveRange = tile->GetLiveRange(summaryAliasTag);

      if (preferenceLiveRange == nullptr) {
         assert(summaryLiveRange->HasHardPreference);

         reg = summaryLiveRange->Register;
         registerAliasTag = aliasInfo->GetTag(reg);
         preferenceLiveRange = tile->GetLiveRange(registerAliasTag);
         assert(preferenceLiveRange != nullptr);
      }

      // If no conflict exists in current tile, then cost the preference edge.

      unsigned globalLiveRangeId = globalLiveRange->Id;
      unsigned preferenceLiveRangeId = preferenceLiveRange->Id;

      if (bitGraph->TestEdge(globalLiveRangeId, preferenceLiveRangeId)) {
         this->AddCostForEdges(globalLiveRange, preferenceLiveRange, &compensationCost);
      }

      // Cost preference edges between this global live range and all other global live ranges 
      // summarized by this summary live range.

      summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;

      llvm::SparseBitVector<>::iterator s;

      // foreach_sparse_bv_bit
      for (s = summaryAliasTagSet->begin(); s != summaryAliasTagSet->end(); ++s)
      {
         unsigned summaryAliasTag = *s;

         preferenceLiveRange = tile->GetLiveRange(summaryAliasTag);

         if (preferenceLiveRange != nullptr) {
            globalLiveRangeId = globalLiveRange->Id;
            preferenceLiveRangeId = preferenceLiveRange->Id;

            if (bitGraph->TestEdge(globalLiveRangeId, preferenceLiveRangeId)) {
               this->AddCostForEdges(globalLiveRange, preferenceLiveRange, &compensationCost);
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate preference cost for edges between summary live ranges in nested tile 
//    the physical live ranges they are preferenced to.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForSummaryPreferenceEdges
(
   GraphColor::LiveRange * summaryLiveRange
)
{
   // Skip physical registers.
   if (summaryLiveRange->IsPhysicalRegister) {
      return;
   }

   GraphColor::Tile *              tile = this->Tile;
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   unsigned                        summaryLiveRangeAliasTag = summaryLiveRange->VrTag;
   GraphColor::LiveRange *         liveRange = tile->GetLiveRange(summaryLiveRangeAliasTag);

   // If there is no live range covering summary variable, it must be because it
   // is hard preferenced already.

   if (liveRange == nullptr) {
      assert(summaryLiveRange->HasHardPreference
             || summaryLiveRange->IsSecondChanceGlobal
             || summaryLiveRange->IsSpilled());
      return;
   }

   // Preference physical registers preferenced by summary live range.

   unsigned                                 liveRangeId = liveRange->Id;
   GraphColor::Tile *                       nestedTile = summaryLiveRange->Tile;
   GraphColor::SummaryPreferenceGraph *     summaryPreferenceGraph = nestedTile->SummaryPreferenceGraph;
   GraphColor::LiveRange *                  summaryPreferenceLiveRange;
   Tiled::Cost                              summaryCost;
   RegisterAllocator::PreferenceConstraint  summaryPreferenceConstraint;

   GraphColor::GraphIterator piter;

   // foreach_preference_summaryliverange
   for (summaryPreferenceLiveRange = summaryPreferenceGraph->GetFirstPreferenceSummaryLiveRange
                                                             (
                                                                &piter,
                                                                summaryLiveRange,
                                                                &summaryCost,
                                                                &summaryPreferenceConstraint
                                                             );
        (summaryPreferenceLiveRange != nullptr);
        summaryPreferenceLiveRange = summaryPreferenceGraph->GetNextPreferenceSummaryLiveRange
                                                             (
                                                                &piter,
                                                                &summaryCost,
                                                                &summaryPreferenceConstraint
                                                             )
       )
   {
      unsigned /*Tiled::BitNumber*/  preferenceAliasTag = summaryPreferenceLiveRange->VrTag;
      GraphColor::LiveRange *        preferenceLiveRange = tile->GetLiveRange(preferenceAliasTag);

      if (preferenceLiveRange != nullptr) {
         if (preferenceLiveRange->IsPhysicalRegister) {
            unsigned  preferenceLiveRangeId = preferenceLiveRange->Id;

            if (liveRangeId != preferenceLiveRangeId) {
               if (bitGraph->TestEdge(liveRangeId, preferenceLiveRangeId)) {
                  this->AddCostForEdges(liveRange, preferenceLiveRange, &summaryCost);
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate the cost of the global live ranges to physical registers.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::AddCostForGlobalPreferenceEdges
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * liveBitVector,
   bool                      doPositiveCost
)
{
   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::Tile *              tile = this->Tile;
   GraphColor::Allocator *         allocator = this->Allocator;
   GraphColor::Tile *              nestedTile = allocator->GetTile(instruction);
   GraphColor::LiveRange *         liveRange;
   unsigned                        liveRangeId;
   unsigned                        registerAliasTag;
   GraphColor::LiveRange *         registerLiveRange;
   unsigned                        registerLiveRangeId;
   unsigned                        globalAliasTag;
   GraphColor::LiveRange *         globalLiveRange;
   llvm::SparseBitVector<> *       preferenceRegisterAliasTagSet;
   llvm::SparseBitVector<> *       conflictRegiserAliasTagSet;
   llvm::SparseBitVector<> *       saveConflictAliasTagSet;
   llvm::SparseBitVector<> *       killedRegisterAliasTagSet;
   llvm::SparseBitVector<> *       globalSpillAliasTagSet = nestedTile->GlobalSpillAliasTagSet;
   Tiled::Cost                     globalPreferredCost;

   assert(tile == nestedTile->ParentTile);

   // Initialize register preference cost

   globalPreferredCost = allocator->IntegerMoveCost;
   allocator->ScaleCyclesByFrequency(&globalPreferredCost, instruction);
   if (!doPositiveCost) {
      allocator->ScaleBy(&globalPreferredCost, -1);
   }

   llvm::SparseBitVector<>::iterator g;

   // foreach_sparse_bv_bit
   for (g = liveBitVector->begin(); g != liveBitVector->end(); ++g)
   {
      globalAliasTag = *g;

      liveRange = tile->GetLiveRange(globalAliasTag);
      if (liveRange == nullptr) {
         continue;
      }

      globalLiveRange = liveRange->GlobalLiveRange;
      if (globalLiveRange == nullptr) {
         continue;
      }

      globalAliasTag = globalLiveRange->VrTag;
      if (globalSpillAliasTagSet->test(globalAliasTag)) {
         continue;
      }

      conflictRegiserAliasTagSet = globalLiveRange->GlobalConflictRegisterAliasTagSet;

      if (conflictRegiserAliasTagSet->empty()) {
         continue;
      }

      conflictRegiserAliasTagSet = allocator->ScratchBitVector3;
      *conflictRegiserAliasTagSet = *(globalLiveRange->GlobalConflictRegisterAliasTagSet);
      killedRegisterAliasTagSet = nestedTile->KilledRegisterAliasTagSet;
      *conflictRegiserAliasTagSet &= *killedRegisterAliasTagSet;

      if (conflictRegiserAliasTagSet->empty()) {
         continue;
      }

      liveRangeId = liveRange->Id;

      // Get preferred register set for global live range.  Note, we temporarily customize
      // the global live ranges conflict set to be the set that corresponds with this nested tile.

      saveConflictAliasTagSet = globalLiveRange->GlobalConflictRegisterAliasTagSet;
      globalLiveRange->GlobalConflictRegisterAliasTagSet = conflictRegiserAliasTagSet;
      preferenceRegisterAliasTagSet = this->GetGlobalRegisterPreferences(globalLiveRange);
      globalLiveRange->GlobalConflictRegisterAliasTagSet = saveConflictAliasTagSet;

      // Cost preferred register set for global live range.

      llvm::SparseBitVector<>::iterator r;

      // foreach_sparse_bv_bit
      for (r = preferenceRegisterAliasTagSet->begin(); r != preferenceRegisterAliasTagSet->end(); ++r)
      {
         registerAliasTag = *r;

         registerLiveRange = tile->GetLiveRange(registerAliasTag);
         if (registerLiveRange == nullptr) {
            continue;
         }

         registerLiveRangeId = registerLiveRange->Id;
         if (bitGraph->TestEdge(liveRangeId, registerLiveRangeId)) {
            this->AddCostForEdges(liveRange, registerLiveRange, &globalPreferredCost);
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Set the preference cost object associated with the passed
//   iterator
//
// Arguments:
//
//    iterator - Preference to set
//    cost     - New preference benefit
//
//------------------------------------------------------------------------------

void
PreferenceGraph::SetPreferenceCost
(
   GraphColor::GraphIterator * iterator,
   Tiled::Cost *               cost
)
{
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 index = iterator->Index;

   ((*adjacencyVector)[index]).Cost = *cost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Install a preference between liveRange and preferenceLiveRange in pass
//    two. This allows for pushing assigned register preferences down from
//    parent tile to child tiles.
//
// Arguments:
//
//    liveRange            - live range to be edited
//    preferenceLiveRange  - new preference live range
//    cost                 - new preference benefit
//    preferenceConstraint - new preference constraint
//
// Notes:
//
//    Currently we only reserve a single edge in pass one construction for pass
//    two push down so only a single preference can be added. Calling this
//    function more than once overwrites the single slot.
//
//------------------------------------------------------------------------------

void
SummaryPreferenceGraph::InstallParentPreference
(
   GraphColor::LiveRange *                   liveRange,
   GraphColor::LiveRange *                   preferenceLiveRange,
   Tiled::Cost *                             cost,
   RegisterAllocator::PreferenceConstraint * preferenceConstraint
)
{
   unsigned                                liveRangeId = liveRange->Id;
   unsigned                                preferenceLiveRangeId = preferenceLiveRange->Id;
   GraphColor::LiveRangePreferenceVector * adjacencyVector = this->AdjacencyVector;
   unsigned                                index;
   unsigned                                edgeCount;

   assert(liveRange != preferenceLiveRange);
   assert(!liveRange->IsPhysicalRegister);
   assert(preferenceLiveRange->IsPhysicalRegister);

   // Edit edges in bit graph.

   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   bitGraph->AddEdge(liveRangeId, preferenceLiveRangeId);

   // Edit edge info on live range.

   index = liveRange->PreferenceAdjacencyIndex;
   index--;
   liveRange->PreferenceAdjacencyIndex = index;

   edgeCount = liveRange->PreferenceEdgeCount;
   edgeCount++;
   liveRange->PreferenceEdgeCount = edgeCount;

   // Make sure this is the hidden edge we left behind.

   assert((*adjacencyVector)[index].LiveRangeId == liveRangeId);

   // Fill in the new preference edge info.

   (*adjacencyVector)[index].LiveRangeId = preferenceLiveRangeId;
   (*adjacencyVector)[index].Cost = *cost;
   (*adjacencyVector)[index].PreferenceConstraint = *preferenceConstraint;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Replace the edge between liveRange and oldPreferenceLiveRange with a new edge
//   between liveRange and newPreferenceLiveRange.  Assign the new cost and constraint.
//
// Arguments:
//
//    liveRange              - live range to be edited
//    oldPreferenceLiveRange - old preference live range
//    newPreferenceLiveRange - new preference live range
//    cost                   - new preference benefit
//    preferenceConstraint   - new preference constraint
//
// Remarks:
//
//    Currently this is pretty specialized and only used to change which
//    physical register a summary live range is preferenced to.   It also doesn't
//    clean up the edges "from" the physical registers summary live ranges.
//
//    If a preference to a register already exists, then just add the cost to 
//    the edge.
//
//------------------------------------------------------------------------------

#ifdef FUTURE_IMPL   //currently NOT used
void
SummaryPreferenceGraph::ReplaceEdge
(
   GraphColor::LiveRange *                  liveRange,
   GraphColor::LiveRange *                  oldPreferenceLiveRange,
   GraphColor::LiveRange *                  newPreferenceLiveRange,
   Tiled::Cost&                             cost,
   RegisterAllocator::PreferenceConstraint& preferenceConstraint
)
{
   Assert(liveRange != oldPreferenceLiveRange);
   Assert(liveRange != newPreferenceLiveRange);
   Assert(oldPreferenceLiveRange != newPreferenceLiveRange);

   Assert(!liveRange->IsPhysicalRegister);
   Assert(oldPreferenceLiveRange->IsPhysicalRegister);
   Assert(newPreferenceLiveRange->IsPhysicalRegister);

   // Edit edges in bit graph.

   BitGraphs::UndirectedBitGraph ^ bitGraph = this->BitGraph;

   Assert(bitGraph->TestEdge(liveRange->Id, oldPreferenceLiveRange->Id));

   Tiled::Boolean hasExistingPreference = bitGraph->TestEdge(liveRange->Id, newPreferenceLiveRange->Id);

   bitGraph->AddEdge(liveRange->Id, newPreferenceLiveRange->Id);

   // Edit edges in adjacency vector.

   Collections::LiveRangePreferenceVector ^ adjacencyVector = this->AdjacencyVector;
   Tiled::UInt32                              index;
   Tiled::Cost                                oldCost;
   GraphColor::PreferenceConstraint         oldPreferenceConstraint;

   if (!hasExistingPreference)
   {
      foreach_preference_summaryliverange_with_iterator(preferenceLiveRange, liveRange, iterator,
         oldCost, oldPreferenceConstraint, this)
      {
         if (oldPreferenceLiveRange == preferenceLiveRange)
         {
            index = iterator.Index;
            adjacencyVector->Item[index].LiveRangeId = newPreferenceLiveRange->Id;
            adjacencyVector->Item[index].Cost = *cost;
            adjacencyVector->Item[index].PreferenceConstraint = *preferenceConstraint;
            return;
         }
      }
      next_preference_summaryliverange_with_iterator;
   }
   else
   {
      // Use new preference live range as old one for search to find existing edge.

      oldPreferenceLiveRange = newPreferenceLiveRange;

      foreach_preference_summaryliverange_with_iterator(preferenceLiveRange, liveRange, iterator,
         oldCost, oldPreferenceConstraint, this)
      {
         if (oldPreferenceLiveRange == preferenceLiveRange)
         {
            index = iterator.Index;
            adjacencyVector->Item[index].LiveRangeId = newPreferenceLiveRange->Id;

            // Increment old cost by new cost.

            oldCost.IncrementBy(cost);
            adjacencyVector->Item[index].Cost = oldCost;
            adjacencyVector->Item[index].PreferenceConstraint = *preferenceConstraint;
            return;
         }
      }
      next_preference_summaryliverange_with_iterator;
   }

   Assert(false);
}
#endif

//------------------------------------------------------------------------------
//
// Description:
//
//   Get the first preference live range for a given live range
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
PreferenceGraph::GetFirstPreferenceLiveRange
(
   GraphColor::GraphIterator *               iterator,
   GraphColor::LiveRange *                   liveRange,
   Tiled::Cost *                             cost,
   RegisterAllocator::PreferenceConstraint * preferenceConstraint
)
{
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 index = liveRange->PreferenceAdjacencyIndex;

   iterator->Count = liveRange->PreferenceEdgeCount;
   iterator->Index = index;

   if (iterator->Count == 0) {
      return nullptr;
   }

   GraphColor::Tile *      tile = this->Tile;
   unsigned                preferenceLiveRangeId = ((*adjacencyVector)[index]).LiveRangeId;
   GraphColor::LiveRange * preferenceLiveRange = tile->GetLiveRangeById(preferenceLiveRangeId);

   *cost = ((*adjacencyVector)[index]).Cost;
   *preferenceConstraint = ((*adjacencyVector)[index]).PreferenceConstraint;

   return preferenceLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Get the next preference live range for a given live range
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
PreferenceGraph::GetNextPreferenceLiveRange
(
   GraphColor::GraphIterator *               iterator,
   Tiled::Cost *                             cost,
   RegisterAllocator::PreferenceConstraint * preferenceConstraint
)
{
   // Pre-increment for next preference
   iterator->Index++;
   iterator->Count--;

   if (iterator->Count == 0) {
      return nullptr;
   }

   unsigned                                 index = iterator->Index;
   GraphColor::Tile *                       tile = this->Tile;
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 preferenceLiveRangeId = ((*adjacencyVector)[index]).LiveRangeId;
   GraphColor::LiveRange *                  preferenceLiveRange = tile->GetLiveRangeById(preferenceLiveRangeId);

   *cost = ((*adjacencyVector)[index]).Cost;
   *preferenceConstraint = ((*adjacencyVector)[index]).PreferenceConstraint;

   return preferenceLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Construct a new summary preference graph object for this tile.
//
//------------------------------------------------------------------------------

GraphColor::SummaryPreferenceGraph *
SummaryPreferenceGraph::New
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *              allocator = tile->Allocator;
   GraphColor::SummaryPreferenceGraph * preferenceGraph = new GraphColor::SummaryPreferenceGraph();

   preferenceGraph->Allocator = allocator;
   preferenceGraph->Tile = tile;

   return preferenceGraph;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Get the first preference live range for a given live range
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
SummaryPreferenceGraph::GetFirstPreferenceSummaryLiveRange
(
   GraphColor::GraphIterator *               iterator,
   GraphColor::LiveRange *                   liveRange,
   Tiled::Cost *                             cost,
   RegisterAllocator::PreferenceConstraint * preferenceConstraint
)
{
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 index = liveRange->PreferenceAdjacencyIndex;

   iterator->Count = liveRange->PreferenceEdgeCount;
   iterator->Index = index;

   if (iterator->Count == 0) {
      return nullptr;
   }

   GraphColor::Tile *      tile = this->Tile;
   unsigned                preferenceLiveRangeId = ((*adjacencyVector)[index]).LiveRangeId;
   GraphColor::LiveRange * preferenceLiveRange = tile->GetSummaryLiveRangeById(preferenceLiveRangeId);

   *cost = ((*adjacencyVector)[index]).Cost;
   *preferenceConstraint = ((*adjacencyVector)[index]).PreferenceConstraint;

   return preferenceLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Get the next preference live range for a given live range
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
SummaryPreferenceGraph::GetNextPreferenceSummaryLiveRange
(
   GraphColor::GraphIterator *               iterator,
   Tiled::Cost *                             cost,
   RegisterAllocator::PreferenceConstraint * preferenceConstraint
)
{
   // Pre-increment for next preference

   iterator->Index++;
   iterator->Count--;

   if (iterator->Count == 0) {
      return nullptr;
   }

   unsigned                                 index = iterator->Index;
   GraphColor::Tile *                       tile = this->Tile;
   GraphColor::LiveRangePreferenceVector *  adjacencyVector = this->AdjacencyVector;
   unsigned                                 preferenceLiveRangeId = ((*adjacencyVector)[index]).LiveRangeId;
   GraphColor::LiveRange *                  preferenceLiveRange =
      tile->GetSummaryLiveRangeById(preferenceLiveRangeId);

   *cost = ((*adjacencyVector)[index]).Cost;
   *preferenceConstraint = ((*adjacencyVector)[index]).PreferenceConstraint;

   return preferenceLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize the summary preference graph bit graph
//
//------------------------------------------------------------------------------

void
SummaryPreferenceGraph::InitializeBitGraph()
{
   GraphColor::Tile *              tile = this->Tile;
   unsigned                        summaryLiveRangeCount = tile->SummaryLiveRangeCount();
   unsigned                        bitGraphSize = (summaryLiveRangeCount + 1);
   BitGraphs::UndirectedBitGraph * bitGraph = BitGraphs::UndirectedBitGraph::New(bitGraphSize);

   this->BitGraph = bitGraph;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the summary preference graph bit graph
//
//------------------------------------------------------------------------------

void
SummaryPreferenceGraph::BuildBitGraph()
{
   // Initialize empty bit graph.

   this->InitializeBitGraph();

   // Process live ranges, for every preference between a pair of live ranges that have been mapped to 
   // summary live ranges, create a preference between the corresponding pair of summary live ranges as
   // long as those summary live ranges don't conflict.

   GraphColor::Tile *                 tile = this->Tile;
   BitGraphs::UndirectedBitGraph *    bitGraph = this->BitGraph;
   GraphColor::Allocator *            allocator = this->Allocator;
   Tiled::Cost                        zeroCost = allocator->ZeroCost;
   GraphColor::SummaryConflictGraph * summaryConflictGraph = tile->SummaryConflictGraph;
   GraphColor::PreferenceGraph *      preferenceGraph = tile->PreferenceGraph;
   GraphColor::AllocatorCostModel *   costModel = tile->CostModel;

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned l = 1 /*vector-base1*/; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];

      unsigned summaryLiveRangeId1 = liveRange->SummaryLiveRangeId;

      if (summaryLiveRangeId1 != 0) {
         RegisterAllocator::PreferenceConstraint  preferenceConstraint;
         Tiled::Cost                              preferenceCost;

         GraphColor::GraphIterator iter;
         GraphColor::LiveRange * preferenceLiveRange;

         // foreach_preference_liverange
         for (preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&iter, liveRange, &preferenceCost, &preferenceConstraint);
              (preferenceLiveRange != nullptr);
              preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&iter, &preferenceCost, &preferenceConstraint))
         {
            unsigned  summaryLiveRangeId2 = preferenceLiveRange->SummaryLiveRangeId;

            if (summaryLiveRangeId2 != 0) {
               if (summaryLiveRangeId1 != summaryLiveRangeId2) {
                  if (!summaryConflictGraph->HasConflict(summaryLiveRangeId1, summaryLiveRangeId2)) {
                     if (costModel->Compare(&preferenceCost, &zeroCost) > 0) {
                        bitGraph->AddEdge(summaryLiveRangeId1, summaryLiveRangeId2);
                     }
                  }
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize empty adjacency vector after bit graph has been built.
//
//------------------------------------------------------------------------------

void
SummaryPreferenceGraph::InitializeAdjacencyVector()
{
   GraphColor::Tile *                       tile = this->Tile;
   GraphColor::Allocator *                  allocator = this->Allocator;
   BitGraphs::UndirectedBitGraph *          bitGraph = this->BitGraph;
   GraphColor::LiveRangePreferenceVector *  adjacencyVector;
   unsigned                                 summaryLiveRangeCount = tile->SummaryLiveRangeCount();
   unsigned                                 edgeCount = 0;
   unsigned                                 nodeCount = 0;

   for (unsigned summaryLiveRangeId1 = 1; summaryLiveRangeId1 <= summaryLiveRangeCount; summaryLiveRangeId1++)
   {
      GraphColor::LiveRange * summaryLiveRange1 = tile->GetSummaryLiveRangeById(summaryLiveRangeId1);

      for (unsigned summaryLiveRangeId2 = (summaryLiveRangeId1 + 1);
           summaryLiveRangeId2 <= summaryLiveRangeCount;
           summaryLiveRangeId2++)
      {
         GraphColor::LiveRange * summaryLiveRange2 = tile->GetSummaryLiveRangeById(summaryLiveRangeId2);

         if (bitGraph->TestEdge(summaryLiveRangeId1, summaryLiveRangeId2)) {
            if (!summaryLiveRange1->IsPhysicalRegister) {
               summaryLiveRange1->PreferenceEdgeCount++;
               edgeCount++;
            }

            assert(!summaryLiveRange2->IsPhysicalRegister);
            summaryLiveRange2->PreferenceEdgeCount++;
            edgeCount++;
         }
      }
   }

   nodeCount = summaryLiveRangeCount;
   this->NodeCount = nodeCount;
   this->EdgeCount = edgeCount;

   // Notice, we are leaving room for nodeCount * 2 more edges.  This allows for us to insert edges on the way
   // down to push down preferences coming from parent.

   unsigned size = (edgeCount + (nodeCount * 2));

   adjacencyVector = new GraphColor::LiveRangePreferenceVector(size);
   this->AdjacencyVector = adjacencyVector;

   Tiled::Cost zeroCost = allocator->ZeroCost;
   unsigned index = 0;

   for (unsigned summaryLiveRangeId1 = 1; summaryLiveRangeId1 <= summaryLiveRangeCount; summaryLiveRangeId1++)
   {
      GraphColor::LiveRange * summaryLiveRange1 = tile->GetSummaryLiveRangeById(summaryLiveRangeId1);

      if (summaryLiveRange1->IsPhysicalRegister) {
         continue;
      }

      // Leave room for preference coming from parent.

      ((*adjacencyVector)[index]).LiveRangeId = summaryLiveRangeId1;
      ((*adjacencyVector)[index]).Cost = zeroCost;
      ((*adjacencyVector)[index]).PreferenceConstraint = RegisterAllocator::PreferenceConstraint::SameRegister;
      index++;
      assert(size > 0);
      --size;

      // Set index after hidden edge.  We will adjust later when updating summary on the way down.

      summaryLiveRange1->PreferenceAdjacencyIndex = index;

      for (unsigned summaryLiveRangeId2 = 1; summaryLiveRangeId2 <= summaryLiveRangeCount; summaryLiveRangeId2++)
      {
         if (bitGraph->TestEdge(summaryLiveRangeId1, summaryLiveRangeId2)) {
            assert(summaryLiveRangeId1 != summaryLiveRangeId2);

            ((*adjacencyVector)[index]).LiveRangeId = summaryLiveRangeId2;
            ((*adjacencyVector)[index]).Cost = zeroCost;
            ((*adjacencyVector)[index]).PreferenceConstraint = RegisterAllocator::PreferenceConstraint::SameRegister;
            index++;
            assert(size > 0);
            --size;
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build adjacency vector after bit graph has been built.
//
// Remarks:
//
//    Set the PreferenceEdgeCount and PreferenceAdjacencyIndex on summary variable.
//    Also, set the initial "Degree" on summary variable.  
//
//    AdjacencyVector layout is as follows:
//
//                                                     AdjacenceyVector
//                                                     +----------------------------------+
//    summaryLiveRange1->PreferenceAdjacenceyIndex --> |preferenceingSummaryLiveRangeId1  |
//                                                     |preferenceingSummaryLiveRangeId2  |
//                                                     |...                               |
//    summaryLiveRange1->PreferenceEdgeCount == n      |preferenceingSummaryLiveRangeId(n)|
//                                                     +----------------------------------+
//    summaryLiveRange2->PreferenceAdjacenceyIndex --> |preferenceingSummaryLiveRangeId1  |
//                                                     |preferenceingSummaryLiveRangeId2  |
//                                                     |...                               |
//    summaryLiveRange2->PreferenceEdgeCount == m      |preferenceingSummaryLiveRangeId(m)|
//                                                     +----------------------------------+
//                                                     ....
//
//------------------------------------------------------------------------------

void
SummaryPreferenceGraph::BuildAdjacencyVector()
{
   this->InitializeAdjacencyVector();

   // Process live ranges, for every preference between a pair of live ranges that have been mapped to 
   // summary live ranges, add the cost of that a preference to the cost of the preference between the 
   // corresponding pair of summary live ranges, so long as those summary live ranges don't conflict.

   BitGraphs::UndirectedBitGraph * bitGraph = this->BitGraph;
   GraphColor::Tile *              tile = this->Tile;
   GraphColor::PreferenceGraph *   preferenceGraph = tile->PreferenceGraph;

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned l = 1 /*vector-base1*/; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];

      unsigned summaryLiveRangeId = liveRange->SummaryLiveRangeId;

      if (summaryLiveRangeId != 0) {
         RegisterAllocator::PreferenceConstraint   preferenceConstraint;
         Tiled::Cost                               preferenceCost;

         GraphColor::GraphIterator iter;
         GraphColor::LiveRange * preferenceLiveRange;

         // foreach_preference_liverange
         for (preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&iter, liveRange, &preferenceCost, &preferenceConstraint);
              (preferenceLiveRange != nullptr);
              preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&iter, &preferenceCost, &preferenceConstraint))
         {
            unsigned summaryPreferenceLiveRangeId = preferenceLiveRange->SummaryLiveRangeId;

            if (summaryPreferenceLiveRangeId != 0) {
               if (summaryLiveRangeId != summaryPreferenceLiveRangeId) {
                  if (bitGraph->TestEdge(summaryLiveRangeId, summaryPreferenceLiveRangeId)) {
                     GraphColor::LiveRange * summaryLiveRange;
                     GraphColor::LiveRange * summaryPreferenceLiveRange;

                     summaryLiveRange = tile->GetSummaryLiveRangeById(summaryLiveRangeId);
                     summaryPreferenceLiveRange = tile->GetSummaryLiveRangeById(summaryPreferenceLiveRangeId);

                     this->AddCostForEdges(summaryLiveRange, summaryPreferenceLiveRange, &preferenceCost);
                  }
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compare two register preferences for equality.
//
// Arguments:
//
//    leftValue  - left hand side register preference
//    rightValue - right hand side register preference
//
// Remarks:
//
//    Used to support collections.
//
// Returns:
//
//    true if they are the same register preference.
//
//------------------------------------------------------------------------------

bool
PhysicalRegisterPreference::Equals
(
   GraphColor::PhysicalRegisterPreference leftValue,
   GraphColor::PhysicalRegisterPreference rightValue
)
{
#if 0
   if (leftValue.Register == rightValue.Register)
   {
      if (Tiled::CostValue::CompareEQ(leftValue.Cost.ExecutionCycles, rightValue.Cost.ExecutionCycles))
      {
         if (Tiled::CostValue::CompareEQ(leftValue.Cost.CodeBytes, rightValue.Cost.CodeBytes))
         {
            return true;
         }
      }
   }

   return false;
#else
   return false;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build global register preferences and annotate global live ranges with them.
//
//------------------------------------------------------------------------------

void
PreferenceGraph::BuildGlobalPreferences
(
   GraphColor::Allocator * allocator
)
{
   llvm::MachineFunction *                         machineFunction = allocator->FunctionUnit->machineFunction;
   Tiled::VR::Info *                                 vrInfo = allocator->VrInfo;

   TargetRegisterAllocInfo *     targetRegisterAllocator = allocator->TargetRegisterAllocator;
 
   GraphColor::LiveRange *                         globalLiveRange;
   unsigned                                        definitionAliasTag;
   GraphColor::LiveRange *                         definitionLiveRange;
   unsigned                                        registerAliasTag;
   unsigned                                        useAliasTag;
   GraphColor::LiveRange *                         useLiveRange;
   llvm::SparseBitVector<> *                       globalPreferenceRegisterAliasTagSet;
   GraphColor::PhysicalRegisterPreferenceVector *  globalRegisterPreferenceVector;
   llvm::SparseBitVector<> *                       allocatableRegistersAliasTagSet;
   GraphColor::PreferenceVector *                  preferenceVector;
   unsigned                                        requiredReg;
   unsigned                                        preferredReg;
   Tiled::Cost                                       preferredCost;

   // Process each instruction function and add preference registers to global live ranges.

   preferenceVector = new GraphColor::PreferenceVector();
   preferenceVector->reserve(16);   //elements pushed back in PreferenceInstruction() below

   // [ MachineFunction doesn't have a sequential accessor to all instructions in the function ]

   // foreach_instr_in_func
   llvm::MachineFunction::iterator b;
   for (b = machineFunction->begin(); b != machineFunction->end(); ++b)
   {
      llvm::MachineBasicBlock::instr_iterator ii;
      for (ii = b->instr_begin(); ii != b->instr_end(); ++ii)
      {
         llvm::MachineInstr * instruction = &(*ii);

         if (PreferenceGraph::HasPreference(instruction)) {
            // Calculate preferences and costs.
            targetRegisterAllocator->PreferenceInstruction(instruction, preferenceVector);

            // Add register preferences to global live ranges.

            for (unsigned i = 0, n = preferenceVector->size(); i < n; i++)
            {
               RegisterAllocator::Preference preference = (*preferenceVector)[i];
               definitionAliasTag = preference.AliasTag1;
               useAliasTag = preference.AliasTag2;

               globalLiveRange = nullptr;
               registerAliasTag = VR::Constants::InvalidTag;

               definitionLiveRange = allocator->GetGlobalLiveRange(definitionAliasTag);

               if (definitionLiveRange != nullptr) {
                  globalLiveRange = definitionLiveRange;
                  if (!vrInfo->IsPhysicalRegisterTag(useAliasTag)) {
                     //was:  (!vrInfo->IsRegisterTag(useAliasTag))
                     continue;
                  }
                  registerAliasTag = useAliasTag;

               } else if (vrInfo->IsPhysicalRegisterTag(definitionAliasTag)) {
                  //was:  (vrInfo->IsRegisterTag(definitionAliasTag))
                  registerAliasTag = definitionAliasTag;
                  useLiveRange = allocator->GetGlobalLiveRange(useAliasTag);
                  if (useLiveRange == nullptr) {
                     continue;
                  }
                  globalLiveRange = useLiveRange;
               } else {
                  continue;
               }

               allocatableRegistersAliasTagSet = allocator->GetCategoryRegisters(globalLiveRange);
               globalRegisterPreferenceVector = globalLiveRange->GlobalRegisterPreferenceVector;
               requiredReg = globalLiveRange->Register;
               preferredReg = vrInfo->GetRegister(registerAliasTag);
               preferredCost = preference.Cost;
               allocator->ScaleCyclesByFrequency(&preferredCost, instruction);
               allocator->InsertOrUpdateRegisterPreference(globalRegisterPreferenceVector,
                  requiredReg, preferredReg, preferredCost, allocatableRegistersAliasTagSet);
            }
         }
      }
   }

   // Create register preference bit vectors.

   GraphColor::LiveRangeVector * lrVector = allocator->GetGlobalLiveRangeEnumerator();

   // foreach_global_liverange_in_allocator
   for (unsigned l = 1; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *   globalLiveRange = (*lrVector)[l];

      globalPreferenceRegisterAliasTagSet = globalLiveRange->GlobalPreferenceRegisterAliasTagSet;
      globalRegisterPreferenceVector = globalLiveRange->GlobalRegisterPreferenceVector;

      for (unsigned index = 0; index < globalRegisterPreferenceVector->size(); ++index)
      {
         preferredReg = ((*globalRegisterPreferenceVector)[index]).Register;
         registerAliasTag = vrInfo->GetTag(preferredReg);
         globalPreferenceRegisterAliasTagSet->set(registerAliasTag);
      }
   }
}

bool
PreferenceGraph::HasPreference(llvm::MachineInstr * instruction) {
   if (instruction->isCopy()) {
      return true;
   }
   return false;
}

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled
