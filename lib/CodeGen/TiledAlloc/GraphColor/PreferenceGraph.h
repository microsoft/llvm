//===-- GraphColor/PreferenceGraph.h ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_PREFERENCEGRAPH_H
#define TILED_GRAPHCOLOR_PREFERENCEGRAPH_H

#include "Graph.h"
#include "../Preference.h"

namespace Tiled
{
class Cost;

namespace RegisterAllocator
{
namespace GraphColor
{
class GraphIterator;
class LiveRange;

   
//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Register allocation preference graph.
//
//-------------------------------------------------------------------------------------------------------------

class PreferenceGraph : public GraphColor::Graph
{

public:

   //[nyi] override void Delete();

   static GraphColor::PreferenceGraph *
   New
   (
      GraphColor::Tile * tile
   );

public:

   static void
   BuildGlobalPreferences
   (
      GraphColor::Allocator * allocator
   );

   GraphColor::LiveRange *
   GetFirstPreferenceLiveRange
   (
      GraphColor::GraphIterator *               iterator,
      GraphColor::LiveRange *                   liveRange,
      Tiled::Cost *                             cost,
      RegisterAllocator::PreferenceConstraint * preferenceConstraint
   );

   llvm::SparseBitVector<> *
   GetGlobalRegisterPreferences
   (
      GraphColor::LiveRange * globalLiveRange
   );

   GraphColor::LiveRange *
   GetNextPreferenceLiveRange
   (
      GraphColor::GraphIterator *               iterator,
      Tiled::Cost *                             cost,
      RegisterAllocator::PreferenceConstraint * preferenceConstraint
   );

   bool
   HasPreference
   (
      unsigned liveRangeId1,
      unsigned liveRangeId2
   );

   static bool
   HasPreference
   (
      llvm::MachineInstr * instruction
   );    //TBD

   void
   SetPreferenceCost
   (
      GraphColor::GraphIterator *  iterator,
      Tiled::Cost *                  cost
   );

   void SubtractCostForGlobalPreferenceEdges();

protected:

   void
   AddCostForEdges
   (
      GraphColor::LiveRange * liveRange1,
      GraphColor::LiveRange * liveRange2,
      Tiled::Cost * cost
   );

   void BuildAdjacencyVector() override;

   void BuildBitGraph() override;

   void InitializeAdjacencyVector() override;

   void InitializeBitGraph() override;

protected:

   GraphColor::LiveRangePreferenceVector * AdjacencyVector;
   GraphColor::PreferenceVector *          PreferenceVector;

private:

   void
   AddBoundaryPreferenceEdges
   (
      llvm::MachineInstr * instruction
   );

   void
   AddCostForBoundaryPreferenceEdges
   (
      llvm::MachineInstr * instruction
   );

   void
   AddCostForGlobalPreferenceEdges
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * liveBitVector,
      bool                      doPositiveCost
   );

   void
   AddCostForGlobalPreferenceEdges
   (
      bool doPositiveCost
   );

   void
   AddCostForGlobalPreferenceEdges
   (
      GraphColor::LiveRange * liveRange,
      bool                    doPositiveCost
   );

   void
   AddCostForInstructionPreferenceEdge
   (
      llvm::MachineInstr * instruction
   );

   void
   AddCostForSummaryPreferenceEdges
   (
      llvm::MachineInstr * instruction
   );

   void
   AddCostForSummaryPreferenceEdges
   (
      GraphColor::LiveRange * summaryLiveRange
   );

   void AddGlobalPreferenceEdges();

   void
   AddGlobalPreferenceEdges
   (
      GraphColor::LiveRange * liveRange
   );

   void
   AddInstructionPreferenceEdges
   (
      llvm::MachineInstr * instruction
   );

   void
   AddSummaryPreferenceEdges
   (
      llvm::MachineInstr * instruction
   );

   void
   AddSummaryPreferenceEdges
   (
      GraphColor::LiveRange * summaryLiveRange
   );
};

/*
comment PreferenceGraph::AdjacencyVector
{
   // Adjacency vector using an index and run-length encoding and accessed by an enumerator.
   // Index and run-length is actually encoded in LiveRange->PreferenceAdjacencyIndex
   // and LiveRange->PreferenceEdegCount.
}
*/

//------------------------------------------------------------------------------
//
// Description:
//
//    Register allocation summary preference graph.
//
//------------------------------------------------------------------------------

class SummaryPreferenceGraph : public GraphColor::PreferenceGraph
{

public:

   //[nyi] override void Delete();

   static GraphColor::SummaryPreferenceGraph *
   New
   (
      GraphColor::Tile * tile
   );

public:

   GraphColor::LiveRange *
   GetFirstPreferenceSummaryLiveRange
   (
      GraphColor::GraphIterator *               iterator,
      GraphColor::LiveRange *                   liveRange,
      Tiled::Cost *                             cost,
      RegisterAllocator::PreferenceConstraint * preferenceConstraint
   );

   GraphColor::LiveRange *
   GetNextPreferenceSummaryLiveRange
   (
      GraphColor::GraphIterator *               iterator,
      Tiled::Cost *                             cost,
      RegisterAllocator::PreferenceConstraint * preferenceConstraint
   );

   void
   InstallParentPreference
   (
      GraphColor::LiveRange *                   liveRange,
      GraphColor::LiveRange *                   preferenceLiveRange,
      Tiled::Cost *                             cost,
      RegisterAllocator::PreferenceConstraint * preferenceConstraint
   );

protected:

   void BuildAdjacencyVector() override;

   void BuildBitGraph() override;

   void InitializeAdjacencyVector() override;

   void InitializeBitGraph() override;
};
} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_PREFERENCEGRAPH_H
