//===-- GraphColor/AvailableExpressions.h -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_AVAILABLEEXPRESSIONS_H
#define TILED_GRAPHCOLOR_AVAILABLEEXPRESSIONS_H

#include "Allocator.h"
#include "Tile.h"
#include "llvm/ADT/SparseBitVector.h"

namespace Tiled
{

namespace Expression
{

enum class CompareControl
{
   IllegalSentinel = 0, // Invalid comparison type
   Default = 0x1, // Default is literal comparison
   Lexical = 0x1, // Compare operands literally
};

class Occurrence;
class Table;

class Value {
public:
   unsigned                 Id;
   unsigned                 HashCode;
   bool                     HasChanged;
   Expression::Occurrence * FirstOccurrence;
   Expression::Occurrence * LastOccurrence;
   Expression::Occurrence * LeaderOccurrence;
   llvm::MachineOperand *   DominatingOperand;  // Dominating occurrence

   Expression::Value *      Next;

   static Expression::Value *
   New
   (
      Expression::Table * table,
      unsigned            hashCode,
      unsigned            valueId
   );

   void
   Append
   (
      Expression::Occurrence * occurrence
   );

   bool
   hasLeader() { return (this->getLeaderOccurrence() != nullptr); }

   Expression::Occurrence *
   getLeaderOccurrence() { return (this->HasChanged ? nullptr : this->LeaderOccurrence); }

   llvm::MachineOperand *  getLeaderOperand();

   bool
   isEmpty() { return (this->FirstOccurrence == nullptr); }

   Expression::Occurrence *
   FindOccurrence
   (
      llvm::MachineOperand * operand
   );

   void
   Remove
   (
      Expression::Occurrence * occurrence
   );

   void
   Unlink
   (
      Expression::Occurrence * occurrence
   );

   void RemoveAll();

private:
   Expression::Table *      Table;
};


class Occurrence {
public:
   llvm::MachineOperand *    Operand;
   Expression::Value *       Value;
   Expression::Occurrence *  Next;
   Expression::Occurrence *  Previous;
   unsigned                  ScratchId;

   static Expression::Occurrence *
   New
   (
      Expression::Value *    value,
      unsigned               scratchId,
      llvm::MachineOperand * operand
   );

   bool
   hasLeader() { return this->Value->hasLeader(); }

   llvm::MachineOperand *
   leaderOperand() { return this->Value->getLeaderOperand(); }
};


class Table {
public:
   static Expression::Table *
   New
   (
      unsigned hashSize
   );

   Expression::Occurrence *
   Lookup
   (
      unsigned tag
   );

   static bool
   Compare
   (
      llvm::MachineInstr *        instruction1,
      llvm::MachineInstr *        instruction2,
      Expression::CompareControl  compareControl
   );

   Expression::Occurrence *
   InsertIfNotFound
   (
      unsigned                    id,
      llvm::MachineInstr *        instruction,
      Expression::CompareControl  compareControl
   );

   bool
   Remove
   (
      Expression::Value *  value
   );

private:
   Expression::Value *
   Lookup
   (
      unsigned                    hashCode,
      llvm::MachineInstr *        instruction,
      Expression::CompareControl  compareControl
   );

   Expression::Occurrence *
   InsertIfNotFound
   (
      unsigned                   id,
      unsigned                   hashCode,
      llvm::MachineInstr *       instruction,
      Expression::CompareControl compareControl
   );

   Expression::Occurrence *
   NewOccurrenceIfNotFound
   (
      Expression::Value *     value,
      unsigned                id,
      unsigned                hashCode,
      llvm::MachineOperand *  operand
   );

   Expression::Occurrence *
   InsertUnique
   (
      unsigned                id,
      unsigned                hashCode,
      llvm::MachineOperand *  operand
   );

   unsigned  NextValueId;

public:
   unsigned                         Size;
   Expression::IdToOccurrenceMap *  OccurrenceMap;
   Expression::ExpressValueVector * ValueMap;

};

} // namespace Expression


namespace RegisterAllocator
{
namespace GraphColor
{
   
class AvailableExpressions
{

public:

   static GraphColor::AvailableExpressions *
   New
   (
      GraphColor::Tile * tile
   );

   static GraphColor::AvailableExpressions *
   New
   (
      GraphColor::Allocator * allocator
   );

public:

   void CheckAvailable();

   void
   ComputeNeverKilled
   (
      Graphs::FlowGraph * functionUnit
   );

   void
   ComputeNeverKilled
   (
      GraphColor::Tile * tile
   );

   void
   ComputeNeverKilled
   (
      llvm::MachineInstr *  instruction,
      GraphColor::Tile *    tile
   );

   llvm::MachineOperand *
   GetAvailable
   (
      llvm::MachineOperand * operand
   )
   {
      assert(operand->isReg());
      Tiled::VR::Info * vrInfo = this->Allocator->VrInfo;
      unsigned aliasTag = vrInfo->GetTag(operand->getReg());

      return this->GetAvailable(aliasTag);
   }

   llvm::MachineOperand *
   GetAvailable
   (
      unsigned aliasTag
   );

   void
   Initialize
   (
      GraphColor::Tile * tile
   );

   void
   Initialize
   (
      GraphColor::Allocator * allocator
   );

   llvm::MachineOperand *
   LookupOccurrenceOperand
   (
      llvm::MachineOperand * operand
   );
   
#ifdef FUTURE_IMPL   //currently not in use
   bool
   Remove
   (
      unsigned aliasTag
   );
#endif

   bool
   RemoveAvailable
   (
      llvm::MachineOperand * operand
   );

public:

   bool IsGlobalScope() { return (this->Tile == nullptr); }

private:

   bool
   IsNeverKilledExpression
   (
      llvm::MachineOperand * operand
   );

   bool
   IsNeverWritten
   (
      llvm::MachineMemOperand * memOperand
   );

   bool
   IsTrackedExpression
   (
      llvm::MachineOperand * operand
   );

private:

   llvm::SparseBitVector<> *  AvailableBitVector;
   llvm::SparseBitVector<> *  DefinedInTileTagsBitVector;
   llvm::SparseBitVector<> *  EscapedGlobalBitVector;
   llvm::SparseBitVector<> *  MultipleShapeBitVector;
   llvm::SparseBitVector<> *  ScratchBitVector;
   Tiled::Expression::Table * ExpressionTable;
   GraphColor::Allocator *    Allocator;
   GraphColor::Tile *         Tile;
   llvm::AliasAnalysis *      AA;
};

#if 0
comment AvailableExpressions::AvailableBitVector
{
   // Bit vector of available value ids from the expression table
}

comment AvailableExpressions::ExpressionTable
{
   // Facility managing the hashing of and dependencies between expressions.
}
#endif

} // namespace GraphColor
} // namespace RegisterAllocator

} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_AVAILABLEEXPRESSIONS_H
