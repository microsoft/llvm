//===-- GraphColor/Tile.h ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_TILE_H
#define TILED_GRAPHCOLOR_TILE_H

#include "../Graphs/Graph.h"
#include "../Graphs/UnionFind.h"
#include "../GraphColor/Graph.h"
#include "llvm/CodeGen/MachineLoopInfo.h"

namespace Tiled
{

namespace RegisterAllocator
{
namespace GraphColor
{

//------------------------------------------------------------------------------
// Description:
//
//    Enumeration of tile kinds.
//
//------------------------------------------------------------------------------

enum class TileKind
{
   Invalid,
   Root,
   EBB,
   ShrinkWrap,
   Loop,
   Acyclic,
   Fat
};
   
class Allocator;
class AllocatorCostModel;
class AvailableExpressions;
class ConflictGraph;
class LiveRange;
class PreferenceGraph;
class SummaryConflictGraph;
class SummaryPreferenceGraph;
class Tile;
class TileExtensionObject;
enum class BlockKind;
enum class Decision;
enum class Pass;
   
typedef std::map<int, GraphColor::TileExtensionObject*> BlockToTileExtensionObjectMap;

//------------------------------------------------------------------------------
//
// Description:
//
//    The tile tree/graph structure for the hierarchical register allocator.
//
//------------------------------------------------------------------------------

class TileGraph
{

public:

   static GraphColor::TileGraph *
   New
   (
      GraphColor::Allocator * allocator
   );

public:

   void BuildGraph();

   void BuildTileForFunction();

   void BuildTileOrders();

   bool
   BuildTilesForFat
   (
      GraphColor::Tile * parentTile
   );

   bool
   BuildTilesForFat
   (
      GraphColor::Tile *        parentTile,
      llvm::MachineBasicBlock * block
   );

   bool
   BuildTilesForLoops
   (
      GraphColor::Tile *      parentTile,
      GraphColor::LoopList *  loopList
   );

   bool
   BuildTilesForShrinkWrapping
   (
      GraphColor::Tile * parentTile
   );

   bool
   BuildTilesForSize
   (
      GraphColor::Tile * parentTile
   );

   bool
   CanAllPredecessorsBeTileEntryEdge
   (
      llvm::MachineBasicBlock * block
   );

   bool
   CanAllSuccessorsBeTileExitEdge
   (
      llvm::MachineBasicBlock * block
   );

   bool
   CanBeTileEntryEdge
   (
      Graphs::FlowEdge& edge
   );

   bool
   CanBeTileExitEdge
   (
      Graphs::FlowEdge& edge
   );

   bool
   EdgeIsSplittable
   (
      Graphs::FlowEdge * edge
   );

   bool
   CanBuildTileForLoop
   (
      llvm::MachineLoop * loop
   );

   bool
   CanMakeEBBBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   CanSplitEdge
   (
      Graphs::FlowEdge& edge
   );

   unsigned
   ComputeNonEHBlockSize
   (
      llvm::MachineBasicBlock *  block,
      llvm::MachineBasicBlock ** headBlock
   );

   int
   FindEarlyOutPath
   (
      llvm::MachineBasicBlock *  startBlock,
      llvm::MachineBasicBlock *  block,
      llvm::MachineBasicBlock ** lastBlock,
      llvm::MachineBasicBlock *  endBlock,
      llvm::SparseBitVector<> *  blockSet
   );

   llvm::SparseBitVector<> *
   GetLiveness
   (
      Graphs::FlowEdge&         edge,
      bool                      doSuccessor
   );

   GraphColor::Tile *
   GetNestedTile
   (
      llvm::MachineBasicBlock * block
   );

   llvm::SparseBitVector<> *
   GetPredecessorLiveness
   (
      Graphs::FlowEdge& edge
   )
   {
      return this->GetLiveness(edge, false);
   }

   llvm::SparseBitVector<> *
   GetSuccessorLiveness
   (
      Graphs::FlowEdge& edge
   )
   {
      return this->GetLiveness(edge, true);
   }

   GraphColor::Tile *
   GetTile
   (
      llvm::MachineBasicBlock * block
   );

   GraphColor::Tile *
   GetTileByBlockId
   (
      unsigned blockId
   );

   GraphColor::Tile *
   GetTileByTileId
   (
      unsigned tileId
   );

   void Initialize();

   void
   InsertTileBoundaryBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile,
      bool                      isEntry
   );

   llvm::MachineBasicBlock *
   InsertTileBoundaryBlock
   (
      Graphs::FlowEdge&         edge,
      GraphColor::Tile *        tile,
      bool                      isEntry
   );

