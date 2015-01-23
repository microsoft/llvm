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
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::object;

#define DEBUG_TYPE "dyld"

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
    llvm_unreachable("Unsupported target for RuntimeDyldCOFF.");
    break;
  case Triple::x86_64: return make_unique<RuntimeDyldCOFF>(MM);
  }
}

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
    unsigned SectionID, relocation_iterator RelI, ObjectImage &ObjImage,
    ObjSectionToIDMap &ObjSectionToID, const SymbolTableMap &Symbols,
    StubMap &Stubs) {
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
  if (Symbol != ObjImage.end_symbols())
    Symbol->getName(TargetName);
  DEBUG(dbgs() << "\t\tIn Section " << SectionID << " Offset " << Offset 
        << " RelType: " << RelType << " TargetName: " << TargetName 
        << " Addend " << Addend << "\n");

  // Obtain the relocation value....
  RelocationValueRef Value;

  // Determine location of target symbol.
  // First search for the symbol in the local symbol table
  SymbolTableMap::const_iterator lsi = Symbols.end();
  SymbolRef::Type SymType = SymbolRef::ST_Unknown;
  if (Symbol != ObjImage.end_symbols()) {
    lsi = Symbols.find(TargetName.data());
    Symbol->getType(SymType);
  }

  // Assert for now that any fixup be resolvable
  // within the object scope.
  if (lsi != Symbols.end()) {
    Value.SectionID = lsi->second.first;
    Value.Offset = lsi->second.second;
    Value.Addend = Addend;
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

void RuntimeDyldCOFF::finalizeLoad(ObjectImage &ObjImg,
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

// COFF Object files do not have any magic signature we can key on.
// We do some basic sanity checks here.
bool RuntimeDyldCOFF::isCompatibleFormat(const ObjectBuffer *Buffer) const {
  // Ensure there's space for the required header.
  size_t BufferSize = Buffer->getBufferSize();

  FILE * f = fopen("coff.obj", "wb");
  fwrite(Buffer->getBufferStart(), sizeof(char), BufferSize, f);
  fclose(f);

  if (BufferSize < COFF::HeaderSize) {
    return false;
  }
  
  assert(COFF::HeaderSize == sizeof(COFF::header));

  // This may not be sufficiently endian kosher...
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

  // There should be space for the symbol table plus the length of the string
  // table.
  if (Header->PointerToSymbolTable > 0) {
    unsigned int SymbolTableSize = Header->NumberOfSymbols * COFF::SymbolSize;
    unsigned int SymbolTableEnd = Header->PointerToSymbolTable + SymbolTableSize;
    
    if (SymbolTableEnd + 4 > BufferSize) {
      return false;
    }

    // Fetch the length of the string table (including the length field).
    unsigned int StringTableSize = 
      *(unsigned int *)(Buffer->getBufferStart() + SymbolTableEnd);

    // Ensure there's room for the string table.
    if (SymbolTableEnd + StringTableSize > BufferSize) {
      return false;
    }
  }

  // Object should not have any characterisic flags set.
  if (Header->Characteristics != 0) {
    return false;
  }  
  
  // Seems plausible this is a coff object file.
  return true;
}

bool RuntimeDyldCOFF::isCompatibleFile(const object::ObjectFile *Obj) const {
  return Obj->isCOFF();
}

} // namespace llvm
