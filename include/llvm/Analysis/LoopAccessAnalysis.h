//===- llvm/Analysis/LoopAccessAnalysis.h -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interface for the loop memory dependence framework that
// was originally developed for the Loop Vectorizer.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_LOOPACCESSANALYSIS_H
#define LLVM_ANALYSIS_LOOPACCESSANALYSIS_H

#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class Value;
class DataLayout;
class AliasAnalysis;
class ScalarEvolution;
class Loop;
class SCEV;

/// Optimization analysis message produced during vectorization. Messages inform
/// the user why vectorization did not occur.
class LoopAccessReport {
  std::string Message;
  const Instruction *Instr;

protected:
  LoopAccessReport(const Twine &Message, const Instruction *I)
      : Message(Message.str()), Instr(I) {}

public:
  LoopAccessReport(const Instruction *I = nullptr) : Instr(I) {}

  template <typename A> LoopAccessReport &operator<<(const A &Value) {
    raw_string_ostream Out(Message);
    Out << Value;
    return *this;
  }

  const Instruction *getInstr() const { return Instr; }

  std::string &str() { return Message; }
  const std::string &str() const { return Message; }
  operator Twine() { return Message; }

  /// \brief Emit an analysis note for \p PassName with the debug location from
  /// the instruction in \p Message if available.  Otherwise use the location of
  /// \p TheLoop.
  static void emitAnalysis(const LoopAccessReport &Message,
                           const Function *TheFunction,
                           const Loop *TheLoop,
                           const char *PassName);
};

/// \brief Collection of parameters shared beetween the Loop Vectorizer and the
/// Loop Access Analysis.
struct VectorizerParams {
  /// \brief Maximum SIMD width.
  static const unsigned MaxVectorWidth;

  /// \brief VF as overridden by the user.
  static unsigned VectorizationFactor;
  /// \brief Interleave factor as overridden by the user.
  static unsigned VectorizationInterleave;
  /// \brief True if force-vector-interleave was specified by the user.
  static bool isInterleaveForced();

  /// \\brief When performing memory disambiguation checks at runtime do not
  /// make more than this number of comparisons.
  static const unsigned RuntimeMemoryCheckThreshold;
};

/// \brief Drive the analysis of memory accesses in the loop
///
/// This class is responsible for analyzing the memory accesses of a loop.  It
/// collects the accesses and then its main helper the AccessAnalysis class
/// finds and categorizes the dependences in buildDependenceSets.
///
/// For memory dependences that can be analyzed at compile time, it determines
/// whether the dependence is part of cycle inhibiting vectorization.  This work
/// is delegated to the MemoryDepChecker class.
///
/// For memory dependences that cannot be determined at compile time, it
/// generates run-time checks to prove independence.  This is done by
/// AccessAnalysis::canCheckPtrAtRT and the checks are maintained by the
/// RuntimePointerCheck class.
class LoopAccessInfo {
public:
  /// This struct holds information about the memory runtime legality check that
  /// a group of pointers do not overlap.
  struct RuntimePointerCheck {
    RuntimePointerCheck() : Need(false) {}

    /// Reset the state of the pointer runtime information.
    void reset() {
      Need = false;
      Pointers.clear();
      Starts.clear();
      Ends.clear();
      IsWritePtr.clear();
      DependencySetId.clear();
      AliasSetId.clear();
    }

    /// Insert a pointer and calculate the start and end SCEVs.
    void insert(ScalarEvolution *SE, Loop *Lp, Value *Ptr, bool WritePtr,
                unsigned DepSetId, unsigned ASId,
                const ValueToValueMap &Strides);

    /// \brief No run-time memory checking is necessary.
    bool empty() const { return Pointers.empty(); }

    /// \brief Decide whether we need to issue a run-time check for pointer at
    /// index \p I and \p J to prove their independence.
    bool needsChecking(unsigned I, unsigned J) const;

    /// \brief Print the list run-time memory checks necessary.
    void print(raw_ostream &OS, unsigned Depth = 0) const;

    /// This flag indicates if we need to add the runtime check.
    bool Need;
    /// Holds the pointers that we need to check.
    SmallVector<TrackingVH<Value>, 2> Pointers;
    /// Holds the pointer value at the beginning of the loop.
    SmallVector<const SCEV*, 2> Starts;
    /// Holds the pointer value at the end of the loop.
    SmallVector<const SCEV*, 2> Ends;
    /// Holds the information if this pointer is used for writing to memory.
    SmallVector<bool, 2> IsWritePtr;
    /// Holds the id of the set of pointers that could be dependent because of a
    /// shared underlying object.
    SmallVector<unsigned, 2> DependencySetId;
    /// Holds the id of the disjoint alias set to which this pointer belongs.
    SmallVector<unsigned, 2> AliasSetId;
  };

  LoopAccessInfo(Loop *L, ScalarEvolution *SE, const DataLayout *DL,
                 const TargetLibraryInfo *TLI, AliasAnalysis *AA,
                 DominatorTree *DT, const ValueToValueMap &Strides);