   void
   InsertTileEnterBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   )
   {
      this->InsertTileBoundaryBlock(block, tile, true);
   }

   llvm::MachineBasicBlock *
   InsertTileEnterBlock
   (
      Graphs::FlowEdge&  edge,
      GraphColor::Tile * tile
   )
   {
      return this->InsertTileBoundaryBlock(edge, tile, true);
   }

   void
   InsertTileExitBlock
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   )
   {
      this->InsertTileBoundaryBlock(block, tile, false);
   }

   llvm::MachineBasicBlock *
   InsertTileExitBlock
   (
      Graphs::FlowEdge&  edge,
      GraphColor::Tile * tile
   )
   {
      return this->InsertTileBoundaryBlock(edge, tile, false);
   }

   bool
   IsValidTile
   (
      GraphColor::Tile * tile
   );

   void MergeAdjacentTiles();

   void
   RemoveTile
   (
      GraphColor::Tile * tile
   );

   void
   SetTile
   (
      llvm::MachineBasicBlock * block,
      GraphColor::Tile *        tile
   );

   bool
   ShouldMakeFatTile
   (
      GraphColor::Tile * tile
   );

   llvm::MachineBasicBlock *
   SplitEntryEdge
   (
      Graphs::FlowEdge& edge
   );

   llvm::MachineBasicBlock *
   SplitExitEdge
   (
      Graphs::FlowEdge& edge
   );

   void
   storePatchRecord
   (
      const std::pair<unsigned, unsigned>&  key
   );

   bool
   definePatch
   (
      const std::pair<unsigned, unsigned>&  key,
      llvm::MachineBasicBlock*              boundaryBlock,
      bool                                  isEnterTile
   );

   llvm::MachineBasicBlock*
   getPatch
   (
      const std::pair<unsigned, unsigned>&  key,
      bool& wasEnterTile
   );

public:

   bool                       HasSplitCriticalEdges;
   unsigned                   InstructionCount;
   GraphColor::TileList *     PostOrderTileList;
   GraphColor::TileList *     PreOrderTileList;
   int                        TileCount;
   GraphColor::Tile *         RootTile;
   Graphs::FlowGraph *        FunctionFlowGraph;
   GraphColor::Allocator *    Allocator;

   static BlockToTileExtensionObjectMap BlockToTileExtension;

protected:
   friend GraphColor::Tile;

   GraphColor::TileVector *  BlockIdToTileVector;
   GraphColor::Tile *        ShrinkWrapTile;
   GraphColor::TileVector *  TileVector;

private:

   typedef std::map< std::pair<unsigned, unsigned>, std::pair<llvm::MachineBasicBlock*, bool > >  BoundaryBlockPatchMap;

   llvm::SparseBitVector<> * VisitedBitVector;

   BoundaryBlockPatchMap *   edgesToPatch;
};

//------------------------------------------------------------------------------
//
// Description:
//
//    The tile context for the hierarchical graph coloring register allocator.
//
//------------------------------------------------------------------------------

class Tile
{

public:

   static GraphColor::Tile *
   New
   (
      GraphColor::TileGraph * tileGraph
   );

   static GraphColor::Tile *
   New
   (
      GraphColor::TileGraph * tileGraph,
      Graphs::FlowGraph *     flowGraph
   );

   static GraphColor::Tile *
   New
   (
      GraphColor::Tile *  parentTile,
      llvm::MachineLoop * loop
   );

   static GraphColor::Tile *
   New
   (
      GraphColor::Tile *        parentTile,
      llvm::MachineBasicBlock * block,
      GraphColor::TileKind      tileKind
   );

   static GraphColor::Tile *
   New
   (
      GraphColor::Tile *   parentTile,
      GraphColor::Tile *   nestedTile,
      GraphColor::TileKind tileKind
   );

public:

   bool
   AddBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   AddChild
   (
      GraphColor::Tile * tile
   );

   void
   AddEntryBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   AddEntryEdge
   (
      Graphs::FlowEdge& edge
   );

   void
   AddExitBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   AddExitEdge
   (
      Graphs::FlowEdge& edge
   );

   GraphColor::LiveRange *
   AddLiveRange
   (
      int       aliasTag,
      unsigned  reg
   );

   void
   AddNestedEntryBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   AddNestedExitBlock
   (
      llvm::MachineBasicBlock * block
   );

   void
   AddNewBlock
   (
      llvm::MachineBasicBlock * block
   );

   GraphColor::LiveRange *
   AddRegisterLiveRange
   (
      unsigned reg
   );

   GraphColor::LiveRange *
   AddSummaryLiveRange
   (
      unsigned   aliasTag,
      unsigned   reg
   );

