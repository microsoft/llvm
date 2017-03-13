//===-- GraphColor/Preference.h ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_PREFERENCE_H
#define TILED_GRAPHCOLOR_PREFERENCE_H

#include "../Preference.h"
#include "../Alias/Alias.h"
#include "../Cost.h"

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

//------------------------------------------------------------------------------
// Description:
//
//    Live range preference edge.
//
// Remarks:
//
//    Preferences are a triple of a live range id (adjacency), a constraint,
//    and a cost (benefit) 
//
//------------------------------------------------------------------------------

class LiveRangePreference
{
public:

   Tiled::Cost                             Cost;
   unsigned                                LiveRangeId;
   RegisterAllocator::PreferenceConstraint PreferenceConstraint;

};

//------------------------------------------------------------------------------
// Description:
//
//    Physical register preference (or anti-preference).
//
// Remarks:
//
//    Physical register preferences is a register and cost/benefit pair.
//    Benefit is for preference, cost is for anti-preference.
//
//------------------------------------------------------------------------------

class PhysicalRegisterPreference
{
public:

   PhysicalRegisterPreference(Tiled::Cost& cost, unsigned reg)
      : Cost(cost), Register(reg) {}

   PhysicalRegisterPreference() : Cost(), Register(Tiled::VR::Constants::InvalidReg) {}

   static bool
   Equals
   (
      GraphColor::PhysicalRegisterPreference leftValue,
      GraphColor::PhysicalRegisterPreference rightValue
   );

public:

   Tiled::Cost           Cost;
   unsigned              Register;
};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#endif //TILED_GRAPHCOLOR_PREFERENCE_H
