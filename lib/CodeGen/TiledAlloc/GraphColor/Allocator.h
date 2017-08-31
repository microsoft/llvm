//===-- GraphColor/Allocator.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Global graph coloring register allocator.  Tile based graph coloring 
// allocator working on hierarchical regions built on the input program structure.
//
// Components:
//
// Allocator.h             - Base allocator object
// Tile.h                  - Region representation used to capture locality.
// Liveness.h              - Allocator view of variable 'liveness'
// Graph.h                 - Base graph implementation shared by conflict-graph and preference-graph
// ConflictGraph.h         - Graph representation capturing conflicts between live ranges.
// PreferenceGraph.h       - Graph representation capturing preference relationships between live ranges
//                           and between live ranges and registers.
// CostModel.h             - Defines allocator cost models.  Captures tradeoff between cycles and code
//                           size for different regions of the program (tiles)
// LiveRange.h             - Captures a variables live range and associated information.  This is the
//                           unit of allocation and is based on an objects alias tag.
// SummaryVariable.h       - Summary information of a tile mapping of live ranges to allocated resource.
// Preference.h            - Preference edge information.
// SpillOptimizer.h        - Spill/reload/recalculate sharing logic and low-level spill costing routines.
// AvailableExpressions.h  - Provides information about interesting expressions for use in recalculation.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_ALLOCATOR_H
#define TILED_GRAPHCOLOR_ALLOCATOR_H

#include "Graph.h"
#include "Preference.h"
#include "../Graphs/Graph.h"
#include "../Graphs/NodeFlowOrder.h"

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/VirtRegMap.h"

#include <unordered_set>

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

class AllocatorCostModel;
class AvailableExpressions;
class SpillOptimizer;
class LiveRange;
class PhysicalRegisterPreference;
class Runtimes;
class Liveness;
class Tile;
class TileGraph;
class TargetRegisterAllocInfo;


typedef GraphColor::LiveRangeVector LiveRangeEnumerator;

//-------------------------------------------------------------------------------------------------------------
// Description:
//
//    Enumeration of passes in the graph coloring allocator.  Used to key different pass specific logic.
//
//-------------------------------------------------------------------------------------------------------------

enum class Pass
{
   Invalid  = 0,
   Allocate = 1,
   Assign   = 2,
   Ready    = 99
};

//-------------------------------------------------------------------------------------------------------------
// Description:
//
//    Enumeration of live range decisions in the graph coloring allocator.
//
//-------------------------------------------------------------------------------------------------------------

enum class Decision
{
   Initial     = 0,
   Allocate    = 1,
   Memory      = 2,
   Recalculate = 3,
   Fold        = 4,
   CallerSave  = 5,
   Invalid     = 6
};

//-------------------------------------------------------------------------------------------------------------
// Description:
//
//    Enumeration of heuristic used for register allocation.
//
//-------------------------------------------------------------------------------------------------------------

enum class Heuristic
{
   Invalid,
   Optimistic,
   Pessimistic
};

//-------------------------------------------------------------------------------------------------------------
// Description:
//
//    Enumeration of block kinds with respect to tiles.
//
//-------------------------------------------------------------------------------------------------------------

enum class BlockKind
{
   Invalid,
   EnterTile,
   ExitTile,
   Interior
};

//-------------------------------------------------------------------------------------------------------------
// Description:
//
//    Block record for slotwise dataflow analysis
//
//-------------------------------------------------------------------------------------------------------------

class BlockInfo
{

public:

   void ClearAll();

public:
   bool IsAnalyzed;
   bool IsAnticipatedIn;
   bool IsAnticipatedOut;
   bool IsAvailableIn;
   bool IsAvailableOut;
   bool IsDefinition;
   bool IsDownDefinedIn;
   bool IsDownDefinedOut;
   bool IsEntry;
   bool IsExit;
   bool IsKill;
   bool IsLiveIn;
   bool IsLiveOut;
   bool IsUpExposedIn;
   bool IsUpExposedOut;
   bool IsUse;
};