   void
   Allocate
   (
      GraphColor::LiveRange * liveRange,
      unsigned                reg
   );

   void
   BeginIteration
   (
      GraphColor::Pass pass
   );

   void
   BeginPass
   (
      GraphColor::Pass pass
   );

   void BuildSummaryConflicts();

   void BuildSummaryLiveRanges();

   void BuildSummaryPreferences();

   void
   ClearAliasTagToSummaryIdMap
   (
      unsigned aliasTag
   );

   void ClearAllocations();

   void ComputeTileFrequency();

   void CountGlobalTransparentSpills();

   void
   EndIteration
   (
      GraphColor::Pass pass
   );

   void
   EndPass
   (
      GraphColor::Pass pass
   );

   llvm::MachineInstr *
   FindNextInstruction
   (
      llvm::MachineBasicBlock *                block,
      llvm::MachineBasicBlock::instr_iterator  i,
      unsigned                                 opcode
   );

   llvm::MachineInstr *
   FindPreviousInstruction
   (
      llvm::MachineBasicBlock *                        block,
      llvm::MachineBasicBlock::reverse_instr_iterator  i,
      unsigned                                         opcode
   );

   llvm::MachineInstr *
   FindEnterTileInstruction
   (
      llvm::MachineBasicBlock * enterTileBlock
   );

   llvm::MachineInstr *
   FindExitTileInstruction
   (
      llvm::MachineBasicBlock * exitTileBlock
   );

   unsigned
   GetAllocatedRegister
   (
      unsigned aliasTag
   );

   GraphColor::Decision
   GetGlobalDecision
   (
      unsigned globalLiveRangeTag
   );

   GraphColor::LiveRange *
   GetLiveRange
   (
      unsigned aliasTag
   );

   GraphColor::LiveRange *
   GetLiveRange
   (
      llvm::MachineOperand * operand
   );

   GraphColor::LiveRange *
   GetLiveRangeById
   (
      unsigned liveRangeId
   );

   GraphColor::LiveRangeEnumerator * GetLiveRangeEnumerator();

   GraphColor::LiveRangeVector * GetLiveRangeOrder();

   GraphColor::LiveRange *
   GetRegisterSummaryLiveRange
   (
      unsigned aliasTag
   );

   unsigned
   GetSummaryDegree
   (
      GraphColor::LiveRange * liveRange
   );

   GraphColor::LiveRange *
   GetSummaryLiveRange
   (
      unsigned aliasTag
   );

   GraphColor::LiveRange *
   GetSummaryLiveRangeById
   (
      unsigned liveRangeId
   );

   GraphColor::LiveRangeEnumerator * GetSummaryLiveRangeEnumerator();

   unsigned
   GetSummaryRegister
   (
      unsigned aliasTag
   );

   bool
   HasGlobalReference
   (
      unsigned globalAliasTag
   );

   bool
   HasGlobalReference
   (
      GraphColor::LiveRange * globalLiveRange
   );

   bool
   HasTransitiveGlobalReference
   (
      unsigned globalAliasTag
   );

   bool
   HasTransitiveGlobalReference
   (
      GraphColor::LiveRange * globalLiveRange
   );

   void Initialize();

   void InsertEntryAndExit();

   bool
   IsAllocated
   (
      unsigned aliasTag
   )
   {
      return (this->GetAllocatedRegister(aliasTag) != VR::Constants::InvalidReg);
   }

   bool
   IsAssigned
   (
      llvm::MachineOperand * operand
   );

   bool
   IsBodyBlockExclusive
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsBodyBlockInclusive
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsCallerSave
   (
      Alias::Tag globalAliasTag
   )
   { 
      return (this->CallerSaveAliasTagSet->test(globalAliasTag));
   }

   static bool
   IsEnterTileInstruction
   (
      llvm::MachineInstr * instruction
   );

   bool
   IsEntryBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsEntryEdge
   (
      Graphs::FlowEdge& edge
   );

   bool
   IsExitBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsExitEdge
   (
      Graphs::FlowEdge& edge
   );

   static bool
   IsExitTileInstruction
   (
      llvm::MachineInstr * instruction
   );

#ifdef ARCH_WITH_FOLDS
   bool
   IsFold
   (
      unsigned globalAliasTag
   )
   {
      return this->FoldAliasTagSet->test(globalAliasTag);
   }
#endif

   bool
   IsMemory
   (
      unsigned globalAliasTag
   )
   {
      return this->MemoryAliasTagSet->test(globalAliasTag);
   }

   bool
   IsNestedEntryBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsNestedExitBlock
   (
      llvm::MachineBasicBlock * block
   );

