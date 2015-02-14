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

  // Obtain the relocation value....
  RelocationValueRef Value;

  // Determine location of target symbol.
  // First search for the symbol in the local symbol table
  RTDyldSymbolTable::const_iterator lsi = GlobalSymbolTable.end();
  SymbolRef::Type SymType = SymbolRef::ST_Unknown;
  if (Symbol != Obj.symbol_end()) {
    lsi = GlobalSymbolTable.find(TargetName.data());
    Symbol->getType(SymType);
  }

  // Assert for now that any fixup be resolvable
  // within the object scope.
  if (lsi != GlobalSymbolTable.end()) {
    const auto &SymInfo = lsi->second;
    Value.SectionID = SymInfo.getSectionID();
    Value.Offset = SymInfo.getOffset();
    Value.Addend = SymInfo.getOffset() + Addend;
  } else {
    llvm_unreachable("External symbol reference?");
  }

  switch (RelType) {
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR64:
  case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR32NB:
  {
    RelocationEntry RE(SectionID, Offset, RelType, Value.Addend);
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
