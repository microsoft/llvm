//===- LowerBitSets.cpp - Unit tests for bitset lowering ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/LowerBitSets.h"
#include "gtest/gtest.h"

using namespace llvm;

TEST(LowerBitSets, BitSetBuilder) {
  struct {
    std::vector<uint64_t> Offsets;
    std::vector<uint8_t> Bits;
    uint64_t ByteOffset;
    uint64_t BitSize;
    unsigned AlignLog2;
    bool IsSingleOffset;
  } BSBTests[] = {
      {{}, {0}, 0, 1, 0, false},
      {{0}, {1}, 0, 1, 0, true},
      {{4}, {1}, 4, 1, 0, true},
      {{37}, {1}, 37, 1, 0, true},
      {{0, 1}, {3}, 0, 2, 0, false},
      {{0, 4}, {3}, 0, 2, 2, false},
      {{0, uint64_t(1) << 33}, {3}, 0, 2, 33, false},
      {{3, 7}, {3}, 3, 2, 2, false},
      {{0, 1, 7}, {131}, 0, 8, 0, false},
      {{0, 2, 14}, {131}, 0, 8, 1, false},
      {{0, 1, 8}, {3, 1}, 0, 9, 0, false},
      {{0, 2, 16}, {3, 1}, 0, 9, 1, false},
  };

  for (auto &&T : BSBTests) {
    BitSetBuilder BSB;
    for (auto Offset : T.Offsets)
      BSB.addOffset(Offset);

    BitSetInfo BSI = BSB.build();

    EXPECT_EQ(T.Bits, BSI.Bits);
    EXPECT_EQ(T.ByteOffset, BSI.ByteOffset);
    EXPECT_EQ(T.BitSize, BSI.BitSize);
    EXPECT_EQ(T.AlignLog2, BSI.AlignLog2);
    EXPECT_EQ(T.IsSingleOffset, BSI.isSingleOffset());

    for (auto Offset : T.Offsets)
      EXPECT_TRUE(BSI.containsGlobalOffset(Offset));

    auto I = T.Offsets.begin();
    for (uint64_t NonOffset = 0; NonOffset != 256; ++NonOffset) {
      if (I != T.Offsets.end() && *I == NonOffset) {
        ++I;
        continue;
      }

      EXPECT_FALSE(BSI.containsGlobalOffset(NonOffset));
    }
  }
}
