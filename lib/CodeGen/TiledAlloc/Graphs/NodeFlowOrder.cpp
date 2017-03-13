//===-- Graphs/NodeFlowOrder.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "NodeWalker.h"
#include "NodeFlowOrder.h"
#include "NumberNodeVisitor.h"

namespace Tiled
{
namespace Graphs
{

NodeFlowOrder::NodeFlowOrder() : Graph(nullptr),
      Order(Order::IllegalSentinel), nodeVisitor(nullptr),
      nodeWalker(nullptr), nodeIdToPositionVector(nullptr),
      positionToNodeIdVector(nullptr), graphVersionNumber(0)
{}

NodeFlowOrder*
NodeFlowOrder::New()
{
   NodeFlowOrder* nodeOrder = new NodeFlowOrder();
   nodeOrder->Order = Order::IllegalSentinel;

   return nodeOrder;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Set the order to reflect a particular ordering of graph nodes.
//
// Arguments:
//
//    graph - graph of interest
//    order - ordering to create
//
// Remarks:
//
//    For most orderings the node order makes use of the graph
//    visitors associated with the graph. For RegionReversePostOrder,
//    however, the a new visitor is allocated here and walked over the
//    graph to record the traversal order.
//
//    If the graph has a start and end node, and there are nodes
//    unreachable from start, they will be given positions past end in
//    the forward graph orderings. Likewise, in reverse graph
//    orderings, nodes that cannot reach end, will be given positions
//    past start in reverse graph orderings.
//
//-----------------------------------------------------------------------------

void
NodeFlowOrder::Build
(
   FlowGraph *           graph,
   Tiled::Graphs::Order  order
      // Orders used by dataflow/RA:
      //   ReversePostOrder, PostOrder, ExtendedBasicBlockReversePostOrder
)
{
   assert(order != Order::IllegalSentinel);

   // Try and avoid doing work -- see if this order is valid and
   // already built according to the requested order.
   if ((this->Graph == graph) && (this->Order == order)
      && (this->graphVersionNumber == graph->VersionNumber)) {
      // already done
      return;
   }

   const unsigned idCount = graph->MaxNodeId + 1;  //graph->machineFunction->getNumBlockIDs()
   const unsigned nodeCount = graph->NodeCount + 1;  //graph->machineFunction->size()

   assert(idCount < nodeCount);

   // Setup map from node Id to position
   if (this->nodeIdToPositionVector == nullptr) {
      this->nodeIdToPositionVector = new std::vector<unsigned>(idCount, 0);
   } else {
      this->nodeIdToPositionVector->clear();
      this->nodeIdToPositionVector->resize(idCount, 0);
   }
   assert(this->nodeIdToPositionVector->size() == idCount);

   // Setup map from position to node Id
   if (this->positionToNodeIdVector == nullptr) {
      this->positionToNodeIdVector = new std::vector<unsigned>(nodeCount, 0);
   } else {
      this->positionToNodeIdVector->clear();
      this->positionToNodeIdVector->resize(nodeCount, 0);
   }
   assert(this->positionToNodeIdVector->size() == nodeCount);

   // Cache other stuff.

   this->graphVersionNumber = graph->VersionNumber;
   this->Graph = graph;
   this->Order = order;

   // Build up the desired numbering.

   switch (this->Order)
   {
      default:
         assert(0);

      case Graphs::Order::PostOrder:
      case Graphs::Order::ReversePostOrder:
      {
         if (this->nodeVisitor == nullptr) {
            this->nodeVisitor = Graphs::NumberNodeVisitor::New();
            graph->numberVisitor = this->nodeVisitor;
            this->nodeVisitor->graph = graph;
         }

         this->Graph->BuildDepthFirstNumbers();

         break;
      }

      case Graphs::Order::ExtendedBasicBlockReversePostOrder:
      {
         if (this->nodeVisitor == nullptr) {
            this->nodeVisitor = Graphs::ExtendedBasicBlockNodeVisitor::New();
            this->nodeVisitor->graph = graph;
         }

         if (this->nodeWalker == nullptr) {
            Graphs::ExtendedBasicBlockWalker * extendedBasicBlockWalker =
               Graphs::ExtendedBasicBlockWalker::New();

            this->nodeWalker = extendedBasicBlockWalker;
         }

         this->nodeWalker->graph = graph;

         this->nodeWalker->WalkFrom(this->nodeVisitor, graph->StartNode);

         break;
      }
   }

   if (this->Order == Graphs::Order::ExtendedBasicBlockReversePostOrder) {

      Graphs::ExtendedBasicBlockNodeVisitor * ebbNodeVisitor
         = static_cast<Graphs::ExtendedBasicBlockNodeVisitor*>(this->nodeVisitor);
      Graphs::NodeFlowOrder * ebbReversePostOrder = ebbNodeVisitor->ReversePostOrder;
      unsigned         position = 1;
      unsigned         count = 1;
      unsigned         remainingInEbb = 0;
      unsigned         backTrackPosition = 0;
      unsigned         nodeCount = ebbReversePostOrder->NodeCount();
      unsigned         currentEbbId = 0;

      // Count is 1 based so less than or equals.

      while (count <= nodeCount && position <= nodeCount)
      {
         llvm::MachineBasicBlock * node = ebbReversePostOrder->Node(count);

         //  RPO is near the right order, we just need to shuffle the
         //  order to ensure that all EBB blocks are together.

         assert((position > 0) && (position < this->positionToNodeIdVector->size()));
         assert((node->getNumber() >= 0) && (node->getNumber() < this->nodeIdToPositionVector->size()));

         unsigned nodeEbbId = ebbNodeVisitor->ExtendedBasicBlockEntryId(node);

         if ((nodeEbbId != currentEbbId) && ((*this->nodeIdToPositionVector)[node->getNumber()] == 0)) {
            //Assert(Graphs::ExtendedBasicBlockWalker::IsEntry(node) || !node->HasPathFromStart);

            if (remainingInEbb > 0) {
               if (backTrackPosition == 0) {
                  backTrackPosition = ebbReversePostOrder->Position(node);
               }
            } else {
               currentEbbId = nodeEbbId;
               remainingInEbb = ebbNodeVisitor->ExtendedBasicBlockCount(nodeEbbId);
            }
         }

         assert(currentEbbId != 0);

         if (currentEbbId == nodeEbbId) {
            // layout at current position

            (*this->nodeIdToPositionVector)[node->getNumber()] = position;
            (*this->positionToNodeIdVector)[position] = node->getNumber();

            position++;
            remainingInEbb--;
         }

         if ((remainingInEbb == 0) && (backTrackPosition != 0)) {
            count = backTrackPosition;
            backTrackPosition = 0;
         } else {
            count++;
         }
      }

   } else {
      // Fill in the mapping arrays.
      llvm::MachineFunction* machineFunction = this->Graph->machineFunction;

      llvm::MachineFunction::iterator node;
      for (node = machineFunction->begin(); node != machineFunction->end(); ++node)
      {
         unsigned position = this->InitialPosition(&*node);

         assert((position > 0) && (position < this->positionToNodeIdVector->size()));
         assert((node->getNumber() >= 0) && (unsigned(node->getNumber()) < this->nodeIdToPositionVector->size()));

         (*this->nodeIdToPositionVector)[node->getNumber()] = position;
         (*this->positionToNodeIdVector)[position] = node->getNumber();
      }
   }

}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Return the Id of the node at a given position in the order.
//
// Arguments:
//
//    position - position of interest
//
// Remarks:
//
//    The first node in the order is at position 1, and subsequent
//    nodes have higher position numbers.
//
// Returns:
//
//    Id of the node in this position.
//
//-----------------------------------------------------------------------------

unsigned
NodeFlowOrder::NodeId
(
   unsigned position
)
{
   assert((position > 0) && (position < this->positionToNodeIdVector->size()));
   assert(this->graphVersionNumber == this->Graph->VersionNumber);
   assert(this->Order != Graphs::Order::IllegalSentinel);

   return (*this->positionToNodeIdVector)[position];
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Return the node at this position in the order.
//
// Arguments:
//
//    position - position of interest
//
// Remarks:
//
//    The first node in the order is at position 1, and
//    subsequent nodes have higher numbers.
//
//    Position and Node are inverses. For a given Node x, position y,
//    and Order z,
//
//       z->Node(z->Position(x)) == x
//       z->Position(z->Node(y)) == y
//
// Returns:
//
//    Node at this position in the order.
//
//-----------------------------------------------------------------------------

llvm::MachineBasicBlock*
NodeFlowOrder::Node
(
   unsigned position
)
{
   assert((position > 0) && (position < this->positionToNodeIdVector->size()));
   assert(this->graphVersionNumber == this->Graph->VersionNumber);
   assert(this->Order != Graphs::Order::IllegalSentinel);

   unsigned nodeId = (*this->positionToNodeIdVector)[position];

   return this->Graph->machineFunction->getBlockNumbered(nodeId);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Return the position of the node with this Id in the order.
//
// Arguments:
//
//    nodeId - node Id of interest
//
// Remarks:
//
//    The first node in the order is at position 1, and
//    subsequent nodes have higher numbers.
//
//    Position and NodeId are inverses. For a given Id I, position y,
//    and Order z,
//
//       z->NodeId(z->Position(i)) == i
//       z->Position(z->NodeId(y)) == y
//
// Returns:
//
//    Position of the node with this Id in the order.
//
//-----------------------------------------------------------------------------

unsigned
NodeFlowOrder::Position
(
   unsigned nodeId
)
{
   assert((nodeId >= 0) && (nodeId < this->nodeIdToPositionVector->size()));
   assert(this->graphVersionNumber == this->Graph->VersionNumber);
   assert(this->Order != Graphs::Order::IllegalSentinel);

   return (*this->nodeIdToPositionVector)[nodeId];
}

unsigned
NodeFlowOrder::Position
(
   llvm::MachineBasicBlock* node
)
{
   assert(node->getNumber() >= 0);

   return this->Position(node->getNumber());
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Compute the initial position of this node in the order.
//
// Arguments:
//
//    node - node of interest
//
// Remarks:
//
//    The first node in the order is at position 1, and subsequent
//    nodes have higher numbers.
//
//    Position and Node are inverses. For a given Node x, position y,
//    and Order z,
//
//       z->Node(z->Position(x)) == x
//       z->Position(z->Node(y)) == y
//
//    This routine is used internally to build the mapping arrays.
//    Once the order is built, clients should use Position() instead.
//
// Returns:
//
//    Initial position of this node in the order.
//
//-----------------------------------------------------------------------------

unsigned
NodeFlowOrder::InitialPosition
(
   llvm::MachineBasicBlock* node
)
{
   assert(node != nullptr);
   assert(this->graphVersionNumber == this->Graph->VersionNumber);
   assert(this->Order != Graphs::Order::IllegalSentinel);

   // The inverse mapping is maintained by the associated
   // numbering object.

   switch (this->Order)
   {
      default:
         assert(0);  // Unreached("");

      case Order::PostOrder:
         assert(this->Graph->AreDepthFirstNumbersValid());

         return this->nodeVisitor->getPostNumber(node);

      case Order::ReversePostOrder:
      {
         assert(this->Graph->AreDepthFirstNumbersValid());
         assert(this->Graph->StartNode != nullptr);

         // The maximum postnumber of any reachable node is
         // the postnumber of start.

         unsigned maxReachNum = this->Graph->getPostNumber(Graph->StartNode);
         unsigned nodeNumber  = this->nodeVisitor->getPostNumber(node);

         if (nodeNumber > maxReachNum) {
            // Node was unreachable.
            //TODO: Assert(!node->HasPathFromStart);
            unsigned maxGraphNum = this->Graph->getMaxPrePostNumber();

            return maxGraphNum - (nodeNumber - maxReachNum) + 1;
         } else {
            // Node was reachable.
            return maxReachNum - nodeNumber + 1;
         }
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Move the start nodes and end node of the graph so they are the first
//    and last nodes of the order.
//
// Remarks:
//
//    In a forward order such as preorder or reverse postorder, the start
//    node will be moved to the beginning of the order and the end node
//    will be moved to the end.  For a backwards order - such as postorder
//    or reverse preorder - the start node will be moved to the end of the
//    order and the end node will be moved to the beginning.  The map from
//    node id to order and the map from order to node id will be updated.
//
//-----------------------------------------------------------------------------

void
NodeFlowOrder::CanonicalizeStartAndEndPositions()
{
   if (this->Order == Graphs::Order::ExtendedBasicBlockReversePostOrder) {
      // Handling this is done inside the EBB order.
      return;
   }

   unsigned position;
   unsigned startPosition;
   unsigned endPosition;
   unsigned adjustedStartPosition;
   unsigned adjustedEndPosition;
   unsigned maxPosition = this->positionToNodeIdVector->size() - 1;

   // Determine where the start and the end nodes are now

   startPosition = this->InitialPosition(this->Graph->StartNode);
   endPosition = this->InitialPosition(this->Graph->EndNode);

   if (startPosition < endPosition) {
      adjustedStartPosition = 1;
      adjustedEndPosition = maxPosition;
   } else {
      unsigned temporary;

      temporary = startPosition;
      startPosition = endPosition;
      endPosition = temporary;

      adjustedStartPosition = maxPosition;
      adjustedEndPosition = 1;
   }

   if (((*this->positionToNodeIdVector)[adjustedStartPosition] == this->Graph->StartNode->getNumber())
      && ((*this->positionToNodeIdVector)[adjustedEndPosition] == this->Graph->EndNode->getNumber())) {
      // Start and end are already in the correct positions
      return;
   }

   // Shift nodes before the start one space right
   for (position = startPosition; position > 1; position--)
   {
      (*this->positionToNodeIdVector)[position] = (*this->positionToNodeIdVector)[position - 1];
   }

   // Shift nodes after the end one space left
   for (position = endPosition; position < maxPosition; position++)
   {
      (*this->positionToNodeIdVector)[position] = (*this->positionToNodeIdVector)[position + 1];
   }

   // Place start and end in their adjusted positions
   (*this->positionToNodeIdVector)[adjustedStartPosition] = this->Graph->StartNode->getNumber();
   (*this->positionToNodeIdVector)[adjustedEndPosition] = this->Graph->EndNode->getNumber();

   // Recalculate the reverse mapping array
   for (position = 1; position <= maxPosition; position++)
   {
      unsigned nodeId = (*this->positionToNodeIdVector)[position];

      (*this->nodeIdToPositionVector)[nodeId] = position;
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Return the number of nodes in the order.
//
// Remarks:
//
//    Equals the node count of the corresponding graph, and is also
//    the maximum value that can be returned by calls to Position.
//
// Returns:
//
//    Number of nodes in the order.
//
//-----------------------------------------------------------------------------

unsigned
NodeFlowOrder::NodeCount()
{
   assert(this->IsValid());

   return (this->positionToNodeIdVector->size() - 1);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Check validity of the order.
//
// Remarks:
//
//    A node order becomes invalid when the graph is modified
//    by adding, deleting, or altering edges or nodes.
//
// Returns:
//
//    True if the order is still valid.
//
//-----------------------------------------------------------------------------

bool
NodeFlowOrder::IsValid()
{
   return ((this->Graph != nullptr)
      && (this->graphVersionNumber == this->Graph->VersionNumber));
}

void
NodeFlowOrder::Delete() {
   if (this->nodeVisitor != nullptr) {
      this->nodeVisitor->Delete();
   }

   if (this->nodeWalker != nullptr) {
      this->nodeWalker->Delete();
   }

   this->Graph = nullptr;
   //TODO: check if objects/vectors are shared, if not delete!
   this->nodeIdToPositionVector = nullptr;
   this->positionToNodeIdVector = nullptr;
   this->graphVersionNumber = 0;
   this->nodeVisitor = nullptr;
   this->nodeWalker = nullptr;
}

} // namespace Graphs
} // namespace Tiled
