//===-- ModuleRaiser.h - Object file dumping utility for llvm -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of ModuleRaiser class for use by
// llvm-mctoll.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_MCTOLL_MODULERAISER_H
#define LLVM_TOOLS_LLVM_MCTOLL_MODULERAISER_H

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include <vector>

using namespace llvm;
using namespace std;

class MachineFunctionRaiser;

using namespace object;

// The ModuleRaiser class encapsulates information needed to raise a given
// module.
class ModuleRaiser {
public:
  ModuleRaiser(Module &m, const TargetMachine *tm, MachineModuleInfo *mmi,
               const MCInstrAnalysis *mia, const MCInstrInfo *mii,
               const ObjectFile *o, MCDisassembler *dis)
      : M(m), TM(tm), MMI(mmi), MIA(mia), MII(mii), Obj(o), DisAsm(dis),
        TextSectionIndex(-1) {
    supportedArch = false;
    auto arch = tm->getTargetTriple().getArch();
    switch (arch) {
    case Triple::x86_64:
      supportedArch = true;
      break;
    case Triple::arm:
      supportedArch = true;
      break;
    default:
      outs() << "\n" << arch << " not yet supported for raising\n";
    }
    // Collect dynamic relocations.
    collectDynamicRelocations();
  }

  // Function to create a MachineFunctionRaiser corresponding to Function f.
  // As noted elsewhere (llvm-mctoll.cpp), f is a place holder to allow for
  // creation of MachineFunction. The Function object representing raising
  // of MachineFunction is accessible by calling getRaisedFunction()
  // on the MachineFunctionRaiser object.
  MachineFunctionRaiser *CreateAndAddMachineFunctionRaiser(Function *f,
                                                           const ModuleRaiser *,
                                                           uint64_t start,
                                                           uint64_t end);

  MachineFunctionRaiser *getCurrentMachineFunctionRaiser() {
    if (mfRaiserVector.size() > 0) {
      return mfRaiserVector.back();
    }
    return nullptr;
  }

  // Insert the map of raised function R to place-holder function PH pointer
  // that inturn has the to corresponding MachineFunction.

  bool insertPlaceholderRaisedFunctionMap(Function *R, Function *PH) {
    auto V = PlaceholderRaisedFunctionMap.insert(std::make_pair(R, PH));
    return V.second;
  }

  bool collectTextSectionRelocs(const SectionRef &);
  bool collectDynamicRelocations();

  MachineFunction *getMachineFunction(Function *);

  // Member getters
  Module &getModule() const { return M; }
  const TargetMachine *getTargetMachine() const { return TM; }
  MachineModuleInfo *getMachineModuleInfo() const { return MMI; }
  const MCInstrAnalysis *getMCInstrAnalysis() const { return MIA; }
  const MCInstrInfo *getMCInstrInfo() const { return MII; }
  const ObjectFile *getObjectFile() const { return Obj; }
  const MCDisassembler *getMCDisassembler() const { return DisAsm; }

  bool isSupportedArch() const { return supportedArch; }

  bool runMachineFunctionPasses();

  // Return the Function * corresponding to input binary function with
  // start offset equal to that specified as argument.
  Function *getFunctionAt(uint64_t) const;

  // Return the Function * corresponding to input binary function from
  // text relocation record with off set in the range [Loc, Loc+Size].
  Function *getCalledFunctionUsingTextReloc(uint64_t Loc, uint64_t Size) const;

  // Get dynamic relocation with offset 'O'
  const RelocationRef *getDynRelocAtOffset(uint64_t O) const;
  // Return text relocation of instruction at index 'I'. 'S' is the size of the
  // instruction at index 'I'.
  const RelocationRef *getTextRelocAtOffset(uint64_t I, uint64_t S) const;
  int64_t getTextSectionAddress() const;

  const Value *getRODataValueAt(uint64_t) const;
  void addRODataValueAt(Value *, uint64_t) const;

  virtual ~ModuleRaiser() {}

private:
  // A sequential list of MachineFunctionRaiser objects created
  // as the instructions of the input binary are parsed. Each of
  // these correspond to a "machine function". A machine function
  // corresponds to a sequence of instructions (possibly interspersed
  // by data bytes) whose start is denoted by a function symbol in
  // the binary.
  std::vector<MachineFunctionRaiser *> mfRaiserVector;
  // A map of raised function pointer to place-holder function pointer
  // that links to the MachineFunction.
  DenseMap<Function *, Function *> PlaceholderRaisedFunctionMap;
  // Sorted vector of text relocations
  std::vector<RelocationRef> TextRelocs;
  // Vector of dynamic relocation records
  std::vector<RelocationRef> DynRelocs;
  // Map of read-only data (i.e., from .rodata) to its corresponding global
  // value.
  // NOTE: A const version of ModuleRaiser object is constructed during the
  // raising process. Making this map mutable since this map is expected to be
  // updated throughout the raising process.
  mutable std::map<uint64_t, Value *> GlobalRODataValues;

  // Commonly used data structures
  Module &M;
  const TargetMachine *TM;
  MachineModuleInfo *MMI;
  const MCInstrAnalysis *MIA;
  const MCInstrInfo *MII;
  bool supportedArch;
  const ObjectFile *Obj;
  MCDisassembler *DisAsm;
  // Index of text section whose instructions are raised
  int64_t TextSectionIndex;
};

#endif // LLVM_TOOLS_LLVM_MCTOLL_MODULERAISER_H