class BlockProbe
{

public:
   unsigned BlockId;
   unsigned FoldCount;
   unsigned FoldedReloadCount;
   unsigned FoldedSpillCount;
   unsigned InstructionCount;
   unsigned ProbeId;
   unsigned RecalculateCount;
   unsigned ReloadCount;
   unsigned SpillCount;
   unsigned TransferCount;
};

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Tile based graph coloring register allocator.
//
// Remarks:
//
//    Allocator object implements a two pass hierarchical approach to allocation in which the first pass does a
//    per-tile coloring of appearances based only on conflicts within the tile bottom up (Allocator::Allocate()) and a
//    second pass assigns registers and inserts inter-tile fixup code top down (Allocator::Assign()).
//
//    Allocation - Follows a traditional graph coloring approach iteratively applying Build(tile), Cost(tile),
//    Simplify(tile), Select(tile), and Spill(tile) until a coloring of the tile in the number of physical
//    registers on the machine is found.
//
//    Diagram of core coloring loop:
//
//                               +---------+
//          +<------------------ |  Spill  | <-------------------+
//          |                    +---------+                     |
//    +----------+      +----------+      +----------+      +----------+
//    |   Build  |----->|   Cost   |----->| Simplify |----->|  Select  |---> K coloring of tile
//    +----------+      +----------+      +----------+      +----------+
//    
//
//    Assignment - Assigns final physical registers to appearances per-tile starting at the root and iterating
//    through child tiles and where different decisions were applied to child tiles inserting compensation
//    code (spill or transfer between resources).
//
//-------------------------------------------------------------------------------------------------------------

class Allocator
{

public:

   enum class Constants
   {
      IndexNotFound             = -1,
      LiveRangeLimit            = 65536,
      PreferenceSearchDepth     = 3,
      AntiPreferenceSearchDepth = 2
   };

public:

   Allocator() :
      ActiveTile(nullptr), VrInfo(nullptr), AliasTagToIdVector(nullptr), AntiPreferenceSearchDepth(std::numeric_limits<unsigned>::max()),
      BaseFloatRegisterCategory(nullptr), BaseIntegerRegisterCategory(nullptr),
      BiasedEntryCount(), CallCountConservativeThreshold(std::numeric_limits<unsigned>::max()),
      CallerSaveSpillLiveRangeIdSetScratch(nullptr), ColdCostModel(nullptr), CostSpillSymbol(std::numeric_limits<Graphs::FrameIndex>::max()),
      DoNotAllocateRegisterAliasTagBitVector(nullptr), DoNotSpillAliasTagBitVector(nullptr), callKilledRegBitVector(nullptr), DoPassTwoReallocate(true),
      DummySpillSymbol(std::numeric_limits<Graphs::FrameIndex>::max()), EarlyOutTraceBlockSizeLimit(50), EntryProfileCount(),
      FlowExtendedBasicBlockReversePostOrder(nullptr), FlowReversePostOrder(nullptr), FunctionUnit(nullptr), LocalIdMap(nullptr),
      MF(nullptr), TRI(nullptr), VRM(nullptr), Indexes(nullptr), LoopInfo(nullptr), AA(nullptr), MbbFreqInfo(nullptr), MBranchProbInfo(nullptr),
      MCID_ENTERTILE(llvm::MCInstrDesc()), MCID_EXITTILE(llvm::MCInstrDesc()), MCID_MOVE(llvm::MCInstrDesc()),
      GlobalAliasTagSet(nullptr), GlobalAliasTagToIdVector(nullptr), GlobalAvailableExpressions(nullptr),
      GlobalLiveRangeConservativeThreshold(std::numeric_limits<unsigned>::max()), GlobalLiveRangeVector(nullptr),
      GlobalSpillLiveRangeIdSetScratch(nullptr),
      HotCostModel(nullptr), InfiniteCost(), IntegerMoveCost(), IsTilingEnabled(true), Liveness(nullptr),
      LivenessScratchBitVector(nullptr), MaximumRegisterCategoryId(std::numeric_limits<unsigned>::max()),
      NodeCountThreshold(std::numeric_limits<unsigned>::max()), NumberCalls(0),
      NumberGlobalCallSpanningLiveRanges(std::numeric_limits<unsigned>::max()), Pass(GraphColor::Pass::Invalid),
      PendingSpillAliasTagBitVector(nullptr), PreferenceSearchDepth(std::numeric_limits<unsigned>::max()),
      RegisterCategoryIdToAllocatableRegisterCount(nullptr), RegisterCostVector(nullptr), ScratchBitVector1(nullptr), ScratchBitVector2(nullptr),
      ScratchBitVector3(nullptr), SearchLiveRangeId(std::numeric_limits<unsigned>::max()), SearchNumber(std::numeric_limits<unsigned>::max()),
      SpillOptimizer(nullptr), inFunctionRemovedLocalLRs(0), IdToSlotMap(nullptr), TargetRegisterAllocator(nullptr),
      TileGraph(nullptr), TotalSpillCost(), TotalTransferCost(), UndefinedRegisterAliasTagBitVector(nullptr), UnitCost(), ZeroCost(),
      BooleanVector(nullptr), CalleeSaveConservativeThreshold(std::numeric_limits<unsigned>::max()),
      ConservativeControlThreshold(std::numeric_limits<unsigned>::max()), ConservativeFatThreshold(std::numeric_limits<unsigned>::max()),
      ConservativeLoopThreshold(std::numeric_limits<unsigned>::max()), CostValueVector(nullptr), IterationLimit(std::numeric_limits<unsigned>::max()),
      MaximumRegisterId(std::numeric_limits<unsigned>::max()), ProfileBasisCount(), RegisterAntiPreferenceVector(nullptr),
      RegisterCategoryIdToAllocatableAliasTagBitVector(nullptr), RegisterPreferenceVector(nullptr), RegisterPressureVector(nullptr),
      SimplificationHeuristic(GraphColor::Heuristic::Invalid), SimplificationStack(nullptr), TemporaryRegisterPreferenceVectors(nullptr),
      BlockIdToMaxLocalRegister()
   {}

