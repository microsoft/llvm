//===-- Dataflow/AvailableDefinitions.h -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_DATAFLOW_AVAILABLEDEFINITIONS_H
#define TILED_DATAFLOW_AVAILABLEDEFINITIONS_H

#include "Traverser.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include <map>

namespace Tiled
{
namespace Dataflow
{

//------------------------------------------------------------------------------
//
// Description:
//
//    The Dataflow structure used by AvailableDefinitionWalker.
//
//------------------------------------------------------------------------------

typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;

class AvailableDefinitionData : public Data
{
 public:

   // New, Delete, Merge, SamePrecondition, SamePostCondition 
   // and Update are mandatory part for any extension of Dataflow::Data.
   // The rest defines custom part of the class.

   void Delete() override;

   static AvailableDefinitionData * New();

public:

   void
   Merge
   (
      Data *     dependencyData,
      Data *     blockData,
      Xcessor    incomingEdge,
      MergeFlags flags
   ) override;

   bool
   SamePostCondition
   (
      Data * blockData
   ) override;

   bool
   SamePrecondition
   (
      Data * blockData
   ) override;

   void
   Update
   (
      Data * temporaryData
   ) override;

public:

   llvm::SparseBitVector<> * AvailableGenerateBitVector;
   llvm::SparseBitVector<> * AvailableInBitVector;
   llvm::SparseBitVector<> * AvailableKillBitVector;
   llvm::SparseBitVector<> * AvailableOutBitVector;
   bool                      WasInitialized;
   bool                      WasVisited;
};

typedef std::map<unsigned, llvm::SparseBitVector<>* > TagToSparseBitVectorMap;

//-----------------------------------------------------------------------------
//
// Description:
//
//    Dataflow walker used for finding where variables are defined.
//
// Remarks:
// 
//    This dataflow walker computes that at any given point of program,
//    whether there is any definition of a variable might reach here. This is
//    a forward dataflow computation. Initial bit-vector is empty.
//
//          IN = INTERSECTION(previous OUTs)
//          OUT = (IN - KILL) + GEN
//
//    Note that this is similar to "reaching definitions" but tracks only
//    fully available definitions that reach a variable use.
//
//-----------------------------------------------------------------------------

class AvailableDefinitionWalker : public Walker
{

public:

   static AvailableDefinitionWalker * New();

public:

   void
   AllocateData
   (
      unsigned numberElements
   ) override;

   void
   EvaluateBlock
   (
      llvm::MachineBasicBlock * block,
      Data *                    temporaryData
   ) override;

   void
   EvaluateInstruction
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   );

   void
   EvaluateOperand
   (
      llvm::MachineOperand *    operand,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   );

   llvm::SparseBitVector<> *
   GetDefinitionKillBitVector
   (
      unsigned resourceAliasTag
   );

   bool
   IsTracked
   (
      llvm::MachineOperand * operand
   );

   static void StaticInitialize();

public:

   llvm::SparseBitVector<> *                   AnticipatedDefinitionsBitVector;
   llvm::SparseBitVector<> *                   AvailableDefinitionsBitVector;
   llvm::SparseBitVector<> *                   BeginDefinitionsBitVector;
   TagToSparseBitVectorMap*                    DefinitionKillBitVectorMap;
   llvm::SparseBitVector<> *                   EndDefinitionsBitVector;
   llvm::SparseBitVector<> *                   NullDefinitionKillBitVector;
   llvm::SparseBitVector<> *                   ScratchKillBitVector;
};

} // namespace Dataflow
} // namespace Tiled

#endif // end TILED_DATAFLOW_AVAILABLEDEFINITIONS_H
