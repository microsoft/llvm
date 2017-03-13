//===-- Graphs/NumberNodeVisitor.h ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_NUMBERNODEVISITOR_H
#define TILED_GRAPHS_NUMBERNODEVISITOR_H

#include "Graph.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

namespace Tiled
{
namespace Graphs
{

class NodeFlowOrder;
class FlowGraph;

//------------------------------------------------------------------------------
//
// Description:
//
//    This class is a graph node visitor that numbers nodes.
//
// Remarks:
//
//    The node numbers are stored in arrays retained by the visitor.
//    The visitor will also record the graph version number that was
//    available when the numbering was made, and will assert if
//    queried once the graph has changed.
//
//    In other words, querying the prenumber or postnumber of either
//    n1 or n2 after the following sequence is invalid:
//
//       Graph * g  = Graph::New();
//       Node *  n1 = g->CreateNode();
//
//       g->BuildDepthFirstNumbers();
//
//       Node *  n2 = g->CreateNode();
//
//------------------------------------------------------------------------------

class NumberNodeVisitor
{
public:

   NumberNodeVisitor();

   virtual void Delete();

   static NumberNodeVisitor * New();

 public:

   virtual void
   Begin
   (
      FlowGraph* graph
   );

   void Drop();

   virtual void End();

   void Invalidate();

   unsigned
   getPostNumber
   (
      llvm::MachineBasicBlock* node
   );

   virtual void
   PostVisit
   (
      llvm::MachineBasicBlock* node
   );

   unsigned
   PreNumber
   (
      llvm::MachineBasicBlock* node
   );

   virtual void
   PreVisit
   (
      llvm::MachineBasicBlock* node
   );

   void
   UnreachedNode
   (
      llvm::MachineBasicBlock* node
   );

 public:

   bool IsValid()
   {
      return (this->graphVersionNumber == this->graph->VersionNumber);
   }

   unsigned getMaxNumber();

 public:

   Graphs::FlowGraph*          graph;

   std::vector<unsigned> * getPostNumberVector() { return postNumberVector; }

 protected:

   unsigned                    graphVersionNumber;
   unsigned                    postDepthFirstNumber;
   std::vector<unsigned>*      postNumberVector;
   unsigned                    preDepthFirstNumber;
   std::vector<unsigned>*      preNumberVector;
};

//------------------------------------------------------------------------------
//
// Description:
//
//    This class is a graph node visitor that numbers nodes with in an
//    extended basic block.
//
// Remarks:
//
//    The node numbers are stored in arrays retained by the visitor.
//    The visitor will also record the graph version number that was
//    available when the numbering was made, and will assert if
//    queried once the graph has changed.
//
//------------------------------------------------------------------------------

class ExtendedBasicBlockNodeVisitor : public Graphs::NumberNodeVisitor
{
public:

   void Delete() override;

   static Graphs::ExtendedBasicBlockNodeVisitor * New();

public:

   void
   Begin
   (
      FlowGraph * graph
   ) override;

   void End() override;

   unsigned
   ExtendedBasicBlockCount
   (
      unsigned ebbId
   );

   unsigned
   ExtendedBasicBlockEntryId
   (
      llvm::MachineBasicBlock * block
   )
   {
      return this->ExtendedBasicBlockEntryId(block->getNumber());
   }

   unsigned
   ExtendedBasicBlockEntryId
   (
      unsigned nodeId
   );

   unsigned
   PostNumber
   (
      unsigned nodeId
   );

   void
   PostVisit
   (
      llvm::MachineBasicBlock * node
   ) override;

   void
   PostVisitEntry
   (
      llvm::MachineBasicBlock * node
   );

   unsigned
   PreNumber
   (
      unsigned nodeId
   );

   void
   PreVisit
   (
      llvm::MachineBasicBlock * node
   ) override;

   void
   PreVisitEntry
   (
      llvm::MachineBasicBlock * node
   );

public:
  
   Tiled::Graphs::NodeFlowOrder * ReversePostOrder;

private:

   unsigned                ebbCount;
   unsigned                ebbEntryId;
   std::vector<unsigned> * ebbCountVector;
   std::vector<unsigned> * ebbEntryIdVector;
};

} // namespace Graphs
} // namespace Tiled

#endif // end TILED_GRAPHS_NUMBERNODEVISITOR_H

