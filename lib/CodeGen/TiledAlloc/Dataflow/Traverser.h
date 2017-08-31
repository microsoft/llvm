//===-- Dataflow/Traverser.h ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_DATAFLOW_TRAVERSER_H
#define TILED_DATAFLOW_TRAVERSER_H

#include "../Alias/Alias.h"
#include "../Graphs/Graph.h"
#include "../Graphs/NodeFlowOrder.h"

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

#include <vector>

namespace Tiled
{

class FlowGraph;
class NodeFlowOrder;

//-----------------------------------------------------------------------------
//
// Description:
//
//    Data flow framework classes.
//
// Remarks:
//
//    This namespace implements a generic, flexible data flow package.
//
//-----------------------------------------------------------------------------

namespace Dataflow
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Enumeration describing the direction of the data flow traversal.
//
//-----------------------------------------------------------------------------

enum class Direction
{
   IllegalSentinel,
   Forward,
   Backward
};

//-----------------------------------------------------------------------------
//
// Description:
//
//    Enumeration describing the kind of the data flow traversal.
//
//-----------------------------------------------------------------------------

enum class TraversalKind
{
   IllegalSentinel,
   NonIterative,
   Iterative
};

//-----------------------------------------------------------------------------
//
// Description:
//
//    Bitmapped enumeration describing the flags for a data merge.
//
//-----------------------------------------------------------------------------

enum class MergeFlags
{
   None  = 0x0,
   First = 0x1,
   EH    = 0x2
};

typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;
typedef std::vector<llvm::MachineBasicBlock *> MachineBasicBlockVector;

//-----------------------------------------------------------------------------
//
// Description:
//
//    Data flow data block base class.
//
//-----------------------------------------------------------------------------

typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;

class Data
{

public:

   virtual void Delete();

public:

   virtual void
   Merge
   (
      Data *       dependencyData,
      Data *       blockData,
      Xcessor      incomingEdge,
      MergeFlags   flags
   ) = 0;

   virtual bool
   SamePostCondition
   (
      Data * blockData
   );

   virtual bool
   SamePrecondition
   (
      Data * blockData
   );

   virtual void
   Update
   (
      Data * temporaryData
   ) = 0;

public:

   llvm::MachineBasicBlock *  Block;
   bool                       MustEvaluate;

   Data(): Block(nullptr), MustEvaluate(false) {}
};

#if 0
comment Data::Merge
{
   // Description:
   //
   //    Merge (meet) an additional incoming data flow value into the current
   //    info.
   //
   // Arguments:
   //
   //    dependencyData   - Incoming data flow value.
   //    blockData - Current block data flow value.
   //    flags     - Merge control flags.
   //
   // Returns:
   //
   //    Nothing.
}

comment Data::Update
{
   // Description:
   //
   //    Update the current data flow value from temporaryData.
   //
   // Arguments:
   //
   //    temporaryData - New value.
   //
   // Returns:
   //
   //    Nothing.
}

comment Data::Block
{
   // Basic block associated with this data.
}

comment Data::MustEvaluate
{
   // Must we evaluate the block from scratch instead of using cached info?
}
#endif

//-----------------------------------------------------------------------------
//
// Description:
//
//    Data flow traversal package base class.
//
//-----------------------------------------------------------------------------

typedef std::vector<Data *> BlockData;

class Walker
{

public:

   virtual void Delete();

public:

   virtual void
   AllocateData
   (
      unsigned numberElements
   );

   virtual void
   EvaluateBlock
   (
      llvm::MachineBasicBlock * block,
      Data *                    temporaryData
   ) = 0;

   Data *
   GetBlockData
   (
      llvm::MachineBasicBlock * block
   );

   Data *
   GetBlockData
   (
      unsigned id
   );

   void
   Initialize
   (
      Direction            direction,
      Graphs::FlowGraph *  functionUnit
   );

   void
   Initialize
   (
      Direction           direction,
      Walker *            walker
   );

