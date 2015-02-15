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
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::object;

#define DEBUG_TYPE "dyld"

namespace {

class LoadedCOFFObjectInfo : public RuntimeDyld::LoadedObjectInfo {
public:
  LoadedCOFFObjectInfo(RuntimeDyldImpl &RTDyld, unsigned BeginIdx,
                        unsigned EndIdx)
    : RuntimeDyld::LoadedObjectInfo(RTDyld, BeginIdx, EndIdx) {}

  OwningBinary<ObjectFile>
  getObjectForDebug(const ObjectFile &Obj) const override {
    return OwningBinary<ObjectFile>();
  }
};

}

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

std::unique_ptr<RuntimeDyldCOFF>
llvm::RuntimeDyldCOFF::create(Triple::ArchType Arch, RTDyldMemoryManager *MM) {
  switch (Arch) {
  default:
    llvm_unreachable("Unsupported target for RuntimeDyldCOFF.");
    break;
  case Triple::x86_64: return make_unique<RuntimeDyldCOFF>(MM);
  }
}

std::unique_ptr<RuntimeDyld::LoadedObjectInfo>
RuntimeDyldCOFF::loadObject(const object::ObjectFile &O) {
  unsigned SectionStartIdx, SectionEndIdx;
  std::tie(SectionStartIdx, SectionEndIdx) = loadObjectImpl(O);
  return llvm::make_unique<LoadedCOFFObjectInfo>(*this, SectionStartIdx,
                                                SectionEndIdx);
}

RuntimeDyldCOFF::RuntimeDyldCOFF(RTDyldMemoryManager *mm) : RuntimeDyldImpl(mm) {}
RuntimeDyldCOFF::~RuntimeDyldCOFF() {}

void RuntimeDyldCOFF::resolveX86_64Relocation(const SectionEntry &Section,
                                             uint64_t Offset, uint64_t Value,
                                             uint32_t Type, int64_t Addend,
                                             uint64_t SymOffset) {
  uint8_t *Target = Section.Address + Offset;

  switch (Type) {
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR32NB: {
    uint32_t *TargetAddress = (uint32_t *)Target;
    *TargetAddress = Value + Addend;
    break;
  }

  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR64: {
    uint64_t *TargetAddress = (uint64_t *)Target;
    *TargetAddress = Value + Addend;
    break;
  }

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

static uint64_t getSymbolOffset(const SymbolRef &Sym) {
  uint64_t Address;
  if (std::error_code EC = Sym.getAddress(Address))
    return UnknownAddressOrSize;

  if (Address == UnknownAddressOrSize)
    return UnknownAddressOrSize;

  const ObjectFile *Obj = Sym.getObject();
  section_iterator SecI(Obj->section_end());
  if (std::error_code EC = Sym.getSection(SecI))
    return UnknownAddressOrSize;

  if (SecI == Obj->section_end())
    return UnknownAddressOrSize;

  uint64_t SectionAddress = SecI->getAddress();
  return Address - SectionAddress;
}

relocation_iterator RuntimeDyldCOFF::processRelocationRef(
    unsigned SectionID, relocation_iterator RelI, const ObjectFile &Obj,
    ObjSectionToIDMap &ObjSectionToID, StubMap &Stubs) {
  uint64_t RelType;
  Check1(RelI->getType(RelType));
  uint64_t Offset;
  Check1(RelI->getOffset(Offset));
  symbol_iterator Symbol = RelI->getSymbol();

  // See if the fixup target has a nonzero addend
  // to contribute to the overall fixup result.
  uint64_t Addend = 0;
  SectionEntry &Section = Sections[SectionID];
  uint8_t *Target = Section.Address + Offset;
  switch (RelType) {
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR32NB: {
    uint32_t *TargetAddress = (uint32_t *)Target;
    Addend = *TargetAddress;
    break; 
  }

  case COFF::IMAGE_REL_AMD64_ADDR64: {
    uint64_t *TargetAddress = (uint64_t *)Target;
    Addend = *TargetAddress;
    break;
  }

  default:
    break;
  }

  // Obtain the symbol name which is referenced in the relocation
  StringRef TargetName;
  if (Symbol != Obj.symbol_end())
    Symbol->getName(TargetName);
  DEBUG(dbgs() << "\t\tIn Section " << SectionID << " Offset " << Offset 
        << " RelType: " << RelType << " TargetName: " << TargetName 
        << " Addend " << Addend << "\n");

  // Obtain the target offset and symbol kind
  bool IsSectionDefinition = false;
  unsigned TargetSectionID = 0;
  uint64_t TargetOffset = UnknownAddressOrSize;
  if (Symbol != Obj.symbol_end()) {
    const COFFObjectFile *COFFObj = cast<COFFObjectFile>(&Obj);
    const COFFSymbolRef COFFSymbol = COFFObj->getCOFFSymbol(*Symbol);
    IsSectionDefinition = COFFSymbol.isSectionDefinition();
    if (IsSectionDefinition) {
      section_iterator SecI(Obj.section_end());
      Symbol->getSection(SecI);
      if (SecI == Obj.section_end())
        llvm_unreachable("Symbol section not found, bad object file");
      bool IsCode = SecI->isText();
      TargetSectionID = findOrEmitSection(Obj, *SecI, IsCode, ObjSectionToID);
    }
    TargetOffset = getSymbolOffset(*Symbol);
  }

  // Assert for now that any fixup be resolvable within the object scope.
  if (TargetOffset == UnknownAddressOrSize) {
    llvm_unreachable("External symbol reference?");
  }

  switch (RelType) {
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR64:
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR32NB:
  {
    RelocationEntry RE(SectionID, Offset, RelType, TargetOffset + Addend);

    if (IsSectionDefinition)
      addRelocationForSection(RE, TargetSectionID);
    else
      addRelocationForSymbol(RE, TargetName);
    break;
  }
  default:
    llvm_unreachable("Unhandled relocation type");
  }

  return ++RelI;
}

void RuntimeDyldCOFF::updateGOTEntries(StringRef Name, uint64_t Addr) {
  // Stub
}

void RuntimeDyldCOFF::finalizeLoad(const ObjectFile &Obj,
                                  ObjSectionToIDMap &SectionMap) {
  // Look for and record the EH frame sections.
  ObjSectionToIDMap::iterator i, e;
  for (i = SectionMap.begin(), e = SectionMap.end(); i != e; ++i) {
    const SectionRef &Section = i->first;
    StringRef Name;
    Section.getName(Name);
    if (Name == ".xdata") {
      UnregisteredEHFrameSections.push_back(i->second);
    }
  }
}

bool RuntimeDyldCOFF::isCompatibleFile(const object::ObjectFile &Obj) const {
  return Obj.isCOFF();
}

} // namespace llvm