   void Delete();

   static GraphColor::Allocator *
   New
   (
      Graphs::FlowGraph *     functionUnit,
      llvm::VirtRegMap  *     vrm,
      llvm::SlotIndexes *     slotIndexes,
      llvm::MachineLoopInfo * mli,
      llvm::AliasAnalysis *   aa,
      llvm::MachineBlockFrequencyInfo * mbfi,
      llvm::MachineBranchProbabilityInfo * mbpi
   );

public:

   GraphColor::LiveRange *
   AddGlobalLiveRange
   (
      unsigned  aliasTag,
      unsigned  reg
   );

   GraphColor::LiveRange *
   AddGlobalRegisterLiveRange
   (
      unsigned  reg
   );

   void Allocate();

   void
   Allocate
   (
      GraphColor::Tile *      tile,
      GraphColor::LiveRange * liveRange,
      unsigned                reg
   );

   void
   AllocateTrivialLiveRange
   (
      GraphColor::Tile *          tile,
      GraphColor::LiveRange *     liveRange,
      GraphColor::OperandVector * operandVector,
      llvm::SparseBitVector<> *   conflictBitVector,
      llvm::SparseBitVector<> *   liveBitVector
   );

   void
   AllocateTrivialLiveRanges
   (
      GraphColor::Tile * tile
   );

   void
   BeginIteration
   (
      GraphColor::Tile * tile,
      GraphColor::Pass   pass
   );

   void
   BeginPass
   (
      GraphColor::Tile * tile,
      GraphColor::Pass   pass
   );

   void
   BeginPass
   (
      GraphColor::Pass pass
   );

   void BuildFlowGraphOrder();

   unsigned
   CalculateDegree
   (
      GraphColor::LiveRange * liveRange,
      GraphColor::LiveRange * conflictingLiveRange
   );

   bool DoAnnotateFunction();

   void
   EndIteration
   (
      GraphColor::Tile * tile,
      GraphColor::Pass   pass
   );

   void
   EndPass
   (
      GraphColor::Tile * tile,
      GraphColor::Pass   pass
   );

   void
   EndPass
   (
      GraphColor::Pass pass
   );

   llvm::SparseBitVector<> *
   GetAllocatableRegisters
   (
      GraphColor::Tile *      tile,
      GraphColor::LiveRange * liveRange
   );

