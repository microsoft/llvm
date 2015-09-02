//===-------- StackMapParser.h - StackMap Parsing Support -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_STACKMAPPARSER_H
#define LLVM_CODEGEN_STACKMAPPARSER_H

#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include <map>
#include <vector>

namespace llvm {

template <support::endianness Endianness>
class StackMapV1Parser {
public:

  template <typename AccessorT>
  class AccessorIterator {
  public:

    AccessorIterator(AccessorT A) : A(A) {}
    AccessorIterator& operator++() { A = A.next(); return *this; }
    AccessorIterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }

    bool operator==(const AccessorIterator &Other) {
      return A.P == Other.A.P;
    }

    bool operator!=(const AccessorIterator &Other) { return !(*this == Other); }

    AccessorT& operator*() { return A; }
    AccessorT* operator->() { return &A; }

  private:
    AccessorT A;
  };

  /// Accessor for function records.
  class FunctionAccessor {
    friend class StackMapV1Parser;
  public:

    /// Get the function address.
    uint64_t getFunctionAddress() const {
      return read<uint64_t>(P);
    }

    /// Get the function's stack size.
    uint32_t getStackSize() const {
      return read<uint64_t>(P + sizeof(uint64_t));
    }

  private:
    FunctionAccessor(const uint8_t *P) : P(P) {}

    const static int FunctionAccessorSize = 2 * sizeof(uint64_t);

    FunctionAccessor next() const {
      return FunctionAccessor(P + FunctionAccessorSize);
    }

    const uint8_t *P;
  };

  /// Accessor for constants.
  class ConstantAccessor {
    friend class StackMapV1Parser;
  public:

    /// Return the value of this constant.
    uint64_t getValue() const { return read<uint64_t>(P); }

  private:

    ConstantAccessor(const uint8_t *P) : P(P) {}

    const static int ConstantAccessorSize = sizeof(uint64_t);

    ConstantAccessor next() const {
      return ConstantAccessor(P + ConstantAccessorSize);
    }

    const uint8_t *P;
  };

  // Forward-declare RecordAccessor so we can friend it below.
  class RecordAccessor;

  enum class LocationKind : uint8_t {
    Register = 1, Direct = 2, Indirect = 3, Constant = 4, ConstantIndex = 5
  };


  /// Accessor for location records.
  class LocationAccessor {
    friend class StackMapV1Parser;
    friend class RecordAccessor;
  public:

    /// Get the Kind for this location.
    LocationKind getKind() const {
      return LocationKind(P[KindOffset]);
    }

    /// Get the Dwarf register number for this location.
    uint16_t getDwarfRegNum() const {
      return read<uint16_t>(P + DwarfRegNumOffset);
    }

    /// Get the small-constant for this location. (Kind must be Constant).
    uint32_t getSmallConstant() const {
      assert(getKind() == LocationKind::Constant && "Not a small constant.");
      return read<uint32_t>(P + SmallConstantOffset);
    }

    /// Get the constant-index for this location. (Kind must be ConstantIndex).
    uint32_t getConstantIndex() const {
      assert(getKind() == LocationKind::ConstantIndex &&
             "Not a constant-index.");
      return read<uint32_t>(P + SmallConstantOffset);
    }

    /// Get the offset for this location. (Kind must be Direct or Indirect).
    int32_t getOffset() const {
      assert((getKind() == LocationKind::Direct ||
              getKind() == LocationKind::Indirect) &&
             "Not direct or indirect.");
      return read<int32_t>(P + SmallConstantOffset);
    }

  private:

    LocationAccessor(const uint8_t *P) : P(P) {}

    LocationAccessor next() const {
      return LocationAccessor(P + LocationAccessorSize);
    }

    static const int KindOffset = 0;
    static const int DwarfRegNumOffset = KindOffset + sizeof(uint16_t);
    static const int SmallConstantOffset = DwarfRegNumOffset + sizeof(uint16_t);
    static const int LocationAccessorSize = sizeof(uint64_t);

    const uint8_t *P;
  };

  /// Accessor for stackmap live-out fields.
  class LiveOutAccessor {
    friend class StackMapV1Parser;
    friend class RecordAccessor;
  public:

