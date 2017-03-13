//===-- Graphs/BitGraph.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Package to implement bit graph operations. Bit graphs are unweighted 
// directed and undirected graphs that use bits to represent adjacencies 
// rather than lists or vectors. This representation is ideal for dense 
// graphs that require constant time testing, adding, and removing edges.  
// Visiting the neighbors of a node in the bit graph is proportional to the
// number of nodes in the graph, not the number of edges of a node.
// Vector and list graph implementations have faster neighbor visiting.
//
//===----------------------------------------------------------------------===//

#include "BitGraph.h"

namespace Tiled
{
namespace BitGraphs
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Intersect the bits of the two directed graphs. The resulting
//    graph nodes will have edges to other nodes only if edges to
//    those nodes existed in both graphs.
//
// Arguments:
//
//    bitGraph - The other bit graph.
//
//-----------------------------------------------------------------------------

void
BitGraph::And
(
   BitGraphs::BitGraph * bitGraph
)
{
   *(this->EdgeBitVector) &= *(bitGraph->EdgeBitVector);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Union the bits of the two directed graphs. The resulting graph
//    nodes will have edges to other nodes if edges to those nodes
//    existed in either graphs.
//
// Arguments:
//
//    bitGraph - The other bit graph.
//
//-----------------------------------------------------------------------------

void
BitGraph::Or
(
   BitGraphs::BitGraph * bitGraph
)
{
   *(this->EdgeBitVector) |= *(bitGraph->EdgeBitVector);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Xor the bits of the two directed graphs. The resulting graph
//    nodes will have edges to other nodes if edges to those nodes
//    existed in one of the graphs, but not both.
//
// Arguments:
//
//    bitGraph - The other bit graph.
//
//-----------------------------------------------------------------------------

void
BitGraph::Xor
(
   BitGraphs::BitGraph * bitGraph
)
{
   *(this->EdgeBitVector) ^= *(bitGraph->EdgeBitVector);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Subtract the bits of the two directed graphs. The resulting
//    graph nodes will have edges to other nodes if edges to those
//    nodes do no exist in the the second graph.
//
// Arguments:
//
//    bitGraph - The other bit graph.
//
//-----------------------------------------------------------------------------

void
BitGraph::Minus
(
   BitGraphs::BitGraph * bitGraph
)
{
   (*(this->EdgeBitVector)).reset(*(bitGraph->EdgeBitVector));
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Invert the bits of the given directed graph. The resulting graph
//    will have edges to every node in which it originally did not
//    have an edge.
//
//-----------------------------------------------------------------------------

void
BitGraph::Not()
{
   this->EdgeBitVector->flip();
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Remove all edges in the given directed bitgraph.
//
//-----------------------------------------------------------------------------

void
BitGraph::Clear()
{
   this->EdgeBitVector->clear();
}


//-----------------------------------------------------------------------------
//
// Description:
//
//    Return the offset in bits to find the undirected edge between
//    the given nodes in the graph.
//
// Arguments:
//
//    sourceNode - Source node bit number
//    sinkNode   - Sink node bit number
//
// Returns:
//
//    BitOffset to find the edge.
//
//-----------------------------------------------------------------------------

unsigned
UndirectedBitGraph::BitOffset
(
   unsigned sourceNode,
   unsigned sinkNode
)
{
   assert(sourceNode < this->Size);
   assert(sinkNode < this->Size);

   if (sourceNode < sinkNode) {
      return ((sinkNode * (sinkNode + 1)) / 2) + sourceNode;
   } else {
      return ((sourceNode * (sourceNode + 1)) / 2) + sinkNode;
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Create a new undirected graph to hold the given number of nodes.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime.
//    size - The number of rows (and columns) in the graph.
//
// Returns:
//
//    An undirected bit graph object
//
// Remarks:
//
//    The rows and columns are 0-based.  This means the addressable space is
//    (0, 0) - (size - 1, size - 1).
//
//-----------------------------------------------------------------------------

UndirectedBitGraph *
UndirectedBitGraph::New
(
   unsigned size
)
{
   UndirectedBitGraph * undirected = new UndirectedBitGraph();
   unsigned             numberBits;

   undirected->Size = size;

   // Guard against bit offset calculations overflowing.
   assert(size < 65536);

   numberBits = undirected->BitOffset(size - 1, size - 1) + 1;
   undirected->EdgeBitVector = new llvm::BitVector(numberBits);

   return undirected;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Return a newly allocated graph with the same number of nodes and
//    the exact same edges as the original graph.
//
// Returns:
//
//    UndirectedBitGraph BitGraphs.
//
//-----------------------------------------------------------------------------

UndirectedBitGraph *
UndirectedBitGraph::Copy()
{
   UndirectedBitGraph * undirected = new UndirectedBitGraph();

   undirected->Size = this->Size;
   undirected->EdgeBitVector = new llvm::BitVector(*(this->EdgeBitVector));

   return undirected;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Delete and free the undirected BitGraphs.
//
//-----------------------------------------------------------------------------

void
UndirectedBitGraph::Delete()
{
   this->EdgeBitVector->clear();
   // TODO
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Test whether an edge exists between the two given graph nodes.
//
// Arguments:
//
//    sourceNode - Source node bit number
//    sinkNode   - Sink node bit number
//
// Returns:
//
//    Boolean true if the edge exists.
//
//-----------------------------------------------------------------------------

bool
UndirectedBitGraph::TestEdge
(
   unsigned sourceNode,
   unsigned sinkNode
)
{
   unsigned bitOff = this->BitOffset(sourceNode, sinkNode);

   return this->EdgeBitVector->test(bitOff);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add an undirected edge between the two given graph nodes.
//
// Arguments:
//
//    sourceNode - Source node bit number
//    sinkNode   - Sink node bit number
//
//-----------------------------------------------------------------------------

void
UndirectedBitGraph::AddEdge
(
   unsigned sourceNode,
   unsigned sinkNode
)
{
   unsigned bitOff = this->BitOffset(sourceNode, sinkNode);

   this->EdgeBitVector->set(bitOff);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Remove the directed edge between the two given graph nodes.
//
// Arguments:
//
//    sourceNode - Source node bit number
//    sinkNode   - Sink node bit number
//
//-----------------------------------------------------------------------------

void
UndirectedBitGraph::RemoveEdge
(
   unsigned sourceNode,
   unsigned sinkNode
)
{
   unsigned bitOff = this->BitOffset(sourceNode, sinkNode);

   this->EdgeBitVector->reset(bitOff);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add an undirected edge between the given node and all other nodes in
//    the graph.
//
// Arguments:
//
//    node - Node bit number
//
//-----------------------------------------------------------------------------

void
UndirectedBitGraph::AddAllEdges
(
   unsigned node
)
{
   for (unsigned neighbor = 0; neighbor < this->Size; neighbor++)
   {
      this->AddEdge(node, neighbor);
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Remove all edges between the given node and all other nodes in
//    the graph.
//
// Arguments:
//
//    node - Node bit number
//
//-----------------------------------------------------------------------------

void
UndirectedBitGraph::RemoveAllEdges
(
   unsigned node
)
{
   for (unsigned neighbor = 0; neighbor < this->Size; neighbor++)
   {
      this->RemoveEdge(node, neighbor);
   }
}


} // namespace BitGraphs
} // namespace Tiled
