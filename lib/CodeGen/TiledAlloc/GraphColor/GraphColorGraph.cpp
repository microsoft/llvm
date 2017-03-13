//===-- GraphColor/GraphColorGraph.cpp --------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Graph.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

//------------------------------------------------------------------------------
// Description:
//
//    Base graph build call - delegates to bit-graph and adjacency builds.
//
//------------------------------------------------------------------------------

void
Graph::Build()
{
   this->BuildBitGraph();
   this->BuildAdjacencyVector();
}

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled
