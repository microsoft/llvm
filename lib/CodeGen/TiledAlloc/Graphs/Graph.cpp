//===-- Graphs/Graph.cpp - Base Flow Graph Support --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Graph.h"
#include "NodeWalker.h"
#include "NumberNodeVisitor.h"

#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/BranchProbability.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"

namespace Tiled
{
namespace Graphs
{

bool operator==(const FlowEdge& fe1, const FlowEdge& fe2)
{
   if (fe1.predecessorBlock == fe2.predecessorBlock) {
      return (fe1.successorBlock == fe2.successorBlock);
   }

   return false;
};

Tiled::Profile::Count
FlowEdge::getProfileCount() const
{
   Tiled::Profile::Count sourceBlockCount = 0;
   llvm::MachineInstr * lastBranch = (this->predecessorBlock->empty()) ? nullptr : &(this->predecessorBlock->back());

   if (this->predecessorBlock->succ_size() == 1) {
      // unconditional branch
      if (lastBranch && !lastBranch->isIndirectBranch()) {
         sourceBlockCount = FlowEdge::flowGraph->getProfileCount(this->predecessorBlock);
      }

      return sourceBlockCount;
   }

   return FlowEdge::flowGraph->getEdgeProfileCount(this->predecessorBlock, this->successorBlock);
}

Tiled::Profile::Probability
FlowEdge::getProfileProbability() const
{
   return FlowEdge::flowGraph->getEdgeProfileProbability(this->predecessorBlock, this->successorBlock);
}

int
FlowEdge::CompareProfileCounts
(
   const Graphs::FlowEdge& edge1,
   const Graphs::FlowEdge& edge2
)
{
   assert(!edge1.isUninitialized() && !edge2.isUninitialized());

   const Profile::Count count1 = edge1.getProfileCount();
   const Profile::Count count2 = edge2.getProfileCount();

   bool isEdge1Larger = Tiled::CostValue::CompareGT(count1, count2);
   if (isEdge1Larger) {
      return 1;
   }

   bool isEdge2Larger = Tiled::CostValue::CompareGT(count2, count1);
   if (isEdge2Larger) {
      return -1;
   }

   // We need to tie-break based on identity to ensure we get the same sorted order on
   // managed vs. native, which have different "Sort" implementations.

   if (edge1.predecessorBlock->getNumber() != edge2.predecessorBlock->getNumber()) {
      return edge1.predecessorBlock->getNumber() > edge2.predecessorBlock->getNumber() ? -1 : 1;
   } else {
      return edge1.successorBlock->getNumber() > edge2.successorBlock->getNumber() ? -1 : 1;
   }

   // Ensure that identity means identity
   assert(edge1 == edge2);

   return 0;
}


Graphs::FlowGraph *  FlowEdge::flowGraph;

void
FlowGraph::Initialize
(
   llvm::MachineFunction*  unit,
   bool                    canBuildDepthFirstNumbers
)
{
   this->machineFunction = unit;
   this->VersionNumber = 1;
   this->editDepth = 0;
   this->canBuildDepthFirstNumbers = canBuildDepthFirstNumbers;

   if (canBuildDepthFirstNumbers) {
      if (this->nodeWalker == nullptr) {
         this->nodeWalker = NodeWalker::New();
      }
      this->nodeWalker->graph = this;
   }

   this->MaxNodeId = this->machineFunction->getNumBlockIDs() - 1;
   this->NodeCount = this->machineFunction->size();
   this->StartNode = llvm::GraphTraits<llvm::MachineFunction*>::getEntryNode(this->machineFunction);

   this->EndNode = nullptr;
   llvm::MachineFunction::reverse_iterator rb;
   for (rb = this->machineFunction->rbegin(); rb != this->machineFunction->rend(); ++rb) {
      if (rb->succ_empty()) {
         this->EndNode = &(*rb);
         break;
      }
   }
   if (this->EndNode == nullptr) {
      assert(!this->machineFunction->empty());
      this->EndNode = &(this->machineFunction->back());
   }

   this->vrInfo = new Tiled::VR::Info(this->machineFunction);
}

FlowGraph*
FlowGraph::New
(
   llvm::MachineFunction *      unit,
   llvm::MachineDominatorTree * mdt,
   llvm::MachineLoopInfo *      mli,
   llvm::Pass&                  pass
)
{
   assert(unit != nullptr);
   FlowGraph* graph = new FlowGraph(pass);
   graph->nodeWalker = nullptr;
   graph->MDT = mdt;
   graph->LoopInfo = mli;
   graph->Initialize(unit, true);

   return graph;
}

unsigned
FlowGraph::getPostNumber
(
   llvm::MachineBasicBlock* node
)
{
   assert(this->numberVisitor != nullptr);
   unsigned postNumber = this->numberVisitor->getPostNumber(node);

   return postNumber;
}

unsigned
FlowGraph::getMaxPrePostNumber()
{
   assert(this->AreDepthFirstNumbersValid());

   if (this->numberVisitor != nullptr) {
      return this->numberVisitor->getMaxNumber();
   }
   return 0;
}

inline
llvm::MachineBasicBlock*
FlowGraph::getNode
(
   unsigned id
)
{
   assert(this->machineFunction != nullptr);
   assert((id > 0) && (id <= this->MaxNodeId));

   llvm::MachineBasicBlock* node = this->machineFunction->getBlockNumbered(id);
   return node;
}

void
FlowGraph::BuildDepthFirstNumbers()
{
   assert(this->canBuildDepthFirstNumbers);

   // Don't rebuild if we're already up to date.
   if (this->AreDepthFirstNumbersValid()) {
      return;
   }

   // Create a numberer, if we never had one.
   if (this->numberVisitor == nullptr) {
      this->numberVisitor = Graphs::NumberNodeVisitor::New();
   }

   // Recompute.

   assert(this->StartNode != nullptr);
   this->nodeWalker->WalkFrom(this->numberVisitor, this->StartNode);

   assert(this->AreDepthFirstNumbersValid());
}

bool
FlowGraph::AreDepthFirstNumbersValid()
{
   if (this->numberVisitor == nullptr) {
      return false;
   }
   return this->numberVisitor->IsValid();
}

llvm::MachineInstr *
findLabelInstr(llvm::MachineBasicBlock * block)
{
   llvm::MachineBasicBlock::instr_iterator(i);
   for (i = block->instr_begin(); i != block->instr_end(); ++i)
   {
      if (i->isLabel()) {
         return &(*i);
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Create a new node by splitting an edge.
//
// Arguments:
//
//    splitEdge - edge to split
//
// Remarks:
//
//    Given a graph edge E: P->S, this method modifies the graph by
//    inserting a new node N so that P->N->S. The existing edge is
//    updated, E: N->S, and a new edge is created, E': P->N.
//
//    There are 2 cases with which we have to deal.
//
//    1) Edge E has a label operand.
//
//    We assert that
//    (a) P ends in a branch* (to S), and
//    (b) S begins with a label.
//
//    We then
//    (a) start N with a new label,
//    (b) change P to branch to N rather than S, and
//    (c) end N with a branch to S:
//
//    ------------------    ------------------
//    <P>: ...              <P>: ...
//         jmp* <succ>           jmp* <current>
//    ------------------    ------------------
//    <S>: label <succ>     <N>: label <current>
//         ...                   jmp <succ>
//    ------------------    ------------------
//                          <S>: label <succ>
//                               ...
//                          ------------------
//
//    [Note: P's branch can be a switch....]
//
//    2) Edge E has no label operand.
//
//    We assert that
//
//    (a) P does not end in a branch, and
//    (b) S does no start with a label.
//
//    We then
//    (a) prepend a new label to S, and
//    (b) end N with a branch to S.
//
//    ------------------    ------------------
//    <P>: ...              <P>: ...
//    ------------------    ------------------
//    <S>: ...              <N>: jmp <succ>
//    ------------------    ------------------
//                          <S>: label <succ>
//                               ...
//                          ------------------
//
// Returns:
//
//    Newly created basic block.
//
//-----------------------------------------------------------------------------

llvm::MachineBasicBlock *
FlowGraph::SplitEdge
(
   //note: no explicit edge objects in the llvm's Machine CFG
   Graphs::FlowEdge& edge
)
{
   llvm::MachineInstr * predecessorLastInstruction = &(edge.predecessorBlock->instr_back());

   //was:  IR::LabelOperand ^ edgeLabelOperand = splitEdge->LabelOperand;
   llvm::MachineInstr * edgeDestinationLabelInstr = findLabelInstr(edge.successorBlock);
   //TBD:  Can there be multiple labels? If so, match with target label of (the) branch in the predecessor

   // We're editing the graph....
   this->BeginEdit();

   // Instructions in the new block will be assigned the same line number
   // as the last instruction in the predecessor block.
   const llvm::DebugLoc& dl( predecessorLastInstruction->getDebugLoc() );

   // Split the edge, resulting in an empty basic block.
   llvm::MachineBasicBlock * currentBlock = this->TopoSplitEdge(edge);

   //llvm::MachineFunction * functionUnit = this->machineFunction;

   // The over-arching concern: does the split edge have a label?

   if (edgeDestinationLabelInstr != nullptr) {
      // Setup N's label.
      PrependLabelToBlock(currentBlock);

      // Setup N's branch to S.
      llvm::MachineFunction * MF = this->machineFunction;
      assert(MF == currentBlock->getParent());
      
      const llvm::TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
      llvm::SmallVector<llvm::MachineOperand, 0> EmptyCond;
      TII->insertBranch(*currentBlock, edge.successorBlock, nullptr, EmptyCond, dl);

      // Change P's branch target from S to N, and update the split edge's label.

      assert(predecessorLastInstruction->isBranch());
      //TBD: isBranch() is true for an indirect branch, IR switch instruction is typically translated into an indirect branch

      //TBD:  ??? replace the operand of the predecessor branch with currentLabel? (extract the current label's MachineInstr?)

   } else {

      // Insert a goto and label pair for this block and the succ.
      this->AppendGotoToBlock(currentBlock);

      assert((edge.predecessorBlock->instr_back()).isBranch());
   }

   // Propagate profile information
   //TBD:  functionUnit->ProfileInfo->FixupSplitEdge(currentBlock);

   // We're done.

   this->EndEdit();

   return currentBlock;
}

llvm::MachineBasicBlock *
FlowGraph::TopoSplitEdge
   // 'downcasted'  Graph::SplitEdge, does only the topological job
(
   //note: no explicit edge objects in the llvm's Machine CFG
   Graphs::FlowEdge& edge
)
{
   this->BeginEdit();

   llvm::MachineBasicBlock * newBlock = this->machineFunction->CreateMachineBasicBlock();
   this->machineFunction->insert(std::next(llvm::MachineFunction::iterator(edge.predecessorBlock)), newBlock);
   this->NodeCount++;
   if (newBlock->getNumber() > this->MaxNodeId)
      this->MaxNodeId = newBlock->getNumber();

   this->SplitEdgeWithNode(edge, newBlock);

   this->EndEdit();

   return newBlock;
}

llvm::MachineInstr *
FlowGraph::PrependLabelToBlock
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineFunction *        MF = this->machineFunction;
   assert(MF == block->getParent());

   const llvm::TargetInstrInfo& TII = *(MF->getSubtarget().getInstrInfo());
   llvm::MCSymbol * Label = ((block->getParent())->getContext()).createTempSymbol();

   llvm::MachineBasicBlock::instr_iterator I(((block->empty()) ? block->instr_end() : block->instr_begin()));

   //TBD:  Can block->instr_front()).isLabel() be already true, i.e. multiple labels?
   BuildMI(*block, I, I->getDebugLoc(), TII.get(llvm::TargetOpcode::GC_LABEL)).addSym(Label); /*1T: TBD: ??*/

   return &(block->instr_front());
}

unsigned
ithPredecessor
(
   llvm::MachineBasicBlock * predecessorBlock,
   llvm::MachineBasicBlock * successorBlock
)
{
   unsigned ith;
   llvm::MachineBasicBlock::const_pred_iterator p;
   for (ith = 1, p = successorBlock->pred_begin(); p != successorBlock->pred_end(); ++p, ++ith)
   {
      if (*p == predecessorBlock) {
         return ith;
      }
   }

   return 0;
}

void
shufflePredecessorIntoPlace
(
   llvm::MachineBasicBlock * successorBlock,
   llvm::MachineBasicBlock * newPredecessor,
   unsigned ith
)
{
   //check that the preceeding addSuccessor() appended the new predecessor at the end of predecessors
   assert(*(successorBlock->pred_rbegin()) == newPredecessor);
   unsigned sz = successorBlock->pred_size();
   assert(ith > 0 && ith < sz);

   llvm::MachineBasicBlock::pred_reverse_iterator p;
   for (p = successorBlock->pred_rbegin(); p != successorBlock->pred_rend(); ++p)
   {
      if (sz == ith) {
         break;
      }
      llvm::MachineBasicBlock::pred_reverse_iterator pp = p + 1;
      llvm::MachineBasicBlock * tmp = (*p);
      *p = *pp;
      *pp = tmp;
      --sz;
   }

   assert(sz == ith);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Internal method to split edge with the indicated node.
//
// Arguments:
//
//    splitEdge - edge to split
//    newNode - node to insert into split
//
// Remarks:
//
//    Given a graph edge E:F->T and a node N, modify the graph
//    so that  E:N->T and create a new E' such that E':F->N.
//    The edge E is preserved for N->T so that if T is a converge
//    point, order of source opnds in Phi instructions still match the
//    order of their reaching definition edges after the split.
//
//-----------------------------------------------------------------------------

void
FlowGraph::SplitEdgeWithNode
(
   Graphs::FlowEdge&         edge,
   llvm::MachineBasicBlock * newBlock
)
{
   llvm::MachineBasicBlock * predecessorBlock = edge.predecessorBlock;
   llvm::MachineBasicBlock * successorBlock = edge.successorBlock;

   this->BeginEdit();

   unsigned ith = ithPredecessor(predecessorBlock, successorBlock);
   assert(ith > 0);
   bool wasLastPredecessor = (ith == successorBlock->pred_size());
   predecessorBlock->replaceSuccessor(successorBlock, newBlock);

   //addSuccessor() also appends the predecessor link at the end predecessors (wrong in some cases).
   newBlock->addSuccessor(successorBlock, llvm::BranchProbability::getOne());
   if (!wasLastPredecessor) {
      //this places the new predecessor in the same position as the old, preserves order for PHI arguments
      shufflePredecessorIntoPlace(successorBlock, newBlock, ith);
   }

   this->EndEdit();
}

void
FlowGraph::ChangeEdgeSuccessorBlock
(
   //note: no explicit edge objects in the llvm's Machine CFG
   Graphs::FlowEdge&          changingEdge,
   llvm::MachineBasicBlock *  newSuccessorBlock
)
{
   assert(!changingEdge.isUninitialized());
   llvm::MachineBasicBlock * oldSuccessorBlock = changingEdge.successorBlock;
   llvm::MachineBasicBlock * predecessorBlock = changingEdge.predecessorBlock;

   assert(predecessorBlock != nullptr && oldSuccessorBlock != nullptr && newSuccessorBlock != nullptr);
   assert(oldSuccessorBlock->getParent() == newSuccessorBlock->getParent());
   assert(this->machineFunction == newSuccessorBlock->getParent());

   // Begin editing
   this->BeginEdit();

   //note: PHX representation w/ explicit edge objects could just re-link the destination end of the edge,
   //      LLVM does not allow that, the predecessor/successor lists of both blocks must be modified.

   predecessorBlock->removeSuccessor(oldSuccessorBlock);
   predecessorBlock->addSuccessor(newSuccessorBlock, llvm::BranchProbability::getOne());

   this->RedirectBranchInBlock(oldSuccessorBlock, predecessorBlock, newSuccessorBlock);

   changingEdge.successorBlock = newSuccessorBlock;

   // Done editing
   this->EndEdit();
}

void
FlowGraph::ChangeEdgePredecessorBlock
(
   //note: no explicit edge objects in the llvm's Machine CFG
   Graphs::FlowEdge&         changingEdge,
   llvm::MachineBasicBlock * newPredecessorBlock
)
{
   assert(!changingEdge.isUninitialized());
   llvm::MachineBasicBlock * oldPredecessorBlock = changingEdge.predecessorBlock;
   llvm::MachineBasicBlock * successorBlock = changingEdge.successorBlock;

   assert(successorBlock != nullptr && oldPredecessorBlock != nullptr && newPredecessorBlock != nullptr);
   assert(oldPredecessorBlock->getParent() == newPredecessorBlock->getParent());
   assert(this->machineFunction == newPredecessorBlock->getParent());

   // Begin editing
   this->BeginEdit();

   // We prepend the edge to block's successor list.

   //note: PHX representation w/ explicit edge objects could just re-link the source end of the edge,
   //      LLVM does not allow that, the predecessor/successor lists of both blocks must be modified.

   oldPredecessorBlock->removeSuccessor(successorBlock);
   newPredecessorBlock->addSuccessor(successorBlock);
   //was:  newPredecessorNode->LinkSuccessorEdgeByPrepend(changingEdge),
   //      the assumption is that prepend/append makes no difference for RA client.
   changingEdge.predecessorBlock = newPredecessorBlock;

   // Done editing
   this->EndEdit();
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Split a single basic block into two blocks by dividing it before the
//    given instruction.
//
// Arguments:
//
//    block - The basic block to split.
//    instruction - The IR::Instruction which will become the first instruction in
//                  the new block.
//
// Returns:
//
//    Newly created basic block.
//
//-----------------------------------------------------------------------------

llvm::MachineBasicBlock *
FlowGraph::SplitBlock
(
   llvm::MachineBasicBlock * block,
   llvm::MachineInstr *      instruction
)
{
   this->BeginEdit();

   llvm::MachineBasicBlock * blockNew = this->SplitBlockWorker(block, instruction);
   this->AppendGotoToBlock(block);  // needed?

   this->EndEdit();

   return blockNew;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Split a single basic block into two blocks by dividing it before the
//    given instruction. This is a helper function for SplitBlock and relies
//    on the fact that flow graph is in the edit mode.
//
// Arguments:
//
//    block - The basic block to split.
//    instruction - The IR::Instruction which will become the first instruction in
//                  the new block.
//
// Returns:
//
//    Newly created basic block.
//
//-----------------------------------------------------------------------------

llvm::MachineBasicBlock *
FlowGraph::SplitBlockWorker
(
   llvm::MachineBasicBlock * block,
   llvm::MachineInstr *      instruction
)
{
   llvm::MachineBasicBlock * newBlock;

   // Make sure flow graph is in the edit mode.
   //assert(this->IsEditable);

   // Create a new block adjacent to the given block.
   newBlock = this->machineFunction->CreateMachineBasicBlock();
   this->machineFunction->insert(std::next(llvm::MachineFunction::iterator(block)), newBlock);
   this->NodeCount++;
   if (newBlock->getNumber() > this->MaxNodeId)
      this->MaxNodeId = newBlock->getNumber();

   // Let all successor edges of block become successors of blockNew.
   llvm::MachineBasicBlock::succ_iterator si;
   for (si = block->succ_begin(); si != block->succ_end(); ++si)
   {
      newBlock->addSuccessor(*si);
      block->removeSuccessor(si);
   }
   // did not use transferSuccesors function because it wouldn't remove block as a pred in each of the succs

   block->addSuccessor(newBlock);

   // Move the range of instructions in block following instruction into the new block.
   llvm::MachineBasicBlock::instr_iterator ii(instruction);
   for (; ii != block->instr_end(); ++ii)
   {
      newBlock->insert(newBlock->instr_end(), &(*ii));
   }

   return newBlock;
}

void
FlowGraph::NewEdge
(
   llvm::MachineBasicBlock * fromBlock,
   llvm::MachineBasicBlock * toBlock
)
{
   assert(fromBlock != nullptr && toBlock != nullptr);

   fromBlock->addSuccessor(toBlock, llvm::BranchProbability::getOne());
}


llvm::MachineBasicBlock *
FlowGraph::uniqueSuccessorBlock(llvm::MachineBasicBlock * block)
{
   if (block->succ_size() == 1)
      return *(block->succ_begin());

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Append the goto instruction to the end of the instruction list
//    of the block and add a target label in the successor block. This
//    method expects the given block to have a single successor edge
//    in the flow-graph to determine the target of the goto.
//
// Arguments:
//
//    block - Append goto to this block
//
// Returns:
//
//    The newly appended goto BranchInstruction *
//
//-----------------------------------------------------------------------------

llvm::MachineInstr *
FlowGraph::AppendGotoToBlock
(
   llvm::MachineBasicBlock * block,
   llvm::MachineBasicBlock * newSuccessorBlock
)
{
   llvm::MachineBasicBlock * successorBlock = (newSuccessorBlock == nullptr)? this->uniqueSuccessorBlock(block): newSuccessorBlock;
   assert(successorBlock != nullptr);

   this->BeginEdit();

   // Setup N's branch to S.

   llvm::DebugLoc dl;
   const llvm::DebugLoc& DL((block->empty()) ? dl : const_cast<const llvm::DebugLoc&>(block->instr_back().getDebugLoc()));
   llvm::SmallVector<llvm::MachineOperand, 0> EmptyCond;
   const llvm::TargetInstrInfo& TII = *(this->machineFunction->getSubtarget().getInstrInfo());

   TII.insertBranch(*block, successorBlock, nullptr, EmptyCond, DL);
   llvm::MachineInstr * gotoInstruction = &(block->instr_back());
   assert(gotoInstruction->isUnconditionalBranch());

   //TODO:
   // profileInfo->EdgeDataProfile->SetCount(newLabelOperand, count,
   //    Profile::Heuristics::MaximumProbability, Profile::Predictor::Unconditional);

   this->EndEdit();

   return gotoInstruction;
}

int
getJTIndexFromUseDefChain
(
   llvm::MachineInstr *              indirectBranch,
   const llvm::MachineRegisterInfo&  mri
)
{
   assert(indirectBranch->isIndirectBranch());
   int index = -1;

   llvm::MachineInstr *   instr = nullptr;
   llvm::MachineOperand * target = nullptr;
   std::vector<llvm::MachineOperand*> operandStack;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(indirectBranch->uses());
   llvm::MachineInstr::mop_iterator oitr;

   // foreach_register_source_opnd
   for (oitr = uses.begin(); oitr != uses.end(); ++oitr)
   {
      if (oitr->isReg() && oitr->getReg() != 0) {
         target = oitr;
         break;
      }
   }
   assert(target != nullptr);
   operandStack.push_back(target);

   while (!operandStack.empty()) {
      llvm::MachineOperand * operand = (operandStack.back());
      operandStack.pop_back();

      llvm::iterator_range<llvm::MachineRegisterInfo::reg_iterator> usedefs = mri.reg_operands(operand->getReg());
      llvm::MachineRegisterInfo::reg_iterator ri;
      for (ri = usedefs.begin(); ri != usedefs.end(); ++ri) {
         llvm::MachineOperand * o = &(*ri);
         if (o->isDef()) {
            instr = o->getParent();
            assert(instr != nullptr);

            llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instr->uses());
            // foreach_register_source_opnd
            for (oitr = uses.begin(); oitr != uses.end(); ++oitr)
            {
               if (oitr->isReg() && oitr->getReg() != 0) {
                  operandStack.push_back(&(*oitr));
               } else if (oitr->isJTI()) {
                  return oitr->getIndex();
               }
            }
         }
      }
   }


   return index;
}

void
FlowGraph::RedirectBranchInBlock
(
   llvm::MachineBasicBlock * oldSuccessorBlock,
   llvm::MachineBasicBlock * predecessorBlock,
   llvm::MachineBasicBlock * newSuccessorBlock
)
{
   assert(newSuccessorBlock != nullptr);
   if (predecessorBlock->empty()) {
      return;
   }
   llvm::MachineInstr *  lastTransfer = &(predecessorBlock->back());

   if (lastTransfer->isBranch()) {

      if (lastTransfer->isIndirectBranch()) {
         const llvm::MachineRegisterInfo& MRI(this->machineFunction->getRegInfo());
         int jtIndex = getJTIndexFromUseDefChain(lastTransfer, MRI);
         assert(jtIndex != -1);
         llvm::MachineJumpTableInfo * jtInfo = this->machineFunction->getJumpTableInfo();

         jtInfo->ReplaceMBBInJumpTable(jtIndex, oldSuccessorBlock, newSuccessorBlock);
         return;
      }

      llvm::MachineOperand * operand = this->getMbbOperand(lastTransfer, oldSuccessorBlock);
      if (operand) {
         operand->setMBB(newSuccessorBlock);
         return;
      }

      //TODO: the code below needs rewrite when the recognizer MachineInstr::isConditionalBranch() works correctly!
      // the edge was for the fall-through
      llvm::MachineInstr *  nextToLastTransfer = lastTransfer->getPrevNode();
      if ((nextToLastTransfer == nullptr) || !nextToLastTransfer->isBranch()) {
         assert(predecessorBlock->succ_size() > 1);  //i.e. conditional branch, make fall-through explicit
         this->AppendGotoToBlock(predecessorBlock, newSuccessorBlock);
         return;
      }

      while (nextToLastTransfer && nextToLastTransfer->isBranch()) {
         operand = this->getMbbOperand(nextToLastTransfer, oldSuccessorBlock);
         if (operand) {
            operand->setMBB(newSuccessorBlock);
            return;
         }
         nextToLastTransfer = nextToLastTransfer->getPrevNode();
      }
      //i.e. all the branches were conditional, make fall-through explicit
      //this->AppendGotoToBlock(predecessorBlock, newSuccessorBlock);
      assert(0 && "No branch with specified old successor block");
   } else {
      this->AppendGotoToBlock(predecessorBlock, newSuccessorBlock);
   }
}

llvm::MachineOperand *
FlowGraph::getMbbOperand
(
   llvm::MachineInstr *      instruction,
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineInstr::mop_iterator operand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> operands(instruction->explicit_operands());

   for (operand = instruction->operands_begin(); operand != instruction->operands_end(); ++operand)
   {
      if (operand->isMBB()) {
         if (operand->getMBB()->getNumber() == block->getNumber())
            return operand;
      }
   }

   return nullptr;
}

llvm::MachineBasicBlock *
uniquePredecessorBlock
(
   llvm::MachineBasicBlock * block
)
{
   if (block->pred_size() == 1)
      return *(block->pred_begin());

   return nullptr;
}

Tiled::Profile::Count
FlowGraph::getProfileCount
(
   llvm::MachineBasicBlock * block
) const
{
   if (block->empty()) {
      llvm::MachineBasicBlock * uniquePredecessor = uniquePredecessorBlock(block);
      if (uniquePredecessor != nullptr) {
         return this->getProfileCount(uniquePredecessor);
      }
   }

   return (this->MbbFreqInfo->getBlockFreq(block)).getFrequency();
}

Tiled::Profile::Count
FlowGraph::getEdgeProfileCount
(
   llvm::MachineBasicBlock * sourceBlock,
   llvm::MachineBasicBlock * destinationBlock
) const
{
   llvm::BranchProbability probability = this->MBranchProbInfo->getEdgeProbability(sourceBlock, destinationBlock);
   llvm::BlockFrequency mbbFrequency = this->MbbFreqInfo->getBlockFreq(sourceBlock);
   llvm::BlockFrequency edgeFrequency(mbbFrequency * probability);

   return edgeFrequency.getFrequency();
}

Tiled::Profile::Probability
FlowGraph::getEdgeProfileProbability
(
   llvm::MachineBasicBlock * sourceBlock,
   llvm::MachineBasicBlock * destinationBlock
)
{
   llvm::BranchProbability probability = this->MBranchProbInfo->getEdgeProbability(sourceBlock, destinationBlock);

   return Tiled::Float64(probability.getNumerator()) / Tiled::Float64(probability.getDenominator());
}

llvm::MachineInstr *
FlowGraph::FindNextInstructionInBlock
(
   unsigned             opcode,
   llvm::MachineInstr * startInstruction
)
{
   assert(startInstruction != nullptr);
   llvm::MachineBasicBlock * block = startInstruction->getParent();
   assert(block != nullptr);

   llvm::MachineBasicBlock::instr_iterator ii(startInstruction);
   for (; ii != block->instr_end(); ++ii)
   {
      if (ii->getOpcode() == opcode) {
         return &*ii;
      }
   }

   return nullptr;
}

llvm::MachineInstr *
FlowGraph::FindPreviousInstructionInBlock
(
   unsigned             opcode,
   llvm::MachineInstr * startInstruction
)
{
   assert(startInstruction != nullptr);
   llvm::MachineBasicBlock * block = startInstruction->getParent();
   assert(block != nullptr);

   llvm::MachineBasicBlock::reverse_instr_iterator rii(startInstruction);
   for (; rii != block->instr_rend(); ++rii)
   {
      if (rii->getOpcode() == opcode) {
         return &*rii;
      }
   }

   return nullptr;
}

void
FlowGraph::BuildDominators()
{
   // (re)construct dom tree.
   this->MDT->runOnMachineFunction(*(this->machineFunction));
}

bool
FlowGraph::Dominates
(
   const llvm::MachineBasicBlock * A,
   const llvm::MachineBasicBlock * B
) const
{
   return this->MDT->dominates(A, B);
}

bool
FlowGraph::Dominates
(
   const llvm::MachineInstr *A,
   const llvm::MachineInstr *B
) const
{
   return this->MDT->dominates(A, B);
}

llvm::MachineBasicBlock *
FlowGraph::UniqueNonEHPredecessorBlock
(
   llvm::MachineBasicBlock * block
)
{
   if (block->isEHPad()) {
      return nullptr;
          //can a block be entered both on non-exception and exception path? 
   }
   llvm::MachineBasicBlock * uniqueNonEHPredecessorBlock = nullptr;

   llvm::MachineBasicBlock::pred_iterator p;
   for (p = block->pred_begin(); p != block->pred_end(); ++p)
   {
      if (!(*p)->isEHPad()) {
         if (uniqueNonEHPredecessorBlock != nullptr) {
            return nullptr;
         }
         uniqueNonEHPredecessorBlock = *p;
      }
   }

   return uniqueNonEHPredecessorBlock;
}

llvm::MachineBasicBlock *
FlowGraph::UniqueNonEHSuccessorBlock
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineBasicBlock * uniqueNonEHSuccessorBlock = nullptr;

   llvm::MachineBasicBlock::succ_iterator s;
   for (s = block->succ_begin(); s != block->succ_end(); ++s)
   {
      if (!(*s)->isEHPad()) {
         if (uniqueNonEHSuccessorBlock != nullptr) {
            return nullptr;
         }
         uniqueNonEHSuccessorBlock = *s;
      }
   }

   return uniqueNonEHSuccessorBlock;
}

void
FlowGraph::MoveAfter
(
   llvm::MachineInstr * thisInstruction,
   llvm::MachineInstr * instructionToMove
)
{
   llvm::MachineBasicBlock * mbb = thisInstruction->getParent();
   instructionToMove = instructionToMove->removeFromParent();
   mbb->insertAfter(thisInstruction, instructionToMove);
}

void
FlowGraph::MoveBefore
(
   llvm::MachineInstr * thisInstruction,
   llvm::MachineInstr * instructionToMove
)
{
   llvm::MachineBasicBlock * mbb = thisInstruction->getParent();
   instructionToMove = instructionToMove->removeFromParent();
   mbb->insert(thisInstruction, instructionToMove);
}

} // namespace Graphs

} // namespace Tiled
