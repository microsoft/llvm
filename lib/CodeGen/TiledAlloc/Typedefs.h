//===-- Typedefs.h - C++ typedefs -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// ISSUE-TODO-aasmith-2017/02/18: Remove this file

#ifndef TILED_TYPEDEFS_H
#define TILED_TYPEDEFS_H

#include "llvm/ADT/APInt.h"

namespace Tiled
{

typedef int8_t           Int8;
typedef int16_t          Int16;
typedef int32_t          Int32;
typedef int64_t          Int64;
typedef uint8_t          UInt8;
typedef uint16_t         UInt16;
typedef uint32_t         UInt32;
typedef uint64_t         UInt64;
typedef float            Float32;
typedef double           Float64;

typedef bool             Boolean;

typedef size_t           Size;

#if defined(_WIN64) || defined(__APPLE__)
typedef Int64            IntPointer;
#elif defined(_WIN32)
typedef Int32            IntPointer;
#else
#error IntPointer Undefined
#error Int Undefined
#endif

typedef int              Int; // Generic signed integer
typedef unsigned int     UInt; // Generic unsigned integer

// Value Primitives

typedef UInt32           Id; // Identifier
typedef Int32            PrimitiveTypeKindNumber; // needs to be int for extensibility
typedef UInt32           BitNumber;  // unsigned for ease of bit vector use
typedef Int64            IntValue; // IR operand signed integer
typedef UInt64           UIntValue; // IR operand unsigned integer
typedef double           FloatValue; // IR operand Floating point value
typedef Int32            BitOffset; // An offset in bits
typedef Int32            ByteOffset; // An offset in bytes
typedef UInt32           BitSize; // A size in bits
typedef UInt32           ByteSize; // An size in bytes

//-----------------------------------------------------------------------------
//
// Description:
//
//   Some constants for the value primitives.
//
// Internal:
//
//    These are here instead of in the Constants enum so that
//    - they are nearby if the underlying type changes
//    - they can all have precise types (const vs. enum member)
//
//    Using const means no metadata information, but since BitOffset doesn't
//    exist in metadata to begin with, it's not a big loss.
//
//-----------------------------------------------------------------------------

namespace TypeConstants
{

//--------------------------------------------------------------------------
//
// Description:
//
//    Max unsigned integer.
//
//--------------------------------------------------------------------------
const uint32_t  MaxUInt = static_cast<Tiled::UInt>(-1);

//--------------------------------------------------------------------------
//
// Description:
//
//    Max signed 32-bit integer.
//
//--------------------------------------------------------------------------
const int32_t  MaxInt32 = static_cast<Tiled::Int32>(0x7fffffff);

//--------------------------------------------------------------------------
//
// Description:
//
//    Min signed 32-bit integer.
//
//--------------------------------------------------------------------------
const int32_t  MinInt32 = static_cast<Tiled::Int32>(0x80000000);

//--------------------------------------------------------------------------
//
// Description:
//
//    Max unsigned 32-bit integer.
//
//--------------------------------------------------------------------------
const uint32_t MaxUInt32 = static_cast<Tiled::UInt32>(0xFFFFFFFF);

//--------------------------------------------------------------------------
//
// Description:
//
//    Bit pattern for Float64 infinify
//
//--------------------------------------------------------------------------
const Tiled::Int64 Float64InfinityPattern = 0x7FF0000000000000;

//--------------------------------------------------------------------------
//
// Description:
//
//    Bit pattern for Float64 negative infinify
//
//--------------------------------------------------------------------------
const Tiled::Int64 Float64NegativeInfinityPattern = 0xFFF0000000000000;

//--------------------------------------------------------------------------
//
// Description:
//
//    Sign bit mask of Float64
//
//--------------------------------------------------------------------------
const Tiled::Int64 Float64SignBitMask = 0x8000000000000000;

} // namespace TypeConstants

namespace Alias
{

typedef Tiled::Int32 Tag;                 // Currently must be signed.

} // namespace Alias

namespace SSA
{

// SSA Tag is not identical with aliasTAG
// Currently, it is AliasTag + Number of Alias Tag Bias

typedef Tiled::UInt32 Tag;

} // namespace Alias

namespace BitVector
{

// Set the size of the Bit Vector Chunks

#if defined(_WIN64)

// 64bit chunks a nice win on 64bit platforms
#define TILED_BIT_VECTOR_CHUNK64
typedef uint64_t  Bits;

#elif defined(_WIN32)

#define TILED_BIT_VECTOR_CHUNK32
typedef uint32_t  Bits;

#else

#define TILED_BIT_VECTOR_CHUNK32
typedef uint32_t  Bits;

#endif

//typedef Tiled::BitNumber FixedPosition; // Fixed bit vector iterator positon

}

class CostValue;     // fwd declare for Profile::COunt

namespace Profile
{
typedef CostValue Count;       // Profile data counts
typedef Float64   Probability; // probability
typedef int64_t   ProbeKey;
typedef int64_t   ProbeOffset;
typedef int32_t   Tag;         // Tag to lookup profile data
}

} // namespace Tiled

#endif // end TILED_TYPEDEFS_H
