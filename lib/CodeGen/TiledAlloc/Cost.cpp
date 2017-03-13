//===-- Cost.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Support class for tracking size/speed costs of optimization decisions.
//
//===----------------------------------------------------------------------===//

#include "Cost.h"
#include <assert.h>

namespace Tiled
{
//------------------------------------------------------------------------------
//
// Description:
//
// Static initialization of Tiled::Cost class.
// Currently, floating point infinite values are intialized here.
//
//------------------------------------------------------------------------------

// TODO: Merge this with Cost::StaticInitialize and define them to something meaningful
Tiled::CostValue    Tiled::Cost::InfinityValue;
Tiled::CostValue    Tiled::Cost::NegativeInfinityValue;
Tiled::Cost         Tiled::Cost::InfiniteCost;
Tiled::Cost         Tiled::Cost::NegativeInfiniteCost;
Tiled::Cost         Tiled::Cost::ZeroCost;

void
Cost::StaticInitialize
(
)
{
   // Couldn't find an appropriate infinity constant for double.
   // In such a case, explicit assignment with *& combination seems
   // to be the only option to properly initialize static fields with infinite values.
   // If an alternative, better approach is found, this code should be rewritten.

   Tiled::Int64 x = 0x7FF0000000000000;
   Cost::InfinityValue = *reinterpret_cast<Tiled::Float64 *>(&x);

   Tiled::Int64 y = 0xFFF0000000000000;
   Cost::NegativeInfinityValue = *reinterpret_cast<Tiled::Float64 *>(&y);

   InfiniteCost.SetInfinity();
   NegativeInfiniteCost.SetNegativeInfinity();
   ZeroCost.Initialize(0, 0);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Compare two Cost objects.
//
// Arguments:
//
//    firstCost - Firt cost object to compare 
//    secondCost - Second cost object to compare
//
// Remarks:
//
//    The default implementation of Compare invokes Evaluate on each of the
//    the two costs and compares the result.
//
//    Implementations of the cost model may override this function to change
//    the way comparisons are done.
//
// Returns:
//
//    A positive value if firstCost greater than secondCost, zero if they are equal,
//    and a negative value if firstCost less than secondCost.
//
//------------------------------------------------------------------------------

int
CostModel::Compare
(
   Tiled::Cost * firstCost,
   Tiled::Cost * secondCost
)
{
   assert(firstCost->IsInitialized);
   assert(secondCost->IsInitialized);

   // Obtain weighted costs and compare.

   Tiled::CostValue firstCostValue = this->Evaluate(firstCost);
   Tiled::CostValue secondCostValue = this->Evaluate(secondCost);

   // Leave some window of error, where 
   // "close enough" means the two things should be considered equal.

   if (this->AreWithinError(firstCostValue, secondCostValue))
   {
      return 0;
   }
   else
   {
      return Tiled::CostValue::CompareGT(firstCostValue, secondCostValue) ? 1 : -1;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Check if two cost values are close enough.
//
// Remarks:
//
//    This is a default implementation.
//
//------------------------------------------------------------------------------

bool
CostModel::AreWithinError
(
   Tiled::CostValue firstCostValue,
   Tiled::CostValue secondCostValue
)
{
   return Tiled::CostValue::CompareEQ(firstCostValue, secondCostValue);
}

//------------------------------------------------------------------------------
//
// Description: 
//    CostValue constructor. It enables implicit conversion from
//    Tiled::Float64 to Tiled::CostValue.
//
// Remarks:
//    Implicit conversion in managed  code requires
//    additional definition of an operator CostValue::operator CostValue.
//
//------------------------------------------------------------------------------

CostValue::CostValue
(
   double _floatValue
) : floatValue(_floatValue)
{}

//------------------------------------------------------------------------------
//
// Description:
//
//    Addition between two floating point costs.
//
// Arguments:
// 
//    Two floating point costs to be added. They should not be NaN.
//
// Remarks:
//
//    (1) This function uses a platform-independent floating point libraries.
//    (2) This function enforces a special semantics for addition 
//        between positive/negative infinity values as follows.
//
//     pinf + ninf => pinf
//
//     In all other cases, it returns standard results from addition.
//
//------------------------------------------------------------------------------

CostValue
CostValue::Add
(
   CostValue costValue1,
   CostValue costValue2
)
{
   assert(!CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   Tiled::CostValue returnValue = floatValue1 + floatValue2;

   // If subtraction returns NaN, it is because of infinity values.
   // Recover from NaN as described above.

   if (Tiled::CostValue::IsQuietNan(returnValue))
   {
      return Tiled::Cost::InfinityValue;
   }

   assert(!Tiled::CostValue::IsNan(returnValue));
   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Addition between three floating point costs.
//
// Arguments:
// 
//    Three floating point costs to be added. They should not be NaN.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Add
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2,
   Tiled::CostValue costValue3
)
{
   Tiled::CostValue returnValue = CostValue::Add(costValue1, costValue2);

   returnValue = CostValue::Add(returnValue, costValue3);

   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Subtraction between two floating point costs.
//
// Arguments:
// 
//    Two floating point costs to be subtracted. They should not be NaN.
//
// Remarks:
//
//    (1) This function uses a platform-independent floating point libraries.
//    (2) This function enforces a special semantics for subtraction 
//        between positive/negative infinity values as follows.
//
//     pinf - pinf => pinf
//     ninf - ninf => pinf
//
//     In all other cases, it returns standard results from subtraction.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Subtract
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   Tiled::CostValue returnValue = floatValue1 - floatValue2;

   // If subtraction returns NaN, it is because of infinity values.
   // Recover from NaN as described above.

   if (Tiled::CostValue::IsQuietNan(returnValue))
   {
      return Tiled::Cost::InfinityValue;
   }

   assert(!Tiled::CostValue::IsNan(returnValue));
   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Multiplication between two floating point costs.
//
// Arguments:
// 
//    Two floating point costs to be multiplied. They should not be NaN.
//
// Remarks:
//
//    (1) This function uses a platform-independent floating point libraries.
//    (2) This function enforces a special semantics for mutiplication 
//        between infinity values and zeros as follows.
//
//     pinf * pzero => pinf
//     pinf * nzero => ninf
//     ninf * pzero => ninf
//     ninf * nzero => pinf
//
//     In all other cases, it returns standard results from multiplication.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Multiply
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   Tiled::CostValue returnValue(floatValue1 * floatValue2);

   // If multiplication returns NaN, it is because of infinity values.
   // Recover from NaN as described above.

   if (Tiled::CostValue::IsQuietNan(returnValue))
   {
      // Determine the sign bit of the return value by performing XOR on input values.

      Tiled::Int64 tmp1 = *reinterpret_cast<Tiled::Int64 *>(&floatValue1);
      Tiled::Int64 tmp2 = *reinterpret_cast<Tiled::Int64 *>(&floatValue2);
      Tiled::Int64 sign = (tmp1 ^ tmp2) & Tiled::TypeConstants::Float64SignBitMask;
      Tiled::Int64 tmp3 = Tiled::TypeConstants::Float64InfinityPattern | sign;

      return Tiled::CostValue(*reinterpret_cast<Tiled::Float64 *>(&tmp3));
   }

   assert(!Tiled::CostValue::IsNan(returnValue));
   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Multiplication between three floating point costs.
//
// Arguments:
// 
//    Three floating point costs to be multiplied. They should not be NaN.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Multiply
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2,
   Tiled::CostValue costValue3
)
{
   Tiled::CostValue returnValue = CostValue::Multiply(costValue1, costValue2);

   returnValue = CostValue::Multiply(returnValue, costValue3);

   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Division between two floating point costs.
//
// Arguments:
// 
//    Two floating point costs to be divided. They should not be NaN.
//
// Remarks:
//
//    (1) This function uses a platform-independent floating point libraries.
//    (2) This function enforces a special semantics for division 
//        between infinity values as follows.
//
//     pinf / pinf => pinf
//     pinf / ninf => ninf
//     ninf / pinf => ninf
//     ninf / ninf => pinf
//
//     In all other cases, it returns standard results from division.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Divide
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   Tiled::CostValue returnValue = floatValue1 / floatValue2;

   // If division returns NaN, it is because of infinity values.
   // Recover from NaN as described above.

   if (Tiled::CostValue::IsQuietNan(returnValue))
   {
      // Determine the sign bit of the return value by performing XOR on input values.

      Tiled::Int64 tmp1 = *reinterpret_cast<Tiled::Int64 *>(&floatValue1);
      Tiled::Int64 tmp2 = *reinterpret_cast<Tiled::Int64 *>(&floatValue2);
      Tiled::Int64 sign = (tmp1 ^ tmp2) & Tiled::TypeConstants::Float64SignBitMask;
      Tiled::Int64 tmp3 = Tiled::TypeConstants::Float64InfinityPattern | sign;

      return Tiled::CostValue(*reinterpret_cast<Tiled::Float64 *>(&tmp3));
   }

   assert(!Tiled::CostValue::IsNan(returnValue));
   return returnValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Negation of a floating point.
//
// Arguments:
// 
//    Floating point to be negated.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Negate
(
   Tiled::CostValue costValue
)
{
   return CostValue::Multiply(costValue, -1);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 == floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareEQ
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 == floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 != floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareNE
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 != floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 > floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareGT
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 > floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 > 0
//    Return false otherwise.
//
// Arguments:
// 
//    costValue -- value to check.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::IsGreaterThanZero
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));

   Tiled::Float64 floatValue1 = costValue.AsFloat64();
   Tiled::Float64 floatValue2 = 0.0;

   return floatValue1 > floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 >= 0
//    Return false otherwise.
//
// Arguments:
// 
//    costValue -- value to check.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::IsGreaterThanOrEqualToZero
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));

   Tiled::Float64 floatValue1 = costValue.AsFloat64();
   Tiled::Float64 floatValue2 = 0.0;

   return floatValue1 >= floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 >= floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareGE
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 >= floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 < floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareLT
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Tiled::Float64 floatValue1 = costValue1.AsFloat64();
   Tiled::Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 < floatValue2;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return true if floatValue1 <= floatValue2.
//    Return false other wise.
//
// Arguments:
// 
//    Two floating point costs to be compared. They should not be NaN.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Boolean
CostValue::CompareLE
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   assert(!Tiled::CostValue::IsNan(costValue1));
   assert(!Tiled::CostValue::IsNan(costValue2));

   Float64 floatValue1 = costValue1.AsFloat64();
   Float64 floatValue2 = costValue2.AsFloat64();

   return floatValue1 <= floatValue2;
}


// The 4 functions below are "quick&dirty" implementations in lieu of
// full conversion of the Phoenix's FloatMath package.

double
convertFromUnsigned
(
   unsigned long intValue
)
{
   double result = double(intValue);
   return result;
}

double
convertFromSigned
(
   long intValue
)
{
   double result = double(intValue);
   return result;
}

unsigned long
convertToUnsigned
(
   double floatValue
)
{
   unsigned long result = (unsigned long)floatValue;
   return result;
}

long int
convertToSigned
(
   double floatValue
)
{
   long int result = (long int)floatValue;
   return result;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert unsigned 64-bit integer into 64-bit floint point.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::ConvertFromUnsigned
(
   UIntValue intValue
)
{
   //Tiled::CostValue resultValue(Toolbox::FloatMath::ConvertFromUnsigned(intValue));
   Tiled::CostValue resultValue(convertFromUnsigned(intValue));
   assert(!Tiled::CostValue::IsNan(resultValue));

   return resultValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert signed 64-bit integer into 64-bit float.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::ConvertFromSigned
(
   IntValue intValue
)
{
   //Tiled::CostValue resultValue(Toolbox::FloatMath::ConvertFromSigned(intValue));
   Tiled::CostValue resultValue(convertFromSigned(intValue));
   assert(!Tiled::CostValue::IsNan(resultValue));

   return resultValue;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert cost value or 64-bit float to signed 64-bit integer.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
// Arguments:
//
//    value -- float64 value
//    costValue -- cost value
//
// Returns:
//
//    IntValue that is closest to the CostValue or Float64.
//
//------------------------------------------------------------------------------

IntValue
CostValue::ConvertToSigned
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));
   return CostValue::ConvertToSigned(costValue.AsFloat64());
}

IntValue
CostValue::ConvertToSigned
(
   Float64 value
)
{
   //return Toolbox::FloatMath::ConvertToSigned(value, 8);
   return convertToSigned(value);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert cost value or 64-bit float point to signed 32-bit integer.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
// Arguments:
//
//    value -- float64 value
//    costValue -- cost value
//
// Returns:
//
//    Int32 that is closest to the CostValue or Float64.
//
//------------------------------------------------------------------------------

Int32
CostValue::ConvertToSigned32
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));
   return CostValue::ConvertToSigned32(costValue.AsFloat64());
}

