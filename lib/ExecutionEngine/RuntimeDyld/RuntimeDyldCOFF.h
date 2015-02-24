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

class RuntimeDyldCOFF : public RuntimeDyldImpl {
  void resolveRelocation(const SectionEntry &Section, uint64_t Offset,
                         uint64_t Value, uint32_t Type, int64_t Addend,
                         uint64_t SymOffset = 0);

  void resolveX86_64Relocation(const SectionEntry &Section, uint64_t Offset,
                               uint64_t Value, uint32_t Type, int64_t Addend,
                               uint64_t SymOffset);

  unsigned getMaxStubSize() override {
    if (Arch == Triple::aarch64 || Arch == Triple::aarch64_be)
      return 20; // movz; movk; movk; movk; br
    if (Arch == Triple::arm || Arch == Triple::thumb)
      return 8; // 32-bit instruction and 32-bit address
    else if (Arch == Triple::x86_64)
      return 6; // 2-byte jmp instruction + 32-bit relative address
    else
      return 0;
  }

  void updateGOTEntries(StringRef Name, uint64_t Addr) override;

  // When a module is loaded we save the SectionID of the unwind
  // sections in a table until we receive a request to register all 
  // unregisteredEH frame sections with the memory manager.
  SmallVector<SID, 2> UnregisteredEHFrameSections;
  SmallVector<SID, 2> RegisteredEHFrameSections;

public:
  RuntimeDyldCOFF(RTDyldMemoryManager *mm);
  virtual ~RuntimeDyldCOFF();

  std::unique_ptr<RuntimeDyld::LoadedObjectInfo>
  loadObject(const object::ObjectFile &Obj) override;

  void resolveRelocation(const RelocationEntry &RE, uint64_t Value) override;
  relocation_iterator
  processRelocationRef(unsigned SectionID, relocation_iterator RelI,
                       const ObjectFile &Obj, ObjSectionToIDMap &ObjSectionToID,
                       StubMap &Stubs) override;

  bool isCompatibleFile(const object::ObjectFile &Obj) const override;
  unsigned int getStubAlignment() override { return 1; }
  void registerEHFrames() override;
  void deregisterEHFrames() override;
  void finalizeLoad(const ObjectFile &Obj,
                    ObjSectionToIDMap &SectionMap) override;

  static std::unique_ptr<RuntimeDyldCOFF> create(Triple::ArchType Arch,
                                                  RTDyldMemoryManager *mm);
};

} // end namespace llvm

#undef DEBUG_TYPE

#endif
