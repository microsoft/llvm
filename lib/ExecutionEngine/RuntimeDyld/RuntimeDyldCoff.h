//===-- RuntimeDyldCOFF.h - Run-time dynamic linker for MC-JIT ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// COFF support for MC-JIT runtime dynamic linker.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_RUNTIME_DYLD_COFF_H
#define LLVM_RUNTIME_DYLD_COFF_H

#include "RuntimeDyldImpl.h"
#include "llvm/ADT/DenseMap.h"

#define DEBUG_TYPE "dyld"

using namespace llvm;

namespace llvm {

namespace {
// Helper for extensive error checking in debug builds.
std::error_code Check1(std::error_code Err) {
  if (Err) {
    report_fatal_error(Err.message());
  }
  return Err;
}
} // end anonymous namespace

class RuntimeDyldCOFF : public RuntimeDyldImpl {
  void resolveRelocation(const SectionEntry &Section, uint64_t Offset,
                         uint64_t Value, uint32_t Type, int64_t Addend,
                         uint64_t SymOffset = 0);

  void resolveX86_64Relocation(const SectionEntry &Section, uint64_t Offset,
                               uint64_t Value, uint32_t Type, int64_t Addend,
                               uint64_t SymOffset);

  unsigned getMaxStubSize() override {
    if (Arch == Triple::aarch64 || Arch == Triple::arm64 ||
        Arch == Triple::aarch64_be || Arch == Triple::arm64_be)
      return 20; // movz; movk; movk; movk; br
    if (Arch == Triple::arm || Arch == Triple::thumb)
      return 8; // 32-bit instruction and 32-bit address
    else if (Arch == Triple::mipsel || Arch == Triple::mips)
      return 16;
    else if (Arch == Triple::ppc64 || Arch == Triple::ppc64le)
      return 44;
    else if (Arch == Triple::x86_64)
      return 6; // 2-byte jmp instruction + 32-bit relative address
    else if (Arch == Triple::systemz)
      return 16;
    else
      return 0;
  }

  void updateGOTEntries(StringRef Name, uint64_t Addr) override;

  // When a module is loaded we save the SectionID of the unwind
  // sectionsin a table until we receive a request to register all 
  // unregisteredEH frame sections with the memory manager.
  SmallVector<SID, 2> UnregisteredEHFrameSections;
  SmallVector<SID, 2> RegisteredEHFrameSections;

public:
  RuntimeDyldCOFF(RTDyldMemoryManager *mm) : RuntimeDyldImpl(mm) {}

  void resolveRelocation(const RelocationEntry &RE, uint64_t Value) override;
  relocation_iterator
  processRelocationRef(unsigned SectionID, relocation_iterator RelI,
                       ObjectImage &Obj, ObjSectionToIDMap &ObjSectionToID,
                       const SymbolTableMap &Symbols, StubMap &Stubs) override;
  bool isCompatibleFormat(const ObjectBuffer *Buffer) const override;
  bool isCompatibleFile(const object::ObjectFile *Buffer) const override;
  unsigned int getStubAlignment() override { return 1; }
  void registerEHFrames() override;
  void deregisterEHFrames() override;
  void finalizeLoad(ObjectImage &ObjImg,
                    ObjSectionToIDMap &SectionMap) override;
  virtual ~RuntimeDyldCOFF();

  static ObjectImage *createObjectImage(ObjectBuffer *InputBuffer);
  static ObjectImage *createObjectImageFromFile(std::unique_ptr<object::ObjectFile> Obj);
  static std::unique_ptr<RuntimeDyldCOFF> create(Triple::ArchType Arch,
                                                  RTDyldMemoryManager *mm);
};

} // end namespace llvm

#undef DEBUG_TYPE

#endif
