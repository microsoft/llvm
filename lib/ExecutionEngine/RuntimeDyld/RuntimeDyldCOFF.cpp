//===-- RuntimeDyldCOFF.cpp - Run-time dynamic linker for MC-JIT -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of COFF support for the MC-JIT runtime dynamic linker.
//
//===----------------------------------------------------------------------===//

#include "RuntimeDyldCOFF.h"
#include "JITRegistrar.h"
#include "ObjectImageCommon.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ExecutionEngine/ObjectBuffer.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::object;

namespace llvm {

void RuntimeDyldCOFF::registerEHFrames() {
  if (!MemMgr)
    return;
  for (int i = 0, e = UnregisteredEHFrameSections.size(); i != e; ++i) {
    SID EHFrameSID = UnregisteredEHFrameSections[i];
    uint8_t *EHFrameAddr = Sections[EHFrameSID].Address;
    uint64_t EHFrameLoadAddr = Sections[EHFrameSID].LoadAddress;
    size_t EHFrameSize = Sections[EHFrameSID].Size;
    MemMgr->registerEHFrames(EHFrameAddr, EHFrameLoadAddr, EHFrameSize);
    RegisteredEHFrameSections.push_back(EHFrameSID);
  }
  UnregisteredEHFrameSections.clear();
}

void RuntimeDyldCOFF::deregisterEHFrames() {
  // Stub
}

ObjectImage *
RuntimeDyldCOFF::createObjectImageFromFile(std::unique_ptr<object::ObjectFile> ObjFile) {
  return new ObjectImageCommon(std::move(ObjFile));
}

ObjectImage *RuntimeDyldCOFF::createObjectImage(ObjectBuffer *Buffer) {
  return new ObjectImageCommon(Buffer);
}

std::unique_ptr<RuntimeDyldCOFF>
llvm::RuntimeDyldCOFF::create(Triple::ArchType Arch, RTDyldMemoryManager *MM) {
  switch (Arch) {
  default:
    llvm_unreachable("Unsupported target for RuntimeDyldMachO.");
    break;
  case Triple::x86_64: return make_unique<RuntimeDyldCOFF>(MM);
  }
}

RuntimeDyldCOFF::~RuntimeDyldCOFF() {}

void RuntimeDyldCOFF::resolveX86_64Relocation(const SectionEntry &Section,
                                             uint64_t Offset, uint64_t Value,
                                             uint32_t Type, int64_t Addend,
                                             uint64_t SymOffset) {
  // Stub
  switch (Type) {
  default:
    llvm_unreachable("Relocation type not implemented yet!");
    break;
  }
}

// The target location for the relocation is described by RE.SectionID and
// RE.Offset.  RE.SectionID can be used to find the SectionEntry.  Each
// SectionEntry has three members describing its location.
// SectionEntry::Address is the address at which the section has been loaded
// into memory in the current (host) process.  SectionEntry::LoadAddress is the
// address that the section will have in the target process.
// SectionEntry::ObjAddress is the address of the bits for this section in the
// original emitted object image (also in the current address space).
//
// Relocations will be applied as if the section were loaded at
// SectionEntry::LoadAddress, but they will be applied at an address based
// on SectionEntry::Address.  SectionEntry::ObjAddress will be used to refer to
// Target memory contents if they are required for value calculations.
//
// The Value parameter here is the load address of the symbol for the
// relocation to be applied.  For relocations which refer to symbols in the
// current object Value will be the LoadAddress of the section in which
// the symbol resides (RE.Addend provides additional information about the
// symbol location).  For external symbols, Value will be the address of the
// symbol in the target address space.
void RuntimeDyldCOFF::resolveRelocation(const RelocationEntry &RE,
                                        uint64_t Value) {
  const SectionEntry &Section = Sections[RE.SectionID];
  return resolveRelocation(Section, RE.Offset, Value, RE.RelType, RE.Addend,
                           RE.SymOffset);
}

void RuntimeDyldCOFF::resolveRelocation(const SectionEntry &Section,
                                       uint64_t Offset, uint64_t Value,
                                       uint32_t Type, int64_t Addend,
                                       uint64_t SymOffset) {
  switch (Arch) {
  case Triple::x86_64:
    resolveX86_64Relocation(Section, Offset, Value, Type, Addend, SymOffset);
    break;
  default:
    llvm_unreachable("Unsupported CPU type!");
  }
}

relocation_iterator RuntimeDyldCOFF::processRelocationRef(
    unsigned SectionID, relocation_iterator RelI, ObjectImage &Obj,
    ObjSectionToIDMap &ObjSectionToID, const SymbolTableMap &Symbols,
    StubMap &Stubs) {
  // Stub
  return ++RelI;
}

void RuntimeDyldCOFF::updateGOTEntries(StringRef Name, uint64_t Addr) {
  // Stub
}

void RuntimeDyldCOFF::finalizeLoad(ObjectImage &ObjImg,
                                  ObjSectionToIDMap &SectionMap) {
  // Look for and record the EH frame sections.
  ObjSectionToIDMap::iterator i, e;
  for (i = SectionMap.begin(), e = SectionMap.end(); i != e; ++i) {
    const SectionRef &Section = i->first;
    StringRef Name;
    Section.getName(Name);
    if (Name == ".pdata") {
      UnregisteredEHFrameSections.push_back(i->second);
    }
  }
}

// COFF Object files do not have any magic signature we can key on.
// We do some basic sanity checks here.
bool RuntimeDyldCOFF::isCompatibleFormat(const ObjectBuffer *Buffer) const {
  // Ensure there's space for the required header.
  size_t BufferSize = Buffer->getBufferSize();
  if (BufferSize < COFF::HeaderSize) {
    return false;
  }
  
  assert(COFF::HeaderSize == sizeof(COFF::header));

  COFF::header * Header = (COFF::header *)(Buffer->getBufferStart());
  
  // (For now) insist we have X64 code...
  if (Header->Machine != COFF::MachineTypes::IMAGE_FILE_MACHINE_AMD64) {
    return false;
  }

  // Object should have at least one section or it's not interesting.
  if (Header->NumberOfSections == 0) {
    return false;
  }

  // Object should not have an optional header.
  if (Header->SizeOfOptionalHeader != 0) {
    return false;
  }

  // There should be space for the symbol table.
  if (Header->PointerToSymbolTable > 0) {
    unsigned int SymbolTableSize = Header->NumberOfSymbols * COFF::SymbolSize;
    if (Header->PointerToSymbolTable + SymbolTableSize > BufferSize) {
      return false;
    }
  }

  // Object should not have any characterisic flags set.
  if (Header->SizeOfOptionalHeader != 0) {
    return false;
  }  
  
  // Seems plausible this is a coff object file.
  return true;
}

bool RuntimeDyldCOFF::isCompatibleFile(const object::ObjectFile *Obj) const {
  return Obj->isCOFF();
}

} // namespace llvm
