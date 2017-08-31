//===-- Dataflow/LivenessWalker.cpp -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This module contains a set of backwards liveness data flow computations 
// for variables of various storage assignments.
//
//===----------------------------------------------------------------------===//

#include "Traverser.h"
#include "Liveness.h"
#include "llvm/Target/TargetOpcodes.h"

#define DEBUG_TYPE "tiled-liveness"

namespace Tiled
{
namespace Dataflow
{

//------------------------------------------------------------------------------
//
// Description:
//
//    LivenessData constructor.
//
// Remarks:
//
//    Allocates an initializes a LivenessData object.
//
// Returns:
//
//    New LivenessData object.
//
//------------------------------------------------------------------------------

LivenessData *
LivenessData::New()
{
   LivenessData * data = new LivenessData();
   data->LiveOutBitVector = new llvm::SparseBitVector<>();
   data->LiveInBitVector = new llvm::SparseBitVector<>();
   data->GenerateBitVector = new llvm::SparseBitVector<>();
   data->KillBitVector = new llvm::SparseBitVector<>();

   return data;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete this LivenessData object.
//
//------------------------------------------------------------------------------

void
LivenessData::Delete()
{
   delete this->LiveInBitVector;
   delete this->LiveOutBitVector;
   delete this->GenerateBitVector;
   delete this->KillBitVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Merge live in from successors into this block's live out.
//
// Arguments:
//
//    baseDependencyData - Dependent data
//    baseBlockData - Block data
//    flags - Merge flags
//
// Remarks:
//
//    If this is the first successor, we can simply copy.
//
//------------------------------------------------------------------------------

void
LivenessData::Merge
(
   Data *     baseDependencyData,
   Data *     baseBlockData,
   Xcessor    incomingEdge,
   MergeFlags flags
)
{
   LivenessData * dependencyData = static_cast<LivenessData *>(baseDependencyData);  //dynamic_cast needed if > 1 derived class
   assert(dependencyData);

   if ((unsigned(flags) & unsigned(MergeFlags::First)) == unsigned(MergeFlags::First)) {
      *(this->LiveOutBitVector) = *(dependencyData->LiveInBitVector);
   } else {
      *(this->LiveOutBitVector) |= *(dependencyData->LiveInBitVector);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determine if this and that have the same live out set.
//
// Arguments:
//
//    baseBlockData - Block data
//
// Returns:
//
//    true if so, false otherwise.
//
//------------------------------------------------------------------------------

bool
LivenessData::SamePrecondition
(
   Data * baseBlockData
)
{
   LivenessData * blockData = static_cast<LivenessData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData);

   return ((*this->LiveOutBitVector) == (*blockData->LiveOutBitVector));
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determine if this and blockData have the same live in set.
//
// Arguments:
//
//    baseBlockData - Block data
//
// Returns:
//
//    true if so, false otherwise.
//
//------------------------------------------------------------------------------

bool
LivenessData::SamePostCondition
(
   Data * baseBlockData
)
{
   LivenessData * blockData = static_cast<LivenessData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData);

   return ((*this->LiveInBitVector) == (*blockData->LiveInBitVector));
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
LivenessData::Update
(
   Data * baseTemporaryData
)
{
   LivenessData *      temporaryData = static_cast<LivenessData *>(baseTemporaryData);  //dynamic_cast needed if > 1 derived class
   assert(temporaryData);
   llvm::SparseBitVector<> * bitVectorSwap;

   // We simply swap the live in and live out bit vectors.
   bitVectorSwap = this->LiveInBitVector;
   this->LiveInBitVector = temporaryData->LiveInBitVector;
   temporaryData->LiveInBitVector = bitVectorSwap;

   bitVectorSwap = this->LiveOutBitVector;
   this->LiveOutBitVector = temporaryData->LiveOutBitVector;
   temporaryData->LiveOutBitVector = bitVectorSwap;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Perform static initialization of the LivenessWalker class.
//
// Remarks:
//
//    We create a debug flag.
//
//------------------------------------------------------------------------------

void
LivenessWalker::StaticInitialize()
{
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete an instance of the LivenessWalker class.
//
//------------------------------------------------------------------------------

void
LivenessWalker::Delete()
{
   Walker::Delete();
   if (this->NotTrackedTagBitVector != nullptr) {
      delete this->NotTrackedTagBitVector;
   }
   if (this->ScratchBitVector != nullptr) {
      delete this->ScratchBitVector;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute backwards liveness for the specified function.
//
// Arguments:
//
//    functionUnit - Function to process
//
//------------------------------------------------------------------------------

void
LivenessWalker::ComputeLiveness
(
   Graphs::FlowGraph*  functionUnit
)
{
   // Compute liveness.

   this->Initialize(Dataflow::Direction::Backward, functionUnit);
   this->Traverse(TraversalKind::Iterative, functionUnit);

   // compute the set of things we are tracking.

   this->NotTrackedTagBitVector = new llvm::SparseBitVector<>();

   this->IsIncomplete = false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update the Liveness information to reflect that the new tag is not tracked
//
// Arguments:
//
//    aliasInfo - the Alias::Info that owns the Alias::Tag.
//    tag - the tag we are not tracking
//
//------------------------------------------------------------------------------

void
LivenessWalker::MarkNotTracked
(
   Tiled::VR::Info *     vrInfo,
   unsigned              tag
)
{
   assert(this->NotTrackedTagBitVector != nullptr);

   vrInfo->OrMemberTags(tag, this->NotTrackedTagBitVector);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update the Liveness information to reflect that the given tag is Live-In
//    to the block
//
// Arguments:
//
//    basicBlock - the block to add tag to the Live-In set for
//    tag - the tag we want to make Live-In
//
//------------------------------------------------------------------------------

void
LivenessWalker::AddToLiveIn
(
   llvm::MachineBasicBlock * basicBlock,
   unsigned                  tag
)
{
   Tiled::VR::Info * vrInfo = this->vrInfo;
   LivenessData * blockData = static_cast<LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class
   assert(blockData);

   if (blockData == nullptr) {
      return;
   }

   vrInfo->OrMemberTags(tag, blockData->LiveInBitVector);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update the Liveness information to reflect that the given tag is Live-Out
//    to the block
//
// Arguments:
//
//    basicBlock - the block to add tag to the Live-Out set for
//    tag - the tag we want to make Live-Out
//
//------------------------------------------------------------------------------

void
LivenessWalker::AddToLiveOut
(
   llvm::MachineBasicBlock * basicBlock,
   unsigned                  tag
)
{
   Tiled::VR::Info * vrInfo = this->vrInfo;
   LivenessData * blockData = static_cast<LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class
   assert(blockData);

   if (blockData == nullptr) {
      return;
   }

   vrInfo->OrMemberTags(tag, blockData->LiveOutBitVector);
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Determines whether the passed tag is tracked by Liveness information. If
//    not the information returned by LiveAfter and LiveOut API's will be
//    unreliable.
//
// Arguments:
//
//    aliasInfo - the Alias::Info that owns the Alias::Tag.
//    tag - the tag we are interested in examining the liveness data for
//    operand - determine if this operand is tracked by the walker.
//
// Remarks:
//
//    The protected overload that takes an Alias::Tag simply checks the tag
//    against those tags registered by calling MarkNotTracked.
//
//    The public overload that takes an operand checks the operand's alias
//    tag and overloads of this function may check other things (such
//    as expression temporary-ness or register vs. memory).
//
//------------------------------------------------------------------------------

bool
LivenessWalker::IsTracked
(
   Tiled::VR::Info *    vrInfo,
   unsigned             tag
)
{
   if (this->NotTrackedTagBitVector == nullptr) {
      return true;
   }

   return !vrInfo->CommonMayPartialTags(tag, this->NotTrackedTagBitVector);
}

bool
LivenessWalker::IsTracked
(
   const llvm::MachineOperand * operand
)
{
   unsigned tag = this->vrInfo->GetTag(operand->getReg());
   return this->IsTracked(this->vrInfo, tag);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate the data flow data.
//
// Arguments:
//
//    numberElements - Number of data elements
//
//------------------------------------------------------------------------------

void
LivenessWalker::AllocateData
(
   unsigned numberElements
)
{
   BlockData*  dataArray;

   dataArray = new BlockData(numberElements, nullptr);

   // Initialize the data.

   for (unsigned i = 0; i < numberElements; i++)
   {
      (*dataArray)[i] = LivenessData::New();
   }

   this->BlockDataArray = dataArray;
}



bool
isNonRenamablePhi(const llvm::MachineInstr *)
{
   //TBD:
   return false;
}

bool
isDanglingInstr(llvm::MachineInstr *)
{
   //TBD:
   return false;
}

bool
isDanglingDefinition(llvm::MachineOperand * destinationOperand)
{
   //TBD:
   return false;
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Compute liveness for the specified block by walking the instructions.
//
// Arguments:
//
//    block - Block to evaluate
//    baseTemporaryData - Temporary data
//
// Remarks:
//
//    We compute live in and the summary gen and kill sets.  ExprTmps
//    are ignored since they are block lifetime.
//
//------------------------------------------------------------------------------

void
LivenessWalker::EvaluateBlock
(
   llvm::MachineBasicBlock * block,
   Dataflow::Data *          baseTemporaryData
)
{
   LivenessData * temporaryData = static_cast<LivenessData *>(baseTemporaryData);  //dynamic_cast needed if > 1 derived class
   LivenessData * blockData = static_cast<LivenessData *>(this->GetBlockData(block));
   assert(temporaryData && blockData);

   DEBUG({
      llvm::dbgs() << "\nMBB#" << block->getNumber() << "   (liveness)\n";
      llvm::SparseBitVector<>::iterator a;
      llvm::dbgs() << "  LiveOut:   {";
      for (a = temporaryData->LiveOutBitVector->begin(); a != temporaryData->LiveOutBitVector->end(); ++a) {
         llvm::dbgs() << *a << ", ";
      }
      llvm::dbgs() << "}\n";
   });

   if (blockData->MustEvaluate) {
      // Compute gen and kill sets.

      llvm::MachineBasicBlock::reverse_instr_iterator rii;
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         const llvm::MachineInstr * instruction = &(*rii);
   
         DEBUG(instruction->dump());

         /* instruction was a CHI or a non-renamable PHI */
         if (instruction->isPHI() && /*TBD: */ isNonRenamablePhi(instruction)) {
            // The CHI instruction represents a definition however a renameable PHI node may
            // represent uses that may be manifested into real assignments during color-out.
            continue;
         }

         // Don't transfer dangling definitions on the last instruction in the
         // block; these are considered part of the unique non-EH successor.
         this->TransferDestinations(instruction, blockData->GenerateBitVector, blockData->KillBitVector);

         // Always transfer source liveness
         this->TransferSources(instruction, blockData->GenerateBitVector, blockData->KillBitVector);

         DEBUG({
            llvm::SparseBitVector<>::iterator a;
            llvm::dbgs() << "    Generate:  {";
            for (a = blockData->GenerateBitVector->begin(); a != blockData->GenerateBitVector->end(); ++a) {
               llvm::dbgs() << *a << ", ";
            }
            llvm::dbgs() << "}\n";
            llvm::dbgs() << "    Kill:      {";
            for (a = blockData->KillBitVector->begin(); a != blockData->KillBitVector->end(); ++a) {
               llvm::dbgs() << *a << ", ";
            }
            llvm::dbgs() << "}\n";
         });
      }
   }

   // Compute LiveInBitVector = (LiveOutBitVector - Kill) + Generate.
   temporaryData->CalculateDataflow(temporaryData->LiveOutBitVector,
                                    blockData->KillBitVector,
                                    blockData->GenerateBitVector);

   DEBUG({
      llvm::SparseBitVector<>::iterator a;
      llvm::dbgs() << "  LiveIn:    {";
      for (a = temporaryData->LiveInBitVector->begin(); a != temporaryData->LiveInBitVector->end(); ++a) {
         llvm::dbgs() << *a << ", ";
      }
      llvm::dbgs() << "}\n\n";
   });
}

void
LivenessData::CalculateDataflow
(
   const llvm::SparseBitVector<> * liveBitVector,
   const llvm::SparseBitVector<> * killBitVector,
   const llvm::SparseBitVector<> * generateBitVector
)
{
   (*this->LiveInBitVector) = (*liveBitVector - *killBitVector) | *generateBitVector;
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Mark the "IsLastUse" property on each source operand in the function.
//
// Remarks:
//
//    An operand marked as "IsLastUse" is one for which the variables that it
//    references all go dead after its instruction executes.
//
// Arguments:
//
//    functionUnit - Function to process
//
//-------------------------------------------------------------------------------

#ifdef FUTURE_IMPL
// This function is never called in the current code but may be useful in the future.
void
LivenessWalker::MarkLastUseOperands
(
   Graphs::FlowGraph * functionUnit
)
{
   Alias::Info ^            aliasInfo = this->AliasInfo;
   Dataflow::LivenessData * temporaryData = static_cast<Dataflow::LivenessData *>(this->TemporaryData);  //dynamic_cast needed if > 1 derived class
   assert(temporaryData);
   BitVector::Sparse ^      liveBitVector = temporaryData->GenerateBitVector;
   Dataflow::LivenessData ^ blockData;

   foreach_block_in_func(basicBlock, functionUnit)
   {
      // Start with the set of live tags out of this basic block. This will
      // include anything that is live along an exception edge on the block's
      // last instruction.

      blockData = static_cast<Dataflow::LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class
      assert(blockData);
      liveBitVector->CopyBits(blockData->LiveOutBitVector);

      foreach_instr_in_block_backward(instruction, basicBlock)
      {
         if (!instruction->IsSsa)
         {
            // Process the destination operands by removing the set of live tags.
            // Never transfer dangling definitions since they only occur on
            // dangling instructions that end blocks, and the live-out bit-vector
            // that we used to seed the live set already represents the execution
            // point prior to the dangling definitions executing.

            this->TransferDestinations(instruction, liveBitVector, temporaryData->KillBitVector);

            foreach_dataflow_source_opnd(sourceOperand, instruction)
            {
               if (sourceOperand->IsExpressionTemporary)
               {
                  // Expression temporaries are implicitly last use.

                  continue;
               }

               if (this->IsTracked(sourceOperand))
               {
                  // This operand is a last use if there are no bits that it references live
                  // after this instruction executes.

                  sourceOperand->IsLastUse = !aliasInfo->CommonMayPartialTags(sourceOperand->AliasTag,
                     liveBitVector);

                  continue;
               }

               sourceOperand->IsLastUse = false;
            }
            next_dataflow_source_opnd;

            // Process the source operands by adding to the set of live tags.

            this->TransferSources(instruction, liveBitVector, temporaryData->KillBitVector);
         }
      }
      next_instr_in_block_backward;
   }
   next_block_in_func;
}
#endif

//-------------------------------------------------------------------------------
//
// Description:
//
//   Construct the set of live values after a given instruction.  If
//   global information is built then this is a global view.
//   Otherwise information is local to the block.
//
// Parameters:
//
//   afterInstruction - instruction after which liveness is computed.
//
// Remarks:
//
//   Performs backwards walk of the instruction's enclosing basic block
//   from the last instruction to afterInstruction->Next. Live values
//   are then computed by the equasion: 
//
//      LiveAfter = (LiveOut - Kill) + Generate
//
// Returns:
//
//   Set of alias tags representing the resources live after the
//   passed instruction (afterInstruction)
//
//-------------------------------------------------------------------------------

llvm::SparseBitVector<> *
LivenessWalker::LiveAfter
(
   llvm::MachineInstr * afterInstruction
)
{
   llvm::MachineBasicBlock * block = afterInstruction->getParent();

   // <place for EH-flow-related code>

   LivenessData *    blockData = static_cast<LivenessData *>(this->GetBlockData(block));  //dynamic_cast needed if > 1 derived class
   if (blockData == nullptr) {
      assert(this->GetBlockData(block) == nullptr);
      // If we have no block data, return a null vector to signify we have no data.
      return nullptr;
   }

   LivenessData *       temporaryData = static_cast<LivenessData *>(this->TemporaryData);  //dynamic_cast needed if > 1 derived class
   assert(temporaryData);
   llvm::MachineInstr * lastInstruction = &(block->instr_back());

   // anything in our live out or "Not Tracked" set is treated as live out of the block.
   *(temporaryData->GenerateBitVector) = *(blockData->LiveOutBitVector);
   *(temporaryData->GenerateBitVector) |= *(this->NotTrackedTagBitVector);

   if (afterInstruction != lastInstruction) {
      temporaryData->KillBitVector->clear();

      // for each instruction from end of block to instruction
      // backwards transfer dsts and srcs

      assert(afterInstruction->getNextNode() != nullptr);

      llvm::MachineInstr * instruction = lastInstruction;
      for ( ; instruction && instruction != afterInstruction; instruction = instruction->getPrevNode())
      {
         // inactive in the current implementation
         if (instruction->isPHI()) {
            continue;
         }

         //  - process kills
         this->TransferDestinations(instruction,
            temporaryData->GenerateBitVector, temporaryData->KillBitVector);

         //  - process gens
         this->TransferSources(instruction,
            temporaryData->GenerateBitVector, temporaryData->KillBitVector);
      }
   }

   // return "live in vector" this is the set of live values at
   // the point immediatly after the instruction

   return temporaryData->GenerateBitVector;
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Determine whether the given alias tag is live after a given point
//    in the IR - i.e. it is live after this instruction.
//
// Parameters:
//
//    aliasTag - alias tag of the resource we're checking
//
// Remarks:
//
//    Computes set of live tags after the given instruction and tests
//    past tag for membership in that set.
//
// Returns:
//
//    TRUE if the alias tag resource is live after the given instruction
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveAfter
(
   unsigned             tag,
   llvm::MachineInstr * afterInstruction
)
{
   // Find preserved set and test whether tag is represented.

   llvm::SparseBitVector<> * liveAfter = this->LiveAfter(afterInstruction);

   return (liveAfter == nullptr) || vrInfo->CommonMayPartialTags(tag, liveAfter);
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Determine whether the given operand is live after a given point
//    in the IR - i.e. it is live after this instruction.
//
// Parameters:
//
//    operand - operand representing the resource we're checking
//
// Remarks:
//
//    Re-calls 'IsLiveAfter()' with operands alias tag.  See alias tag
//    version of 'IsLiveAfter()' for further comments.
//
// Returns:
//
//    TRUE if the alias tag resource is live after the given instruction
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveAfter
(
   llvm::MachineOperand * operand,
   llvm::MachineInstr *   afterInstruction
)
{
   assert(operand->isReg());

   Tiled::VR::Info * vrInfo = this->vrInfo;
   unsigned tag = vrInfo->GetTag(operand->getReg());

   // call 'IsLiveAfter()' with operand's alias tag.
   return this->IsLiveAfter(tag, afterInstruction);
}

//-------------------------------------------------------------------------------
//
// Description:
//
//   Check whether the alias tag is live out of the block
//
// Arguments:
//
//   aliasTag - the resource under consideration
//   basicBlock - basic block in the flow graph to test live out.
//
// Return:
//
//   true if the alias tag is live out.
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveOut
(
   unsigned                  tag,
   llvm::MachineBasicBlock * basicBlock
)
{
   Dataflow::LivenessData * livenessData = static_cast<Dataflow::LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class

   if (livenessData == nullptr) {
      assert(this->GetBlockData(basicBlock) == nullptr);
      return true;
   }

   Tiled::VR::Info *         vrInfo = this->vrInfo;
   llvm::SparseBitVector<> * currentLiveSet = livenessData->LiveOutBitVector;

   return !this->IsTracked(vrInfo, tag) || vrInfo->CommonMayPartialTags(tag, currentLiveSet);
}
//-------------------------------------------------------------------------------
//
// Description:
//
//   Check whether the resource represented by the operand is live out
//   of the block
//
// Arguments:
//
//   operand    - the resource under consideration
//   basicBlock - basic block in the flow graph to test live out.
//
// Return:
//
//   true if the operand is live out.
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveOut
(
   llvm::MachineOperand    * operand,
   llvm::MachineBasicBlock * basicBlock
)
{
   assert(operand->isReg());

   Tiled::VR::Info *  vrInfo = this->vrInfo;
   unsigned tag = vrInfo->GetTag(operand->getReg());
   return !this->IsTracked(vrInfo, tag) || this->IsLiveOut(tag, basicBlock);
}

//-------------------------------------------------------------------------------
//
// Description:
//
//   Determine the set of alias tags live into the given basic block
//
// Arguments:
//
//   basicBlock - basic block in the flow graph to test live in.
//
// Return:
//
//   the set of tags live into the block in a freshly allocated bit vector
//
//-------------------------------------------------------------------------------

llvm::SparseBitVector<> *
LivenessWalker::LiveIn
(
   llvm::MachineBasicBlock * basicBlock
)
{
   Dataflow::LivenessData * livenessData = static_cast<Dataflow::LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class

   if (livenessData == nullptr) {
      assert(this->GetBlockData(basicBlock) == nullptr);
      return nullptr;
   }

   llvm::SparseBitVector<> * liveIn = new llvm::SparseBitVector<>( *(livenessData->LiveInBitVector) );

   *liveIn |= *(this->NotTrackedTagBitVector);

   return liveIn;
}

//-------------------------------------------------------------------------------
//
// Description:
//
//   Check whether the alias tag is live into the block
//
// Arguments:
//
//   aliasTag - the resource under consideration
//   basicBlock - basic block in the flow graph to test live in.
//
// Return:
//
//   true if the alias tag is live in.
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveIn
(
   unsigned                  vrTag,
   llvm::MachineBasicBlock * basicBlock
)
{
   Dataflow::LivenessData * livenessData = static_cast<Dataflow::LivenessData *>(this->GetBlockData(basicBlock));  //dynamic_cast needed if > 1 derived class

   if (livenessData == nullptr) {
      assert(this->GetBlockData(basicBlock));
      return true;
   }

   Tiled::VR::Info *          vrInfo         = this->vrInfo;
   llvm::SparseBitVector<> *  currentLiveSet = livenessData->LiveInBitVector;

   return !this->IsTracked(vrInfo, vrTag) || vrInfo->CommonMayPartialTags(vrTag, currentLiveSet);
}

//-------------------------------------------------------------------------------
//
// Description:
//
//   Check whether the resource represented by the operand is live
//   into the block
//
// Arguments:
//
//   operand    - the resource under consideration
//   basicBlock - basic block in the flow graph to test live in.
//
// Return:
//
//   true if the operand is live in.
//
//-------------------------------------------------------------------------------

bool
LivenessWalker::IsLiveIn
(
   llvm::MachineOperand *    operand,
   llvm::MachineBasicBlock * basicBlock
)
{
   if (!operand->isReg())
      return false;

   Tiled::VR::Info * vrInfo = this->vrInfo;
   unsigned tag = vrInfo->GetTag(operand->getReg());
   return this->IsLiveIn(tag, basicBlock);
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for the RegisterLivenessWalker class.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//
// Returns:
//
//    RegisterLivenessWalker object.
//
//-------------------------------------------------------------------------------

RegisterLivenessWalker *
RegisterLivenessWalker::New()
{
   RegisterLivenessWalker *  walker = new RegisterLivenessWalker();

   walker->IsIncomplete = true;
   walker->callKilledRegBitVector = new llvm::SparseBitVector<>();

   return walker;
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Delete this RegisterLivenessWalker
//
//-------------------------------------------------------------------------------

void
RegisterLivenessWalker::Delete()
{
   LivenessWalker::Delete();
}

//-------------------------------------------------------------------------------
//
// Description:
//
//    Determines whether the operand is tracked by Liveness information.
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
RegisterLivenessWalker::IsTracked
(
   const llvm::MachineOperand * operand
)
{
   return (operand->isReg() /*|| operand->IsRegisterAlias*/) && LivenessWalker::IsTracked(operand);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update current bit vectors with liveness sideeffects from this
//    instruction's destinations
//
// Arguments:
//
//   instruction - transfer instruction destination liveness
//   generateBitVector - bitvector of generated live tags
//   killBitVector - bitvector of killed live tags
//   doTransferDangling - true to include dangling definitions in processing
//   doTransferNonDangling - true to include non-dangling definitions in processing
//
// Remarks:
//
//   Updates GEN and KILL - removing total definitions from the GEN
//   set and adding the same total defs to KILL. (for tempraries)
//
// Returns:
//
//   True if liveness was changed (defined as any update to the
//   current BitVectors)
//
//------------------------------------------------------------------------------

bool
RegisterLivenessWalker::TransferDestinations
(
   const llvm::MachineInstr* instruction,
   llvm::SparseBitVector<>*  generateBitVector,
   llvm::SparseBitVector<>*  killBitVector
)
{
   Tiled::VR::Info* vrInfo = this->vrInfo;
   bool  hasDataflowDestination = false;

   // Add into the KILL set any totally overlapped must-def tag aliases.

   llvm::iterator_range<llvm::MachineInstr::const_mop_iterator> range(instruction->operands());
   llvm::MachineInstr::const_mop_iterator destinationOperand;

   // foreach_destination_opnd
   for (destinationOperand = range.begin(); destinationOperand != range.end(); ++destinationOperand)
   {
      if (!this->IsTracked(destinationOperand) || !destinationOperand->isDef()) {  //TODO: filter flag registers?
         continue;
      }

      unsigned vrTag = vrInfo->GetTag(destinationOperand->getReg());
      vrInfo->MinusMustTotalTags(vrTag, generateBitVector);
      vrInfo->OrMustTotalTags(vrTag, killBitVector);
      hasDataflowDestination = true;
   }

   if (instruction->isCall()) {
      for (unsigned i = 0, e = instruction->getNumOperands(); i != e; ++i) {
         const llvm::MachineOperand& mo = instruction->getOperand(i);
         if (mo.isReg() && (mo.isKill() || mo.isDef())) {
            unsigned rTag = vrInfo->GetTag(mo.getReg());

            vrInfo->MinusMustTotalTags(rTag, generateBitVector);
            vrInfo->OrMustTotalTags(rTag, killBitVector);
            hasDataflowDestination = true;
         }
      }

      if (this->callKilledRegBitVector->empty()) {
         llvm::MachineFunction * MF = this->FunctionUnit->machineFunction;
         const llvm::TargetRegisterInfo * TRI = MF->getSubtarget().getRegisterInfo();
         unsigned vectorSize = ((TRI->getNumRegs() + 31) / 32) * 32;

         llvm::BitVector callPreservedBitVector(vectorSize);
         callPreservedBitVector.setBitsInMask(TRI->getCallPreservedMask(*MF, llvm::CallingConv::Fast));

         llvm::BitVector callKilledRegBitVector(vectorSize);
         callKilledRegBitVector = TRI->getAllocatableSet(*MF);
         callKilledRegBitVector.reset(Tiled::NoReg);

         callKilledRegBitVector.reset(callPreservedBitVector);  // X &= ~Y

         for (int reg = callKilledRegBitVector.find_first(); reg != -1; reg = callKilledRegBitVector.find_next(reg))
         {
            unsigned tag = vrInfo->GetTag(reg);
            this->callKilledRegBitVector->set(tag);
         }
      }

      *killBitVector |= *(this->callKilledRegBitVector);
   }

   return hasDataflowDestination;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update current bit vectors with liveness sideeffects from this
//    instruction's sources
//
// Arguments:
//
//   instruction - transfer instruction source liveness
//   blockData   - block data structure containing current bit vectors
//
// Remarks:
//
//   Add May Total tags for temporaries to GEN set.
//
// Returns:
//
//   True if liveness was changed (defined as any update to the
//   current BitVectors)
//
//------------------------------------------------------------------------------

bool
RegisterLivenessWalker::TransferSources
(
   const llvm::MachineInstr* instruction,
   llvm::SparseBitVector<>* generateBitVector,
   llvm::SparseBitVector<>* killBitVector
)
{
   Tiled::VR::Info*      vrInfo = this->vrInfo;
   bool       hasDataflowSource = false;

   llvm::MachineInstr::const_mop_iterator firstSource(instruction->uses().begin());
   llvm::MachineInstr::const_mop_iterator lastOperand(instruction->operands().end());
   llvm::MachineInstr::const_mop_iterator sourceOperand;

   // foreach_source_opnd
   for (sourceOperand = firstSource; sourceOperand != lastOperand; ++sourceOperand)
   {
      if (this->IsTracked(sourceOperand) && sourceOperand->isUse() && !sourceOperand->isUndef()) {
         unsigned vrTag = vrInfo->GetTag(sourceOperand->getReg());
         hasDataflowSource = true;
         vrInfo->OrMemberTags(vrTag, generateBitVector);
      }
   }

   return hasDataflowSource;
}

} // namespace Dataflow

} // namespace Tiled

