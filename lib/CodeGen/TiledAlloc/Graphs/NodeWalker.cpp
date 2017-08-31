//===-- Graphs/NodeWalker.cpp -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "NodeWalker.h"
#include "NumberNodeVisitor.h"
#include "../GraphColor/Tile.h"

namespace Tiled
{
namespace Graphs
{

NodeWalker*
NodeWalker::New()
{
   NodeWalker* nodeWalker = new NodeWalker();

   nodeWalker->Initialize();

   return nodeWalker;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate common temporary data structures for a depth first walk.
//
//------------------------------------------------------------------------------

void
NodeWalker::AllocateTemporaryData()
{
   assert(this->VisitedBitVector == nullptr);
   assert(this->QueuedBitVector == nullptr);

   // Note that node IDs are 1 based.
   const unsigned requiredLength = this->graph->MaxNodeId + 1;

   this->VisitedBitVector = new llvm::BitVector(requiredLength);
   this->QueuedBitVector =  new llvm::BitVector(requiredLength);

   assert(this->NodeStack == nullptr);

   this->NodeStack = new std::vector<NodeWalker::StackNode>(requiredLength);
   this->NodeStackDepth = 0;
}

void
NodeWalker::FreeTemporaryData()
{
   delete this->VisitedBitVector;
   delete this->QueuedBitVector;
   delete this->NodeStack;
   this->VisitedBitVector = this->QueuedBitVector = nullptr;
   this->NodeStack = nullptr;
}

void
NodeWalker::Initialize()
{
   this->walkDisconnectedNodes = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Walk visitor over nodes of graph in depth first order, starting
//    from the indicated node.
//
// Arguments:
//
//    nodeVisitor - visitor to walk over graph
//    startingNode - node to begin walk 
//
//------------------------------------------------------------------------------

void
NodeWalker::WalkFrom
(
   NumberNodeVisitor*        nodeVisitor,
   llvm::MachineBasicBlock*  startingNode
)
{
   // We should have a valid visitor.
   assert(nodeVisitor != nullptr);

   // This node should be relevant.
   assert(startingNode->getParent() == this->graph->machineFunction);

   // This walker should not be in the middle of a walk already.
   assert(this->nodeVisitor == nullptr);
   this->nodeVisitor = nodeVisitor;

   // Notify the visitor that the walk is starting.
   this->nodeVisitor->Begin(this->graph);

   // Cache the graph version to detect if the graph is changed during
   // the walk.
   this->graphVersionNumber = this->graph->VersionNumber;

   // Allocate temporary data structures.
   this->AllocateTemporaryData();

   // Start at the indicated node.
   this->WalkNodeAndSuccessors(startingNode);

   if (this->walkDisconnectedNodes) {
      // Now handle any disconnected nodes.

      llvm::MachineFunction* machineFunction = this->graph->machineFunction;

      llvm::MachineFunction::iterator node;
      for (node = machineFunction->begin(); node != machineFunction->end(); ++node)
      {
         if (!this->VisitedBitVector->test(node->getNumber())) {
            // This node could not be reached from the first
            // node. Notify the visitor, which may want to give this
            // node special treatment.

            this->nodeVisitor->UnreachedNode(&*node);
            this->WalkNodeAndSuccessors(&*node);
         }
      }
   }

   // Done walking.
   this->FreeTemporaryData();

   this->nodeVisitor->End();
   this->nodeVisitor = nullptr;  //TBD: precede with a delete command, or is it shared by other instances ??
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Walk visitor over this node and all successors, in depth first order.
//
// Arguments:
//
//    node - node of interest
//
//------------------------------------------------------------------------------

void
NodeWalker::WalkNodeAndSuccessors
(
   llvm::MachineBasicBlock* node
)
{
   this->IterativeWalk(node, GraphWalkerLink::Successor);
}

void
NodeWalker::WalkNodeAndPredecessors(llvm::MachineBasicBlock* node)
{
   this->IterativeWalk(node, GraphWalkerLink::Predecessor);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Walk visitor over this node and all successors, in depth
//    first order.
//
// Arguments:
//
//    node - node of interest
//    link - direction of edges to follow
//
// Remarks:
//
//    Iterative walk keeping a stack of nodes via an array member.  The
//    GraphWalkerLink indicates which link of the edges to follow.
//
//------------------------------------------------------------------------------

void
NodeWalker::IterativeWalk
(
   llvm::MachineBasicBlock*  node,
   GraphWalkerLink           link
)
{
   NodeWalker::StackNode* stackNode;

   assert(this->NodeStackDepth == 0);
   assert(node != nullptr);
   assert(!this->VisitedBitVector->test(node->getNumber()));

   // Queue this block and begin the walk.
   this->PushNode(node, link);

   while (this->NodeStackDepth > 0)
   {
      stackNode = &((*this->NodeStack)[this->NodeStackDepth - 1]);
      node = stackNode->Node;

      // The graph should not have changed out from under us.
      assert(this->graph->VersionNumber == this->graphVersionNumber);

      // First time seeing this node?
      if (!this->VisitedBitVector->test(node->getNumber())) {
         this->VisitedBitVector->set(node->getNumber());

         this->nodeVisitor->PreVisit(node);
      }

      // Recurse on block successors.

      if (!stackNode->Edge.empty()) {
         Xcessor edge = stackNode->Edge.begin;

         while (edge != stackNode->Edge.end)
         {
            if (link == GraphWalkerLink::Successor) {
               llvm::MachineBasicBlock* successorNode = (*edge);

               if (!this->QueuedBitVector->test(successorNode->getNumber())) {
                  this->PushNode(successorNode, link);
                  stackNode->Edge.begin = edge + 1;
                  break;
               }

            } else {
               assert(link == GraphWalkerLink::Predecessor);

               llvm::MachineBasicBlock* predecessorNode = (*edge);

               if (!this->QueuedBitVector->test(predecessorNode->getNumber())) {
                  this->PushNode(predecessorNode, link);
                  stackNode->Edge.begin = edge + 1;
                  break;
               }
            }

            ++edge;
         }

         if (edge != stackNode->Edge.end) {
            // More succ/pred edges to process before finishing this node.
            continue;
         }
      }

      // The graph should not have changed out from under us.
      assert(this->graph->VersionNumber == this->graphVersionNumber);

      this->nodeVisitor->PostVisit(node);

      this->NodeStackDepth--;
   }
}

void
NodeWalker::PushNode
(
   llvm::MachineBasicBlock*  node,
   GraphWalkerLink           link
)
{
   NodeWalker::StackNode* stackNode;

   assert(!this->QueuedBitVector->test(node->getNumber()));
   this->QueuedBitVector->set(node->getNumber());

   stackNode = &((*this->NodeStack)[this->NodeStackDepth]);
   assert(this->NodeStackDepth <= this->graph->MaxNodeId);
   this->NodeStackDepth++;

   stackNode->Node = node;

   if (link == GraphWalkerLink::Successor) {
      stackNode->Edge = XcessorRange(node->succ_begin(), node->succ_end());
   } else {
      assert(link == GraphWalkerLink::Predecessor);
      stackNode->Edge = XcessorRange(node->pred_begin(), node->pred_end());
   }
}

void
NodeWalker::Delete()
{
   if (this->NodeStack != nullptr) {
      delete this->NodeStack;
   }
   this->NodeStack = nullptr;

   if (this->VisitedBitVector != nullptr) {
      delete this->VisitedBitVector;
   }
   this->VisitedBitVector = nullptr;

   if (this->QueuedBitVector != nullptr) {
      delete this->QueuedBitVector;
   }
   this->QueuedBitVector = nullptr;

   this->graph = nullptr;
}


//------------------------------------------------------------------------------
//
// Tiled Allocator
// Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
// Description:
//
//    Graph package extended basic block walker.
//
// Remarks:
//
//    Oversees traversal of the nodes of the graph in extended basic
//    block order (by EBB, DFO within EBB) , invoking a visitor as
//    each node is first encountered and again immediately after the
//    node's successors (or predecessors, for a reverse walk) have been visited.
//
//    No assumption is made about graph connectivity.
//
//------------------------------------------------------------------------------

void
ExtendedBasicBlockWalker::Delete()
{
   delete this->EntryNodeStack;
   this->EntryNodeStack = nullptr;

   NodeWalker::Delete();
}

ExtendedBasicBlockWalker *
ExtendedBasicBlockWalker::New()
{
   ExtendedBasicBlockWalker * extendedBasicBlockWalker = new ExtendedBasicBlockWalker();

   extendedBasicBlockWalker->Initialize();

   return extendedBasicBlockWalker;
}

bool isJoin(llvm::MachineBasicBlock * node)
{
   llvm::MachineBasicBlock::pred_iterator p = node->pred_begin();
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
//    Test a given node to see if it meets the critera for an entry.
//
// Arguments:
//
//    node - Node to test.
//
// Returns:
//
//    True if node is entry.
//
//------------------------------------------------------------------------------

bool
ExtendedBasicBlockWalker::IsEntry
(
   llvm::MachineBasicBlock * block
)
{
   if (block->pred_empty()) {
      return true;
   } else if (isJoin(block)) {
      return true;
   } else {
      if (!block->empty()) {
         llvm::MachineInstr *   firstInstruction = &(block->instr_front());
         if (RegisterAllocator::GraphColor::Tile::IsExitTileInstruction(firstInstruction)) {
            // Force tile exits to count as entries.
            return true;
         }
      }

      return false;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Walk visitor over this node and all successors, in depth first order.
//
// Arguments:
//
//    node - entry node of interest
//
//------------------------------------------------------------------------------

void
ExtendedBasicBlockWalker::WalkNodeAndSuccessors
(
   llvm::MachineBasicBlock * node
)
{
   llvm::MachineBasicBlock * entryNode;

   // Blocks unreached from entry may have a single predecessor (loop back edge) under Od.  Allow these as
   // entries.  We trust that callers of this function are correct when they give us an entry.

   this->PushEntry(node);

   while (this->EntryNodeStackDepth > 0)
   {
      entryNode = this->PopEntry();

      // Tell the visitor that we're starting a new extended block

      Graphs::ExtendedBasicBlockNodeVisitor * ebbNodeVisitor
         = static_cast<Graphs::ExtendedBasicBlockNodeVisitor*>(this->nodeVisitor);

      ebbNodeVisitor->PreVisitEntry(entryNode);

      // The graph should not have changed out from under us.

      assert(this->graph->VersionNumber == this->graphVersionNumber);

      // Walk the EBB
      this->IterativeWalk(entryNode, GraphWalkerLink::Successor);

      ebbNodeVisitor->PostVisitEntry(entryNode);
   }
}

void
ExtendedBasicBlockWalker::PushEntry(llvm::MachineBasicBlock * node)
{
   this->QueuedEntryBitVector->set(node->getNumber());

   (*(this->EntryNodeStack))[this->EntryNodeStackDepth] = node;

   assert(this->EntryNodeStackDepth <= this->graph->MaxNodeId);
   this->EntryNodeStackDepth++;
}

llvm::MachineBasicBlock *
ExtendedBasicBlockWalker::PopEntry()
{
   // Should not pop an empty stack.
   assert(this->EntryNodeStackDepth != 0);

   llvm::MachineBasicBlock * node = (*(this->EntryNodeStack))[this->EntryNodeStackDepth - 1];
   this->EntryNodeStackDepth--;

   // Popped node should have been in the queued set.
   assert(this->QueuedEntryBitVector->test(node->getNumber()));

   this->QueuedEntryBitVector->reset(node->getNumber());

   return node;
}

void
ExtendedBasicBlockWalker::AllocateTemporaryData()
{
   assert(this->EntryNodeStack == nullptr);

   NodeWalker::AllocateTemporaryData();

   // Note that node IDs are 1 based.

   const unsigned requiredLength = this->graph->MaxNodeId + 1;

   this->QueuedEntryBitVector = new llvm::BitVector(requiredLength);
   this->EntryNodeStack = new Graphs::MachineBasicBlockVector(requiredLength);
   this->EntryNodeStackDepth = 0;
}

void
ExtendedBasicBlockWalker::FreeTemporaryData()
{
   if (this->QueuedEntryBitVector != nullptr) {
      delete this->QueuedEntryBitVector;
      this->QueuedEntryBitVector = nullptr;
   }

   NodeWalker::FreeTemporaryData();
   delete this->EntryNodeStack;
   this->EntryNodeStack = nullptr;
}

void
ExtendedBasicBlockWalker::IterativeWalk(llvm::MachineBasicBlock * node, Graphs::GraphWalkerLink link)
{
   NodeWalker::StackNode * stackNode;

   // We only support forward if EBBs support regions (has exits) then
   // we can support walking predecessors.
   assert(link == GraphWalkerLink::Successor);

   assert(this->NodeStackDepth == 0);
   assert(!this->VisitedBitVector->test(node->getNumber()));

   // Queue this block and begin the walk.
   this->PushNode(node, link);

   while (this->NodeStackDepth > 0)
   {
      stackNode = &((*this->NodeStack)[this->NodeStackDepth - 1]);
      node = stackNode->Node;

      // The graph should not have changed out from under us.
      assert(this->graph->VersionNumber == this->graphVersionNumber);

      // First time seeing this node?
      if (!this->VisitedBitVector->test(node->getNumber())) {
         this->VisitedBitVector->set(node->getNumber());

         this->nodeVisitor->PreVisit(node);
      }

      // Recurse on block successors.

      if (!stackNode->Edge.empty()) {
         NodeWalker::Xcessor edge = (stackNode->Edge).begin;

         while (edge != stackNode->Edge.end)
         {
            assert(link == GraphWalkerLink::Successor);

            llvm::MachineBasicBlock * successorNode = (*edge);

            if (ExtendedBasicBlockWalker::IsEntry(successorNode) /*EH: || edge->AsFlowEdge->IsException*/) {

               if (!this->QueuedEntryBitVector->test(successorNode->getNumber())
                  && !this->VisitedBitVector->test(successorNode->getNumber()) ) {
                  assert(!this->QueuedBitVector->test(successorNode->getNumber()));

                  // Push node for subsequent traversal
                  this->PushEntry(successorNode);
               }

               // Fall out to next successor. We've already seen this entry.
            } else if (!this->QueuedBitVector->test(successorNode->getNumber())) {
               assert(!ExtendedBasicBlockWalker::IsEntry(successorNode));

               this->PushNode(successorNode, link);
               stackNode->Edge.begin = edge + 1;
               break;
            }

            ++edge;
         }

         if (edge != stackNode->Edge.end) {
            // More succ edges to process before finishing this node.
            continue;
         }

         this->nodeVisitor->PostVisit(node);
      }

      // The graph should not have changed out from under us.
      assert(this->graph->VersionNumber == this->graphVersionNumber);

      this->NodeStackDepth--;
   }
}

} // namespace Graphs
} // namespace Tiled
