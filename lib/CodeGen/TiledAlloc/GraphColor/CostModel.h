//===-- GraphColor/CostModel.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_COSTMODEL_H
#define TILED_GRAPHCOLOR_COSTMODEL_H

#include "../Cost.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//   Base allocator cost model implementation
//
//-------------------------------------------------------------------------------------------------------------

class AllocatorCostModel : public Tiled::CostModel
{

public:

   static GraphColor::AllocatorCostModel *
   New
   (
      GraphColor::Allocator * allocator
   );

public:

   Tiled::Int
   Compare
   (
      Tiled::Cost * firstCost,
      Tiled::Cost * secondCost
   );

   Tiled::CostValue
   Evaluate
   (
      Tiled::Cost * cost
   );

public:

   // Number of cycles to consider equivalent to 1 byte when evaluating costs.
   Tiled::Int32 SpeedWeightFactor;
};

class HotCostModel : public GraphColor::AllocatorCostModel
{

public:

   static GraphColor::HotCostModel *
   New
   (
      GraphColor::Allocator * allocator
   );
};

class ColdCostModel : public GraphColor::AllocatorCostModel
{

public:

   static GraphColor::ColdCostModel *
   New
   (
      GraphColor::Allocator * allocator
   );
};
} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif // end TILED_GRAPHCOLOR_COSTMODEL_H
