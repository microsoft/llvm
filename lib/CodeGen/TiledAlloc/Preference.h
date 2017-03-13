//===-- Preference.h - Register Preference Information ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Used to communicate register allocation preferences from machine dependent
// portions of the target code to the machine independent portions of the
// register allocator.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_PREFERENCE_H
#define TILED_PREFERENCE_H

#include "Cost.h"

namespace Tiled
{
   
namespace RegisterAllocator
{
   
//------------------------------------------------------------------------------
// Description:
//
//    Enumerate type of preference constraints used in the allocator.
//
//    SameRegister     - Adjacent nodes must use the same register to satisfy constraint.
//    NextRegister     - Use 'next' register from associated live range.  (register + 1)
//    PreviousRegister - Use 'previous' register from associated live range (register - 1)
//
//------------------------------------------------------------------------------

enum class PreferenceConstraint
{
   InvalidRegister  = 0,
   SameRegister     = 1,
   NextRegister     = 2,
   PreviousRegister = 3
};

//------------------------------------------------------------------------------
// Description:
//
//    Enumerate type of preference combine modes
//
//------------------------------------------------------------------------------

enum class PreferenceCombineMode
{
   Add = 1,
   Max = 2
};

//------------------------------------------------------------------------------
// Description:
//
//    Preference between to alias tags used to denote live ranges.
//
// Remarks:
//
//    Preferences are a quad of two alias tags (used to denote live ranges), cost (benefit), and constraint
//    (see PreferenceConstraint above).
//
//------------------------------------------------------------------------------

class Preference
{
public:

   unsigned                                AliasTag1;
   unsigned                                AliasTag2;
   Tiled::Cost                             Cost;
   RegisterAllocator::PreferenceConstraint PreferenceConstraint;
};
} // namespace RegisterAllocator
} // namespace Tiled

#endif // TILED_PREFERENCE_H

