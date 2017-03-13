//===-- RegAllocTiled.cpp - Tiled register allocator ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the RegAllocTiled function pass for register allocation.
//
//===----------------------------------------------------------------------===//

#include "RegAllocBase.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "TiledAlloc/GraphColor/Allocator.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static RegisterRegAlloc tiledRegAlloc("tiled", "tiled register allocator",
                                      createTiledRegisterAllocator);

namespace {

class RegAllocTiled : public MachineFunctionPass {

public:

   static char ID;

   RegAllocTiled() : MachineFunctionPass(ID) {}

   ~RegAllocTiled() {}

   StringRef getPassName() const override {
      return "Tiled register allocator";
   }

   void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<LiveIntervals>();
      AU.addPreserved<LiveIntervals>();
      AU.addRequired<SlotIndexes>();
      AU.addPreserved<SlotIndexes>();
      AU.addRequired<MachineDominatorTree>();
      AU.addRequired<VirtRegMap>();
      AU.addRequired<MachineLoopInfo>(); 
      AU.addRequired<AAResultsWrapperPass>();
      AU.addPreserved<AAResultsWrapperPass>();
      AU.addRequired<MachineBlockFrequencyInfo>();
      AU.addRequired<MachineBranchProbabilityInfo>();
      MachineFunctionPass::getAnalysisUsage(AU);
   }

private:
   /// Perform register allocation.
   bool runOnMachineFunction(MachineFunction &mf) override;

};

char RegAllocTiled::ID = 0;
}


bool RegAllocTiled::runOnMachineFunction(MachineFunction &mf) {
   DEBUG(dbgs() << "********** TILED REGISTER ALLOCATION **********\n"
                << "********** Function: " << mf.getName() << '\n');

   Tiled::Graphs::FlowGraph * fg = 
      Tiled::Graphs::FlowGraph::New(&mf, &getAnalysis<MachineDominatorTree>(), &getAnalysis<MachineLoopInfo>(), *this);
   Tiled::RegisterAllocator::GraphColor::Allocator *tiledRegAlloc =
      Tiled::RegisterAllocator::GraphColor::Allocator::New(fg, &getAnalysis<VirtRegMap>(),
                                                               &getAnalysis<SlotIndexes>(),
                                                               &getAnalysis<MachineLoopInfo>(),
                                                               &getAnalysis<AAResultsWrapperPass>().getAAResults(),
                                                               &getAnalysis<MachineBlockFrequencyInfo>(),
                                                               &getAnalysis<MachineBranchProbabilityInfo>());

   tiledRegAlloc->Allocate();
   
   return true;
}


FunctionPass* llvm::createTiledRegisterAllocator() {
   return new RegAllocTiled();
}