    /// Get the Dwarf register number for this live-out.
    uint16_t getDwarfRegNum() const {
      return read<uint16_t>(P + DwarfRegNumOffset);
    }

    /// Get the size in bytes of live [sub]register.
    unsigned getSizeInBytes() const {
      return read<uint8_t>(P + SizeOffset);
    }

  private:

    LiveOutAccessor(const uint8_t *P) : P(P) {}

    LiveOutAccessor next() const {
      return LiveOutAccessor(P + LiveOutAccessorSize);
    }

    static const int DwarfRegNumOffset = 0;
    static const int SizeOffset =
      DwarfRegNumOffset + sizeof(uint16_t) + sizeof(uint8_t);
    static const int LiveOutAccessorSize = sizeof(uint32_t);

    const uint8_t *P;
  };

  /// Accessor for stackmap records.
  class RecordAccessor {
    friend class StackMapV1Parser;
  public:

    typedef AccessorIterator<LocationAccessor> location_iterator;
    typedef AccessorIterator<LiveOutAccessor> liveout_iterator;

    /// Get the patchpoint/stackmap ID for this record.
    uint64_t getID() const {
      return read<uint64_t>(P + PatchpointIDOffset);
    }

    /// Get the instruction offset (from the start of the containing function)
    /// for this record.
    uint32_t getInstructionOffset() const {
      return read<uint32_t>(P + InstructionOffsetOffset);
    }

    /// Get the number of locations contained in this record.
    uint16_t getNumLocations() const {
      return read<uint16_t>(P + NumLocationsOffset);
    }

    /// Get the location with the given index.
    LocationAccessor getLocation(unsigned LocationIndex) const {
      unsigned LocationOffset =
        LocationListOffset + LocationIndex * LocationSize;
      return LocationAccessor(P + LocationOffset);
    }

    /// Begin iterator for locations.
    location_iterator location_begin() const {
      return location_iterator(getLocation(0));
    }

    /// End iterator for locations.
    location_iterator location_end() const {
      return location_iterator(getLocation(getNumLocations()));
    }

    /// Iterator range for locations.
    iterator_range<location_iterator> locations() const {
      return make_range(location_begin(), location_end());
    }

    /// Get the number of liveouts contained in this record.
    uint16_t getNumLiveOuts() const {
      return read<uint16_t>(P + getNumLiveOutsOffset());
    }

    /// Get the live-out with the given index.
    LiveOutAccessor getLiveOut(unsigned LiveOutIndex) const {
      unsigned LiveOutOffset =
        getNumLiveOutsOffset() + sizeof(uint16_t) + LiveOutIndex * LiveOutSize;
      return LiveOutAccessor(P + LiveOutOffset);
    }

    /// Begin iterator for live-outs.
    liveout_iterator liveouts_begin() const {
      return liveout_iterator(getLiveOut(0));
    }


    /// End iterator for live-outs.
    liveout_iterator liveouts_end() const {
      return liveout_iterator(getLiveOut(getNumLiveOuts()));
    }

    /// Iterator range for live-outs.
    iterator_range<liveout_iterator> liveouts() const {
      return make_range(liveouts_begin(), liveouts_end());
    }

  private:

    RecordAccessor(const uint8_t *P) : P(P) {}

    unsigned getNumLiveOutsOffset() const {
      return LocationListOffset + LocationSize * getNumLocations() +
             sizeof(uint16_t);
    }

    unsigned getSizeInBytes() const {
      unsigned RecordSize =
        getNumLiveOutsOffset() + sizeof(uint16_t) + getNumLiveOuts() * LiveOutSize;
      return (RecordSize + 7) & ~0x7;
    }

    RecordAccessor next() const {
      return RecordAccessor(P + getSizeInBytes());
    }

    static const unsigned PatchpointIDOffset = 0;
    static const unsigned InstructionOffsetOffset =
      PatchpointIDOffset + sizeof(uint64_t);
    static const unsigned NumLocationsOffset =
      InstructionOffsetOffset + sizeof(uint32_t) + sizeof(uint16_t);
    static const unsigned LocationListOffset =
      NumLocationsOffset + sizeof(uint16_t);
    static const unsigned LocationSize = sizeof(uint64_t);
    static const unsigned LiveOutSize = sizeof(uint32_t);

    const uint8_t *P;
  };

