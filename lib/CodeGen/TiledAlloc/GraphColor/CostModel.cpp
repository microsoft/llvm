//===-- GraphColor/CostModel.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Allocator.h"
#include "CostModel.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{
//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for AllocatorCostModel
//
// Arguments:
//
//    lifetime - allocation pool
//
// Returns:
//
//    GraphColor::AllocatorCostModel
//
//------------------------------------------------------------------------------

GraphColor::AllocatorCostModel *
GraphColor::AllocatorCostModel::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::AllocatorCostModel * costModel = new GraphColor::AllocatorCostModel();
   
   // Use a trival number now for speed to size comparisons.

   costModel->SpeedWeightFactor = 1;

   return costModel;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compare two Cost objects.
//
// Arguments:
//
//    firstCost - First cost object to compare 
//    secondCost - Second cost object to compare
//
// Remarks:
//
//    Implement two level compare. If we are optimizing for speed, then compare cycles
//    and let code bytes be a tie breaker. If we optimizing for space, then compare
//    code bytes and let cycles be the tie breaker.
//
// Returns:
//
//    A positive value if firstCost greater than secondCost, zero if they are equal,
//    and a negative value if firstCost less than secondCost.
//
//------------------------------------------------------------------------------

Tiled::Int
AllocatorCostModel::Compare
(
   Tiled::Cost * firstCost,
   Tiled::Cost * secondCost
)
{
   assert(firstCost->IsInitialized);
   assert(secondCost->IsInitialized);

   Tiled::CostValue firstCostValue;
   Tiled::CostValue secondCostValue;

   // Favor speed

   if (this->SpeedWeightFactor != 0)
   {
      Tiled::CostValue firstCostValue = firstCost->GetExecutionCycles();
      Tiled::CostValue secondCostValue = secondCost->GetExecutionCycles();

      if (Tiled::CostValue::CompareEQ(firstCostValue, secondCostValue))
      {
         firstCostValue = firstCost->GetCodeBytes();
         secondCostValue = secondCost->GetCodeBytes();

         if (Tiled::CostValue::CompareEQ(firstCostValue, secondCostValue))
         {
            return 0;
         }
      }

      return Tiled::CostValue::CompareGT(firstCostValue, secondCostValue) ? 1 : -1;
   }

   // Favor size

   else
   {
      Tiled::CostValue firstCostValue = firstCost->GetCodeBytes();
      Tiled::CostValue secondCostValue = secondCost->GetCodeBytes();

      if (Tiled::CostValue::CompareEQ(firstCostValue, secondCostValue))
      {
         firstCostValue = firstCost->GetExecutionCycles();
         secondCostValue = secondCost->GetExecutionCycles();

         if (Tiled::CostValue::CompareEQ(firstCostValue, secondCostValue))
         {
            return 0;
         }
      }

      return Tiled::CostValue::CompareGT(firstCostValue, secondCostValue) ? 1 : -1;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return the weighted cost combining the given size and speed individual costs
//
// Arguments:
//
//    cost - pointer to the cost object to evaluate
//
// Returns:
//
//    The combined (weighted) cost as a float64
//
//------------------------------------------------------------------------------

Tiled::CostValue
AllocatorCostModel::Evaluate
(
   Tiled::Cost * cost
)
{
   assert(cost->IsInitialized);

   Tiled::CostValue weightedExecutionCycles =
      Tiled::CostValue::Multiply(cost->GetExecutionCycles(), Tiled::CostValue(this->SpeedWeightFactor));

   return Tiled::CostValue::Add(weightedExecutionCycles, cost->GetCodeBytes());
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for ColdCostModel
//
// Arguments:
//
//    allocator - allocator cost model is for
//
// Returns:
//
//    GraphColor::ColdCostModel
//
//------------------------------------------------------------------------------

GraphColor::ColdCostModel *
GraphColor::ColdCostModel::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::ColdCostModel * costModel = new GraphColor::ColdCostModel;

   // Use a trivial number now for speed to size comparisons.

   costModel->SpeedWeightFactor = 0;

   return costModel;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Static constructor for HotCostModel
//
// Arguments:
//
//    allocator - allocator cost model is for
//
// Returns:
//
//    GraphColor::HotCostModel
//
//------------------------------------------------------------------------------

GraphColor::HotCostModel *
GraphColor::HotCostModel::New
(
   GraphColor::Allocator * allocator
)
{
   GraphColor::HotCostModel * costModel = new GraphColor::HotCostModel;

   costModel->SpeedWeightFactor = 1;

   return costModel;
}

} // GraphColor
} // RegisterAllocator
} // Tiled

