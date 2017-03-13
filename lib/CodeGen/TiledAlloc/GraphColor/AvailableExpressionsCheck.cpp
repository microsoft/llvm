//===-- GraphColor/AvailableExpressionsCheck.cpp ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{
#if defined (TILED_DEBUG_CHECKS)

void
AvailableExpressions::CheckAvailable()
{
   foreach_value_in_table(value, this->ExpressionTable)
   {
      foreach_occurrence_of_value(occurrence, value)
      {
         // Ensure that if an occurrence is in the table there exists a mapping from tag to an occurrence.
         // Though it may not be this one.

         AssertMObject((this->ExpressionTable->OccurrenceMap->Lookup(occurrence->ScratchId) != nullptr),
            "Occurrence tag not mapped!", occurrence);

         // ensure that each def is still linked to an instruction. (nobody has replaced this def in error)

         AssertObject(occurrence->Operand->IsLinked, occurrence->Operand);
      }
      next_occurrence_of_value;
   }
   next_value_in_table;

   if (this->IsGlobalScope)
   {
      // global table assert that we've put nothing in the global escaped set.

      Assert(this->EscapedGlobalBitVector->IsEmpty);
   }
}

#endif // TILED_DEBUG_CHECKS
} // GraphColor
} // RegisterAllocator
} // Phx