  /// Construct a parser for a version-1 stackmap. StackMap data will be read
  /// from the given array.
  StackMapV1Parser(ArrayRef<uint8_t> StackMapSection)
      : StackMapSection(StackMapSection) {
    ConstantsListOffset = FunctionListOffset + getNumFunctions() * FunctionSize;

    assert(StackMapSection[0] == 1 &&
           "StackMapV1Parser can only parse version 1 stackmaps");

    unsigned CurrentRecordOffset =
      ConstantsListOffset + getNumConstants() * ConstantSize;

    for (unsigned I = 0, E = getNumRecords(); I != E; ++I) {
      StackMapRecordOffsets.push_back(CurrentRecordOffset);
      CurrentRecordOffset +=
        RecordAccessor(&StackMapSection[CurrentRecordOffset]).getSizeInBytes();
    }
  }

  typedef AccessorIterator<FunctionAccessor> function_iterator;
  typedef AccessorIterator<ConstantAccessor> constant_iterator;
  typedef AccessorIterator<RecordAccessor> record_iterator;

  /// Get the version number of this stackmap. (Always returns 1).
  unsigned getVersion() const { return 1; }

  /// Get the number of functions in the stack map.
  uint32_t getNumFunctions() const {
    return read<uint32_t>(&StackMapSection[NumFunctionsOffset]);
  }

  /// Get the number of large constants in the stack map.
  uint32_t getNumConstants() const {
    return read<uint32_t>(&StackMapSection[NumConstantsOffset]);
  }

  /// Get the number of stackmap records in the stackmap.
  uint32_t getNumRecords() const {
    return read<uint32_t>(&StackMapSection[NumRecordsOffset]);
  }

  /// Return an FunctionAccessor for the given function index.
  FunctionAccessor getFunction(unsigned FunctionIndex) const {
    return FunctionAccessor(StackMapSection.data() +
                            getFunctionOffset(FunctionIndex));
  }

  /// Begin iterator for functions.
  function_iterator functions_begin() const {
    return function_iterator(getFunction(0));
  }

  /// End iterator for functions.
  function_iterator functions_end() const {
    return function_iterator(
             FunctionAccessor(StackMapSection.data() +
                              getFunctionOffset(getNumFunctions())));
  }

  /// Iterator range for functions.
  iterator_range<function_iterator> functions() const {
    return make_range(functions_begin(), functions_end());
  }

  /// Return the large constant at the given index.
  ConstantAccessor getConstant(unsigned ConstantIndex) const {
    return ConstantAccessor(StackMapSection.data() +
                            getConstantOffset(ConstantIndex));
  }

  /// Begin iterator for constants.
  constant_iterator constants_begin() const {
    return constant_iterator(getConstant(0));
  }

  /// End iterator for constants.
  constant_iterator constants_end() const {
    return constant_iterator(
             ConstantAccessor(StackMapSection.data() +
                              getConstantOffset(getNumConstants())));
  }

  /// Iterator range for constants.
  iterator_range<constant_iterator> constants() const {
    return make_range(constants_begin(), constants_end());
  }

  /// Return a RecordAccessor for the given record index.
  RecordAccessor getRecord(unsigned RecordIndex) const {
    std::size_t RecordOffset = StackMapRecordOffsets[RecordIndex];
    return RecordAccessor(StackMapSection.data() + RecordOffset);
  }

  /// Begin iterator for records.
  record_iterator records_begin() const {
    if (getNumRecords() == 0)
      return record_iterator(RecordAccessor(nullptr));
    return record_iterator(getRecord(0));
  }

  /// End iterator for records.
  record_iterator records_end() const {
    // Records need to be handled specially, since we cache the start addresses
    // for them: We can't just compute the 1-past-the-end address, we have to
    // look at the last record and use the 'next' method.
    if (getNumRecords() == 0)
      return record_iterator(RecordAccessor(nullptr));
    return record_iterator(getRecord(getNumRecords() - 1).next());
  }

  /// Iterator range for records.
  iterator_range<record_iterator> records() const {
    return make_range(records_begin(), records_end());
  }

private:

  template <typename T>
  static T read(const uint8_t *P) {
    return support::endian::read<T, Endianness, 1>(P);
  }

