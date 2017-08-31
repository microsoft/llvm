//===-- Graphs/Graph.h - Base Flow Graph Support ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_GRAPH_H
#define TILED_GRAPHS_GRAPH_H

#include "../Alias/Alias.h"
#include "../Cost.h"

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"

#include <list>
#include <map>

namespace Tiled
{
namespace Graphs
{

class NumberNodeVisitor;
class NodeWalker;
class FlowGraph;

struct FlowEdge
{
   llvm::MachineBasicBlock * predecessorBlock;
   llvm::MachineBasicBlock * successorBlock;

   FlowEdge()
      : predecessorBlock(nullptr), successorBlock(nullptr) {}

   FlowEdge(llvm::MachineBasicBlock* pred, llvm::MachineBasicBlock* succ)
      : predecessorBlock(pred), successorBlock(succ) {}

   FlowEdge(const FlowEdge& other)
   {
      predecessorBlock = other.predecessorBlock;
      successorBlock = other.successorBlock;
   }

   friend bool operator==(const FlowEdge &, const FlowEdge &);

   bool isUninitialized() const
   {
      return (this->predecessorBlock == nullptr || this->successorBlock == nullptr);
   }

   bool isCritical() const
   {
      return (this->predecessorBlock->succ_size() > 1 && this->successorBlock->pred_size() > 1);
   }

   bool isBack
   (
      llvm::MachineLoopInfo * mli
   )
   {
      llvm::MachineBasicBlock * succBlock = this->successorBlock;
      return (mli->isLoopHeader(succBlock) && (mli->getLoopFor(succBlock)->contains(mli->getLoopFor(this->predecessorBlock))));
   }

   bool isSplittable()
   {
      if (this->successorBlock->isEHPad())
         return false;

      if (this->predecessorBlock->empty())
         return true;

      llvm::MachineInstr *  lastTransfer = &(this->predecessorBlock->back());
      if (!lastTransfer->isBranch()) {
         // fall-through edge
         return true;
      }

      if (lastTransfer->isIndirectBranch())
         return true;

      // extra filter logic here for conditional branch(s) ?

      return true;
   }

   Tiled::Profile::Count getProfileCount() const;

   Tiled::Profile::Probability getProfileProbability() const;

   static int CompareProfileCounts
   (
      const Graphs::FlowEdge& edge1,
      const Graphs::FlowEdge& edge2
   );

   static FlowGraph * flowGraph;
};

typedef std::list<Graphs::FlowEdge>            FlowEdgeList;
typedef std::vector<Graphs::FlowEdge>          FlowEdgeVector;
typedef std::list<llvm::MachineBasicBlock* >   MachineBasicBlockList;
typedef std::vector<llvm::MachineBasicBlock* > MachineBasicBlockVector;

typedef int FrameIndex;

struct SlotEntry
{
   Graphs::FrameIndex frameSlot;
   Tiled::VR::StorageClass  storageClass;
   SlotEntry() : frameSlot(-1), storageClass(Tiled::VR::StorageClass::IllegalSentinel) {}
   SlotEntry
   (
      Graphs::FrameIndex index,
      Tiled::VR::StorageClass  storage = Tiled::VR::StorageClass::IllegalSentinel
   ) : frameSlot(index), storageClass(storage) {}
};
//TBD: typedef stdext::hash_map<unsigned, Graphs::FrameIndex> OperandToSlotMap;
typedef std::map<unsigned, Graphs::SlotEntry> OperandToSlotMap;


class FlowGraph
{
public:

   static FlowGraph *
   New
   (
      llvm::MachineFunction*      unit,
      llvm::MachineDominatorTree* mdt,
      llvm::MachineLoopInfo *     mli,
      llvm::Pass&                 pass
   );

   FlowGraph(llvm::Pass& P) : pass(P) {}

   llvm::Pass&                          pass;
   llvm::MachineFunction *              machineFunction;
   llvm::MachineDominatorTree*          MDT;
   llvm::MachineLoopInfo *              LoopInfo;
   llvm::MachineBasicBlock *            StartNode;
   llvm::MachineBasicBlock *            EndNode;
   Tiled::VR::Info *                    vrInfo;
   llvm::MachineBlockFrequencyInfo *    MbbFreqInfo;
   llvm::MachineBranchProbabilityInfo * MBranchProbInfo;

   llvm::MachineBasicBlock *
   SplitEdge
   (
      //note: no explicit edge objects in the llvm's Machine CFG
      Graphs::FlowEdge& edge
   );

   llvm::MachineBasicBlock *
   SplitBlock
   (
     llvm::MachineBasicBlock * block,
     llvm::MachineInstr *      instruction
   );

