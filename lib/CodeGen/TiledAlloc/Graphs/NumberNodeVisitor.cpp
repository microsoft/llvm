//===-- Graphs/NumberNodeVisitor.cpp ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "NumberNodeVisitor.h"
#include "NodeFlowOrder.h"
#include "../Graphs/Graph.h"

namespace Tiled
{
namespace Graphs
{

NumberNodeVisitor*
NumberNodeVisitor::New()
{
   return new NumberNodeVisitor();
}

NumberNodeVisitor::NumberNodeVisitor() :
   graphVersionNumber(0), postDepthFirstNumber(0),
   postNumberVector(nullptr), preDepthFirstNumber(0),
   preNumberVector(nullptr) { }

void
NumberNodeVisitor::Delete()
{
   //TBD: check if objects/vectors are shared, if not delete!
   this->graph = nullptr;

   delete this->preNumberVector;
   this->preNumberVector = nullptr;
   delete this->postNumberVector;
   this->postNumberVector = nullptr;
   this->graphVersionNumber = 0;
}

void
NumberNodeVisitor::Begin
(
   FlowGraph* graphToVisit
)
{
   const unsigned nodeCount = graphToVisit->MaxNodeId + 1;

   if (this->preNumberVector == nullptr) {
      this->postNumberVector = new std::vector<unsigned>(nodeCount, 0);
      this->preNumberVector = new std::vector<unsigned>(nodeCount, 0);
   } else {
      this->postNumberVector->clear();
      this->postNumberVector->resize(nodeCount, 0);
      this->preNumberVector->clear();
      this->preNumberVector->resize(nodeCount, 0);
   }

   this->preDepthFirstNumber = 1;
   this->postDepthFirstNumber = 1;
   this->graphVersionNumber = graphToVisit->VersionNumber;
   this->graph = graphToVisit;
}

void
NumberNodeVisitor::End()
{
   assert(this->IsValid());
   assert(this->preDepthFirstNumber == this->postDepthFirstNumber);
}

void
NumberNodeVisitor::PreVisit
(
   llvm::MachineBasicBlock* node
)
{
   assert(node->getNumber() >= 0);
   (*this->preNumberVector)[node->getNumber()] = this->preDepthFirstNumber;
   ++this->preDepthFirstNumber;
}

void
NumberNodeVisitor::PostVisit
(
   llvm::MachineBasicBlock* node
)
{
   assert(node->getNumber() >= 0);
   (*this->postNumberVector)[node->getNumber()] = this->postDepthFirstNumber;
   ++this->postDepthFirstNumber;
}

unsigned
NumberNodeVisitor::getPostNumber
(
   llvm::MachineBasicBlock* node
)
{
   assert(this->IsValid() && (node->getNumber() >= 0));
   return (*this->postNumberVector)[node->getNumber()];
}

unsigned
NumberNodeVisitor::getMaxNumber()
{
   assert(this->IsValid());
   assert(this->preDepthFirstNumber == this->postDepthFirstNumber);
   return (this->preDepthFirstNumber - 1);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Notification that the graph contains a node that could not be
//    reached from the first node in the walk.
//
// Arguments:
//
//    node - unreached node
//
//------------------------------------------------------------------------------

void
NumberNodeVisitor::UnreachedNode
(
   llvm::MachineBasicBlock* node
)
{
   // nothing
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Visitor for numbering nodes based on extended basic blocks.
//
//------------------------------------------------------------------------------

void
ExtendedBasicBlockNodeVisitor::Begin
(
   FlowGraph* graphToVisit
)
{
   NumberNodeVisitor::Begin(graphToVisit);

   const unsigned nodeCount = graphToVisit->MaxNodeId + 1;

   if (this->ebbEntryIdVector == nullptr) {
      this->ebbEntryIdVector = new std::vector<unsigned>(nodeCount);
   } else {
      this->ebbEntryIdVector->clear();
   }

   this->ebbEntryIdVector->resize(nodeCount);

   if (this->ebbCountVector == nullptr) {
      this->ebbCountVector = new std::vector<unsigned>(nodeCount + 1);
   } else {
      this->ebbCountVector->clear();
   }

   this->ebbCountVector->resize(nodeCount + 1, 0);

   this->ebbEntryId = 0; // Invalid

   // Calculate standard RPO

   Tiled::Graphs::NodeFlowOrder * reversePostOrder = Tiled::Graphs::NodeFlowOrder::New();
   reversePostOrder->Build(graphToVisit, Graphs::Order::ReversePostOrder);
   reversePostOrder->CanonicalizeStartAndEndPositions();

   this->ReversePostOrder = reversePostOrder;
}

void
ExtendedBasicBlockNodeVisitor::End()
{
   NumberNodeVisitor::End();
}

unsigned
ExtendedBasicBlockNodeVisitor::ExtendedBasicBlockEntryId
(
   unsigned nodeId
)
{
   assert(nodeId < this->ebbEntryIdVector->size());
   assert(this->graphVersionNumber == this->graph->VersionNumber);

   return (*this->ebbEntryIdVector)[nodeId];
}

unsigned
ExtendedBasicBlockNodeVisitor::ExtendedBasicBlockCount
(
   unsigned ebbId
)
{
   assert(ebbId < this->ebbCountVector->size());
   assert(this->graphVersionNumber == this->graph->VersionNumber);

   return (*this->ebbCountVector)[ebbId];
}

void
ExtendedBasicBlockNodeVisitor::PreVisit
(
   llvm::MachineBasicBlock* node
)
{
   assert(node->getNumber() >= 0);

   (*this->ebbEntryIdVector)[node->getNumber()] = this->ebbEntryId;
   this->ebbCount++;
}

void
ExtendedBasicBlockNodeVisitor::PostVisit
(
   llvm::MachineBasicBlock* node
)
{
   ; // do nothing for now.
}

unsigned
ExtendedBasicBlockNodeVisitor::PreNumber
(
   unsigned nodeId
)
{
   return (*this->preNumberVector)[nodeId];
}

unsigned
ExtendedBasicBlockNodeVisitor::PostNumber
(
   unsigned nodeId
)
{
   return (*this->postNumberVector)[nodeId];
}

void
ExtendedBasicBlockNodeVisitor::PreVisitEntry
(
   llvm::MachineBasicBlock* node
)
{
   assert(unsigned(node->getNumber()) < this->ebbEntryIdVector->size());
   assert(this->graphVersionNumber == this->graph->VersionNumber);

   // ebbCount is incremented in node PreVisit.

   this->ebbCount = 0;
   this->ebbEntryId++;
}

void
ExtendedBasicBlockNodeVisitor::PostVisitEntry
(
   llvm::MachineBasicBlock* node
)
{
   assert(unsigned(node->getNumber()) < this->ebbEntryIdVector->size());
   assert(this->graphVersionNumber == this->graph->VersionNumber);

   unsigned ebbEntryId = (*this->ebbEntryIdVector)[node->getNumber()];
   (*this->ebbCountVector)[ebbEntryId] = this->ebbCount;
}

void
ExtendedBasicBlockNodeVisitor::Delete()
{
   NumberNodeVisitor::Delete();

   delete this->ebbEntryIdVector;
   this->ebbEntryIdVector = nullptr;
}

ExtendedBasicBlockNodeVisitor *
ExtendedBasicBlockNodeVisitor::New()
{
   ExtendedBasicBlockNodeVisitor * extendedBasicBlockNodeVisitor =
      new ExtendedBasicBlockNodeVisitor();

   return extendedBasicBlockNodeVisitor;
}

} // namespace Graphs
} // namespace Tiled