  static const unsigned HeaderOffset = 0;
  static const unsigned NumFunctionsOffset = HeaderOffset + sizeof(uint32_t);
  static const unsigned NumConstantsOffset = NumFunctionsOffset + sizeof(uint32_t);
  static const unsigned NumRecordsOffset = NumConstantsOffset + sizeof(uint32_t);
  static const unsigned FunctionListOffset = NumRecordsOffset + sizeof(uint32_t);

  static const unsigned FunctionSize = 2 * sizeof(uint64_t);
  static const unsigned ConstantSize = sizeof(uint64_t);

  std::size_t getFunctionOffset(unsigned FunctionIndex) const {
    return FunctionListOffset + FunctionIndex * FunctionSize;
  }

  std::size_t getConstantOffset(unsigned ConstantIndex) const {
    return ConstantsListOffset + ConstantIndex * ConstantSize;
  }

  ArrayRef<uint8_t> StackMapSection;
  unsigned ConstantsListOffset;
  std::vector<unsigned> StackMapRecordOffsets;
};

template <support::endianness Endianness> class StackMapV2Parser {
public:
  template <typename AccessorT> class AccessorIterator {
  public:
    AccessorIterator(AccessorT A) : A(A) {}
    AccessorIterator &operator++() {
      A = A.next();
      return *this;
    }
    AccessorIterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }

    bool operator==(const AccessorIterator &Other) { return A.P == Other.A.P; }

    bool operator!=(const AccessorIterator &Other) { return !(*this == Other); }

    AccessorT &operator*() { return A; }
    AccessorT *operator->() { return &A; }

  private:
    AccessorT A;
  };

  /// Accessor for constants.
  class ConstantAccessor {
    friend class StackMapV2Parser;

  public:
    /// Return the value of this constant.
    uint64_t getValue() const { return read<uint64_t>(P); }

  private:
    ConstantAccessor(const uint8_t *P) : P(P) {}

    ConstantAccessor next() const {
      return ConstantAccessor(P + StackMapV2Parser::ConstantAccessorSize);
    }

    const uint8_t *P;
  };

  // Forward-declare RecordAccessor so we can friend it below.
  class StackMapRecordAccessor;

  enum class LocationKind : uint8_t {
    Register = 1,
    Direct = 2,
    Indirect = 3,
    Constant = 4,
    ConstantIndex = 5
  };

  /// Accessor for location records.
  class LocationAccessor {
    friend class StackMapV2Parser;
    friend class StackMapRecordAccessor;

  public:
    /// Get the Kind for this location.
    LocationKind getKind() const { return LocationKind(P[KindOffset]); }

    /// Get the Dwarf register number for this location.
    uint16_t getDwarfRegNum() const {
      return read<uint16_t>(P + DwarfRegNumOffset);
    }

    /// Get the small-constant for this location. (Kind must be Constant).
    uint32_t getSmallConstant() const {
      assert(getKind() == LocationKind::Constant && "Not a small constant.");
      return read<uint32_t>(P + SmallConstantOffset);
    }

    /// Get the constant-index for this location. (Kind must be ConstantIndex).
    uint32_t getConstantIndex() const {
      assert(getKind() == LocationKind::ConstantIndex &&
             "Not a constant-index.");
      return read<uint32_t>(P + SmallConstantOffset);
    }

    /// Get the offset for this location. (Kind must be Direct or Indirect).
    int32_t getOffset() const {
      assert((getKind() == LocationKind::Direct ||
              getKind() == LocationKind::Indirect) &&
             "Not direct or indirect.");
      return read<int32_t>(P + SmallConstantOffset);
    }

  private:
    LocationAccessor(const uint8_t *P) : P(P) {}

    LocationAccessor next() const {
      return LocationAccessor(P + StackMapV2Parser::LocationAccessorSize);
    }

    static const int KindOffset = 0;
    static const int DwarfRegNumOffset = KindOffset + sizeof(uint16_t);
    static const int SmallConstantOffset = DwarfRegNumOffset + sizeof(uint16_t);

    const uint8_t *P;
  };

  /// Accessor for stackmap live-out fields.
  class LiveOutAccessor {
    friend class StackMapV2Parser;
    friend class StackMapRecordAccessor;

  public:
    /// Get the Dwarf register number for this live-out.
    uint16_t getDwarfRegNum() const {
      return read<uint16_t>(P + DwarfRegNumOffset);
    }

    /// Get the size in bytes of live [sub]register.
    unsigned getSizeInBytes() const { return read<uint8_t>(P + SizeOffset); }

  private:
    LiveOutAccessor(const uint8_t *P) : P(P) {}

    LiveOutAccessor next() const {
      return LiveOutAccessor(P + StackMapV2Parser::LiveOutAccessorSize);
    }

    static const int DwarfRegNumOffset = 0;
    static const int SizeOffset =
        DwarfRegNumOffset + sizeof(uint16_t) + sizeof(uint8_t);

    const uint8_t *P;
  };

  /// Accessor for stackmap records.
  class StackMapRecordAccessor {
    friend class StackMapV2Parser;

  public:
    typedef AccessorIterator<LocationAccessor> location_iterator;
    typedef AccessorIterator<LiveOutAccessor> liveout_iterator;

    /// Get the patchpoint/stackmap ID for this record.
    uint64_t getID() const { return read<uint64_t>(P + IDOffset); }

    /// Get the instruction offset (from the start of the containing function).
    uint32_t getInstructionOffset() const {
      return read<uint32_t>(P + InstructionOffsetOffset);
    }

    /// Get the call size.
    uint8_t getCallSize() const { return read<uint8_t>(P + CallSizeOffset); }

    /// Get the flags.
    uint8_t getFlags() const { return read<uint8_t>(P + FlagsOffset); }

    /// Get the location index.
    uint16_t getLocationIndex() const {
      return read<uint16_t>(P + LocationIndexOffset);
    }

    /// Get the number of location.
    uint16_t getNumLocations() const {
      return read<uint16_t>(P + NumLocationOffset);
    }

    /// Begin iterator for locations.
    location_iterator location_begin() const {
      return location_iterator(SMParser->getLocation(getLocationIndex()));
    }

    /// End iterator for locations.
    location_iterator location_end() const {
      return location_iterator(
          SMParser->getLocation(getLocationIndex() + getNumLocations()));
    }

    /// Iterator range for locations.
    iterator_range<location_iterator> locations() const {
      return make_range(location_begin(), location_end());
    }

    /// Get the live-out index.
    uint16_t getLiveOutIndex() const {
      return read<uint16_t>(P + LiveOutIndexOffset);
    }

    /// Get the number of live-out.
    uint16_t getNumLiveOuts() const {
      return read<uint16_t>(P + NumLiveOutOffset);
    }

    /// Begin iterator for live-outs.
    liveout_iterator liveouts_begin() const {
      return liveout_iterator(SMParser->getLiveOut(getLiveOutIndex()));
    }

    /// End iterator for live-outs.
    liveout_iterator liveouts_end() const {
      return liveout_iterator(
          SMParser->getLiveOut(getLiveOutIndex() + getNumLiveOuts()));
    }

    /// Iterator range for live-outs.
    iterator_range<liveout_iterator> liveouts() const {
      return make_range(liveouts_begin(), liveouts_end());
    }

  private:
    StackMapRecordAccessor(const uint8_t *P, StackMapV2Parser const *SMParser)
        : P(P), SMParser(SMParser) {}

    StackMapRecordAccessor next() const {
      return StackMapRecordAccessor(
          P + StackMapV2Parser::StackMapRecordAccessorSize, SMParser);
    }

    static const unsigned IDOffset = 0;
    static const unsigned InstructionOffsetOffset = IDOffset + sizeof(uint64_t);
    static const unsigned CallSizeOffset =
        InstructionOffsetOffset + sizeof(uint32_t);
    static const unsigned FlagsOffset = CallSizeOffset + sizeof(uint8_t);
    static const unsigned LocationIndexOffset = FlagsOffset + sizeof(uint8_t);
    static const unsigned NumLocationOffset =
        LocationIndexOffset + sizeof(uint16_t);
    static const unsigned LiveOutIndexOffset =
        NumLocationOffset + sizeof(uint16_t);
    static const unsigned NumLiveOutOffset =
        LiveOutIndexOffset + sizeof(uint16_t);

    const uint8_t *P;
    StackMapV2Parser const *SMParser;
  };

  /// Accessor for Frame Record
  class FrameRecordAccessor {
    friend class StackMapV2Parser;

  public:
    typedef AccessorIterator<StackMapRecordAccessor> stackmaprecord_iterator;

    /// Get function address
    uint64_t getFunctionAddress() const {
      return read<uint64_t>(P + FunctionAddressOffset);
    }

    /// Get the function size.
    uint32_t getFunctionSize() const {
      return read<uint32_t>(P + FunctionSizeOffset);
    }

    /// Get the stack size.
    uint32_t getStackSize() const {
      return read<uint32_t>(P + StackSizeOffset);
    }

    /// Get the flags.
    uint16_t getFlags() const { return read<uint16_t>(P + FlagsOffset); }

    /// Get the frame base register.
    uint16_t getFrameBaseRegister() const {
      return read<uint16_t>(P + FrameBaseRegisterOffset);
    }

    /// Get the stack map record index
    uint16_t getStackMapRecordIndex() const {
      return read<uint16_t>(P + StackMapRecordIndexOffset);
    }

    // Get the number of stack map record.
    uint16_t getNumStackMapRecords() const {
      return read<uint16_t>(P + NumStackMapRecordsOffset);
    }

    /// Begin iterator for stack map records
    stackmaprecord_iterator stackmaprecords_begin() const {
      return stackmaprecord_iterator(
          SMParser->getStackMapRecord(getStackMapRecordIndex()));
    }

    /// End iterator for stack map records
    stackmaprecord_iterator stackmaprecords_end() const {
      return stackmaprecord_iterator(SMParser->getStackMapRecord(
          getStackMapRecordIndex() + getNumStackMapRecords()));
    }

    /// Iterator range for stack map records
    iterator_range<stackmaprecord_iterator> stackmaprecords() const {
      return make_range(stackmaprecords_begin(), stackmaprecords_end());
    }

  private:
    FrameRecordAccessor(const uint8_t *P, StackMapV2Parser const *SMParser)
        : P(P), SMParser(SMParser) {}

    FrameRecordAccessor next() const {
      return FrameRecordAccessor(P + StackMapV2Parser::FrameRecordAccessorSize,
                                 SMParser);
    }

    static const unsigned FunctionAddressOffset = 0;
    static const unsigned FunctionSizeOffset =
        FunctionAddressOffset + sizeof(uint64_t);
    static const unsigned StackSizeOffset =
        FunctionSizeOffset + sizeof(uint32_t);
    static const unsigned FlagsOffset = StackSizeOffset + sizeof(uint32_t);
    static const unsigned FrameBaseRegisterOffset =
        FlagsOffset + sizeof(uint16_t);
    static const unsigned StackMapRecordIndexOffset =
        FrameBaseRegisterOffset + sizeof(uint16_t);
    static const unsigned NumStackMapRecordsOffset =
        StackMapRecordIndexOffset + sizeof(uint16_t);

    const uint8_t *P;
    StackMapV2Parser const *SMParser;
  };

  /// Construct a parser for a version-1 stackmap. StackMap data will be read
  /// from the given array.
  StackMapV2Parser(ArrayRef<uint8_t> StackMapSection)
      : StackMapSection(StackMapSection) {

    assert(StackMapSection[0] == 2 &&
           "StackMapParser can only parse version 2 stackmaps");
  }

  typedef AccessorIterator<FrameRecordAccessor> function_iterator;

  /// Get the version number of this stackmap. (Always returns 2).
  unsigned getVersion() const { return 2; }

  /// Get the number of functions in the stack map.
  uint32_t getNumFunctions() const {
    return read<uint32_t>(&StackMapSection[NumFunctionsOffset]);
  }

  /// Get offset to Constants
  uint32_t getConstantOffset() const {
    return read<uint32_t>(&StackMapSection[ConstantsOffset]);
  }

  /// Get a Constant offset
  std::size_t getConstantOffset(unsigned ConstantIndex) const {
    return getConstantOffset() + ConstantIndex * ConstantAccessorSize;
  }

  /// Return an FunctionAccessor for the given function index.
  ConstantAccessor getConstant(unsigned ConstantIndex) const {
    return ConstantAccessor(StackMapSection.data() +
                            getConstantOffset(ConstantIndex));
  }

  /// Get offset to Frame records (functions)
  uint32_t getFrameRecordOffset() const {
    return read<uint32_t>(&StackMapSection[FrameRecordOffset]);
  }

  /// Get a function offset
  std::size_t getFrameRecordOffset(unsigned FunctionIndex) const {
    return getFrameRecordOffset() + FunctionIndex * FrameRecordAccessorSize;
  }

  /// Return an FunctionAccessor for the given function index.
  FrameRecordAccessor getFrameRecord(unsigned FunctionIndex) const {
    return FrameRecordAccessor(
        StackMapSection.data() + getFrameRecordOffset(FunctionIndex), this);
  }

  /// Get offset to Stack Map Records
  uint32_t getStackMapRecordOffset() const {
    return read<uint32_t>(&StackMapSection[StackMapRecordOffset]);
  }

  /// Get a stack map record offset for the given index
  std::size_t getStackMapRecordOffset(unsigned StackMapRecordIndex) const {
    return getStackMapRecordOffset() +
           StackMapRecordIndex * StackMapRecordAccessorSize;
  }

  /// Return an StackMapRecordAccessor for the given index.
  StackMapRecordAccessor getStackMapRecord(unsigned StackMapRecordIndex) const {
    return StackMapRecordAccessor(
        StackMapSection.data() + getStackMapRecordOffset(StackMapRecordIndex),
        this);
  }

  /// Get offset to location
  uint32_t getLocationOffset() const {
    return read<uint32_t>(&StackMapSection[LocationOffset]);
  }

  /// Get a location offset for the given index
  std::size_t getLocationOffset(unsigned LocationIndex) const {
    return getLocationOffset() + LocationIndex * LocationAccessorSize;
  }

  /// Return an StackMapRecordAccessor for the given index.
  LocationAccessor getLocation(unsigned LocationIndex) const {
    return LocationAccessor(StackMapSection.data() +
                            getLocationOffset(LocationIndex));
  }

  /// Get offset to live out
  uint32_t getLiveOutOffset() const {
    return read<uint32_t>(&StackMapSection[LiveOutOffset]);
  }

  /// Get a location offset for the given index
  std::size_t getLiveOutOffset(unsigned LiveOutIndex) const {
    return getLiveOutOffset() + LiveOutIndex * LiveOutAccessorSize;
  }

  /// Return an StackMapRecordAccessor for the given index.
  LiveOutAccessor getLiveOut(unsigned LiveOutIndex) const {
    return LiveOutAccessor(StackMapSection.data() +
                           getLiveOutOffset(LiveOutIndex));
  }

  /// Begin iterator for function/frame record.
  function_iterator functions_begin() const {
    return function_iterator(getFrameRecord(0));
  }

  /// End iterator for function/frame record.
  function_iterator functions_end() const {
    return function_iterator(getFrameRecord(getNumFunctions()));
  }

  /// Iterator range for functions.
  iterator_range<function_iterator> functions() const {
    return make_range(functions_begin(), functions_end());
  }