   bool
   IsRecalculate
   (
      unsigned globalAliasTag
   )
   {
      return (this->RecalculateAliasTagSet->test(globalAliasTag));
   }

   bool
   IsSpilled
   (
      unsigned globalAliasTag
   )
   {
      return (this->IsMemory(globalAliasTag) || this->IsRecalculate(globalAliasTag)
#ifdef ARCH_WITH_FOLDS
                                             || this->IsFold(globalAliasTag)
#endif
            );
   }

   static bool
   IsTileBoundaryInstruction
   (
      llvm::MachineInstr * instruction
   );

   bool
   IsTransparent
   (
      unsigned globalAliasTag
   );

   void
   MapAliasTagToSummaryId
   (
      unsigned aliasTag,
      unsigned summaryLiveRangeId
   );

   void
   MapLiveRangesToSummaryLiveRanges
   (
      llvm::SparseBitVector<> * rewriteAliasTagBitVector
   );

   void PropagateGlobalAppearances();

   void PruneSummaryLiveRanges();

   void Rebuild();

   void
   RemoveAvailable
   (
      llvm::MachineOperand * operand
   );

   void
   RemoveChild
   (
      GraphColor::Tile * tile
   );

   bool
   RemoveEntryEdge
   (
      Graphs::FlowEdge& edge
   );

   bool
   RemoveExitEdge
   (
      Graphs::FlowEdge& edge
   );

   void ResetLiveRangeVector();

   void ReuseLiveRangesAsSummary();

   void
   RewriteConstrainedRegisters
   (
      llvm::SparseBitVector<> * rewriteAliasTagBitVector
   );

   void
   SetGlobalDecision
   (
      unsigned              globalLiveRangeAliasTag,
      GraphColor::Decision  decision
   );

   void UnionAllocated();

   void UpdateEnterTileAndExitTileInstructions();

   void UpdateGlobalConflicts();

   void pruneCompensatingCopysOfTile();

public:

   Tiled::VR::Info *                    VrInfo;

   GraphColor::IdVector *               AliasTagToIdVector;
   llvm::SparseBitVector<> *            AllocatableRegisterAliasTagSet;
   GraphColor::Allocator *              Allocator;
   GraphColor::AvailableExpressions *   AvailableExpressions;
   Graphs::MachineBasicBlockVector *    BlockVector;
   unsigned                             BodyBlockCount() const { return this->BodyBlockSet->count(); }
   llvm::SparseBitVector<> *            BodyBlockSet;
   Profile::Count                       BoundaryProfileCount;
   llvm::SparseBitVector<> *            CallerSaveAliasTagSet;
   UnionFind::Tree *                    ColoredRegisterTree;
   GraphColor::ConflictGraph *          ConflictGraph;
   unsigned                             DependencyCount;
   bool                                 DoUseRegisterCosts;
   Graphs::FrameIndex                   DummySpillSymbol;
   Graphs::MachineBasicBlockList *      EntryBlockList;
   llvm::SparseBitVector<> *            EntryBlockSet;
   unsigned                             EntryCount() const { return EntryBlockList->size(); }
   Graphs::FlowEdgeList *               EntryEdgeList;
   Graphs::MachineBasicBlockList *      ExitBlockList;
   llvm::SparseBitVector<> *            ExitBlockSet;
   unsigned                             ExitCount() const { return ExitBlockList->size(); }
   Graphs::FlowEdgeList *               ExitEdgeList;
   unsigned                             ExtendedBasicBlockCount;
   llvm::SparseBitVector<> *            FloatMaxKillBlockBitVector;
   llvm::SparseBitVector<> *            FoldAliasTagSet;
   Tiled::CostValue                     Frequency;
   Graphs::FlowGraph *                  FunctionFlowGraph;
   llvm::MachineFunction *              MF;
   llvm::SparseBitVector<> *            GlobalAliasTagSet;
   llvm::SparseBitVector<> *            GlobalDefinedAliasTagSet;
   llvm::SparseBitVector<> *            GlobalGenerateAliasTagSet;
   llvm::SparseBitVector<> *            GlobalKillAliasTagSet;
   llvm::SparseBitVector<> *            GlobalLiveInAliasTagSet;
   llvm::SparseBitVector<> *            GlobalLiveOutAliasTagSet;

   GraphColor::SparseBitVectorVector *  GlobalLiveRangeConflictsVector;

