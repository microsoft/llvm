//===-- Dataflow/Liveness.h - Dataflow Package ------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_DATAFLOW_LIVENESS_H
#define TILED_DATAFLOW_LIVENESS_H

#include "Traverser.h"
#include "../Alias/Alias.h"
#include "../Graphs/Graph.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

namespace Tiled
{
namespace Dataflow
{

class LivenessWalker;

//------------------------------------------------------------------------------
//
// Description:
//
//    Liveness data object.
//
//------------------------------------------------------------------------------

typedef std::vector<llvm::MachineBasicBlock *>::iterator Xcessor;

class LivenessData : public Data
{

public:

   void Delete() override;

   static LivenessData * New();

public:

   virtual void
   Merge
   (
      Data *         dependencyData,
      Data *         blockData,
      Xcessor        incomingEdge,
      MergeFlags     flags
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

   void
   CalculateDataflow
   (
      const llvm::SparseBitVector<> * liveBitVector,
      const llvm::SparseBitVector<> * killBitVector,
      const llvm::SparseBitVector<> * generateBitVector
   );

public:

   llvm::SparseBitVector<> * LiveInBitVector;
   llvm::SparseBitVector<> * LiveOutBitVector;

public:   // CLI's  "public private"

   llvm::SparseBitVector<> * GenerateBitVector;
   llvm::SparseBitVector<> * KillBitVector;

   LivenessData() : Data(), LiveInBitVector(nullptr), LiveOutBitVector(nullptr), GenerateBitVector(nullptr), KillBitVector(nullptr) {}

};

#if 0
comment LivenessData::GenerateBitVector
{
   // Generated variables bit vector.
}

comment LivenessData::KillBitVector
{
   // Killed variables bit vector.
}

comment LivenessData::LiveInBitVector
{
   // Live in variables bit vector.
}

comment LivenessData::LiveOutBitVector
{
   // Live out variables bit vector.
}
#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Generic backwards liveness data flow walker to have derived
//    memory and temporary liveness implementations.
//
//------------------------------------------------------------------------------

class LivenessWalker : public Walker
{
   friend class LivenessData;

public:

   void Delete() override;

public:

   void
   AddToLiveIn
   (
      llvm::MachineBasicBlock * basicBlock,
      unsigned                  tag
   );

   void
   AddToLiveOut
   (
      llvm::MachineBasicBlock * basicBlock,
      unsigned                  tag
   );

   void
   AllocateData
   (
      unsigned numberElements
   ) override;

   virtual void
   ComputeLiveness
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   EvaluateBlock
   (
      llvm::MachineBasicBlock * block,
      Data *                    temporaryData
   ) override;

   bool
   IsLiveAfter
   (
      unsigned             tag,
      llvm::MachineInstr * afterInstruction
   );

   bool
   IsLiveAfter
   (
      llvm::MachineOperand *     operand,
      llvm::MachineInstr *       afterInstruction
   );

   bool
   IsLiveIn
   (
      unsigned                  tag,
      llvm::MachineBasicBlock * basicBlock
   );

   bool
   IsLiveIn
   (
      llvm::MachineOperand *    operand,
      llvm::MachineBasicBlock * basicBlock
   );

   bool
   IsLiveOut
   (
      unsigned                  tag,
      llvm::MachineBasicBlock * basicBlock
   );

   bool
   IsLiveOut
   (
      llvm::MachineOperand *    operand,
      llvm::MachineBasicBlock * basicBlock
   );

   virtual bool
   IsTracked
   (
      const llvm::MachineOperand * operand
   );

   llvm::SparseBitVector<> *
   LiveAfter
   (
      llvm::MachineInstr * afterInstruction
   );

   llvm::SparseBitVector<> *
   LiveIn
   (
      llvm::MachineBasicBlock * basicBlock
   );

   void
   MarkLastUseOperands
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   MarkNotTracked
   (
      Tiled::VR::Info *   vrInfo,
      unsigned            tag
   );

   static void StaticInitialize();

public:

   bool                          IsIncomplete;
   llvm::SparseBitVector<> *     NotTrackedTagBitVector;
    
protected:

   bool
   IsTracked
   (
      Tiled::VR::Info *    vrInfo,
      unsigned             tag
   );

   virtual bool
   TransferDestinations
   (
      const llvm::MachineInstr * instruction,
      llvm::SparseBitVector<> *  generateBitVector,
      llvm::SparseBitVector<> *  killBitVector
   ) = 0;

   virtual bool
   TransferSources
   (
      const llvm::MachineInstr * instruction,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   ) = 0;

protected:

   llvm::SparseBitVector<> * ScratchBitVector;
};


//------------------------------------------------------------------------------
//
// Description:
//
//    Backwards liveness data flow walker to track memory,
//    temporaries, and registers.
//
//------------------------------------------------------------------------------

#ifdef FUTURE_IMPL
class MemoryLivenessWalker : public Dataflow::LivenessWalker
{

public:

   void Delete() override;

   static Dataflow::MemoryLivenessWalker * New();

public:

   void
   ComputeLiveness
   (
      Tiled::FunctionUnit * functionUnit
   ) override;

   bool
   IsTracked
   (
      IR::Operand * operand
   ) override;

   bool
   TransferDestinations
   (
      IR::Instruction *   instruction,
      BitVector::Sparse * generateBitVector,
      BitVector::Sparse * killBitVector
   ) override;

   bool
   TransferSources
   (
      IR::Instruction *   instruction,
      BitVector::Sparse * generateBitVector,
      BitVector::Sparse * killBitVector
   ) override;
};
#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Backwards liveness data flow walker to track temporaries.
//
//------------------------------------------------------------------------------

class RegisterLivenessWalker : public LivenessWalker
{

public:

   void Delete() override;

   static RegisterLivenessWalker * New();

public:

   bool
   IsTracked
   (
      const llvm::MachineOperand * operand
   ) override;

   bool
   TransferDestinations
   (
      const llvm::MachineInstr * instruction,
      llvm::SparseBitVector<> *  generateBitVector,
      llvm::SparseBitVector<> *  killBitVector
   ) override;

   bool
   TransferSources
   (
      const llvm::MachineInstr * instruction,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   ) override;

private:
   llvm::SparseBitVector<> * callKilledRegBitVector;
};
} // namespace Dataflow
} // namespace Tiled

#endif // end TILED_DATAFLOW_LIVENESS_H
