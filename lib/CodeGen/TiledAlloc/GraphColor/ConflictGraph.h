//===-- GraphColor/ConflictGraph.h ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_CONFLICTGRAPH_H
#define TILED_GRAPHCOLOR_CONFLICTGRAPH_H

#include "../GraphColor/Graph.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{
class Allocator;
class GraphIterator;
class LiveRange;
class Tile;
   
//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Register allocation conflict graph.
//
//-------------------------------------------------------------------------------------------------------------

class ConflictGraph : public GraphColor::Graph
{

public:

   static GraphColor::ConflictGraph *
   New
   (
      GraphColor::Tile * tile
   );

public:

   static void
   BuildGlobalConflicts
   (
      GraphColor::Allocator * allocator
   );

   GraphColor::LiveRange *
   GetFirstConflictLiveRange
   (
      GraphColor::GraphIterator * iterator,
      GraphColor::LiveRange *     liveRange
   );

   GraphColor::LiveRange *
   GetNextConflictLiveRange
   (
      GraphColor::GraphIterator * iterator
   );

   bool
   HasConflict
   (
      unsigned liveRangeId1,
      unsigned liveRangeId2
   );

   static void
   MarkGlobalLiveRangesSpanningCall
   (
      GraphColor::Allocator *   allocator,
      llvm::SparseBitVector<> * liveBitVector
   );

   static void
   UpdateGlobalConflicts
   (
      llvm::SparseBitVector<> * globalLiveRangeUpdateAliasTagSet,
      GraphColor::Allocator *   allocator
   );

public:

   bool        DoPressureCalculation;
   unsigned    MaxFloatRegisterCount;
   unsigned    MaxIntegerRegisterCount;
  
protected:

   static void
   AddGlobalConflicts
   (
      GraphColor::Tile *        tile,
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * killBitVector,
      llvm::SparseBitVector<> * liveBitVector
   );

   void BuildAdjacencyVector() override;

   void BuildBitGraph() override;

   void InitializeAdjacencyVector() override;

   void InitializeBitGraph() override;

#if 0
   static bool
   IsPreferentialCopy
   (
      Types::Type * definitionType,
      Types::Type * sourceType
   );
#endif

protected:

   GraphColor::IdVector * AdjacencyVector;

private:

   void
   AddConflictEdges
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * definitionBitVector,
      llvm::SparseBitVector<> * liveBitVector
   );

   void
   AddConflictEdges
   (
      GraphColor::Tile *      nestedTile,
      GraphColor::LiveRange * summaryLiveRange
   );

   void
   AddConflictEdges
   (
      llvm::MachineOperand * operand
   );

   void
   AddDefinitionLiveConflicts
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * definitionBitVector,
      llvm::SparseBitVector<> * liveBitVector
   );

   static void
   AddGlobalRegisterConflicts
   (
      GraphColor::Tile *      tile,
      llvm::MachineOperand *  operand
   );

   void
   MarkLiveRangesSpanningCall
   (
      llvm::SparseBitVector<> * liveBitVector
   );
};

/*
comment ConflictGraph::AdjacencyVector
{
   // Adjacency vector using an offset run-length encoding and accessed by an enumerator
}
*/

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Register allocation summary conflict graph.
//
//-------------------------------------------------------------------------------------------------------------

class SummaryConflictGraph : public GraphColor::ConflictGraph
{

public:

   static GraphColor::SummaryConflictGraph *
   New
   (
      GraphColor::Tile * tile
   );

public:

   GraphColor::LiveRange *
   GetFirstConflictSummaryLiveRange
   (
      GraphColor::GraphIterator *   iterator,
      GraphColor::LiveRange *       liveRange
   );

   GraphColor::LiveRange *
   GetNextConflictSummaryLiveRange
   (
      GraphColor::GraphIterator * iterator
   );

protected:

   void BuildAdjacencyVector() override;

   void BuildBitGraph() override;

   void InitializeAdjacencyVector() override;

   void InitializeBitGraph() override;

private:

   unsigned AdjacencyVectorInitialSize;
};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // TILED_GRAPHCOLOR_CONFLICTGRAPH_H