   llvm::SparseBitVector<> *            GlobalSpillAliasTagSet;
   llvm::SparseBitVector<> *            GlobalTransitiveDefinedAliasTagSet;
   llvm::SparseBitVector<> *            GlobalTransitiveUsedAliasTagSet;
   unsigned                             GlobalTransparentSpillCount;
   llvm::SparseBitVector<> *            GlobalUsedAliasTagSet;
   Tiled::Cost                          GlobalWeightCost;
   bool                                 HasAssignedRegisters;
   llvm::MachineBasicBlock *            HeadBlock;
   int                                  Id;
   unsigned                             InstructionCount;
   llvm::SparseBitVector<> *            IntegerMaxKillBlockBitVector;
   bool                                 IsAcyclic() { return (this->TileKind == GraphColor::TileKind::Acyclic); }
   bool                                 IsFat() { return (this->TileKind == GraphColor::TileKind::Fat); }
   bool                                 IsInitialized;
   bool                                 IsLeaf() { return (this->NestedTileSet->empty()); }
   bool                                 IsLoop() { return (this->TileKind == GraphColor::TileKind::Loop); }
   bool                                 IsMapped;
   bool                                 IsRoot() { return (this->TileKind == GraphColor::TileKind::Root); }
   bool                                 IsShrinkWrap() { return (this->TileKind == GraphColor::TileKind::ShrinkWrap); }
 
   unsigned                             Iteration;
   llvm::SparseBitVector<> *            KilledRegisterAliasTagSet;

   typedef std::vector<llvm::MachineInstr*> CopyVector;
   typedef std::map<unsigned, CopyVector >  BlockToCopysMap;
   llvm::SparseBitVector<> *            RegistersOverwrittentInTile;
   BlockToCopysMap                      TransparentBoundaryCopys;

   unsigned
   LiveRangeCount() {
      return (this->LiveRangeVector->size() - 1);
   }

   Graphs::OperandToSlotMap *           LocalIdMap;

   //[check]
   llvm::MachineLoop *                  Loop;

   llvm::SparseBitVector<> *            MemoryAliasTagSet;
   llvm::SparseBitVector<> *            NestedEntryBlockSet;
   llvm::SparseBitVector<> *            NestedExitBlockSet;
   llvm::SparseBitVector<> *            NestedTileAliasTagSet;
   GraphColor::TileList *               NestedTileList;
   llvm::SparseBitVector<> *            NestedTileSet;
   llvm::SparseBitVector<> *            NonBlockLocalAliasTagSet;
   unsigned                             NumberCallSpanningLiveRanges;
   unsigned                             NumberCalls;
 
   GraphColor::Tile *                   ParentTile;
   GraphColor::Pass                     Pass;

   GraphColor::PreferenceGraph *        PreferenceGraph;
   llvm::SparseBitVector<> *            RecalculateAliasTagSet;
   llvm::SparseBitVector<> *            SpilledAliasTagSet;
   GraphColor::SummaryConflictGraph *   SummaryConflictGraph;
   
   unsigned                           
   SummaryLiveRangeCount() {
      return (this->SummaryLiveRangeVector->size() - 1);
   }

   GraphColor::SummaryPreferenceGraph * SummaryPreferenceGraph;
   Graphs::OperandToSlotMap *           IdToSlotMap;
   GraphColor::TileGraph *              TileGraph;
   GraphColor::TileKind                 TileKind;

protected:

   void FreeMemory();

public:
   
   GraphColor::AllocatorCostModel *     CostModel;
   GraphColor::LiveRangeVector *        PreferenceLiveRangeVector;

private:

   GraphColor::LiveRangeVector *        LiveRangeVector;
   GraphColor::AliasTagToIdMap *        SummaryAliasTagToIdMap;
   GraphColor::LiveRangeVector *        SummaryLiveRangeVector;

};

#if 0
comment Tile::PreferenceLiveRangeVector
{
   // Live range vector for storing the results of preference search.
}

comment Tile::LiveRangeVector
{
   // vector of region live ranges
}
#endif

//------------------------------------------------------------------------------
//
// Description:
//
//    Each tile enter/exit block is mapped to an extension object
//    with additional information.
//
//------------------------------------------------------------------------------

class TileExtensionObject
{
public:

   llvm::SparseBitVector<> * GlobalAliasTagSet;
   GraphColor::Tile *        Tile;

public:

   static TileExtensionObject *
   New()
   {
      TileExtensionObject * teo = new TileExtensionObject();
      return teo;
   }

   static TileExtensionObject *
   GetExtensionObject
   (
      llvm::MachineBasicBlock * block
   );

   static void
   AddExtensionObject
   (
      llvm::MachineBasicBlock * block,
      TileExtensionObject *     object
   );

   static void
   RemoveExtensionObject
   (
      llvm::MachineBasicBlock * block,
      TileExtensionObject *     object
   );
};


} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_TILE_H