  /// Return true we can analyze the memory accesses in the loop and there are
  /// no memory dependence cycles.
  bool canVectorizeMemory() const { return CanVecMem; }

  const RuntimePointerCheck *getRuntimePointerCheck() const {
    return &PtrRtCheck;
  }

  /// Return true if the block BB needs to be predicated in order for the loop
  /// to be vectorized.
  static bool blockNeedsPredication(BasicBlock *BB, Loop *TheLoop,
                                    DominatorTree *DT);

  /// Returns true if the value V is uniform within the loop.
  bool isUniform(Value *V) const;

  unsigned getMaxSafeDepDistBytes() const { return MaxSafeDepDistBytes; }
  unsigned getNumStores() const { return NumStores; }
  unsigned getNumLoads() const { return NumLoads;}

  /// \brief Add code that checks at runtime if the accessed arrays overlap.
  ///
  /// Returns a pair of instructions where the first element is the first
  /// instruction generated in possibly a sequence of instructions and the
  /// second value is the final comparator value or NULL if no check is needed.
  std::pair<Instruction *, Instruction *>
    addRuntimeCheck(Instruction *Loc) const;

  /// \brief The diagnostics report generated for the analysis.  E.g. why we
  /// couldn't analyze the loop.
  const Optional<LoopAccessReport> &getReport() const { return Report; }

  /// \brief Print the information about the memory accesses in the loop.
  void print(raw_ostream &OS, unsigned Depth = 0) const;

  /// \brief Used to ensure that if the analysis was run with speculating the
  /// value of symbolic strides, the client queries it with the same assumption.
  /// Only used in DEBUG build but we don't want NDEBUG-depedent ABI.
  unsigned NumSymbolicStrides;

private:
  /// \brief Analyze the loop.  Substitute symbolic strides using Strides.
  void analyzeLoop(const ValueToValueMap &Strides);

  /// \brief Check if the structure of the loop allows it to be analyzed by this
  /// pass.
  bool canAnalyzeLoop();

  void emitAnalysis(LoopAccessReport &Message);

  /// We need to check that all of the pointers in this list are disjoint
  /// at runtime.
  RuntimePointerCheck PtrRtCheck;
  Loop *TheLoop;
  ScalarEvolution *SE;
  const DataLayout *DL;
  const TargetLibraryInfo *TLI;
  AliasAnalysis *AA;
  DominatorTree *DT;

  unsigned NumLoads;
  unsigned NumStores;

  unsigned MaxSafeDepDistBytes;

  /// \brief Cache the result of analyzeLoop.
  bool CanVecMem;

  /// \brief The diagnostics report generated for the analysis.  E.g. why we
  /// couldn't analyze the loop.
  Optional<LoopAccessReport> Report;
};

Value *stripIntegerCast(Value *V);

///\brief Return the SCEV corresponding to a pointer with the symbolic stride
///replaced with constant one.
///
/// If \p OrigPtr is not null, use it to look up the stride value instead of \p
/// Ptr.  \p PtrToStride provides the mapping between the pointer value and its
/// stride as collected by LoopVectorizationLegality::collectStridedAccess.
const SCEV *replaceSymbolicStrideSCEV(ScalarEvolution *SE,
                                      const ValueToValueMap &PtrToStride,
                                      Value *Ptr, Value *OrigPtr = nullptr);

/// \brief This analysis provides dependence information for the memory accesses
/// of a loop.
///
/// It runs the analysis for a loop on demand.  This can be initiated by
/// querying the loop access info via LAA::getInfo.  getInfo return a
/// LoopAccessInfo object.  See this class for the specifics of what information
/// is provided.
class LoopAccessAnalysis : public FunctionPass {
public:
  static char ID;

  LoopAccessAnalysis() : FunctionPass(ID) {
    initializeLoopAccessAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// \brief Query the result of the loop access information for the loop \p L.
  ///
  /// If the client speculates (and then issues run-time checks) for the values
  /// of symbolic strides, \p Strides provides the mapping (see
  /// replaceSymbolicStrideSCEV).  If there is no cached result available run
  /// the analysis.
  const LoopAccessInfo &getInfo(Loop *L, const ValueToValueMap &Strides);

  void releaseMemory() override {
    // Invalidate the cache when the pass is freed.
    LoopAccessInfoMap.clear();
  }

  /// \brief Print the result of the analysis when invoked with -analyze.
  void print(raw_ostream &OS, const Module *M = nullptr) const override;

private:
  /// \brief The cache.
  DenseMap<Loop *, std::unique_ptr<LoopAccessInfo>> LoopAccessInfoMap;

  // The used analysis passes.
  ScalarEvolution *SE;
  const DataLayout *DL;
  const TargetLibraryInfo *TLI;
  AliasAnalysis *AA;
  DominatorTree *DT;
};
} // End llvm namespace

#endif