   llvm::SparseBitVector<> *
   GetAllocatableRegisters
   (
      const llvm::TargetRegisterClass * registerCategory
   );

   Tiled::CostValue
   GetBiasedInstructionFrequency
   (
      llvm::MachineInstr * instruction
   );

   Tiled::CostValue
   GetBiasedBlockFrequency
   (
      llvm::MachineBasicBlock * block
   );

   Profile::Count
   getProfileCount
   (
      llvm::MachineInstr * instruction
   ) const;

   Profile::Count
   getProfileCount
   (
      llvm::MachineBasicBlock * block
   ) const;

   llvm::SparseBitVector<> *
   GetCategoryRegisters
   (
      GraphColor::LiveRange * liveRange
   );

   GraphColor::LiveRange *
   GetGlobalLiveRange
   (
      llvm::MachineOperand * operand
   );

   GraphColor::LiveRange *
   GetGlobalLiveRange
   (
      unsigned aliasTag
   );

   GraphColor::LiveRange *
   GetGlobalLiveRange
   (
      GraphColor::LiveRange * summaryLiveRange
   );

   GraphColor::LiveRange *
   GetGlobalLiveRangeById
   (
      unsigned globalLiveRangeId
   );

   GraphColor::LiveRangeEnumerator * GetGlobalLiveRangeEnumerator();

   unsigned
   GetGlobalLiveRangeId
   (
      unsigned aliasTag
   );

   unsigned
   GetGlobalLiveRangeIdWithMerge
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   unsigned
   GetGlobalLiveRangeIdWithOverlap
   (
      unsigned aliasTag
   );

   unsigned
   GetLiveRangeId
   (
      unsigned aliasTag
   );

   unsigned
   GetLiveRangeIdWithMerge
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   unsigned
   GetLiveRangeIdWithOverlap
   (
      unsigned aliasTag
   );

   GraphColor::LiveRangeVector *
   GetLiveRangeOrder
   (
      GraphColor::Tile * tile
   );

   unsigned
   GetTileId
   (
      llvm::MachineInstr * instruction
   );

   void
   SetTileId
   (
      llvm::MachineInstr * instruction,
      unsigned             tileId
   );

   GraphColor::Tile *
   GetTile
   (
      llvm::MachineInstr * instruction
   );

   GraphColor::Tile *
   GetTile
   (
      llvm::MachineBasicBlock * block
   );

   void
   InsertOrUpdateRegisterPreference
   (
      GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
      unsigned                                       requiredReg,
      unsigned                                       preferredReg,
      Tiled::Cost                                    preferredCost,
      llvm::SparseBitVector<> *                      allocatableRegisterTagBitVector
   );

   void
   InsertRegisterPreference
   (
      GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
      unsigned                                       reg,
      Tiled::Cost                                    cost
   );

   bool
   IsFatBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsLargeBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   MapAliasTagToLiveRangeId
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   void
   MapAliasTagToLiveRangeIdMerged
   (
      unsigned aliasTag,
      unsigned liveRangeId,
      unsigned mergedLiveRangeId
   );

   void
   MapAliasTagToLiveRangeIdOverlapped
   (
      unsigned   aliasTag,
      unsigned   liveRangeId,
      bool       allowRemap
   );

   void
   MapAliasTagToLiveRangeIdOverlappedExclusive
   (
      unsigned aliasTag,
      unsigned liveRangeId
   )
   {
      this->MapAliasTagToLiveRangeIdOverlapped(aliasTag, liveRangeId, false);
   }

   void
   MapAliasTagToLiveRangeIdOverlappedRemap
   (
      unsigned aliasTag,
      unsigned liveRangeId
   )
   {
      this->MapAliasTagToLiveRangeIdOverlapped(aliasTag, liveRangeId, true);
   }

   void
   MapAliasTagToRegisterLiveRangeIdOverlapped
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   void
   MapGlobalAliasTagToLiveRangeId
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   void
   MapGlobalAliasTagToLiveRangeIdMerged
   (
      unsigned aliasTag,
      unsigned liveRangeId,
      unsigned mergedLiveRangeId
   );

