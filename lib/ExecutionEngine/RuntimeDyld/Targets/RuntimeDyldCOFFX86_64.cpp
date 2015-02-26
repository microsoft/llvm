//===-- RuntimeDyldCOFFX86_64.cpp - COFF/X86_64 specific code ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// COFF x86_x64 support for MC-JIT runtime dynamic linker.
//
//===----------------------------------------------------------------------===//

#include "RuntimeDyldCOFFX86_64.h"

#define DEBUG_TYPE "dyld"

namespace llvm {

void RuntimeDyldCOFFX86_64::registerEHFrames() {
  if (!MemMgr)
    return;
  for (auto const &EHFrameSID : UnregisteredEHFrameSections) {
    uint8_t *EHFrameAddr = Sections[EHFrameSID].Address;
    uint64_t EHFrameLoadAddr = Sections[EHFrameSID].LoadAddress;
    size_t EHFrameSize = Sections[EHFrameSID].Size;
    MemMgr->registerEHFrames(EHFrameAddr, EHFrameLoadAddr, EHFrameSize);
    RegisteredEHFrameSections.push_back(EHFrameSID);
  }
  UnregisteredEHFrameSections.clear();
}

void RuntimeDyldCOFFX86_64::deregisterEHFrames() {
  // Stub
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
void RuntimeDyldCOFFX86_64::resolveRelocation(const RelocationEntry &RE,
  uint64_t Value) {
  const SectionEntry &Section = Sections[RE.SectionID];
  uint8_t *Target = Section.Address + RE.Offset;

  switch (RE.RelType) {
  case COFF::IMAGE_REL_AMD64_ADDR32NB: {
    uint32_t *TargetAddress = (uint32_t *)Target;
    *TargetAddress = Value + RE.Addend;
    break;
  }

  case COFF::IMAGE_REL_AMD64_ADDR64: {
    uint64_t *TargetAddress = (uint64_t *)Target;
    *TargetAddress = Value + RE.Addend;
    break;
  }

  default:
    llvm_unreachable("Relocation type not implemented yet!");
    break;
  }
}

relocation_iterator RuntimeDyldCOFFX86_64::processRelocationRef(
  unsigned SectionID, relocation_iterator RelI, const ObjectFile &Obj,
  ObjSectionToIDMap &ObjSectionToID, StubMap &Stubs) {
  uint64_t RelType;
  Check(RelI->getType(RelType));
  uint64_t Offset;
  Check(RelI->getOffset(Offset));
  symbol_iterator Symbol = RelI->getSymbol();

  // See if the fixup target has a nonzero addend
  // to contribute to the overall fixup result.
  uint64_t Addend = 0;
  SectionEntry &Section = Sections[SectionID];
  uint8_t *Target = Section.Address + Offset;
  switch (RelType) {
  case COFF::IMAGE_REL_AMD64_ADDR32NB: {
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
        report_fatal_error("Symbol section not found, bad object file");
      bool IsCode = SecI->isText();
      TargetSectionID = findOrEmitSection(Obj, *SecI, IsCode, ObjSectionToID);
    }
    TargetOffset = getSymbolOffset(*Symbol);
  }

  // Verify for now that any fixup be resolvable within the object scope.
  if (TargetOffset == UnknownAddressOrSize)
    report_fatal_error("External symbol reference");

  switch (RelType) {
  case COFF::IMAGE_REL_AMD64_ADDR64:
  case COFF::IMAGE_REL_AMD64_ADDR32NB: {
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

void RuntimeDyldCOFFX86_64::finalizeLoad(const ObjectFile &Obj,
  ObjSectionToIDMap &SectionMap) {
  // Look for and record the EH frame section IDs.
  for (const auto &SectionPair : SectionMap) {
    const SectionRef &Section = SectionPair.first;
    StringRef Name;
    Check(Section.getName(Name));
    // Note unwind info is split across .pdata and .xdata, so this
    // may not be sufficiently general for all users.
    if (Name == ".xdata") {
      UnregisteredEHFrameSections.push_back(SectionPair.second);
    }
  }
}

}