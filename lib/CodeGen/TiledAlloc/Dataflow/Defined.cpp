//===--- Dataflow/Defined.cpp -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Traverser.h"
#include "Liveness.h"
#include "Defined.h"
#include "../Graphs/Graph.h"

#define DEBUG_TYPE "tiled-defined"

namespace Tiled
{
namespace Dataflow
{

//------------------------------------------------------------------------------
//
// Description:
//
//    Static initialization of the DefinedWalker class.
//
//------------------------------------------------------------------------------

void
DefinedWalker::StaticInitialize()
{

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Create a new DefinedWalker object.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//    function - Function for dataflow analysis
//
//------------------------------------------------------------------------------

DefinedWalker *
DefinedWalker::New()
{
   DefinedWalker * definedWalker = new DefinedWalker();

   return definedWalker;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate DATA datastructure for the function. 
//
// Arguments:
//
//    numberElements - Number of blocks in the function
//
//------------------------------------------------------------------------------

void
DefinedWalker::AllocateData
(
   unsigned numberElements
)
{
   BlockData*  dataArray;

   dataArray = new BlockData(numberElements, nullptr);

   // Initialize the data.

   for (unsigned i = 0; i < numberElements; i++)
   {
      (*dataArray)[i] = DefinedData::New();
   }

   this->BlockDataArray = dataArray;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Main routine of DefinedWalker computation. Evaluate each basic block.
//    If it is the first time for a block, or if there is anything changed
//    and it is marked as MustEvaluate, recompute the in/out vector.
//
//    If liveness information is provided, trim In by LiveIn
//
//       In = In & Kill (aka LiveIn)
//
//    Otherwise, compute the defined-ness with:
//
//       Out = In .OR. Generate
//
//    This is a forward computation of dataflow. 
//
// Arguments:
// 
//    block - the basic block being evaluated
//    baseTemporaryData - the current DATA for this block before evaluation
//
//------------------------------------------------------------------------------

void
DefinedWalker::EvaluateBlock
(
   llvm::MachineBasicBlock * block,
   Data *                    baseTemporaryData
)
{
   Tiled::VR::Info *          vrInfo = this->vrInfo;
   Dataflow::LivenessWalker * livenessWalker = this->LivenessWalker;
   DefinedData *              temporaryData = static_cast<DefinedData *>(baseTemporaryData);
   DefinedData *              blockData = static_cast<DefinedData *>(this->GetBlockData(block));

   assert(temporaryData && blockData);

   DEBUG(llvm::dbgs() << "\nMBB#" << block->getNumber() << "   (DefinedWalker)\n");

   llvm::SparseBitVector<> * outBitVector = temporaryData->OutBitVector;
   llvm::SparseBitVector<> * inBitVector = temporaryData->InBitVector;
   llvm::SparseBitVector<> * generateBitVector = blockData->GenerateBitVector;
   llvm::SparseBitVector<> * killBitVector = blockData->KillBitVector;

   if (blockData->MustEvaluate) {
      // Compute gen and kill sets.

      generateBitVector->clear();
      killBitVector->clear();

      //EH: llvm::MachineBasicBlock * predecessorBlock = block->UniqueNonEHPredecessorBlock;
      //if (predecessorBlock != nullptr && predecessorBlock->LastInstruction->IsDanglingInstruction) {
         // Found a predecessor with a dangling instruction.  Verify that we are
         // its unique successor and process its dangling defs (if any) into
         // this block's dataflow

         //assert(block == predecessorBlock->UniqueNonEHSuccessorBlock);
         //assert(predecessorBlock == block->UniquePredecessorBlock);

         //llvm::MachineInstr * lastInstruction = &(predecessorBlock->instr_back());
         //this->EvaluateInstruction(lastInstruction, generateBitVector);
     //}

      llvm::MachineBasicBlock::instr_iterator ii;
      for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
      {
         const llvm::MachineInstr * instruction = &(*ii);
         //was: if (!isDanglingInstr(instruction)) {
         this->EvaluateInstruction(instruction, generateBitVector);

         DEBUG(instruction->dump());
     }

      // If liveness walker has been provided, we can use it to trim definitions
      // we are tracking using a kill set.

      if (livenessWalker != nullptr) {
         Dataflow::LivenessData *   livenessData;
         llvm::SparseBitVector<> *  liveInBitVector;
         unsigned                   vrTag;

         livenessData = static_cast<Dataflow::LivenessData *>(livenessWalker->GetBlockData(block->getNumber()));  //dynamic_cast needed if > 1 derived class
         assert(livenessData);
         liveInBitVector = livenessData->LiveInBitVector;

         llvm::SparseBitVector<>::iterator iter;

         // foreach_sparse_bv_bit
         for (iter = liveInBitVector->begin(); iter != liveInBitVector->end(); ++iter)
         {
            vrTag = *iter;
            vrInfo->OrMayPartialTags(vrTag, killBitVector);
         }

         DEBUG({
            llvm::SparseBitVector<>::iterator a;
            llvm::dbgs() << "  LiveIn:    {";
            for (a = liveInBitVector->begin(); a != liveInBitVector->end(); ++a) {
               llvm::dbgs() << *a << ", ";
            }
            llvm::dbgs() << "}\n";
         });
     }
   }

   // If liveness walker has been provided, we can use it to trim definitions
   // we are tracking using a kill set.
   // In = In & Kill (aka LiveIn)

   if (livenessWalker != nullptr) {
      (*inBitVector) &= (*killBitVector);
   }

   // Compute Out = In + Generate. 
   // This is update which can trigger iterative walks if temporaryData->OutBitVector
   // changes compared to what is saved in the block data.

   (*outBitVector) = (*inBitVector);
   (*outBitVector) |= (*generateBitVector);

   DEBUG({
      llvm::SparseBitVector<>::iterator a;
      llvm::dbgs() << "  Generate:  {";
      for (a = generateBitVector->begin(); a != generateBitVector->end(); ++a) {
         llvm::dbgs() << *a << ", ";
      }
      llvm::dbgs() << "}\n";
      llvm::dbgs() << "  Kill:      {";
      for (a = killBitVector->begin(); a != killBitVector->end(); ++a) {
         llvm::dbgs() << *a << ", ";
      }
      llvm::dbgs() << "}\n";

      llvm::dbgs() << "\n  TmpLiveOut:   {";
      for (a = outBitVector->begin(); a != outBitVector->end(); ++a) {
         llvm::dbgs() << *a << ", ";
      }
      llvm::dbgs() << "}\n\n";
   });
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Evaluate instruction for register definition generate.
//
//------------------------------------------------------------------------------

void
DefinedWalker::EvaluateInstruction
(
   const llvm::MachineInstr * instruction,
   llvm::SparseBitVector<> *  generateBitVector
)
{
   Tiled::VR::Info * vrInfo = this->vrInfo;

   llvm::iterator_range<llvm::MachineInstr::const_mop_iterator> range(instruction->defs());
   llvm::MachineInstr::const_mop_iterator destinationOperand;
   for (destinationOperand = range.begin(); destinationOperand != range.end(); ++destinationOperand)
   {
      if (!this->IsTracked(destinationOperand)) {
         continue;
      }

      unsigned vrTag = vrInfo->GetTag(destinationOperand->getReg());

      vrInfo->OrMayTotalTags(vrTag, generateBitVector);
   }
}

#if defined(TILED_DEBUG_DUMPS)

//------------------------------------------------------------------------------
//
// Description:
//
//    Dumping routine of DefinedWalker for debugging purpose. 
//
//------------------------------------------------------------------------------

void DefinedWalker::Dump()
{
   Tiled::FunctionUnit ^ functionUnit = this->FunctionUnit;

   foreach_block_in_func(block, functionUnit)
   {
      if (Controls::DebugControls::VerboseTraceControl->IsEnabled(this->DebugControl, functionUnit))
      {
         DefinedData ^ blockDefined = safe_cast<DefinedData ^>(this->GetBlockData(block));

         Output::Write(L"==== BasicBlock ");
         Output::WriteLine(Tiled::Output::ToString(block->Id));
         Output::Write(L"In\t: ");
         blockDefined->InBitVector->Dump();
         Output::Write(L"Out\t: ");
         blockDefined->OutBitVector->Dump();
         Output::Write(L"Generate\t: ");
         blockDefined->GenerateBitVector->Dump();
         Output::Write(L"Kill\t: ");
         blockDefined->KillBitVector->Dump();
      }
   }
   next_block_in_func;
}

#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Determines whether the operand is tracked by Defined information.
//
// Arguments:
//
//    operand - determine if this operand is tracked by the walker.
//
// Remarks:
//
//    This walker only tracks registers.
//
//------------------------------------------------------------------------------

bool
RegisterDefinedWalker::IsTracked
(
   const llvm::MachineOperand * operand
)
{
   return (operand->isReg() /*|| operand->IsRegisterAlias*/) && DefinedWalker::IsTracked(operand);

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Create a new RegisterDefinedWalker object.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//    function - Function for dataflow analysis
//
//------------------------------------------------------------------------------

RegisterDefinedWalker *
RegisterDefinedWalker::New()
{
   Dataflow::RegisterDefinedWalker * registerDefinedWalker = new RegisterDefinedWalker();

   return registerDefinedWalker;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    DefinedData constructor.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//
// Returns:
//
//    New DefinedData object.
//
//------------------------------------------------------------------------------

DefinedData *
DefinedData::New()
{
   // Allocate all bit vectors the data contains

   DefinedData * data = new DefinedData;

   data->InBitVector = new llvm::SparseBitVector<>();
   data->OutBitVector = new llvm::SparseBitVector<>();
   data->GenerateBitVector = new llvm::SparseBitVector<>();
   data->KillBitVector = new llvm::SparseBitVector<>();

   return data;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for Defined variables block data
//
//------------------------------------------------------------------------------

void
DefinedData::Delete()
{
   // Delete all bit vectors the data contains

   delete this->InBitVector;
   delete this->OutBitVector;
   delete this->GenerateBitVector;
   delete this->KillBitVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Merge defined out from predeccessors into this block's defined in.
//
// Arguments:
//
//    baseDependencyData - Dependent data
//    baseBlockData - Block data
//    flags - Merge flags
//
// Remarks:
//
//    If this is the first predeccessor, we can simply copy.
//
//------------------------------------------------------------------------------

void
DefinedData::Merge
(
   Data *     baseDependencyData,
   Data *     baseBlockData,
   Xcessor    incomingEdge,
   MergeFlags flags
)
{
   DefinedData * dependencyData = static_cast<DefinedData *>(baseDependencyData);  //dynamic_cast needed if > 1 derived class

   if ((unsigned(flags) & unsigned(MergeFlags::First)) != unsigned(MergeFlags::None)) {
      *(this->InBitVector) = *(dependencyData->OutBitVector);
   } else {
      *(this->InBitVector) |= *(dependencyData->OutBitVector);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determine if this and that have the same in set.
//
// Arguments:
//
//    baseBlockData - Block data
//
// Returns:
//
//    True if in sets are identical, false otherwise.
//
//------------------------------------------------------------------------------

bool
DefinedData::SamePrecondition
(
   Data * baseBlockData
)
{
   DefinedData * blockData = static_cast<DefinedData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData != nullptr);
   return (*(this->InBitVector) == *(blockData->InBitVector));
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determine if this and blockData have the same out set.
//
// Arguments:
//
//    baseBlockData - Block data
//
// Returns:
//
//    True if out sets are identical, false otherwise.
//
//------------------------------------------------------------------------------

bool
DefinedData::SamePostCondition
(
   Data * baseBlockData
)
{
   DefinedData * blockData = static_cast<DefinedData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData != nullptr);

   return (*(this->OutBitVector) == *(blockData->OutBitVector));
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update this block's data from temporaryData.
//
// Arguments:
//
//    baseTemporaryData - Temporary data
//
//------------------------------------------------------------------------------

void
DefinedData::Update
(
   Data * baseTemporaryData
)
{
   DefinedData * temporaryData = static_cast<DefinedData *>(baseTemporaryData);  //dynamic_cast needed if > 1 derived class
   assert(temporaryData != nullptr);

   // We simply swap the live in and live out bit vectors.

   llvm::SparseBitVector<> * bitVectorSwap;
   bitVectorSwap = this->InBitVector;
   this->InBitVector = temporaryData->InBitVector;
   temporaryData->InBitVector = bitVectorSwap;

   bitVectorSwap = this->OutBitVector;
   this->OutBitVector = temporaryData->OutBitVector;
   temporaryData->OutBitVector = bitVectorSwap;
}

} // namespace Dataflow
} // namespace Tiled