   void
   MapGlobalAliasTagToLiveRangeIdOverlapped
   (
      unsigned aliasTag,
      unsigned liveRangeId,
      bool     allowRemap
   );

   void
   MapGlobalAliasTagToLiveRangeIdOverlappedExclusive
   (
      unsigned aliasTag,
      unsigned liveRangeId
   )
   {
      this->MapGlobalAliasTagToLiveRangeIdOverlapped(aliasTag, liveRangeId, false);
   }

   void
   MapGlobalAliasTagToLiveRangeIdOverlappedRemap
   (
      unsigned aliasTag,
      unsigned liveRangeId
   )
   {
      this->MapGlobalAliasTagToLiveRangeIdOverlapped(aliasTag, liveRangeId, true);
   }

   void
   MapGlobalAliasTagToRegisterLiveRangeIdOverlapped
   (
      unsigned aliasTag,
      unsigned liveRangeId
   );

   unsigned
   NumberAllocatableRegisters
   (
      GraphColor::LiveRange * liveRange
   );

   void ResetAliasTagToIdMap();

   void
   ScaleBy
   (
      Tiled::Cost * cost,
      int         scaleFactor
   )
   {
      cost->ScaleBy(scaleFactor);
   }

   void
   ScaleCyclesByFrequency
   (
      Tiled::Cost *         cost,
      llvm::MachineInstr *  instruction
   )
   {
      cost->ScaleCyclesByFrequency(this->GetBiasedInstructionFrequency(instruction));
   }

   void
   SpillAtBoundary
   (
      GraphColor::Tile *        tile,
      llvm::SparseBitVector<> * boundarSpillAliasTagSet
   );

   const llvm::TargetRegisterClass *
   GetRegisterCategory
   (
      unsigned Reg
   );

   const llvm::TargetRegisterClass *
   GetBaseRegisterCategory
   (
      unsigned Reg
   )
   {
      if (TRI->isVirtualRegister(Reg))
         MF->getRegInfo().getRegClass(Reg);
      else
         return TRI->getLargestLegalSuperClass(TRI->getMinimalPhysRegClass(Reg), *MF);

   }


public:

   GraphColor::Tile *                          ActiveTile;
   Tiled::VR::Info *                           VrInfo;
   GraphColor::IdVector *                      AliasTagToIdVector;
   llvm::SparseBitVector<> *                   AllRegisterTags;

   unsigned                                    AntiPreferenceSearchDepth;

   const llvm::TargetRegisterClass *           BaseFloatRegisterCategory;
   const llvm::TargetRegisterClass *           BaseIntegerRegisterCategory;
   Tiled::Profile::Count                       BiasedEntryCount;
   unsigned                                    CallCountConservativeThreshold;
   llvm::SparseBitVector<> *                   CalleeSaveRegisterAliasTagSet;
   llvm::SparseBitVector<> *                   CallerSaveSpillLiveRangeIdSetScratch;
   GraphColor::AllocatorCostModel *            ColdCostModel;
   Graphs::FrameIndex                          CostSpillSymbol;
   //property int                              CycleMargin { cycleMargin }
   bool                                        DoFavorSpeed;

   llvm::SparseBitVector<> *                   DoNotAllocateRegisterAliasTagBitVector;
   llvm::SparseBitVector<> *                   DoNotSpillAliasTagBitVector;
   llvm::SparseBitVector<> *                   callKilledRegBitVector;
   bool                                        DoPassTwoReallocate;
   /*
   bool
   DoShrinkWrap()
   {
      return (this->TileGraph->ShrinkWrapTile != nullptr);
   }*/

   bool                                        DoUseRegisterCosts;
   Graphs::FrameIndex                          DummySpillSymbol;
   unsigned                                    EarlyOutTraceBlockSizeLimit { 50 };
   Tiled::Profile::Count                       EntryProfileCount;
   Graphs::NodeFlowOrder *                     FlowExtendedBasicBlockReversePostOrder;

   Graphs::NodeFlowOrder *                     FlowReversePostOrder;

