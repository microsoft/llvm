//===-- Graphs/BitGraph.h ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_BITGRAPH_H
#define TILED_GRAPHS_BITGRAPH_H

#include "llvm/ADT/BitVector.h"

namespace Tiled
{

//-----------------------------------------------------------------------------
//
// Tiled Allocator
// Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
// Description:
//
//    Package to implement bit graph operations. Bit graphs are unweighted
//    directed and undirected graphs that use bits to represent adjacencies
//    rather than lists or vectors. This representation is ideal for dense
//    graphs that require constant time testing, adding, and removing edges.
//    Visiting the neighbors of a node in the bit graph is proportional to the
//    number of nodes in the graph, not the number of edges of a node.  Vector
//    and list graph implementations have faster neighbor visiting.
//
// Remarks:
//
//    Some duplication has been done to avoid virtual functions in time
//    performance critical areas such as computing bitoffsets.
//
// See Also:
//
//    foreach_neighbor_in_directed_graph, et al in bit-graph-foreach.h
//
//-----------------------------------------------------------------------------

namespace BitGraphs
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    This class contains the abstract class for the bit graph package.
//
// Remarks:
//
//    Directed and undirected bit graph implementations are provided based
//    upon this abstract graph.
//
//-----------------------------------------------------------------------------


class BitGraph
{

 public:

   virtual void Delete() = 0;

   void
   And
   (
      BitGraphs::BitGraph * bitGraph
   );

   void Clear();

   void
   Minus
   (
      BitGraphs::BitGraph * bitGraph
   );

   void Not();

   void
   Or
   (
      BitGraphs::BitGraph * bitGraph
   );

   void
   Xor
   (
      BitGraphs::BitGraph * bitGraph
   );

 protected:

   llvm::BitVector * EdgeBitVector;
   unsigned          Size;
};

//-----------------------------------------------------------------------------
//
// Description:
//
//    This class implements an undirected bit graph package.
//
// Remarks:
//
//    The undirected bit graph is essentially a lower triangular bit-matrix
//    where the edges are represented by a bit indexed by row and column. This
//    naturally consumes half the space as the directed.
//
//    Macro iterators to visit the neighbors are provided elsewhere.
//
//-----------------------------------------------------------------------------

class UndirectedBitGraph : public BitGraphs::BitGraph
{

 public:

   void Delete() override;

   static BitGraphs::UndirectedBitGraph *
   New
   (
      unsigned size
   );

 public:

   void
   AddAllEdges
   (
      unsigned node
   );

   void
   AddEdge
   (
      unsigned sourceNode,
      unsigned sinkNode
   );

   BitGraphs::UndirectedBitGraph * Copy();

   void
   RemoveAllEdges
   (
      unsigned node
   );

   void
   RemoveEdge
   (
      unsigned sourceNode,
      unsigned sinkNode
   );

   bool
   TestEdge
   (
      unsigned sourceNode,
      unsigned sinkNode
   );

 private:

   unsigned
   BitOffset
   (
      unsigned sourceNode,
      unsigned sinkNode
   );
};

} // namespace BitGraphs
} // namespace Tiled

#endif // TILED_GRAPHS_BITGRAPH_H