Int32
CostValue::ConvertToSigned32
(
   Float64 value
)
{
   IntValue intValue = CostValue::ConvertToSigned(value);

   if (intValue > Tiled::TypeConstants::MaxInt32)
   {
      return Tiled::TypeConstants::MaxInt32;
   }

   if (intValue < Tiled::TypeConstants::MinInt32)
   {
      return Tiled::TypeConstants::MinInt32;
   }

   return static_cast<Tiled::Int32>(intValue);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert CostValue or 64-bit float to unsigned 64-bit integer.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
// Arguments:
//
//    value -- float64 value
//    costValue -- cost value
//
// Returns:
//
//    UIntValue that is closest to the CostValue or Float64.
//
//------------------------------------------------------------------------------

UIntValue
CostValue::ConvertToUnsigned
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));
   return CostValue::ConvertToUnsigned(costValue.AsFloat64());
}

UIntValue
CostValue::ConvertToUnsigned
(
   Float64 value
)
{
   //return Toolbox::FloatMath::ConvertToUnsigned(value);
   return convertToUnsigned(value);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Convert CostValue or 64-bit float point to unsigned 32-bit integer.
//
// Remarks:
//
//    This function uses platform independent floating point libraries.
//
// Arguments:
//
//    value -- float64 value
//    costValue -- cost value
//
// Returns:
//
//    UInt32 value that is closest to the CostValue or Float64.
//
//------------------------------------------------------------------------------

UInt32
CostValue::ConvertToUnsigned32
(
   Tiled::CostValue costValue
)
{
   assert(!Tiled::CostValue::IsNan(costValue));
   return CostValue::ConvertToUnsigned32(costValue.AsFloat64());
}

UInt32
CostValue::ConvertToUnsigned32
(
   Float64 value
)
{
   UIntValue uintValue = CostValue::ConvertToUnsigned(value);

   if (uintValue > Tiled::TypeConstants::MaxUInt32)
   {
      return Tiled::TypeConstants::MaxUInt32;
   }

   UInt32 result = (UInt32) uintValue;

   return result;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return the maximum value among floatValue1 and floatValue2.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Max
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   if (Tiled::CostValue::CompareGT(costValue1, costValue2))
   {
      return costValue1;
   }
   else
   {
      return costValue2;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Return the minimum value among floatValue1 and floatValue2.
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Min
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   if (Tiled::CostValue::CompareLT(costValue1, costValue2))
   {
      return costValue1;
   }
   else
   {
      return costValue2;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Bound the cost values passed in so that they are between the minimum and
//    maximum, and then apply them to this cost object.
//
// Arguments:
//
//    executionCycles - cost measured in execution cycles
//    codeBytes - cost measured in code bytes
//
//------------------------------------------------------------------------------

void
Cost::SetValues
(
   Tiled::CostValue executionCycles,
   Tiled::CostValue codeBytes
)
{
   this->ExecutionCycles = executionCycles;
   this->CodeBytes = codeBytes;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Initialize the execution cycles and code bytes properties of a cost
//    object.
//
// Arguments:
//
//    executionCycles - cost measured in execution cycles
//    codeBytes - cost measured in code bytes
//
//------------------------------------------------------------------------------

void
Cost::Initialize
(
   Tiled::CostValue executionCycles,
   Tiled::CostValue codeBytes
)
{
   this->SetValues(executionCycles, codeBytes);
   this->IsInitialized = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Copy a cost value.
//
// Arguments:
//
//    sourceCost - Cost object to copy.
//
//------------------------------------------------------------------------------

void
Cost::Copy
(
   const Tiled::Cost * sourceCost
)
{
   assert(sourceCost->IsInitialized);

   this->ExecutionCycles = sourceCost->ExecutionCycles;
   this->CodeBytes = sourceCost->CodeBytes;
   this->IsInitialized = sourceCost->IsInitialized;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set cost to ZERO.
//
//------------------------------------------------------------------------------

void
Cost::SetZero()
{
   this->ExecutionCycles = Tiled::CostValue(0);
   this->CodeBytes = Tiled::CostValue(0);
   this->IsInitialized = true;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set cost to INFINITY.
//
//------------------------------------------------------------------------------

void
Cost::SetInfinity()
{
   this->ExecutionCycles = Tiled::Cost::InfinityValue;
   this->CodeBytes = Tiled::Cost::InfinityValue;
   this->IsInitialized = true;

   //assert(this->IsInfinity);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Set cost to INFINITY.
//
//------------------------------------------------------------------------------

void
Cost::SetNegativeInfinity()
{

   this->ExecutionCycles = Tiled::Cost::NegativeInfinityValue;
   this->CodeBytes = Tiled::Cost::NegativeInfinityValue;
   this->IsInitialized = true;

   //assert(this->IsNegativeInfinity);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Add one cost value to another.
//
// Arguments:
//
//    otherCost - Cost object to increment by.
//
// Remarks:
//
//    Incrementing by INFINITY sets INFINITY.
//
//------------------------------------------------------------------------------

void
Cost::IncrementBy
(
   Tiled::Cost * otherCost
)
{
   assert(this->IsInitialized);
   assert(otherCost->IsInitialized);

   this->ExecutionCycles = Tiled::CostValue::Add(this->ExecutionCycles, otherCost->ExecutionCycles);
   this->CodeBytes = Tiled::CostValue::Add(this->CodeBytes, otherCost->CodeBytes);
}

//------------------------------------------------------------------------------
//
// Description: 
// 
//    Return execution cyles of the cost object.
//
//------------------------------------------------------------------------------

Tiled::CostValue
Cost::GetExecutionCycles()
{
   return this->ExecutionCycles;
}

//------------------------------------------------------------------------------
//
// Description: 
//
//    Set execution cycles of the cost object.
//
//------------------------------------------------------------------------------

void
Cost::SetExecutionCycles
(
   Tiled::CostValue value
)
{
   assert(!Tiled::CostValue::IsNan(value));
   this->ExecutionCycles = value;
}

//------------------------------------------------------------------------------
//
// Description: 
//
//    Return code bytes of the cost object.
//
//------------------------------------------------------------------------------

Tiled::CostValue
Cost::GetCodeBytes()
{
   return this->CodeBytes;
}

//------------------------------------------------------------------------------
//
// Description: 
//
//    Set code bytes of the cost object.
//
//------------------------------------------------------------------------------
#if 0
void
Cost::SetCodeBytes
(
   Tiled::CostValue value
)
{
   assert(!Tiled::CostValue::IsNan(value));
   this->CodeBytes = value;
}
#endif
//------------------------------------------------------------------------------
//
// Description:
//
//    Subtract one cost value from another.
//
// Arguments:
//
//    otherCost - Cost object to decrement by.
//
// Remarks:
//
//    Decrementing by INFINITY sets ZERO.
//
//------------------------------------------------------------------------------

void
Cost::DecrementBy
(
   Tiled::Cost * otherCost
)
{
   assert(this->IsInitialized);
   assert(otherCost->IsInitialized);

   this->ExecutionCycles = Tiled::CostValue::Subtract(this->ExecutionCycles, otherCost->ExecutionCycles);
   this->CodeBytes = Tiled::CostValue::Subtract(this->CodeBytes, otherCost->CodeBytes);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Multiply the cost by a scale factor.
//
// Arguments:
//
//    scaleFactor - the scaling factor.
//
//------------------------------------------------------------------------------

void
Cost::ScaleBy
(
   Tiled::CostValue scaleFactor
)
{
   assert(this->IsInitialized);

   this->ExecutionCycles = Tiled::CostValue::Multiply(ExecutionCycles, scaleFactor);
   this->CodeBytes = Tiled::CostValue::Multiply(CodeBytes, scaleFactor);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Multiply the code bytes by a scale factor.
//
// Arguments:
//
//    scaleFactor - the scaling factor.
//
//------------------------------------------------------------------------------

void
Cost::ScaleCodeBytesBy
(
   Tiled::CostValue scaleFactor
)
{
   assert(this->IsInitialized);

   this->CodeBytes = Tiled::CostValue::Multiply(CodeBytes, scaleFactor);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Multiply the execution cycles by a scale factor.
//
// Arguments:
//
//    scaleFactor - the scaling factor.
//
//------------------------------------------------------------------------------

void
Cost::ScaleExecutionCyclesBy
(
   Tiled::CostValue scaleFactor
)
{
   assert(this->IsInitialized);

   this->ExecutionCycles = Tiled::CostValue::Multiply(ExecutionCycles, scaleFactor);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    divide the cost by an integer divisor.
//
// Arguments:
//
//    divisor - the integer divisor.
//
//------------------------------------------------------------------------------

void
Cost::DivideBy
(
   Tiled::CostValue divisor
)
{
   assert(this->IsInitialized);
   assert(Tiled::CostValue::CompareNE(divisor, Tiled::CostValue(0)) && "Can not divide cost by zero");

   this->ExecutionCycles = Tiled::CostValue::Divide(this->ExecutionCycles, divisor);
   this->CodeBytes = Tiled::CostValue::Divide(this->CodeBytes, divisor);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Scale the execution cycles according to the loop nest level.
//
// Arguments:
//
//    loopNestLevel - the loop nest level
//
//------------------------------------------------------------------------------

void
Cost::ScaleByLoopNestLevel
(
   int loopNestLevel
)
{
   int scaleFactor;

   assert(this->IsInitialized);

   scaleFactor = 1;

   if (loopNestLevel > Cost::MaximumLoopNestLevel)
   {
      loopNestLevel = Cost::MaximumLoopNestLevel;
   }

   for (; loopNestLevel > 0; loopNestLevel--)
   {
      scaleFactor *= Cost::LoopWeightFactor;
   }

   Tiled::CostValue executionCycles = this->ExecutionCycles;
   Tiled::CostValue codeBytes = this->CodeBytes;

   executionCycles = Tiled::CostValue::Multiply(executionCycles, scaleFactor);

   this->SetValues(executionCycles, codeBytes);
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Scale the execution cycles by profile count
//
// Arguments:
//
//   count - profile count
//
//------------------------------------------------------------------------------

void
Cost::ScaleCyclesByFrequency
(
   Profile::Count count
)
{
   assert(this->IsInitialized);

   Tiled::CostValue newExecutionCycles = Tiled::CostValue::Multiply(this->ExecutionCycles, count);

   this->ExecutionCycles = newExecutionCycles;
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Scale the execution cycles by a count relative to another.
//
// Arguments:
//
//   count - Relative profile count.
//   enterCount - Base profile count.
//
//------------------------------------------------------------------------------

void
Cost::ScaleCyclesByFrequency
(
   Profile::Count count,
   Profile::Count enterCount
)
{
   assert(this->IsInitialized);

   if (Tiled::CostValue::IsGreaterThanZero(enterCount))
   {
      Tiled::CostValue newExecutionCycles = Tiled::CostValue::Multiply(this->ExecutionCycles, count);
      Tiled::CostValue scaledExecutionCycles = Tiled::CostValue::Divide(newExecutionCycles, enterCount);

      this->ExecutionCycles = scaledExecutionCycles;
   }
   else
   {
      // If enter count of a function is 0, 
      // profile count of any point within the function must be zero, too.

      assert(Tiled::CostValue::IsZero(count));

      // Since this function is very cold,
      // we set the execution cycle benefit to zero.

      this->ExecutionCycles = 0;
   }
}

//------------------------------------------------------------------------------
//
// Description:
//
//    Amortize the execution cycles by profile count
//
// Arguments:
//
//   hitCount - Hit count.
//   missCount - Miss count.
//
//------------------------------------------------------------------------------

void
Cost::AmortizeCyclesByFrequency
(
   Profile::Count hitCount,
   Profile::Count missCount
)
{
   assert(this->IsInitialized);

   // executionCycles = executionCycles * hitCount / (hitCount + missCount)

   Tiled::CostValue executionCycles(this->ExecutionCycles);
   Tiled::CostValue hitExecutionCycles = Tiled::CostValue::Multiply(executionCycles, hitCount);
   Tiled::CostValue totalCycles = Tiled::CostValue::Add(hitCount, missCount);
   Tiled::CostValue newExecutionCycles = Tiled::CostValue::Divide(hitExecutionCycles, totalCycles);

   this->ExecutionCycles = newExecutionCycles;
}

//------------------------------------------------------------------------------
//
//  Description:
//
//     Negate cost
//
//------------------------------------------------------------------------------

void
Cost::Negate()
{
   Tiled::CostValue executionCycles(this->ExecutionCycles);
   Tiled::CostValue codeBytes(this->CodeBytes);

   this->ExecutionCycles = Tiled::CostValue::Subtract(Tiled::CostValue(0), executionCycles);
   this->CodeBytes = Tiled::CostValue::Subtract(Tiled::CostValue(0), codeBytes);
}

//------------------------------------------------------------------------------
//
//  Description:
//
//     Return true if the cost object is infinite in both execution cycles and
//     code bytes.
//
//------------------------------------------------------------------------------

bool
Cost::IsInfinity()
{
   Tiled::CostValue executionCycles(this->ExecutionCycles);
   Tiled::CostValue codeBytes(this->CodeBytes);

   return Tiled::CostValue::IsInfinity(executionCycles)
      && Tiled::CostValue::IsInfinity(codeBytes);
}

//------------------------------------------------------------------------------
//
//  Description: 
//
//     Return true if the given FP value is inifinity.
//     Otherwise, return false.
//
//------------------------------------------------------------------------------

bool
CostValue::IsInfinity
(
   Tiled::CostValue costValue
)
{
   Tiled::Float64 floatValue = costValue.AsFloat64();

   return *(reinterpret_cast<Tiled::Int64*>(&floatValue))
      == Tiled::TypeConstants::Float64InfinityPattern;
}

//------------------------------------------------------------------------------
//
//  Description:
//
//     Return true if the given FP value is QNAN.
//     Otherwise, return false.
//
//  Remarks:
//
//     The following function may not work with big endian machines.
//
//------------------------------------------------------------------------------

bool
CostValue::IsQuietNan
(
   Tiled::CostValue costValue
)
{
   Tiled::Float64 floatValue = costValue.AsFloat64();

   Tiled::UInt16 tmp = *(reinterpret_cast<Tiled::UInt16 *>(&floatValue) + 3);
   return (tmp & 0x7ff8) == 0x7ff8;
}

//------------------------------------------------------------------------------
//
//  Description: 
//
//     Return true if the given FP value is SNAN.
//     Otherwise, return false.
//
//  Remarks:
//
//     The following function may not work with big endian machines.
//
//------------------------------------------------------------------------------

bool
CostValue::IsSignalingNan
(
   Tiled::CostValue costValue
)
{
   Tiled::Float64 floatValue = costValue.AsFloat64();

   Tiled::UInt16 tmp1 = *(reinterpret_cast<Tiled::UInt16 *>(&floatValue) + 3);
   Tiled::UInt32 tmp2 = *(reinterpret_cast<Tiled::UInt32 *>(&floatValue) + 1);
   Tiled::UInt32 tmp3 = *(reinterpret_cast<Tiled::UInt32 *>(&floatValue));

   return ((tmp1 & 0x7ff8) == 0x7ff0) & ((tmp2 << 13 != 0) | (tmp3 != 0));
}

//------------------------------------------------------------------------------
//
//  Description: 
//
//     Return true if the given FP value is NAN.
//     Otherwise, return false.
//
//  Remarks:
//
//     The following function may not work with big endian machines.
//
//------------------------------------------------------------------------------

bool
CostValue::IsNan
(
   Tiled::CostValue costValue
)
{
   return CostValue::IsQuietNan(costValue) | CostValue::IsSignalingNan(costValue);
}

//------------------------------------------------------------------------------
//
//  Description:
//
//     Return true if the given FP value is zero
//     Otherwise, return false.
//
//------------------------------------------------------------------------------

bool
CostValue::IsZero
(
   Tiled::CostValue costValue
)
{
   // May want epsilon here.

   return CostValue::CompareEQ(costValue, 0.0);
}

//------------------------------------------------------------------------------
//
//  Description: 
//
//     Compute absolute value of the given cost.
//
//  Arguments:
//  
//     costValue - cost value
//
//  Returns:
//
//     absolute value of the cost value
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Abs
(
   Tiled::CostValue costValue
)
{
   if (CostValue::IsGreaterThanOrEqualToZero(costValue))
   {
      return costValue;
   }

   return CostValue::Multiply(costValue, -1);
}

//------------------------------------------------------------------------------
//
//  Description:
//
//     Compute difference between two cost values.
//
//  Arguments:
//
//     costValue1 - first cost value
//     costValue2 - second cost value
//
//  Returns:
//
//     difference between two cost values (always non-zero)
//
//------------------------------------------------------------------------------

Tiled::CostValue
CostValue::Diff
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2
)
{
   return CostValue::Abs(CostValue::Subtract(costValue1, costValue2));
}

//------------------------------------------------------------------------------
//
//  Description: 
//
//     Check if the difference between given values are within
//     the error margin.
//
//------------------------------------------------------------------------------

#if 0
bool
CostValue::IsWithinErrorMargin
(
   Tiled::CostValue costValue1,
   Tiled::CostValue costValue2,
   Tiled::Float64   errorMargin
)
{
   assert(Toolbox::FloatMath::CompareGE(errorMargin, 0.0));

   Tiled::CostValue absCostValue1 = CostValue::Abs(costValue1);
   Tiled::CostValue absCostValue2 = CostValue::Abs(costValue2);

   Tiled::CostValue error;

   if (CostValue::CompareLE(absCostValue2, errorMargin))
   {
      // If costValue2 is less than error margin, 
      // use difference between values as error.
      // This covers a case that costValue2 is zero.

      error = CostValue::Diff(costValue1, costValue2);
   }
   else
   {
      // Otherwise, use difference between 1 and absCostValue1 / absCostValue2 as error.

      Tiled::CostValue ratio = CostValue::Divide(costValue1, costValue2);
      error = CostValue::Diff(1, ratio);
   }

   return CostValue::CompareLE(error, errorMargin);
}
#endif

} // namespace Tiled
