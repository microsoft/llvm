//===-- Dataflow/AvailableDefinitions.cpp -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AvailableDefinitions.h"

namespace Tiled
{

namespace Dataflow
{

//------------------------------------------------------------------------------
//
// Description:
//
//    Static initialization of the AvailableDefinitionWalker class.
//
//------------------------------------------------------------------------------

void
AvailableDefinitionWalker::StaticInitialize()
{

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Create a new AvailableDefinitionWalker object.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//    function - Function for dataflow analysis
//
//------------------------------------------------------------------------------

AvailableDefinitionWalker *
AvailableDefinitionWalker::New()
{
   AvailableDefinitionWalker * availableDefinitionWalker = new AvailableDefinitionWalker;

   return availableDefinitionWalker;
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
AvailableDefinitionWalker::AllocateData
(
   unsigned numberElements
)
{
   BlockData*                          dataArray;
   Dataflow::AvailableDefinitionData * availableDefinitionData;

   dataArray = new BlockData(numberElements, nullptr);

   // Initialize the data.

   for (unsigned i = 0; i < numberElements; i++)
   {
      (*dataArray)[i] = AvailableDefinitionData::New();
   }

   availableDefinitionData = static_cast<AvailableDefinitionData *>((*dataArray)[0]);  //dynamic_cast needed if > 1 derived class
   assert(availableDefinitionData != nullptr);
   availableDefinitionData->WasVisited = true;
   availableDefinitionData = static_cast<AvailableDefinitionData *>((*dataArray)[1]);  //dynamic_cast needed if > 1 derived class
   assert(availableDefinitionData != nullptr);
   availableDefinitionData->WasVisited = true;

   this->BlockDataArray = dataArray;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Main routine of AvailableDefinitionWalker computation. Evaluate each basic block.
//    If it is the first time for a block, or if there is anything changed
//    and it is marked as MustEvaluate, recompute the in/out vector.
//
//    Compute the available definitions with:
//
//          IN = INTERSECTION(previous OUTs)
//          OUT = (IN - KILL) + GEN
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
AvailableDefinitionWalker::EvaluateBlock
(
   llvm::MachineBasicBlock * block,
   Data *                    baseTemporaryData
)
{
   AvailableDefinitionData * temporaryData;
   AvailableDefinitionData * blockData;

   temporaryData = static_cast<AvailableDefinitionData *>(baseTemporaryData);  //dynamic_cast needed if > 1 derived class
   blockData = static_cast<AvailableDefinitionData *>(this->GetBlockData(block));
   assert(temporaryData && blockData);

   llvm::SparseBitVector<> * outBitVector = temporaryData->AvailableOutBitVector;
   llvm::SparseBitVector<> * inBitVector = temporaryData->AvailableInBitVector;
   llvm::SparseBitVector<> * generateBitVector = blockData->AvailableGenerateBitVector;
   llvm::SparseBitVector<> * killBitVector = blockData->AvailableKillBitVector;

   if (blockData->MustEvaluate) {
      // Compute gen and kill sets.

      generateBitVector->clear();
      killBitVector->clear();

      llvm::MachineBasicBlock::instr_iterator(ii);
      for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
      {
         llvm::MachineInstr * instruction = &(*ii);
         this->EvaluateInstruction(instruction, generateBitVector, killBitVector);
      }
   }

   // Compute Out = (In - Kill) + Generate. 
   // This is update which can trigger iterative walks if temporaryData->AvailableOutBitVector
   // changes compared to what is saved in the block data.

   *outBitVector = *inBitVector;
   *outBitVector = (*outBitVector - *killBitVector);
   *outBitVector |= *generateBitVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determines whether the operand is tracked by AvailableDefinition information.
//
// Arguments:
//
//    operand - determine if this operand is tracked by the walker.
//
//------------------------------------------------------------------------------

inline
bool
AvailableDefinitionWalker::IsTracked
(
   llvm::MachineOperand * operand
)
{
#if 0
   //TODO:   ??
   return (operand->AliasTag != Alias::Constants::InvalidTag);
#else
   return false;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get definition kill bit vector for given resource alias tag
//
// Returns:
//
//    Definition kill bitvector if one exists, otherwise returns null.
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
AvailableDefinitionWalker::GetDefinitionKillBitVector
(
   unsigned resourceAliasTag
)
{
#if 0
   TagToSparseBitVectorMap *  definitionKillBitVectorMap;
   llvm::SparseBitVector<> *  definitionKillBitVector;
   llvm::SparseBitVector<> *  nullDefinitionKillBitVector;
   llvm::SparseBitVector<> *  scratchKillBitVector;

   definitionKillBitVectorMap = this->DefinitionKillBitVectorMap;
   nullDefinitionKillBitVector = this->NullDefinitionKillBitVector;
   scratchKillBitVector = this->ScratchKillBitVector;

   // Look up definition kill bit vector base on resource alias tag (register or memory)

   definitionKillBitVector = (*definitionKillBitVectorMap)[resourceAliasTag];
   if (definitionKillBitVector != nullptr) {
      return definitionKillBitVector;
   }

   // Otherwise, insert new definition kill vector for resourceAliasTag.

   VR::Info *                vrInfo = this->vrInfo;

   assert(nullDefinitionKillBitVector->empty());

   // Walk definitions looking for those that would be killed by this resource changing.
   // Use null kill vector as temporary kill vector.

   scratchKillBitVector->clear();
   if (vrInfo->IsRegisterTag(resourceAliasTag)) {
      vrInfo->OrMayPartialTags(resourceAliasTag, scratchKillBitVector);
   } else {
      vrInfo->OrMustTotalTags(resourceAliasTag, scratchKillBitVector);
   }

   definitionKillBitVector = nullDefinitionKillBitVector;
   if (!scratchKillBitVector->empty()) {
      for (definitionId = 1; definitionId <= maxDefinitionId; definitionId++)
      {
         Alias::Tag definitionResourceAliasTag = debugInfo->GetResourceAliasTag(definitionId);

         if (scratchKillBitVector->GetBit(definitionResourceAliasTag))
         {
            definitionKillBitVector->SetBit(definitionId);
         }
      }
   }

   // If we used the null kill vector, copy it to a new kill vector and clear it.

   if (!nullDefinitionKillBitVector->IsEmpty)
   {
      Tiled::Lifetime ^ lifetime = this->Lifetime;

      definitionKillBitVector = TILED_NEW_SPARSE_BITVECTOR(lifetime);
      definitionKillBitVector->CopyBits(nullDefinitionKillBitVector);
      nullDefinitionKillBitVector->ClearAll();
   }

   // Insert new kill vector.

   definitionKillBitVectorMap->Insert(resourceAliasTag, definitionKillBitVector);

   // Return the definition kill vector.

   return definitionKillBitVector;
#else
   return nullptr;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Evaluate operand for available definitions.
//
//------------------------------------------------------------------------------

inline
void
AvailableDefinitionWalker::EvaluateOperand
(
   llvm::MachineOperand *    operand,
   llvm::SparseBitVector<> * generateBitVector,
   llvm::SparseBitVector<> * killBitVector
)
{
#if 0
   Assert(operand->IsDefinition);

   foreach_debug_alias_tag_cell(debugAliasTagCell, operand)
   {
      Alias::Tag          aliasTag = debugAliasTagCell->AliasTag;
      Tiled::Id             definitionNumber = debugAliasTagCell->DefinitionNumber;
      BitVector::Sparse ^ definitionKillBitVector;

      // Kill affected definitions

      definitionKillBitVector = this->GetDefinitionKillBitVector(aliasTag);
      if (!definitionKillBitVector->IsEmpty)
      {
         generateBitVector->Minus(definitionKillBitVector);
         killBitVector->Or(definitionKillBitVector);
      }

      // Generate definition

      if (definitionNumber != 0)
      {
         killBitVector->ClearBit(definitionNumber);
         generateBitVector->SetBit(definitionNumber);
      }
   }
   next_debug_alias_tag_cell;
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Evaluate instruction for available definitions.
//
//------------------------------------------------------------------------------

void
AvailableDefinitionWalker::EvaluateInstruction
(
   llvm::MachineInstr *      instruction,
   llvm::SparseBitVector<> * generateBitVector,
   llvm::SparseBitVector<> * killBitVector
)
{
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> range(instruction->defs());
   llvm::MachineInstr::mop_iterator destinationOperand;
   for (destinationOperand = range.begin(); destinationOperand != range.end(); ++destinationOperand)
   {
      if (!this->IsTracked(destinationOperand)) {
         continue;
      }

      this->EvaluateOperand(destinationOperand, generateBitVector, killBitVector);

   }
}


//------------------------------------------------------------------------------
//
// Description:
//
//    AvailableDefinitionData constructor.
//
// Arguments:
//
//    lifetime - Memory allocation lifetime
//
// Returns:
//
//    New AvailableDefinitionData object.
//
//------------------------------------------------------------------------------

AvailableDefinitionData *
AvailableDefinitionData::New()
{
   // Allocate all bit vectors the data contains

   AvailableDefinitionData * data = new AvailableDefinitionData;

   data->AvailableInBitVector = new llvm::SparseBitVector<>();
   data->AvailableOutBitVector = new llvm::SparseBitVector<>();
   data->AvailableGenerateBitVector = new llvm::SparseBitVector<>();
   data->AvailableKillBitVector = new llvm::SparseBitVector<>();

   return data;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for AvailableDefinition variables block data
//
//------------------------------------------------------------------------------

void
AvailableDefinitionData::Delete()
{
   // Delete all bit vectors the data contains

   delete this->AvailableInBitVector;
   delete this->AvailableOutBitVector;
   delete this->AvailableGenerateBitVector;
   delete this->AvailableKillBitVector;
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
//    If this is the first predeccessor, we can simply COPY, otherwise we AND.
//
//------------------------------------------------------------------------------

void
AvailableDefinitionData::Merge
(
   Data *     baseDependencyData,
   Data *     baseBlockData,
   Xcessor    incomingEdge,
   MergeFlags flags
)
{
#if 0
   BitVector::Sparse *       availableInBitVector = this->AvailableInBitVector;
   Graphs::BasicBlock *      baseBlock = baseBlockData->Block;
   AvailableDefinitionData * blockData = reinterpret_cast<AvailableDefinitionData *>(baseBlockData);
   AvailableDefinitionData * dependencyData = reinterpret_cast<AvailableDefinitionData *>(baseDependencyData);

   // Merge operation.

   if ((flags & MergeFlags::First) != MergeFlags::None)
   {
      if (dependencyData->WasVisited)
      {
         availableInBitVector->CopyBits(dependencyData->AvailableOutBitVector);
         blockData->WasInitialized = true;
      }
   }
   else
   {
      if (dependencyData->WasVisited)
      {
         if (blockData->WasInitialized)
         {
            availableInBitVector->And(dependencyData->AvailableOutBitVector);
         }
         else
         {
            availableInBitVector->CopyBits(dependencyData->AvailableOutBitVector);
            blockData->WasInitialized = true;
         }
      }
   }
#if 0
   // Handle case where full scope locals become available when entering the body 
   // of the function and become unavailable when exiting the body of the function.

   IR::Instruction ^ instruction = baseBlock->FirstInstruction;

   if (instruction->Opcode == Common::Opcode::EnterBody)
   {
      Tiled::FunctionUnit ^ functionUnit = instruction->FunctionUnit;
      Debug::Info ^       debugInfo = functionUnit->DebugInfo;
      BitVector::Sparse ^ fullScopeDefinitionBitVector = debugInfo->GetFullScopeDefinitionBitVector();

      availableInBitVector->Or(fullScopeDefinitionBitVector);
   }
   else if (instruction->Opcode == Common::Opcode::ExitBody)
   {
      Tiled::FunctionUnit ^ functionUnit = instruction->FunctionUnit;
      Debug::Info ^       debugInfo = functionUnit->DebugInfo;
      BitVector::Sparse ^ fullScopeDefinitionBitVector = debugInfo->GetFullScopeDefinitionBitVector();

      availableInBitVector->Minus(fullScopeDefinitionBitVector);
   }
#endif
#endif
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
AvailableDefinitionData::SamePrecondition
(
   Data * baseBlockData
)
{
   AvailableDefinitionData * blockData = static_cast<AvailableDefinitionData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData != nullptr);

   return ((*this->AvailableInBitVector) == (*blockData->AvailableInBitVector));
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
AvailableDefinitionData::SamePostCondition
(
   Dataflow::Data * baseBlockData
)
{
   AvailableDefinitionData * blockData = static_cast<AvailableDefinitionData *>(baseBlockData);  //dynamic_cast needed if > 1 derived class
   assert(blockData != nullptr);

   return ((*this->AvailableOutBitVector) == (*blockData->AvailableOutBitVector));
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
AvailableDefinitionData::Update
(
   Dataflow::Data * baseTemporaryData
)
{
   AvailableDefinitionData * temporaryData = static_cast<AvailableDefinitionData *>(baseTemporaryData);  //dynamic_cast needed if > 1 derived class
   assert(temporaryData);

   // We simply swap the available in and available out bit vectors.

   llvm::SparseBitVector<> * bitVectorSwap;

   bitVectorSwap = this->AvailableInBitVector;
   this->AvailableInBitVector = temporaryData->AvailableInBitVector;
   temporaryData->AvailableInBitVector = bitVectorSwap;

   bitVectorSwap = this->AvailableOutBitVector;
   this->AvailableOutBitVector = temporaryData->AvailableOutBitVector;
   temporaryData->AvailableOutBitVector = bitVectorSwap;

   this->WasVisited = true;
}

} // namespace Dataflow
} // namespace Tiled
