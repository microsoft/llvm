//===-- Graphs/NodeFlowOrder.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_NODEFLOWORDER_H
#define TILED_GRAPHS_NODEFLOWORDER_H

#include "llvm/CodeGen/MachineBasicBlock.h"

namespace Tiled
{
namespace Graphs
{

class FlowGraph;
class NodeWalker;
class NumberNodeVisitor;

enum class Order
{
   IllegalSentinel,
   //PreOrder,
   //ReversePreOrder,
   PostOrder,
   ReversePostOrder,
   //RegionReversePostOrder,
   //RegionReversePostOrderExcludeHandlers,
   //ReverseGraphReversePostOrder,
   //ExtendedBasicBlockPreOrder,
   ExtendedBasicBlockPostOrder,
   //ExtendedBasicBlockReversePreOrder,
   ExtendedBasicBlockReversePostOrder
   //ContextSensitiveReversePostOrder,
   //UserDefinedOrder
};


class NodeFlowOrder
{
public:

   void Delete();

   NodeFlowOrder();

   static NodeFlowOrder * New();

public:

   // Orders used by dataflow/RA:
   //   ReversePostOrder, PostOrder, ?ExtendedBasicBlockReversePostOrder
   void
   Build
   (
      FlowGraph*  graph,
      Order       order
   );

   void CanonicalizeStartAndEndPositions();

   llvm::MachineBasicBlock*
   Node
   (
      unsigned  position
   );

   unsigned
   NodeId
   (
      unsigned  position
   );

   unsigned
   Position
   (
      unsigned  nodeId
   );

   unsigned
   Position
   (
      llvm::MachineBasicBlock* node
   );

   unsigned NodeCount();

public:

   FlowGraph*      Graph;
   bool            IsValid();
   Order           Order;

private:

   unsigned
   InitialPosition
   (
      llvm::MachineBasicBlock* node
   );

private:

   NumberNodeVisitor*          nodeVisitor;
   NodeWalker*                 nodeWalker;
   std::vector<unsigned>*      nodeIdToPositionVector;
   std::vector<unsigned>*      positionToNodeIdVector;
   unsigned                    graphVersionNumber;
};


} // namespace Graphs
} // namespace Tiled

#endif // end TILED_GRAPHS_NODEFLOWORDER_H
