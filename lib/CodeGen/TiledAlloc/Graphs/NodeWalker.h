//===-- Graphs/NodeWalker.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_NODEWALKER_H
#define TILED_GRAPHS_NODEWALKER_H

#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "../Dataflow/Traverser.h"

namespace Tiled
{
namespace Graphs
{

class FlowGraph;
class NumberNodeVisitor;

//------------------------------------------------------------------------------
//
// Description:
//
//    Helper class used to iteratively perform recursive walks.
//
// Remarks:
//
//    Points to the node visited and the next edge to traverse out of that node.
//
//------------------------------------------------------------------------------

enum class GraphWalkerLink
{
   IllegalSentinel,
   Successor,
   Predecessor
};

class NodeWalker
{
public:
     typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;
     //warning: This uses common internal type of llvm::MachineBasicBlock for successors & predecessors,
     // using only the public types succ_iterator & pred_iterator would be very akward for this client.

   struct XcessorRange
   {
      Xcessor begin;
      Xcessor end;

      XcessorRange() : begin(), end() {}
      XcessorRange(Xcessor x1, Xcessor x2) : begin(x1), end(x2) {}

      bool
      empty() const
      {
         return (this->begin == this->end);
      }
   };

   struct StackNode
   {
      XcessorRange              Edge;
            // Next edge to follow when node is reconsidered
      llvm::MachineBasicBlock*  Node;
            // Partially visited node
   };

public:
   virtual void Delete();

   static Graphs::NodeWalker * New();

   void
   WalkFrom
   (
      NumberNodeVisitor*          visitor,
      llvm::MachineBasicBlock*    node
   );

   FlowGraph *   graph;

protected:
   virtual void AllocateTemporaryData();
   virtual void FreeTemporaryData();

   void Initialize();

   virtual void
   IterativeWalk
   (
      llvm::MachineBasicBlock* node,
      GraphWalkerLink          link
   );

   virtual void
   PushNode
   (
      llvm::MachineBasicBlock* node,
      GraphWalkerLink          link
   );

   virtual void
   WalkNodeAndPredecessors
   (
      llvm::MachineBasicBlock* node
   );

   virtual void
   WalkNodeAndSuccessors
   (
      llvm::MachineBasicBlock* node
   );

protected:
   unsigned                            graphVersionNumber;
   NumberNodeVisitor*                  nodeVisitor;
   bool                                walkDisconnectedNodes;
   std::vector<NodeWalker::StackNode>* NodeStack;
         // Stack of partially visited nodes
   unsigned                            NodeStackDepth;

   llvm::BitVector*                    VisitedBitVector;
   llvm::BitVector*                    QueuedBitVector;
};

//------------------------------------------------------------------------------
//
// Description:
//
//    This class represents a extended basic block graph walker.
//
// Remarks:
//
//    It walks the nodes in extended basic block order.  It can walk
//    the nodes from the start node in forward order using Walk, or
//    from the end node in reverse order using ReverseWalk.  Or you
//    can specify a starting point for the walk using WalkFrom or
//    ReverseWalkFrom.  By default, the walk sees all nodes, including
//    those disconnected from the starting point of the walk, but
//    setting WalkDisconnectedNodes to false disables this.
//
//------------------------------------------------------------------------------

class ExtendedBasicBlockWalker : public Graphs::NodeWalker
{
  public:

   void Delete() override;

   static Graphs::ExtendedBasicBlockWalker * New();

   static bool
   IsEntry
   (
      llvm::MachineBasicBlock * node
   );

protected:

   void AllocateTemporaryData() override;
   void FreeTemporaryData() override;

   void
   IterativeWalk
   (
      llvm::MachineBasicBlock * node,
      Graphs::GraphWalkerLink   link
   ) override;

   llvm::MachineBasicBlock *
   PopEntry();

   void
   PushEntry
   (
      llvm::MachineBasicBlock * node
   );

   void
   WalkNodeAndSuccessors
   (
      llvm::MachineBasicBlock * node
   ) override;

protected:

   Graphs::MachineBasicBlockVector *  EntryNodeStack;
   unsigned                           EntryNodeStackDepth;
   llvm::BitVector *                  QueuedEntryBitVector;
};

} // namespace Graphs
} // namespace Tiled

#endif // end TILED_GRAPHS_NODEWALKER_H
