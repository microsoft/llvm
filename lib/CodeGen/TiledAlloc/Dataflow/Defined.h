//===-- Dataflow/Defined.h - Dataflow Package -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_DATAFLOW_DEFINED_H
#define TILED_DATAFLOW_DEFINED_H

#include "Liveness.h"
#include "Traverser.h"
#include "llvm/CodeGen/MachineOperand.h"

namespace Tiled
{
namespace Dataflow
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    The Dataflow structure used by DefinedWalker. 
//
//-----------------------------------------------------------------------------

typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;

class DefinedData : public Data
{

public:

   // New, Delete, Merge, SamePrecondition, SamePostCondition 
   // and Update are mandatory part for any extension of Dataflow::Data.
   // The rest defines custom part of the class.

   void Delete() override;

   static DefinedData * New();

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

   llvm::SparseBitVector<> * GenerateBitVector;
   llvm::SparseBitVector<> * KillBitVector;
   llvm::SparseBitVector<> * InBitVector;
   llvm::SparseBitVector<> * OutBitVector;
};

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
//          IN = UNION(previous OUTs)
//          OUT = IN + GEN
//
//    Note that this is not the same as "reaching definitions", which is a
//    more expensive analysis that computes exactly which defintions reach
//    each variable use.
//
//-----------------------------------------------------------------------------

class DefinedWalker : public Walker
{

public:

   static DefinedWalker * New();

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

   virtual void
   EvaluateInstruction
   (
      const llvm::MachineInstr * instruction,
      llvm::SparseBitVector<> *  generateBitVector
   );

   virtual bool
   IsTracked
   (
      const llvm::MachineOperand * operand
   )
   {
      return true;
   }

   static void StaticInitialize();

public:

   LivenessWalker *            LivenessWalker;
};

//-----------------------------------------------------------------------------
//
// Description:
//
//    Forward defined dataflow walker to track registers.
//
//-----------------------------------------------------------------------------

class RegisterDefinedWalker : public DefinedWalker
{

public:

   static RegisterDefinedWalker * New();

public:

   bool
   IsTracked
   (
      const llvm::MachineOperand * operand
   ) override;
};
} // namespace Dataflow
} // namespace Tiled

#endif // end TILED_DATAFLOW_DEFINED_H
