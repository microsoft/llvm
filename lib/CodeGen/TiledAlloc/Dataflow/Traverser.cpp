//===-- Dataflow/Traverser.h ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This namespace implements a generic, flexible data flow package.
//
//===----------------------------------------------------------------------===//

#include "Traverser.h"

#define DEBUG_TYPE "tiled-traverser"

namespace Tiled
{
namespace Dataflow
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Delete this Data object.
//
//-----------------------------------------------------------------------------

void
Data::Delete()
{
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Determine if this has the same pre-condition as that.
//
// Arguments:
//
//    blockData - Block data to be processed
//
// Remarks:
//
//    This API is used to determine when a block does not need to be
//    evaluated.  Exact equality may not be required by the client algorithm.
//
//    Since the API is not needed for non-iterative walks, we provided a
//    default implementation which asserts.
//
// Returns:
//
//    true if equal, false otherwise.
//
//-----------------------------------------------------------------------------

bool
Data::SamePrecondition
(
   Dataflow::Data * blockData
)
{
   assert(0 && "Implement SamePrecondition for iterative analysis");

   return false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Determine if this has the same post-condition as that.
//
// Arguments:
//
//    blockData - Block data to be processed
//
// Remarks:
//
//    This API is used to determine when a block has not changed and thus its
//    dependents don't need to be evaluated.  Exact equality may not be
//    required by the client algorithm.
//
//    Since the API is not needed for non-iterative walks, we provided a
//    default implementation which asserts.
//
// Returns:
//
//    true if equal, false otherwise.
//
//-----------------------------------------------------------------------------

bool
Data::SamePostCondition
(
   Dataflow::Data * blockData
)
{
   assert(0 && "Implement SamePostCondition for iterative analysis");

   return false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize this walker from scratch.
//
// Arguments:
//
//    direction - Direction of traversal
//    functionUnit - Function to process
//
// Remarks:
//
//    The data elements are directly copied (pointers.)  It is assumed that
//    the specified walker will not be used again.
//
//-----------------------------------------------------------------------------

void
Walker::Initialize
(
   Dataflow::Direction  direction,
   Graphs::FlowGraph *  functionUnit
)
{
   assert(functionUnit != nullptr &&
      "FlowGraph must be built in order to run dataflow.");
   assert(functionUnit->machineFunction != nullptr &&
      "Walker::Initialize requires a valid FunctionUnit");
   assert(this->BlockDataArray == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->InitializeData == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->TemporaryData == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->InitialQueuedBitVector == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->QueuedBitVector == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->NextQueuedBitVector == nullptr &&
      "Initialize should be called only once per Walker object");

   // Allocate array of Data * for each block. +1 because MaxNodeId is the
   // highest Id in use, +1 for InitializeData 
   // (we'll use [0] for TemporaryData, and [1] for InitializeData)

   this->FunctionUnit = this->FlowGraph = functionUnit;
   this->vrInfo = functionUnit->vrInfo;
   this->MaxBlockId = this->FlowGraph->MaxNodeId;
  
   // ISSUE-TODO-aasmith-2015/11/07: Remove RTTI which is not allowed in LLVM
#if defined(_MSC_VER)
   DEBUG(llvm::dbgs() << "Before allocateData " << typeid(this).name() << "\n");
#else
   DEBUG(llvm::dbgs() << "Before allocateData\n");
#endif
  
   this->AllocateData((this->MaxBlockId + 1) + 1 + 1);
   DEBUG(llvm::dbgs() << "After allocateData\n");
   this->InitializeData = (*this->BlockDataArray)[1];
   this->TemporaryData = (*this->BlockDataArray)[0];

   llvm::MachineFunction* machineFunction = functionUnit->machineFunction;
   llvm::MachineFunction::iterator mbb;
   for (mbb = machineFunction->begin(); mbb != machineFunction->end(); ++mbb)
   {
      assert(mbb->getNumber() >= 0 && unsigned(mbb->getNumber()) <= this->MaxBlockId);

      llvm::MachineBasicBlock* block = &(*mbb);
      Dataflow::Data * data = this->GetBlockData(block);
      assert(data != nullptr);
      data->Block = block;
      assert(!data->MustEvaluate);
   }


   this->InitialQueuedBitVector = new llvm::SparseBitVector<>();
   this->QueuedBitVector = new llvm::SparseBitVector<>();
   this->NextQueuedBitVector = new llvm::SparseBitVector<>();

   this->Direction = direction;

   this->NodeOrder = Graphs::NodeFlowOrder::New();

   // TODO: here we need to set visitor of this->NodeOrder
   //this->NodeOrder->nodeVisitor = functionUnit->numberVisitor;

   // Set up node ordering based on direction.
   this->SetupNodeOrder();

   this->MemoryIsOwned = true;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform static initialization of the Walker class.
//
// Remarks:
//
//    We create a debug flag.
//
//-----------------------------------------------------------------------------

void
Walker::StaticInitialize()
{
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Setup the node order for the walker.
//
// Remarks:
//
//    Node order is set to reverse postorder for forward traversals,
//    and reverse graph reverse postorder for backward traversals.
//
//-----------------------------------------------------------------------------

void
Walker::SetupNodeOrder()
{
   switch (this->Direction)
   {
      default:
         assert(0 && "Unexpected traversal direction");

      case Dataflow::Direction::Forward:

         this->NodeOrder->Build(this->FlowGraph, Graphs::Order::ReversePostOrder);
         break;

      case Dataflow::Direction::Backward:

         this->NodeOrder->Build(this->FlowGraph, Graphs::Order::PostOrder);
         break;
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize this walker from the supplied one.
//
// Arguments:
//
//    direction - Direction of traversal
//    copyWalker - Walker to copy data from
//
// Remarks:
//
//    The data elements are directly copied (pointers.)  It is assumed that
//    the specified walker will not be used again.
//
//-----------------------------------------------------------------------------

void
Walker::Initialize
(
   Dataflow::Direction direction,
   Walker *            copyWalker
)
{
   assert(copyWalker != nullptr &&
      "Walker::Initialize requires a valid Walker");
   assert(this->BlockDataArray == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->InitializeData == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->TemporaryData == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->QueuedBitVector == nullptr &&
      "Initialize should be called only once per Walker object");
   assert(this->NodeOrder == nullptr);

   this->BlockDataArray = copyWalker->BlockDataArray;
   this->InitializeData = copyWalker->InitializeData;
   this->TemporaryData = copyWalker->TemporaryData;

   this->InitialQueuedBitVector = copyWalker->InitialQueuedBitVector;
   this->QueuedBitVector = copyWalker->QueuedBitVector;
   this->NextQueuedBitVector = copyWalker->NextQueuedBitVector;
   this->MaxBlockId = copyWalker->MaxBlockId;

   this->Direction = direction;

   this->MemoryIsOwned = false;

   this->NodeOrder = copyWalker->NodeOrder;
   this->FlowGraph = copyWalker->FlowGraph;

   assert(this->BlockDataArray != nullptr);
   assert(this->InitializeData != nullptr);
   assert(this->TemporaryData != nullptr);
   assert(this->QueuedBitVector != nullptr);
   assert(this->NextQueuedBitVector != nullptr);
   assert(this->NodeOrder != nullptr);

   // Set up node ordering based on direction.

   this->SetupNodeOrder();
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Delete this Walker instance.
//
//-----------------------------------------------------------------------------

void
Walker::Delete()
{
   if (!this->MemoryIsOwned) {
      return;
   }

   for (unsigned i = 0; i <= this->MaxBlockId + 1; i++)
   {
      ((*this->BlockDataArray)[i])->Delete();
   }

   delete BlockDataArray;

   delete this->QueuedBitVector;
   delete this->NextQueuedBitVector;
   this->NodeOrder->Delete();
   //TBD ???:  delete this->NodeOrder;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Allocate the data flow data.
//
// Arguments:
//
//    numberElements - Number of elements to allocate
//
//-----------------------------------------------------------------------------

void
Walker::AllocateData
(
   unsigned numberElements
)
{
   DEBUG(llvm::dbgs() << "Inside allocateData\n");
   assert(0 && "Implement AllocateData if initializing from scratch");
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform the data flow traversal as specified by the parameters.
//
// Arguments:
//
//    traversalKind - Kind of traversal
//    functionUnit - Function to traverse
//
// Remarks:
//
//    Traversal direction was specified to Initialize.
//
//-----------------------------------------------------------------------------

void
Walker::Traverse
(
   Dataflow::TraversalKind traversalKind,
   Graphs::FlowGraph *     functionUnit
)
{
   assert(functionUnit != nullptr && "Walker::Traverse requires a valid FunctionUnit");
   assert(this->BlockDataArray != nullptr && "Called Walker::Initialize before Traverse");
   assert(this->Direction != Dataflow::Direction::IllegalSentinel);
   assert(traversalKind != TraversalKind::IllegalSentinel && "Invalid traversal kind");
   assert(!this->DoNotIterate);

   // Queue all the blocks to seed local analysis.  Ideally we'd just queue
   // entry/exit blocks but the graph could be disjoint.  E.g. a function with
   // an infinite loop that "exits" by calling another function.

   this->InitialQueuedBitVector->clear();

   Graphs::FlowGraph*  flowGraph = functionUnit;
   assert(flowGraph != nullptr);

   llvm::MachineFunction* machineFunction = functionUnit->machineFunction;
   llvm::MachineFunction::iterator mbb;

   for (mbb = machineFunction->begin(); mbb != machineFunction->end(); ++mbb)
   {
      // Force at least one (re)evaluation to guarantee local analysis.

      llvm::MachineBasicBlock* block = &(*mbb);
      this->GetBlockData(block)->MustEvaluate = true;
      this->AddToInitialQueue(block);
   }

   // Process the queue.

   if (traversalKind == TraversalKind::NonIterative) {
      this->DoNotIterate = true;
   }

   this->Iterate();

   this->DoNotIterate = false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform the data flow traversal as specified by the parameters.
//
// Arguments:
//
//    traversalKind - Kind of traversal
//    blockVector   - block vector to traverse
//
// Remarks:
//
//    Traversal direction was specified to Initialize.
//
//-----------------------------------------------------------------------------

void
Walker::Traverse
(
   Dataflow::TraversalKind    traversalKind,
   MachineBasicBlockVector *  blockVector
)
{
   assert(blockVector != nullptr && "Walker::Traverse requires a valid BlockVector");
   assert(this->BlockDataArray != nullptr && "Called Walker::Initialize before Traverse");
   assert(this->Direction != Dataflow::Direction::IllegalSentinel);
   assert(traversalKind != TraversalKind::IllegalSentinel && "Invalid traversal kind");
   assert(!this->DoNotIterate);

   // Queue all the blocks to seed local analysis.  Ideally we'd just queue
   // entry/exit blocks but the graph could be disjoint.  E.g. a function with
   // an infinite loop that "exits" by calling another function.

   this->InitialQueuedBitVector->clear();

   MachineBasicBlockVector::iterator mbb;
   for (mbb = blockVector->begin(); mbb != blockVector->end(); ++mbb)
   {
      llvm::MachineBasicBlock * block = (*mbb);
      // Force at least one (re)evaluation to guarantee local analysis.

      this->GetBlockData(block)->MustEvaluate = true;
      this->AddToInitialQueue(block);
   }

   // Process the queue.

   if (traversalKind == TraversalKind::NonIterative) {
      this->DoNotIterate = true;
   }

   this->Iterate();

   this->DoNotIterate = false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform dataflow merge prior to doing block analysis
//
// Arguments:
//
//    block - Block to be merged
//
// Remarks:
//
//    Merge the data flow info from the predecessors (successors for a
//       backward walk) to compute the block pre-condition into a temporary
//       data object using Merge.
//
//-----------------------------------------------------------------------------

Dataflow::Data *
Walker::Merge
(
   llvm::MachineBasicBlock * block,
   Dataflow::Data *          blockData
)
{
   Dataflow::Data *     temporaryData = this->TemporaryData;
   Dataflow::MergeFlags flags = MergeFlags::First;

   if (this->Direction == Dataflow::Direction::Forward) {

      // Merge the predecessors.
      llvm::MachineBasicBlock::pred_iterator predecessorEdge;

      for (predecessorEdge = block->pred_begin(); predecessorEdge != block->pred_end(); ++predecessorEdge)
      {
         llvm::MachineBasicBlock * predecessorBlock = (*predecessorEdge);
         Dataflow::Data *          predData = this->GetBlockData(predecessorBlock);

         // Set EH flag bit appropriately.
         flags = static_cast<Dataflow::MergeFlags>(unsigned(flags) & ~(unsigned(MergeFlags::EH)));

         // <EH-flow-related code>
         //if (predecessorEdge->IsException) {
         //   flags = static_cast<Dataflow::MergeFlags>(flags | MergeFlags::EH);
         //}

         // Do the merge and clear the First flag bit.
         temporaryData->Merge(predData, blockData, predecessorEdge, flags);
         flags = static_cast<Dataflow::MergeFlags>(unsigned(flags) & ~(unsigned(MergeFlags::First)));
      }

   } else {
      assert(this->Direction == Dataflow::Direction::Backward);

      // Merge the successors.
      llvm::MachineBasicBlock::succ_iterator successorEdge;

      for (successorEdge = block->succ_begin(); successorEdge != block->succ_end(); ++successorEdge)
      {
         llvm::MachineBasicBlock * successorBlock = (*successorEdge);
         Dataflow::Data *          successorData = this->GetBlockData(successorBlock);

         // Set EH flag bit appropriately.
         flags = static_cast<Dataflow::MergeFlags>(unsigned(flags) & ~(unsigned(MergeFlags::EH)));

         // <place for EH-flow-related code>

         // Do the merge and clear the First flag bit.
         temporaryData->Merge(successorData, blockData, successorEdge, flags);
         flags = static_cast<Dataflow::MergeFlags>(unsigned(flags) & ~(unsigned(MergeFlags::First)));
      }
   }

   if ((unsigned(flags) & unsigned(MergeFlags::First)) == unsigned(MergeFlags::First)) {
      // Nothing was merged above, either because we have no pred/succ or we
      // have none that were computed.
      temporaryData->Merge(this->InitializeData, blockData, Xcessor(), flags);
   }

   return temporaryData;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform local analysis for a single block.
//
// Arguments:
//
//    block - Block to be analysed
//
// Remarks:
//
//    Algorithm:
//
//       1. Merge the data flow info from the predecessors (successors for a
//          backward walk) to compute the block pre-condition into a temporary
//          data object using Merge.
//
//       2. If the block data is marked MustEvaluate, invoke EvaluateBlock, update
//          the block data and return true.
//
//       3. Call SamePrecondition to determine if the emp data has the same
//          condition at the start of processing the block as the block data.
//          If so, return false.
//
//       4. Invoke EvaluateBlock.
//
//       5. Call SamePostCondition to determine if the temporary data has the same
//          condition at the end of processing the block as the block data.
//
//       6. Update the block data from the temporary data.
//
//       7. Return the value returned by SamePostCondition.
//
// Returns:
//
//    true if the block data has changed, false otherwise.
//
//-----------------------------------------------------------------------------

bool
Walker::BlockAnalysis
(
   llvm::MachineBasicBlock * block
)
{
   Dataflow::Data * blockData = this->GetBlockData(block);
   Dataflow::Data * temporaryData;
   bool             wasChanged;

   //block->dump();

   temporaryData = this->Merge(block, blockData);

   if (blockData->MustEvaluate) {
      // Forced evaluation indicates we are performing local analysis of this
      // block.  This may be the first time ever or the first time for a
      // particular traversal.  Either way, act as if the block changed.

      this->EvaluateBlock(block, temporaryData);
      blockData->Update(temporaryData);
      blockData->MustEvaluate = false;

      return true;
   }

   // See if we need to reevaluate the block.
   if (temporaryData->SamePrecondition(blockData)) {

      return false;
   }

   this->EvaluateBlock(block, temporaryData);

   // See if anything has changed.
   wasChanged = !temporaryData->SamePostCondition(blockData);

   // Update the block with the recomputed data.
   //
   // Will there be clients that want to update pre- and post-conditions separately for efficiency issues?

   blockData->Update(temporaryData);

   return wasChanged;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Add a block to the iteration queue.
//
// Arguments:
//
//    block - Block data to be queued
//
//-----------------------------------------------------------------------------

void
Walker::AddToInitialQueue
(
   llvm::MachineBasicBlock*  block
)
{
   assert(block->getNumber() >= 0);
   unsigned blockId = block->getNumber();
   unsigned id;

   id = this->ConvertBlockIdToQueueId(blockId);
   this->InitialQueuedBitVector->set(id);
   this->QueuedBitVector->set(id);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Iterate until fix point.
//
// Remarks:
//
//    Iteration is demand driven via a priority queue implementation.  Upon
//    entry, the priority queue has been seeded with the blocks requiring
//    recomputation.
//
//    The earliest block is removed from the queue and local analysis
//    performed.  If the block changed, its successors (predecessors) are
//    queued.  The process repeats until the queue is empty.
//
//    We use a bit vector to implement a priority queue.  A sparse bit vector
//    is employed because it will be quicker to get the first bit and we don't
//    expect to have a huge number of blocks live at once.
//
//-----------------------------------------------------------------------------

void
Walker::Iterate()
{
   llvm::SparseBitVector<>*      initialQueuedBitVector;
   llvm::SparseBitVector<>*      queuedBitVector;
   llvm::SparseBitVector<>*      nextQueuedBitVector;
   int                           id;
   unsigned                      maximumBlockId;

#if defined(TILED_DEBUG_CHECKS)
   unsigned numberBlocksProcessed = 0;
#endif

   maximumBlockId = this->MaxBlockId;
   queuedBitVector = this->QueuedBitVector;
   initialQueuedBitVector = this->InitialQueuedBitVector;
   nextQueuedBitVector = this->NextQueuedBitVector;

   assert(initialQueuedBitVector != nullptr);
   assert(!initialQueuedBitVector->empty());
   assert(queuedBitVector != nullptr);
   assert(!queuedBitVector->empty());
   assert(nextQueuedBitVector != nullptr);
   assert(nextQueuedBitVector->empty());

   while ((id = queuedBitVector->find_first()) != -1)
   {

      queuedBitVector->reset(id);
      id = this->ConvertQueueIdToBlockId(id);

      Dataflow::Data *          blockData = this->GetBlockData(id);
      llvm::MachineBasicBlock * block = blockData->Block;

      // Compute this block.

      if (this->BlockAnalysis(block)) {
         // The block was changed; we need to queue its dependents.

         if (this->Direction == Dataflow::Direction::Forward) {
            llvm::MachineBasicBlock::succ_iterator s;
            for (s = block->succ_begin(); s != block->succ_end(); ++s)
            {
               llvm::MachineBasicBlock* successorBlock(*s);
               if (this->DoNotIterate && !this->GetBlockData(successorBlock)->MustEvaluate) {
                  // For non-iterative analysis, we Only queue successors that
                  // must be evaluated.
                  continue;
               }

               id = this->ConvertBlockIdToQueueId(successorBlock->getNumber());
               nextQueuedBitVector->set(id);
            }

         } else {
            llvm::MachineBasicBlock::pred_iterator p;
            for (p = block->pred_begin(); p != block->pred_end(); ++p)
            {
               llvm::MachineBasicBlock* predecessorBlock(*p);
               if (this->DoNotIterate && !this->GetBlockData(predecessorBlock)->MustEvaluate) {
                  // For non-iterative analysis, we Only queue predecessors
                  // that must be evaluated.
                  continue;
               }

               id = this->ConvertBlockIdToQueueId(predecessorBlock->getNumber());
               nextQueuedBitVector->set(id);
            }
         }
      }

      // Once we are done passing over queued set of blocks check if there
      // are any queued blocks in the next queued bit vector that tracked
      // blocks that had any changes. If there are any such blocks, swap the
      // queuedBitVector and nextQueuedBitVector bit vector pointers and repeat the process.

      if (queuedBitVector->empty() && !nextQueuedBitVector->empty()) {
         llvm::SparseBitVector<>* swapBitVector = queuedBitVector;
         queuedBitVector = nextQueuedBitVector;
         nextQueuedBitVector = swapBitVector;

         // Make sure we stay within initial queued set of blocks for dataflow iteration.
         *(queuedBitVector) &= *(initialQueuedBitVector);
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get the data flow node for the specified block.
//
// Returns:
//
//    Node object.
//
//-----------------------------------------------------------------------------

Dataflow::Data *
Walker::GetBlockData
(
   llvm::MachineBasicBlock *  block // Block to process
)
{
   return this->GetBlockDataInternal(block->getNumber() + 2);
}

Dataflow::Data *
Walker::GetBlockData
(
   unsigned id
)
{
   return this->GetBlockDataInternal(id + 2);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get the data flow node for the specified block Id.
//
// Returns:
//
//    Node object.
//
//-----------------------------------------------------------------------------

Dataflow::Data *
Walker::GetBlockDataInternal
(
   unsigned id // Block Id
)
{
   // if it beyond our allocated bounds, then return nullptr to signal we have no data 
   // (rather than indexing too far)

   if (id > this->MaxBlockId + 2) {
      DEBUG(llvm::dbgs() << "ID is " << id << ". Returning nullptr\n");
      return nullptr;
   }
   else {
      DEBUG(llvm::dbgs() << "ID is " << id << " and BlockData.size is " << this->BlockDataArray->size() << "\n");
      return (*this->BlockDataArray)[id];
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get the Id of block to be used by the priority queue.
//
// Returns:
//
//    Node object.
//
//-----------------------------------------------------------------------------

unsigned
Walker::ConvertBlockIdToQueueId
(
   unsigned id // Block Id
)
{
   return this->NodeOrder->Position(id);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Get the Id of block as used by the priority queue.
//
// Returns:
//
//    Node object.
//
//-----------------------------------------------------------------------------

unsigned
Walker::ConvertQueueIdToBlockId
(
   unsigned id // Queue Id
)
{
   return this->NodeOrder->NodeId(id);
}

} // namespace Dataflow
} // namespace Tiled
