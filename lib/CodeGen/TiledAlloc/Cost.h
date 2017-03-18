//===-- Cost.h - Cost Information -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Cost information is fully defined by the target.  The details of what is
// and isn't measured (execution cycles, code bytes, register pressure,
// etc.), or how they are combined is irrelavant at this level.  API's are
// provided to allow basic arithmetic and comparison operations.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_COSTONE_H
#define TILED_COSTONE_H

#include "Typedefs.h"

namespace Tiled
{

//------------------------------------------------------------------------------
//
// Description: 
//
// This is a class that represents a cost value.
// Cost values are essentially double precision floating points
// but using this class enforces all cost-related computation
// to be performed with APIs defined in this class. 
//
// Remarks:
// 
// In many places, infinite (or negative infinite) costs are used
// to express extremely strong preferences.
// In most cases, floating point computation naturally handles infinite
// values but there are possibilities that it could lead to generation of NaN.
//
// To prevent such cases, we enforce a special semantics for arithematics
// with infinite values (add/sub/mul/div).
//
//------------------------------------------------------------------------------

class CostValue
{

public:

   CostValue() : floatValue(0.0) { }

   // Static functions that performs computation on cost value.

   static Tiled::CostValue
   Abs
   (
      Tiled::CostValue costValue
   );

   static Tiled::CostValue
   Add
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   Add
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2,
      Tiled::CostValue costValue3
   );

   static bool
   CompareEQ
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   CompareGE
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   CompareGT
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   CompareLE
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   CompareLT
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   CompareNE
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   ConvertFromSigned
   (
      IntValue intValue
   );

   static Tiled::CostValue
   ConvertFromUnsigned
   (
      UIntValue intValue
   );

   static IntValue
   ConvertToSigned
   (
      Tiled::CostValue costValue
   );

   static IntValue
   ConvertToSigned
   (
      double value
   );

   static Int32
   ConvertToSigned32
   (
      Tiled::CostValue costValue
   );

   static Int32
   ConvertToSigned32
   (
      double value
   );

   static UIntValue
   ConvertToUnsigned
   (
      Tiled::CostValue costValue
   );

   static UIntValue
   ConvertToUnsigned
   (
      double value
   );

   static UInt32
   ConvertToUnsigned32
   (
      Tiled::CostValue costValue
   );

   static UInt32
   ConvertToUnsigned32
   (
      double value
   );

   CostValue
   (
      double floatValue
   );

   static Tiled::CostValue
   Diff
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   Divide
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static bool
   IsGreaterThanOrEqualToZero
   (
      Tiled::CostValue costValue
   );

   static bool
   IsGreaterThanZero
   (
      Tiled::CostValue costValue
   );

   static bool
   IsInfinity
   (
      Tiled::CostValue costValue
   );

   static bool
   IsNan
   (
      Tiled::CostValue costValue
   );

   static bool
   IsQuietNan
   (
      Tiled::CostValue costValue
   );

   static bool
   IsSignalingNan
   (
      Tiled::CostValue costValue
   );

   static bool
   IsWithinErrorMargin
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2,
      double         errorMargin
   );

   static bool
   IsZero
   (
      Tiled::CostValue costValue
   );

   static Tiled::CostValue
   Max
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   Min
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   Multiply
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

   static Tiled::CostValue
   Multiply
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2,
      Tiled::CostValue costValue3
   );

   static Tiled::CostValue
   Negate
   (
      Tiled::CostValue costValue
   );

   static Tiled::CostValue
   Subtract
   (
      Tiled::CostValue costValue1,
      Tiled::CostValue costValue2
   );

public:

   double floatValue;
   
   Float64 AsFloat64() { return floatValue; }
};

namespace Profile
{
   typedef CostValue Count;       // Profile data counts
}

class Cost
{
   friend class CostModel;

public:

   Cost() : IsInitialized(false), CodeBytes(), ExecutionCycles() {};

   void
   AmortizeCyclesByFrequency
   (
      Profile::Count hitCount,
      Profile::Count missCount
   );

   void
   Copy
   (
      const Tiled::Cost * sourceCost
   );

   void
   DecrementBy
   (
      Tiled::Cost * otherCost
   );

   void
   DivideBy
   (
      Tiled::CostValue divisor
   );

   Tiled::CostValue GetCodeBytes();

   Tiled::CostValue GetExecutionCycles();

   void
   SetExecutionCycles
   (
      Tiled::CostValue value
   );

   //[dump] void Dump();

   void
   IncrementBy
   (
      Tiled::Cost * otherCost
   );

   void
   Initialize
   (
      Tiled::CostValue executionCycles,
      Tiled::CostValue codeBytes
   );


   bool IsInfinity();

   void Negate();

   void
   ScaleBy
   (
      Tiled::CostValue scaleFactor
   );

   void
   ScaleByLoopNestLevel
   (
      int loopNestLevel
   );

   void
   ScaleCodeBytesBy
   (
      Tiled::CostValue scaleFactor
   );

   void
   ScaleCyclesByFrequency
   (
      Profile::Count count
   );

   void
   ScaleCyclesByFrequency
   (
      Profile::Count count,
      Profile::Count enterCount
   );

   void
   ScaleExecutionCyclesBy
   (
      Tiled::CostValue scaleFactor
   );

   void SetInfinity();

   void SetNegativeInfinity();

   void SetZero();

   static void StaticInitialize();

public:

   bool              IsInitialized;

public:

   static Tiled::CostValue InfinityValue;
   static Tiled::CostValue NegativeInfinityValue;
   static Tiled::Cost      InfiniteCost;
   static Tiled::Cost      NegativeInfiniteCost;
   static Tiled::Cost      ZeroCost;

private:

   void
   SetValues
   (
      Tiled::CostValue executionCycles,
      Tiled::CostValue codeBytes
   );

private:

   Tiled::CostValue        CodeBytes;
   Tiled::CostValue        ExecutionCycles;
   static const int        LoopWeightFactor = 4;
   static const int        MaximumLoopNestLevel = 15;
};

#if 0
comment Cost::CodeBytes
{
   // Description:
   //
   //    The size-based component of the cost.
}

comment Cost::ExecutionCycles
{
   // Description:
   //
   //    The speed-based component of the cost.
}

comment Cost::IsInitialized
{
   // Description:
   //
   //    True if the cost has been defined.
}
#endif

//-----------------------------------------------------------------------------
//
// Description:
//
//    An abstract class for modeling costs.
//
// Remarks:
//
//    Provides a set of interfaces used to compare and evaluate costs.
//
//-----------------------------------------------------------------------------

class CostModel
{

public:

   virtual int
   Compare
   (
      Tiled::Cost * firstCost,
      Tiled::Cost * secondCost
   );

   virtual Tiled::CostValue
   Evaluate
   (
      Tiled::Cost * cost
   ) = 0;

protected:

   virtual bool
   AreWithinError
   (
      Tiled::CostValue firstCostValue,
      Tiled::CostValue secondCostValue
   );
};

} // namespace Tiled

#endif // end TILED_COST_H