   Data *
   Merge
   (
      llvm::MachineBasicBlock * block,
      Data *                    blockData
   );

   static void StaticInitialize();

   void
   Traverse
   (
      TraversalKind           traversalKind,
      Graphs::FlowGraph *     functionUnit
   );

   void
   Traverse
   (
      TraversalKind              traversalKind,
      MachineBasicBlockVector *  blockVector
   );

protected:

   VR::Info *          vrInfo;
   BlockData *         BlockDataArray;
   Direction           Direction;
   Graphs::FlowGraph * FunctionUnit;
   Data *              InitializeData;
   Data *              TemporaryData;

protected:

   void
   AddToInitialQueue
   (
      llvm::MachineBasicBlock * block
   );

   bool
   BlockAnalysis
   (
      llvm::MachineBasicBlock * block
   );

   void Iterate();

protected:

   bool MemoryIsOwned;

private:

   unsigned
   ConvertBlockIdToQueueId
   (
      unsigned id
   );

   unsigned
   ConvertQueueIdToBlockId
   (
      unsigned id
   );

   Data *
   GetBlockDataInternal
   (
      unsigned id
   );

   void SetupNodeOrder();

private:

   bool                      DoNotIterate;
   Graphs::FlowGraph *       FlowGraph;
   llvm::SparseBitVector<> * InitialQueuedBitVector;
   unsigned                  MaxBlockId;
   llvm::SparseBitVector<> * NextQueuedBitVector;
   Graphs::NodeFlowOrder *   NodeOrder;
   llvm::SparseBitVector<> * QueuedBitVector;

public:
   Walker() :
      vrInfo(nullptr), BlockDataArray(nullptr), Direction(Direction::IllegalSentinel),
      FunctionUnit(nullptr), InitializeData(nullptr), TemporaryData(nullptr),
      DoNotIterate(false), FlowGraph(nullptr), InitialQueuedBitVector(nullptr), MaxBlockId(std::numeric_limits<unsigned>::max()),
      NextQueuedBitVector(nullptr), NodeOrder(nullptr), QueuedBitVector(nullptr)
      {}
};

#if 0
comment Walker::EvaluateBlock
{
   // Description:
   //
   //    Perform analysis on a block.
   //
   // Arguments:
   //
   //    block -   Block to analyze.
   //    temporaryData - Temporary data to use for scratch calculations.
}

comment Walker::BlockDataArray
{
   // Array of data elements associated with each flow graph block.
}

comment Walker::DebugControl
{
   // Component control for debugging Walker.
}

comment Walker::Direction
{
   // Traversal direction.
}

comment Walker::DoNotIterate
{
   // Should we iterate to convergence?
}

comment Walker::InitializeData
{
   // Data value to use when initializing data elements.
}

comment Walker::Lifetime
{
   // Memory allocation lifetime.
}

comment Walker::MaxBlockId
{
   // Max block Id at time of creation.
}

comment Walker::MaxIteratorNumberPerBlock
{
   // Max number of iteratons for any block which is expected before
   // walker computed result.
}

comment Walker::MemoryIsOwned
{
   // Does this Walker object own the memory it is using?
}

comment Walker::InitialQueuedBitVector
{
   // Bit vector used to hold the initially queued set of blocks to include in the
   // dataflow solution. We also stay within this set of blocks when iterating. No
   // other blocks are looked at.
}

comment Walker::NextQueuedBitVector
{
   // Bit vector used for swapping in changed blocks.
}

comment Walker::NodeOrder
{
   // Node ordering used by the walker.
}

comment Walker::QueuedBitVector
{
   // Bit vector representing block queue.
}

comment Walker::TemporaryData
{
   // Data value to use for temporary calculations.
}

comment Walker::DefaultMaxIteratorNumberPerBlock
{
   // Default value for MaxIteratorNumberPerBlock
}
#endif

} // namespace Dataflow
} // namespace Tiled

#endif // end TILED_DATAFLOW_TRAVERSER_H
