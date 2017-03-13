//===-- GraphColor/SpillOptimizerCheck.cpp ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Tiled register allocator target interface
//
//===----------------------------------------------------------------------===//

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

#if defined(TILED_DEBUG_CHECKS)

void
SpillRecord::IsValid()
{
   if (this->HasRecalculate)
   {
      Assert(this->RecalculateOperand->IsLinked);
   }

   if (this->HasReload)
   {
      Assert(this->ReloadOperand->IsLinked || this->IsFoldedReload);
   }

   if (this->HasSpill)
   {
      Assert(this->SpillOperand->IsAllocated);
   }

   if (this->HasValue)
   {
      Assert(this->ValueOperand->IsAllocated);
   }
}

Tiled::Boolean
SpillOptimizer::IsEmpty::get()
{
   return (this->SpillMap->IsEmpty && this->RegionTrackedBitVector->IsEmpty);
}

void
SpillOptimizer::CheckRecordConsistency()
{
   // check each record for individual consistency

   foreach_record_in_spillmap(spillRecord, keyTag, this->SpillMap)
   {
      spillRecord->IsValid();
   }
   next_record_in_spillmap;

   // ensure available expression consistency

   this->AvailableExpressions->CheckAvailable();
}

#endif // TILED_DEBUG_CHECKS

} // GraphColor
} // RegisterAllocator
} // Phx
