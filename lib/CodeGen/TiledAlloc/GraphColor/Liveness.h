//===-- GraphColor/Liveness.h -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Description:
//
//    Liveness view used by the register allocator.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_LIVENESS_H
#define TILED_GRAPHCOLOR_LIVENESS_H

#include "../Dataflow/Defined.h"
#include "../Dataflow/Liveness.h"
#include "../Graphs/UnionFind.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

class Allocator;
class Tile;
   
class Liveness
{

public:

   void Delete();

   static GraphColor::Liveness *
   New
   (
      GraphColor::Allocator * allocator
   );

public:

   void BuildGlobalLiveRanges();

   void
   BuildLiveRanges
   (
      GraphColor::Tile * tile
   );

   void
   ComputeDefinedRegisters
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   ComputeGlobalRegisterLiveness
   (
      llvm::SparseBitVector<> * globalLiveBitVector
   );

   static UnionFind::Tree *
   ComputeLiveRangeAliasTagSets
   (
      llvm::iterator_range<llvm::MachineBasicBlock::instr_iterator>& instructionRange,
      GraphColor::Allocator *                                        allocator,
      bool                                                           onlyRegistersWithOverlap
   );

   void
   ComputeRegisterLiveness
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   ComputeRegisterLiveness
   (
      GraphColor::Tile * tile
   );

   llvm::SparseBitVector<> *
   ComputeRegisterLiveness
   (
      llvm::MachineInstr *      instruction,
      llvm::MachineBasicBlock * block,
      llvm::SparseBitVector<> * outputBitVector
   );

   void ComputeTileGlobalAliasTagSetsAndWeight();

   unsigned
   EnumerateGlobalLiveRanges
   (
      llvm::SparseBitVector<> * globalAliasTagSet
   );

   unsigned
   EnumerateGlobalLiveRangesWithInTile
   (
      GraphColor::Tile *        tile,
      GraphColor::Tile *        nestedTile,
      llvm::SparseBitVector<> * globalLiveBitVector
   );

   unsigned
   EnumerateLiveRanges
   (
      GraphColor::Tile * tile
   );

   void
   EstimateBlockPressure
   (
      llvm::MachineBasicBlock * block,
      std::vector<unsigned> *   registerPressureVector
   );

   llvm::SparseBitVector<> *
   GetGlobalAliasTagSet
   (
      llvm::MachineBasicBlock * block
   );

   Dataflow::DefinedData *
   GetRegisterDefinedData
   (
      llvm::MachineBasicBlock * block
   );

   Dataflow::LivenessData *
   GetRegisterLivenessData
   (
      llvm::MachineBasicBlock * block
   );

   void
   PruneRegisterLiveness
   (
      llvm::SparseBitVector<> * liveBitVector,
      llvm::SparseBitVector<> * definedBitVector
   );

   void
   PruneRegisterLiveness
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   PruneRegisterLiveness
   (
      GraphColor::Tile * tile
   );

   void
   RemoveDeadCode
   (
      GraphColor::Tile * tile
   );

   void
   TransferInstruction
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   );

   void
   UpdateInstruction
   (
      llvm::MachineInstr *      instruction,
      llvm::SparseBitVector<> * liveBitVector,
      llvm::SparseBitVector<> * generateBitVector,
      llvm::SparseBitVector<> * killBitVector
   );

public:
   Dataflow::RegisterDefinedWalker *   RegisterDefinedWalker;
   Dataflow::RegisterLivenessWalker *  RegisterLivenessWalker;

   bool                                IsRegisterLivenessPruningRequired;

private:

   void
   DecrementAllocatable
   (
      const llvm::TargetRegisterClass * RC,
      std::vector<unsigned> *           pressureVector
   );

   llvm::MachineInstr *
   FindFirstRealInstruction
   (
      llvm::MachineBasicBlock * block
   );

   llvm::MachineBasicBlock *
   FindSingleLiveSuccessorBlock
   (
      llvm::MachineBasicBlock * block,
      unsigned                  aliasTag
   );

   void
   IncrementAllocatable
   (
      const llvm::TargetRegisterClass * RC,
      std::vector<unsigned> *           pressureVector
   );

   const llvm::TargetRegisterClass *
   getChildRegisterCategory
   (
      const llvm::TargetRegisterClass * RC
   );

private:
   GraphColor::Allocator *           Allocator;
   VR::Info *                        vrInfo;
   Graphs::FlowGraph *               FunctionUnit;

   llvm::SparseBitVector<> *         GenerateBitVector;
   llvm::SparseBitVector<> *         KillBitVector;
   llvm::SparseBitVector<> *         LiveBitVector;
   std::vector<unsigned> *           ScratchPressureVector;
};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_LIVENESS_H