   Graphs::FlowGraph *                         FunctionUnit;
   Graphs::OperandToSlotMap *                  LocalIdMap;
   llvm::MachineFunction *                     MF;
   const llvm::TargetRegisterInfo *            TRI;
   llvm::VirtRegMap  *                         VRM;
   llvm::SlotIndexes *                         Indexes;
   llvm::MachineLoopInfo *                     LoopInfo;
   llvm::AliasAnalysis *                       AA;
   llvm::MachineBlockFrequencyInfo *           MbbFreqInfo;
   llvm::MachineBranchProbabilityInfo *        MBranchProbInfo;
   llvm::MCInstrDesc                           MCID_ENTERTILE;
   llvm::MCInstrDesc                           MCID_EXITTILE;
   llvm::MCInstrDesc                           MCID_MOVE;

   llvm::SparseBitVector<> *                   GlobalAliasTagSet;
   GraphColor::IdVector *                      GlobalAliasTagToIdVector;
   GraphColor::AvailableExpressions *          GlobalAvailableExpressions;
   unsigned                                    GlobalLiveRangeConservativeThreshold;
   unsigned                                    GlobalLiveRangeCount();
   GraphColor::LiveRangeVector *               GlobalLiveRangeVector;
   llvm::SparseBitVector<> *                   GlobalSpillLiveRangeIdSetScratch;

   GraphColor::AllocatorCostModel *            HotCostModel;
   Tiled::Cost                                 InfiniteCost;

   Tiled::Cost                                 IntegerMoveCost;
   //bool                                      IsX87SupportEnabled;
   bool                                        IsShrinkWrappingEnabled;
   bool                                        IsTilingEnabled;
   GraphColor::Liveness *                      Liveness;
   llvm::SparseBitVector<> *                   LivenessScratchBitVector;
   unsigned                                    MaximumRegisterCategoryId;
   llvm::SparseBitVector<> *                   MentionedBaseRegisterCategoryBitVector;
   unsigned                                    NodeCountThreshold;
   unsigned                                    NumberCalls;
   unsigned                                    NumberGlobalCallSpanningLiveRanges;
   GraphColor::Pass                            Pass;
   llvm::SparseBitVector<> *                   PendingSpillAliasTagBitVector;
   unsigned                                    PreferenceSearchDepth;

   std::vector<unsigned> *                     RegisterCategoryIdToAllocatableRegisterCount;

   GraphColor::CostVector *                    RegisterCostVector;
   llvm::SparseBitVector<> *                   ScratchBitVector1;
   llvm::SparseBitVector<> *                   ScratchBitVector2;
   llvm::SparseBitVector<> *                   ScratchBitVector3;
   unsigned                                    SearchLiveRangeId;
   unsigned                                    SearchNumber;
   GraphColor::SpillOptimizer *                SpillOptimizer;
   unsigned                                    inFunctionRemovedLocalLRs;

   Graphs::OperandToSlotMap *                  IdToSlotMap;
   TargetRegisterAllocInfo *                   TargetRegisterAllocator;
   GraphColor::TileGraph *                     TileGraph;
   Tiled::Cost                                 TotalSpillCost;
   Tiled::Cost                                 TotalTransferCost;
   llvm::SparseBitVector<> *                   UndefinedRegisterAliasTagBitVector;
   Tiled::Cost                                 UnitCost;
   Tiled::Cost                                 MemorySpillCost;
   Tiled::Cost                                 ZeroCost;

private:

   void
   Allocate
   (
      GraphColor::Tile * tile
   );

   void AnnotateIRForCalleeSave();

   void
   Assign
   (
      GraphColor::Tile * tile
   );

