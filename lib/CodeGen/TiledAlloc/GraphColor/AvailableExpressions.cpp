//===-- GraphColor/AvailableExpressions.cpp ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AvailableExpressions.h"
#include "Allocator.h"
#include "LiveRange.h"
#include "Liveness.h"
#include "llvm/Support/MathExtras.h"

namespace Tiled
{

namespace Expression
{

Expression::Value *
Value::New
(
   Expression::Table * table,
   unsigned            hashCode,
   unsigned            valueId
)
{
   Expression::Value * value = new Value();

   value->HashCode = hashCode;

   // unique identifier for this value suitable for use in a sparse bit vector
   value->Id = valueId;
   value->HasChanged = false;
   value->Table = table;

   return value;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Append the given expression occurrence into the value class.
//
// Arguments:
//
//    occurrence - Occurrence to append to the value
//
//------------------------------------------------------------------------------

void
Value::Append
(
   Expression::Occurrence * occurrence
)
{
   Expression::Value * value = this;

   assert(occurrence->Next == nullptr);
   assert(occurrence->Previous == nullptr);
   assert(occurrence->Value == nullptr);

   if (value->FirstOccurrence == nullptr) {
      // Make this occurrence value the class leader.

      value->FirstOccurrence = occurrence;
      value->LastOccurrence = occurrence;
   } else {
      // Append this onto the end of the value occurrence list thereby
      // keeping the occurrences in order of appearance.

      Expression::Occurrence * lastOccurrence = value->LastOccurrence;

      lastOccurrence->Next = occurrence;
      occurrence->Previous = lastOccurrence;
      value->LastOccurrence = occurrence;
   }

   occurrence->Value = value;
}

Expression::Occurrence *
Value::FindOccurrence
(
   llvm::MachineOperand * operand
)
{
   Expression::Value * value = this;
   Expression::Occurrence * occurrence;

   // foreach_occurrence_of_value
   for (occurrence = value->FirstOccurrence; occurrence != nullptr; occurrence = occurrence->Next)
   {
      // Search all occurrences in this value for the requested one.
      if (occurrence->Operand == operand) {
         return occurrence;
      }
   }

   return nullptr;
}

void
Value::Remove
(
   Expression::Occurrence * occurrence
)
{
   Expression::Value * value = this;

   value->Unlink(occurrence);

   occurrence->Value = value;  // needed only when put on a free list

   delete occurrence;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Unlink the given expression occurrence from the value class.
//
// Arguments:
//
//    occurrence - Unlink this occurrence
//
// Remarks:
//
//    The given occurrence is not deleted, only unlinked.
//
//------------------------------------------------------------------------------

void
Value::Unlink
(
   Expression::Occurrence * occurrence
)
{
   Expression::Value *  value = this;
   Expression::Table *  table = this->Table;
   unsigned             id = occurrence->ScratchId;

   assert(occurrence != nullptr);
   assert(occurrence->Value == value);
   assert((occurrence->Operand == nullptr)
      || (occurrence->Operand != value->DominatingOperand));

   //table->OccurrenceMap->Remove(id, occurrence);
   table->OccurrenceMap->erase(id);

   if (value->FirstOccurrence == occurrence) {
      if (value->LastOccurrence == occurrence) {
         value->FirstOccurrence = nullptr;
         value->LastOccurrence = nullptr;
      } else {
         Expression::Occurrence * nextOccurrence = occurrence->Next;

         value->FirstOccurrence = nextOccurrence;
         nextOccurrence->Previous = nullptr;
      }

   } else if (value->LastOccurrence == occurrence) {
      Expression::Occurrence * prevOccurrence = occurrence->Previous;

      prevOccurrence->Next = nullptr;
      value->LastOccurrence = prevOccurrence;

   } else {
      Expression::Occurrence * prevOccurrence = occurrence->Previous;
      Expression::Occurrence * nextOccurrence = occurrence->Next;

      prevOccurrence->Next = nextOccurrence;
      nextOccurrence->Previous = prevOccurrence;
   }

   if (value->getLeaderOccurrence() == occurrence) {
      // The leader occurrence has been removed.

      if (value->isEmpty()) {
         value->LeaderOccurrence = nullptr;
      } else {
         // Choose another leader.
         value->LeaderOccurrence = value->FirstOccurrence;
      }
   }

   // null out list linkage
   occurrence->Next = nullptr;
   occurrence->Previous = nullptr;
   occurrence->Value = nullptr;
}

void
Value::RemoveAll()
{
   Expression::Value * value = this;

   // Iterate through all expression occurrences in this class.

   while (!value->isEmpty())
   {
      Expression::Occurrence * occurrence = value->FirstOccurrence;

      value->Remove(occurrence);
   }
}


llvm::MachineOperand *
Value::getLeaderOperand()
{
   return (this->hasLeader() ? this->getLeaderOccurrence()->Operand : nullptr);
}


Expression::Occurrence *
Occurrence::New
(
   Expression::Value *    value,
   unsigned               scratchId,
   llvm::MachineOperand * operand
)
{
   Expression::Occurrence * occurrence = new Occurrence();

   occurrence->Operand = operand;

   occurrence->ScratchId = scratchId;

   return occurrence;
}


Expression::Table *
Table::New
(
   unsigned hashSize
)
{
   Expression::Table * table = new Expression::Table() ;

   assert(llvm::isPowerOf2_32(hashSize));

   table->Size = hashSize;
   table->ValueMap = new Expression::ExpressValueVector(hashSize, nullptr);
   table->OccurrenceMap = new Expression::IdToOccurrenceMap();

   return table;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Search the expression table for the expression occurrence with
//    the given Id.
//
// Arguments:
//
//    id - Unique expression id
//
// Returns:
//
//    The expression table Occurrence * entry.
//
//------------------------------------------------------------------------------

Expression::Occurrence *
Table::Lookup
(
   unsigned id
)
{
   Expression::Table * table = this;
   assert(table->OccurrenceMap != nullptr);
   Expression::Occurrence * occurrence = nullptr;

   Expression::IdToOccurrenceMap::iterator i = this->OccurrenceMap->find(id);
   if (i != this->OccurrenceMap->end()) {
      occurrence = i->second;
   }

   return occurrence;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Lookup the expression table for an equivalent expression using
//    the given hash code and expression tree.
//
// Arguments:
//
//    hashCode - Hash code
//    instruction - Instruction to lookup
//    compareControl - Compare control
//
// Remarks:
//
//    This version looks up an instruction in the table without specifying
//    a destination operand result of the instruction.
//
// Returns:
//
//    The expression table Value * entry.
//
//------------------------------------------------------------------------------

Expression::Value *
Table::Lookup
(
   unsigned                    hashCode,
   llvm::MachineInstr *        instruction,
   Expression::CompareControl  compareControl
)
{
   Expression::Table * table = this;
   unsigned            bucket = hashCode & (table->Size - 1);
   Expression::Value * lastValue = nullptr;

   for (Expression::Value * value = (*table->ValueMap)[bucket]; value != nullptr; value = value->Next)
   {
      //TODO: HasChanged is initialized to false, it will have to be set (possibly to true) when
      //     registers, other than FP, will be allowed as arguments of NeverKilled instructions.
      if ((hashCode == value->HashCode) && !value->HasChanged) {
         llvm::MachineOperand * leaderOperand = value->getLeaderOperand();

         // There must be a leader occurrence unless the value has
         // changed and the leader has already left the class.
         assert(leaderOperand != nullptr);

         // Perform the expensive process of matching expressions.

         if (leaderOperand->isDef() && Table::Compare(instruction, leaderOperand->getParent(), compareControl)) {
            if (lastValue != nullptr) {
               // Promote this expression node to the head of the list.
               lastValue->Next = value->Next;
               value->Next = (*table->ValueMap)[bucket];
               (*table->ValueMap)[bucket] = value;
            }

            return value;
         }
      }

      lastValue = value;
   }

   return nullptr;
}

bool
Table::Compare
(
   llvm::MachineInstr *        instruction1,
   llvm::MachineInstr *        instruction2,
   Expression::CompareControl  compareControl
)
{
   return llvm::MachineInstrExpressionTrait::isEqual(instruction1, instruction2);
   //note: the implementation uses the MachineInstr::IgnoreVRegDefs control
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert the given instruction into the hashtable. This expression
//    will be put into an equivalence class with any other equivalent
//    expressions that are already in the table. If there are no other
//    expressions in the table then this expression is made the
//    beginning of a new equivalence group.
//
// Arguments:
//
//    id - Expression identifier
//    instruction - Expression instruction
//    compareControl - Compare control
//
// Returns:
//
//    Expression::Occurrence *
//
//------------------------------------------------------------------------------

Expression::Occurrence *
Table::InsertIfNotFound
(
   unsigned                    id,
   llvm::MachineInstr *        instruction,
   Expression::CompareControl  compareControl
)
{
   Expression::Table * table = this;

   // Compute the hashing code for this operand.
   unsigned hashCode = llvm::MachineInstrExpressionTrait::getHashValue(instruction);

   // Search or make a new occurrance in the hash table.

   Expression::Occurrence * occurrence =
      table->InsertIfNotFound(id, hashCode, instruction, compareControl);

   return occurrence;

}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert the given instruction into the hashtable using the given
//    hash code. This expression will be put into an equivalence class
//    with any other equivalent expressions that are already in the
//    table. If there are no other expressions in the table then this
//    expression is made the beginning of a new equivalence group.
//
// Arguments:
//
//    id - Expression identifier
//    hashCode - Hash code
//    instruction - Instruction to hash
//    compareControl - Compare control
//
// Returns:
//
//    Expression::Occurrence *
//
//------------------------------------------------------------------------------

Expression::Occurrence *
Table::InsertIfNotFound
(
   unsigned                   id,
   unsigned                   hashCode,
   llvm::MachineInstr *       instruction,
   Expression::CompareControl compareControl
)
{
   Expression::Table * table = this;

   // Search for an equivalent value already in the table.

   Expression::Value * value = table->Lookup(hashCode, instruction, compareControl);

   // Now find or make the occurrence within that value class.

   Expression::Occurrence * occurrence =
      table->NewOccurrenceIfNotFound(value, id, hashCode, instruction->defs().begin());

   return occurrence;
}

Expression::Occurrence *
Table::NewOccurrenceIfNotFound
(
   Expression::Value *     value,
   unsigned                id,
   unsigned                hashCode,
   llvm::MachineOperand *  operand
)
{
   Expression::Table *      table = this;
   Expression::Occurrence * occurrence;

   if (value == nullptr) {
      // Insert a new unique expression into the table.
      occurrence = table->InsertUnique(id, hashCode, operand);

   } else {
      // Search for the existing operand occurrence in the class.
      occurrence = value->FindOccurrence(operand);

      if (occurrence == nullptr) {
         // Allocate the new expression and make equivalent.
         occurrence = Expression::Occurrence::New(value, id, operand);

         value->Append(occurrence);

         Expression::IdToOccurrenceMap::value_type entry(id, occurrence);
         std::pair<Expression::IdToOccurrenceMap::iterator, bool> result =
            table->OccurrenceMap->insert(entry);

         if (!(result.second)) {
            (result.first)->second = occurrence;  //overwrite
         }
      }
   }

   return occurrence;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Insert the given unique hash value into the hashtable using the
//    given hash code and expression identifier. This interface will
//    allocate the corresponding expression node.
//
// Arguments:
//
//    id - Occurrence identifier
//    hashCode - Hash code code
//    operand - Expression operand
//
// Returns:
//
//    Expression:Occurrence *
//
//------------------------------------------------------------------------------

Expression::Occurrence *
Table::InsertUnique
(
   unsigned                id,
   unsigned                hashCode,
   llvm::MachineOperand *  operand
)
{
   Expression::Table * table = this;
   unsigned            nextValueId = this->NextValueId++;
   unsigned            bucket = hashCode & (this->Size - 1);

   // Allocate a new expression class entry.
   Expression::Value * value = Value::New(table, hashCode, nextValueId);

   // Allocate a new expression value entry.
   Expression::Occurrence * occurrence = Occurrence::New(value, id, operand);

   value->Append(occurrence);

   // Setup the value leader representative.
   value->LeaderOccurrence = occurrence;

   // Insert it into the front of the table chain.
   value->Next = (*table->ValueMap)[bucket];
   (*table->ValueMap)[bucket] = value;

   Expression::IdToOccurrenceMap::value_type entry(id, occurrence);
   std::pair<Expression::IdToOccurrenceMap::iterator, bool> result =
      table->OccurrenceMap->insert(entry);
   assert(result.second);

   return occurrence;
}

bool
Table::Remove
(
   Expression::Value * value
)
{
   Expression::Table * table = this;
   unsigned            bucket = value->HashCode & (table->Size - 1);
   Expression::Value * lastValue = nullptr;

   // Do remove all occurrences.

   if (value->FirstOccurrence != nullptr) {
      value->RemoveAll();
   }

   for (Expression::Value * thisValue = (*table->ValueMap)[bucket]; thisValue != nullptr; thisValue = thisValue->Next)
   {
      if (thisValue == value) {
         // We found the value so remove it from the table.

         if (lastValue == nullptr) {
            (*table->ValueMap)[bucket] = thisValue->Next;
         } else {
            lastValue->Next = thisValue->Next;
         }

         return true;
      }

      lastValue = thisValue;
   }

   return false;
}

}

namespace RegisterAllocator
{
namespace GraphColor
{

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for AvailableExpressions object.
//
// Arguments:
//
//    tile - context for this available expressions object
//
// Returns:
//
//    AvailableExpression object
//
//------------------------------------------------------------------------------

GraphColor::AvailableExpressions *
AvailableExpressions::New
(
   GraphColor::Tile * tile
)
{
   GraphColor::Allocator *            allocator = tile->Allocator;
   GraphColor::AvailableExpressions * available = new GraphColor::AvailableExpressions();

   // Create defined bit vector early and in the tile graph lifetime so that defined information can be
   // propagated between tiles.

   llvm::SparseBitVector<> *  definedInTileTagsBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  scratchBitVector = new llvm::SparseBitVector<>();

   available->Allocator = allocator;
   available->Tile = tile;
   available->DefinedInTileTagsBitVector = definedInTileTagsBitVector;
   available->ScratchBitVector = scratchBitVector;
   available->AA = allocator->AA;

   return available;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for AvailableExpressions object.
//
// Arguments:
//
//    allocator - global context for this available expressions object
//
// Returns:
//
//    AvailableExpression object
//
//------------------------------------------------------------------------------

GraphColor::AvailableExpressions *
AvailableExpressions::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::AvailableExpressions * available = new GraphColor::AvailableExpressions();

   llvm::SparseBitVector<> *  definedInTileTagsBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  scratchBitVector = new llvm::SparseBitVector<>();

   available->Allocator = allocator;
   available->DefinedInTileTagsBitVector = definedInTileTagsBitVector;
   available->ScratchBitVector = scratchBitVector;
   available->AA = allocator->AA;

   return available;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize per-iteration data underlying the available expressions analysis
//
//------------------------------------------------------------------------------

void
AvailableExpressions::Initialize
(
   GraphColor::Tile * tile
)
{
   assert((tile != nullptr) && (tile == this->Tile));

   //Tiled::FunctionUnit * functionUnit = tile->FunctionUnit;
   //Expression::Info *  expressionInfo = functionUnit->ExpressionInfo;

   // Create an expression table to compute available expressions for the allocator.

   Expression::Table * expressionTable = Expression::Table::New(256);

   llvm::SparseBitVector<> *  availableBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  multipleShapeBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  escapedGlobalBitVector = new llvm::SparseBitVector<>();

   this->ExpressionTable = expressionTable;
   this->AvailableBitVector = availableBitVector;
   this->MultipleShapeBitVector = multipleShapeBitVector;
   this->EscapedGlobalBitVector = escapedGlobalBitVector;

   // Reinitialize defined set for this iteration.

   this->DefinedInTileTagsBitVector->clear();
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Initialize per-iteration data underlying the available expressions analysis
//
//--------------------------------------------------------------------------------------------------------------

void
AvailableExpressions::Initialize
(
   GraphColor::Allocator * allocator
)
{
   assert(allocator == this->Allocator);
   assert(allocator != nullptr && this->IsGlobalScope());

   //Tiled::FunctionUnit ^     functionUnit = allocator->FunctionUnit;
   //Expression::Info ^      expressionInfo = functionUnit->ExpressionInfo;

   // Create an expression table to compute available expressions for the allocator.
   Expression::Table * expressionTable = Expression::Table::New(256);

   llvm::SparseBitVector<> *  availableBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  multipleShapeBitVector = new llvm::SparseBitVector<>();
   llvm::SparseBitVector<> *  escapedGlobalBitVector = new llvm::SparseBitVector<>();

   this->ExpressionTable = expressionTable;
   this->AvailableBitVector = availableBitVector;
   this->MultipleShapeBitVector = multipleShapeBitVector;
   this->EscapedGlobalBitVector = escapedGlobalBitVector;

   assert(this->DefinedInTileTagsBitVector->empty());
}

llvm::MachineOperand *
getSingleExplicitDestinationOperand
(
   llvm::MachineInstr * instruction
)
{
   llvm::MachineOperand * result = nullptr;

   if ((instruction->defs().end() - instruction->defs().begin()) == 1) {
      llvm::MachineOperand * tmp = instruction->defs().begin();
      if (!tmp->isReg() || !tmp->isImplicit()) {
         result = tmp;
      }
   }

   return result;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Compute any updates to never killed expressions from this instruction
//
// Arguments:
//
//    instruction - Instruction being analyzed
//    tile        - Current context
//
//--------------------------------------------------------------------------------------------------------------

void
AvailableExpressions::ComputeNeverKilled
(
   llvm::MachineInstr *  instruction,
   GraphColor::Tile *    tile
)
{
   GraphColor::Allocator *    allocator = this->Allocator;
   Tiled::VR::Info *          vrInfo = (tile? tile->VrInfo : allocator->VrInfo);
   llvm::SparseBitVector<> *  escapedGlobalBitVector = this->EscapedGlobalBitVector;

   llvm::MachineInstr::mop_iterator destinationOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> range(instruction->defs().begin(),
                                                                instruction->defs().end());

   // foreach_register_destination_opnd
   for (destinationOperand = range.begin(); destinationOperand != range.end(); ++destinationOperand)
   {
      if (!destinationOperand->isReg()) {
         continue;
      }

      // destinationOperand iterates only over explicit results of instruction

      unsigned destinationOperandTag = vrInfo->GetTag(destinationOperand->getReg());

      Expression::Table *        expressionTable = this->ExpressionTable;
      llvm::SparseBitVector<> *  availableBitVector = this->AvailableBitVector;
      llvm::SparseBitVector<> *  multipleShapeBitVector = this->MultipleShapeBitVector;
      GraphColor::LiveRange *    liveRange;

      if (tile == nullptr) {
         // Doing global walk so consult the allocator for global live range information.
         assert(allocator != nullptr);

         liveRange = allocator->GetGlobalLiveRange(destinationOperandTag);
         if (liveRange == nullptr) {
            // Untracked appearance - just continue.
            continue;
         }

      } else {
         liveRange = tile->GetLiveRange(destinationOperandTag);
         if ((liveRange == nullptr) || liveRange->IsGlobal()) {
            continue;
         }
      }

      unsigned              definitionTag = liveRange->VrTag;
      llvm::MachineInstr *  destinationInstruction = destinationOperand->getParent();

      Expression::Occurrence * recalculateOccurrence = expressionTable->Lookup(definitionTag);

      // track side-effects
      // make available

      if (recalculateOccurrence != nullptr) {
         llvm::MachineOperand *  recalculateOperand = recalculateOccurrence->Operand;
         llvm::MachineInstr *    recalculateInstruction = recalculateOperand->getParent();

         if (Expression::Table::Compare(recalculateInstruction, destinationInstruction, Expression::CompareControl::Lexical)
            && getSingleExplicitDestinationOperand(destinationInstruction) == destinationOperand) {
            // Same shape is still available so add it to the value, note that if the shape is the same and
            // it's in the table we know it's 'never killed'

            expressionTable->InsertIfNotFound(definitionTag, destinationInstruction, Expression::CompareControl::Lexical);
         } else {
            Expression::Value *  value = recalculateOccurrence->Value;

            // Found different shape in map, this is a new value so clear it. Note we just remove this
            // occurrence so that the tag doesn't map.  If there are other occurrences of this shape that's
            // fine.

            value->Remove(recalculateOccurrence);

            // look for any other occurrence that need be removed from
            // the table.

            Expression::Occurrence *  occurrence;
            Expression::Occurrence *  next_occurrence;

            // foreach_occurrence_of_value_editing
            for (occurrence = value->FirstOccurrence; occurrence != nullptr; occurrence = next_occurrence)
            {
               next_occurrence = occurrence->Next;

               if (occurrence->ScratchId == definitionTag) {
                  value->Remove(occurrence);
               }
            }

            if (value->isEmpty()) {
               expressionTable->Remove(value);
            }

            multipleShapeBitVector->set(definitionTag);
         }

      } else if (getSingleExplicitDestinationOperand(destinationInstruction) == destinationOperand) {
         bool isKillable = true;

         if (this->IsNeverKilledExpression(destinationOperand)) {

            //TBD: ?? if (!instruction->HasHandlerLabelOperand)
            {
               if (!multipleShapeBitVector->test(definitionTag)) {

                  if (!escapedGlobalBitVector->test(definitionTag)) {
                     Expression::Occurrence *  occurrence;

                     // definition can be recalculated so add it to the table
                     occurrence = expressionTable->
                        InsertIfNotFound(definitionTag, destinationInstruction, Expression::CompareControl::Lexical);

                     // make expression available
                     availableBitVector->set(occurrence->Value->Id);

                     isKillable = false;
                  }
               }
            }
         }

         if (isKillable) {
            // Treat killable as multishape.
            multipleShapeBitVector->set(definitionTag);
         }

      } else {
         // By default if we have a def we don't understand track it as multishape.

         multipleShapeBitVector->set(definitionTag);
      }
   }

}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Compute available never killed expressions for an entire function.
//
// Arguments:
//
//    rootTile -  root tile of tile graph
//
// Notes:
//
//    Prepopulate available expressions with never killed instances to improve recalculation
//
//--------------------------------------------------------------------------------------------------------------

void
AvailableExpressions::ComputeNeverKilled
(
   Graphs::FlowGraph * functionUnit
)
{
   llvm::MachineFunction *    machineFunction = functionUnit->machineFunction;
   GraphColor::Allocator *    allocator = this->Allocator;
   Tiled::VR::Info *          aliasInfo = allocator->VrInfo;
   llvm::SparseBitVector<> *  definedInTileTagsBitVector = this->DefinedInTileTagsBitVector;
   llvm::SparseBitVector<> *  scratchBitVector = this->ScratchBitVector;

   if (!definedInTileTagsBitVector->empty())
   {
      definedInTileTagsBitVector->clear();
      scratchBitVector->clear();
   }

   // Initialize the set of defined tags for this tile.  This provides us with region never killed per tile.

   GraphColor::TileList * postOrderTiles = allocator->TileGraph->PostOrderTileList;
   GraphColor::TileList::iterator t;

   // foreach_tile_in_dfs_postorder
   for (t = postOrderTiles->begin(); t != postOrderTiles->end(); ++t)
   {
      GraphColor::Tile * tile = *t;

      Graphs::MachineBasicBlockVector::iterator tb;

      // foreach_block_in_tile
      for (tb = tile->BlockVector->begin(); tb != tile->BlockVector->end(); ++tb)
      {
         llvm::MachineBasicBlock * block = *tb;
         if (block->empty()) continue;

         llvm::MachineInstr *  firstInstruction = &(block->instr_front());
         llvm::MachineInstr *  lastInstruction = &(block->instr_back());

         // Advance past the function entry. Those writes are a priori.

         //TBD: check if any 'entry' instructions in MachineFunction ?
         //if (firstInstruction->Opcode == Common::Opcode::EnterFunction) {
         //   firstInstruction = firstInstruction->Next;
         //}

         // Summarize defs in block.

         aliasInfo->SummarizeDefinitionTagsInRange(firstInstruction, lastInstruction,
                                                   Tiled::VR::AliasType::May,
                                                   Tiled::VR::SideEffectSummaryOptions::IgnoreLocalTemporaries,
                                                   scratchBitVector);
      }

   }

   // Add may partial writes

   aliasInfo->OrMayPartialTags(scratchBitVector, definedInTileTagsBitVector);

   // The FRAME pseudo register can be viewed as 'fixed' even on
   // platforms where it is adjusted.  (symbol offsets are modified
   // with respect to stack depth to ensure that they're always
   // addressing the same location) Remove FRAME from the defined set
   // so that params and locals can be recalculate candidates.

   unsigned fpRegister = allocator->TRI->getFrameRegister(*machineFunction);
   unsigned fpTag = aliasInfo->GetTag(fpRegister);
   aliasInfo->MinusMayPartialTags(fpTag, definedInTileTagsBitVector);

   // Walk the exclusive instructions of the tile computing available expressions.

   // [ MachineFunction doesn't have a sequential accessor to all instructions in the function ]

   // foreach_instr_in_func
   llvm::MachineFunction::iterator b;
   for (b = machineFunction->begin(); b != machineFunction->end(); ++b)
   {
      llvm::MachineBasicBlock::instr_iterator ii;
      for (ii = b->instr_begin(); ii != b->instr_end(); ++ii)
      {
         llvm::MachineInstr * instruction = &(*ii);
         this->ComputeNeverKilled(instruction, nullptr);
      }
   }
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Compute available never killed expressions for an entire tile.
//
// Arguments:
//
//    tile - tile to process
//
// Notes:
//
//    Prepopulate available expressions with never killed instances to improve recalculation
//
//--------------------------------------------------------------------------------------------------------------

void
AvailableExpressions::ComputeNeverKilled
(
   GraphColor::Tile * tile
)
{
   Tiled::VR::Info *          aliasInfo = tile->VrInfo;
   llvm::SparseBitVector<> *  definedInTileTagsBitVector = this->DefinedInTileTagsBitVector;
   llvm::SparseBitVector<> *  escapedGlobalBitVector = this->EscapedGlobalBitVector;
   llvm::SparseBitVector<> *  scratchBitVector = this->ScratchBitVector;

   if (!definedInTileTagsBitVector->empty()) {
      definedInTileTagsBitVector->clear();
      scratchBitVector->clear();
   }

   if (!escapedGlobalBitVector->empty()) {
      escapedGlobalBitVector->clear();
   }

   // Initialize the set of defined tags for this tile.  This provides us with region never killed per tile.

   Graphs::MachineBasicBlockVector * blockVector = tile->BlockVector;
   Graphs::MachineBasicBlockVector::iterator biter;

   // foreach_block_in_tile
   for (biter = blockVector->begin(); biter != blockVector->end(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;
      if (block->empty()) continue;

      llvm::MachineInstr *  firstInstruction = &(block->instr_front());
      llvm::MachineInstr *  lastInstruction = &(block->instr_back());

      // Summarize defs in block.
      aliasInfo->SummarizeDefinitionTagsInRange(firstInstruction, lastInstruction,
                                                Tiled::VR::AliasType::May,
                                                Tiled::VR::SideEffectSummaryOptions::IgnoreLocalTemporaries,
                                                scratchBitVector);
   }

   // Add may partial writes.
   aliasInfo->OrMayPartialTags(scratchBitVector, definedInTileTagsBitVector);

   // Or in definitions of nested tiles.

   GraphColor::TileList::iterator nt;

   // foreach_nested_tile_in_tile
   for (nt = tile->NestedTileList->begin(); nt != tile->NestedTileList->end(); ++nt)
   {
      GraphColor::Tile * nestedTile = *nt;

      llvm::SparseBitVector<> *  nestedDefinedInTileTagsBitVector
         = nestedTile->AvailableExpressions->DefinedInTileTagsBitVector;

      *definedInTileTagsBitVector |= *nestedDefinedInTileTagsBitVector;
   }

   // Process entries and exits adding any live in/out globals to the escaped set.  Any global that falls in
   // the escaped set can't be moved freely in a flow insensitive way. (we could miss an exit or kill a
   // incoming value)

   GraphColor::Allocator *  allocator = tile->Allocator;
   GraphColor::Liveness *   liveness = allocator->Liveness;
   GraphColor::LiveRange *  globalLiveRange;
   unsigned                 aliasTag;

   Graphs::MachineBasicBlockList::iterator b;

   // foreach_BasicBlock_in_List
   for (b = tile->EntryBlockList->begin(); b != tile->EntryBlockList->end(); ++b)
   {
      llvm::MachineBasicBlock *  entryBlock = *b;

      llvm::SparseBitVector<> *  liveInBitVector = liveness->GetGlobalAliasTagSet(entryBlock);

      llvm::SparseBitVector<>::iterator a;

      // foreach_sparse_bv_bit
      for (a = liveInBitVector->begin(); a != liveInBitVector->end(); ++a)
      {
         unsigned aliasTag = *a;

         if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
            continue;
         }
         // remaining are tags of (unallocated) virtual register operands

         globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);
         assert(globalLiveRange != nullptr);

         escapedGlobalBitVector->set(globalLiveRange->VrTag);
      }
   }

   // foreach_BasicBlock_in_List
   for (b = tile->ExitBlockList->begin(); b != tile->ExitBlockList->end(); ++b)
   {
      llvm::MachineBasicBlock *  exitBlock = *b;

      llvm::SparseBitVector<> *  liveOutBitVector = liveness->GetGlobalAliasTagSet(exitBlock);

      llvm::SparseBitVector<>::iterator a;

      // foreach_sparse_bv_bit
      for (a = liveOutBitVector->begin(); a != liveOutBitVector->end(); ++a)
      {
         unsigned aliasTag = *a;

         if (aliasInfo->IsPhysicalRegisterTag(aliasTag)) {
            continue;
         }
         // remaining are tags of (unallocated) virtual register operands

         globalLiveRange = allocator->GetGlobalLiveRange(aliasTag);
         assert(globalLiveRange != nullptr);

         escapedGlobalBitVector->set(globalLiveRange->VrTag);
      }
   }

   // The FRAME pseudo register can be viewed as 'fixed' even on
   // platforms where it is adjusted.  (symbol offsets are modified
   // with respect to stack depth to ensure that they're always
   // addressing the same location) Remove FRAME from the defined set
   // so that params and locals can be recalculate candidates.

   unsigned fpRegister = tile->Allocator->TRI->getFrameRegister(*(tile->MF));
   unsigned fpTag = aliasInfo->GetTag(fpRegister);
   aliasInfo->MinusMayPartialTags(fpTag, definedInTileTagsBitVector);

   // Walk the exclusive instructions of the tile computing available expressions.

   // foreach_instruction_in_tile is implemented as the nested:
   //               foreach_block_in_tile/foreach_instr_in_block

   // foreach_block_in_tile
   for (biter = blockVector->begin(); biter != blockVector->end(); ++biter)
   {
      llvm::MachineBasicBlock * block = *biter;

      llvm::MachineInstr * instruction = nullptr;
      llvm::MachineBasicBlock::instr_iterator(i);

      // foreach_instr_in_block
      for (i = block->instr_begin(); i != block->instr_end(); ++i)
      {
         instruction = &*i;

         this->ComputeNeverKilled(instruction, tile);
      }
   }
}


//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Get current available expression for this alias tag if any
//
// Returns
//
//    Definition operand for the available expression
//
//--------------------------------------------------------------------------------------------------------------

llvm::MachineOperand *
AvailableExpressions::GetAvailable
(
   unsigned aliasTag
)
{
   GraphColor::LiveRange *    liveRange = nullptr;
   Expression::Table *        expressionTable;
   Expression::Occurrence *   occurrence;
   llvm::SparseBitVector<> *  availableBitVector;

   // If there is a tile context check that first.

   if (this->Tile != nullptr) {
      GraphColor::Tile * tile = this->Tile;

      liveRange = tile->GetLiveRange(aliasTag);
      if (liveRange == nullptr) {
         return nullptr;
      }

      Alias::Tag liveRangeTag = liveRange->GetAliasTag();

      expressionTable = this->ExpressionTable;

      occurrence = expressionTable->Lookup(liveRangeTag);
      availableBitVector = this->AvailableBitVector;

      if (occurrence != nullptr) {
         Expression::Value * value = occurrence->Value;

         // check available
         if (availableBitVector->test(value->Id)) {
            return value->getLeaderOperand();
         }
      }

   } else {
      liveRange = Allocator->GetGlobalLiveRange(aliasTag);
      if (liveRange == nullptr) {
         return nullptr;
      }
   }

   assert(liveRange != nullptr);

   // If live range is global recheck with the global table.

   if (liveRange->IsGlobal()) {
      GraphColor::LiveRange *            globalLiveRange = liveRange->GlobalLiveRange;
      Alias::Tag                         globalLiveRangeTag = globalLiveRange->GetAliasTag();
      GraphColor::Allocator *            allocator = this->Allocator;
      GraphColor::AvailableExpressions * globalAvailableExpressions = allocator->GlobalAvailableExpressions;

      expressionTable = globalAvailableExpressions->ExpressionTable;

      occurrence = expressionTable->Lookup(globalLiveRangeTag);
      availableBitVector = globalAvailableExpressions->AvailableBitVector;

      if (occurrence != nullptr) {
         Expression::Value * value = occurrence->Value;

         // check available
         if (availableBitVector->test(value->Id)) {
            return value->getLeaderOperand();
         }
      }
   }

   return nullptr;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Get the occurrence operand if an occurrence is being tracked
//
// Returns
//
//    occurrence operand
//
//--------------------------------------------------------------------------------------------------------------

llvm::MachineOperand *
AvailableExpressions::LookupOccurrenceOperand
(
   llvm::MachineOperand * operand
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::VR::Info *        vrInfo = allocator->VrInfo;
   unsigned                 aliasTag = vrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange *  liveRange;

   if (this->Tile == nullptr) {
      liveRange = allocator->GetGlobalLiveRange(aliasTag);
   } else {
      GraphColor::Tile *  tile = this->Tile;
      liveRange = tile->GetLiveRange(aliasTag);
   }

   if (liveRange == nullptr) {
      return nullptr;
   }

   unsigned                  liveRangeTag = liveRange->GetAliasTag();
   Expression::Table *       expressionTable = this->ExpressionTable;
   Expression::Occurrence *  occurrence = expressionTable->Lookup(liveRangeTag);

   if (occurrence != nullptr) {
      assert(occurrence->Operand != nullptr);

      return occurrence->Operand;
   }

   return nullptr;
}
   
//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Check passed definition as to whether it is tracked for availability
//
// Arguments:
//
//    definitionOperand - definition to check
//
// Returns:
//
//    True if tracked for availability
//
//--------------------------------------------------------------------------------------------------------------

bool
AvailableExpressions::IsTrackedExpression
(
   llvm::MachineOperand * definitionOperand
)
{
   return false;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Check passed definition to see if it is the result of a never killed expression.
//
// Arguments:
//
//    definitionOperand - definition to check
//
// Returns:
//
//    True if never killed
//
//--------------------------------------------------------------------------------------------------------------

bool
AvailableExpressions::IsNeverKilledExpression
(
   llvm::MachineOperand * definitionOperand
)
{
   assert(definitionOperand->isDef());
   GraphColor::Allocator *  allocator = this->Allocator;
   Tiled::VR::Info *        aliasInfo = allocator->VrInfo;

   llvm::MachineInstr *  instruction = definitionOperand->getParent();

   if (Tiled::VR::Info::IsPhysicalRegister(definitionOperand->getReg()) ||
       (instruction->getDesc().getNumDefs() != 1) ) {
      return false;
   }

   // Do not recalculate instructions with opaque side effects
   if (instruction->hasUnmodeledSideEffects()) {
      return false;
   }

   if (instruction->mayLoad()) {
      if (instruction->isDereferenceableInvariantLoad(this->AA)) {
         return true;
      }

      return false;
   }

   llvm::MachineFunction *  machineFunction = instruction->getParent()->getParent();
   unsigned fpReg = allocator->TRI->getFrameRegister(*machineFunction);

   // return true if none of the sources are dataflow

   llvm::MachineInstr::mop_iterator sourceOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(),
                                                               instruction->operands().end());

   // foreach_source_opnd
   for (sourceOperand = uses.begin(); sourceOperand != uses.end(); ++sourceOperand)
   {
      if (!sourceOperand->isReg() || !sourceOperand->isUse()) {
         continue;
      }

      // special case for frame register which we know is constant.
      if (sourceOperand->getReg() != fpReg) {
         return false;
      }

      if (sourceOperand->isRegLiveOut() || sourceOperand->isMetadata() || sourceOperand->isRegMask()) {
         return false;
      }

      // x86 semantics require spilling of precise FP live ranges for correctness of the down stream x87
      // allocator.  This code goes very conservative to preserve that.
      //if (sourceOperand->IsFloat && sourceOperand->IsDataflow && allocator->IsX87SupportEnabled) {
      //   return false;
      //}

   }

   return true;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Test if the operand is written to in the current tile.
//
// Arguments:
//
//    operand - test is written
//
// Returns:
//
//    True if never written.
//
//--------------------------------------------------------------------------------------------------------------

bool
AvailableExpressions::IsNeverWritten
(
   llvm::MachineMemOperand *  memOperand
)
{
   llvm::SparseBitVector<> *  definedInTileTagsBitVector = this->DefinedInTileTagsBitVector;

   // see the note in the above IsNeverKilledExpression()
#ifdef FUTURE_IMPL   //MULTITILE +
   GraphColor::Allocator ^ allocator = this->Allocator;
   Alias::Info ^           aliasInfo = allocator->AliasInfo;
   Alias::Tag              aliasTag = operand->AliasTag;

   // A location is never written if it's not address taken and it's not written to in the function.

   Tiled::Boolean neverWritten = !aliasInfo->IsReferenceTag(aliasTag)
      && !aliasInfo->IsAddressTaken(aliasTag)
      && !definedInTileTagsBitVector->GetBit(aliasTag);

   return neverWritten;
#endif

   return false;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Remove the passed alias tag if it exists.
//
// Arguments:
//
//    aliasTag - Tag to remove.
//
// Returns:
//
//    True if availability was cleared
//
//--------------------------------------------------------------------------------------------------------------

#ifdef FUTURE_IMPL   //currently not in use
bool
AvailableExpressions::Remove
(
   unsigned aliasTag
)
{
   GraphColor::Tile ^      tile = this->Tile;
   GraphColor::Allocator ^ allocator = this->Allocator;
   GraphColor::LiveRange ^ liveRange;

   if (this->IsGlobalScope)
   {
      liveRange = allocator->GetGlobalLiveRange(aliasTag);
   }
   else
   {
      liveRange = tile->GetLiveRange(aliasTag);
   }

   if (liveRange == nullptr)
   {
      return false;
   }

   // remove this tag from the value.

   Alias::Tag               liveRangeTag = liveRange->AliasTag;
   Expression::Table ^      expressionTable = this->ExpressionTable;
   Expression::Occurrence ^ recalculateOccurrence = expressionTable->Lookup(liveRangeTag);

   Tiled::Boolean isAvailable = (recalculateOccurrence != nullptr);

   if (isAvailable)
   {
      Expression::Value ^ recalculateValue = recalculateOccurrence->Value;

      // DominatingOperand was only set in RenameOptimization in global-optimizer
      //if (recalculateOccurrence->Operand == recalculateValue->DominatingOperand) {
      //   recalculateValue->DominatingOperand = nullptr;
      //}

      recalculateValue->Remove(recalculateOccurrence);

      foreach_occurrence_of_value_editing(occurrence, recalculateValue)
      {
         if (occurrence->ScratchId == liveRangeTag)
         {
            recalculateValue->Remove(occurrence);
         }
      }
      next_occurrence_of_value_editing;

      if (recalculateValue->IsEmpty)
      {
         expressionTable->Remove(recalculateValue);
      }
   }

   // return the fact that the passed def was available

   return isAvailable;
}
#endif // FUTURE_IMPL

int
getIndexOfDestination
(
   llvm::MachineOperand *  recalculateOperand,
   llvm::MachineInstr *    recalculateInstruction
)
{
   unsigned numDefs = recalculateInstruction->getDesc().getNumDefs();

   for (unsigned index = 0; index < numDefs; ++index) {
      llvm::MachineOperand * operand = &(recalculateInstruction->getOperand(index));
      if (operand == recalculateOperand) {
         return index;
      }
   }

   return -1;
}

//--------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Remove availability for the passed operand if it exists.
//
// Arguments:
//
//    operand - Appearance to remove availability for
//
// Returns:
//
//    True if availability was cleared
//
//--------------------------------------------------------------------------------------------------------------

bool
AvailableExpressions::RemoveAvailable
(
   llvm::MachineOperand * operand
)
{
   GraphColor::Allocator *  allocator = this->Allocator;
   GraphColor::Tile *       tile = this->Tile;
   Tiled::VR::Info *        vrInfo = (tile? tile->VrInfo : allocator->VrInfo);
   unsigned                 appearanceTag = vrInfo->GetTag(operand->getReg());
   GraphColor::LiveRange *  liveRange;

   if (this->IsGlobalScope()) {
      liveRange = allocator->GetGlobalLiveRange(appearanceTag);
   } else {
      liveRange = tile->GetLiveRange(appearanceTag);
   }

   if (liveRange == nullptr) {
      return false;
   }

   // remove this operand from the value.

   unsigned                  liveRangeTag = liveRange->VrTag;
   Expression::Table *       expressionTable = this->ExpressionTable;
   Expression::Occurrence *  recalculateOccurrence = expressionTable->Lookup(liveRangeTag);

   bool isAvailable = (recalculateOccurrence != nullptr);

   if (isAvailable) {
      Expression::Value * recalculateValue = recalculateOccurrence->Value;

      if (this->IsGlobalScope()) {
         // Copy global occurrence for reuse in another tile.

         llvm::MachineOperand *  recalculateOperand = recalculateOccurrence->Operand;
         llvm::MachineInstr *    recalculateInstruction = recalculateOperand->getParent();
         assert(recalculateInstruction != nullptr);
         int                     destinationIndex = getIndexOfDestination(recalculateOperand, recalculateInstruction);
         assert(destinationIndex != -1);

         if (operand != recalculateOperand) {
            Expression::Occurrence * operandOccurrence = nullptr;

            // We have multiple occurrences of the same tag we're currently mapped to an occurrence other than
            // that of this operand. Look through the value to find the correct occurrence.

            Expression::Occurrence * occurrence;

            // foreach_occurrence_of_value
            for (occurrence = recalculateValue->FirstOccurrence; occurrence != nullptr; occurrence = occurrence->Next)
            {
               if (operand == occurrence->Operand) {
                  operandOccurrence = occurrence;
                  break;
               }
            }

            // If we find no occurrence of this def in the table it is a newly made definition through
            // recalculation and we can omit.

            if (operandOccurrence == nullptr) {
               return isAvailable;
            } else {
               recalculateOccurrence = operandOccurrence;
            }
         }

         llvm::MachineFunction * MF = this->Allocator->MF;
         llvm::MachineInstr *    copyInstruction = MF->CloneMachineInstr(recalculateInstruction);
         llvm::MachineOperand *  copyOperand = &(copyInstruction->getOperand(destinationIndex));

         // Set up occurrence with new copied operand
         recalculateOccurrence->Operand = copyOperand;

      } else {
         // Remove local occurrence from table.

         // DominatingOperand was only set in RenameOptimization in global-optimizer
         //if (recalculateOccurrence->Operand == recalculateValue->DominatingOperand) {
         //   recalculateValue->DominatingOperand = nullptr;
         //}

         recalculateValue->Remove(recalculateOccurrence);

         Expression::Occurrence * occurrence;

         // foreach_occurrence_of_value_editing
         for (occurrence = recalculateValue->FirstOccurrence; occurrence != nullptr; occurrence = occurrence->Next)
         {
            if (occurrence->ScratchId == liveRangeTag) {
               recalculateValue->Remove(occurrence);
            }
         }
      }
   }

   // return the fact that the passed def was available
   return isAvailable;
}

} // GraphColor
} // RegisterAllocator
} // Phx