private:
  template <typename T> static T read(const uint8_t *P) {
    return support::endian::read<T, Endianness, 1>(P);
  }

  static const unsigned HeaderOffset = 0;
  static const unsigned NumFunctionsOffset = HeaderOffset + sizeof(uint32_t);

  static const unsigned ConstantsOffset = HeaderOffset + sizeof(uint32_t) * 2;
  static const unsigned FrameRecordOffset = HeaderOffset + sizeof(uint32_t) * 3;
  static const unsigned StackMapRecordOffset =
      HeaderOffset + sizeof(uint32_t) * 4;
  static const unsigned LocationOffset = HeaderOffset + sizeof(uint32_t) * 5;
  static const unsigned LiveOutOffset = HeaderOffset + sizeof(uint32_t) * 6;

  static const unsigned ConstantAccessorSize = sizeof(uint64_t);
  static const unsigned FrameRecordAccessorSize = 3 * sizeof(uint64_t);
  static const unsigned StackMapRecordAccessorSize = 3 * sizeof(uint64_t);
  static const unsigned LocationAccessorSize = 2 * sizeof(uint32_t);
  static const unsigned LiveOutAccessorSize = 2 * sizeof(uint16_t);

  ArrayRef<uint8_t> StackMapSection;
};
}

#endif