   void
   BeginBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   );

   void
   BeginBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      const bool                doInsert
   );

   void
   Build
   (
      GraphColor::Tile * tile
   );

   void
   MarkLiveRangesSpanningCall
   (
      GraphColor::Tile * tile
   );

   void BuildFlowGraph();

   void BuildGlobalAvailable();

   void BuildGlobalConflictGraph();

   void BuildGlobalLiveRanges();

   void BuildGlobalPreferenceGraph();

   void BuildLiveness();

   void BuildLoopGraph();

   void BuildTileGraph();

   void
   ClearAllocations
   (
      GraphColor::Tile * tile
   );

   bool
   Color
   (
      GraphColor::Tile * tile
   );

   void
   CombineRegisterPreference
   (
      RegisterAllocator::PreferenceCombineMode        combineMode,
      GraphColor::AllocatorCostModel *                costModel,
      GraphColor::PhysicalRegisterPreferenceVector *  summaryRegisterPreferenceVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
   );

   void
   CombineRegisterPreference
   (
      RegisterAllocator::PreferenceCombineMode        combineMode,
      GraphColor::AllocatorCostModel *                costModel,
      GraphColor::PhysicalRegisterPreferenceVector *  summaryRegisterPreferenceVector,
      unsigned                                        reg,
      Tiled::Cost                                     cost
   );

   void ComputeCalleeSaveHeuristic();

   void
   ComputeCalleeSaveHeuristic
   (
      GraphColor::Tile * tile
   );

   unsigned
   ComputePreference
   (
      GraphColor::Tile *        tile,
      GraphColor::LiveRange *   liveRange,
      llvm::SparseBitVector<> * allocatableRegisterTagBitVector
   );

   unsigned
   ConservativeThreshold
   (
      GraphColor::Tile * tile
   );

   void
   Cost
   (
      GraphColor::Tile * tile
   );

   void CostGlobalLiveRanges();

   void
   CostGlobals
   (
      GraphColor::Tile * tile
   );

   void
   CostSummary
   (
      GraphColor::Tile * tile
   );

   void DeleteLiveness();

   void DeleteTileGraph();

   void
   EndBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   );

   void
   EndBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      bool                      doInsert
   );

   int
   FindRegisterPreference
   (
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector,
      unsigned                                        reg
   );

   void
   GetAntiPreferredRegisters
   (
      GraphColor::Tile *                              tile,
      GraphColor::LiveRange *                         liveRange,
      unsigned                                        requiredReg,
      GraphColor::LiveRangeVector *                   preferenceLiveRangeVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector,
      int                                             searchDepth,
      llvm::SparseBitVector<> *                       allocatableRegisterTagBitVector,
      llvm::SparseBitVector<> *                       antiPreferencePrunedAllocatableRegisterTagBitVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerAntiPreferenceVector
   );

   Profile::Count
   GetBiasedFrequency
   (
      Profile::Count count
   );

   unsigned
   GetLeastAntiPreferredRegister
   (
      GraphColor::LiveRange *                         liveRange,
      GraphColor::AllocatorCostModel *                costModel,
      Tiled::Cost *                                   outCost,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
   );

   unsigned
   GetMostPreferredRegister
   (
      GraphColor::AllocatorCostModel *                costModel,
      Tiled::Cost *                                   outCost,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
   );

   void
   GetPreferredLiveRanges
   (
      GraphColor::Tile *             tile,
      GraphColor::LiveRange *        liveRange,
      unsigned                       searchDepth,
      GraphColor::LiveRangeVector *  preferenceLiveRangeVector
   );

   void
   GetPreferredLiveRangesRecursively
   (
      GraphColor::Tile *            tile,
      GraphColor::LiveRange *       liveRange,
      unsigned                      searchDepth,
      GraphColor::LiveRangeVector * preferenceLiveRangeVector
   );

   void
   GetPreferredRegisters
   (
      GraphColor::Tile *                              tile,
      GraphColor::LiveRange *                         liveRange,
      unsigned                                        requiredReg,
      int                                             searchDepth,
      llvm::SparseBitVector<> *                       allocatableRegisterTagBitVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
   );

   void
   GetPreferredRegistersRecursively
   (
      GraphColor::Tile *                              tile,
      GraphColor::LiveRange *                         liveRange,
      unsigned                                        requiredReg,
      int                                             searchDepth,
      llvm::SparseBitVector<> *                       allocatableRegisterTagBitVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
   );

   void Initialize();

   void
   InitializeRegisterCosts
   (
      GraphColor::CostVector * registerCostVector
   );

   void
   InsertCompensationCode
   (
      GraphColor::Tile * tile
   );

   void
   Rebuild
   (
      GraphColor::Tile * tile
   );

   void
   RewriteRegister
   (
      llvm::MachineOperand *  operand,
      unsigned                reg
   );

   void
   RewriteRegistersInBlock
   (
      GraphColor::Tile *        tile,
      llvm::MachineBasicBlock * block,
      VR::Info *                vrInfo
   );

   void
   RewriteRegisters
   (
      GraphColor::Tile * tile
   );

   void ScheduleTileGraph();

   Tiled::Boolean
   Select
   (
      GraphColor::Tile * tile
   );

   unsigned
   SelectPreferredRegister
   (
      GraphColor::LiveRange *                         liveRange,
      GraphColor::AllocatorCostModel *                costModel,
      Tiled::Cost *                                   outCost,
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector,
      GraphColor::PhysicalRegisterPreferenceVector *  registerAntiPreferenceVector
   );

   void
   SetRegisterPreferenceCost
   (
      GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
      GraphColor::AllocatorCostModel *               costModel,
      Tiled::Cost                                    cost
   );

   void
   Simplify
   (
      GraphColor::Tile * tile
   );

   void
   Simplify
   (
      GraphColor::Tile *      tile,
      GraphColor::LiveRange * liveRange
   );

   unsigned
   SimplifyColorable
   (
      GraphColor::Tile * tile
   );

   unsigned
   SimplifyOptimistic
   (
      GraphColor::Tile * tile
   );

   unsigned
   SimplifyPessimistic
   (
      GraphColor::Tile * tile
   );

   bool
   Spill
   (
      GraphColor::Tile * tile
   );

   bool
   Spill
   (
      GraphColor::Tile *        tile,
      llvm::SparseBitVector<> * spillAliasTagBitVector
   );

   void
   SpillGlobals
   (
      GraphColor::Tile * tile
   );

   void
   SpillPending
   (
      GraphColor::Tile *        tile,
      GraphColor::LiveRange *   liveRange,
      llvm::SparseBitVector<> * pendingSpillAliasTagBitVector
   );

   /*
   bool
   SpillWriteThrough
   (
      GraphColor::Tile * tile
   );*/

   void
   Summarize
   (
      GraphColor::Tile * tile
   );

   void Terminate();

   void
   UpdateRegisterPreference
   (
      GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
      unsigned                                       index,
      unsigned                                       reg,
      Tiled::Cost                                    cost
   );
   
   void
   UpdateSummary
   (
      GraphColor::Tile * tile
   );

