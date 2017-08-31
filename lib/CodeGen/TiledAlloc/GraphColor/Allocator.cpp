//===-- GraphColor/Allocator.cpp --------------------------------*- C++ -*-===//
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

#include "Allocator.h"
#include "ConflictGraph.h"
#include "CostModel.h"
#include "Liveness.h"
#include "LiveRange.h"
#include "PreferenceGraph.h"
#include "SpillOptimizer.h"
#include "TargetRegisterAllocInfo.h"
#include "../Graphs/Graph.h"

#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Timer.h"
#include "llvm/Target/TargetOpcodes.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"

#define DEBUG_TYPE "tiled"

const char TimerGroupName[] = "regalloc";
const char TimerGroupDescription[] = "Register Allocation";

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

void
BlockInfo::ClearAll()
{
   this->IsAnticipatedIn = false;
   this->IsAnticipatedOut = false;
   this->IsAvailableIn = false;
   this->IsAvailableOut = false;
   this->IsDefinition = false;
   this->IsDownDefinedIn = false;
   this->IsDownDefinedOut = false;
   this->IsExit = false;
   this->IsLiveIn = false;
   this->IsLiveOut = false;
   this->IsKill = false;
   this->IsUse = false;
   this->IsUpExposedIn = false;
   this->IsUpExposedOut = false;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for the register allocator.
//
// Arguments:
//
//    functionUnit - function unit being allocated
//
// Returns:
//
//    GraphColor::Allocator object.
//
//------------------------------------------------------------------------------

GraphColor::Allocator *
Allocator::New
(
   Graphs::FlowGraph *                  functionUnit,
   llvm::VirtRegMap  *                  vrm,
   llvm::SlotIndexes *                  slotIndexes,
   llvm::MachineLoopInfo *              mli,
   llvm::AliasAnalysis *                aa,
   llvm::MachineBlockFrequencyInfo *    mbfi,
   llvm::MachineBranchProbabilityInfo * mbpi
)
{
   Graphs::OperandToSlotMap *  localIdMap = new Graphs::OperandToSlotMap();
   GraphColor::Allocator * allocator = new GraphColor::Allocator();

   allocator->FunctionUnit = functionUnit;
   allocator->VRM = vrm;
   allocator->Indexes = slotIndexes;
   allocator->LoopInfo = mli;
   allocator->AA = aa;
   allocator->MbbFreqInfo = mbfi;
   allocator->MBranchProbInfo = mbpi;
   functionUnit->MbbFreqInfo = mbfi;
   functionUnit->MBranchProbInfo = mbpi;
   Graphs::FlowEdge::flowGraph = functionUnit;

   const llvm::TargetInstrInfo& TII = *(functionUnit->machineFunction->getSubtarget().getInstrInfo());

   llvm::MCInstrDesc mcid_move(TII.get(llvm::TargetOpcode::COPY));
   llvm::MCInstrDesc mcid_entertile(TII.get(llvm::TargetOpcode::ENTERTILE));
   llvm::MCInstrDesc mcid_exittile(TII.get(llvm::TargetOpcode::EXITTILE));
   allocator->MCID_MOVE = mcid_move;
   allocator->MCID_ENTERTILE = mcid_entertile;
   allocator->MCID_EXITTILE = mcid_exittile;

   allocator->LocalIdMap = localIdMap;

   return allocator;
}

//temporary map in place of a direct, unsigned field (InstructionId) of an instruction
std::map<llvm::MachineInstr*,unsigned>  Allocator::instruction2TileId;

//------------------------------------------------------------------------------
//
// Description:
//
//    Destructor for allocator.  Tear down and and free memory.
//
//------------------------------------------------------------------------------

void
Allocator::Delete()
{
   //TODO: it may be neccessary to insert here explicit delete statements
   //      for all the members until memory pools are implemented.
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize allocator
//
// Notes:
//
//    We initialize the hot/cold models here and determine the
//    tradeoffs based on the user flags.
//
//------------------------------------------------------------------------------

void
Allocator::Initialize()
{
   Graphs::FlowGraph *                          functionUnit = this->FunctionUnit;
   Tiled::VR::Info *                            vrInfo = functionUnit->vrInfo;

   //in the future class TargetRegisterAllocInfo can be split into an abstract class and derived target-specific class.
   TargetRegisterAllocInfo *                    targetRegisterAllocator;

   this->MF = functionUnit->machineFunction;
   this->MF->getRegInfo().freezeReservedRegs(*(this->MF));
   this->TRI = this->MF->getSubtarget().getRegisterInfo();

   // Initialize target register allocator;
   targetRegisterAllocator = new TargetRegisterAllocInfo(vrInfo, this->MF);
   this->TargetRegisterAllocator = targetRegisterAllocator;
   targetRegisterAllocator->InitializeFunction();

   // Initialize cost models
   targetRegisterAllocator->InitializeCosts();

   GraphColor::AllocatorCostModel * costModel = GraphColor::ColdCostModel::New(this);
   this->ColdCostModel = costModel;

   costModel = GraphColor::HotCostModel::New(this);
   this->HotCostModel = costModel;

   this->DoFavorSpeed = true;  //temporarily always true

   Tiled::Cost::StaticInitialize();

   this->VrInfo = vrInfo;

   // Create a vector to map alias tags to id's (live range ids bottom up, reg ids top down)
   // Note that we use double the maximum alias tag since we may be generating
   // new during splitting.  Also note that we only allocate the vector to map 
   // alias tags to global live ranges here.  The vector to map alias tags to 
   // local live  ranges is allocated during EnumerateLiveRanges.  Also, we 
   // don't need this part particular vector under fastpath if there is only 
   // going to be a single tile.
   GraphColor::IdVector * aliasTagToIdVector;

   llvm::MachineRegisterInfo&  MRI = this->MF->getRegInfo();
   unsigned  numberLocationTags = MRI.getNumVirtRegs();
   unsigned maximumAliasTag = vrInfo->HighPhysicalRegister + 1 + numberLocationTags * 2;

   aliasTagToIdVector = new GraphColor::IdVector(maximumAliasTag, 0);
   this->GlobalAliasTagToIdVector = aliasTagToIdVector;

   // Initialize the write through enregistration analysis set.
   //this->WriteThroughAliasTagSet = new llvm::SparseBitVector<>();

   // Initialize global live range data structures, reserve slot 0.
   this->GlobalAliasTagSet = new llvm::SparseBitVector<>();

   this->GlobalLiveRangeVector = new GraphColor::LiveRangeVector();
   this->GlobalLiveRangeVector->reserve(256);
   this->GlobalLiveRangeVector->push_back(nullptr);

   // Initialize SimplificationStack
   this->SimplificationStack = new GraphColor::IdVector();
   this->SimplificationStack->reserve(255);


   // Initialize the scratch bit vectors.

   this->ScratchBitVector1 = new llvm::SparseBitVector<>();
   this->ScratchBitVector2 = new llvm::SparseBitVector<>();
   this->ScratchBitVector3 = new llvm::SparseBitVector<>();
   this->CallerSaveSpillLiveRangeIdSetScratch = new llvm::SparseBitVector<>();
   this->GlobalSpillLiveRangeIdSetScratch = new llvm::SparseBitVector<>();
   this->LivenessScratchBitVector = new llvm::SparseBitVector<>();

   // Initialize spill tracking alias tag bit vectors.
   this->PendingSpillAliasTagBitVector = new llvm::SparseBitVector<>();
   this->DoNotSpillAliasTagBitVector = new llvm::SparseBitVector<>();
   this->callKilledRegBitVector = new llvm::SparseBitVector<>();

   // Initialize undefined register alias tags bit vector
   llvm::SparseBitVector<> * undefinedRegisterAliasTagBitVector = new llvm::SparseBitVector<>();
   this->UndefinedRegisterAliasTagBitVector = undefinedRegisterAliasTagBitVector;

   // Set up the allocatable registers by category table from the target.
   // Set up do not allocate register id and alias tag bit vector from the target. 
   // Set up byte addressable register alias tag vector from the target.

   this->MaximumRegisterId = targetRegisterAllocator->MaximumRegisterId;
   this->MaximumRegisterCategoryId = targetRegisterAllocator->MaximumRegisterCategoryId;

   this->RegisterCategoryIdToAllocatableRegisterCount =
      targetRegisterAllocator->RegisterCategoryIdToAllocatableRegisterCount;

   this->RegisterCategoryIdToAllocatableAliasTagBitVector =
      targetRegisterAllocator->RegisterCategoryIdToAllocatableAliasTagBitVector;

   this->DoNotAllocateRegisterAliasTagBitVector =
      new llvm::SparseBitVector<>(*(targetRegisterAllocator->DoNotAllocateRegisterAliasTagBitVector));

   // Set up from the target what the base integer/float categories are.  These are used in register pressure
   // tracking heuristics.

   const llvm::TargetRegisterClass * baseIntegerRegisterCategory
      = targetRegisterAllocator->GetBaseIntegerRegisterCategory();
   const llvm::TargetRegisterClass * baseFloatRegisterCategory
      = targetRegisterAllocator->GetBaseFloatRegisterCategory();

   this->BaseIntegerRegisterCategory = baseIntegerRegisterCategory;
   this->BaseFloatRegisterCategory = baseFloatRegisterCategory;

   // Create all physical register Alias ref tag - this is used when
   // managing physregs live across tile boundaries so we omit
   // computing it when tiling is disabled.

   if (this->IsTilingEnabled) {
      this->AllRegisterTags = new llvm::SparseBitVector<>();
      for (int i = 0; i < targetRegisterAllocator->RegisterCategoryIdToAllocatableAliasTagBitVector->size(); ++i) {
         llvm::SparseBitVector<> * bv = (*targetRegisterAllocator->RegisterCategoryIdToAllocatableAliasTagBitVector)[i];
         if (bv) {
            *(this->AllRegisterTags) |= *bv;
         }
      }
   } else {
      this->AllRegisterTags = nullptr;
   }

   // create a FrameIndex
   Graphs::FrameIndex  dummySpillSymbol = (this->MF->getFrameInfo()).CreateSpillStackObject(8, 8);
   this->DummySpillSymbol = dummySpillSymbol;

   // Set up entry profile weights for frequency scaling functions (required for ScaleCyclesByFrequency()).

   Profile::Count      entryProfileCount(1);
      // set to a count of 1 instead of the '= entryInstruction->ProfileCount;'

   this->EntryProfileCount = entryProfileCount;

   Profile::Count profileBasisCount =
      Tiled::CostValue::Divide(entryProfileCount, Tiled::CostValue::ConvertFromUnsigned(4));

   // Set basis count to self or 1 if zero
   profileBasisCount = Tiled::CostValue::IsGreaterThanZero(profileBasisCount)
      ? profileBasisCount
      : Tiled::CostValue::ConvertFromUnsigned(1);

   this->ProfileBasisCount = profileBasisCount;

   Tiled::Cost zeroCost = Tiled::Cost::ZeroCost;
   this->ZeroCost = zeroCost;

   // Biased entry frequency.
   this->BiasedEntryCount = Tiled::CostValue::Divide(entryProfileCount, profileBasisCount);

   // Build up global register costs
   GraphColor::CostVector * registerCostVector =
      new GraphColor::CostVector(this->MaximumRegisterId + 1, this->ZeroCost);

   Tiled::Cost infiniteCost = Tiled::Cost::InfiniteCost;
   this->InfiniteCost = infiniteCost;

   // Get basic integer move cost.

   Tiled::Cost integerMoveCost;
   Tiled::Cost tempCost(targetRegisterAllocator->IntegerMoveCost());

   integerMoveCost.Copy(&tempCost);
   this->IntegerMoveCost = integerMoveCost;

   Tiled::Cost unitCost;

   unitCost.Initialize(0.0001, 1);
   this->UnitCost = unitCost;

   this->RegisterCostVector = registerCostVector;

   // Set basic preference search depths
   this->PreferenceSearchDepth = unsigned(Allocator::Constants::PreferenceSearchDepth);
   this->AntiPreferenceSearchDepth = unsigned(Allocator::Constants::AntiPreferenceSearchDepth);

   // Build up preference and anti-preference vectors.

   unsigned                                             registerPreferenceVectorSize = 32;
   GraphColor::PhysicalRegisterPreferenceVector *       registerPreferenceVector;
   GraphColor::PhysicalRegisterPreferenceVector *       registerAntiPreferenceVector;
   GraphColor::PhysicalRegisterPreferenceVectorVector * temporaryRegisterPreferenceVectors;

   registerPreferenceVector = new GraphColor::PhysicalRegisterPreferenceVector();
   registerPreferenceVector->reserve(registerPreferenceVectorSize);
   this->RegisterPreferenceVector = registerPreferenceVector;

   registerAntiPreferenceVector = new GraphColor::PhysicalRegisterPreferenceVector();
   registerAntiPreferenceVector->reserve(registerPreferenceVectorSize);
   this->RegisterAntiPreferenceVector = registerAntiPreferenceVector;

   unsigned numberOfTemporaryRegisterPreferenceVectors = (this->PreferenceSearchDepth + 1);

   temporaryRegisterPreferenceVectors =
      new GraphColor::PhysicalRegisterPreferenceVectorVector();
   temporaryRegisterPreferenceVectors->reserve(numberOfTemporaryRegisterPreferenceVectors);
   this->TemporaryRegisterPreferenceVectors = temporaryRegisterPreferenceVectors;

   while (numberOfTemporaryRegisterPreferenceVectors--)
   {
      registerPreferenceVector = new GraphColor::PhysicalRegisterPreferenceVector();
      registerPreferenceVector->reserve(registerPreferenceVectorSize);
      temporaryRegisterPreferenceVectors->push_back(registerPreferenceVector);
   }

   // Initialize total spill cost for allocation.
   this->TotalSpillCost = this->ZeroCost;

   // Initialize total transfer cost for allocation.
   this->TotalTransferCost = this->ZeroCost;

   // Set allocator ready for pass one.
   this->Pass = GraphColor::Pass::Ready;

   // Initialize tracking info for block walks
   GraphColor::SpillOptimizer * spillOptimizer = GraphColor::SpillOptimizer::New(this);

   this->ConservativeLoopThreshold = 18;
   this->ConservativeFatThreshold = 15;
   this->ConservativeControlThreshold = 14;

   this->CalleeSaveConservativeThreshold = targetRegisterAllocator->CalleeSaveConservativeThreshold();

   this->CallCountConservativeThreshold = 3;
   this->GlobalLiveRangeConservativeThreshold = 24;
   this->NodeCountThreshold = 130000;

   spillOptimizer->EdgeProbabilityInstructionShareStopIteration
      = targetRegisterAllocator->EdgeProbabilityInstructionShareStopIteration();
   spillOptimizer->CrossCallInstructionShareStopIteration
      = targetRegisterAllocator->CrossCallInstructionShareStopIteration();
   spillOptimizer->InstructionShareDecayRateValue
      = targetRegisterAllocator->InstructionShareDecayRateValue();
   spillOptimizer->DoShareAcrossCalls = true;
   spillOptimizer->DoCallerSaveStrategy = false;   //1T+:  = true;

   spillOptimizer->InitialInstructionShareLimit
      = targetRegisterAllocator->InitialInstructionShareLimit();
   spillOptimizer->ConstrainedInstructionShareStartIteration
      = targetRegisterAllocator->ConstrainedInstructionShareStartIteration();
   spillOptimizer->CallerSaveIterationLimit
      = targetRegisterAllocator->CallerSaveIterationLimit();

   // Initialize upper bound on tile iterations.
   this->IterationLimit = 100;

   this->SpillOptimizer = spillOptimizer;

   this->SimplificationHeuristic = GraphColor::Heuristic::Optimistic;

   GraphColor::AvailableExpressions * globalAvailableExpressions = GraphColor::AvailableExpressions::New(this);
   this->GlobalAvailableExpressions = globalAvailableExpressions;

   this->IsShrinkWrappingEnabled = false;

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize global register costs via the target call back.
//
// Arguments:
//
//    registerCostVector - Allocated cost vector to be initialized by the target.  
//
// Remarks:
//
//    Called twice, once for pass 1 and once for pass 2.  This second call ensures that pass 2 sees the same
//    constraints (since it's starting fresh)
//
//------------------------------------------------------------------------------

void
Allocator::InitializeRegisterCosts
(
   GraphColor::CostVector * registerCostVector
)
{
   Tiled::Id        regId;
   Tiled::Cost      zeroCost = this->ZeroCost;
   Tiled::Cost      unitCost;
   Tiled::Cost      registerCost;
   Profile::Count   frequency;

   for (regId = 0; regId < this->MaximumRegisterId; regId++)
   {
      (*registerCostVector)[regId] = zeroCost;
   }

   this->TargetRegisterAllocator->InitializeRegisterVectorCosts(registerCostVector);

   // For allocate pass, just use a unit cost to help avoid using callee save register
   // when caller save register are available.

   if (this->Pass == GraphColor::Pass::Allocate) {
      unitCost = this->UnitCost;

      for (regId = 0; regId < this->MaximumRegisterId; regId++)
      {
         registerCost = (*registerCostVector)[regId];

         if (this->ColdCostModel->Compare(&registerCost, &zeroCost) != 0) {
            (*registerCostVector)[regId] = unitCost;
         }
      }
   }

   // For assign pass, use actual target cost of using callee save register scaled by 
   // entry profile frequency.

   else if (this->Pass == GraphColor::Pass::Assign) {
      for (regId = 0; regId < this->MaximumRegisterId; regId++)
      {
         registerCost = (*registerCostVector)[regId];

         if (this->ColdCostModel->Compare(&registerCost, &zeroCost) != 0) {
            frequency = this->GetBiasedFrequency(this->EntryProfileCount);
            registerCost.ScaleCyclesByFrequency(frequency);
            (*registerCostVector)[regId] = registerCost;
         }
      }
   }

   // Must be a valid pass.

   else {
      llvm_unreachable("Invalid pass");
   }
}



//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if it is useful to annotate the function with callee save live ranges for shrinkwrapping.
//    This controls the TP hit we take for allocating the injected live ranges.
//
//------------------------------------------------------------------------------

bool
Allocator::DoAnnotateFunction()
{
#ifdef FUTURE_IMPL
   Tiled::FunctionUnit ^        functionUnit = this->FunctionUnit;
   GraphColor::TileGraph ^    tileGraph = this->TileGraph;
   Tiled::Boolean               disableShrinkWrap = false;

   if (!this->IsNewFramingEnabled || !this->IsShrinkWrappingEnabled)
   {
      return false;
   }

   Assert(tileGraph->TileCount > 0);

   if ((tileGraph->TileCount == 1))
   {
      disableShrinkWrap = true;
   }

   // Turn off shrink wrap if optimization goal is either flavor of size.

   else if ((functionUnit->OptimizationGoal == Optimization::Goal::FavorSize)
      || (functionUnit->OptimizationGoal == Optimization::Goal::ExtremeSize))
   {
      disableShrinkWrap = true;
   }

   // Turn off shrinkwrap if the optimization goal is to favor speed and we have not be able to form an early
   // out tile.

   else if ((functionUnit->OptimizationGoal == Optimization::Goal::FavorSpeed)
      && (tileGraph->ShrinkWrapTile == nullptr))
   {
      disableShrinkWrap = true;
   }

   if (!disableShrinkWrap)
   {
      // Check the tiles in the function for dangling exits and opt
      // out of shrink wrapping if there are any.

      foreach_tile_in_tilegraph(tile, tileGraph)
      {
         if (tile->HasDanglingExit)
         {
            disableShrinkWrap = true;
            break;
         }
      }
      next_tile_in_tilegraph;
   }

   if (disableShrinkWrap)
   {
      // Limited or no win available so turn off shrink wrapping/newframing. 

      // Turn this decision into one that triggers save/restore insertion after allocation
      // based on the registers killed.  This will make NewFraming simpler. (include EH in this)

      this->IsShrinkWrappingEnabled = false;

      return false;
   }
   else
   {
      return true;
   }
#endif // FUTURE_IMPL
   llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    AnnotateIRForCalleeSave - Add copies to and from allocator temporaries for live through callee save
//    values. This allows us to shrink wrap them to tile boundaries.
//
//    ... START
//     tv_calleeSave(reg)    = mov physicalRegister
//     tv_calleeSave2(reg+1) = mov physicalRegister+1
//        ENTERBODY
//
//        EXITBODY
//    physicalRegister   = mov tv_calleeSave(reg) 
//    physicalRegister+1 = mov tv_calleeSave(reg+1)
//    ... END
//
//------------------------------------------------------------------------------

void
Allocator::AnnotateIRForCalleeSave()
{
#ifdef FUTURE_IMPL   //1T +
   Tiled::FunctionUnit ^      functionUnit = this->FunctionUnit;
   IR::Instruction ^          entryInstruction = functionUnit->FirstEnterInstruction;
   IR::Instruction ^          exitInstruction = functionUnit->LastExitInstruction;
   IR::Instruction ^          entryInsertInstruction =
      entryInstruction->FindNextInstruction(Common::Opcode::EnterBody);
   IR::Instruction ^          exitInsertInstruction =
      exitInstruction->FindPreviousInstruction(Common::Opcode::ExitBody);
   Targets::Runtimes::Frame ^ frame = functionUnit->Frame;
   BitVector::Sparse ^        calleeSaveRegisterAliasTagSet =
      this->CalleeSaveRegisterAliasTagSet;
   BitVector::Sparse ^        allocatableRegisterAliasTagBitVector;
   Tiled::Boolean             doAnnotateFunction = this->DoAnnotateFunction();
   Targets::Architectures::RegisterAllocator ^ targetRegisterAllocator = this->TargetRegisterAllocator;

   if (!doAnnotateFunction)
   {
      return;
   }

   // Set up the insert points to drop instructions just before the GOTO to the enter body after the exit body.

   entryInsertInstruction = entryInsertInstruction->Previous;
   exitInsertInstruction = exitInsertInstruction->Next;

   Assert(entryInsertInstruction->IsBranchInstruction
      && entryInsertInstruction->AsBranchInstruction->IsUnconditional);
   Assert(exitInsertInstruction->Opcode != Common::Opcode::EnterExitFunction);

   if (calleeSaveRegisterAliasTagSet == nullptr)
   {
      calleeSaveRegisterAliasTagSet = TILED_NEW_SPARSE_BITVECTOR(lifetime);

      this->CalleeSaveRegisterAliasTagSet = calleeSaveRegisterAliasTagSet;
   }
   else
   {
      calleeSaveRegisterAliasTagSet->ClearAll();
   }

   BitVector::Sparse ^ mentionedBaseRegisterCategoryBitVector = this->MentionedBaseRegisterCategoryBitVector;

   // Foreach category being allocated, add copies for callee save values.

   foreach_register_class(registerCategory)
   {
      // Don't consider categories that are not compiler allocated nor child categories.

      if (registerCategory->IsDoNotAllocate
         || !targetRegisterAllocator->IsCalleeSaveCategory(functionUnit, registerCategory))
      {
         continue;
      }

      // Omit callee save transfers for floats if there are no floats in the function.

      if (!mentionedBaseRegisterCategoryBitVector->GetBit(registerCategory->BaseRegisterCategory->Id))
      {
         continue;
      }

      allocatableRegisterAliasTagBitVector = this->GetAllocatableRegisters(registerCategory);

      // Walk this base register category and for each register that is callee save add a transfer to and from
      // an allocator temporary.

      Alias::Tag registerAliasTag;

      foreach_sparse_bv_bit(registerAliasTag, allocatableRegisterAliasTagBitVector)
      {
         Registers::Register ^ reg = aliasInfo->GetRegister(registerAliasTag);

         if (frame->RegisterIsCallee(reg))
         {
            Alias::Tag            registerTag = aliasInfo->GetTag(reg);
            Registers::Register ^ categoryPseudoRegister = registerCategory->PseudoRegister;
            Types::Type ^         registerType = reg->GetType(functionUnit);
            IR::Operand ^         candidateOperand
               = IR::VariableOperand::NewTemporaryRegister(functionUnit, registerType, categoryPseudoRegister);
            IR::Operand ^         physicalRegisterOperand
               = IR::VariableOperand::NewRegister(functionUnit, registerType, reg);

            entryInsertInstruction->SetCurrentDebugTag();

            IR::Instruction ^     definitionInstruction
               = IR::ValueInstruction::NewUnary(functionUnit,
               Common::Opcode::Assign, candidateOperand, physicalRegisterOperand);

            exitInsertInstruction->SetCurrentDebugTag();

            IR::Instruction ^     useInstruction
               = IR::ValueInstruction::NewUnary(functionUnit,
               Common::Opcode::Assign, physicalRegisterOperand, candidateOperand);

            // Keep track of the registers we generate transfers for.

            calleeSaveRegisterAliasTagSet->SetBit(registerTag);

            // Insert new copies into the function.

            entryInsertInstruction->InsertBefore(definitionInstruction);
            exitInsertInstruction->InsertBefore(useInstruction);

            IR::Instruction ^ beforeEntryInstruction = definitionInstruction->Previous;
            IR::Instruction ^ afterEntryInstruction = definitionInstruction->Next;

            functionUnit->Lower->Range(beforeEntryInstruction->Next, afterEntryInstruction->Previous);

            // Ensure that lowering didn't introduce unexpected instructions.

            Assert(beforeEntryInstruction->Next->Next == afterEntryInstruction);

            GraphColor::SpillOptimizer::SetCalleeSaveTransfer(definitionInstruction);

            IR::Instruction ^ beforeExitInstruction = useInstruction->Previous;
            IR::Instruction ^ afterExitInstruction = useInstruction->Next;

            functionUnit->Lower->Range(beforeExitInstruction->Next, afterExitInstruction->Previous);

            // Ensure that lowering didn't introduce unexpected instructions.

            Assert(beforeExitInstruction->Next->Next == afterExitInstruction);

            GraphColor::SpillOptimizer::SetCalleeSaveTransfer(useInstruction);
         }
      }
      next_sparse_bv_bit;
   }
   next_register_class;

   // Set up start and end birth and death of callee save registers if we're tracking them for shrinkwrapping.

   if (!calleeSaveRegisterAliasTagSet->IsEmpty)
   {
      Alias::Tag    registerReferenceTag
         = aliasInfo->GetTagForTagSet(calleeSaveRegisterAliasTagSet, nullptr, Alias::Type::Must);
      IR::Operand ^ registerAliasOperand = IR::AliasOperand::New(functionUnit, registerReferenceTag);

      IR::Instruction ^ startInstruction = functionUnit->FirstInstruction;
      IR::Instruction ^ endInstruction = functionUnit->LastInstruction;

      endInstruction = endInstruction->FindPreviousInstruction(Common::Opcode::ExitExitFunction);

      startInstruction->AppendDestination(registerAliasOperand);
      endInstruction->AppendSource(registerAliasOperand);
   }
#endif // FUTURE_IMPL
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Determine the conservative threshold to use based on the type of tile passed.
//
//------------------------------------------------------------------------------

unsigned
Allocator::ConservativeThreshold
(
   GraphColor::Tile * tile
)
{
   if (tile->IsLoop()) {
      return this->ConservativeLoopThreshold;
   } else if (tile->IsFat()) {
      return this->ConservativeFatThreshold;
   } else {
      return this->ConservativeControlThreshold;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize tile/allocator maps with summary info for insertion
//    of assignment and compensation code
//
//------------------------------------------------------------------------------

void
Allocator::Rebuild
(
   GraphColor::Tile * tile
)
{
   // Ensure that the map is zero'd and that the size is big enough.

   this->ResetAliasTagToIdMap();

   tile->AliasTagToIdVector = this->AliasTagToIdVector;

   tile->Rebuild();

   GraphColor::SpillOptimizer * spillOptimizer = this->SpillOptimizer;

   spillOptimizer->Initialize(tile);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Push down allocation decisions from parent tile to child tile.
//
//------------------------------------------------------------------------------

void
Allocator::UpdateSummary
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *               allocator = tile->Allocator;
   Tiled::VR::Info *                     aliasInfo = this->VrInfo;
   GraphColor::Tile *                    parentTile = tile->ParentTile;
   TargetRegisterAllocInfo *             targetRegisterAllocator = this->TargetRegisterAllocator;
   GraphColor::SummaryPreferenceGraph *  summaryPreferenceGraph = tile->SummaryPreferenceGraph;

   GraphColor::LiveRangeVector * slrVector = tile->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[slr];
      assert(summaryLiveRange->IsSummary());

      if (summaryLiveRange->IsPhysicalRegister) {
         continue;
      }

      unsigned aliasTag;

      if (summaryLiveRange->IsSecondChanceGlobal) {
         // Use single global tag in summary to do parent look up. Second chance globals do not add proxy
         // temporaries to their parent tiles, they were spilled pass 1 as not colorable and are only added
         // back pass 2 if we can find space due to spilling up the tree pass 1.

         llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
         assert(!summaryAliasTagSet->empty());

         GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(summaryLiveRange);
         assert(globalLiveRange != nullptr);

         aliasTag = globalLiveRange->VrTag;
      } else {
         // Pass 1 allocated summary, since there could be multiple global tags in this summary use alias tag
         // for proxy temporary in the parent to find the register.

         aliasTag = summaryLiveRange->VrTag;
      }

      unsigned reg = parentTile->GetSummaryRegister(aliasTag);

      if (reg != VR::Constants::InvalidReg) {
         unsigned  summaryReg = summaryLiveRange->Register;
         assert(!summaryLiveRange->HasHardPreference || (summaryReg == reg));

         // <place for code supporting architectures with sub-registers>

         assert(VR::Info::IsPhysicalRegister(reg));
         summaryLiveRange->Register = reg;

         if (summaryPreferenceGraph != nullptr) {
            // Determine if this summary live range covers a global live range.  If not,
            // don't push down register selection.   There is no cost for changing our minds
            // going back down.

            GraphColor::LiveRange * globalLiveRange = summaryLiveRange->GetGlobalLiveRange();
            if (globalLiveRange == nullptr) {
               continue;
            }

            unsigned                globalAliasTag = globalLiveRange->VrTag;
            unsigned                registerAliasTag = aliasInfo->GetTag(reg);
            GraphColor::LiveRange * registerSummaryLiveRange = tile->GetRegisterSummaryLiveRange(registerAliasTag);
            assert(registerSummaryLiveRange != nullptr);

            // Initialize preference cost.  This is cost of failing to get the register that
            // the parent has specified.

            Tiled::Cost preferenceCost = this->ZeroCost;
            Tiled::Cost cost;

            // Process tile boundary instructions looking for where this global live range is 
            // live.  Calculate and add physical register preference cost at that point.

            GraphColor::Liveness *    liveness = allocator->Liveness;
            Dataflow::LivenessData *  registerLivenessData;
            llvm::SparseBitVector<> * liveBitVector;

            Tiled::Cost intMoveCost = targetRegisterAllocator->IntegerMoveCost();
            Graphs::MachineBasicBlockList::iterator bi;

            // foreach_tile_entry_block
            for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
            {
               llvm::MachineBasicBlock * entryBlock = *bi;

               registerLivenessData = liveness->GetRegisterLivenessData(entryBlock);
               liveBitVector = registerLivenessData->LiveOutBitVector;

               if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveBitVector)) {
                  llvm::MachineInstr * instruction =
                     tile->FindNextInstruction(entryBlock, entryBlock->instr_begin(), llvm::TargetOpcode::ENTERTILE);
                  assert(instruction != nullptr);

                  cost.Copy(&intMoveCost);
                  allocator->ScaleCyclesByFrequency(&cost, instruction);
                  preferenceCost.IncrementBy(&cost);
               }
            }

            // foreach_tile_exit_block
            for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
            {
               llvm::MachineBasicBlock * exitBlock = *bi;

               registerLivenessData = liveness->GetRegisterLivenessData(exitBlock);
               liveBitVector = registerLivenessData->LiveInBitVector;

               if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveBitVector)) {
                  llvm::MachineInstr * instruction =
                     tile->FindNextInstruction(exitBlock, exitBlock->instr_begin(), llvm::TargetOpcode::EXITTILE);
                  assert(instruction != nullptr);

                  cost.Copy(&intMoveCost);
                  allocator->ScaleCyclesByFrequency(&cost, instruction);
                  preferenceCost.IncrementBy(&cost);
               }
            }

            // Get cost of failing to get this register in the nested tile.

            RegisterAllocator::PreferenceConstraint preferenceConstraint = RegisterAllocator::PreferenceConstraint::SameRegister;

            summaryPreferenceGraph->InstallParentPreference(summaryLiveRange, registerSummaryLiveRange,
               &preferenceCost, &preferenceConstraint);
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Terminate the allocator.
//
//------------------------------------------------------------------------------

void
Allocator::Terminate()
{
   Graphs::FlowGraph * functionUnit = this->FunctionUnit;

   // Since llvm::MachineInstr does not have a direct (InstructionId) field
   // this is equivalent to removing any markings stashed in the field.
   instruction2TileId.clear();
}

GraphColor::LiveRange *
Allocator::GetGlobalLiveRange
(
   llvm::MachineOperand * operand
)
{
   if (!operand->isReg()) {
      return nullptr;
   }

   return this->GetGlobalLiveRange(this->VrInfo->GetTag(operand->getReg()));
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Get global LiveRange enumerator
//
// Remarks:
//
//     Currently we just use the vector as the basis for enumeration.
//
//------------------------------------------------------------------------------

GraphColor::LiveRangeEnumerator *
Allocator::GetGlobalLiveRangeEnumerator
(
)
{
   GraphColor::LiveRangeVector * liveRangeVector = this->GlobalLiveRangeVector;

   return liveRangeVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get live range id given an alias tag.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetLiveRangeId
(
   unsigned aliasTag
)
{
   GraphColor::IdVector *  aliasTagToIdVector = this->AliasTagToIdVector;
   unsigned                liveRangeId = 0;

   if (aliasTag < aliasTagToIdVector->size()) {
      liveRangeId = (*aliasTagToIdVector)[aliasTag];
   }

   return liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get global live range id given an alias tag.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetGlobalLiveRangeId
(
   unsigned aliasTag
)
{
   GraphColor::IdVector *  aliasTagToIdVector = this->GlobalAliasTagToIdVector;
   unsigned                liveRangeId = 0;

   if (aliasTag < aliasTagToIdVector->size()) {
      liveRangeId = (*aliasTagToIdVector)[aliasTag];
   }

   return liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get global live range by alias tag.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Allocator::GetGlobalLiveRange
(
   unsigned aliasTag
)
{
   // Don't be calling this when there isn't an active tile with a current 
   // aliasTag to liveRange mapping.

   assert(this->GlobalLiveRangeVector != nullptr);

   if (aliasTag == VR::Constants::InvalidTag) {
      return nullptr;
   }

   unsigned                       liveRangeId = this->GetGlobalLiveRangeId(aliasTag);
   GraphColor::LiveRangeVector *  liveRangeVector = this->GlobalLiveRangeVector;
   GraphColor::LiveRange *        liveRange = (*liveRangeVector)[liveRangeId];

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get global live range by live range id.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Allocator::GetGlobalLiveRangeById
(
   unsigned globalLiveRangeId
)
{
   // Don't be calling this when there isn't an active tile with a current 
   // aliasTag to liveRange mapping.
   assert(this->GlobalLiveRangeVector != nullptr);

   GraphColor::LiveRangeVector * liveRangeVector = this->GlobalLiveRangeVector;

   if (globalLiveRangeId >= liveRangeVector->size()) {
      return nullptr;
   }

   GraphColor::LiveRange * liveRange = (*liveRangeVector)[globalLiveRangeId];

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get live range id for an alias tag or overlapped alias tag.
//
// Notes:
//
//    Used during build up of live ranges before we know widest
//    appearance tag.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetLiveRangeIdWithOverlap
(
   unsigned aliasTag
)
{
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->AliasTagToIdVector;
   unsigned                liveRangeId;

   liveRangeId = this->GetLiveRangeId(aliasTag);

   if (liveRangeId != 0) {
      return liveRangeId;
   }

   // See if this alias tag overlaps alias tags
   // that were already mapped to a live range id.

   // <place for code supporting architectures with sub-registers>

   return liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get live range id for an overlapped alias tag that requires merging.
//
// Arguments:
//
//    aliasTag - Overlapping alias tag.
//    liveRangeId - Current live range for which a merge sibling is being looked for.
//
// Notes:
//
//    Allows for the discovery of live ranges needing merging when a combined use is found subsequent to
//    multiple partial defs.  (As in a piece wise vector definition on T2 NEON) This function may be called
//    iteratively to find the n pairs that need to be merged to cover the use in question.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetLiveRangeIdWithMerge
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   unsigned                mergeLiveRangeId = 0;
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->AliasTagToIdVector;
   unsigned                nextLiveRangeId;

   assert(this->GetLiveRangeIdWithOverlap(aliasTag) == liveRangeId);

   // <place for code supporting architectures with sub-registers>
   // Without sub-registers there are NO mergeable live ranges, return 0.

   return mergeLiveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get global live range id for an overlapped alias tag that reqires merging.
//
// Arguments:
//
//    aliasTag - Overlapping alias tag.
//    liveRangeId - Current live range for which a merge sibling is being looked for.
//
// Notes:
//
//    Allows for the discovery of live ranges needing merging when a combined use is found subsequent to
//    multiple partial defs.  (As in a piece wise vector definition on T2 NEON) This function may be called
//    iteratively to find the n pairs that need to be merged to cover the use in question.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetGlobalLiveRangeIdWithMerge
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->GlobalAliasTagToIdVector;
   unsigned                nextLiveRangeId;
   unsigned                mergeLiveRangeId = this->GetGlobalLiveRangeIdWithOverlap(aliasTag);

   assert(mergeLiveRangeId == liveRangeId);

   // <place for code supporting architectures with sub-registers>
   // Without sub-registers there are NO mergeable live ranges, return 0.

   return mergeLiveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get global live range id for an alias tag or overlapped alias tag.
//
// Notes:
//
//    Used during build up of live ranges before we know widest
//    appearance tag.
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetGlobalLiveRangeIdWithOverlap
(
   unsigned aliasTag
)
{
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->GlobalAliasTagToIdVector;
   unsigned                liveRangeId;

   liveRangeId = this->GetGlobalLiveRangeId(aliasTag);
   if (liveRangeId != 0) {
      return liveRangeId;
   }

   // See if this alias tag overlaps alias tags
   // that were already mapped to a live range id.

   // <place for code supporting architectures with sub-registers>
   // Without sub-registers there are NO mergeable live ranges, return 0.

   return liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the global live range summarized by this summary variable.
//
// Arguments:
//
//    summaryLiveRange - summary variable potentially containing a global live range.
//
// Remarks:
//
//    When more than one global live range is available, chose one with symbolic information.
//
// Returns:
//
//    Global live range if found, otherwise returns nullptr.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Allocator::GetGlobalLiveRange
(
   GraphColor::LiveRange * summaryLiveRange
)
{
   llvm::SparseBitVector<> *  summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
   GraphColor::LiveRange *    globalLiveRange = nullptr;
   GraphColor::LiveRange *    liveRange;

   llvm::SparseBitVector<>::iterator a;

   // foreach_sparse_bv_bit
   for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
   {
      unsigned aliasTag = *a;

      liveRange = this->GetGlobalLiveRange(aliasTag);

      if (liveRange != nullptr) {
         // Find first global live range.

         if (globalLiveRange == nullptr) {
            globalLiveRange = liveRange;
            //For an architecture without sub-register no need to look for the largest lr
            break;
         }

         // Find largest global live range.

         // <place for code supporting architectures with sub-registers>

      }
   }

   return globalLiveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Map alias tag (and all must overlap tags) to live range id.
//
// Arguments:
//
//    aliasTag    - alias tag to be mapped
//    liveRangeId - live range id to map to
//
//------------------------------------------------------------------------------

void
Allocator::MapAliasTagToLiveRangeIdOverlapped
(
   unsigned   aliasTag,
   unsigned   liveRangeId,
   bool       allowRemap
)
{
   Tiled::VR::Info *       aliasInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->AliasTagToIdVector;

   // Map alias tags to the given live range id. 
   // Allow for remapping the tag to the same live range id.
   // This remapping happens during widening.

   // For architectures with no sub-registers no widening and no (total) aliases:
   // <place for code supporting architectures with sub-registers>

   assert(aliasTag < aliasTagToIdVector->size());

   if (((*aliasTagToIdVector)[aliasTag] == 0) || allowRemap) {
      this->MapAliasTagToLiveRangeId(aliasTag, liveRangeId);
   } else {
      // Either this is a physical register (high byte, high FP pair, ...) or
      // this overlap tag is already mapped in.
      assert(aliasInfo->IsPhysicalRegisterTag(aliasTag) || ((*aliasTagToIdVector)[aliasTag] == liveRangeId));
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Map alias tag (and all must overlap tags) to live range id (this
//    doesn't just widen but overwrites merged live ranges)
//
// Arguments:
//
//    aliasTag          - alias tag to be mapped
//    liveRangeId       - live range id to map to
//    mergedLiveRangeId - live range to overwrite.
//
//------------------------------------------------------------------------------

void
Allocator::MapAliasTagToLiveRangeIdMerged
(
   unsigned aliasTag,
   unsigned liveRangeId,
   unsigned mergedLiveRangeId
)
{
   // Merge path overwrites a previous with a new live range.

   // <place for code supporting architectures with sub-registers>
   // Without sub-registers there are NO mergeable live ranges.
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Map alias tag (and all must overlap tags) to live range id (this
//    doesn't just widen but overwrites merged live ranges) in the
//    global map.
//
// Arguments:
//
//    aliasTag          - alias tag to be mapped
//    liveRangeId       - live range id to map to
//    mergedLiveRangeId - live range to overwrite.
//
//------------------------------------------------------------------------------

void
Allocator::MapGlobalAliasTagToLiveRangeIdMerged
(
   unsigned aliasTag,
   unsigned liveRangeId,
   unsigned mergedLiveRangeId
)
{
   // Merge path overwrites a previous with a new live range.

   // <place for code supporting architectures with sub-registers>
   // Without sub-registers there are NO mergeable live ranges.
}

void
Allocator::MapAliasTagToRegisterLiveRangeIdOverlapped
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   // Register path is total write of overlapped.

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_must_total_alias_of_tag(totallyOverlappedAliasTag, aliasTag, aliasInfo)
   //{
      this->MapAliasTagToLiveRangeId(aliasTag, liveRangeId);
   //}
   //next_must_total_alias_of_tag;
}

void
Allocator::MapAliasTagToLiveRangeId
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   GraphColor::IdVector * aliasTagToIdVector = this->AliasTagToIdVector;

   assert(aliasTag < aliasTagToIdVector->size());

   (*aliasTagToIdVector)[aliasTag] = liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Map global alias tag (and all must overlap tags) to live range id.
//
// Arguments:
//
//    aliasTag    - alias tag to be mapped
//    liveRangeId - live range id to map to
//    allowRemap  - allow merge path "remapping" of a prior live range ID
//
// Remarks:
//
//    Remapping allow for the combination of live ranges when an
//    overlapping use is encountered.  In this case we relax the
//    requirement that the alias map slot is in its initial state.
//
//------------------------------------------------------------------------------

void
Allocator::MapGlobalAliasTagToLiveRangeIdOverlapped
(
   unsigned aliasTag,
   unsigned liveRangeId,
   bool     allowRemap
)
{
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   GraphColor::IdVector *  aliasTagToIdVector = this->GlobalAliasTagToIdVector;

   // Map alias tags to the given live range id. 
   // Allow for remapping the tag to the same live range id.
   // This remapping happens during widening.

   assert(aliasTag < aliasTagToIdVector->size());

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_must_total_alias_of_tag(totallyOverlappedAliasTag, aliasTag, aliasInfo)
   //{
      if (((*aliasTagToIdVector)[aliasTag] == 0) || allowRemap) {
         assert(aliasTag < aliasTagToIdVector->size());

         this->MapGlobalAliasTagToLiveRangeId(aliasTag, liveRangeId);
      } else {
         // Either this is a physical register (high byte, high FP pair, ...) or
         // this overlap tag is already mapped in.

         assert(vrInfo->IsPhysicalRegisterTag(aliasTag) || ((*aliasTagToIdVector)[aliasTag] == liveRangeId));
      }
   //}
   //next_must_total_alias_of_tag;
}

void
Allocator::MapGlobalAliasTagToRegisterLiveRangeIdOverlapped
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   // Register path is total write of overlapped.

   //For architectures without sub-registers the commented below loop collapses to single iteration
   //foreach_must_total_alias_of_tag(totallyOverlappedAliasTag, aliasTag, aliasInfo)
   //{
      this->MapGlobalAliasTagToLiveRangeId(aliasTag, liveRangeId);
   //}
   //next_must_total_alias_of_tag;
}

void
Allocator::MapGlobalAliasTagToLiveRangeId
(
   unsigned aliasTag,
   unsigned liveRangeId
)
{
   GraphColor::IdVector * aliasTagToIdVector = this->GlobalAliasTagToIdVector;

   assert(aliasTag < aliasTagToIdVector->size());
   (*aliasTagToIdVector)[aliasTag] = liveRangeId;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add global live range
//
// Argurments:
//
//    aliasTag    - alias tag to be mapped
//    reg         - reg (or pseudo) associated with live range
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Allocator::AddGlobalLiveRange
(
   unsigned  aliasTag,
   unsigned  reg
)
{
   Tiled::VR::Info *              vrInfo = this->VrInfo;

   GraphColor::LiveRangeVector *  liveRangeVector = this->GlobalLiveRangeVector;
   unsigned                       liveRangeId;
   GraphColor::LiveRange *        liveRange;

   // Check to see if live range already exists
   liveRange = this->GetGlobalLiveRange(aliasTag);

   // If it doesn't see we have previously mapped alias tag and update
   // the live range.   Or, generate a new live range.

   if (liveRange == nullptr) {
      // Don't create live ranges for things we can't allocate.

      if (reg == this->TRI->getFrameRegister(*(this->MF))) {
         return nullptr;
      }

      const llvm::TargetRegisterClass *  registerCategory = this->GetRegisterCategory(reg);
      if (!this->GetAllocatableRegisters(registerCategory)) {
         return nullptr;
      }

      // Determine if we have mapped this alias tag or any tags it overlaps already.  
      // If so, we just found a larger appearance so use existing live range and 
      // widen the alias tag on the live range.

      liveRangeId = this->GetGlobalLiveRangeIdWithOverlap(aliasTag);

      if (liveRangeId != 0) {

         unsigned mergedLiveRangeId = this->GetGlobalLiveRangeIdWithMerge(aliasTag, liveRangeId);

         liveRange = (*liveRangeVector)[liveRangeId];
         assert(liveRange != nullptr);

         assert(vrInfo->MustTotallyOverlap(aliasTag, liveRange->VrTag));
         liveRange->VrTag = aliasTag;

         if (mergedLiveRangeId != 0) {
            // Handle n-way merge.  In practice this is very rare (2-
            // way is rare - more than 2 is super rare).

            do
            {
               this->MapGlobalAliasTagToLiveRangeIdMerged(aliasTag, liveRangeId, mergedLiveRangeId);

               GraphColor::LiveRange * mergedLiveRange = (*liveRangeVector)[mergedLiveRangeId];
               assert(mergedLiveRange != nullptr);

               //was: liveRangeVector->Remove(mergedLiveRange);
               GraphColor::LiveRangeVector::iterator oldEnd = liveRangeVector->end();
               GraphColor::LiveRangeVector::iterator newEnd = std::remove(liveRangeVector->begin(), oldEnd, mergedLiveRange);
               liveRangeVector->erase(newEnd, oldEnd);

               // fix up liverange ids. (so the rest of the code doesn't
               // have to skip nullptrs everywhere).

               for (Tiled::Id index = mergedLiveRangeId; index < liveRangeVector->size(); index++)
               {
                  GraphColor::LiveRange * adjustedLiveRange = (*liveRangeVector)[index];
                  assert(adjustedLiveRange->Id == (index + 1));

                  adjustedLiveRange->Id = index;

                  // Update alias tag map to reflect the new ID.
                  this->MapGlobalAliasTagToLiveRangeIdOverlappedRemap(adjustedLiveRange->VrTag, index);
               }

               // Update liveRangeId to account for the case where the above loop adjusted the id as part of
               // the merge.

               liveRangeId = this->GetGlobalLiveRangeIdWithOverlap(aliasTag);

               mergedLiveRangeId = this->GetGlobalLiveRangeIdWithMerge(aliasTag, liveRangeId);
            } while (mergedLiveRangeId != 0);

         } else {
            this->MapGlobalAliasTagToLiveRangeIdOverlappedExclusive(aliasTag, liveRangeId);
         }

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;

      // Construct a new live range for this alias tag and map it in.
      } else {
         liveRangeId = liveRangeVector->size();
         liveRange = GraphColor::LiveRange::NewGlobal(this, liveRangeId, aliasTag);
         liveRangeVector->push_back(liveRange);

         this->MapGlobalAliasTagToLiveRangeIdOverlappedExclusive(aliasTag, liveRangeId);

         if (!(reg == VR::Constants::InitialPseudoReg || vrInfo->IsVirtualRegister(reg))) {
            //was: !IsRegisterCategoryPseudo
            liveRange->IsPreColored = true;
            if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
               liveRange->IsPhysicalRegister = true;
            }
            liveRange->Register = reg;
         } else {
            liveRange->Register = VR::Constants::InitialPseudoReg;
         }

         // Do we know the difference?  When are these not the same?
         assert(liveRange->IsPhysicalRegister == liveRange->IsPreColored);

      }
   }

   // comments below about missing section relevant only to architectures with sub-registers:

   // Track whether we have any byte references to this live range.  This
   // will further constrain register selection.

   // It is possible to see a larger pseudo register request because of
   // machine constraints (example: X86 push tv22(reg32).i8.   Keep track
   // of largest pseudo register associated with live range.

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add global register live range to allocator
//
// Note: 
//
//    Each distinct physical register that crosses a tile boundary gets their own live range. This allows for
//    full preferencing and register constraints.
//
//------------------------------------------------------------------------------

GraphColor::LiveRange *
Allocator::AddGlobalRegisterLiveRange
(
   unsigned  reg
)
{
   Tiled::VR::Info *              vrInfo = this->VrInfo;
   GraphColor::LiveRangeVector *  liveRangeVector = this->GlobalLiveRangeVector;
   unsigned                       vrTag = vrInfo->GetTag(reg);
   unsigned                       liveRangeId;
   GraphColor::LiveRange *        liveRange = this->GetGlobalLiveRange(vrTag);

   if (liveRange == nullptr) {
      unsigned originalLiveRangeId = this->GetGlobalLiveRangeIdWithOverlap(vrTag);
        // On an architecture without sub-registers, if liveRange is null
        // then GetGlobalLiveRangeIdWithOverlap will always return 0.

      if (originalLiveRangeId == 0) {
         // New, write overlapped

         liveRangeId = liveRangeVector->size();
         liveRange = GraphColor::LiveRange::NewGlobal(this, liveRangeId, vrTag);
         liveRangeVector->push_back(liveRange);

         this->MapGlobalAliasTagToRegisterLiveRangeIdOverlapped(vrTag, liveRangeId);

         liveRange->IsPreColored = true;
         liveRange->IsPhysicalRegister = true;

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;
      } else {
         assert(originalLiveRangeId != 0);

         // Widen
         liveRange = (*liveRangeVector)[originalLiveRangeId];
         assert(liveRange != nullptr);

         assert(vrInfo->MustTotallyOverlap(vrTag, liveRange->VrTag));
         liveRange->VrTag = vrTag;

         this->MapGlobalAliasTagToLiveRangeIdOverlappedExclusive(vrTag, originalLiveRangeId);

         assert(VR::Info::IsPhysicalRegister(reg));
         liveRange->Register = reg;
      }

   }  else {

      // <place for code supporting architectures with sub-registers>

   }

   return liveRange;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Reset alias tag to live range id map.  Grow the table size to handle
//    any new alias tags that have been added do to spilling.
//
//------------------------------------------------------------------------------

void
Allocator::ResetAliasTagToIdMap()
{
   Tiled::VR::Info *           vrInfo = this->VrInfo;
   llvm::MachineRegisterInfo&  MRI = this->MF->getRegInfo();
   unsigned  numberLocationTags = MRI.getNumVirtRegs();
   unsigned  maximumAliasTag = vrInfo->HighPhysicalRegister + 1 + numberLocationTags * 2;
   this->VRM->grow();

   GraphColor::IdVector * aliasTagToIdVector = this->AliasTagToIdVector;

   // The first time into the register allocator the array won't have been allocated yet.

   if (aliasTagToIdVector == nullptr) {
      aliasTagToIdVector = new GraphColor::IdVector(maximumAliasTag, 0);
      this->AliasTagToIdVector = aliasTagToIdVector;
      return;
   }

   unsigned mapSize = aliasTagToIdVector->size();

   for (unsigned i = 0; i < mapSize; i++)
   {
      (*aliasTagToIdVector)[i] = 0;
   }

   // We should only reallocate this array if it is too small.
   if (mapSize < (vrInfo->HighPhysicalRegister + 1 + numberLocationTags)) {
      aliasTagToIdVector->resize(maximumAliasTag, 0);
   }
}

#if defined(TILED_DEBUG_DUMPS)
void
dumpTile
(
   GraphColor::Tile * tile
)
{
   std::cout << "\n **** Tile#" << tile->Id << "  (Kind=" << int(tile->TileKind) << ") : parent = ";
   if (tile->ParentTile) {
      std::cout << "Tile#" << tile->ParentTile->Id << std::endl;
   }
   else {
      std::cout << std::endl;
   }

   Graphs::MachineBasicBlockVector::iterator tb;
   Graphs::MachineBasicBlockList::iterator bi;

   std::cout << "   blocks = { ";
   // foreach_block_in_tile
   for (tb = tile->BlockVector->begin(); tb != tile->BlockVector->end(); ++tb)
   {
      std::cout << (*tb)->getNumber() << ", ";
   }
   std::cout << "}" << std::endl;

   std::cout << "   entry =  { ";
   // foreach_tile_entry_block
   for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
   {
      std::cout << (*bi)->getNumber() << ", ";
   }
   std::cout << "}" << std::endl;

   std::cout << "   exit  =   { ";
   // foreach_tile_entry_block
   for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
   {
      std::cout << (*bi)->getNumber() << ", ";
   }
   std::cout << "}" << std::endl;
}
#endif

void
filterDbgValueInstructions(llvm::MachineFunction * mf)
{
   llvm::MachineFunction::iterator mbb;
   llvm::MachineBasicBlock::iterator i, next_i;

   for (mbb = mf->begin(); mbb != mf->end(); ++mbb) {
      if (!mbb->empty()) {
         for (i = mbb->instr_begin(); i != mbb->instr_end(); i = next_i) {
            next_i = i; ++next_i;
            if (i->getOpcode() == llvm::TargetOpcode::DBG_VALUE) {
               mbb->erase_instr(&(*i));
            }
         }
      }
   }
}

void
filterRedundantCopyInstructions(llvm::MachineFunction * mf)
{
   llvm::MachineFunction::iterator mbb;
   llvm::MachineBasicBlock::iterator i, next_i;

   for (mbb = mf->begin(); mbb != mf->end(); ++mbb) {
      if (!mbb->empty()) {
         for (i = mbb->instr_begin(); i != mbb->instr_end(); i = next_i) {
            next_i = i; ++next_i;
            if (i->isCopy() && i->getNumOperands() == 2) {
               if ((i->getOperand(0)).getReg() == (i->getOperand(1)).getReg()) {
                  mbb->erase_instr(&(*i));
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Root allocator routine.  Color all the appearances of a given
//    function unit.
//
// Arguments:
//
//    functionUnit - Function unit to allocate.
//
//------------------------------------------------------------------------------

void
Allocator::Allocate()
{
   {
      llvm::NamedRegionTimer T("build", "Build", TimerGroupName,
         TimerGroupDescription, llvm::TimePassesIsEnabled);

      // ISSUE-REVIEW-aasmith-2017/02/17: Review handling of debug values
      filterDbgValueInstructions(this->FunctionUnit->machineFunction);
      //llvm::dbgs() << " *** function :  " << this->FunctionUnit->machineFunction->getName().str() << "\n";
  
      // Begin register allocation.

      SpillOptimizer::ResetIrExtensions();
      this->Initialize();
      this->VRM->grow();

      DEBUG(llvm::dbgs() << "\n====== Allocating function " << MF->getName() << "\n");
      // Build the necessary support data structures.  These steps must
      // be done in order, since each step depends on the previous step.

      this->BuildFlowGraph();

      // BuildLoopGraph() is just an empty stub, in this llvm implementation of Tiled RA the loop analysis was
      // requested and done prior to calling this function, the analysis results were stored in this->LoopInfo.
      this->BuildLoopGraph();
      this->BuildLiveness();
      this->BuildTileGraph();
      this->BuildGlobalLiveRanges();
      this->BuildGlobalConflictGraph();
      this->BuildGlobalPreferenceGraph();   // if commented out, blocks execution of multi-tile code
      this->BuildGlobalAvailable();
      this->ScheduleTileGraph();
      this->CostGlobalLiveRanges();   // if commented out, blocks execution of multi-tile code
   }
   // Pass one - allocate bottom up each tile

   this->BeginPass(GraphColor::Pass::Allocate);

   GraphColor::TileList * postOrderTiles = this->TileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      this->Allocate(tile);
   }

   this->EndPass(GraphColor::Pass::Allocate);

   // Pass two - assign physical registers and insert tile compensation code.

   this->BeginPass(GraphColor::Pass::Assign);

   GraphColor::TileList * preOrderTiles = this->TileGraph->PreOrderTileList;

   // foreach_tile_in_dfs_preorder
   for (t = preOrderTiles->begin(); t != preOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      this->Assign(tile);
   }

   this->EndPass(GraphColor::Pass::Assign);

   DEBUG({
      if (this->SpillOptimizer->inFunctionSpilledLRs > 0) {
         llvm::dbgs() << " +++ SpilledLRs = " << this->SpillOptimizer->inFunctionSpilledLRs << "  ("
            << this->FunctionUnit->machineFunction->getName().str() << ")" << "\n";
         llvm::dbgs() << " +++ RemovedLocalLRs = " << this->inFunctionRemovedLocalLRs << "\n";
      }
   });

   // Delete the support data structures.  Note, we may want to leave these around for
   // subsequent phases.

   this->DeleteTileGraph();
   this->DeleteLiveness();

   // End register allocation.

   // ISSUE-REVIEW-aasmith-2017/02/17: Review handling of redundant copies
   filterRedundantCopyInstructions(this->FunctionUnit->machineFunction);

#if defined(TILED_DEBUG_SUPPORT)
   if (this->FunctionUnit->machineFunction->getName().str() == "***") {
      llvm::dbgs() << "\n\n **** CFG AFTER ALLOCATION ****" << "\n";
      this->FunctionUnit->machineFunction->dump();
   }
#endif

   ////this->ReverseCanonicalizeIR();
   this->Terminate();

   return;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate a tile.  Color all the appearances in a given tile.
//
// Arguments:
//
//    tile - Region to allocate
//
//------------------------------------------------------------------------------

void
Allocator::Allocate
(
   GraphColor::Tile * tile
)
{
   llvm::NamedRegionTimer T("allocate", "Allocate", TimerGroupName,
      TimerGroupDescription, llvm::TimePassesIsEnabled);
   // Begin allocation pass on this tile, allocate memory.
   DEBUG(llvm::dbgs() << "\n==== Allocating tile " << tile->Id << " of kind " << (int)tile->TileKind << "\n");
   this->BeginPass(tile, this->Pass);

   // Graph coloring loop reduces the conflict graph to <= K colorable
   // inserting spill code for uncolorable live ranges.

   bool isColored = false;

   do
   {
      // Initialize per iteration data structures and memory.
      this->BeginIteration(tile, this->Pass);

      // Attempt to color the tile.
      DEBUG(llvm::dbgs() << "== Color\n");
      isColored = this->Color(tile);

      // If there were uncolorable live ranges in tile insert spill code and keep iterating.

      if (!isColored) {
         DEBUG(llvm::dbgs() << "== Spill\n");
         isColored = this->Spill(tile);
      }

      if (isColored) {
         // Otherwise summarize tile coloring, we are done iterating.

         DEBUG(llvm::dbgs() << "== Summarize\n");
         this->Summarize(tile);
      }

      // Clean up per iteration data structures and memory.
      this->EndIteration(tile, this->Pass);

   } while (!isColored);

   // End allocation pass for this tile, free memory.

   this->EndPass(tile, this->Pass);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Assigns registers to the register candidates in a given tile if it is
//    possible to do so without spilling.  Returns whether or not it succeeded.
//
// Arguments:
//
//    tile - tile to do allocation for
//
//------------------------------------------------------------------------------

bool
Allocator::Color
(
   GraphColor::Tile * tile
)
{
   // We use a more traditional graph coloring approach that involves
   // building a conflict graph.

   // Build tile liveness as well as conflict and preference graphs
   this->Build(tile);

   // Compute costs for tile live ranges
   this->Cost(tile);

   // Stackify tile liveranges on allocator stack
   this->Simplify(tile);

   // Color allocator stack for tile
   return this->Select(tile);
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Build information required for single iteration of allocation on a tile.
//    This includes liveness, live ranges, build conflict graph and preference graph.
//
//    Also, analyze tile for constant value definitions.
//
// Arguments:
//
//    tile - tile to build iteration info for
//
//------------------------------------------------------------------------------

void
Allocator::Build
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *            allocator = tile->Allocator;
   GraphColor::Liveness *             liveness = allocator->Liveness;
   GraphColor::ConflictGraph *        conflictGraph = tile->ConflictGraph;
   GraphColor::PreferenceGraph *      preferenceGraph = tile->PreferenceGraph;
   GraphColor::AvailableExpressions * availableExpressions = tile->AvailableExpressions;

   //TIMER_START(ra_tilebuild, L"Tile build", Toolbox::TimerKind::Function);

   // The following steps must be done in order, as each step relies on the
   // previous step.

   liveness->ComputeRegisterLiveness(tile);
   liveness->RemoveDeadCode(tile);
   liveness->PruneRegisterLiveness(tile);

   liveness->BuildLiveRanges(tile);

   conflictGraph->Build();

   if (preferenceGraph != nullptr) {
      preferenceGraph->Build();
   }

   availableExpressions->ComputeNeverKilled(tile);

   if ((tile->Pass == GraphColor::Pass::Allocate) && (tile->Iteration > 1)) {
      tile->UpdateGlobalConflicts();
   }

   //TIMER_END(ra_tilebuild, L"Tile build", Toolbox::TimerKind::Function);
}


void
markLiveRangesSpanningCall
(
   llvm::SparseBitVector<> * liveBitVector,
   GraphColor::Tile *        tile
)
{
   tile->NumberCalls++;
   unsigned  liveAliasTag;

   llvm::SparseBitVector<>::iterator l;

   // foreach_sparse_bv_bit
   for (l = liveBitVector->begin(); l != liveBitVector->end(); ++l)
   {
      liveAliasTag = *l;

      GraphColor::LiveRange * liveRange = tile->GetLiveRange(liveAliasTag);

      if ((liveRange != nullptr) && !liveRange->IsPreColored) {
         if (!liveRange->IsCallSpanning) {
            tile->NumberCallSpanningLiveRanges++;
            liveRange->IsCallSpanning = true;
         }
      }
   }
}

void
Allocator::MarkLiveRangesSpanningCall
(
   GraphColor::Tile * tile
)
{
   GraphColor::Liveness *   liveness = this->Liveness;
   Dataflow::LivenessData * registerLivenessData;

   // Create temporary bit vectors.
   llvm::SparseBitVector<> liveBitVector;
   llvm::SparseBitVector<> genBitVector;
   llvm::SparseBitVector<> killBitVector;

   Graphs::MachineBasicBlockVector::reverse_iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile_backward
   for (biter = mbbVector->rbegin(); biter != mbbVector->rend(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;

      registerLivenessData = liveness->GetRegisterLivenessData(block);
      liveBitVector = (*registerLivenessData->LiveOutBitVector);
      // Process each instruction in the block moving backwards calculating liveness.

      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward_editing
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);

         liveness->TransferInstruction(instruction, &genBitVector, &killBitVector);

         if (!Tile::IsEnterTileInstruction(instruction)) {
            // Note live ranges that span calls.
            if (instruction->isCall()) {
               markLiveRangesSpanningCall(&liveBitVector, tile);
            }
         }

         // Update liveness gens and kills.
         liveness->UpdateInstruction(instruction, &liveBitVector, &genBitVector, &killBitVector);
      }
   }
}

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Compute live range costs for allocate, spill, recalculate
//
// Arguments:
//
//    tile - tile to cost
//
//------------------------------------------------------------------------------

void
Allocator::Cost
(
   GraphColor::Tile * tile
)
{
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   GraphColor::LiveRange *   liveRange;
   llvm::SparseBitVector<> * scratchBitVector = this->ScratchBitVector1;
   bool                      doInsert = false;
   Tiled::Cost               infiniteCost = this->InfiniteCost;

   //TIMER_START(ra_tilecost, L"Tile cost", Toolbox::TimerKind::Function);

   // Do costing with allocator spill dummy to avoid extraneous entries in the symbol table.
   this->CostSpillSymbol = this->DummySpillSymbol;

   // Initialize the spiller for this tile
   GraphColor::SpillOptimizer * spillOptimizer = this->SpillOptimizer;
   spillOptimizer->Initialize(tile);

   // Initialize globals with bottom up costs based on nested tiles.
   //
   this->CostGlobals(tile);  // if commented, blocks execution of multi-tile code:  

   llvm::SparseBitVector<> * doNotSpillAliasTagBitVector = this->DoNotSpillAliasTagBitVector;
   Graphs::MachineBasicBlockVector * blockVector = tile->BlockVector;
   Graphs::MachineBasicBlockVector::iterator b;

   // foreach_block_in_tile_by_ebb_forward
   for (b = blockVector->begin(); b != blockVector->end(); ++b)
   {
      llvm::MachineBasicBlock * block = *b;

      // Reset tile spill context for block boundary.
      this->BeginBlock(block, tile);

      llvm::MachineInstr * instruction = nullptr;
      llvm::MachineBasicBlock::instr_iterator(ii);

      // foreach_instr_in_block
      for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
      {
         instruction = &(*ii);

         if (Tile::IsTileBoundaryInstruction(instruction)) {

            if (Tile::IsEnterTileInstruction(instruction)) {
               llvm::iterator_range<llvm::MachineInstr::mop_iterator> defs(instruction->implicit_operands().begin(),
                                                                           instruction->implicit_operands().end()  );
               llvm::MachineInstr::mop_iterator destinationOperand;

               // foreach_register_destination_opnd
               for (destinationOperand = defs.begin(); destinationOperand != defs.end(); ++destinationOperand)
               {
                  if (destinationOperand->isReg()) {
                     liveRange = tile->GetLiveRange(destinationOperand);

                     if (liveRange->IsSummaryProxy && !liveRange->IsCalleeSaveValue) {
                        liveRange->MarkAsInfinite();
                     }
                  }
               }
            }

            spillOptimizer->TileBoundary(instruction);
            spillOptimizer->Finish(instruction, tile);

            continue;
         }

         // Clear scratch that tracks alias tags across the instruction.
         scratchBitVector->clear();

         // foreach_register_source_and_destination_opnd
         foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
            unsigned aliasTag = vrInfo->GetTag(operand->getReg());

            if (scratchBitVector->test(aliasTag)) {
               // early out if we've already seen this tag in this instruction enforces that we only make one
               // costing call per unique tag (to preserve opeq semantics)
               next_source_and_destination_opnd_v2(operand, instruction, end_iter);
               continue;
            }

            vrInfo->OrMayPartialTags(aliasTag, scratchBitVector);

            if (Tiled::VR::Info::IsVirtualRegister(operand->getReg())) {
               //was:  operand->Register->IsRegisterCategoryPseudo
               liveRange = tile->GetLiveRange(aliasTag);
               aliasTag = liveRange->VrTag;

               if (liveRange->IsTrivial) {
                  doNotSpillAliasTagBitVector->set(aliasTag);
                  liveRange->MarkAsInfinite();
               } else if (liveRange->IsSummaryProxy || doNotSpillAliasTagBitVector->test(aliasTag)) {
                  liveRange->MarkAsInfinite();
               }

               // fill in live range costs for this appearance
               spillOptimizer->Cost(operand, instruction, tile);
            }

            next_source_and_destination_opnd_v2(operand, instruction, end_iter);
         }

         // Finished processing instruction
         spillOptimizer->Finish(instruction, tile, doInsert);
      }

      this->EndBlock(block, tile);
   }

   // Compute hazard spill strategies - Caller save, pressure.

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();
   // foreach_liverange_in_tile
   for (unsigned l = 1; l < lrVector->size(); ++l)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[l];

      spillOptimizer->CostCallerSave(liveRange, tile);
   }

   // Iterate through live ranges now that all costs are set and add call crossing preferences based on final
   // tile costs.

   if (tile->PreferenceGraph != nullptr) {
      GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

      // foreach_liverange_in_tile
      for (unsigned l = 1; l < lrVector->size(); ++l)
      {
         GraphColor::LiveRange *  liveRange = (*lrVector)[l];

         if (liveRange->IsCallSpanning) {

            llvm::SparseBitVector<> * availableRegistersAliasTagBitVector
               = this->GetAllocatableRegisters(tile, liveRange);
            RegisterAllocator::PreferenceConstraint  preferenceConstraint;
            GraphColor::PreferenceGraph *            preferenceGraph = tile->PreferenceGraph;
            Tiled::Cost                              cost;

            GraphColor::GraphIterator iter;
            GraphColor::LiveRange *   preferenceLiveRange;

            // foreach_preference_liverange_with_iterator
            for (preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&iter, liveRange, &cost, &preferenceConstraint);
                 (preferenceLiveRange != nullptr);
                 preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&iter, &cost, &preferenceConstraint))
            {
               unsigned preferenceTag = preferenceLiveRange->VrTag;

               if (vrInfo->CommonMayPartialTags(preferenceTag, availableRegistersAliasTagBitVector)) {
                  if (!liveRange->IsGlobal()) {
                     Tiled::Cost benefitCost;
                     Tiled::Cost preferenceCost = liveRange->GetBestSpillCost();
                     benefitCost.Copy(&cost);
                     benefitCost.IncrementBy(&preferenceCost);
                     preferenceGraph->SetPreferenceCost(&iter, &benefitCost);
                  }
               }
            }
         }
      }
   }

   // Finished tile - reset any remaining state (tile boundaries may not match EBBs exactly)
   spillOptimizer->Reset();

   this->ComputeCalleeSaveHeuristic(tile);  // if commented, blocks execution of multi-tile code:  

   // Clear dummy spill symbol now that costing is complete

   this->CostSpillSymbol = VR::Constants::InvalidFrameIndex;

   //TIMER_END(ra_tilecost, L"Tile cost", Toolbox::TimerKind::Function);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute summary live range costs for reload, spill, and recalculate at nested tile boundaries.
//
// Arguments:
//
//    tile - tile to cost
//
//------------------------------------------------------------------------------

void
Allocator::CostSummary
(
   GraphColor::Tile * tile
)
{
   Graphs::FlowGraph *               flowGraph = this->FunctionUnit;
   Tiled::VR::Info *                 vrInfo = this->VrInfo;
   GraphColor::SpillOptimizer *      spillOptimizer = this->SpillOptimizer;
   GraphColor::AllocatorCostModel *  costModel = tile->CostModel;

   GraphColor::LiveRangeVector *  slrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned slr = 1; slr < slrVector->size(); ++slr)
   {
      GraphColor::LiveRange *   summaryLiveRange = (*slrVector)[slr];
      assert(summaryLiveRange->IsSummary());

      llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
      Tiled::Cost               totalAllocationCost;
      Tiled::Cost               totalSpillCost;
      Tiled::Cost               entryCost;
      Tiled::Cost               exitCost;
      Tiled::Cost               zeroCost = this->ZeroCost;

      Tiled::Cost summaryWeightCost = summaryLiveRange->WeightCost;

      if (costModel->Compare(&summaryWeightCost, &zeroCost) > 0) {
         // We have local appearances that were allocated a register pass 1.  This tile segment can not be
         // spilled.

         summaryLiveRange->AllocationCost = this->ZeroCost;
         summaryLiveRange->SpillCost = this->InfiniteCost;
         summaryLiveRange->RecalculateCost = this->InfiniteCost;
#ifdef ARCH_WITH_FOLDS
         summaryLiveRange->FoldCost = this->InfiniteCost;
#endif
         summaryLiveRange->CallerSaveCost = this->InfiniteCost;
         continue;
      }

      // Start at zero for each summary.

      // Spill cost is the cost of inserting compensation code at the entries/exits of nested/parent tiles
      // assuming a spill.
      totalSpillCost = this->ZeroCost;

      // Allocation cost is the cost of inserting compensation code at entries/exits of nested/parent tiles
      // assuming a register.

      totalAllocationCost = this->ZeroCost;

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         Graphs::MachineBasicBlockList::iterator bi;
         llvm::SparseBitVector<>::iterator a;

         // foreach_tile_entry_block
         for (bi = nestedTile->EntryBlockList->begin(); bi != nestedTile->EntryBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * entryBlock = *bi;

            llvm::SparseBitVector<> * liveInBitVector = this->Liveness->GetGlobalAliasTagSet(entryBlock);

            // foreach_sparse_bv_bit
            for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
            {
               unsigned aliasTag = *a;

               if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
                  continue;
               }

               if (vrInfo->CommonMayPartialTags(aliasTag, liveInBitVector)) {

                  llvm::MachineInstr * firstInstruction = &(entryBlock->front());
                  llvm::MachineInstr * enterTileInstruction = flowGraph->FindNextInstructionInBlock(llvm::TargetOpcode::ENTERTILE, firstInstruction);
                  assert(enterTileInstruction != nullptr);

                  GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(aliasTag);
                  assert(globalLiveRange != nullptr);
                  unsigned nestedSummaryRegister = nestedTile->GetSummaryRegister(aliasTag);
                  unsigned globalAliasTag = globalLiveRange->VrTag;

                  if (nestedSummaryRegister != VR::Constants::InvalidReg) {

                     if (nestedTile->GlobalTransitiveUsedAliasTagSet->test(globalAliasTag)
                        || nestedTile->GlobalTransitiveDefinedAliasTagSet->test(globalAliasTag)) {
                        entryCost = spillOptimizer->ComputeEnterLoad(tile, aliasTag, nestedSummaryRegister, enterTileInstruction, false);
                        totalSpillCost.IncrementBy(&entryCost);
                     }

                     // If we can show coming top down that we will get a copy cost it.

                     GraphColor::LiveRange * nestedSummaryLiveRange = nestedTile->GetSummaryLiveRange(aliasTag);

                     if (summaryLiveRange->HasHardPreference && nestedSummaryLiveRange->HasHardPreference &&
                         this->GetBaseRegisterCategory(summaryLiveRange->Register) != this->GetBaseRegisterCategory(nestedSummaryRegister)) {
                        //for architectures with only one kind of registers the third condition is always false

                        entryCost = spillOptimizer->ComputeTransfer(nestedTile, aliasTag, entryBlock);
                        totalAllocationCost.IncrementBy(&entryCost);
                     }
                  }
               }
            }
         }

         // foreach_tile_exit_block
         for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * exitBlock = *bi;

            llvm::SparseBitVector<> * liveOutBitVector = this->Liveness->GetGlobalAliasTagSet(exitBlock);

            // foreach_sparse_bv_bit
            for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
            {
               unsigned aliasTag = *a;

               if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
                  continue;
               }

               if (vrInfo->CommonMayPartialTags(aliasTag, liveOutBitVector)) {

                  llvm::MachineInstr * lastInstruction = &(exitBlock->back());
                  llvm::MachineInstr * exitTileInstruction = flowGraph->FindPreviousInstructionInBlock(llvm::TargetOpcode::EXITTILE, lastInstruction);
                  assert(exitTileInstruction != nullptr);

                  GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(aliasTag);
                  assert(globalLiveRange != nullptr);
                  unsigned nestedSummaryRegister = nestedTile->GetSummaryRegister(aliasTag);
                  unsigned globalAliasTag = globalLiveRange->VrTag;

                  // If there is a allocated register in the nested region

                  if (nestedSummaryRegister != VR::Constants::InvalidReg) {

                     if (nestedTile->GlobalTransitiveDefinedAliasTagSet->test(globalAliasTag)) {
                        exitCost = spillOptimizer->ComputeExitSave(tile, aliasTag, nestedSummaryRegister, exitTileInstruction, false);
                        totalSpillCost.IncrementBy(&exitCost);
                     }

                     GraphColor::LiveRange * nestedSummaryLiveRange = nestedTile->GetSummaryLiveRange(aliasTag);

                     if (summaryLiveRange->HasHardPreference && nestedSummaryLiveRange->HasHardPreference &&
                         this->GetBaseRegisterCategory(summaryLiveRange->Register) != this->GetBaseRegisterCategory(nestedSummaryRegister)) {
                        //for architectures with only one kind of registers the third condition is always false

                        exitCost = spillOptimizer->ComputeTransfer(nestedTile, aliasTag, exitBlock);
                        totalAllocationCost.IncrementBy(&exitCost);
                     }
                  }
               }
            }
         }

      }

      GraphColor::Tile * parentTile = tile->ParentTile;

      // Compute Entry and Exit cost to the parent tile.

      Graphs::MachineBasicBlockList::iterator bi;
      llvm::SparseBitVector<>::iterator a;

      // foreach_tile_entry_block
      for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;

         llvm::SparseBitVector<> * liveInBitVector = this->Liveness->GetGlobalAliasTagSet(entryBlock);

         // foreach_sparse_bv_bit
         for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
         {
            unsigned aliasTag = *a;

            if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            if (vrInfo->CommonMayPartialTags(aliasTag, liveInBitVector)) {
               llvm::MachineInstr * firstInstruction = &(entryBlock->front());

               llvm::MachineInstr * nextInstr = firstInstruction->getNextNode();
               if (nextInstr && Tile::IsEnterTileInstruction(nextInstr)) {
                  firstInstruction = nextInstr;
               }

               unsigned summaryLiveRangeRegister = parentTile->GetSummaryRegister(aliasTag);

               if (summaryLiveRangeRegister != VR::Constants::InvalidReg) {
                  exitCost = spillOptimizer->ComputeExitSave(tile, aliasTag, summaryLiveRangeRegister, firstInstruction, false);
                  totalSpillCost.IncrementBy(&exitCost);
               }

               entryCost = spillOptimizer->ComputeTransfer(parentTile, aliasTag, entryBlock);
               totalAllocationCost.IncrementBy(&entryCost);
            }
         }
      }

      // foreach_tile_exit_block
      for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * exitBlock = *bi;

         llvm::SparseBitVector<> * liveOutBitVector = this->Liveness->GetGlobalAliasTagSet(exitBlock);

         // foreach_sparse_bv_bit
         for (a = summaryAliasTagSet->begin(); a != summaryAliasTagSet->end(); ++a)
         {
            unsigned aliasTag = *a;

            if (vrInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            if (vrInfo->CommonMayPartialTags(aliasTag, liveOutBitVector)) {
               llvm::MachineInstr * lastInstruction = &(exitBlock->back());

               llvm::MachineInstr * previousInstr = lastInstruction->getPrevNode();
               if (previousInstr && Tile::IsExitTileInstruction(previousInstr)) {
                  lastInstruction = previousInstr;
               }

               unsigned summaryLiveRangeRegister = parentTile->GetSummaryRegister(aliasTag);

               if (summaryLiveRangeRegister != VR::Constants::InvalidReg) {
                  entryCost = spillOptimizer->ComputeEnterLoad(tile, aliasTag, summaryLiveRangeRegister, lastInstruction, false);
                  totalSpillCost.IncrementBy(&entryCost);
               }

               exitCost = spillOptimizer->ComputeTransfer(parentTile, aliasTag, exitBlock);
               totalAllocationCost.IncrementBy(&exitCost);
            }
         }
      }

      summaryLiveRange->AllocationCost = totalAllocationCost;
      summaryLiveRange->SpillCost = totalSpillCost;
#ifdef ARCH_WITH_FOLDS
      summaryLiveRange->FoldCost = this->InfiniteCost;
#endif
      summaryLiveRange->RecalculateCost = this->InfiniteCost;
      summaryLiveRange->CallerSaveCost = this->InfiniteCost;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute global live range costs for nested tile transfers.
//
// Arguments:
//
//    tile - tile to cost
//
//------------------------------------------------------------------------------

void
Allocator::CostGlobals
(
   GraphColor::Tile * tile
)
{
   Graphs::FlowGraph *               flowGraph = this->FunctionUnit;
   Tiled::VR::Info *                 aliasInfo = this->VrInfo;
   GraphColor::SpillOptimizer *      spillOptimizer = this->SpillOptimizer;
   GraphColor::AllocatorCostModel *  costModel = tile->CostModel;
   Tiled::Cost                       zeroCost = this->ZeroCost;

   GraphColor::LiveRangeVector * lrVector = tile->GetLiveRangeEnumerator();

   // foreach_liverange_in_tile
   for (unsigned idx = 1 /*vector-base1*/; idx < lrVector->size(); ++idx)
   {
      GraphColor::LiveRange *  liveRange = (*lrVector)[idx];

      if (!liveRange->IsGlobal() || liveRange->IsPhysicalRegister) {
         continue;
      }

      // Initialize costs so bottom up decisions are predictable.
      liveRange->InitializeCosts();

      // Iterate through nested tiles and look entry/exit and cost the transfers to/from memory if any.

      unsigned     globalAliasTag = liveRange->VrTag;
      Tiled::Cost  allocationTransferCost = this->ZeroCost;
      Tiled::Cost  spillTransferCost = this->ZeroCost;
      Tiled::Cost  recalculateTransferCost = this->ZeroCost;
      unsigned     liveRangeRegister = liveRange->Register;

      // If global allocation cost is greater than best global spill cost assume that the global is not
      // allocated in the parent.

      GraphColor::LiveRange * globalLiveRange = liveRange->GlobalLiveRange;

      llvm::SparseBitVector<> * globalLiveInAliasTagSet = tile->GlobalLiveInAliasTagSet;
      llvm::SparseBitVector<> * globalLiveOutAliasTagSet = tile->GlobalLiveOutAliasTagSet;

      // Mark live ranges that cross a tile boundary as having a spill partition (used to reason about the
      // benefit of spilling the live range with respect to register cost).

      if (globalLiveInAliasTagSet->test(globalLiveRange->VrTag) || globalLiveOutAliasTagSet->test(globalLiveRange->VrTag)) {
         liveRange->HasSpillPartition = true;
      }

      Tiled::Cost bestGlobalSpillCost = globalLiveRange->GetBestSpillCost(tile->CostModel);
      Tiled::Cost globalAllocationCost = globalLiveRange->AllocationCost;

      if (costModel->Compare(&bestGlobalSpillCost, &globalAllocationCost) < 0) {
         // Foreach entry and exit cost the enter/exit save info.

         Graphs::MachineBasicBlockList::iterator bi;

         // foreach_tile_entry_block
         for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * entryBlock = *bi;

            llvm::SparseBitVector<> * liveInBitVector = this->Liveness->GetGlobalAliasTagSet(entryBlock);

            if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveInBitVector)) {

               llvm::MachineInstr * firstInstruction = &(entryBlock->front());
               llvm::MachineInstr * enterTileInstruction = flowGraph->FindNextInstructionInBlock(llvm::TargetOpcode::ENTERTILE, firstInstruction);
               assert(enterTileInstruction != nullptr);

               Tiled::Cost  entryCost =
                  spillOptimizer->ComputeEnterLoad(tile, globalAliasTag, liveRangeRegister, enterTileInstruction, false);
               allocationTransferCost.IncrementBy(&entryCost);
            }
         }

         // foreach_tile_exit_block
         for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * exitBlock = *bi;

            llvm::SparseBitVector<> * liveOutBitVector = this->Liveness->GetGlobalAliasTagSet(exitBlock);

            if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveOutBitVector)) {

               llvm::MachineInstr * lastInstruction = &(exitBlock->back());
               llvm::MachineInstr * exitTileInstruction = flowGraph->FindPreviousInstructionInBlock(llvm::TargetOpcode::EXITTILE, lastInstruction);
               assert(exitTileInstruction != nullptr);

               Tiled::Cost  exitCost =
                  spillOptimizer->ComputeExitSave(tile, globalAliasTag, liveRangeRegister, exitTileInstruction, false);
               allocationTransferCost.IncrementBy(&exitCost);
            }
         }
      }

      GraphColor::TileList::iterator nt;

      // foreach_nested_tile_in_tile
      for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
      {
         GraphColor::Tile * nestedTile = *nt;

         if (!nestedTile->GlobalAliasTagSet->test(globalAliasTag)) {
            continue;
         }

         GraphColor::Decision decision = nestedTile->GetGlobalDecision(globalAliasTag);

         if (decision != GraphColor::Decision::Allocate  && liveRange->IsCalleeSaveValue && !this->IsShrinkWrappingEnabled) {
            // Live range leading into a nested tile where it's spilled.  Force all callee save values to
            // spill from here on up.  (force to spill at entry)

            liveRange->AllocationCost = this->InfiniteCost;
         }

         Graphs::MachineBasicBlockList::iterator bi;

         // foreach_tile_entry_block
         for (bi = nestedTile->EntryBlockList->begin(); bi != nestedTile->EntryBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * entryBlock = *bi;

            llvm::SparseBitVector<> * liveInBitVector = this->Liveness->GetGlobalAliasTagSet(entryBlock);

            if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveInBitVector)) {

               llvm::MachineInstr * firstInstruction = &(entryBlock->front());
               llvm::MachineInstr * enterTileInstruction = flowGraph->FindNextInstructionInBlock(llvm::TargetOpcode::ENTERTILE, firstInstruction);
               assert(enterTileInstruction != nullptr);

               Tiled::Cost         entryCost;

               switch (decision)
               {
                  case GraphColor::Decision::Memory:
                  {
                     entryCost =
                        spillOptimizer->ComputeExitSave(nestedTile, globalAliasTag, liveRangeRegister, enterTileInstruction, false);
                     allocationTransferCost.IncrementBy(&entryCost);

                     recalculateTransferCost = this->InfiniteCost;
                  }
                  break;

                  case GraphColor::Decision::Recalculate:
#ifdef ARCH_WITH_FOLDS
                  case GraphColor::Decision::Fold:
#endif
                  {
                     spillTransferCost = this->InfiniteCost;
                  }
                  break;

                  case GraphColor::Decision::CallerSave:
                  case GraphColor::Decision::Allocate:
                  {
                     if (nestedTile->HasTransitiveGlobalReference(globalAliasTag)) {
                        unsigned nestedRegister = nestedTile->GetSummaryRegister(globalAliasTag);
                        assert(nestedRegister != VR::Constants::InvalidReg);

                        entryCost = spillOptimizer->ComputeEnterLoad(tile, globalAliasTag, nestedRegister, enterTileInstruction, false);
                        spillTransferCost.IncrementBy(&entryCost);

                        recalculateTransferCost.IncrementBy(&entryCost);
                     }
                  }
                  break;

                  default:
                  {
                     llvm_unreachable("No decision!");
                  }
               };
            }
         }

         // foreach_tile_exit_block
         for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
         {
            llvm::MachineBasicBlock * exitBlock = *bi;

            llvm::SparseBitVector<> * liveOutBitVector = this->Liveness->GetGlobalAliasTagSet(exitBlock);

            if (aliasInfo->CommonMayPartialTags(globalAliasTag, liveOutBitVector)) {

               llvm::MachineInstr * lastInstruction = &(exitBlock->back());
               llvm::MachineInstr * exitTileInstruction = flowGraph->FindPreviousInstructionInBlock(llvm::TargetOpcode::EXITTILE, lastInstruction);
               assert(exitTileInstruction != nullptr);

               Tiled::Cost         exitCost;

               switch (decision)
               {
                  case GraphColor::Decision::Memory:
                  {
                     exitCost =
                        spillOptimizer->ComputeEnterLoad(nestedTile, globalAliasTag, liveRangeRegister, exitTileInstruction, false);
                     allocationTransferCost.IncrementBy(&exitCost);

                     recalculateTransferCost = this->InfiniteCost;
                  }
                  break;

                  case GraphColor::Decision::Recalculate:
#ifdef ARCH_WITH_FOLDS
                  case GraphColor::Decision::Fold:
#endif
                  {
                     spillTransferCost = this->InfiniteCost;

                     exitCost =
                        spillOptimizer->ComputeEnterLoad(nestedTile, globalAliasTag, liveRangeRegister, exitTileInstruction, false);
                     allocationTransferCost.IncrementBy(&exitCost);

                  }
                  break;

                  case GraphColor::Decision::CallerSave:
                  case GraphColor::Decision::Allocate:
                  {
                     if (nestedTile->HasTransitiveGlobalReference(globalAliasTag)) {
                        unsigned nestedRegister = nestedTile->GetSummaryRegister(globalAliasTag);
                        assert(nestedRegister != VR::Constants::InvalidReg);

                        exitCost = spillOptimizer->ComputeExitSave(tile, globalAliasTag, nestedRegister, exitTileInstruction, false);
                        spillTransferCost.IncrementBy(&exitCost);
                     }
                  }
                  break;

                  default:
                  {
                     llvm_unreachable("No decision!");
                  }
               };
            }
         }
      }

      Tiled::Cost allocationCost = liveRange->AllocationCost;
      Tiled::Cost spillCost = liveRange->SpillCost;
      Tiled::Cost recalculateCost = liveRange->RecalculateCost;
#ifdef ARCH_WITH_FOLDS
      Tiled::Cost foldCost = liveRange->FoldCost;
#endif

      allocationCost.IncrementBy(&allocationTransferCost);
      spillCost.IncrementBy(&spillTransferCost);
      recalculateCost.IncrementBy(&recalculateTransferCost);
#ifdef ARCH_WITH_FOLDS
      foldCost.IncrementBy(&recalculateTransferCost);
#endif
      liveRange->AllocationCost = allocationCost;
      liveRange->SpillCost = spillCost;
      liveRange->RecalculateCost = recalculateCost;
#ifdef ARCH_WITH_FOLDS
      liveRange->FoldCost = foldCost;
#endif
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get an ordered live range vector ready for simplification processing.
//
// Remarks:
//
//    Currently, the tile gives us a default ordering which is just reverse flow order.
//    Because simplification processing is done with a stack, this causes live ranges to be simplified
//    in flow order.
//
// Arguments:
//
//    tile - tile containing live ranges to order
//
// Returns:
//
//     liveRange vector ordered for simplification processing.
//
//------------------------------------------------------------------------------

GraphColor::LiveRangeVector *
Allocator::GetLiveRangeOrder
(
   GraphColor::Tile * tile
)
{
   GraphColor::LiveRangeVector * orderedLiveRangeVector = tile->GetLiveRangeOrder();

   return orderedLiveRangeVector;
}

unsigned
Allocator::SimplifyColorable
(
   GraphColor::Tile * tile
)
{
   unsigned                      colorable = 0;
   unsigned                      lastCount;
   GraphColor::LiveRangeVector * liveRangeVector = this->GetLiveRangeOrder(tile);

   if (!tile->IsRoot()) {
      GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

      // foreach_liverange_in_order
      for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
      {
         GraphColor::LiveRange * liveRange = *lr;

         if (liveRange->IsSimplified || liveRange->IsAssigned() || liveRange->IsSpilled()) {
            continue;
         }

         if (liveRange->IsCalleeSaveValue) {
            this->Simplify(tile, liveRange);

            this->SimplificationStack->push_back(liveRange->Id);
         }
      }
   }

   // Iteratively remove degree < k nodes from the graph until there
   // are no more to remove or we remove all live ranges.

   do
   {
      lastCount = colorable;
      GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

      // foreach_liverange_in_order
      for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
      {
         GraphColor::LiveRange * liveRange = *lr;

         if (liveRange->IsSimplified || liveRange->IsAssigned()) {
            continue;
         }

         unsigned allocatableRegisterCount = this->NumberAllocatableRegisters(liveRange);

         if (liveRange->Degree < allocatableRegisterCount) {
            this->Simplify(tile, liveRange);

            this->SimplificationStack->push_back(liveRange->Id);

            colorable++;
         }
      }

   } while (colorable > lastCount);

   delete liveRangeVector;

   return colorable;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Simplify high degree nodes in a pessimistic way.
//
// Arguments:
//
//    tile - tile to simplify
//
// Notes:
//    1) push optimistically of all spilled or trivial nodes (these must get colors)
//    2) spill all other high degree nodes - force convergence
//
//------------------------------------------------------------------------------

unsigned
Allocator::SimplifyPessimistic
(
   GraphColor::Tile * tile
)
{
   unsigned                       spilled = 0;
   GraphColor::LiveRangeVector *  liveRangeVector = this->GetLiveRangeOrder(tile);
   llvm::SparseBitVector<> *      pendingSpillAliasTagBitVector = this->PendingSpillAliasTagBitVector;
   llvm::SparseBitVector<> *      doNotSpillAliasTagBitVector = this->DoNotSpillAliasTagBitVector;

   // Colorable have already been pushed.

   // Push all remaining trivial or prior spilled appearances to the head of the simplification stack.
   GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

   // foreach_liverange_in_order
   for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
   {
      GraphColor::LiveRange * liveRange = *lr;
      if (liveRange->IsSpilled() || liveRange->IsSimplified || liveRange->IsAssigned()) {
         continue;
      }

      if (doNotSpillAliasTagBitVector->test(liveRange->VrTag)
         || liveRange->IsTrivial || liveRange->IsSummaryProxy) {
         // Summary proxies should always be allocatable, even if they look high degree because of bytable,
         // etc. but we don't force them to be infinite and change their allocation order.  So we force
         // simplification because we're will get a register. 
         assert(liveRange->SpillCost.IsInfinity() || liveRange->IsSummaryProxy);

         this->Simplify(tile, liveRange);

         this->SimplificationStack->push_back(liveRange->Id);
      }
   }

   // Everything left must be spilled directly.
   lr = liveRangeVector->begin();

   // foreach_liverange_in_order
   for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
   {
      GraphColor::LiveRange * liveRange = *lr;
      if (liveRange->IsSpilled() || liveRange->IsSimplified) {
         continue;
      }

      // force live range spill in conservative mode of everything left

      pendingSpillAliasTagBitVector->set(liveRange->VrTag);

      this->Simplify(tile, liveRange);

      // account for spill but don't push on simplification stack
      spilled++;
   }

   delete liveRangeVector;

   return spilled;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Simplify high degree nodes in an optimistic way.
//
// Arguments:
//
//    tile - tile to simplify
//
// Notes:
//
//    Push all nodes in order of live range cost / degree
//
//------------------------------------------------------------------------------

unsigned
Allocator::SimplifyOptimistic
(
   GraphColor::Tile * tile
)
{
   GraphColor::LiveRangeVector *    liveRangeVector = this->GetLiveRangeOrder(tile);
   GraphColor::AllocatorCostModel * costModel = tile->CostModel;
   (costModel);

   Tiled::Cost             lowCost = this->InfiniteCost;
   Tiled::Cost             bestCost;
   Tiled::Cost             weightCost;
   Tiled::Cost             scaledCost;
   Tiled::Cost             lowScaledCost;
   Tiled::Cost             lowWeightCost = this->InfiniteCost;
   unsigned                lowDenominator = 1;
   unsigned                denominator;
   GraphColor::LiveRange * lowLiveRange = nullptr;
   bool                    unsimplified;
   unsigned                simplified = 0;
   Tiled::Cost             zeroCost = this->ZeroCost;
   Tiled::Cost             localWeightCost;
   int                     category;
   int                     lowCategory = 0;

   // Push all high degree callee save value live ranges in reverse order of discovery.
   // This avoids turning the handling out of callee save registers conservative.

   if (!tile->IsRoot() || (tile->Pass == GraphColor::Pass::Assign)) {
      GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

      // foreach_liverange_in_order
      for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
      {
         GraphColor::LiveRange * liveRange = *lr;

         if (liveRange->IsSimplified || liveRange->IsAssigned() || liveRange->IsSpilled()) {
            continue;
         }

         if (liveRange->IsCalleeSaveValue) {
            this->Simplify(tile, liveRange);

            this->SimplificationStack->push_back(liveRange->Id);

            // count simplified live ranges
            simplified++;
         }
      }
   }

   if ( true ) {
      //The section of code in the else branch below is currently not active, see
      // the comment under the else-statement.

      // Iteratively remove lowest spill cost, degree > k nodes from the graph until we have none left
      // in the tile graph.
      do
      {
         unsimplified = false;

         // Look for unsimplified live ranges in the graph selecting the lowest cost/degree node.
         GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

         // foreach_liverange_in_order
         for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
         {
            GraphColor::LiveRange * liveRange = *lr;

            if (liveRange->IsSimplified || liveRange->IsAssigned() || liveRange->IsSpilled()) {
               continue;
            } else {
               // Found an unsimplified live range - mark for another iteration
               unsimplified = true;
            }

            bestCost = liveRange->GetBestSpillCost();
            localWeightCost = liveRange->WeightCost;

            if (liveRange->IsGlobal()) {
               weightCost = liveRange->GlobalLiveRange->WeightCost;
            } else {
               weightCost = localWeightCost;
            }

            assert(!liveRange->IsSimplified);

            denominator = liveRange->Degree
#ifdef ARCH_WITH_FOLDS
                        + liveRange->SpillBenefit()
#endif
               ;

            category = liveRange->IsSecondChanceGlobal;

            // Cross multiply costs - implements (lowCost/denominator) > (bestCost/denominator) but with a
            // cheaper multiply.

            scaledCost = bestCost;
            scaledCost.ScaleBy(lowDenominator);

            lowScaledCost = lowCost;
            lowScaledCost.ScaleBy(denominator);

            int categoryValue = (lowCategory - category);
            int compareValue = (costModel->Compare(&scaledCost, &lowScaledCost));
            int weightValue = (costModel->Compare(&weightCost, &lowWeightCost));

            if ((lowLiveRange != nullptr) && (categoryValue > 0)) {
               continue;
            }

            if ((categoryValue < 0) || (compareValue < 0) || ((compareValue == 0) && (weightValue <= 0))) {
               lowLiveRange = liveRange;
               lowCost = bestCost;
               lowWeightCost = weightCost;
               lowDenominator = denominator;
               lowCategory = category;
            }
         }

         if (unsimplified) {
            assert(lowLiveRange != nullptr);

            this->Simplify(tile, lowLiveRange);

            this->SimplificationStack->push_back(lowLiveRange->Id);

            // count simplified live ranges
            simplified++;

            // set up for next iteration
            lowLiveRange = nullptr;
            lowCost = this->InfiniteCost;
            lowWeightCost = this->InfiniteCost;
            lowDenominator = 1;
            lowCategory = 0;
         }

      } while (unsimplified);

   }
   else {
      assert(0 && "This code is for user-defined formulas/heuristics to compute LR weigts based on known parameters computed in prior program analysis.");
#ifdef FUTURE_IMPL
    //if (Allocator::SimplificationExpressionControl->WasExplicitlySet) {

      Tiled::Cost      infiniteCost = this->InfiniteCost;
      Tiled::CostValue lowCostValue = infiniteCost.GetExecutionCycles();
      Tiled::CostValue scaledCostValue;

      GraphColor::CostValueVector *   costValueVector = this->CostValueVector;
      GraphColor::BooleanVector *     booleanVector = this->BooleanVector;
      //Tiled::StringExpressionEvaluator ^ evaluator = this->ExpressionEvaluator;
      //PhxString                        simplificationString = this->SimplificationExpressionString;

      // Initialize expression evaluator to use local vectors for input values.

      assert(costValueVector != nullptr);
      assert(booleanVector != nullptr);
      //Assert(evaluator->CostValueParameters == costValueVector);
      //Assert(evaluator->BooleanParameters == booleanVector);

      do
      {
         unsimplified = false;

         // Push any second chance globals first by global weight.
         GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

         // foreach_liverange_in_order
         for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
         {
            GraphColor::LiveRange * liveRange = *lr;

            if (liveRange->IsSimplified ||
                liveRange->IsAssigned() || liveRange->IsSpilled() || !liveRange->IsSecondChanceGlobal) {
               continue;
            } else {
               // Found an unsimplified live range - mark for another iteration
               unsimplified = true;
            }

            bestCost = liveRange->GetBestSpillCost();
            localWeightCost = liveRange->WeightCost;

            if (liveRange->IsGlobal()) {
               weightCost = liveRange->GlobalLiveRange->WeightCost;
            } else {
               weightCost = localWeightCost;
            }

            assert(!liveRange->IsSimplified);

            denominator = liveRange->Degree
#ifdef ARCH_WITH_FOLDS
               + liveRange->SpillBenefit()
#endif
               ;

            // Cross multiply costs - implements (lowCost/denominator) > (bestCost/denominator) but with a
            // cheaper multiply.

            scaledCost = bestCost;
            scaledCost.ScaleBy(lowDenominator);

            lowScaledCost = lowCost;
            lowScaledCost.ScaleBy(denominator);

            int compareValue = (costModel->Compare(&scaledCost, &lowScaledCost));
            int weightValue = (costModel->Compare(&weightCost, &lowWeightCost));

            if ((compareValue < 0) || ((compareValue == 0) && (weightValue <= 0))) {
               lowLiveRange = liveRange;
               lowCost = bestCost;
               lowWeightCost = weightCost;
               lowDenominator = denominator;
            }
         }

         if (unsimplified) {
            assert(lowLiveRange != nullptr);

            this->Simplify(tile, lowLiveRange);

            this->SimplificationStack->push_back(lowLiveRange->Id);

            // count simplified live ranges
            simplified++;

            // set up for next iteration
            lowLiveRange = nullptr;
            lowCost = this->InfiniteCost;
            lowWeightCost = this->InfiniteCost;
            lowDenominator = 1;
         }

      } while (unsimplified);

      // Push any spillable live ranges by expression priority function.

      do
      {
         unsimplified = false;

         // Look for unsimplified live ranges in the graph selecting the lowest priority node.
         GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

         // foreach_liverange_in_order
         for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
         {
            GraphColor::LiveRange * liveRange = *lr;

            if (liveRange->IsSimplified || liveRange->IsAssigned()
               || liveRange->IsSpilled() || !liveRange->CanBeSpilled()) {
               continue;
            } else {
               // Found an unsimplified live range - mark for another iteration
               unsimplified = true;
            }

            // Build vector of facts
            // 
            // Cost values:
            // F0 - memory cost execution cycles
            // F1 - memory cost code bytes
            // F2 - recalculate execution cycles
            // F3 - recalculate code bytes
            // F4 - fold execution cycles
            // F5 - fold code bytes
            // F6 - global weight execution cycles
            // F7 - global weight code bytes
            // F8 - local weight execution cycles
            // F9 - local weight code bytes
            // F10 - degree
            // F11 - conflict graph edge count
            // F12 - preference graph edge count
            // F13 - area (+1 for each block the live range is live in or out of)
            // F14 - pressure (sum of frequency weighted live range point pressure)
            // F15 - def count
            // F16 - use count
            // F17 - current live range count (count in order of appearance in function)
            // F18 - current iteration
            // F19 - tile entry profile weight
            // F20 - total tile count
            //
            // Boolean values:
            // B0 - is global
            // B1 - is decision memory
            // B2 - is decision recalculate
            // B3 - is decision fold        (NYI)
            // B4 - is second chance global
            // B5 - is call spanning
            // B6 - has byte reference
            // B7 - has physical register conflict
            // B8 - has definition
            // B9 - has use
            // B10 - is float               (NYI)

            // Initialize vectors by remove any prior state.
            costValueVector->clear();
            booleanVector->clear();

            Tiled::Cost bestSpillCost = liveRange->GetBestSpillCost();

            // Memory cost
            Tiled::Cost spillCost = liveRange->SpillCost;
            costValueVector->push_back(spillCost.GetExecutionCycles());
            costValueVector->push_back(spillCost.GetCodeBytes());

            // Recalculate cost
            Tiled::Cost recalculateCost = liveRange->RecalculateCost;
            costValueVector->push_back(recalculateCost.GetExecutionCycles());
            costValueVector->push_back(recalculateCost.GetCodeBytes());

            // Fold cost
#ifdef ARCH_WITH_FOLDS
            Tiled::Cost foldCost = liveRange->FoldCost;
            costValueVector->push_back(foldCost.GetExecutionCycles());
            costValueVector->push_back(foldCost.GetCodeBytes());*/
#endif

            // Global weight
            Tiled::Cost globalWeight = zeroCost;
            if (liveRange->IsGlobal()) {
               globalWeight = liveRange->GlobalLiveRange->WeightCost;
            }

            booleanVector->push_back(liveRange->IsGlobal());

            costValueVector->push_back(globalWeight.GetExecutionCycles());
            costValueVector->push_back(globalWeight.GetCodeBytes());

            // Local weight
            Tiled::Cost localWeight = liveRange->WeightCost;
            costValueVector->push_back(localWeight.GetExecutionCycles());
            costValueVector->push_back(localWeight.GetCodeBytes());

            // Conflict degree
            unsigned degree = liveRange->Degree;
            costValueVector->push_back(degree);

            unsigned conflictEdgeCount = liveRange->ConflictEdgeCount;
            costValueVector->push_back(conflictEdgeCount);

            unsigned preferenceEdgeCount = liveRange->PreferenceEdgeCount;
            costValueVector->push_back(preferenceEdgeCount);

            costValueVector->push_back(liveRange->Area);
            costValueVector->push_back(liveRange->Pressure);
            costValueVector->push_back(liveRange->DefinitionCount);
            costValueVector->push_back(liveRange->UseCount);

            // live range id is current count based on order in the IR
            costValueVector->push_back(liveRange->Id);

            costValueVector->push_back(tile->Iteration);

            // Profile frequency for head label of the tile. Used to as part of heuristic to schedule tiles
            // for allocationt. Very rough gage of how important.
            costValueVector->push_back(tile->Frequency);
            costValueVector->push_back(tile->TileGraph->TileCount);

            // "Best" spill decision 
            GraphColor::Decision decision = liveRange->Decision();

            booleanVector->push_back(decision == GraphColor::Decision::Memory);
            booleanVector->push_back(decision == GraphColor::Decision::Recalculate);
            booleanVector->push_back(false /*decision == GraphColor::Decision::Fold*/);

            // Live range special cases
            booleanVector->push_back(liveRange->IsSecondChanceGlobal);
            booleanVector->push_back(liveRange->IsCallSpanning);
            booleanVector->push_back(false /*liveRange->HasByteReference*/);
            booleanVector->push_back(liveRange->HasPhysicalRegisterConflict());
            booleanVector->push_back(liveRange->HasDefinition());
            booleanVector->push_back(liveRange->HasUse());

            // Live range candidate category
            booleanVector->push_back(false /*liveRange->Type->IsFloat*/);

            // Evaluate a cost

            //TODO: scaledCostValue = evaluator->EvaluateCostValue(simplificationString);

            bool setLow = false;

            if ((lowLiveRange == nullptr) || Tiled::CostValue::CompareLT(scaledCostValue, lowCostValue))  {
               setLow = true;
            } else if (Tiled::CostValue::CompareEQ(scaledCostValue, lowCostValue)) {
               Tiled::CostValue codeBytes = bestSpillCost.GetCodeBytes();
               Tiled::CostValue lowCodeBytes = lowScaledCost.GetCodeBytes();
               unsigned       degree = liveRange->Degree
#ifdef ARCH_WITH_FOLDS
                  + liveRange->SpillBenefit()
#endif
                  ;
               unsigned       lowDegree = lowLiveRange->Degree
#ifdef ARCH_WITH_FOLDS
                  + lowLiveRange->SpillBenefit()
#endif
                  ;

               Tiled::CostValue adjustedCodeBytes = Tiled::CostValue::Multiply(codeBytes, lowDegree);
               Tiled::CostValue adjustedLowCodeBytes = Tiled::CostValue::Multiply(lowCodeBytes, degree);

               // break ties

               if (Tiled::CostValue::CompareEQ(adjustedCodeBytes, adjustedLowCodeBytes)) {
                  // Break tie of code bytes by weight

                  if (liveRange->IsGlobal()) {
                     weightCost = liveRange->GlobalLiveRange->WeightCost;
                  } else {
                     weightCost = liveRange->WeightCost;
                  }

                  if (lowLiveRange->IsGlobal()) {
                     lowWeightCost = lowLiveRange->GlobalLiveRange->WeightCost;
                  } else {
                     lowWeightCost = lowLiveRange->WeightCost;
                  }

                  if (costModel->Compare(&weightCost, &lowWeightCost) <= 0) {
                     setLow = true;
                  }
               }

               // Break tie with bytes
               else if (Tiled::CostValue::CompareLT(adjustedCodeBytes, adjustedLowCodeBytes)) {
                  setLow = true;
               }
            }

            if (setLow) {
               lowLiveRange = liveRange;
               lowCostValue = scaledCostValue;
               lowScaledCost = bestSpillCost;
            }
         }

         if (unsimplified) {
            assert(lowLiveRange != nullptr);

            this->Simplify(tile, lowLiveRange);

            this->SimplificationStack->push_back(lowLiveRange->Id);

            // count simplified live ranges
            simplified++;

            // set up for next iteration
            lowLiveRange = nullptr;

            // Needs to be positive infinity - max, top, value we will
            // drive down from.

            lowCostValue = infiniteCost.GetExecutionCycles();
         }

      } while (unsimplified);

      // Initialize lowWeightCost to top value for next set of iterations.
      lowWeightCost = this->InfiniteCost;

      do
      {
         // Push any must allocate live ranges by their weight.

         unsimplified = false;
         GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

         // foreach_liverange_in_order
         for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
         {
            GraphColor::LiveRange * liveRange = *lr;

            if (liveRange->IsSimplified || liveRange->IsAssigned() || liveRange->IsSpilled()) {
               continue;
            } else {
               // Found an unsimplified live range - mark for another iteration
               unsimplified = true;
            }

            // We should only be processing live ranges that can't be
            // spilled in this loop.
            assert(!liveRange->CanBeSpilled());

            if (liveRange->IsGlobal()) {
               weightCost = liveRange->GlobalLiveRange->WeightCost;
            } else {
               weightCost = liveRange->WeightCost;
            }

            if (costModel->Compare(&weightCost, &lowWeightCost) <= 0) {
               lowWeightCost = weightCost;
               lowLiveRange = liveRange;
            }
         }

         if (unsimplified) {
            assert(lowLiveRange != nullptr);

            this->Simplify(tile, lowLiveRange);

            this->SimplificationStack->push_back(lowLiveRange->Id);

            // count simplified live ranges
            simplified++;

            // set up for next iteration
            lowLiveRange = nullptr;
            lowWeightCost = this->InfiniteCost;
         }

      } while (unsimplified);

#endif // FUTURE_IMPL
   }

   delete liveRangeVector;

   return simplified;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Simplify tile conflict graph
//
//    (stackify tile live ranges in the conflict graph biased by degree and preference)
//
// Arguments:
//
//    tile - tile to simplify
//
//------------------------------------------------------------------------------

void
Allocator::Simplify
(
   GraphColor::Tile * tile
)
{
   unsigned                       preAllocated = 0;
   unsigned                       spilled = 0;
   GraphColor::LiveRangeVector *  liveRangeVector = this->GetLiveRangeOrder(tile);

   //TIMER_START(ra_simplify, L"Allocator tile simplify", Toolbox::TimerKind::SubFunction);

   // account for pre-color live ranges
   GraphColor::LiveRangeVector::iterator lr = liveRangeVector->begin();

   // foreach_liverange_in_order
   for (++lr /*vector-base1*/; lr != liveRangeVector->end(); ++lr)
   {
      GraphColor::LiveRange * liveRange = *lr;
      if (liveRange->IsAssigned()) {
         preAllocated++;
         liveRange->IsSimplified = true;
      }
   }

   delete liveRangeVector;

   // add colorable to simplification stack
   this->SimplifyColorable(tile);

   unsigned iteration = tile->Iteration;

   if ((this->SimplificationHeuristic == GraphColor::Heuristic::Optimistic)
      && (iteration > this->ConservativeThreshold(tile))) {
      this->SimplificationHeuristic = GraphColor::Heuristic::Pessimistic;
   }

   if (this->SimplificationHeuristic == GraphColor::Heuristic::Pessimistic) {
      spilled += this->SimplifyPessimistic(tile);
   } else {
      this->SimplifyOptimistic(tile);
   }

   // make sure we have all the live ranges on the simplification stack
   assert(this->SimplificationStack->size() == (tile->LiveRangeCount() - preAllocated - spilled));

   //TIMER_END(ra_simplify, L"Allocator tile simplify", Toolbox::TimerKind::SubFunction);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Calculate degree between two live ranges, the live range being considered (for which we are calculating
//    degree) and the conflicting live range.
//
//------------------------------------------------------------------------------

unsigned
Allocator::CalculateDegree
(
   GraphColor::LiveRange * liveRange,
   GraphColor::LiveRange * conflictingLiveRange
)
{
   unsigned           degree = 1;

   // Handle trivial case.
   if (liveRange->GetRegisterCategory() == conflictingLiveRange->GetRegisterCategory()) {
      return degree;
   }

   const llvm::TargetRegisterClass *liveRangeRC = liveRange->GetRegisterCategory();
   const llvm::TargetRegisterClass *conflictingLiveRangeRC = conflictingLiveRange->GetRegisterCategory();
   // Examine register hierarchy to determine degree.
   if (this->TRI->getRegSizeInBits(*liveRangeRC) < this->TRI->getRegSizeInBits(*conflictingLiveRangeRC)) {
      const llvm::TargetRegisterClass *common = this->TRI->getCommonSubClass(liveRange->GetRegisterCategory(), conflictingLiveRange->GetRegisterCategory());
      if (common)
         llvm_unreachable("Implement to handle situation of sub-register class.");
   }

   return degree;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Mark live range as simplified
//
//------------------------------------------------------------------------------

void
Allocator::Simplify
(
   GraphColor::Tile *      tile,
   GraphColor::LiveRange * liveRange
)
{
   GraphColor::ConflictGraph * conflictGraph = tile->ConflictGraph;
   GraphColor::GraphIterator iter;

   // foreach_conflict_liverange
   for (GraphColor::LiveRange * conflictingLiveRange = conflictGraph->GetFirstConflictLiveRange(&iter, liveRange);
        (conflictingLiveRange != nullptr);
        conflictingLiveRange = conflictGraph->GetNextConflictLiveRange(&iter)
      )
   {
      if (conflictingLiveRange->IsSpilled()) {
         continue;
      }

      conflictingLiveRange->Degree -= this->CalculateDegree(liveRange, conflictingLiveRange);
   }

   liveRange->IsSimplified = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build liveness object and calculate liveness for function.
//
//------------------------------------------------------------------------------

void
Allocator::BuildLiveness()
{
   // We may build liveness twice, once before tile construction and
   // once after. If liveness was already built, then first delete the
   // stale liveness information.

   if (this->Liveness != nullptr) {
      this->DeleteLiveness();
   }

   Graphs::FlowGraph *    functionUnit = this->FunctionUnit;
   GraphColor::Liveness * liveness = GraphColor::Liveness::New(this);

   this->Liveness = liveness;
   liveness->ComputeRegisterLiveness(functionUnit);
   liveness->ComputeDefinedRegisters(functionUnit);
   liveness->PruneRegisterLiveness(functionUnit);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete liveness object and data.
//
//------------------------------------------------------------------------------

void
Allocator::DeleteLiveness()
{
   this->Liveness->Delete();
   this->Liveness = nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the tile graph for function.
//
//------------------------------------------------------------------------------

void
Allocator::BuildTileGraph()
{
   GraphColor::TileGraph * tileGraph = GraphColor::TileGraph::New(this);
   this->TileGraph = tileGraph;

   // Actually construct the tile graph (i.e. tree) by analyzing the IR.
   tileGraph->BuildGraph();

   // If we have more than a single tile check for whether the
   // function needs to be annotated for CalleeSave (set up for shrinkwrapping)
   // 
   // Also if there's more than 1 tile or the tile graph has found
   // critical edges to split then rebuild liveness to ensure we have
   // a complete snapshot of the new flow graph (tile entry/exit
   // blocks and critical edge airlocks).

   bool doAnnotateIRForCalleeSave = (tileGraph->TileCount > 1);

   if (doAnnotateIRForCalleeSave || tileGraph->HasSplitCriticalEdges) {
      if (doAnnotateIRForCalleeSave) {
         this->AnnotateIRForCalleeSave();
      }

      this->BuildLiveness();
   }

   //if uncommented, blocks execution of multi-tile code:  return;

   GraphColor::TileExtensionObject *  tileExtensionObject;
   llvm::SparseBitVector<> *          liveInBitVector;
   llvm::SparseBitVector<> *          liveOutBitVector;
   Dataflow::LivenessData *           livenessData;

   GraphColor::TileList::iterator ti;

   // foreach_tile_in_dfs_postorder
   for (ti = tileGraph->PostOrderTileList->begin(); ti != tileGraph->PostOrderTileList->end(); ++ti)
   {
      GraphColor::Tile * tile = *ti;
      Profile::Count boundaryProfileCount = Tiled::CostValue::ConvertFromUnsigned(0);

      Graphs::MachineBasicBlockList::iterator bi;

      // foreach_tile_entry_block
      for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;

         assert(entryBlock->pred_size() == 1);
         llvm::MachineBasicBlock * pred_mbb = *(entryBlock->pred_begin());
         boundaryProfileCount = Tiled::CostValue::Add(boundaryProfileCount, getProfileCount(pred_mbb));

         liveInBitVector = new llvm::SparseBitVector<>();
         livenessData = this->Liveness->GetRegisterLivenessData(entryBlock);
         *liveInBitVector = *(livenessData->LiveInBitVector);

         tileExtensionObject = TileExtensionObject::GetExtensionObject(entryBlock);
         tileExtensionObject->GlobalAliasTagSet = liveInBitVector;
      }

      // foreach_tile_exit_block
      for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * exitBlock = *bi;

         assert(exitBlock->pred_size() == 1);
         llvm::MachineBasicBlock * pred_mbb = *(exitBlock->pred_begin());
         boundaryProfileCount = Tiled::CostValue::Add(boundaryProfileCount, getProfileCount(pred_mbb));

         liveOutBitVector = new llvm::SparseBitVector<>();
         livenessData = this->Liveness->GetRegisterLivenessData(exitBlock);
         *liveOutBitVector = *(livenessData->LiveOutBitVector);

         tileExtensionObject = TileExtensionObject::GetExtensionObject(exitBlock);
         tileExtensionObject->GlobalAliasTagSet = liveOutBitVector;
      }

      tile->BoundaryProfileCount = boundaryProfileCount;

#if defined(TILED_DEBUG_SUPPORT)
      if (this->FunctionUnit->machineFunction->getName().str() == "***")
         dumpTile(tile);
#endif
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Schedule the tile graph for function.  We establish a new depth first order sorted by 
//    frequency and global weight cost.
//
//------------------------------------------------------------------------------

void
Allocator::ScheduleTileGraph()
{
   GraphColor::TileGraph * tileGraph = this->TileGraph;

   // Only one tile, no need to schedule.
   if (tileGraph->TileCount == 1) {
      return;
   }

   GraphColor::TileList    readyTileList;
   GraphColor::TileList *  preOrderTileList = tileGraph->PreOrderTileList;
   GraphColor::TileList *  postOrderTileList = tileGraph->PostOrderTileList;
   GraphColor::Tile *      tile = nullptr;
   GraphColor::Tile *      firstTile;
   Tiled::CostValue        frequency;
   Tiled::CostValue        cycles;

   // Initialize tiles and ready list.

   GraphColor::TileList * postOrderTiles = tileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      tile->DependencyCount = tile->NestedTileList->size();
      if (tile->DependencyCount == 0) {
         readyTileList.push_back(tile);
      }
   }

   // Schelude tile tree.

   preOrderTileList->clear();
   postOrderTileList->clear();
   while (!readyTileList.empty())
   {
      // Remove best tile from ready list.

      firstTile = readyTileList.front();
      readyTileList.pop_front();
      tile = firstTile;
      frequency = tile->Frequency;
      cycles = tile->GlobalWeightCost.GetExecutionCycles();

      // foreach_Tile_in_List
      for (t = readyTileList.begin(); t != readyTileList.end(); ++t)
      {
         GraphColor::Tile *readyTile = *t;

         if (Tiled::CostValue::CompareGT(readyTile->Frequency, frequency)
            || Tiled::CostValue::CompareGE(readyTile->Frequency, frequency)
            && Tiled::CostValue::CompareGT(readyTile->GlobalWeightCost.GetExecutionCycles(), cycles)) {
            tile = readyTile;
            frequency = tile->Frequency;
            cycles = tile->GlobalWeightCost.GetExecutionCycles();
         }
      }

      if (tile != firstTile) {
         readyTileList.push_front(firstTile);
         //readyTileList.Remove(tile);
         GraphColor::TileList::iterator t;
         t = std::find(readyTileList.begin(), readyTileList.end(), tile);
         if (t != readyTileList.end()) {
            readyTileList.erase(t);
         }
      }

      // Add tile to pre and post orders.

      preOrderTileList->push_front(tile);
      postOrderTileList->push_back(tile);

      // Decrement dependency count on parent tile and if zero add to ready list.

      if (tile->ParentTile != nullptr) {
         tile = tile->ParentTile;
         tile->DependencyCount--;
         if (tile->DependencyCount == 0) {
            readyTileList.push_front(tile);
         }
      }
   }

   // Last tile to be scheduled should be root.

   assert(tile == tileGraph->RootTile);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the global live ranges (live range that spans tile boundary) for function.
//
//------------------------------------------------------------------------------

void
Allocator::BuildGlobalLiveRanges()
{
   // If there is just a single tile for the entire function, then global live
   // ranges aren't really relevant.

   if (!this->IsTilingEnabled) {
      return;
   }

   GraphColor::Liveness * liveness = this->Liveness;

   liveness->BuildGlobalLiveRanges();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    If the target requires it, set up a temporary for tile spills.
//
//------------------------------------------------------------------------------

//removed Allocator::SetupTileSpillTemporary()

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the conflicts for global live ranges.
//
//------------------------------------------------------------------------------

void
Allocator::BuildGlobalConflictGraph()
{
   //TIMER_START(ra_globalconflict, L"Allocator build global conflicts", Toolbox::TimerKind::Function);

   ConflictGraph::BuildGlobalConflicts(this);

   //TIMER_END(ra_globalconflict, L"Allocator build global conflicts", Toolbox::TimerKind::Function);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the preferences for global live ranges.
//
//------------------------------------------------------------------------------

void
Allocator::BuildGlobalPreferenceGraph()
{
   //TIMER_START(ra_globalpreference, L"Allocator build global preference", Toolbox::TimerKind::Function);

   PreferenceGraph::BuildGlobalPreferences(this);

   //TIMER_END(ra_globalpreference, L"Allocator build global preference", Toolbox::TimerKind::Function);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build the global available table for global recalculations
//
//------------------------------------------------------------------------------

void
Allocator::BuildGlobalAvailable()
{
   Graphs::FlowGraph *                functionUnit = this->FunctionUnit;
   GraphColor::AvailableExpressions * globalAvailableExpressions = this->GlobalAvailableExpressions;

   //TIMER_START(ra_globalavailable, L"Allocator build global available", Toolbox::TimerKind::Function);

   globalAvailableExpressions->Initialize(this);

   globalAvailableExpressions->ComputeNeverKilled(functionUnit);

   //TIMER_END(ra_globalavailable, L"Allocator build global available", Toolbox::TimerKind::Function);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Cost global live ranges.
//
//------------------------------------------------------------------------------

void
Allocator::CostGlobalLiveRanges()
{
   GraphColor::SpillOptimizer * spillOptimizer = this->SpillOptimizer;
   Tiled::VR::Info *            aliasInfo = this->VrInfo;

   GraphColor::TileList * preOrderTiles = this->TileGraph->PreOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_preorder
   for (t = preOrderTiles->begin(); t != preOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      spillOptimizer->Initialize(this);

      Graphs::MachineBasicBlockVector * blockVector = tile->BlockVector;
      Graphs::MachineBasicBlockVector::iterator b;

      // foreach_block_in_tile_by_ebb_forward
      for (b = blockVector->begin(); b != blockVector->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         // Reset tile spill context for block boundary.
         this->BeginBlock(block, tile);

         llvm::MachineInstr * instruction = nullptr;
         llvm::MachineBasicBlock::instr_iterator(ii);

         // foreach_instr_in_block
         for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
         {
            instruction = &(*ii);

            if (Tile::IsTileBoundaryInstruction(instruction)) {
               continue;
            }

            llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(),
                                                                          instruction->defs().end());
            llvm::MachineInstr::mop_iterator destinationOperand;

            // foreach_register_destination_opnd
            for (destinationOperand = drange.begin(); destinationOperand != drange.end(); ++destinationOperand)
            {
               if (destinationOperand->isReg()) {
                  // If destination is global cost the fold/recalculate

                  unsigned aliasTag = aliasInfo->GetTag(destinationOperand->getReg());
                  GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(aliasTag);

                  if (globalLiveRange != nullptr) {
                     Tiled::Cost cost;
                     Tiled::Cost allocationCost = globalLiveRange->AllocationCost;

                     cost = spillOptimizer->ComputeDefinition(destinationOperand);
                     allocationCost.IncrementBy(&cost);
                     globalLiveRange->AllocationCost = allocationCost;

                     // Force spill and caller save costs to infinity for all globals

                     globalLiveRange->SpillCost = this->InfiniteCost;
                     globalLiveRange->CallerSaveCost = this->InfiniteCost;
                  }
               }
            }

            llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                        instruction->explicit_operands().end());
            llvm::MachineInstr::mop_iterator sourceOperand;

            // foreach_register_source_opnd
            for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
            {
               if (sourceOperand->isReg()) {
                  // If source is global cost the fold/recalculate

                  unsigned aliasTag = aliasInfo->GetTag(sourceOperand->getReg());
                  GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(aliasTag);

                  if (globalLiveRange != nullptr) {
                     Tiled::Cost cost;

                     // Force spill and caller save costs to infinity for all globals

                     globalLiveRange->SpillCost = this->InfiniteCost;
                     globalLiveRange->CallerSaveCost = this->InfiniteCost;

#ifdef ARCH_WITH_FOLDS
                     Tiled::Cost foldCost = globalLiveRange->FoldCost;
                     if (!foldCost.IsInfinity) {
                        cost = spillOptimizer->ComputeFold(sourceOperand, tile);
                        foldCost.IncrementBy(&cost);
                        globalLiveRange->FoldCost = foldCost;
                     }
#endif
                     Tiled::Cost recalculateCost = globalLiveRange->RecalculateCost;

                     if (!recalculateCost.IsInfinity()) {
                        llvm::MachineOperand * registerOperand = nullptr;

                        cost = spillOptimizer->ComputeRecalculate(&registerOperand, sourceOperand, tile);
                        recalculateCost.IncrementBy(&cost);
                        globalLiveRange->RecalculateCost = recalculateCost;
                     }
                  }
               }
            }

            // Finished processing instruction
            spillOptimizer->Finish(instruction, tile);
         }

         this->EndBlock(block, tile);
      }

      spillOptimizer->Reset();
   }

   // Compute global callee save heuristic.  May be overridden by the tile during tile costing.
   this->ComputeCalleeSaveHeuristic();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Delete the tile graph for function.
//
//------------------------------------------------------------------------------

void
Allocator::DeleteTileGraph()
{
   // Remove extension objects placed on blocks for tiles, and remove entertile and exittile
   // opcodes.

   GraphColor::TileList * postOrderTiles = this->TileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      Graphs::MachineBasicBlockList::iterator b;

      // foreach_tile_entry_block
      for (b = tile->EntryBlockList->begin(); b != tile->EntryBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         GraphColor::TileExtensionObject * tileExtensionObject = TileExtensionObject::GetExtensionObject(block);
         assert(tileExtensionObject != nullptr);
         TileExtensionObject::RemoveExtensionObject(block, tileExtensionObject);

         // Remove tile summary instructions.

         bool foundEnterTile = false;

         llvm::MachineBasicBlock::instr_iterator ii;

         // foreach_instr_in_block_editing
         for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
         {
            llvm::MachineInstr * instruction = &(*ii);

            if (Tile::IsEnterTileInstruction(instruction)) {
               foundEnterTile = true;
               instruction->removeFromParent();
               break;
            }
         }

         assert(foundEnterTile);
      }

      // foreach_tile_exit_block
      for (b = tile->ExitBlockList->begin(); b != tile->ExitBlockList->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         GraphColor::TileExtensionObject * tileExtensionObject = TileExtensionObject::GetExtensionObject(block);
         assert(tileExtensionObject != nullptr);
         TileExtensionObject::RemoveExtensionObject(block, tileExtensionObject);

         Tiled::Boolean foundExitTile = false;
 
         llvm::MachineBasicBlock::instr_iterator ii;

         // foreach_instr_in_block_editing
         for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
         {
            llvm::MachineInstr * instruction = &(*ii);

            if (Tile::IsExitTileInstruction(instruction)) {
               foundExitTile = true;
               instruction->removeFromParent();
               break;
            }
         }

         assert(foundExitTile);
      }
   }

   delete this->TileGraph;
   this->TileGraph = nullptr;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build flow graph order function.
//
// Remarks:
//
//    The register allocator uses: ReversePostOrder
//
//------------------------------------------------------------------------------

void
Allocator::BuildFlowGraphOrder()
{
   Graphs::FlowGraph *     flowGraph = this->FunctionUnit;
   Graphs::NodeFlowOrder * flowReversePostOrder;
   Graphs::NodeFlowOrder * flowExtendedBasicBlockReversePostOrder;

   // Calculate extended basic block RPO

   if ((this->FlowExtendedBasicBlockReversePostOrder == nullptr)
      || !this->FlowExtendedBasicBlockReversePostOrder->IsValid()) {
      flowExtendedBasicBlockReversePostOrder = Graphs::NodeFlowOrder::New();
      flowExtendedBasicBlockReversePostOrder->Build(flowGraph, Graphs::Order::ExtendedBasicBlockReversePostOrder);
      flowExtendedBasicBlockReversePostOrder->CanonicalizeStartAndEndPositions();

      if (this->FlowExtendedBasicBlockReversePostOrder) {
         delete this->FlowExtendedBasicBlockReversePostOrder;
      }
      this->FlowExtendedBasicBlockReversePostOrder = flowExtendedBasicBlockReversePostOrder;
   }

   // Calculate standard RPO

   if ((this->FlowReversePostOrder == nullptr)
      || !this->FlowReversePostOrder->IsValid()) {
      flowReversePostOrder = Graphs::NodeFlowOrder::New();
      flowReversePostOrder->Build(flowGraph, Graphs::Order::ReversePostOrder);
      flowReversePostOrder->CanonicalizeStartAndEndPositions();
      //TODO: do we need to remap with llvm::MachineFunction::*MBBNumbering() methods?

      if (this->FlowReversePostOrder) {
         delete this->FlowReversePostOrder;
      }
      this->FlowReversePostOrder = flowReversePostOrder;
   }

   // Build dominators
   flowGraph->BuildDominators();  // must be recomputed after inserting new blocks for tile entry/exit

#ifdef FUTURE_IMPL   //MULTITILE +
   if (this->IsShrinkWrappingEnabled) {
      // Build reverse graph depth first numbers so path to node
      // function may be called.

      flowGraph->BuildReverseGraphDepthFirstNumbers();
   }
#endif
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Build flow graph for function.
//
// Remarks:
//
//    If there are no EH regions in the function, we can avoid
//    breaking the blocks for exception edges.
//
//------------------------------------------------------------------------------

void
Allocator::BuildFlowGraph()
{
   // Machine CFG graph was constructed earlier by llvm's framework,
   // before call to Tiled RA. The Graphs::FlowGraph wrapper is constructed
   // as the first step in the RA.

   // Get rid of SSA information, if any, coming in.
   //functionUnit->DeleteSsaInfo();

   // Build flow graph order

   this->BuildFlowGraphOrder();
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Build loop graph for function.
//
//------------------------------------------------------------------------------

void
Allocator::BuildLoopGraph()
{

   // note: This stub and the comments below are just to inform in case of changes in the implementation flow.
   //       The LLVM implementation computes MachineLoopInfo prior to invoking Tiled RA. 

   // In this llvm implementation of RA the loop analysis was requested and done prior to calling Allocator::Allocate(),
   // the analysis results were stored in this->LoopInfo.
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Actually allocate a register to a live range.  Keep track of global register costs.
//
//------------------------------------------------------------------------------

void
Allocator::Allocate
(
   GraphColor::Tile *      tile,
   GraphColor::LiveRange * liveRange,
   unsigned                reg
)
{
   tile->Allocate(liveRange, reg);

   // If this is a callee save register clear the global cost since
   // somebody has "paid" the cost to allocate it.
   //if (this->Frame->RegisterIsCallee(reg)) {

   // <place for code supporting architectures with sub-registers>

   //}
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Spill pending for live range, add to bit vector of live range alias tags to spill.
//
// Arguments:
//
//    tile      - tile to spill
//    liveRange - live range to spill
//
//------------------------------------------------------------------------------

void
Allocator::SpillPending
(
   GraphColor::Tile *        tile,
   GraphColor::LiveRange *   liveRange,
   llvm::SparseBitVector<> * pendingSpillAliasTagBitVector
)
{
   unsigned aliasTag = liveRange->VrTag;

   pendingSpillAliasTagBitVector->set(aliasTag);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Select pseudo registers for tile live ranges
//
//    (select colors for live ranges in the simplify stack)
//
// Arguments:
//
//    tile - tile to select registers for
//
//------------------------------------------------------------------------------

bool
Allocator::Select
(
   GraphColor::Tile * tile
)
{
   llvm::SparseBitVector<> * pendingSpillAliasTagBitVector = this->PendingSpillAliasTagBitVector;
   llvm::SparseBitVector<> * allocatableRegisterTagBitVector;
   unsigned                  reg = Tiled::VR::Constants::InvalidReg;
   GraphColor::LiveRange *   liveRange = nullptr;
   bool                      isColored;

   //TIMER_START(ra_select, L"Allocator tile select", Toolbox::TimerKind::SubFunction);

   //EH: this->SpillWriteThrough(tile);

   // Early out of we've gone conservative on allocation - don't bother
   // selecting if we're already signed up to do spilling work.

   if (!pendingSpillAliasTagBitVector->empty()) {
      // Clear anything on the simplification stack.
      this->SimplificationStack->clear();
      isColored = false;

      return isColored;
   }

   while (!this->SimplificationStack->empty())
   {
      liveRange = tile->GetLiveRangeById(this->SimplificationStack->back());
      this->SimplificationStack->pop_back();
      assert(!liveRange->IsAssigned());

      allocatableRegisterTagBitVector = this->GetAllocatableRegisters(tile, liveRange);

      if (!allocatableRegisterTagBitVector->empty()) {
         reg = this->ComputePreference(tile, liveRange, allocatableRegisterTagBitVector);

         if (!(reg == VR::Constants::InvalidReg || reg == VR::Constants::InitialPseudoReg)) {
            this->Allocate(tile, liveRange, reg);
         } else {
            this->SpillPending(tile, liveRange, pendingSpillAliasTagBitVector);
         }
      } else {
         this->SpillPending(tile, liveRange, pendingSpillAliasTagBitVector);
      }
   }

   isColored = pendingSpillAliasTagBitVector->empty();

   //TIMER_END(ra_select, L"Allocator tile select", Toolbox::TimerKind::SubFunction);

   return isColored;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Spill/recalculate uncolored tile live ranges by inserting new IR
//
// Arguments:
//
//    tile - tile to spill
//
//------------------------------------------------------------------------------

bool
Allocator::Spill
(
   GraphColor::Tile * tile
)
{
   return this->Spill(tile, this->PendingSpillAliasTagBitVector);
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Spill/recalculate uncolored tile live ranges by inserting new IR
//
// Arguments:
//
//    tile - tile to spill
//
//------------------------------------------------------------------------------

bool
Allocator::Spill
(
   GraphColor::Tile *        tile,
   llvm::SparseBitVector<> * spillAliasTagBitVector
)
{
   Tiled::Cost               cost;
   Tiled::Cost               totalSpillCost = this->ZeroCost;
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   llvm::SparseBitVector<> * doNotSpillAliasTagBitVector = this->DoNotSpillAliasTagBitVector;
   llvm::SparseBitVector<> * scratchBitVector = this->ScratchBitVector1;
   llvm::SparseBitVector<> * scratchCallerSaveBitVector = this->ScratchBitVector2;
   llvm::SparseBitVector<> * calleeSaveSpillBitVector = this->ScratchBitVector2;
   bool                      hasLocalSpill = false;
   const bool                doInsert = true;

   //TIMER_START(ra_tilespill, L"Tile spill", Toolbox::TimerKind::Function);

   *spillAliasTagBitVector = *spillAliasTagBitVector - *doNotSpillAliasTagBitVector;
   assert(!spillAliasTagBitVector->empty());

   calleeSaveSpillBitVector->clear();

   llvm::SparseBitVector<>::iterator a;

   if (tile->IsRoot() && (tile->Iteration == 1)) {

      // foreach_sparse_bv_bit
      for (a = spillAliasTagBitVector->begin(); a != spillAliasTagBitVector->end(); ++a)
      {
         unsigned spillAliasTag = *a;

         // Walk through each spilling live range and note the decisions on segments of global live ranges.

         GraphColor::LiveRange * liveRange = tile->GetLiveRange(spillAliasTag);

         if (liveRange->IsGlobal() && liveRange->IsCalleeSaveValue) {
            GraphColor::Decision decision = liveRange->Decision();
            int                  globalAliasTag = liveRange->GlobalAliasTag();

            tile->SetGlobalDecision(globalAliasTag, decision);

            // Callee saves in tile 1 have appearances and needs the spill pass.
            hasLocalSpill = true;

            calleeSaveSpillBitVector->set(globalAliasTag);
         }
      }
   }

   // If this is pass one, and we have callee save values to spill, confine spilling to only those variables.

   if (!calleeSaveSpillBitVector->empty()) {
      *spillAliasTagBitVector &= *calleeSaveSpillBitVector;

   } else {

      // foreach_sparse_bv_bit
      for (a = spillAliasTagBitVector->begin(); a != spillAliasTagBitVector->end(); ++a)
      {
         unsigned spillAliasTag = *a;

         // Walk through each spilling live range and note the decisions on segments of global live ranges.
         GraphColor::LiveRange * liveRange = tile->GetLiveRange(spillAliasTag);

         if (liveRange->IsGlobal()) {
            GraphColor::Decision decision = liveRange->Decision();
            int                  globalAliasTag = liveRange->GlobalAliasTag();

            tile->SetGlobalDecision(globalAliasTag, decision);

            if (liveRange->HasReference() || !liveRange->IsCalleeSaveValue) {
               hasLocalSpill = true;
            } else {
               liveRange->MarkSpilled();
            }

         } else {
            hasLocalSpill = true;
         }
      }

   }

   if (hasLocalSpill) {

      // Initialize the spiller for this tile
      GraphColor::SpillOptimizer * spillOptimizer = this->SpillOptimizer;

      // Ensure we're still on the same tile that was initialized in costing.
      assert(spillOptimizer->Tile == tile);

      // Remove any caller save spills before main spill pass.
      *scratchCallerSaveBitVector = *spillAliasTagBitVector;

      // foreach_sparse_bv_bit
      for (a = scratchCallerSaveBitVector->begin(); a != scratchCallerSaveBitVector->end(); ++a)
      {
         unsigned spillAliasTag = *a;

         GraphColor::LiveRange * liveRange = tile->GetLiveRange(spillAliasTag);

         if (liveRange->Decision() == GraphColor::Decision::CallerSave) {
            Tiled::Cost cost = spillOptimizer->ComputeCallerSave(liveRange, tile, doInsert);

            // If the user is using the count limiter we can return infinite (no transform) and the live range
            // will have been changed to mem.
            if (!cost.IsInfinity()) {
               spillAliasTagBitVector->reset(spillAliasTag);

               totalSpillCost.IncrementBy(&cost);
            }
         }
      }

      // Reset instruction count for spill walk.
      spillOptimizer->InstructionCount = 1;

      Graphs::MachineBasicBlockVector::iterator b;

      // foreach_block_in_tile_by_ebb_forward
      for (b = tile->BlockVector->begin(); b != tile->BlockVector->end(); ++b)
      {
         llvm::MachineBasicBlock * block = *b;

         this->BeginBlock(block, tile, doInsert);

         llvm::MachineBasicBlock::instr_iterator ii, next_ii;

         // foreach_instr_in_block_editing
         for (ii = block->instr_begin(); ii != block->instr_end(); ii = next_ii)
         {
            llvm::MachineInstr * instruction = &(*ii);
            next_ii = ii; ++next_ii;

            if (Tile::IsTileBoundaryInstruction(instruction)) {
               spillOptimizer->TileBoundary(instruction, spillAliasTagBitVector, doInsert);
               continue;
            }

            scratchBitVector->clear();

            // foreach_register_source_and_destination_opnd_editing
            foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
               int spillAliasTag = vrInfo->GetTag(operand->getReg());

               if (scratchBitVector->test(spillAliasTag)) {
                  // early out if we've already seen this tag in this instruction enforces that we only make
                  // one spill call per unique tag (to preserve opeq semantics)
                  next_source_and_destination_opnd_v2(operand, instruction, end_iter);
                  continue;
               }

               // test if the operand has been spilled in select.

               if (vrInfo->CommonMayPartialTags(spillAliasTag, spillAliasTagBitVector)) {
                  bool isUse = operand->isUse();

                  assert(!tile->IsAssigned(operand));

                  vrInfo->OrMayPartialTags(spillAliasTag, scratchBitVector);

                  cost = spillOptimizer->Spill(operand, instruction, tile);
                  totalSpillCost.IncrementBy(&cost);

                  // go back to the start of the applicable list and make sure we've spilled everything we
                  // need to
                  operand = ((isUse) ? instruction->uses().begin() : instruction->defs().begin());
                  if (isUse) end_iter = instruction->explicit_operands().end();
                  continue;   //intentionally skipping  next_source_and_destination_opnd()
               }

               next_source_and_destination_opnd_v2(operand, instruction, end_iter);
            }

            spillOptimizer->Finish(instruction, tile, doInsert);
         }

         this->EndBlock(block, tile, doInsert);
      }

      // Finished tile - reset any remaining state (tile boundaries may not match EBBs exactly)
      spillOptimizer->Reset();
   }

   // Keep track of spills, don't build live ranges for them again.
   *(tile->SpilledAliasTagSet) |= *spillAliasTagBitVector;

   // Keep track of global live ranges spilled in this tile.
   GraphColor::Allocator *   allocator = tile->Allocator;
   llvm::SparseBitVector<> * globalSpillAliasTagSet = tile->GlobalSpillAliasTagSet;
   llvm::SparseBitVector<> * globalAliasTagSet = allocator->GlobalAliasTagSet;

   *globalSpillAliasTagSet |= *spillAliasTagBitVector;
   *globalSpillAliasTagSet &= *globalAliasTagSet;

   //TIMER_END(ra_tilespill, L"Tile spill", Toolbox::TimerKind::Function);

   // no local spills, the tile is still colored.
   return !hasLocalSpill;
}

//------------------------------------------------------------------------------
//
// Description:
//
//   Spill colored summary proxies if we don't want to propagate their allocation up.  Callee save values are
//   the primary example of this.  Spill callee save summary proxies of callee save values if their global
//   callee save is spilled in the parent.  This frees up allocation in pass one.  (currently modeled as
//   conflict edges which can over constrain the problem)
//
// Arguments:
//
//    tile                     - Tile to spill in
//    boundarySpillAliasTagSet - Set of summary proxy tags to spill at boundaries.
//
//------------------------------------------------------------------------------

void
Allocator::SpillAtBoundary
(
   GraphColor::Tile *        tile,
   llvm::SparseBitVector<> * boundarySpillAliasTagSet
)
{
   Tiled::VR::Info * vrInfo = this->VrInfo;

   Graphs::MachineBasicBlockVector::iterator tb;

   // foreach_block_in_tile
   for (tb = tile->BlockVector->begin(); tb != tile->BlockVector->end(); ++tb)
   {
      llvm::MachineBasicBlock * block = *tb;

      if (tile->IsNestedEntryBlock(block) || tile->IsNestedExitBlock(block)) {

         llvm::MachineBasicBlock::instr_iterator i;

         // foreach_instr_in_block
         for (i = block->instr_begin(); i != block->instr_end(); ++i)
         {
            llvm::MachineInstr * instruction = &(*i);
            
            if (GraphColor::Tile::IsTileBoundaryInstruction(instruction)) {
               foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
                  unsigned aliasTag = vrInfo->GetTag(operand->getReg());

                  if (vrInfo->CommonMayPartialTags(aliasTag, boundarySpillAliasTagSet)) {
                     GraphColor::LiveRange * liveRange = tile->GetLiveRange(aliasTag);
                     assert(liveRange != nullptr && liveRange->IsSummaryProxy);

                     liveRange->MarkSpilled();

                     int operandIndex = this->SpillOptimizer->getOperandIndex(instruction, operand);
                     if (operandIndex < 0) {
                        assert(0 && "operand to be removed not found on the instruction");
                        return;
                     }
                     instruction->RemoveOperand(operandIndex);
                  }

                  next_source_and_destination_opnd_v2(operand, instruction, end_iter);
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Spill tile globals not worth allocating pass two
//
// Arguments:
//
//    tile - Tile being processed
//
// Notes:
//
//    We know that live ranges are allocatable in the second pass so spilling of globals just is a cost
//    decision.  So this pass of spilling runs before the simplify/select logic.
//
//------------------------------------------------------------------------------

void
Allocator::SpillGlobals
(
   GraphColor::Tile * tile
)
{
   GraphColor::SpillOptimizer *     spillOptimizer = this->SpillOptimizer;
   Tiled::VR::Info *                aliasInfo = this->VrInfo;
   GraphColor::Tile *               parentTile = tile->ParentTile;
   GraphColor::AllocatorCostModel * costModel = tile->CostModel;
   llvm::SparseBitVector<> *        summaryScratchBitVector = this->ScratchBitVector1;
   llvm::SparseBitVector<> *        totalSpillAliasTagBitVector = this->ScratchBitVector2;

   // Clear scratch for use.

   totalSpillAliasTagBitVector->clear();

   // Pass 2 spilling - test whether it is cheaper to insert transfers here or down the tree.

   GraphColor::LiveRangeVector * slrVector = tile->GetSummaryLiveRangeEnumerator();
   llvm::SparseBitVector<>::iterator g;

   // foreach_summaryliverange_in_tile
   for (unsigned i = 1; i < slrVector->size(); ++i)
   {
      GraphColor::LiveRange * summaryLiveRange = (*slrVector)[i];

      llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
      *summaryScratchBitVector = *summaryAliasTagSet;

      if (summaryLiveRange->IsSpilled() || summaryLiveRange->Register == VR::Constants::InitialPseudoReg) {
         //was: summaryLiveRange->Register->IsRegisterCategoryPseudo => IsNotAssignedVirtual(aliasInfo)
         *totalSpillAliasTagBitVector |= *summaryScratchBitVector;

         // No register allocated, no longer a candidate.
         summaryLiveRange->Register = VR::Constants::InvalidReg;

         // foreach_sparse_bv_bit
         for (g = summaryScratchBitVector->begin(); g != summaryScratchBitVector->end(); ++g)
         {
            unsigned aliasTag = *g;
            assert(!aliasInfo->IsPhysicalRegisterTag(aliasTag));

            spillOptimizer->SpillFromSummary(aliasTag, summaryLiveRange);
         }

         // Remove all alias tags from the summary so it can not be looked up by tag.
         assert(summaryAliasTagSet->empty());
         continue;

      } else {
         // We got an allocation, look through the globals to see if individually we want to remove any.

         // foreach_sparse_bv_bit
         for (g = summaryScratchBitVector->begin(); g != summaryScratchBitVector->end(); ++g)
         {
            unsigned aliasTag = *g;

            if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            // Only look at spilling globals
            GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(aliasTag);
            if (globalLiveRange == nullptr) {
               continue;
            }

            assert(summaryLiveRange->Register != VR::Constants::InvalidReg);

            // Push down transfers when global live range is spilled in parent, has no
            // reference in child tile, and is cheaper to spill down the tile tree.

            Tiled::Cost spillCost = summaryLiveRange->SpillCost;
            Tiled::Cost allocationCost = summaryLiveRange->AllocationCost;

            if (parentTile == nullptr || parentTile->GlobalSpillAliasTagSet->test(aliasTag)) {
               if (!tile->HasGlobalReference(globalLiveRange) && (costModel->Compare(&spillCost, &allocationCost) <= 0)) {
                  spillOptimizer->SpillFromSummary(aliasTag, summaryLiveRange);
               } else if (!tile->HasTransitiveGlobalReference(globalLiveRange)) {
                  spillOptimizer->SpillFromSummary(aliasTag, summaryLiveRange);
               }
            }
         }
      }
   }

   // Clean up entry and exits for the total spills in this tile.

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      //llvm::MachineInstr * instruction;
      Graphs::MachineBasicBlockList::iterator bi;

      // foreach_tile_entry_block
      for (bi = nestedTile->EntryBlockList->begin(); bi != nestedTile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;

         // Remove total spills from destination side of enter tile instructions.

         llvm::MachineInstr * entryInstruction =
            nestedTile->FindNextInstruction(entryBlock, entryBlock->instr_begin(), llvm::TargetOpcode::ENTERTILE);
         assert(entryInstruction != nullptr);

         llvm::MachineOperand * dstOperand;
         unsigned dstIndex = 0;

         // foreach_destination_opnd_editing
         while (dstIndex < entryInstruction->getNumOperands() &&
                (dstOperand = &(entryInstruction->getOperand(dstIndex)))->isReg() && dstOperand->isDef())
         {
            unsigned aliasTag = aliasInfo->GetTag(dstOperand->getReg());

            if (aliasInfo->CommonMayPartialTags(aliasTag, totalSpillAliasTagBitVector)) {
               entryInstruction->RemoveOperand(dstIndex);
               continue;
            }

            ++dstIndex;
         }
      }

      // foreach_tile_exit_block
      for (bi = nestedTile->ExitBlockList->begin(); bi != nestedTile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * exitBlock = *bi;

         // Remove total spills from source side of exit tile instructions.

         llvm::MachineInstr * exitInstruction =
            nestedTile->FindPreviousInstruction(exitBlock, exitBlock->instr_rbegin(), llvm::TargetOpcode::EXITTILE);
         assert(exitInstruction != nullptr);

         llvm::MachineOperand * srcOperand = (exitInstruction->uses()).begin();
         unsigned srcIndex = exitInstruction->getOperandNo(srcOperand);

         // foreach_source_opnd_editing
         while (srcIndex < exitInstruction->getNumExplicitOperands())
         {
            srcOperand = &(exitInstruction->getOperand(srcIndex));

            if (srcOperand->isReg()) {
               unsigned aliasTag = aliasInfo->GetTag(srcOperand->getReg());

               if (aliasInfo->CommonMayPartialTags(aliasTag, totalSpillAliasTagBitVector)) {
                  exitInstruction->RemoveOperand(srcIndex);
                  continue;
               }
            }

            ++srcIndex;
         }
      }
   }
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Summarize colored tile.
//
// Arguments:
//
//    tile - tile context
//
//------------------------------------------------------------------------------

void
Allocator::Summarize
(
   GraphColor::Tile * tile
)
{
   if (this->TileGraph->TileCount > 1) {
      llvm::SparseBitVector<> * rewriteAliasTagBitVector = this->ScratchBitVector1;

      // Count spills of transparent globals.  This is required to occur before construction of the summary live
      // ranges and conflict and preference summary graphs.
      tile->CountGlobalTransparentSpills();

      // Reduce allocated regs to disjoint resource sets that will be the base of the summary variables
      tile->UnionAllocated();

      // Build a summary variable for each union find root
      tile->BuildSummaryLiveRanges();

      // Map live ranges to summaries
      tile->MapLiveRangesToSummaryLiveRanges(rewriteAliasTagBitVector);

      // Propagate bottom up global appearance info
      tile->PropagateGlobalAppearances();

      // Prune a summary variables, keep only those actually summarizing live ranges.
      tile->PruneSummaryLiveRanges();

      // Rewrite operands to denote when high registers are required (only for architectures w/ register-pairs).
      //tile->RewriteConstrainedRegisters(rewriteAliasTagBitVector);

      // Build summary conflict graph.
      tile->BuildSummaryConflicts();

      // Build summary preference graph.
      tile->BuildSummaryPreferences();

      // Update enter tile and exit tile instructions.
      tile->UpdateEnterTileAndExitTileInstructions();

      // Update any global conflicts for transparent live ranges spilled
      // with out iteration.  Other call to UpdateGlobalConflicts occurs
      // in "Build" so this is needed for any last iterations where there
      // is transparent spilling.

      tile->UpdateGlobalConflicts();

   } else {
      // Set up current live ranges as summary.  This allows rewrite registers to run and marks the tile as
      // 'HasAssigedRegisters' short circuiting pass two reallocation.

      tile->ReuseLiveRangesAsSummary();

      // Redirect tile alias tag to live range map pointer to the currently valid global alias tag to live
      // range map.
      tile->AliasTagToIdVector = this->AliasTagToIdVector;

      // If there is only a single tile, then assign pass 1 allocations (since we saw
      // global scope in pass 1) and mark the tile as 'HasAssignedRegisters'.

      this->RewriteRegisters(tile);

      // Clear mapping.
      tile->AliasTagToIdVector = nullptr;
   }

   // Done.
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Insert compensation code for between a tile and it's parent.
//
// Arguments:
//
//    tile - tile context
//
//------------------------------------------------------------------------------

void
Allocator::InsertCompensationCode
(
   GraphColor::Tile * tile
)
{
   bool                          doInsert = true;
   Tiled::Cost                   tileTransferCost = this->ZeroCost;
   Tiled::Cost                   spillCost;
   Graphs::FlowGraph *           flowGraph = this->FunctionUnit;
   GraphColor::SpillOptimizer *  spillOptimizer = this->SpillOptimizer;
   GraphColor::Liveness *        liveness = this->Liveness;
   Tiled::VR::Info *             aliasInfo = this->VrInfo;

   // Iterate through tile summary and for each summary variable live
   // in/out compute transfers from the parent.

   GraphColor::Tile * parentTile = tile->ParentTile;

   if (parentTile == nullptr) {
      // No compensation code for parent tile, early out.
      return;
   }

   Graphs::MachineBasicBlockList::iterator bi;
   bool exitLiveInsAdded = false;

   GraphColor::LiveRangeVector * slrVector = tile->GetSummaryLiveRangeEnumerator();

   // foreach_summaryliverange_in_tile
   for (unsigned i = 1; i < slrVector->size(); ++i)
   {
      GraphColor::LiveRange * summaryLiveRange = (*slrVector)[i];

      if (summaryLiveRange->IsPhysicalRegister) {
         continue;
      }

      llvm::SparseBitVector<> *  summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;
      Tiled::Cost                totalTransferCost;
      Tiled::Cost                entryCost;
      Tiled::Cost                exitCost;
      unsigned                   aliasTag;

      // Start at zero for each summary.

      totalTransferCost.SetZero();

      llvm::SparseBitVector<>::iterator sa;

      // foreach_tile_entry_block
      for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;

         unsigned                   maxLiveAliasTag = Tiled::VR::Constants::InvalidTag;
         //Tiled::BitSize             maxBitSize = Tiled::TypeConstants::MaxUInt;
         llvm::SparseBitVector<> *  liveInBitVector = liveness->GetGlobalAliasTagSet(entryBlock);

         // Find largest element of summary that is live in at this entry.  This is the element we will insert
         // compensation code for.

         // foreach_sparse_bv_bit
         for (sa = summaryAliasTagSet->begin(); sa != summaryAliasTagSet->end(); ++sa)
         {
            aliasTag = *sa;

            if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            if (aliasInfo->CommonMayPartialTags(aliasTag, liveInBitVector)) {
               GraphColor::LiveRange *  parentSummaryLiveRange = parentTile->GetSummaryLiveRange(aliasTag);
               // <place for code supporting architectures with sub-registers>

               if ((maxLiveAliasTag == Tiled::VR::Constants::InvalidTag) || (parentSummaryLiveRange != nullptr)) {
                  maxLiveAliasTag = aliasTag;
               }
            }
         }

         // If we've found a max tag that is live at this entry so insert compensation transfer.

         if (maxLiveAliasTag != Tiled::VR::Constants::InvalidTag) {
            entryCost = spillOptimizer->ComputeTransfer(parentTile, maxLiveAliasTag, entryBlock, doInsert);

            totalTransferCost.IncrementBy(&entryCost);
#if defined(TILED_DEBUG_DUMPS)
            llvm::dbgs() << "\nENTRY#" << entryBlock->getNumber() << "   (compensated-summary)" << std::endl;
            entryBlock->dump();
#endif
         }
      }

      // foreach_tile_exit_block
      for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * exitBlock = *bi;

         llvm::SparseBitVector<> * liveOutBitVector = liveness->GetGlobalAliasTagSet(exitBlock);

         Dataflow::LivenessData *  registerLivenessData = liveness->GetRegisterLivenessData(exitBlock);

         // foreach_sparse_bv_bit
         for (sa = summaryAliasTagSet->begin(); sa != summaryAliasTagSet->end(); ++sa)
         {
            aliasTag = *sa;

            if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
               continue;
            }

            if (aliasInfo->CommonMayPartialTags(aliasTag, liveOutBitVector)) {
               exitCost = spillOptimizer->ComputeTransfer(parentTile, aliasTag, exitBlock, doInsert);

               totalTransferCost.IncrementBy(&exitCost);
#if defined(TILED_DEBUG_DUMPS)
               llvm::dbgs() << "\nEXIT#" << exitBlock->getNumber() << "   (compensated-summary)" << std::endl;
               exitBlock->dump();
#endif
            }
         }

         if (!exitLiveInsAdded) {
            llvm::MachineInstr * lastInstruction = &(exitBlock->back());
            llvm::MachineInstr * exitTileInstruction = flowGraph->FindPreviousInstructionInBlock(llvm::TargetOpcode::EXITTILE, lastInstruction);
            assert(exitTileInstruction != nullptr);
            llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(exitTileInstruction->uses().begin(),
                                                                        exitTileInstruction->explicit_operands().end());
            llvm::MachineInstr::mop_iterator sourceOperand;

            // foreach_register_source_opnd
            for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
            {
               unsigned physReg = sourceOperand->getReg();
               assert(Tiled::VR::Info::IsPhysicalRegister(physReg));
               exitBlock->addLiveIn(physReg);
               //llvm::dbgs()  << " ** BB#" << exitBlock->getNumber() << "  addLiveIn  R" << (sourceOperand->getReg() - 1) << std::endl;
               registerLivenessData->LiveInBitVector->set(physReg);
               registerLivenessData->LiveOutBitVector->set(physReg);
            }
         }
      }
      exitLiveInsAdded = true;

      summaryLiveRange->SpillCost = totalTransferCost;

      tileTransferCost.IncrementBy(&totalTransferCost);
   }

   llvm::SparseBitVector<> *  globalSpillAliasTagSet = tile->GlobalSpillAliasTagSet;
   llvm::SparseBitVector<> *  globalSpillLiveRangeIdSet = this->GlobalSpillLiveRangeIdSetScratch;
   llvm::SparseBitVector<> *  calleeSaveSpillLiveRangeIdSet = this->CallerSaveSpillLiveRangeIdSetScratch;
   //unsigned  globalSpillLiveRangeId = 0;
   //unsigned  calleeSaveSpillLiveRangeId = 0;

   globalSpillLiveRangeIdSet->clear();
   calleeSaveSpillLiveRangeIdSet->clear();

   // Convert spilled alias tag set to live range id - this removes
   // one of the main sources of spurious test vs. release diffs.

   llvm::SparseBitVector<>::iterator st;

   // foreach_sparse_bv_bit
   for (st = globalSpillAliasTagSet->begin(); st != globalSpillAliasTagSet->end(); ++st)
   {
      unsigned  spillTag = *st;

      GraphColor::LiveRange * globalSpillLiveRange = this->GetGlobalLiveRange(spillTag);

      if (globalSpillLiveRange->IsCalleeSaveValue) {
         calleeSaveSpillLiveRangeIdSet->set(globalSpillLiveRange->Id);
      } else {
         globalSpillLiveRangeIdSet->set(globalSpillLiveRange->Id);
      }
   }

   // Walk spilled live ranges in live range id order inserting spills.

   llvm::SparseBitVector<>::iterator glr;

   // foreach_sparse_bv_bit
   for (glr = globalSpillLiveRangeIdSet->begin(); glr != globalSpillLiveRangeIdSet->end(); ++glr)
   {
      unsigned  globalSpillLiveRangeId = *glr;

      GraphColor::LiveRange * globalSpillLiveRange = this->GetGlobalLiveRangeById(globalSpillLiveRangeId);
      unsigned                globalSpillAliasTag = globalSpillLiveRange->VrTag;
      Graphs::MachineBasicBlockList::iterator bi;

      // foreach_tile_entry_block
      for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;
         llvm::SparseBitVector<> * liveInBitVector = liveness->GetGlobalAliasTagSet(entryBlock);
         GraphColor::LiveRange *   summaryLiveRange = parentTile->GetSummaryLiveRange(globalSpillAliasTag);

         if (aliasInfo->CommonMayPartialTags(globalSpillAliasTag, liveInBitVector)) {
            if (summaryLiveRange != nullptr) {
               Tiled::Cost enterLoadCost =
                  spillOptimizer->ComputeTransfer(parentTile, globalSpillAliasTag, entryBlock, doInsert);

               tileTransferCost.IncrementBy(&enterLoadCost);
            }
         } else {
            GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(globalSpillAliasTag);
            (globalLiveRange);
            assert((summaryLiveRange == nullptr) || (globalLiveRange != nullptr && !globalLiveRange->IsCalleeSaveValue));
         }
#if defined(TILED_DEBUG_DUMPS)
         llvm::dbgs() << "\nENTRY#" << entryBlock->getNumber() << "   (compensated-spill)" << std::endl;
         entryBlock->dump();
#endif
      }
   }

   // Walk spilled callee saves in live range id order inserting
   // spills.  This ensures that they are first in the block so the
   // frame tree can better optimize.

   llvm::SparseBitVector<>::iterator clr;

   // foreach_sparse_bv_bit
   for (clr = calleeSaveSpillLiveRangeIdSet->begin(); clr != calleeSaveSpillLiveRangeIdSet->end(); ++clr)
   {
      unsigned  calleeSaveSpillLiveRangeId = *clr;

      GraphColor::LiveRange * calleeSaveSpillLiveRange = this->GetGlobalLiveRangeById(calleeSaveSpillLiveRangeId);
      unsigned                calleeSaveSpillAliasTag = calleeSaveSpillLiveRange->VrTag;

      // foreach_tile_entry_block
      for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * entryBlock = *bi;

         llvm::SparseBitVector<> * liveInBitVector = liveness->GetGlobalAliasTagSet(entryBlock);
         GraphColor::LiveRange *   summaryLiveRange = parentTile->GetSummaryLiveRange(calleeSaveSpillAliasTag);

         if (aliasInfo->CommonMayPartialTags(calleeSaveSpillAliasTag, liveInBitVector)) {
            if (summaryLiveRange != nullptr) {
               Tiled::Cost enterLoadCost =
                  spillOptimizer->ComputeTransfer(parentTile, calleeSaveSpillAliasTag, entryBlock, doInsert);

               tileTransferCost.IncrementBy(&enterLoadCost);
            }
         } else {
            GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(calleeSaveSpillAliasTag);
            (globalLiveRange);
            assert((summaryLiveRange == nullptr) || (globalLiveRange != nullptr && !globalLiveRange->IsCalleeSaveValue));
         }
      }
   }

   llvm::SparseBitVector<> * globalSpillScratchAliasTagSet = this->ScratchBitVector1;

   // foreach_tile_exit_block
   for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
   {
      llvm::MachineBasicBlock * exitBlock = *bi;

      llvm::SparseBitVector<> * liveOutBitVector = liveness->GetGlobalAliasTagSet(exitBlock);

      *globalSpillScratchAliasTagSet = *globalSpillAliasTagSet;

      // foreach_sparse_bv_bit
      for (glr = globalSpillLiveRangeIdSet->begin(); glr != globalSpillLiveRangeIdSet->end(); ++glr)
      {
         unsigned  globalSpillLiveRangeId = *glr;

         GraphColor::LiveRange * globalSpillLiveRange = this->GetGlobalLiveRangeById(globalSpillLiveRangeId);
         unsigned                globalSpillAliasTag = globalSpillLiveRange->VrTag;

         // Find spilled and live out.
         // 
         // globalSpillScratchAliasTagSet tracks what global spills
         // have been covered since we're inserting the transfer for
         // the widest appearance in the parent summary (and removing
         // all overlapped)

         if (aliasInfo->CommonMayPartialTags(globalSpillAliasTag, globalSpillScratchAliasTagSet)
            && aliasInfo->CommonMayPartialTags(globalSpillAliasTag, liveOutBitVector)) {
            unsigned maxLiveAliasTag = Tiled::VR::Constants::InvalidTag;

            // Find max tag of any spilled global allocated in the parent.

            GraphColor::LiveRange * summaryLiveRange = parentTile->GetSummaryLiveRange(globalSpillAliasTag);

            if (summaryLiveRange != nullptr) {
               llvm::SparseBitVector<> * summaryAliasTagSet = summaryLiveRange->SummaryAliasTagSet;

               // Find largest element of summary that is live in at this exit.  This is the element we will
               // insert compensation code for if there is a register in the parent.  (all values are
               // equivalent so we chose to do a single reload of the largest)

               llvm::SparseBitVector<>::iterator st;

               // foreach_sparse_bv_bit
               for (st = summaryAliasTagSet->begin(); st != summaryAliasTagSet->end(); ++st)
               {
                  unsigned  aliasTag = *st;

                  if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
                     continue;
                  }

                  if (aliasInfo->CommonMayPartialTags(aliasTag, liveOutBitVector)) {
                     if (maxLiveAliasTag == Tiled::VR::Constants::InvalidTag) {
                        //|| (aliasInfo->GetField(maxLiveAliasTag)->BitSize < aliasInfo->GetField(aliasTag)->BitSize)
                        maxLiveAliasTag = aliasTag;
                     }
                  }

                  // Remove any totally overlapped tags that were covered by this alias tag this ensures we
                  // only get singleton loads.

                  aliasInfo->MinusMustTotalTags(aliasTag, globalSpillScratchAliasTagSet);
               }

               assert(maxLiveAliasTag != Tiled::VR::Constants::InvalidTag);

               Tiled::Cost exitSaveCost =
                  spillOptimizer->ComputeTransfer(parentTile, maxLiveAliasTag, exitBlock, doInsert);

               tileTransferCost.IncrementBy(&exitSaveCost);
            }
         }
      }

#if defined(TILED_DEBUG_DUMPS)
      llvm::dbgs() << "\nEXIT#" << exitBlock->getNumber() << "   (compensated)" << std::endl;
#endif
   }

   // Walk spilled callee saves in live range id order inserting
   // spills.  This ensures that they are first in the block so the
   // frame tree can better optimize.

   // foreach_sparse_bv_bit
   for (clr = calleeSaveSpillLiveRangeIdSet->begin(); clr != calleeSaveSpillLiveRangeIdSet->end(); ++clr)
   {
      unsigned  calleeSaveSpillLiveRangeId = *clr;

      GraphColor::LiveRange * calleeSaveSpillLiveRange = this->GetGlobalLiveRangeById(calleeSaveSpillLiveRangeId);
      unsigned                calleeSaveSpillAliasTag = calleeSaveSpillLiveRange->VrTag;

      // foreach_tile_exit_block
      for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
      {
         llvm::MachineBasicBlock * exitBlock = *bi;

         llvm::SparseBitVector<> * liveOutBitVector = liveness->GetGlobalAliasTagSet(exitBlock);
         GraphColor::LiveRange *   summaryLiveRange = parentTile->GetSummaryLiveRange(calleeSaveSpillAliasTag);

         if (aliasInfo->CommonMayPartialTags(calleeSaveSpillAliasTag, liveOutBitVector)) {
            if (summaryLiveRange != nullptr) {
               Tiled::Cost enterLoadCost =
                  spillOptimizer->ComputeTransfer(parentTile, calleeSaveSpillAliasTag, exitBlock, doInsert);

               tileTransferCost.IncrementBy(&enterLoadCost);
            }
         } else {
            GraphColor::LiveRange * globalLiveRange = this->GetGlobalLiveRange(calleeSaveSpillAliasTag);
            (globalLiveRange);
            assert((summaryLiveRange == nullptr) || (globalLiveRange != nullptr && !globalLiveRange->IsCalleeSaveValue));
         }
      }
   }


   Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->Liveness->RegisterLivenessWalker;

   // foreach_tile_entry_block
   for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
   {
      llvm::MachineBasicBlock * entryBlock = *bi;
      llvm::MachineBasicBlock * uniqueSuccessor = flowGraph->uniqueSuccessorBlock(entryBlock);
      assert(uniqueSuccessor != nullptr);

      Dataflow::LivenessData *  registerLivenessData = this->Liveness->GetRegisterLivenessData(entryBlock);
      unsigned physReg;

      Dataflow::LivenessData * blockLiveData =
         static_cast<Dataflow::LivenessData *>(registerLivenessWalker->GetBlockData(uniqueSuccessor));
      assert(blockLiveData);
      llvm::SparseBitVector<> * succLiveInBitVector = blockLiveData->LiveInBitVector;
      llvm::SparseBitVector<>::iterator r;
      llvm::SparseBitVector<> tempBitVector;

      // foreach_sparse_bv_bit
      for (r = succLiveInBitVector->begin(); r != succLiveInBitVector->end(); ++r)
      {
         unsigned regTag = *r;

         if (!VR::Info::IsPhysicalRegisterTag(regTag)) {
            physReg = tile->GetAllocatedRegister(regTag);
            if (physReg == VR::Constants::InvalidReg) {
               // This VR got spilled
               continue;
            }
         } else {
            physReg = aliasInfo->GetRegister(regTag);
         }
         assert(VR::Info::IsPhysicalRegister(physReg));
         tempBitVector.set(physReg);
      }

      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward
      for (rii = entryBlock->instr_rbegin(); rii != entryBlock->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);

         llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(),
                                                                       instruction->defs().end());
         llvm::MachineInstr::mop_iterator destOperand;

         // foreach_register_destination_opnd
         for (destOperand = drange.begin(); destOperand != drange.end(); ++destOperand)
         {
            physReg = destOperand->getReg();
            tempBitVector.reset(physReg);
         }
      }

      // add to live-ins of the tile entry block live-through registers if any
      // foreach_sparse_bv_bit
      for (r = tempBitVector.begin(); r != tempBitVector.end(); ++r)
      {
         physReg = *r;
         entryBlock->addLiveIn(physReg);
         registerLivenessData->LiveInBitVector->set(physReg);
         registerLivenessData->LiveOutBitVector->set(physReg);
      }
   }

   // foreach_tile_entry_block
   for (bi = tile->EntryBlockList->begin(); bi != tile->EntryBlockList->end(); ++bi)
   {
      this->RewriteRegistersInBlock(tile, *bi, aliasInfo);

      (*bi)->sortUniqueLiveIns();

      if (!(*bi)->empty() && (*bi)->canFallThrough()) {
         llvm::MachineInstr * branchInstruction = &((*bi)->instr_back());
         if (branchInstruction->isUnconditionalBranch()) {
            (*bi)->erase_instr(branchInstruction);
         }
      }
   }

   Graphs::MachineBasicBlockVector::iterator tb;
   // foreach_block_in_tile
   for (tb = tile->BlockVector->begin(); tb != tile->BlockVector->end(); ++tb)
   {
      (*tb)->sortUniqueLiveIns();
   }

   // foreach_tile_exit_block
   for (bi = tile->ExitBlockList->begin(); bi != tile->ExitBlockList->end(); ++bi)
   {
      this->RewriteRegistersInBlock(tile, *bi, aliasInfo);

      (*bi)->sortUniqueLiveIns();

      if (!(*bi)->empty() && (*bi)->canFallThrough()) {
         llvm::MachineInstr * branchInstruction = &((*bi)->instr_back());
         if (branchInstruction->isUnconditionalBranch()) {
            (*bi)->erase_instr(branchInstruction);
         }
      }
   }

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Pass to assign physical registers in a given tile. Build summary information
//    from using child tiles and insert compensation code as necessary.
//
// Arguments:
//
//    tile - tile for assignment
//
//------------------------------------------------------------------------------

void
Allocator::Assign
(
   GraphColor::Tile * tile
)
{
   if (tile->HasAssignedRegisters) {
      // This is the fast exit for any tiles that either through simplicity or other allocation methods we
      // accepted the pass 1 allocation and assigned the registers.
      return;
   }

   this->BeginPass(tile, GraphColor::Pass::Assign);
   llvm::NamedRegionTimer T("assign", "Assign", TimerGroupName,
      TimerGroupDescription, llvm::TimePassesIsEnabled);

   DEBUG(llvm::dbgs() << "==== Assigning tile " << tile << "\n");

   // Assign is only single iteration but to enable sharing of allocation routines between passes we ensure
   // that set up is the same.
   this->BeginIteration(tile, GraphColor::Pass::Assign);

   // Rebuild tile mappings for register rewrite and reallocation.
   this->Rebuild(tile);

   // Compute summary costs
   this->CostSummary(tile);

   // Do top down, second pass reallocation on tile tree.

   if (this->DoPassTwoReallocate) {

      if (this->TileGraph->TileCount > 1) {
         this->ClearAllocations(tile);
         this->SimplifyOptimistic(tile);
         this->Select(tile);
      }
   }

   DEBUG(llvm::dbgs() << "== SpillGlobals\n");
   this->SpillGlobals(tile);

   // Rewrite all tile appearances with the final physical register allocations.
   DEBUG(llvm::dbgs() << "== RewriteRegisters\n");
   this->RewriteRegisters(tile);

   // Add transfers at entries and exits from the parent tile.
   this->InsertCompensationCode(tile);

   // Push allocation decisions down

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      this->UpdateSummary(nestedTile);
   }

   this->EndIteration(tile, GraphColor::Pass::Assign);

   this->EndPass(tile, GraphColor::Pass::Assign);
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Rewrite the operand and assign physical register.
//
//------------------------------------------------------------------------------

bool
isBoundaryBlock
(
   llvm::MachineBasicBlock * block
)
{
   llvm::MachineBasicBlock::instr_iterator  i;
   for (i = block->instr_begin(); i != block->instr_end(); ++i)
   {
      if (i->getOpcode() == llvm::TargetOpcode::ENTERTILE || i->getOpcode() == llvm::TargetOpcode::EXITTILE) {
         return true;
      }
   }
   return false;
}

void
Allocator::RewriteRegister
(
   llvm::MachineOperand *  operand,
   unsigned                reg
)
{
   unsigned  pseudoReg = operand->getReg();
   assert(Tiled::VR::Info::IsVirtualRegister(pseudoReg));
   assert(Tiled::VR::Info::IsPhysicalRegister(reg));

   // Operand appearance field offsets must match the
   // allocated reg.  This implies that for a zero offset
   // appearance of t22.i32 the allocated register on the
   // summary is a 32 bit (high or low) and no further
   // offset computation is needed to rewrite the
   // register. E.g. it would be an error to have the 64
   // bit base and require the appearance to offset to the
   // high portion.

   // <place for code supporting architectures with sub-registers>

   this->VRM->assignVirt2Phys(pseudoReg, reg);
   // Actually rewrite/assign the register.
   //operand->setReg(reg);
   //note: setReg() updates the associated information in MachineRegisterInfo
}

void
Allocator::RewriteRegistersInBlock
(
   GraphColor::Tile *        tile,
   llvm::MachineBasicBlock * block,
   VR::Info *                vrInfo
)
{
   Dataflow::LivenessData *  registerLivenessData = this->Liveness->GetRegisterLivenessData(block);
   std::set<unsigned> inBlockVRegs;

   llvm::MachineInstr * instruction = nullptr;
   llvm::MachineBasicBlock::instr_iterator(ii);

   // foreach_instr_in_block
   for (ii = block->instr_begin(); ii != block->instr_end(); ++ii)
   {
      instruction = &*ii;

#if defined(TILED_DEBUG_DUMPS)
      instruction->dump();
      llvm::dbgs() << "    ";
#endif
      bool isDBG_VALUE = (instruction->getOpcode() == llvm::TargetOpcode::DBG_VALUE);

      // foreach_register_source_and_destination_opnd
      foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)
         unsigned pseudoReg = operand->getReg();

         if (VR::Info::IsVirtualRegister(pseudoReg)) {
            unsigned aliasTag = vrInfo->GetTag(pseudoReg);
            unsigned reg = tile->GetAllocatedRegister(aliasTag);

#if defined(TILED_DEBUG_DUMPS)
            if (!VR::Info::IsPhysicalRegister(reg)) {
               llvm::dbgs() << "\n **** vr" << pseudoReg << "/" << reg << "\n";
               instruction->dump();
               block->dump();
            }
#endif
            assert(VR::Info::IsPhysicalRegister(reg));
            if (reg && operand->getSubReg()) {
               reg = TRI->getSubReg(reg, operand->getSubReg());
               assert(reg && "The register class does not have the required subregister");
               operand->setSubReg(0);
            }
            operand->setReg(reg);

            if (iteratingSources) {
               if (!inBlockVRegs.count(pseudoReg)) {
                  if (!isDBG_VALUE) {
                     block->addLiveIn(reg);
                     inBlockVRegs.insert(pseudoReg);
                     registerLivenessData->LiveInBitVector->set(reg);
                  }
               }
            } else {
               inBlockVRegs.insert(pseudoReg);
            }
#if defined(TILED_DEBUG_DUMPS)
            unsigned pseudo = pseudoReg & (~(1u << 31));
            llvm::dbgs() << "vreg" << pseudo << "(R" << (reg - 1) << ")" << (operand->isDef() ? " <= " : ", ");
#endif
         }

         next_source_and_destination_opnd_v2(operand, instruction, end_iter);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Rewrite the IR of a tile and assign physical registers
//
// Arguments:
//
//    tile - tile for assignment
//
// Remarks:
//
//    Do one pass over the IR replacing all category pseudo
//    appearances with the allocated physical register.
//
//------------------------------------------------------------------------------

//TBD: In this algorithm the virtual => physical mapping is done on a per/tile basis, llvm::VirtRegMap,
//     on the other hand, works on a per function basis. With SSA it's fine but extra care has to be
//     taken with multiple definition points of eliminated PHIs

void
Allocator::RewriteRegisters
(
   GraphColor::Tile * tile
)
{
   Dataflow::RegisterLivenessWalker * registerLivenessWalker = this->Liveness->RegisterLivenessWalker;
   VR::Info *  vrInfo = this->VrInfo;

   DEBUG(llvm::dbgs() << "\nTILE#" << tile->Id << "   (allocated)\n");

   Graphs::MachineBasicBlockVector::iterator tb;

   // foreach_block_in_tile
   for (tb = tile->BlockVector->begin(); tb != tile->BlockVector->end(); ++tb)
   {
      llvm::MachineBasicBlock * block = *tb;

      DEBUG(llvm::dbgs() << "\nMBB#" << block->getNumber() << "   (allocated)\n");

      this->RewriteRegistersInBlock(tile, block, vrInfo);

      if (isBoundaryBlock(block)) continue;

      Dataflow::LivenessData * blockLiveData =
         static_cast<Dataflow::LivenessData *>(registerLivenessWalker->GetBlockData(block));
      assert(blockLiveData);
      llvm::SparseBitVector<> * liveInBitVector = blockLiveData->LiveInBitVector;
      llvm::SparseBitVector<>::iterator r;

      // foreach_sparse_bv_bit
      for (r = liveInBitVector->begin(); r != liveInBitVector->end(); ++r)
      {
         unsigned regTag = *r;
         unsigned vrReg = vrInfo->GetRegister(regTag);

         if (Tiled::VR::Info::IsVirtualRegister(vrReg)) {
            unsigned physReg = tile->GetAllocatedRegister(regTag);
            if (physReg != VR::Constants::InvalidReg) {
               assert(Tiled::VR::Info::IsPhysicalRegister(physReg));
               block->addLiveIn(physReg);
               // Why is there a -1 here ?
               // DEBUG(llvm::dbgs() << " >>> BB#" << block->getNumber() << "  addLiveIn  R" << (physReg-1) << "\n");
               liveInBitVector->set(this->VrInfo->GetTag(physReg));
            }
         } else {
            assert(Tiled::VR::Info::IsPhysicalRegister(vrReg));
            block->addLiveIn(vrReg);
            // DEBUG(llvm::dbgs() << " >> BB#" << block->getNumber() << "  addLiveIn  R" << (vrReg-1) << "\n");
         }
      }
   }

   // Mark tile as having assigned registers.
   tile->HasAssignedRegisters = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the tile for the given instruction.
//
// Remarks:
//
//    Currently only works on boundary instructions.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Allocator::GetTile
(
   llvm::MachineInstr * instruction
)
{
   assert(GraphColor::Tile::IsTileBoundaryInstruction(instruction));

   //TODO: llvm::MachineInstr does not have an ID field
   std::map<llvm::MachineInstr*, unsigned>::iterator i = Allocator::instruction2TileId.find(instruction);
   if (i == Allocator::instruction2TileId.end())
      return nullptr;

   unsigned                 tileId = i->second;
   GraphColor::TileGraph *  tileGraph = this->TileGraph;
   GraphColor::Tile *       nestedTile = tileGraph->GetTileByTileId(tileId);

   return nestedTile;
}

unsigned
Allocator::GetTileId
(
   llvm::MachineInstr * instruction
)
{
   assert(GraphColor::Tile::IsTileBoundaryInstruction(instruction));

   //TODO: llvm::MachineInstr does not have an ID field
   std::map<llvm::MachineInstr*, unsigned>::iterator i = Allocator::instruction2TileId.find(instruction);
   if (i == Allocator::instruction2TileId.end())
      return 0;

   return i->second;
}

void
Allocator::SetTileId
(
   llvm::MachineInstr * instruction,
   unsigned             tileId
)
{
   assert(GraphColor::Tile::IsTileBoundaryInstruction(instruction));

   //TODO: llvm::MachineInstr does not have an ID field
   std::map<llvm::MachineInstr*,unsigned>::value_type entry(instruction, tileId);

   std::pair<std::map<llvm::MachineInstr*,unsigned>::iterator, bool> result = Allocator::instruction2TileId.insert(entry);
   if (!result.second) {
      //TODO: is the override valid to happen?
      (result.first)->second = tileId;
   }
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Get the tile for the given block
//
// Returns:
//
//    Tile for given block iff TileGraph exists, otherwise returns nullptr.
//
//------------------------------------------------------------------------------

GraphColor::Tile *
Allocator::GetTile
(
   llvm::MachineBasicBlock * block
)
{
   GraphColor::TileGraph *  tileGraph = this->TileGraph;
   GraphColor::Tile *       tile = nullptr;

   if (tileGraph != nullptr) {
      tile = tileGraph->GetTile(block);
   }

   return tile;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Find the scaling factor for a given profile count.
//
// Arguments:
//
//    count - Frequency to adjust
//
// Returns:
//
//    Input count divided by entry frequency or unit.
//
//------------------------------------------------------------------------------

Profile::Count
Allocator::GetBiasedFrequency
(
   Profile::Count      count
)
{
   Profile::Count profileBasisCount = this->ProfileBasisCount;

   assert(Tiled::CostValue::IsGreaterThanZero(profileBasisCount));

   Profile::Count    frequency = Tiled::CostValue::Divide(count, profileBasisCount);
   Tiled::CostValue  minFrequency = Tiled::CostValue::Divide(1.0, 65536.0);
   bool  isUnderLimit = Tiled::CostValue::CompareLT(frequency, minFrequency);

   if (isUnderLimit) {
      frequency = minFrequency;
   }

   return frequency;
}

Profile::Count
Allocator::getProfileCount
(
   llvm::MachineInstr * instruction
) const
{
   llvm::MachineBasicBlock * mbb = instruction->getParent();

   if (Tile::IsTileBoundaryInstruction(instruction)) {
      assert(mbb->pred_size() == 1);
      llvm::MachineBasicBlock * pred_mbb = *(mbb->pred_begin());
      return getProfileCount(pred_mbb);
   } else {
      return getProfileCount(mbb);
   }
}


Profile::Count
Allocator::getProfileCount
(
   llvm::MachineBasicBlock * mbb
) const
{
   Profile::Count count = (this->MbbFreqInfo->getBlockFreq(mbb)).getFrequency();

   if (!Profile::Count::IsZero(count) || mbb->pred_size() != 1)
      return count;

   llvm::MachineBasicBlock * pred_mbb = *(mbb->pred_begin());
   return getProfileCount(pred_mbb);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Find the scaling factor for costs on a given instruction based on profile info.
//
// Arguments:
//
//    instruction - Instruction frequency to bias
//
// Returns:
//
//    Execution frequency of the instruction divided by entry
//    frequency or unit.
//
//------------------------------------------------------------------------------

Tiled::CostValue
Allocator::GetBiasedInstructionFrequency
(
   llvm::MachineInstr * instruction
)
{
   Profile::Count count = this->getProfileCount(instruction);

   if (Profile::Count::IsZero(count)) {
      llvm::MachineBasicBlock * mbb = instruction->getParent();
      if (mbb && isBoundaryBlock(mbb)) {
         count = this->getProfileCount(mbb);
      }
   }

   Profile::Count frequency = this->GetBiasedFrequency(count); 

   return frequency;
}

Tiled::CostValue
Allocator::GetBiasedBlockFrequency
(
   llvm::MachineBasicBlock * block
)
{
   Profile::Count count = (this->MbbFreqInfo->getBlockFreq(block)).getFrequency();
   Profile::Count frequency = this->GetBiasedFrequency(count);

   return frequency;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Begin allocation pass
//
// Arguments:
//
//    pass - Pass to begin
//
//------------------------------------------------------------------------------

void
Allocator::BeginPass
(
   GraphColor::Pass pass
)
{
   // Passes can not be nested
   assert(this->Pass == GraphColor::Pass::Ready);

   this->Pass = pass;

   // Initialize global register cost vector.
   GraphColor::CostVector * registerCostVector = this->RegisterCostVector;

   this->InitializeRegisterCosts(registerCostVector);

   // Modify global register costs for second pass allocation.

   if (pass == GraphColor::Pass::Assign) {
      GraphColor::TileList * tilesPreOrder = this->TileGraph->PreOrderTileList;
      GraphColor::TileList::iterator titer;

      // foreach_tile_in_dfs_preorder
      for (titer = tilesPreOrder->begin(); titer != tilesPreOrder->end(); ++titer)
      {
         GraphColor::Tile * tile = *titer;

         if (!tile->HasAssignedRegisters) {
            GraphColor::LiveRangeVector * slrVector = tile->GetSummaryLiveRangeEnumerator();

            // foreach_summaryliverange_in_tile
            for (unsigned i = 1; i < slrVector->size(); ++i)
            {
               GraphColor::LiveRange *  summaryLiveRange = (*slrVector)[i];

               if (summaryLiveRange->IsPhysicalRegister) {
                  continue;
               }

               if (summaryLiveRange->HasHardPreference) {
                  // Clear register cost since we know that we've used it.

                  unsigned           registerAliasTag = this->VrInfo->GetTag(summaryLiveRange->Register);
                  //Tiled::VR::Info *  vrInfo = this->VrInfo;

                  //For architectures without sub-registers the commented below loop collapses to single iteration
                  //foreach_may_partial_alias_of_tag(aliasTag, registerAliasTag, vrInfo)
                  //{
                     unsigned scratchRegister = this->VrInfo->GetRegister(registerAliasTag);
                     (*this->RegisterCostVector)[scratchRegister] = this->ZeroCost;
                  //}
                  //next_may_partial_alias_of_tag;
               }
            }
         }
      }
   }
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Begin pass on a tile.
//
// Arguments:
//
//    tile - tile to begin pass on
//    pass - Operative pass in the allocator
//
// Remarks
//
//    Create all the data structures used to by the allocator to 
//    process the tile.
//
//------------------------------------------------------------------------------

void
Allocator::BeginPass
(
   GraphColor::Tile * tile,
   GraphColor::Pass   pass
)
{
   assert(this->ActiveTile == nullptr);
   assert(this->Pass == pass);

   this->ActiveTile = tile;
   tile->BeginPass(pass);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Begin iteration on a tile.
//
// Arguments:
//
//    tile - tile to begin iteration on
//    pass - Operative pass in the allocator
//
//------------------------------------------------------------------------------

void
Allocator::BeginIteration
(
   GraphColor::Tile * tile,
   GraphColor::Pass   pass
)
{
   assert(this->Pass == pass);

   if (tile->Iteration >= this->IterationLimit) {
      assert(0 && "Run away Graph Color iterations");

      //What's llvm FatalError w/ message ?
   }

   tile->Iteration++;

   tile->BeginIteration(pass);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End pass 
//
// Arguments:
//
//    pass - Pass to end
//
//------------------------------------------------------------------------------

void
Allocator::EndPass
(
   GraphColor::Pass pass
)
{
   // Passes must be properly sequenced.
   assert(this->Pass == pass);

   this->Pass = GraphColor::Pass::Ready;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End pass on a tile.
//
// Arguments:
//
//    tile - tile to end pass on
//    pass - Operative pass in the allocator
//
// Remarks
//
//    Tear down all the data structures used to by the allocator to 
//    process the tile.
//
//------------------------------------------------------------------------------

void
Allocator::EndPass
(
   GraphColor::Tile * tile,
   GraphColor::Pass   pass
)
{
   assert(this->ActiveTile == tile);
   assert(this->Pass == pass);

   // Set up the HasDeadRegisterSpills property of functionUnit

   if ((tile->Iteration > 1) && (tile->ExtendedBasicBlockCount > 1)) {
      // The only places that may yield dead spills are those EBB boudaries. 
      // We typically don't share spilled registers across EBBs, rather, we
      // spill registers in one EBB and reload them in others. Different 
      // iterations may spill the same register in the same place repeatedly,
      // due to the limitation of the current spill optimizer, such that we
      // may end up with multiple repeated spills and some of them may be dead. 

      this->FunctionUnit->HasDeadRegisterSpills = true;
   }

   tile->EndPass(pass);

   tile->Iteration = 0;

   this->ActiveTile = nullptr;

   // Clear allocator 'do not spill' vector after tile is complete -
   // So it won't collide but the vector doesn't need to grow across tiles.

   this->DoNotSpillAliasTagBitVector->clear();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    End iteration on a tile.
//
// Arguments:
//
//    tile - tile to end iteration on
//    pass - Operative pass in the allocator
//
//------------------------------------------------------------------------------

void
Allocator::EndIteration
(
   GraphColor::Tile * tile,
   GraphColor::Pass   pass
)
{
   assert(this->Pass == pass);

   tile->EndIteration(pass);

   // Ending iterations of pass: clear the pending spill alias tag bit vector
   llvm::SparseBitVector<> * pendingSpillAliasTagBitVector = this->PendingSpillAliasTagBitVector;
   pendingSpillAliasTagBitVector->clear();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the allocatable registers for this register category.
//
// Arguments:
//
//    registerCategory - The register category 
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
Allocator::GetAllocatableRegisters
(
   const llvm::TargetRegisterClass * registerCategory
)
{
   unsigned                  registerCategoryId = registerCategory->getID();
   llvm::SparseBitVector<> * allocatableRegisterAliasTagBitVector = nullptr;

   allocatableRegisterAliasTagBitVector =
      (*this->RegisterCategoryIdToAllocatableAliasTagBitVector)[registerCategoryId];

   //TODO: initialize the map (currently a single entry) with a SparseBitVector-s produced
   //     from TargetRegisterClass:
   //     loop over MCPhysReg-s with TargetRegisterClass::begin()/end(), and set the (sparse) bits

   return allocatableRegisterAliasTagBitVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the allocatable registers for this live ranges register category.
//
// Arguments:
//
//    liveRange - live range to get registers for
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
Allocator::GetCategoryRegisters
(
   GraphColor::LiveRange * liveRange
)
{
   const llvm::TargetRegisterClass * registerCategory = liveRange->GetRegisterCategory();
   llvm::SparseBitVector<> *         allocatableRegisterAliasTagBitVector;
   llvm::SparseBitVector<> *         allocatableRegistersForCategoryAliasTagBitVector;

   allocatableRegistersForCategoryAliasTagBitVector = this->GetAllocatableRegisters(registerCategory);
   allocatableRegisterAliasTagBitVector = this->ScratchBitVector1;

   *allocatableRegisterAliasTagBitVector = *allocatableRegistersForCategoryAliasTagBitVector;

   // Prune the allocatable registers for live range with byte references.
   // <place for code for filtering of byte-addressable registers>

   return allocatableRegisterAliasTagBitVector;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get the allocatable registers for this live range/tile based on
//    conflict information.
//
// Arguments:
//
//    tile      - Tile context.
//    liveRange - live range to get registers for
//
//------------------------------------------------------------------------------

llvm::SparseBitVector<> *
Allocator::GetAllocatableRegisters
(
   GraphColor::Tile *      tile,
   GraphColor::LiveRange * liveRange
)
{
   Tiled::VR::Info *         vrInfo = this->VrInfo;
   llvm::SparseBitVector<> * allocatableRegisterAliasTagBitVector;

   // Get allocatable registers for the register category used by this live range.
   allocatableRegisterAliasTagBitVector = this->GetCategoryRegisters(liveRange);

   // Prune the allocatable registers for non-block local live ranges to avoid bug in X87 allocator.
   // We only need to do this on the first iteration inorder to spill appropriately the first time around.

   //note: <place for special code for IsX87SupportEnabled>

   // Prune the allocatable registers to avoid conflicts.

   if (!allocatableRegisterAliasTagBitVector->empty()) {
      GraphColor::ConflictGraph * conflictGraph = tile->ConflictGraph;
      GraphColor::GraphIterator iter;

      // foreach_conflict_liverange
      for (GraphColor::LiveRange * conflictLiveRange = conflictGraph->GetFirstConflictLiveRange(&iter, liveRange);
           (conflictLiveRange != nullptr);
           conflictLiveRange = conflictGraph->GetNextConflictLiveRange(&iter)
          )
      {
         // Remove any partially overlapping physical registers from conflicting live ranges.
         if (conflictLiveRange->IsAssigned()) {
            unsigned registerTag = vrInfo->GetTag(conflictLiveRange->Register);
            vrInfo->MinusMayPartialTags(registerTag, allocatableRegisterAliasTagBitVector);

            if (allocatableRegisterAliasTagBitVector->empty()) {
               // Nothing to allocate we're done.
               break;
            }
         }
      }
   }

   return allocatableRegisterAliasTagBitVector;
}


//------------------------------------------------------------------------------
//
// Description:
//
//    Find a register in a given register preference vector.
//
// Arguments:
//
//    registerPreferenceVector - register preference vector to search
//    reg                      - register to search for
//
// Returns
//
//    Returns index in register was found in vector or 
//    GraphColor::Allocator::Constants::IndexNotFound.
//
//------------------------------------------------------------------------------

int
Allocator::FindRegisterPreference
(
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector,
   unsigned                                        reg
)
{
   for (unsigned index = 0; index < registerPreferenceVector->size(); index++)
   {
      if (((*registerPreferenceVector)[index]).Register == reg) {
         return index;
      }
   }

   return static_cast<int>(Allocator::Constants::IndexNotFound);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set the preferred costs for a vector preferred registers.
//
// Remarks:
//
//    Used to override the costs in the vector.
//
//------------------------------------------------------------------------------

void
Allocator::SetRegisterPreferenceCost
(
   GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
   GraphColor::AllocatorCostModel *               costModel,
   Tiled::Cost                                    cost
)
{
   // If costs in vector are are greater than unit cost, then they are overridden to preferred cost.

   Tiled::Cost unitCost = this->UnitCost;

   for (unsigned index = 0; index < registerPreferenceVector->size(); index++)
   {
      Tiled::Cost regCost = ((*registerPreferenceVector)[index]).Cost;

      if (costModel->Compare(&regCost, &unitCost) > 0) {
         ((*registerPreferenceVector)[index]).Cost = cost;
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert register preference (register and cost) to a given register preference 
//    vector.
//
// Arguments:
//
//    registerPreferenceVector - register preference vector being added to
//    reg                      - register to add
//    cost                     - cost to add
//
// Returns
//
//    void
//
//------------------------------------------------------------------------------

void
Allocator::InsertRegisterPreference
(
   GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
   unsigned                                       reg,
   Tiled::Cost                                    cost
)
{
   GraphColor::PhysicalRegisterPreference registerPreference;

   registerPreference.Register = reg;
   registerPreference.Cost = cost;

   registerPreferenceVector->push_back(registerPreference);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Update register preference (register and cost) in a given register preference 
//    vector.
//
// Arguments:
//
//    registerPreferenceVector - register preference vector being added to
//    index                    - index of register preference
//    reg                      - register being updated
//    cost                     - cost being updated 
//
// Remarks:
//
//    The specified cost is added to the the existing cost in the preference vector.
//
// Returns:
//
//    void
//
//------------------------------------------------------------------------------

void
Allocator::UpdateRegisterPreference
(
   GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
   unsigned                                       index,
   unsigned                                       reg,
   Tiled::Cost                                    cost
)
{
   Tiled::Cost updatedCost;

   (reg);
   assert(reg == ((*registerPreferenceVector)[index]).Register);

   updatedCost = ((*registerPreferenceVector)[index]).Cost;
   updatedCost.IncrementBy(&cost);
   ((*registerPreferenceVector)[index]).Cost = updatedCost;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Combine one register preference into a summary register 
//    preference vector.
//
// Arguments:
//
//    summaryRegisterPreferenceVector - summary register preference vector being 
//                                      combined into
//    reg                             - register to combine
//    cost                            - cost to combine
//
// Remarks:
//
//    The specified cost for the register is calculated to be the max of the 
//    combined costs.
//
// Returns:
//
//    void
//
//------------------------------------------------------------------------------

void
Allocator::CombineRegisterPreference
(
   RegisterAllocator::PreferenceCombineMode        combineMode,
   GraphColor::AllocatorCostModel                * costModel,
   GraphColor::PhysicalRegisterPreferenceVector *  summaryRegisterPreferenceVector,
   unsigned                                        preferredReg,
   Tiled::Cost                                     preferredCost
)
{
   unsigned                summaryPreferredReg;
   Tiled::Cost             summaryPreferredCost;
   int                     index;

   // Get summary preferred register and cost.

   summaryPreferredReg = preferredReg;
   index = this->FindRegisterPreference(summaryRegisterPreferenceVector, summaryPreferredReg);

   // If it doesn't exist, add it.

   if (index == static_cast<int>(Allocator::Constants::IndexNotFound)) {
      this->InsertRegisterPreference(summaryRegisterPreferenceVector, preferredReg, preferredCost);
   }

   // If in addition mode, then add preference costs together.

   else if (combineMode == RegisterAllocator::PreferenceCombineMode::Add) {
      this->UpdateRegisterPreference(summaryRegisterPreferenceVector, index, preferredReg, preferredCost);
   }

   // Otherwise keep track of max preference cost.

   else {
      assert(combineMode == RegisterAllocator::PreferenceCombineMode::Max);

      summaryPreferredCost = ((*summaryRegisterPreferenceVector)[index]).Cost;
      if ((costModel->Compare(&summaryPreferredCost, &preferredCost)) < 0) {
         ((*summaryRegisterPreferenceVector)[index]).Cost = preferredCost;
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Combine one register preference vector into another, summary register 
//    preference vector.
//
// Arguments:
//
//    summaryRegisterPreferenceVector - summary register preference vector being 
//                                      combined into
//    registerPreferenceVector        - register preference vector being combined
//
// Remarks:
//
//    The specified cost foreach register is calculated to be the max of the combined
//    costs.
//
// Returns:
//
//    void
//
//------------------------------------------------------------------------------

void
Allocator::CombineRegisterPreference
(
   RegisterAllocator::PreferenceCombineMode        combineMode,
   GraphColor::AllocatorCostModel               *  costModel,
   GraphColor::PhysicalRegisterPreferenceVector *  summaryRegisterPreferenceVector,
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
)
{
   unsigned                preferredReg;
   Tiled::Cost             preferredCost;
   unsigned                index;

   // Calculated most preferred register with the least anti-preferred cost.

   for (index = 0; index < registerPreferenceVector->size(); index++)
   {
      // Get preferred register and cost.

      preferredReg = ((*registerPreferenceVector)[index]).Register;
      preferredCost = ((*registerPreferenceVector)[index]).Cost;

      // Combine this register and cost into summary preference vector.  Keep track of max cost.

      this->CombineRegisterPreference(combineMode, costModel, summaryRegisterPreferenceVector,
         preferredReg, preferredCost);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert or update preferred registers set given a required and preferred 
//    register.
//
// Arguments:
//
//    requiredReg                      - required register (physical or pseudo)
//    preferredReg                     - preferred register (physical)
//    allocatableRegisterTagBitVector - bit vector of allocatable register tags
//    doOverridePreferredCost          - should we override the preferred cost
//    overridePreferredCost            - cost to be used when overriding
//    registerPreferenceVector         - output register preference vector (registers and costs)
//
//------------------------------------------------------------------------------

void
Allocator::InsertOrUpdateRegisterPreference
(
   GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector,
   unsigned                                       requiredReg,
   unsigned                                       preferredReg,
   Tiled::Cost                                    preferredCost,
   llvm::SparseBitVector<> *                      allocatableRegisterTagBitVector
)
{
   unsigned  regAliasTag;
   int       index;
 
   // Make our preferred register match the size of our required register.

   // <place for code supporting architectures with sub-registers>

   // Calculate the total cost savings of being allocated the preferred register.

   regAliasTag = this->VrInfo->GetTag(preferredReg);
   if (allocatableRegisterTagBitVector->test(regAliasTag)) {
      // Add to or update the preference vector.

      index = this->FindRegisterPreference(registerPreferenceVector, preferredReg);
      if (index == static_cast<int>(Allocator::Constants::IndexNotFound)) {
         // Add register and initial cost to preference vector.

         this->InsertRegisterPreference(registerPreferenceVector, preferredReg, preferredCost);
      } else {
         // Update cost in existing entry in preference vector.  Costs are added together.
         this->UpdateRegisterPreference(registerPreferenceVector, index, preferredReg, preferredCost);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get preferred registers for a given live range (recursively).
//
// Remarks:
//
//    Look at preference edges in the preference graph, get a selection of 
//    preferred registers and costs.
//
// Arguments:
//
//    tile                             - tile to preference with
//    liveRange                        - live range to preference
//    requiredReg                      - required register (pseudo or physical, constraints)
//    searchDepth                      - depth to conduct search on preference graph
//    allocatableRegisterTagBitVector  - bit vector of allocatable register tags
//    doOverridePreferredCost          - should we override the preferred cost
//    overridePreferredCost            - cost to be used when overriding
//    registerPreferenceVector         - output register preference vector (registers and costs)
//
//------------------------------------------------------------------------------

void
Allocator::GetPreferredRegistersRecursively
(
   GraphColor::Tile *                              tile,
   GraphColor::LiveRange *                         liveRange,
   unsigned                                        requiredReg,
   int                                             searchDepth,
   llvm::SparseBitVector<> *                       allocatableRegisterTagBitVector,
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
)
{
   GraphColor::PreferenceGraph *                   preferenceGraph = tile->PreferenceGraph;
   GraphColor::ConflictGraph *                     conflictGraph = tile->ConflictGraph;
   GraphColor::AllocatorCostModel *                costModel = tile->CostModel;
   GraphColor::PhysicalRegisterPreferenceVector *  temporaryRegisterPreferenceVector;
   Tiled::RegisterAllocator::PreferenceConstraint  preferenceConstraint;
   Tiled::Cost                                     preferredCost;
   unsigned                                        preferredReg;
   unsigned                                        searchLiveRangeId = this->SearchLiveRangeId;

   assert(liveRange->SearchNumber == this->SearchNumber);
   assert(searchDepth > 0);

   searchDepth--;

   // If this live range is assigned, add it as a preference.

   if (liveRange->IsAssigned()) {
      // The assigned register is the preferred register.
      preferredReg = liveRange->Register;

      // Cost starts out as zero, will be added by parent when preference edge cost is known.
      preferredCost = this->ZeroCost;

      // Insert or update register preference.
      this->InsertOrUpdateRegisterPreference(registerPreferenceVector, requiredReg, preferredReg,
         preferredCost, allocatableRegisterTagBitVector);

      return;
   }

   // We are processing an unassigned live range, so do the search for 
   // preferred registers.

   assert((requiredReg == VR::Constants::InitialPseudoReg));
      //was: (requiredReg->IsRegisterCategoryPseudo)
   assert(!conflictGraph->HasConflict(searchLiveRangeId, liveRange->Id));

   GraphColor::GraphIterator piter;
   GraphColor::LiveRange *   preferenceLiveRange;

   //foreach_preference_liverange
   for (preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&piter, liveRange, &preferredCost, &preferenceConstraint);
        (preferenceLiveRange != nullptr);
        preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&piter, &preferredCost, &preferenceConstraint))
   {
      assert(preferenceConstraint == Tiled::RegisterAllocator::PreferenceConstraint::SameRegister);

      // Search recursively across pseudo register preferences.

      preferredReg = preferenceLiveRange->Register;
      if (preferredReg == VR::Constants::InitialPseudoReg) {
         //was: (preferredReg->IsRegisterCategoryPseudo)

         // Starting again from the root live range.  Reset the search
         // number for the new preference edge.

         if (liveRange->Id == searchLiveRangeId) {
            this->SearchNumber++;
            liveRange->SearchNumber = this->SearchNumber;
         }

         if (preferenceLiveRange->SearchNumber != this->SearchNumber) {
            preferenceLiveRange->SearchNumber = this->SearchNumber;

            if (!conflictGraph->HasConflict(searchLiveRangeId, preferenceLiveRange->Id)) {
               // Recursively look for preferences.

               if (searchDepth > 0) {
                  // Get a temporary preference vector to use.

                  temporaryRegisterPreferenceVector =
                     (*this->TemporaryRegisterPreferenceVectors)[searchDepth];
                  temporaryRegisterPreferenceVector->clear();

                  // Recursively get preferences in temporary vector.

                  this->GetPreferredRegistersRecursively(tile, preferenceLiveRange, requiredReg, searchDepth,
                     allocatableRegisterTagBitVector, temporaryRegisterPreferenceVector);

                  // Override the cost in vector to be no more than the cost of this edge.

                  this->SetRegisterPreferenceCost(temporaryRegisterPreferenceVector, costModel, preferredCost);

                  // Add these preferences to those already calculated at this level of the search.

                  this->CombineRegisterPreference(RegisterAllocator::PreferenceCombineMode::Add, costModel,
                     registerPreferenceVector, temporaryRegisterPreferenceVector);
               }
            }
         }

      } else {
         // Insert or update register preference.

         this->InsertOrUpdateRegisterPreference(registerPreferenceVector, requiredReg, preferredReg,
            preferredCost, allocatableRegisterTagBitVector);
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get preferred registers for a given live range.
//
// Remarks:
//
//    Look at preference edges in the preference graph, get a selection of 
//    preferred registers and costs.
//
// Arguments:
//
//    tile                             - Tile to preference with
//    liveRange                        - LiveRange to preference
//    requiredReg                      - required register (pseudo or physical, constraints)
//    searchDepth                      - depth to conduct search on preference graph
//    allocatableRegisterTagBitVector - BitVector of allocatable register tags
//    registerPreferenceVector         - output register preference vector (registers and costs)
//
//------------------------------------------------------------------------------

void
Allocator::GetPreferredRegisters
(
   GraphColor::Tile *                             tile,
   GraphColor::LiveRange *                        liveRange,
   unsigned                                       requiredReg,
   int                                            searchDepth,
   llvm::SparseBitVector<> *                      allocatableRegisterTagBitVector,
   GraphColor::PhysicalRegisterPreferenceVector * registerPreferenceVector
)
{
   // Reset the preferred register vector and associated costs.

   registerPreferenceVector->clear();

   // Reset search controls.

   this->SearchLiveRangeId = liveRange->Id;
   this->SearchNumber++;

   // Search recursively for preferred registers for this live range.

   liveRange->SearchNumber = this->SearchNumber;
   this->GetPreferredRegistersRecursively(tile, liveRange, requiredReg, searchDepth,
      allocatableRegisterTagBitVector, registerPreferenceVector);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get most preferred register for a given register preference vector
//
// Arguments:
//
//    tile                             - tile to preference with
//    registerPreferenceVector         - register preference vector (registers and costs)
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetMostPreferredRegister
(
   GraphColor::AllocatorCostModel *                costModel,
   Tiled::Cost *                                   outCost,
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
)
{
   unsigned            preferredReg;
   Tiled::Cost         preferredCost = this->ZeroCost;
   unsigned            reg;
   Tiled::Cost         cost;
   unsigned            index;

   preferredReg = VR::Constants::InvalidReg;

   for (index = 0; index < registerPreferenceVector->size(); index++)
   {
      reg = ((*registerPreferenceVector)[index]).Register;
      cost = ((*registerPreferenceVector)[index]).Cost;

      if (preferredReg == VR::Constants::InvalidReg) {
         preferredCost = cost;
         preferredReg = reg;
      } else if ((costModel->Compare(&preferredCost, &cost)) < 0) {
         preferredCost = cost;
         preferredReg = reg;
      }
   }

   // Return most preferred register.

   *outCost = preferredCost;

   return preferredReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get least anti-preferred register for a given live range and register preference vector
//
// Arguments:
//
//    liveRange                 - liveRange associated with register preference vector
//    costModel                 - cost model to use
//    registerPreferenceVector  - register preference vector (registers and costs)
//
//------------------------------------------------------------------------------

unsigned
Allocator::GetLeastAntiPreferredRegister
(
   GraphColor::LiveRange *                         liveRange,
   GraphColor::AllocatorCostModel *                costModel,
   Tiled::Cost *                                   outCost,
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector
)
{
   unsigned              preferredReg = VR::Constants::InvalidReg;
   unsigned              summaryReg = VR::Constants::InvalidReg;
   Tiled::Cost           preferredCost = this->ZeroCost;
   unsigned              reg;
   Tiled::Cost           cost;
   uint64_t              index;

   // If live range is summary proxy then prime the search with the register 
   // allocated bottom up (if it is available).  

   if (liveRange->IsSummaryProxy) {
      summaryReg = liveRange->SummaryRegister;
      assert(summaryReg != VR::Constants::InvalidReg);

      index = this->FindRegisterPreference(registerPreferenceVector, summaryReg);
      if (index != static_cast<int>(Allocator::Constants::IndexNotFound)) {
         preferredReg = ((*registerPreferenceVector)[index]).Register;
         preferredCost = ((*registerPreferenceVector)[index]).Cost;
      }

      if (preferredReg != VR::Constants::InvalidReg) {
         assert(summaryReg == preferredReg);
      } else {
         summaryReg = VR::Constants::InvalidReg;
      }
   }

   // Find the least anti-preferred register.

   for (index = 0; index < registerPreferenceVector->size(); index++)
   {
      reg = ((*registerPreferenceVector)[index]).Register;
      cost = ((*registerPreferenceVector)[index]).Cost;

      if (preferredReg == VR::Constants::InvalidReg) {
         preferredCost = cost;
         preferredReg = reg;
      } else {
         int compare = costModel->Compare(&preferredCost, &cost);

         if (compare > 0) {
            preferredCost = cost;
            preferredReg = reg;
         }

         // If this register has the same cost/benefit trade-off, then select the 
         // register with the lower id.  This mimics the selection process when
         // no preference is present.  Helps with consistency.

         else if ((compare == 0) && (preferredReg > reg)) {
            // But, don't undo preferencing of summary register when present.
            if (preferredReg != summaryReg) {
               preferredReg = reg;
            }
         }
      }
   }

   // Return least anti-preferred register.

   *outCost = preferredCost;

   return preferredReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get a vector of preferred live ranges by conduction a search to the specified depth.
//
//------------------------------------------------------------------------------

void
Allocator::GetPreferredLiveRangesRecursively
(
   GraphColor::Tile *            tile,
   GraphColor::LiveRange *       liveRange,
   unsigned                      searchDepth,
   GraphColor::LiveRangeVector * preferenceLiveRangeVector
)
{
   GraphColor::PreferenceGraph *    preferenceGraph = tile->PreferenceGraph;
   GraphColor::ConflictGraph *      conflictGraph = tile->ConflictGraph;
   Tiled::RegisterAllocator::PreferenceConstraint preferenceConstraint;
   unsigned                         preferredReg;
   Tiled::Cost                      preferredCost;
   Tiled::Id                        searchLiveRangeId;

   assert(liveRange->SearchNumber == this->SearchNumber);
   assert(searchDepth > 0);

   searchDepth--;

   searchLiveRangeId = this->SearchLiveRangeId;
   GraphColor::GraphIterator piter;
   GraphColor::LiveRange *   preferenceLiveRange;

   //foreach_preference_liverange
   for ( preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&piter, liveRange, &preferredCost, &preferenceConstraint);
         preferenceLiveRange != nullptr;
         preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&piter, &preferredCost, &preferenceConstraint))
   {
      if (preferenceLiveRange->SearchNumber != this->SearchNumber) {
         preferenceLiveRange->SearchNumber = this->SearchNumber;

         if (!conflictGraph->HasConflict(searchLiveRangeId, preferenceLiveRange->Id)) {
            preferenceLiveRangeVector->push_back(preferenceLiveRange);

            if (searchDepth > 0) {
               preferredReg = preferenceLiveRange->Register;
               if (preferredReg == VR::Constants::InitialPseudoReg || Tiled::VR::Info::IsVirtualRegister(preferredReg)) {
                  //was: (preferredReg->IsRegisterCategoryPseudo)
                  this->GetPreferredLiveRangesRecursively(tile, preferenceLiveRange, searchDepth, preferenceLiveRangeVector);
               }
            }
         }
      }
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get a vector of preferred live ranges by conducting a search to the specified depth.
//
//------------------------------------------------------------------------------

void
Allocator::GetPreferredLiveRanges
(
   GraphColor::Tile *            tile,
   GraphColor::LiveRange *       liveRange,
   unsigned                      searchDepth,
   GraphColor::LiveRangeVector * preferenceLiveRangeVector
)
{
   // Reset the preference live range vector.

   preferenceLiveRangeVector->clear();

   // Reset search controls.

   this->SearchLiveRangeId = liveRange->Id;
   this->SearchNumber++;

   // Do a recursive search to find a live directly and indirectly preferred live ranges.

   liveRange->SearchNumber = this->SearchNumber;
   this->GetPreferredLiveRangesRecursively(tile, liveRange, searchDepth, preferenceLiveRangeVector);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Get anti-preferred registers for a given live range.
//
// Remarks:
//
//    Look at conflict edges and the conflicting live ranges.  Walk the preference edges for the
//    conflicting live range in the preference graph. Get a selection of anti-preferred registers and costs
//
//    Also, prune the allocatable register set to avoid anti-preferred registers.
//
// Arguments:
//
//    tile                                                - tile to preference with
//    liveRange                                           - liveRange to preference
//    requiredReg                                         - required register (pseudo or physical, constraints)
//    searchDepth                                         - depth to conduct search on preference graph
//    allocatableRegisterTagBitVector                     - bit vector of allocatable register tags
//    antiPreferencePrunedAllocatableRegisterTagBitVector - output bit vector of pruned allocatable reg tags
//    registerAntiPreferenceVector                        - output anti-preferred register vector
//    registerPreferenceVector                            - scratch preferred cost vector (used in search)
//
//------------------------------------------------------------------------------

void
Allocator::GetAntiPreferredRegisters
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
)
{
   GraphColor::ConflictGraph *                     conflictGraph;
   GraphColor::PreferenceGraph *                   preferenceGraph;
   GraphColor::AllocatorCostModel *                costModel;
   GraphColor::PhysicalRegisterPreferenceVector *  temporaryRegisterPreferenceVector;
   GraphColor::CostVector *                        registerCostVector;
   unsigned                                        antiPreferredReg;
   unsigned                                        preferredReg;
   Tiled::Cost                                     preferredCost;
   RegisterAllocator::PreferenceConstraint         preferenceConstraint;
   unsigned                                        reg;
   Tiled::Cost                                     cost;
   Tiled::Cost                                     zeroCost;
   unsigned                                        regAliasTag;

   // Get cost model and standard values
   costModel = tile->CostModel;
   zeroCost = this->ZeroCost;

   // Reset the anti-preferred register vector and associated costs.
   registerAntiPreferenceVector->clear();

   // Use temporary register preference vector for calculating preferences of conflicting live ranges.
   temporaryRegisterPreferenceVector = (*this->TemporaryRegisterPreferenceVectors)[0];

   // Look at all the conflicting live ranges and their preferences and find the max anti preference
   // cost for each register.

   conflictGraph = tile->ConflictGraph;
   GraphColor::GraphIterator citer;
   GraphColor::LiveRange * conflictingLiveRange;

   // foreach_conflict_liverange
   for (conflictingLiveRange = conflictGraph->GetFirstConflictLiveRange(&citer, liveRange);
        (conflictingLiveRange != nullptr);
        conflictingLiveRange = conflictGraph->GetNextConflictLiveRange(&citer))
   {
      if (conflictingLiveRange->IsSpilled()) {
         continue;
      }

      // TODO: Remove SecondChanceGlobal opt out when anti pref opt out cost is complete.
      // Do computation if both live range and conflict live range are not second chance globals or
      // if search root is a second chance global.

      if (liveRange->IsSecondChanceGlobal || !conflictingLiveRange->IsSecondChanceGlobal) {
         antiPreferredReg = conflictingLiveRange->Register;

         if ((antiPreferredReg == VR::Constants::InitialPseudoReg) || Tiled::VR::Info::IsVirtualRegister(antiPreferredReg)) {
            //was: (antiPreferredReg->IsRegisterCategoryPseudo)
            temporaryRegisterPreferenceVector->clear();
            this->GetPreferredRegisters(tile, conflictingLiveRange, requiredReg, searchDepth,
               allocatableRegisterTagBitVector, temporaryRegisterPreferenceVector);
            this->CombineRegisterPreference(RegisterAllocator::PreferenceCombineMode::Max, costModel,
               registerAntiPreferenceVector, temporaryRegisterPreferenceVector);
         }
      }
   }

   // Look at all the preference live ranges and their conflicts and find the max anti preference
   // cost for each register is the set of preferred registers of the conflicting live ranges.

   preferenceGraph = tile->PreferenceGraph;
   GraphColor::LiveRange *   preferenceLiveRange;
   GraphColor::GraphIterator piter;

   // foreach_preference_liverange
   for (preferenceLiveRange = preferenceGraph->GetFirstPreferenceLiveRange(&piter, liveRange, &preferredCost, &preferenceConstraint);
        (preferenceLiveRange != nullptr);
        preferenceLiveRange = preferenceGraph->GetNextPreferenceLiveRange(&piter, &preferredCost, &preferenceConstraint))
   {
      preferredReg = preferenceLiveRange->Register;
      if ((preferredReg == VR::Constants::InitialPseudoReg) || Tiled::VR::Info::IsVirtualRegister(preferredReg)) {
         //was: (preferredReg->IsRegisterCategoryPseudo)

         // foreach_conflict_liverange
         for (conflictingLiveRange = conflictGraph->GetFirstConflictLiveRange(&citer, preferenceLiveRange);
              (conflictingLiveRange != nullptr);
              conflictingLiveRange = conflictGraph->GetNextConflictLiveRange(&citer))
         {
            // TODO: Remove SecondChanceGlobal opt out when anti pref opt out cost is complete.

            if (!conflictingLiveRange->IsSecondChanceGlobal) {
               temporaryRegisterPreferenceVector->clear();

               this->GetPreferredRegisters(tile, conflictingLiveRange, requiredReg, searchDepth,
                  allocatableRegisterTagBitVector, temporaryRegisterPreferenceVector);

               // Override the cost in vector to be the cost of this edge.
               this->SetRegisterPreferenceCost(temporaryRegisterPreferenceVector, costModel, preferredCost);

               this->CombineRegisterPreference(RegisterAllocator::PreferenceCombineMode::Add, costModel,
                  registerAntiPreferenceVector, temporaryRegisterPreferenceVector);
            }
         }
      }
   }

   // Calculate anti-preferred registers based on global cost.   Currently, if we have global live range
   // that spans a call, we don't even factor in the anti-preferences.

   registerCostVector = this->RegisterCostVector;

   llvm::SparseBitVector<>::iterator r;

   // foreach_sparse_bv_bit
   for (r = allocatableRegisterTagBitVector->begin(); r != allocatableRegisterTagBitVector->end(); ++r)
   {
      regAliasTag = *r;
      reg = this->VrInfo->GetRegister(regAliasTag);
      assert(Tiled::VR::Info::IsPhysicalRegister(reg));
      cost = (*registerCostVector)[reg];

      if (costModel->Compare(&cost, &zeroCost) != 0) {
         this->CombineRegisterPreference(RegisterAllocator::PreferenceCombineMode::Add,
            costModel, registerAntiPreferenceVector, reg, cost);
      }
   }

   // Remove anti-preferred registers from pruned allocatable set.

   for (unsigned index = 0; index < registerAntiPreferenceVector->size(); index++)
   {
      antiPreferredReg = ((*registerAntiPreferenceVector)[index]).Register;
      regAliasTag = this->VrInfo->GetTag(antiPreferredReg);

      antiPreferencePrunedAllocatableRegisterTagBitVector->reset(regAliasTag);
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Select the preferred register that maximizes preference benefit while
//    minimizing anti-preference cost.
//
// Arguments
//
//    costModel                    - cost model being used
//    registerPreferenceVector     - preferred registers and costs
//    registerAntiPreferenceVector - anti-preferred registers and costs
//
//------------------------------------------------------------------------------

unsigned
Allocator::SelectPreferredRegister
(
   GraphColor::LiveRange *                         liveRange,
   GraphColor::AllocatorCostModel *                costModel,
   Tiled::Cost *                                   outCost,
   GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector,
   GraphColor::PhysicalRegisterPreferenceVector *  registerAntiPreferenceVector
)
{
   Tiled::Cost           zeroCost = this->ZeroCost;
   unsigned              selectedPreferredReg = VR::Constants::InvalidReg;
   unsigned              summaryReg = VR::Constants::InvalidReg;
   Tiled::Cost           selectedPreferredCost = zeroCost;
   Tiled::Cost           selectedAntiPreferredCost = zeroCost;
   unsigned              preferredReg;
   Tiled::Cost           preferredCost;
   unsigned              antiPreferredReg;
   Tiled::Cost           antiPreferredCost;
   uint64_t              index;
   uint64_t              antiIndex;
   int                   compare;

   // If live range is summary proxy or was preferenced to a summary proxy (determined during search for
   // preferred registers), then prime the search with the register allocated to the proxy during
   // bottom up (if it is available).

   summaryReg = liveRange->SummaryRegister;

   if (summaryReg != VR::Constants::InvalidReg) {
      index = this->FindRegisterPreference(registerPreferenceVector, summaryReg);

      if (index != static_cast<uint64_t>(Allocator::Constants::IndexNotFound)) {
         selectedPreferredReg = ((*registerPreferenceVector)[index]).Register;
         selectedPreferredCost = ((*registerPreferenceVector)[index]).Cost;
      }

      if (selectedPreferredReg != VR::Constants::InvalidReg) {
         assert(summaryReg == selectedPreferredReg);
      } else {
         summaryReg = VR::Constants::InvalidReg;
      }
   }

   // Calculated most preferred register with the least anti-preferred cost.

   for (index = 0; index < registerPreferenceVector->size(); index++)
   {
      // Get preferred register and cost.
      preferredReg = ((*registerPreferenceVector)[index]).Register;
      preferredCost = ((*registerPreferenceVector)[index]).Cost;

      // Get anti-preferred register and cost.
      antiPreferredReg = preferredReg;
      antiIndex = this->FindRegisterPreference(registerAntiPreferenceVector,
                                               antiPreferredReg);
     
      antiPreferredCost =
        antiIndex == static_cast<uint64_t>(Allocator::Constants::IndexNotFound)
        ? zeroCost
        : ((*registerAntiPreferenceVector)[antiIndex]).Cost;


      // Select best cost/benefit trade-off.
      if (selectedPreferredReg == VR::Constants::InvalidReg) {
         selectedPreferredReg = preferredReg;
         selectedPreferredCost = preferredCost;
         selectedAntiPreferredCost = antiPreferredCost;
      } else {
         compare = costModel->Compare(&selectedPreferredCost, &preferredCost);

         // If this register has a better cost/benefit trade-off, then select it.
         if (compare < 0) {
            selectedPreferredReg = preferredReg;
            selectedPreferredCost = preferredCost;
            selectedAntiPreferredCost = antiPreferredCost;
         } else if (compare == 0) {
            // Benefit is the same, so pick the register with the least penalty.
            compare = costModel->Compare(&selectedAntiPreferredCost,
                                         &antiPreferredCost);

            if (compare > 0) {
               selectedPreferredReg = preferredReg;
               selectedAntiPreferredCost = antiPreferredCost;
            }

            // If this register has the same cost/benefit trade-off, then select the 
            // register with the lower id.  This mimics the selection process when
            // no preference is present.  Helps with consistency.

            else if ((compare == 0) && (selectedPreferredReg > preferredReg)) {
               // But, don't undo preferencing of summary register when present.
               if (selectedPreferredReg != summaryReg)
                  selectedPreferredReg = preferredReg;
            }
         }
      }
   }

   *outCost = selectedPreferredCost;

   return selectedPreferredReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compute preferred register out of the allocatable set of registers for 
//    the given live range.
//
// Arguments:
//
//    tile                            - Tile to preference with
//    liveRange                       - LiveRange to preference
//    allocatableRegisterTagBitVector - BitVector of allocatable register tags
//
//------------------------------------------------------------------------------

unsigned
Allocator::ComputePreference
(
   GraphColor::Tile *        tile,
   GraphColor::LiveRange *   liveRange,
   llvm::SparseBitVector<> * allocatableRegisterTagBitVector
)
{
   unsigned preferredReg = VR::Constants::InvalidReg;

   if (!allocatableRegisterTagBitVector->empty()) {

      Tiled::VR::Info *                               vrInfo = tile->VrInfo;
      GraphColor::AllocatorCostModel *                costModel;
      GraphColor::LiveRangeVector *                   preferenceLiveRangeVector;
      GraphColor::PhysicalRegisterPreferenceVector *  registerPreferenceVector;
      GraphColor::PhysicalRegisterPreferenceVector *  registerAntiPreferenceVector;
      llvm::SparseBitVector<> *                       antiPreferencePrunedAllocatableRegisterTagBitVector;
      unsigned                                        regAliasTag;
      int                                             searchDepth;
      unsigned                                        requiredReg;
      Tiled::Cost                                     registerCost = this->ZeroCost;

      // If we don't have a preference graph then just hand back the next available register.

      if (tile->PreferenceGraph == nullptr) {
         regAliasTag = allocatableRegisterTagBitVector->find_first();
         preferredReg = vrInfo->GetRegister(regAliasTag);

         if ((preferredReg != VR::Constants::InvalidReg) && liveRange->IsCalleeSaveValue) {
            Tiled::Cost       allocationCost = liveRange->AllocationCost;
            Tiled::Cost       bestSpillCost = liveRange->GetBestSpillCost();
            Tiled::Boolean    doAllocate;

            GraphColor::AllocatorCostModel * costModel = tile->CostModel;

            if (tile->Pass == GraphColor::Pass::Allocate) {
               doAllocate = (costModel->Compare(&bestSpillCost, &allocationCost) >= 0);

               if (!doAllocate)
                  // Clear reg if we need to spill.
                  preferredReg = VR::Constants::InvalidReg;
            }
         }

         return preferredReg;
      }

      if (liveRange->IsCalleeSaveValue && (liveRange->PreferenceEdgeCount == 0))
         // This is "no reg" case (a spill case).
         return VR::Constants::InvalidReg;

      // Get the constraints for the required register.

      requiredReg = liveRange->Register;
      assert(requiredReg == VR::Constants::InitialPseudoReg);

      // Get preference vectors for calculations.
      registerPreferenceVector = this->RegisterPreferenceVector;
      registerAntiPreferenceVector = this->RegisterAntiPreferenceVector;
      preferenceLiveRangeVector = tile->PreferenceLiveRangeVector;

      // Get copy of allocatable register.  We will prune this during anti-preference 
      // calculations.
      antiPreferencePrunedAllocatableRegisterTagBitVector = this->ScratchBitVector2;
      *antiPreferencePrunedAllocatableRegisterTagBitVector = *allocatableRegisterTagBitVector;

      // Get list of all directly and indirectly preferred live ranges.
      searchDepth = this->PreferenceSearchDepth;
      this->GetPreferredLiveRanges(tile, liveRange, searchDepth, preferenceLiveRangeVector);

      // Get preferred registers.

      searchDepth = this->PreferenceSearchDepth;
      this->GetPreferredRegisters(tile, liveRange, requiredReg, searchDepth, allocatableRegisterTagBitVector,
         registerPreferenceVector);

      // Get anti-preferred registers. 

      searchDepth = this->AntiPreferenceSearchDepth;
      this->GetAntiPreferredRegisters(tile, liveRange, requiredReg, preferenceLiveRangeVector,
         registerPreferenceVector, searchDepth, allocatableRegisterTagBitVector,
         antiPreferencePrunedAllocatableRegisterTagBitVector, registerAntiPreferenceVector);

      // Select the preferred register with most benefit and the least anti-preferred cost.
      costModel = tile->CostModel;
      preferredReg = this->SelectPreferredRegister(liveRange, costModel, &registerCost,
         registerPreferenceVector, registerAntiPreferenceVector);

      // Could not find a preferred register, get a register from the pruned allocatable set of registers.

      if (preferredReg == VR::Constants::InvalidReg) {
         regAliasTag = antiPreferencePrunedAllocatableRegisterTagBitVector->find_first();
         preferredReg = vrInfo->GetRegister(regAliasTag);
      }

      // Still could not find a preferred register.  Use the most acceptable anti-preferred register.

      if (preferredReg == VR::Constants::InvalidReg) {
         preferredReg = this->GetLeastAntiPreferredRegister(liveRange, costModel, &registerCost,
            registerAntiPreferenceVector);
      }

      // Still could not find a register, just get the next available register.

      if (preferredReg == VR::Constants::InvalidReg) {
         regAliasTag = allocatableRegisterTagBitVector->find_first();
         preferredReg = vrInfo->GetRegister(regAliasTag);
      }

      // Do a final check to see if we want to allocate.

      if (preferredReg != VR::Constants::InvalidReg) {
         Tiled::Cost allocationCost = liveRange->AllocationCost;
         Tiled::Cost bestSpillCost = liveRange->GetBestSpillCost();
         bool        doAllocate;

         GraphColor::AllocatorCostModel * costModel = tile->CostModel;

         if (tile->Pass == GraphColor::Pass::Allocate) {
            // Check if we need to use register costs (conservative selection).  Also opt out for callee save
            // values since getting the allocation removes the push/pop cost (that's what they model)

            if (tile->DoUseRegisterCosts && !liveRange->IsCalleeSaveValue && liveRange->HasSpillPartition) {
               Tiled::Cost registerCost = (*this->RegisterCostVector)[preferredReg];

               // Increase allocation cost by current register cost if we are under a conservative regime.
               allocationCost.IncrementBy(&registerCost);
            }

            //was: !(liveRange->IsAlsaValue && (tile->Iteration > 1))
            doAllocate = (costModel->Compare(&bestSpillCost, &allocationCost) >= 0);
         } else {

            // During top down assignment change the bias to spilling.
            doAllocate = (costModel->Compare(&bestSpillCost, &allocationCost) > 0);

            assert(doAllocate || !bestSpillCost.IsInfinity());

            // Generalize the preference opt out for all live ranges.  Today we only handle
            // second chance globals that fail their preference and has an anti preference
            // for the allocatable register.

            Tiled::Cost  mostCost;
            unsigned     mostPreferredRegister
               = this->GetMostPreferredRegister(costModel, &mostCost, registerPreferenceVector);

            if (doAllocate && liveRange->IsSecondChanceGlobal && (preferredReg != mostPreferredRegister)) {
               // Didn't make the second chance preference.  Only take the allocatable register if it has no
               // anti-preference. (it's not being taken from somebody else)

               int index = this->FindRegisterPreference(registerAntiPreferenceVector, preferredReg);

               if (index != static_cast<int>(Allocator::Constants::IndexNotFound)) {
                  doAllocate = false;
               }
            }
         }

         if (!doAllocate)
            preferredReg = VR::Constants::InvalidReg;
      }
   }

   return preferredReg;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Number of registers that can be allocated for this live range category
//
// Arguments:
//
//    tile      - tile context
//    liveRange - live range allocating
//
//------------------------------------------------------------------------------

unsigned
Allocator::NumberAllocatableRegisters
(
   GraphColor::LiveRange * liveRange
)
{
   return (this->GetCategoryRegisters(liveRange))->count();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate trivial live range given vector of operands and conflict registers and registers live across
//    live range.
//
//------------------------------------------------------------------------------

void
Allocator::AllocateTrivialLiveRange
(
   GraphColor::Tile *           tile,
   GraphColor::LiveRange *      liveRange,
   GraphColor::OperandVector *  operandVector,
   llvm::SparseBitVector<> *    conflictBitVector,
   llvm::SparseBitVector<> *    liveBitVector
)
{
#ifdef FUTURE_IMPL
   Tiled::Boolean wasAllocated = false;

   Assert(liveRange->IsTrivial);
   Assert(!operandVector->IsEmpty);

   if (!liveRange->IsPreColored)
   {
      // Get allocatable register set.  

      BitVector::Sparse ^ allocatableRegisterTagBitVector;

      allocatableRegisterTagBitVector = this->GetAllocatableRegisters(tile, liveRange);

      // Add registers that are currently live across trivial live range
      // to conflict set.

      conflictBitVector->Or(liveBitVector);

      // Remove conflicting live registers from allocatable register set.

      Alias::Tag    registerTag;
      Alias::Info ^ aliasInfo = tile->AliasInfo;

      foreach_sparse_bv_bit(registerTag, conflictBitVector)
      {
         if (aliasInfo->IsRegisterTag(registerTag))
            aliasInfo->MinusMayPartialTags(registerTag, allocatableRegisterTagBitVector);
      }
      next_sparse_bv_bit;

      // If register is available, allocate it to the live range and rewrite all operands
      // in live range.

      if (!allocatableRegisterTagBitVector->IsEmpty)
      {
         Alias::Info ^             aliasInfo = this->AliasInfo;
         IR::Operand ^             operand;
         Registers::Register ^     reg;
         Alias::Tag                regAliasTag;
         BitVector::SparsePosition position;
         GraphColor::LiveRange ^   operandLiveRange;

         // Get the next available register.

         regAliasTag = allocatableRegisterTagBitVector->GetFirstBit(&position);
         aliasInfo = tile->AliasInfo;
         reg = aliasInfo->GetRegister(regAliasTag);

         // Allocate the register to the live range.

         this->Allocate(tile, liveRange, reg);

         // Rewrite all operands that are appearances of this live range.
         // At the same time, remove operands for this live range from operand vector.

         Tiled::UInt32 count = 0;
         Tiled::UInt32 liveRangeAppearanceCount = 0;

         for (Tiled::UInt32 index = 0; index < operandVector->Count(); index++)
         {
            operand = operandVector->Item[index];
            operandLiveRange = tile->GetLiveRange(operand);

            if (liveRange == operandLiveRange)
            {
               this->RewriteRegister(operand, reg);
               liveRangeAppearanceCount++;
               wasAllocated = true;
            }
            else
            {
               operandVector->Item[count++] = operand;
            }
         }
         operandVector->Resize(count);

         // We had to allocate at least one appearance.

         Assert(liveRangeAppearanceCount > 0);
      }
   }

   // If we couldn't allocate the trivial live range clean up vector operand
   // and possibly modify live range to not be trivial anymore.

   if (!wasAllocated)
   {
      // Remove operands for this live range from operand vector.

      Tiled::UInt32 count = 0;
      Tiled::UInt32 liveRangeAppearanceCount = 0;

      for (Tiled::UInt32 index = 0; index < operandVector->Count(); index++)
      {
         IR::Operand ^           operand = operandVector->Item[index];
         GraphColor::LiveRange ^ operandLiveRange = tile->GetLiveRange(operand);

         if (liveRange == operandLiveRange)
         {
            liveRangeAppearanceCount++;
         }
         else
         {
            operandVector->Item[count++] = operand;
         }
      }
      operandVector->Resize(count);

      // There had to be at least one appearance.

      Assert(liveRangeAppearanceCount > 0);

      // We couldn't allocate the trivial live range and if it is more than just a 
      // point appearance mark it as non-trivial.   This will give us an opportunity 
      // to spill the live range if necessary.

      if (liveRangeAppearanceCount > 1)
      {
         liveRange->IsTrivial = false;
      }
   }
#endif // FUTURE_IMPL
   llvm_unreachable("NYI");
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Allocate trivial live ranges to reduce the overall number of live ranges.
//
// Remarks:
//
//    Allocate trivial live ranges.
//
//    Simple:
//
//    t1 =
//       = op t1, ...
//
//    Compound:
//
//    t1 =
//    t1 = op t1, ...
//    t1 = op t1, ...
//       = t1
//
// Arguments:
//
//    tile - allocate trivial live ranges on this tile
//
//------------------------------------------------------------------------------

void
Allocator::AllocateTrivialLiveRanges
(
   GraphColor::Tile * tile
)
{
   GraphColor::Liveness *             liveness = this->Liveness;
   GraphColor::OperandVector *        operandVector = new GraphColor::OperandVector();
   operandVector->reserve(256);
   llvm::SparseBitVector<> *          liveBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *          genBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *          killBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *          conflictBitVector = new llvm::SparseBitVector<>();
   GraphColor::LiveRangeVector *      killLiveRangeVector = new GraphColor::LiveRangeVector();
   killLiveRangeVector->reserve(256);
   GraphColor::LiveRangeVector *      liveLiveRangeVector = new GraphColor::LiveRangeVector();
   liveLiveRangeVector->reserve(256);

   // Process each block working backwards through the function tracking liveness and
   // allocating trivial live ranges as they are seen.

   Graphs::MachineBasicBlockVector::reverse_iterator biter;
   Graphs::MachineBasicBlockVector * mbbVector = tile->BlockVector;

   // foreach_block_in_tile_backward
   for (biter = mbbVector->rbegin(); biter != mbbVector->rend(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;

      // Initialize live out register set for the block.
      Dataflow::LivenessData *   registerLivenessData = liveness->GetRegisterLivenessData(block);
      llvm::SparseBitVector<> *  liveOutBitVector = registerLivenessData->LiveOutBitVector;

      *liveBitVector = *liveOutBitVector;

      // Process instructions backwards tracking register liveness and trivial live ranges.
      // Allocate trivial live ranges as they are seen.

      llvm::MachineBasicBlock::reverse_instr_iterator rii;

      // foreach_instr_in_block_backward
      for (rii = block->instr_rbegin(); rii != block->instr_rend(); ++rii)
      {
         llvm::MachineInstr * instruction = &(*rii);

         // Process trivial live ranges on destination list.

         llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(),
                                                                       instruction->defs().end());
         llvm::MachineInstr::mop_iterator destinationOperand;

         // foreach_register_destination_opnd
         for (destinationOperand = drange.begin(); destinationOperand != drange.end(); ++destinationOperand)
         {
            if (destinationOperand->isReg()) {
               GraphColor::LiveRange * destinationLiveRange = tile->GetLiveRange(destinationOperand);

               if (destinationLiveRange != nullptr) {

                  if (destinationLiveRange->IsTrivial) {
                     assert(std::find(killLiveRangeVector->begin(), killLiveRangeVector->end(), destinationLiveRange) == killLiveRangeVector->end());
                     killLiveRangeVector->push_back(destinationLiveRange);
                     liveLiveRangeVector->erase(std::remove(liveLiveRangeVector->begin(), liveLiveRangeVector->end(), destinationLiveRange),
                                                liveLiveRangeVector->end());
                     operandVector->push_back(destinationOperand);
                  }
               }
            }
         }

         // Process trivial live ranges on source list.

         llvm::MachineInstr::mop_iterator sourceOperand;
         llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                                     instruction->explicit_operands().end());
         // foreach_register_source_opnd
         for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
         {
            if (sourceOperand->isReg()) {
               GraphColor::LiveRange * sourceLiveRange = tile->GetLiveRange(sourceOperand);

               if (sourceLiveRange != nullptr) {
                  if (sourceLiveRange->IsTrivial) {
                     if (std::find(liveLiveRangeVector->begin(), liveLiveRangeVector->end(), sourceLiveRange) == liveLiveRangeVector->end()) {
                        liveLiveRangeVector->push_back(sourceLiveRange);
                     }
                     killLiveRangeVector->erase(std::remove(killLiveRangeVector->begin(), killLiveRangeVector->end(), sourceLiveRange),
                                                killLiveRangeVector->end());
                     operandVector->push_back(sourceOperand);
                  }
               }
            }
         }

         // Keep track of live registers so they can be excluded from allocatable register set.

         liveness->TransferInstruction(instruction, genBitVector, killBitVector);
         liveness->UpdateInstruction(instruction, liveBitVector, genBitVector, killBitVector);

         // Allocate live ranges terminated by this instruction.

         while (!killLiveRangeVector->empty())
         {
            GraphColor::LiveRange * liveRange = killLiveRangeVector->back();
            killLiveRangeVector->pop_back();

            // Calculate conservative conflicting registers.

            unsigned               firstIndex = 0;
            unsigned               lastIndex = operandVector->size() - 1;
            llvm::MachineInstr *   firstInstruction = ((*operandVector)[lastIndex])->getParent();
            llvm::MachineInstr *   lastInstruction = ((*operandVector)[firstIndex])->getParent();

            conflictBitVector->clear();

            // If this is a single instruction live range, it is a point lifetime.

            if (lastInstruction == firstInstruction) {
               liveness->TransferInstruction(firstInstruction, genBitVector, killBitVector);
               *conflictBitVector |= *killBitVector;
            }
            // Otherwise normal, multi instruction live ranges.

            else {
               // Add conflicts for kill on first instruction.
               liveness->TransferInstruction(firstInstruction, genBitVector, killBitVector);
               *conflictBitVector |= *killBitVector;

               // Add conflicts for gens on last instruction.
               liveness->TransferInstruction(lastInstruction, genBitVector, killBitVector);
               *conflictBitVector |= *genBitVector;

               // Add conflicts for intervening instructions.
               if (firstInstruction->getNextNode() != lastInstruction) {

              // foreach_instr_in_range_backward
              llvm::MachineInstr * instruction = lastInstruction->getPrevNode();
              for ( ; instruction != firstInstruction; instruction = instruction->getPrevNode())
                  {
                     liveness->TransferInstruction(instruction, genBitVector, killBitVector);
                     *conflictBitVector |= *genBitVector;
                     *conflictBitVector |= *killBitVector;
                  }
               }
            }

            // Allocate trivial live range.

            this->AllocateTrivialLiveRange(tile, liveRange, operandVector, conflictBitVector, liveBitVector);
         }
      }
   }

   // Make sure we allocated trivial live ranges and their appearances.

   assert(killLiveRangeVector->empty());
   assert(liveLiveRangeVector->empty());
   assert(operandVector->empty());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Clear pass 1 allocations for reallocation.
//
// Arguments:
//
//    tile - tile context
//
//------------------------------------------------------------------------------

void
Allocator::ClearAllocations
(
   GraphColor::Tile * tile
)
{
   tile->ClearAllocations();
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeCalleeSaveHeuristic - Inspect the function and decide on a callee save heuristic
//
//------------------------------------------------------------------------------

void
Allocator::ComputeCalleeSaveHeuristic()
{
   Graphs::FlowGraph *     flowGraph                = this->FunctionUnit;
   llvm::MachineFunction * MF                       = flowGraph->machineFunction;
   bool                    conservativeCallCount    = false;
   bool                    conservativeBlockSize    = true;
   bool                    conservativeCallCrossing = false;
   bool                    conservativeGlobalLiveRange = false;
   bool                    isCallTreeEntryPoint     = false;

   //TODO: If this function (MF) is the program entry point set:   isCallTreeEntryPoint = true;

   // Determine if we're using register costs for conservative selection.

   unsigned  globalLiveRangeConservativeThreshold = this->GlobalLiveRangeConservativeThreshold;
   if (this->GlobalLiveRangeCount() <= globalLiveRangeConservativeThreshold) {
      conservativeGlobalLiveRange = true;
   }

   unsigned  calleeSaveConservativeThreshold = this->CalleeSaveConservativeThreshold;
   if (this->NumberGlobalCallSpanningLiveRanges <= calleeSaveConservativeThreshold) {
      conservativeCallCrossing = true;
   }

   // If global call count is below the call count threshold we can go conservative.
   if (this->NumberCalls < this->CallCountConservativeThreshold) {
      conservativeCallCount = true;
   }

   // Check each block in the function and opt out of conservative mode if there is a fat block.
   // A fat block currently means instruction count > 50.

   llvm::MachineFunction::iterator b;

   // foreach_block_in_func
   for (b = MF->begin(); b != MF->end(); ++b) {
      llvm::MachineBasicBlock * block = &*b;
      if (this->IsLargeBlock(block)) {
         conservativeBlockSize = false;
      }
   }

   this->DoUseRegisterCosts
      = (!isCallTreeEntryPoint && conservativeGlobalLiveRange
      && conservativeCallCrossing && conservativeCallCount && conservativeBlockSize);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    ComputeCalleeSaveHeuristic - Inspect the tile and do and over ride the global callee save heuristic if
//    it is appropriate.
//
// Arguments:
//
//    tile - tile context
//
//------------------------------------------------------------------------------

void
Allocator::ComputeCalleeSaveHeuristic
(
   GraphColor::Tile * tile
)
{
   bool                         globalDoUseRegisterCosts = this->DoUseRegisterCosts;
   bool                         conservativeCallCrossing = globalDoUseRegisterCosts;
   bool                         doUseRegisterCosts = globalDoUseRegisterCosts;
   GraphColor::SpillOptimizer * spillOptimizer = this->SpillOptimizer;

   // Opt out of callee save heuristic at half the spill share limit.

   unsigned iterationLimit = (spillOptimizer->SpillShareIterationLimit / 2);

   if (tile->Iteration > iterationLimit) {
      // Go aggressive if we don't seem to be closing.

      doUseRegisterCosts = false;
   }

   GraphColor::Allocator * allocator = tile->Allocator;  // ever != this  ?

   if (Tiled::CostValue::CompareGT(tile->Frequency, allocator->EntryProfileCount)) {
      doUseRegisterCosts = false;
   }

   // Determine if we're using register costs for conservative selection.

   unsigned calleeSaveConservativeThreshold = this->CalleeSaveConservativeThreshold;

   // Over ride global decision if we find a high number of local call crossing live ranges within the tile.

   if (tile->NumberCallSpanningLiveRanges > calleeSaveConservativeThreshold) {
      conservativeCallCrossing = false;
   }

   doUseRegisterCosts = (doUseRegisterCosts && conservativeCallCrossing);

   // Set up tile to be conservative with respect to callee save.
   tile->DoUseRegisterCosts = doUseRegisterCosts;

   if (!doUseRegisterCosts && globalDoUseRegisterCosts) {
      // If we're switching the local tile heuristic and the tile head block dominates the exit override the
      // global decision.

      Graphs::FlowGraph *       flowGraph = this->FunctionUnit;
      llvm::MachineFunction *   MF = flowGraph->machineFunction;
      llvm::MachineBasicBlock * exitBlock = nullptr;
      llvm::MachineFunction::reverse_iterator rbb;
      for (rbb = MF->rbegin(); rbb != MF->rend(); ++rbb)
      {
         if (rbb->isReturnBlock()) {
            exitBlock = &(*rbb);
            break;
         }
      }
      assert(exitBlock != nullptr);

      llvm::MachineBasicBlock * headBlock = tile->HeadBlock;

      if (headBlock != nullptr && flowGraph->Dominates(headBlock, exitBlock)) {
         // Override global decision with tile decision.  Logic for this is that if the tile dominates the
         // exit (this is a straight line function) and we go optimistic in this tile there's no benefit to
         // going conservative in the parent since there is no way to avoid compensation spills based on the
         // different heuristics.

         this->DoUseRegisterCosts = doUseRegisterCosts;
      }
   }

}

//------------------------------------------------------------------------------
//
// Description:
//
//    IsLargeBlock - Test a block to see if it meets heuristic threshold for "large".
//
// Arguments:
//
//    block - Block to Test.
//
// Notes:
//
//    Logically this is a weighted count of instructions.
//
// Returns:
//
//   True if block is heuristically large.
//
//------------------------------------------------------------------------------

bool
Allocator::IsLargeBlock
(
   llvm::MachineBasicBlock * block
)
{
   TargetRegisterAllocInfo * targetRegisterAllocator = this->TargetRegisterAllocator;

   return targetRegisterAllocator->IsLargeBlock(block);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    IsFatBlock - Test a block to see if it is likely to require a large number of registers or spill
//    optimization.  Rough measure of the local allocation complexity.
//
// Arguments:
//
//    block - Block to Test.
//
// Notes:
//
//    A fat block is a heuristically large block with a max kill of some register category.
//
// Returns:
//
//   True if block is fat (complex).
//
//------------------------------------------------------------------------------

bool
Allocator::IsFatBlock
(
   llvm::MachineBasicBlock * block
)
{
   if (this->IsLargeBlock(block)) {
      return true;
   }

   return false;
}


unsigned
Allocator::GlobalLiveRangeCount()
{
   return (this->GlobalLiveRangeVector->size() - 1);
}

void
Allocator::BeginBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile
)
{
   this->SpillOptimizer->BeginBlock(block, tile, false);
}

void
Allocator::BeginBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   const bool                doInsert
)
{
   this->SpillOptimizer->BeginBlock(block, tile, false);
}

void
Allocator::EndBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile
)
{
   this->SpillOptimizer->EndBlock(block, tile, false);
}

void
Allocator::EndBlock
(
   llvm::MachineBasicBlock * block,
   GraphColor::Tile *        tile,
   bool                      doInsert
)
{
   this->SpillOptimizer->EndBlock(block, tile, doInsert);
}

const llvm::TargetRegisterClass *
Allocator::GetRegisterCategory
(
   unsigned Reg
)
{
   return this->TargetRegisterAllocator->GetRegisterCategory(Reg);
}


} // GraphColor
} // RegisterAllocator
} // Tiled