   llvm::MachineBasicBlock *
   SplitBlockWorker
   (
      llvm::MachineBasicBlock * block,
      llvm::MachineInstr *      instruction
   );

   llvm::MachineInstr *
   PrependLabelToBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   ChangeEdgeSuccessorBlock
   (
      Graphs::FlowEdge&         edge,
      llvm::MachineBasicBlock * newSuccessorBlock
   );


   void
   NewEdge
   (
      llvm::MachineBasicBlock * fromBlock,
      llvm::MachineBasicBlock * toBlock
   );

   llvm::MachineInstr *
   AppendGotoToBlock
   (
      llvm::MachineBasicBlock * block,
      llvm::MachineBasicBlock * newSuccessorBlock = nullptr
   );

   void
   ChangeEdgePredecessorBlock
   (
      Graphs::FlowEdge&         edge,
      llvm::MachineBasicBlock * newPredecessorBlock
   );

   void
   RedirectBranchInBlock
   (
      llvm::MachineBasicBlock * oldSuccessorBlock,
      llvm::MachineBasicBlock * predecessorBlock,
      llvm::MachineBasicBlock * newSuccessorBlock
   );

   void
   BuildDominators();

   bool
   Dominates
   (
      const llvm::MachineBasicBlock * A,
      const llvm::MachineBasicBlock * B
   ) const;

   bool
   Dominates
   (
      const llvm::MachineInstr *A,
      const llvm::MachineInstr *B
   ) const;

   Tiled::Profile::Count
   getProfileCount
   (
      llvm::MachineBasicBlock * block
   ) const;

   Tiled::Profile::Count
   getEdgeProfileCount
   (
      llvm::MachineBasicBlock * sourceBlock,
      llvm::MachineBasicBlock * destinationBlock
   ) const;

   Tiled::Profile::Probability
   getEdgeProfileProbability
   (
      llvm::MachineBasicBlock * sourceBlock,
      llvm::MachineBasicBlock * destinationBlock
   );

   static llvm::MachineBasicBlock *
   UniqueNonEHPredecessorBlock
   (
      llvm::MachineBasicBlock * block
   );

   static llvm::MachineBasicBlock *
   UniqueNonEHSuccessorBlock
   (
      llvm::MachineBasicBlock * block
   );

   llvm::MachineInstr *
   FindNextInstructionInBlock
   (
      unsigned             opcode,
      llvm::MachineInstr * startInstruction
   );

   llvm::MachineInstr *
   FindPreviousInstructionInBlock
   (
      unsigned             opcode,
      llvm::MachineInstr * startInstruction
   );

   void
   MoveAfter
   (
      llvm::MachineInstr * thisInstruction,
      llvm::MachineInstr * instructionToMove
   );

   void
   MoveBefore
   (
      llvm::MachineInstr * thisInstruction,
      llvm::MachineInstr * instructionToMove
   );

   unsigned                  MaxNodeId;
   unsigned                  NodeCount;
   unsigned                  VersionNumber;
   bool                      AreDepthFirstNumbersValid();
   unsigned                  editDepth;
   bool                      HasDeadRegisterSpills;

   llvm::MachineBasicBlock*  getNode(unsigned id);
   unsigned                  getPostNumber(llvm::MachineBasicBlock*);
   unsigned                  getMaxPrePostNumber();

   void     BuildDepthFirstNumbers();  //called by NodeFlowOrder::Build

   NumberNodeVisitor*  numberVisitor;
   NodeWalker*         nodeWalker;

   llvm::MachineBasicBlock *
   uniqueSuccessorBlock
   (
      llvm::MachineBasicBlock * block
   );

private:

   void     Initialize(llvm::MachineFunction* unit, bool canBuildDepthFirstNumbers);

   void
   BeginEdit()
   {
      ++this->editDepth;
   }

   void
   EndEdit()
   {
      --this->editDepth;
      ++this->VersionNumber;
   }

   llvm::MachineOperand *
   getMbbOperand
   (
      llvm::MachineInstr *      instruction,
      llvm::MachineBasicBlock * block = nullptr
   );

   llvm::MachineBasicBlock *
   TopoSplitEdge
   (
      Graphs::FlowEdge& edge
   );

   void
   SplitEdgeWithNode
   (
      Graphs::FlowEdge&         splitEdge,
      llvm::MachineBasicBlock * newBlock
   );

   bool  canBuildDepthFirstNumbers;
};

} // namespace Graphs
} // namespace Tiled

#endif // end TILED_GRAPHS_GRAPH_H