private:

   GraphColor::BooleanVector *                          BooleanVector;
   unsigned                                             CalleeSaveConservativeThreshold;
   unsigned                                             ConservativeControlThreshold;
   unsigned                                             ConservativeFatThreshold;
   unsigned                                             ConservativeLoopThreshold;
   GraphColor::CostValueVector *                        CostValueVector;
   unsigned                                             IterationLimit;
   unsigned                                             MaximumRegisterId;
   Tiled::Profile::Count                                ProfileBasisCount;
   GraphColor::PhysicalRegisterPreferenceVector *       RegisterAntiPreferenceVector;
   GraphColor::SparseBitVectorVector *                  RegisterCategoryIdToAllocatableAliasTagBitVector;
   GraphColor::PhysicalRegisterPreferenceVector *       RegisterPreferenceVector;
   std::vector<unsigned> *                              RegisterPressureVector;

   GraphColor::Heuristic                                SimplificationHeuristic;
   GraphColor::IdVector *                               SimplificationStack;
   GraphColor::PhysicalRegisterPreferenceVectorVector * TemporaryRegisterPreferenceVectors;
   std::map<int, unsigned>                              BlockIdToMaxLocalRegister;

   static std::map<llvm::MachineInstr*, unsigned>  instruction2TileId;
};

#if 0
comment Allocator::AliasTagToIdVector
{
   // Used to map from alias tag to live range id on the bottom up allocate pass
   // Used to map from alias tag to register id on the top down assign pass
}

comment Allocator::TileGraph
{
   // Tile graph (tree).
}

comment Allocator::RegisterCostVector
{
   // Global costs as currently known for registers by Id.  Includes cost info for caller/callee
   // save primarily.
}

#endif

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_ALLOCATOR_H
