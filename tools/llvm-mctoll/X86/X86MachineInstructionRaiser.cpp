//==-- X86MachineInstructionRaiser.cpp - Binary raiser utility llvm-mctoll -==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of X86MachineInstructionRaiser class
// for use by llvm-mctoll.
//
//===----------------------------------------------------------------------===//
#include "X86MachineInstructionRaiser.h"
#include "ExternalFunctions.h"
#include "X86InstrBuilder.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <set>
#include <vector>

using namespace llvm;
using namespace mctoll;

// Constructor

X86MachineInstructionRaiser::X86MachineInstructionRaiser(
    MachineFunction &machFunc, Module &m, const ModuleRaiser *mr,
    MCInstRaiser *mcir)
    : MachineInstructionRaiser(machFunc, m, mr, mcir),
      machineRegInfo(MF.getRegInfo()),
      x86TargetInfo(MF.getSubtarget<X86Subtarget>()) {
  x86InstrInfo = x86TargetInfo.getInstrInfo();
  x86RegisterInfo = x86TargetInfo.getRegisterInfo();
  PrintPass =
      (cl::getRegisteredOptions()["print-after-all"]->getNumOccurrences() > 0);
  FPUStack.TOP = 0;
  for (int i = 0; i < FPUSTACK_SZ; i++) {
    FPUStack.Regs[i] = nullptr;
  }
}

/* Delete noop instructions */

bool X86MachineInstructionRaiser::deleteNOOPInstrMI(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) {
  MachineInstr &MI = *MBBI;
  if (isNoop(MI.getOpcode())) {
    MBB.remove(&MI);
    return true;
  }
  return false;
}

bool X86MachineInstructionRaiser::deleteNOOPInstrMF() {
  bool modified = false;
  for (MachineBasicBlock &MBB : MF) {
    // MBBI may be invalidated by the raising operation.
    MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
    while (MBBI != E) {
      MachineBasicBlock::iterator NMBBI = std::next(MBBI);
      modified |= deleteNOOPInstrMI(MBB, MBBI);
      MBBI = NMBBI;
    }
  }
  return modified;
}

/* Function prototype discovery */

// Unfortunately, tablegen does not have an interface to query
// information about argument registers used for calling
// convention used.
static const std::vector<MCPhysReg> GPR64ArgRegs64Bit({X86::RDI, X86::RSI,
                                                       X86::RDX, X86::RCX,
                                                       X86::R8, X86::R9});

static const std::vector<MCPhysReg> GPR64ArgRegs32Bit({X86::EDI, X86::ESI,
                                                       X86::EDX, X86::ECX,
                                                       X86::R8D, X86::R9D});

static const std::vector<MCPhysReg>
    GPR64ArgRegs16Bit({X86::DI, X86::SI, X86::DX, X86::CX, X86::R8W, X86::R9W});

static const std::vector<MCPhysReg> GPR64ArgRegs8Bit({X86::DIL, X86::SIL,
                                                      X86::DL, X86::CL,
                                                      X86::R8B, X86::R9B});

// static const ArrayRef<MCPhysReg> GPR64ArgRegsWin64({X86::RCX, X86::RDX,
// X86::R8,
//                                                    X86::R9});

static inline bool is64BitPhysReg(unsigned int PReg) {
  return X86MCRegisterClasses[X86::GR64RegClassID].contains(PReg);
}

static bool inline is32BitPhysReg(unsigned int PReg) {
  return X86MCRegisterClasses[X86::GR32RegClassID].contains(PReg);
}

static bool inline is16BitPhysReg(unsigned int PReg) {
  return X86MCRegisterClasses[X86::GR16RegClassID].contains(PReg);
}

static bool inline is8BitPhysReg(unsigned int PReg) {
  return X86MCRegisterClasses[X86::GR8RegClassID].contains(PReg);
}

static inline Type *getImmOperandType(const MachineInstr &mi,
                                      unsigned int OpIndex) {
  LLVMContext &llvmContext(mi.getMF()->getFunction().getContext());
  MachineOperand Op = mi.getOperand(OpIndex);
  assert(Op.isImm() && "Attempt to get size of non-immediate operand");
  // Initialize to nullptr - unknown
  Type *ImmType = nullptr;
  uint8_t ImmSize = X86II::getSizeOfImm(mi.getDesc().TSFlags);

  switch (ImmSize) {
  case 8:
    ImmType = Type::getInt64Ty(llvmContext);
    break;
  case 4:
    ImmType = Type::getInt32Ty(llvmContext);
    break;
  case 2:
    ImmType = Type::getInt16Ty(llvmContext);
    break;
  case 1:
    ImmType = Type::getInt8Ty(llvmContext);
    break;
  default:
    assert(false && "Immediate operand of unknown size");
    break;
  }
  return ImmType;
}

static inline uint8_t getPhysRegOperandSize(const MachineInstr &mi,
                                            unsigned int OpIndex) {
  MCOperandInfo OpInfo = mi.getDesc().OpInfo[OpIndex];
  MachineOperand Op = mi.getOperand(OpIndex);
  // Initialize to 0 - unknown
  uint8_t RegSize = 0;
  assert(Op.isReg() && "Attempt to get size of non-register operand");
  if (TargetRegisterInfo::isPhysicalRegister(Op.getReg())) {
    switch (OpInfo.RegClass) {
    case X86::GR64RegClassID:
      RegSize = 8;
      break;
    case X86::GR32RegClassID:
      RegSize = 4;
      break;
    case X86::GR16RegClassID:
      RegSize = 2;
      break;
    case X86::GR8RegClassID:
      RegSize = 1;
      break;
    default:
      assert(false && "Register operand of unknown register class");
      break;
    }
  } else {
    assert(false &&
           "Unexpected non-physical register found in store instruction");
  }
  return RegSize;
}

static inline Type *getPhysRegOperandType(const MachineInstr &mi,
                                          unsigned int OpIndex) {
  LLVMContext &llvmContext(mi.getMF()->getFunction().getContext());
  MachineOperand Op = mi.getOperand(OpIndex);
  MCOperandInfo OpInfo = mi.getDesc().OpInfo[OpIndex];
  // Initialize to nullptr - unknown
  Type *RegTy = nullptr;

  assert(Op.isReg() && "Attempt to get type of non-register operand");
  if (TargetRegisterInfo::isPhysicalRegister(Op.getReg())) {
    switch (OpInfo.RegClass) {
    case X86::GR64RegClassID:
      RegTy = Type::getInt64Ty(llvmContext);
      break;
    case X86::GR32RegClassID:
      RegTy = Type::getInt32Ty(llvmContext);
      break;
    case X86::GR16RegClassID:
      RegTy = Type::getInt16Ty(llvmContext);
      break;
    case X86::GR8RegClassID:
      RegTy = Type::getInt8Ty(llvmContext);
      break;
    default:
      assert(false && "Register operand of unknown register class");
      break;
    }
  } else {
    assert(false &&
           "Unexpected non-physical register found in store instruction");
  }

  return RegTy;
}

static inline bool isPushToStack(const MachineInstr &mi) {
  unsigned char BaseOpcode = X86II::getBaseOpcodeFor(mi.getDesc().TSFlags);
  // Note : Encoding of PUSH [CS | DS | ES | SS | FS | GS] not checked.
  return ((BaseOpcode == 0x50) || (BaseOpcode == 0x6A) ||
          (BaseOpcode == 0x68) || (BaseOpcode == 0xFF) ||
          (BaseOpcode == 0x60) || (BaseOpcode == 0x9c));
}

static inline bool isPopFromStack(const MachineInstr &mi) {
  unsigned char BaseOpcode = X86II::getBaseOpcodeFor(mi.getDesc().TSFlags);
  // Note : Encoding of POP [DS | ES | SS | FS | GS] not checked.
  return ((BaseOpcode == 0x58) || (BaseOpcode == 0x8F) ||
          (BaseOpcode == 0x9D) || (BaseOpcode == 0x61) ||
          // or LEAVE
          (BaseOpcode == 0xC9));
}

static inline bool isEffectiveAddrValue(Value *val) {
  if (isa<LoadInst>(val)) {
    return true;
  } else if (isa<BinaryOperator>(val)) {
    BinaryOperator *binOpVal = dyn_cast<BinaryOperator>(val);
    if (binOpVal->isBinaryOp(BinaryOperator::Add) ||
        binOpVal->isBinaryOp(BinaryOperator::Mul)) {
      return true;
    }
  }
  return false;
}

// FPU Access functions
void X86MachineInstructionRaiser::FPURegisterStackPush(Value *val) {
  assert(val->getType()->isFloatingPointTy() &&
         "Attempt to push non-FP type value on FPU register stack");
  assert((FPUStack.TOP < FPUSTACK_SZ) && (FPUStack.TOP >= 0) &&
         "Incorrect initial FPU Register Stack top in push");

  int8_t PushIndex = (FPUSTACK_SZ + FPUStack.TOP - 1) % FPUSTACK_SZ;

  assert((PushIndex < FPUSTACK_SZ) && (PushIndex >= 0) &&
         "Incorrect FPU Register Stack index computed in push");
  FPUStack.Regs[PushIndex] = val;
  FPUStack.TOP = PushIndex;
}

void X86MachineInstructionRaiser::FPURegisterStackPop() {
  assert((FPUStack.TOP < FPUSTACK_SZ) && (FPUStack.TOP >= 0) &&
         "Incorrect initial FPU Register Stack top in pop");

  int8_t PostPopIndex = (FPUSTACK_SZ + FPUStack.TOP + 1) % FPUSTACK_SZ;

  assert((PostPopIndex < FPUSTACK_SZ) && (PostPopIndex >= 0) &&
         "Incorrect FPU Register Stack index computed in pop");
  // Clear the value at current TOP
  FPUStack.Regs[FPUStack.TOP] = nullptr;
  // Adjust TOP value
  FPUStack.TOP = PostPopIndex;
}

// Get value at index
Value *X86MachineInstructionRaiser::FPURegisterStackGetValueAt(int8_t index) {
  assert((FPUStack.TOP < FPUSTACK_SZ) && (FPUStack.TOP >= 0) &&
         "Incorrect initial FPU Register Stack top in FPU register access");

  int8_t AccessIndex = (FPUSTACK_SZ + FPUStack.TOP + index) % FPUSTACK_SZ;

  assert((AccessIndex < FPUSTACK_SZ) && (AccessIndex >= 0) &&
         "Incorrect FPU Register Stack index computed in FPU register access");

  return FPUStack.Regs[AccessIndex];
}

// Set value at index to val
void X86MachineInstructionRaiser::FPURegisterStackSetValueAt(int8_t index,
                                                             Value *val) {
  assert(val->getType()->isFloatingPointTy() &&
         "Attempt to insert non-FP type value in FPU register stack");
  assert((FPUStack.TOP < FPUSTACK_SZ) && (FPUStack.TOP >= 0) &&
         "Incorrect initial FPU Register Stack top in FPU register access");

  int8_t AccessIndex = (FPUSTACK_SZ + FPUStack.TOP + index) % FPUSTACK_SZ;

  assert((AccessIndex < FPUSTACK_SZ) && (AccessIndex >= 0) &&
         "Incorrect FPU Register Stack index computed in FPU register access");

  FPUStack.Regs[AccessIndex] = val;
}

Value *X86MachineInstructionRaiser::FPURegisterStackTop() {
  return FPURegisterStackGetValueAt(0);
}

// Construct argument type vector from the physical register vector.
// Requirements : PhysRegs is a set of registers each with no super or
// sub-registers.
bool X86MachineInstructionRaiser::buildFuncArgTypeVector(
    const std::set<MCPhysReg> &PhysRegs, std::vector<Type *> &ArgTyVec) {
  // A map of argument number and type as discovered
  std::map<unsigned int, Type *> argNumTypeMap;
  llvm::LLVMContext &funcLLVMContext = MF.getFunction().getContext();

  for (MCPhysReg PReg : PhysRegs) {
    // If Reg is an argument register per C standard calling convention
    // construct function argument.
    int argNum = getArgumentNumber(PReg);

    if (argNum > 0) {
      // Make sure each argument position is discovered only once
      assert(argNumTypeMap.find(argNum) == argNumTypeMap.end());
      if (is8BitPhysReg(PReg)) {
        argNumTypeMap.insert(
            std::make_pair(argNum, Type::getInt8Ty(funcLLVMContext)));
      } else if (is16BitPhysReg(PReg)) {
        argNumTypeMap.insert(
            std::make_pair(argNum, Type::getInt16Ty(funcLLVMContext)));
      } else if (is32BitPhysReg(PReg)) {
        argNumTypeMap.insert(
            std::make_pair(argNum, Type::getInt32Ty(funcLLVMContext)));
      } else if (is64BitPhysReg(PReg)) {
        argNumTypeMap.insert(
            std::make_pair(argNum, Type::getInt64Ty(funcLLVMContext)));
      } else {
        outs() << x86RegisterInfo->getRegAsmName(PReg) << "\n";
        assert(false && "Unhandled register type encountered in binary");
      }
    }
  }

  // Build argument type vector that will be used to build FunctionType
  // while sanity checking arguments discovered
  for (unsigned int i = 1; i <= argNumTypeMap.size(); i++) {
    // If the function has arguments, we assume that the conventional
    // argument registers are used in order. If the arg register
    // corresponding to position i is not a live in, it implies that the
    // function has i-1 arguments.
    if (argNumTypeMap.find(i) == argNumTypeMap.end()) {
      break;
    }
    auto Ty = argNumTypeMap.find(i)->second;
    ArgTyVec.push_back(Ty);
  }
  return true;
}

// Return argument number associated with physical
// register PReg according to C calling convention.

int X86MachineInstructionRaiser::getArgumentNumber(unsigned PReg) {
  int pos = -1;
  if (is8BitPhysReg(PReg)) {
    int diff = std::distance(
        GPR64ArgRegs8Bit.begin(),
        std::find(GPR64ArgRegs8Bit.begin(), GPR64ArgRegs8Bit.end(), PReg));
    if ((diff >= 0) && (diff < (int)GPR64ArgRegs8Bit.size())) {
      pos = diff + 1;
    }
  } else if (is16BitPhysReg(PReg)) {
    int diff = std::distance(
        GPR64ArgRegs16Bit.begin(),
        std::find(GPR64ArgRegs16Bit.begin(), GPR64ArgRegs16Bit.end(), PReg));
    if ((diff >= 0) && (diff < (int)GPR64ArgRegs16Bit.size())) {
      pos = diff + 1;
    }
  } else if (is32BitPhysReg(PReg)) {
    int diff = std::distance(
        GPR64ArgRegs32Bit.begin(),
        std::find(GPR64ArgRegs32Bit.begin(), GPR64ArgRegs32Bit.end(), PReg));
    if ((diff >= 0) && (diff < (int)GPR64ArgRegs32Bit.size())) {
      pos = diff + 1;
    }
  } else if (is64BitPhysReg(PReg)) {
    int diff = std::distance(
        GPR64ArgRegs64Bit.begin(),
        std::find(GPR64ArgRegs64Bit.begin(), GPR64ArgRegs64Bit.end(), PReg));
    if ((diff >= 0) && (diff < (int)GPR64ArgRegs64Bit.size())) {
      pos = diff + 1;
    }
  }
  return pos;
}

// Return a Value representing stack-allocated object
Value *X86MachineInstructionRaiser::createPCRelativeAccesssValue(
    const MachineInstr &mi, BasicBlock *curBlock) {
  Value *memrefValue = nullptr;
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  // Should have found the index of the memory reference operand
  assert(memoryRefOpIndex != -1 &&
         "Unable to find memory reference operand of a load/store instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);

  // LLVM represents memory operands using 5 operands
  //    viz., <opcode> BaseReg, ScaleAmt, IndexReg, Disp, Segment, ...
  // The disassembly in AT&T syntax is shown as
  //      Segment:Disp(BaseReg, IndexReg, ScaleAmt).
  // or as
  //      Segment:[BaseReg + Disp + IndexReg * ScaleAmt]
  // in Intel syntax.
  // effective address is calculated to be Segment:[BaseReg + IndexReg *
  // ScaleAmt + Disp] Segment is typically X86::NoRegister.

  assert(mi.getOperand(memoryRefOpIndex + X86::AddrSegmentReg).getReg() ==
             X86::NoRegister &&
         "Expect no segment register");
  // Construct non-stack memory referencing value
  unsigned BaseReg = memRef.Base.Reg;
  unsigned IndexReg = memRef.IndexReg;
  unsigned ScaleAmt = memRef.Scale;
  int Disp = memRef.Disp;
  const MachineOperand &SegRegOperand =
      mi.getOperand(memoryRefOpIndex + X86::AddrSegmentReg);
  // For now, we assume default segment DS (and hence no specification of
  // Segment register.
  assert(SegRegOperand.isReg() && (SegRegOperand.getReg() == X86::NoRegister) &&
         "Unhandled memory reference instruction with non-zero segment "
         "register");
  // Also assume that PC-relative addressing does not involve index register
  assert(IndexReg == X86::NoRegister &&
         "Unhandled index register in PC-relative memory addressing "
         "instruction");
  assert(ScaleAmt == 1 && "Unhandled value of scale amount in PC-relative "
                          "memory addressing instruction");

  // Non-stack memory address is supported by this function.
  uint64_t BaseSupReg = find64BitSuperReg(BaseReg);
  assert(((BaseSupReg == X86::RIP) || (BaseSupReg == X86::NoRegister)) &&
         "Base register that is not PC encountered in memory access "
         "instruction");

  // 1. Get the text section address
  int64_t TextSectionAddress = MR->getTextSectionAddress();

  assert(TextSectionAddress >= 0 && "Failed to find text section address");

  // 2. Get MCInst offset - the offset of machine instruction in the binary
  // and instruction size
  MCInstRaiser *MCIRaiser = getMCInstRaiser();
  uint64_t MCInstOffset = MCIRaiser->getMCInstIndex(mi);
  uint64_t MCInstSz = MCIRaiser->getMCInstSize(MCInstOffset);

  // 3. Compute the PC-relative offset.

  const ELF64LEObjectFile *Elf64LEObjFile =
      dyn_cast<ELF64LEObjectFile>(MR->getObjectFile());
  assert(Elf64LEObjFile != nullptr &&
         "Only 64-bit ELF binaries supported at present.");

  auto EType = Elf64LEObjFile->getELFFile()->getHeader()->e_type;
  if ((EType == ELF::ET_DYN) || (EType == ELF::ET_EXEC)) {
    uint64_t PCOffset = TextSectionAddress + MCInstOffset + MCInstSz + Disp;
    const RelocationRef *DynReloc = MR->getDynRelocAtOffset(PCOffset);

    // assert(DynReloc &&
    //       "Failed to get dynamic relocation for pc-relative offset");
    // If there is a dynamic relocation for the PCOffset
    if (DynReloc) {
      if (DynReloc->getType() == ELF::R_X86_64_GLOB_DAT) {
        Expected<StringRef> Symname = DynReloc->getSymbol()->getName();
        assert(Symname &&
               "Failed to find symbol associated with dynamic relocation.");
        // Find if a global value associated with symbol name is already
        // created
        for (GlobalVariable &gv : MR->getModule().globals()) {
          if (gv.getName().compare(Symname.get()) == 0) {
            memrefValue = &gv;
          }
        }
        if (memrefValue == nullptr) {
          // Get all necessary information about the global symbol.
          llvm::LLVMContext &llvmContext(MF.getFunction().getContext());
          DataRefImpl symbImpl = DynReloc->getSymbol()->getRawDataRefImpl();
          // get symbol
          auto symb = Elf64LEObjFile->getSymbol(symbImpl);
          // get symbol size
          uint64_t symbSize = symb->st_size;
          GlobalValue::LinkageTypes linkage;
          switch (symb->getBinding()) {
          case ELF::STB_GLOBAL:
            linkage = GlobalValue::ExternalLinkage;
            break;
          default:
            assert(false && "Unhandled dynamic symbol");
          }

          // Check that symbol type is data object, representing a variable or
          // array etc.
          assert((symb->getType() == ELF::STT_OBJECT) &&
                 "Function symbol type expected. Not found");
          Type *GlobalValTy = nullptr;
          switch (symbSize) {
          case 8:
            GlobalValTy = Type::getInt64Ty(llvmContext);
            break;
          case 4:
            GlobalValTy = Type::getInt32Ty(llvmContext);
            break;
          case 2:
            GlobalValTy = Type::getInt16Ty(llvmContext);
            break;
          case 1:
            GlobalValTy = Type::getInt8Ty(llvmContext);
            break;
          default:
            assert(false && "Unexpected symbol size");
          }
          // get symbol value - this is the virtual address of symbol's value
          uint64_t symVirtualAddr = symb->st_value;

          // get the initial value of the global data symbol at symVirtualAddr
          // from the section that contains the virtual address symVirtualAddr.
          // In executable and shared object files, st_value holds a virtual
          // address.
          uint64_t symbVal = 0;
          for (section_iterator SecIter : Elf64LEObjFile->sections()) {
            uint64_t SecStart = SecIter->getAddress();
            uint64_t SecEnd = SecStart + SecIter->getSize();
            if ((SecStart <= symVirtualAddr) && (SecEnd >= symVirtualAddr)) {
              // Get the initial symbol value only if this is not a bss section.
              // Else, symVal is already initialized to 0.
              if (SecIter->isBSS()) {
                linkage = GlobalValue::CommonLinkage;
              } else {
                StringRef SecData;
                SecIter->getContents(SecData);
                unsigned index = symVirtualAddr - SecStart;
                const unsigned char *beg = SecData.bytes_begin() + index;
                char shift = 0;
                while (symbSize-- > 0) {
                  // We know this is little-endian
                  symbVal = ((*beg++) << shift) | symbVal;
                  shift += 8;
                }
              }
              break;
            }
          }
          Constant *GlobalInit = ConstantInt::get(GlobalValTy, symbVal);
          auto GlobalVal = new GlobalVariable(MR->getModule(), GlobalValTy,
                                              false /* isConstant */, linkage,
                                              GlobalInit, Symname->data());
          // Don't use symbSize as it was modified.
          GlobalVal->setAlignment(symb->st_size);
          GlobalVal->setDSOLocal(true);
          memrefValue = GlobalVal;
        }
      } else {
        assert(false && "Unexpected relocation type referenced in PC-relative "
                        "memory access instruction.");
      }
    } else {
      memrefValue = getGlobalVariableValueAt(mi, PCOffset);
    }
  } else if (EType == ELF::ET_REL) {
    const RelocationRef *TextReloc =
        MR->getTextRelocAtOffset(MCInstOffset, MCInstSz);

    assert(TextReloc &&
           "Failed to get dynamic relocation for pc-relative offset");

    if (TextReloc->getType() == ELF::R_X86_64_32S) {
      Expected<StringRef> Symname = TextReloc->getSymbol()->getName();
      assert(Symname &&
             "Failed to find symbol associated with text relocation.");
      // Find if a global value associated with symbol name is already
      // created
      for (GlobalVariable &gv : MR->getModule().globals()) {
        if (gv.getName().compare(Symname.get()) == 0) {
          memrefValue = &gv;
        }
      }
      if (memrefValue == nullptr) {
        // Get all necessary information about the text relocation symbol
        // which is most likely global.

        llvm::LLVMContext &llvmContext(MF.getFunction().getContext());
        DataRefImpl symbImpl = TextReloc->getSymbol()->getRawDataRefImpl();
        // get symbol
        auto symb = Elf64LEObjFile->getSymbol(symbImpl);
        // get symbol size
        uint64_t symSize = symb->st_size;
        GlobalValue::LinkageTypes linkage;
        switch (symb->getBinding()) {
        case ELF::STB_GLOBAL:
          linkage = GlobalValue::ExternalLinkage;
          break;
        default:
          assert(false && "Unhandled dynamic symbol");
        }

        // get symbol value - this is the offset from the beginning of the
        // section st_shndex identifies.
        uint64_t symVal = symb->st_value;

        uint64_t symValSecIndex = symb->st_shndx;
        uint8_t symAlignment = 0;
        uint64_t symInitVal = 0;
        if (((symValSecIndex >= ELF::SHN_LORESERVE) &&
             (symValSecIndex <= ELF::SHN_HIRESERVE)) ||
            (symValSecIndex == ELF::SHN_UNDEF)) {
          if (symValSecIndex == ELF::SHN_COMMON) {
            // st_value holds symbol alignment constraints
            symAlignment = symVal;
            linkage = GlobalValue::CommonLinkage;
          }
        } else {
          // get the initial value of the global data symbol at offset symVal
          // in section with index symValSecIndex

          for (section_iterator SecIter : Elf64LEObjFile->sections()) {
            if (SecIter->getIndex() == symValSecIndex) {
              StringRef SecData;
              SecIter->getContents(SecData);
              const unsigned char *beg = SecData.bytes_begin() + symVal;
              char shift = 0;
              while (symSize-- > 0) {
                // We know this is little-endian
                symInitVal = ((*beg++) << shift) | symInitVal;
                shift += 8;
              }
              break;
            }
          }
          // REVISIT : Set symbol alignment to be the same as symbol size
          // NOTE : Do not use symSize since it has been modified in the while
          // loop above.
          symAlignment = symb->st_size;
        }
        Type *GlobalValTy = nullptr;

        switch (symAlignment) {
        case 8:
          GlobalValTy = Type::getInt64Ty(llvmContext);
          break;
        case 4:
          GlobalValTy = Type::getInt32Ty(llvmContext);
          break;
        case 2:
          GlobalValTy = Type::getInt16Ty(llvmContext);
          break;
        case 1:
          GlobalValTy = Type::getInt8Ty(llvmContext);
          break;
        default:
          assert(false && "Unexpected symbol size");
        }

        Constant *GlobalInit = ConstantInt::get(GlobalValTy, symInitVal);
        auto GlobalVal = new GlobalVariable(MR->getModule(), GlobalValTy,
                                            false /* isConstant */, linkage,
                                            GlobalInit, Symname->data());
        // Don't use symSize as it was modified.
        GlobalVal->setAlignment(symAlignment);
        GlobalVal->setDSOLocal(true);
        memrefValue = GlobalVal;
      }
    } else {
      assert(false && "Unexpected relocation type referenced in PC-relative "
                      "memory access instruction.");
    }
  } else {
    assert(false && "Unhandled binary type. Only object files and shared "
                    "libraries supported");
  }
  return memrefValue;
}

unsigned int
X86MachineInstructionRaiser::find64BitSuperReg(unsigned int PhysReg) {
  unsigned int SuperReg;
  bool SuperRegFound = false;

  // No super register for 0 register
  if (PhysReg == X86::NoRegister) {
    return X86::NoRegister;
  }

  // Nothing to do if PhysReg is EFLAGS
  if (PhysReg == X86::EFLAGS) {
    return PhysReg;
  }

  if (is64BitPhysReg(PhysReg)) {
    SuperReg = PhysReg;
    SuperRegFound = true;
  } else {
    for (MCSuperRegIterator SuperRegs(PhysReg, x86RegisterInfo);
         SuperRegs.isValid(); ++SuperRegs) {
      SuperReg = *SuperRegs;
      if (is64BitPhysReg(SuperReg)) {
        assert(SuperRegFound != true &&
               "Expect only one 64-bit super register");
        SuperRegFound = true;
      }
    }
  }
  assert(SuperRegFound && "Super register not found");
  return SuperReg;
}

Value *X86MachineInstructionRaiser::findPhysRegSSAValue(unsigned int PhysReg) {
  // Always convert PhysReg to the 64-bit version.
  unsigned int SuperReg = find64BitSuperReg(PhysReg);

  // Get the Value associated with SuperReg
  std::map<unsigned int, Value *>::iterator physToValueMapIter =
      physToValueMap.find(SuperReg);
  if (physToValueMapIter != physToValueMap.end()) {
    return physToValueMapIter->second;
  }
  return nullptr;
}

Value *X86MachineInstructionRaiser::getStackAllocatedValue(
    const MachineInstr &mi, BasicBlock *, int ldOrStOpIndex) {
  Value *memoryRefValue = nullptr;
  unsigned int stackFrameIndex;
  // Need to separate the stackFrameIndex value and that it is set since
  // stackFrameIndex can be any value (can't use a special value like -1).
  bool stackFrameIndexSet = false;

  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  // Should have found the index of the memory reference operand
  assert(memoryRefOpIndex != -1 &&
         "Unable to find memory reference operand of a load/store instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);
  // If this is a new stack reference
  MemRefToFrameIndexMapType::iterator mapIter =
      memRefToFrameIndexMap.find(memRef);
  if (mapIter == memRefToFrameIndexMap.end()) {
    // Create an alloca instruction since this memory reference is
    // encountered for the first time.
    // TODO : Memory references outside of stack are checked for at the
    // beginning of this function and not handled. At this point we know
    // that this is a stack reference.
    Type *Ty = nullptr;
    // Uninitialized stack object size
    uint64_t stackObjectSize = 0;
    unsigned int typeAlignment;
    LLVMContext &llvmContext(MF.getFunction().getContext());
    const DataLayout &dataLayout = MR->getModule().getDataLayout();
    unsigned allocaAddrSpace = dataLayout.getAllocaAddrSpace();
    const MCInstrDesc &MIDesc = mi.getDesc();

    assert((ldOrStOpIndex >= 0) && "Uninitialized load/store operand index");

    stackObjectSize = getInstructionMemOpSize(mi.getOpcode());
    switch (stackObjectSize) {
    case 8:
      Ty = Type::getInt64Ty(llvmContext);
      break;
    case 4:
      Ty = Type::getInt32Ty(llvmContext);
      break;
    case 2:
      Ty = Type::getInt16Ty(llvmContext);
      break;
    case 1:
      Ty = Type::getInt8Ty(llvmContext);
      break;
    default:
      assert(false && "Encountered unexpected memory operand size ");
      break;
    }

    assert(stackObjectSize != 0 && Ty != nullptr &&
           "Unknown type of operand in memory referencing instruction");
    typeAlignment = dataLayout.getPrefTypeAlignment(Ty);

    // Create alloca instruction to allocate stack slot
    AllocaInst *alloca = new AllocaInst(Ty, allocaAddrSpace, 0, typeAlignment);

    // Create a stack slot associated with the alloca instruction
    stackFrameIndex = MF.getFrameInfo().CreateStackObject(
        stackObjectSize, dataLayout.getPrefTypeAlignment(Ty),
        false /* isSpillSlot */, alloca);

    // Note that stack frame index corresponding to memory reference
    // has now been created and exists.
    stackFrameIndexSet = true;
    MF.getFrameInfo().setObjectOffset(stackFrameIndex, memRef.Disp);

    // Book-keeping: Add memory reference to the map
    memRefToFrameIndexMap.emplace(memRef, stackFrameIndex);

    // If the instruction is not an FPU load/store, check if operand stored to
    // is an argument or return register, set its name to reference arg<n>.addr
    // (the name decoration is primarily for aesthetics).
    auto InstKind = getInstructionKind(mi.getOpcode());
    if ((InstKind != InstructionKind::LOAD_FPU_REG) &&
        (InstKind != InstructionKind::STORE_FPU_REG)) {
      const MachineOperand &loadOrStoreOp = mi.getOperand(ldOrStOpIndex);
      if (loadOrStoreOp.isReg()) {
        unsigned int storeReg = loadOrStoreOp.getReg();

        // Set name of alloca instruction only if this is the first use of
        // argument or return register.
        if (findPhysRegSSAValue(storeReg) == nullptr) {
          int argNum = -1;
          argNum = getArgumentNumber(storeReg);
          // If the register is an argument register
          if ((argNum > 0) && (argNum <= (int)raisedFunction->arg_size())) {
            Function::arg_iterator argIter =
                raisedFunction->arg_begin() + argNum - 1;
            alloca->setName(argIter->getName() + ".addr");
          }
          // storeReg not an argument. Is it return register?
          else if ((storeReg == X86::EAX) || (storeReg == X86::RAX)) {
            alloca->setName("retval");
            // If this is a store instruction, create the store instruction
            // to store to the stack slot.
            if (MIDesc.mayStore()) {
              assert(false && "Unhandled situation where rax/eax are stored on "
                              "stack before intialization");
            }
          }
        }
      }
    }
    // Add the alloca instruction to entry block
    insertAllocaInEntryBlock(alloca);
  }
  // Stack frame slot already allocated for this memRef
  else {
    stackFrameIndex = (*mapIter).second;
    stackFrameIndexSet = true;
  }
  assert(stackFrameIndexSet && "Undefined stack slot");
  memoryRefValue = const_cast<AllocaInst *>(
      MF.getFrameInfo().getObjectAllocation(stackFrameIndex));

  return memoryRefValue;
}
// Return the Function * referenced by the PLT entry at offset
Function *X86MachineInstructionRaiser::getTargetFunctionAtPLTOffset(
    const MachineInstr &mi, uint64_t pltEntOff) {
  Function *CalledFunc = nullptr;
  const ELF64LEObjectFile *Elf64LEObjFile =
      dyn_cast<ELF64LEObjectFile>(MR->getObjectFile());
  assert(Elf64LEObjFile != nullptr &&
         "Only 64-bit ELF binaries supported at present.");
  unsigned char ExecType = Elf64LEObjFile->getELFFile()->getHeader()->e_type;
  assert((ExecType == ELF::ET_DYN) || (ExecType == ELF::ET_EXEC));
  // Find the section that contains the offset. That must be the PLT section
  for (section_iterator SecIter : Elf64LEObjFile->sections()) {
    uint64_t SecStart = SecIter->getAddress();
    uint64_t SecEnd = SecStart + SecIter->getSize();
    if ((SecStart <= pltEntOff) && (SecEnd >= pltEntOff)) {
      StringRef SecName;
      if (SecIter->getName(SecName)) {
        assert(false && "Failed to get section name with PLT offset");
      }
      if (SecName.compare(".plt") != 0) {
        assert(false && "Unexpected section name of PLT offset");
      }
      StringRef SecData;
      SecIter->getContents(SecData);
      // StringRef BytesStr;
      //    error(Section.getContents(BytesStr));
      ArrayRef<uint8_t> Bytes(reinterpret_cast<const uint8_t *>(SecData.data()),
                              SecData.size());
      // Disassemble the first instruction at the offset
      MCInst Inst;
      uint64_t InstSz;
      bool Success = MR->getMCDisassembler()->getInstruction(
          Inst, InstSz, Bytes.slice(pltEntOff - SecStart), pltEntOff, nulls(),
          nulls());
      assert(Success && "Failed to disassemble instruction in PLT");
      unsigned int Opcode = Inst.getOpcode();
      MCInstrDesc MCID = MR->getMCInstrInfo()->get(Opcode);
      if ((Opcode != X86::JMP64m) || (MCID.getNumOperands() != 5)) {
        assert(false && "Unexpected non-jump instruction or number of operands "
                        "of jmp instruction in PLT entry");
      }
      MCOperand Oprnd = Inst.getOperand(0);
      int64_t PCOffset = 0;

      // First operand should be PC
      if (Oprnd.isReg()) {
        if (Oprnd.getReg() != X86::RIP) {
          assert(false && "PC-relative jmp instruction expected in PLT entry");
        }
      } else {
        assert(false && "PC operand expected in jmp instruction of PLT entry");
      }

      Oprnd = Inst.getOperand(1);
      // Second operand should be 1
      if (Oprnd.isImm()) {
        if (Oprnd.getImm() != 1) {
          assert(false && "Unexpected immediate second operand in jmp "
                          "instruction of PLT entry");
        }
      } else {
        assert(false && "Unexpected non-immediate second operand in jmp "
                        "instruction of PLT entry");
      }

      Oprnd = Inst.getOperand(2);
      // Third operand should be X86::No_Register
      if (Oprnd.isReg()) {
        if (Oprnd.getReg() != X86::NoRegister) {
          assert(false && "Unexpected third operand - non-zero register in jmp "
                          "instruction of PLT entry");
        }
      } else {
        assert(false && "Unexpected third operand - non-register in jmp "
                        "instruction of PLT entry");
      }

      Oprnd = Inst.getOperand(3);
      // Fourth operand should be an immediate
      if (!Oprnd.isImm()) {
        assert(false && "Unexpected non-immediate fourth operand in jmp "
                        "instruction of PLT entry");
      }
      // Get the pc offset
      PCOffset = Oprnd.getImm();

      Oprnd = Inst.getOperand(4);
      // Fifth operand should be X86::No_Register
      if (Oprnd.isReg()) {
        if (Oprnd.getReg() != X86::NoRegister) {
          assert(false && "Unexpected fifth operand - non-zero register in jmp "
                          "instruction of PLT entry");
        }
      } else {
        assert(false && "Unexpected fifth operand - non-register in jmp "
                        "instruction of PLT entry");
      }

      // Get dynamic relocation in .got.plt section corresponding to the PLT
      // entry. The relocation offset is calculated by adding the following:
      //    a) offset of jmp instruction + size of the instruction
      //    (representing pc-related addressing) b) jmp target offset in the
      //    instruction
      uint64_t GotPltRelocOffset = pltEntOff + InstSz + PCOffset;
      const RelocationRef *GotPltReloc =
          MR->getDynRelocAtOffset(GotPltRelocOffset);
      assert(GotPltReloc != nullptr &&
             "Failed to get dynamic relocation for jmp target of PLT entry");

      assert((GotPltReloc->getType() == ELF::R_X86_64_JUMP_SLOT) &&
             "Unexpected relocation type for PLT jmp instruction");
      symbol_iterator CalledFuncSym = GotPltReloc->getSymbol();
      assert(CalledFuncSym != Elf64LEObjFile->symbol_end() &&
             "Failed to find relocation symbol for PLT entry");
      Expected<StringRef> CalledFuncSymName = CalledFuncSym->getName();
      assert(CalledFuncSymName &&
             "Failed to find symbol associated with dynamic "
             "relocation of PLT jmp target.");
      Expected<uint64_t> CalledFuncSymAddr = CalledFuncSym->getAddress();
      assert(CalledFuncSymAddr &&
             "Failed to get called function address of PLT entry");
      CalledFunc = MR->getFunctionAt(CalledFuncSymAddr.get());

      if (CalledFunc == nullptr) {
        // This is an undefined function symbol. Look through the list of
        // known glibc interfaces and construct a Function accordingly.
        CalledFunc =
            ExternalFunctions::Create(*CalledFuncSymName, MR->getModule());
      }
      // Found the section we are looking for
      break;
    }
  }
  return CalledFunc;
}
// Return a global value corresponding to read-only  data.

const Value *X86MachineInstructionRaiser::getOrCreateGlobalRODataValueAtAt(
    const MachineInstr &mi, uint64_t Offset) {
  const Value *RODataValue = MR->getRODataValueAt(Offset);
  if (RODataValue == nullptr) {
    // Only if the imm value is a positive value
    const ELF64LEObjectFile *Elf64LEObjFile =
        dyn_cast<ELF64LEObjectFile>(MR->getObjectFile());
    assert(Elf64LEObjFile != nullptr &&
           "Only 64-bit ELF binaries supported at present.");
    LLVMContext &llvmContext(MF.getFunction().getContext());
    // Check if this is an address in .rodata
    for (section_iterator SecIter : Elf64LEObjFile->sections()) {
      uint64_t SecStart = SecIter->getAddress();
      uint64_t SecEnd = SecStart + SecIter->getSize();
      // We know that SrcImm is a positive value. So, casting it is OK.
      if ((SecStart <= (uint64_t)Offset) && (SecEnd >= (uint64_t)Offset)) {
        if (SecIter->isData()) {
          StringRef SecData;
          SecIter->getContents(SecData);
          unsigned DataOffset = Offset - SecStart;
          const unsigned char *RODataBegin = SecData.bytes_begin() + DataOffset;
          StringRef ROStringRef(reinterpret_cast<const char *>(RODataBegin));
          Constant *StrConstant =
              ConstantDataArray::getString(llvmContext, ROStringRef);
          auto GlobalStrConstVal = new GlobalVariable(
              MR->getModule(), StrConstant->getType(), true /* isConstant */,
              GlobalValue::PrivateLinkage, StrConstant, "rostr");
          // Record the mapping between offset and global value
          MR->addRODataValueAt(GlobalStrConstVal, Offset);
          RODataValue = GlobalStrConstVal;
        } else if (SecIter->isBSS()) {
          // Get symbol name associated with the address
          // Find symbol at Offset
          SymbolRef GlobalDataSym;
          for (auto Symbol : Elf64LEObjFile->symbols()) {
            if (Symbol.getELFType() == ELF::STT_OBJECT) {
              auto SymAddr = Symbol.getAddress();
              assert(SymAddr && "Failed to lookup symbol for global address");
              uint64_t SymAddrVal = SymAddr.get();
              // We have established that Offset is not negative above.
              // So, OK to cast. Check if the memory address Offset is
              // SymAddrVal
              if (SymAddrVal == (unsigned)Offset) {
                GlobalDataSym = Symbol;
                break;
              }
            }
          }
          assert((GlobalDataSym.getObject() != nullptr) &&
                 "Failed to find symbol for global address.");
          Expected<StringRef> GlobalDataSymName = GlobalDataSym.getName();
          assert(GlobalDataSymName &&
                 "Failed to find symbol name for global address");
          // Find if a global value associated with symbol name is
          // already created
          for (GlobalVariable &gv : MR->getModule().globals()) {
            if (gv.getName().compare(GlobalDataSymName.get()) == 0) {
              RODataValue = &gv;
            }
          }
          if (RODataValue == nullptr) {
            Type *ImmType = nullptr;
            uint8_t ImmSize = X86II::getSizeOfImm(mi.getDesc().TSFlags);

            switch (ImmSize) {
            case 8:
              ImmType = Type::getInt64Ty(llvmContext);
              break;
            case 4:
              ImmType = Type::getInt32Ty(llvmContext);
              break;
            case 2:
              ImmType = Type::getInt16Ty(llvmContext);
              break;
            case 1:
              ImmType = Type::getInt8Ty(llvmContext);
              break;
            default:
              assert(false && "Immediate operand of unknown size");
              break;
            }
            Constant *GlobalInit = ConstantInt::get(ImmType, 0);
            auto GlobalVal = new GlobalVariable(
                MR->getModule(), ImmType, false /* isConstant */,
                GlobalValue::PrivateLinkage, GlobalInit,
                GlobalDataSymName->data());
            GlobalVal->setAlignment(getInstructionMemOpSize(mi.getOpcode()));
            GlobalVal->setDSOLocal(true);
            RODataValue = GlobalVal;
          }
        } else {
          assert(false && "Section corresponding to referenced data not found");
        }
        break;
      }
    }
  }
  return RODataValue;
}

// Return a value corresponding to global symbol at Offset referenced in
// MachineInst mi.
Value *
X86MachineInstructionRaiser::getGlobalVariableValueAt(const MachineInstr &mi,
                                                      uint64_t Offset) {
  Value *GlobalVariableValue = nullptr;
  const ELF64LEObjectFile *Elf64LEObjFile =
      dyn_cast<ELF64LEObjectFile>(MR->getObjectFile());
  assert(Elf64LEObjFile != nullptr &&
         "Only 64-bit ELF binaries supported at present.");
  assert((Offset > 0) &&
         "Unhandled non-positive displacement global variable value");
  // Find symbol at Offset
  SymbolRef GlobalDataSym;
  bool GlobalDataSymFound = false;
  unsigned GlobalDataOffset = 0;
  for (auto Symbol : Elf64LEObjFile->symbols()) {
    if (Symbol.getELFType() == ELF::STT_OBJECT) {
      auto SymAddr = Symbol.getAddress();
      auto SymSize = Symbol.getSize();
      assert(SymAddr && "Failed to lookup symbol for global address");
      uint64_t SymAddrVal = SymAddr.get();
      // We have established that Offset is not negative above. So, OK to
      // cast.
      // Check if the memory address Offset is in the range [SymAddrVal,
      // SymAddrVal+SymSize)
      if ((SymAddrVal <= (unsigned)Offset) &&
          ((SymAddrVal + SymSize) > (unsigned)Offset)) {
        GlobalDataSym = Symbol;
        GlobalDataOffset = Offset - SymAddrVal;
        GlobalDataSymFound = true;
        break;
      }
    }
  }
  if (GlobalDataSymFound) {
    Expected<StringRef> GlobalDataSymName = GlobalDataSym.getName();
    assert(GlobalDataSymName && "Failed to find global symbol name.");
    // If this location is an offset from the start of GlobalDataSym, consider
    // it to be a new global variable with the offset as suffix.
    std::string DataSymNameIndexStr(GlobalDataSymName.get().data());
    if (GlobalDataOffset) {
      DataSymNameIndexStr.append(std::to_string(GlobalDataOffset));
    }
    // Find if a global value associated with symbol name is already
    // created
    StringRef GlobalDataSymNameIndexStrRef(DataSymNameIndexStr);
    for (GlobalVariable &gv : MR->getModule().globals()) {
      if (gv.getName().compare(GlobalDataSymNameIndexStrRef) == 0) {
        GlobalVariableValue = &gv;
      }
    }
    if (GlobalVariableValue == nullptr) {
      // Get all necessary information about the global symbol.
      llvm::LLVMContext &llvmContext(MF.getFunction().getContext());
      DataRefImpl symbImpl = GlobalDataSym.getRawDataRefImpl();
      // get symbol
      auto symb = Elf64LEObjFile->getSymbol(symbImpl);
      // get symbol size
      uint64_t symbSize = symb->st_size;
      GlobalValue::LinkageTypes linkage;
      switch (symb->getBinding()) {
      case ELF::STB_GLOBAL:
        linkage = GlobalValue::ExternalLinkage;
        break;
      default:
        assert(false && "Unhandled global symbol binding type");
      }

      // Check that symbol type is data object, representing a variable or
      // array etc.
      assert((symb->getType() == ELF::STT_OBJECT) &&
             "Function symbol type expected. Not found");
      Type *GlobalValTy = nullptr;
      unsigned short MemOpSize = getInstructionMemOpSize(mi.getOpcode());
      switch (MemOpSize) {
      case 8:
        GlobalValTy = Type::getInt64Ty(llvmContext);
        break;
      case 4:
        GlobalValTy = Type::getInt32Ty(llvmContext);
        break;
      case 2:
        GlobalValTy = Type::getInt16Ty(llvmContext);
        break;
      case 1:
        GlobalValTy = Type::getInt8Ty(llvmContext);
        break;
      default:
        assert(false && "Unexpected symbol size");
      }
      // get symbol value - this is the virtual address of symbol's value
      uint64_t symVirtualAddr = symb->st_value;

      // get the initial value of the global data symbol at symVirtualAddr
      // from the section that contains the virtual address symVirtualAddr.
      // In executable and shared object files, st_value holds a virtual
      // address.
      uint64_t symbVal = 0;
      for (section_iterator SecIter : Elf64LEObjFile->sections()) {
        uint64_t SecStart = SecIter->getAddress();
        uint64_t SecEnd = SecStart + SecIter->getSize();
        if ((SecStart <= symVirtualAddr) && (SecEnd >= symVirtualAddr)) {
          // Get the initial symbol value only if this is not a bss section.
          // Else, symVal is already initialized to 0.
          if (SecIter->isBSS()) {
            linkage = GlobalValue::CommonLinkage;
          } else {
            StringRef SecData;
            SecIter->getContents(SecData);
            unsigned index = symVirtualAddr - SecStart;
            const unsigned char *beg = SecData.bytes_begin() + index;
            char shift = 0;
            while (symbSize-- > 0) {
              // We know this is little-endian
              symbVal = ((*beg++) << shift) | symbVal;
              shift += 8;
            }
          }
          break;
        }
      }
      Constant *GlobalInit = ConstantInt::get(GlobalValTy, symbVal);
      auto GlobalVal = new GlobalVariable(
          MR->getModule(), GlobalValTy, false /* isConstant */, linkage,
          GlobalInit, GlobalDataSymNameIndexStrRef);
      GlobalVal->setAlignment(MemOpSize);
      GlobalVal->setDSOLocal(true);
      GlobalVariableValue = GlobalVal;
    }
  } else {
    GlobalVariableValue =
        const_cast<Value *>(getOrCreateGlobalRODataValueAtAt(mi, Offset));
  }
  assert((GlobalVariableValue != nullptr) && "Failed to global variable value");
  return GlobalVariableValue;
}

// Construct and return a Value* corresponding to PC-relative memory address
// access. Insert any intermediate values created in the process into
// curBlock.
// Construct and return a Value* corresponding to non-stack memory address
// expression in MachineInstr mi. Insert any intermediate values created in
// the process into curBlock. NOTE: This returns a value that may need to be
// loaded from if the expression does not involve global variable or
// dereferencing the global variable if expression involves global variable.
Value *
X86MachineInstructionRaiser::getMemoryAddressExprValue(const MachineInstr &mi,
                                                       BasicBlock *curBlock) {
  Value *memrefValue = nullptr;
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  // Should have found the index of the memory reference operand
  assert(memoryRefOpIndex != -1 &&
         "Unable to find memory reference operand of a load/store instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);

  // LLVM represents memory operands using 5 operands
  //    viz., <opcode> BaseReg, ScaleAmt, IndexReg, Disp, Segment, ...
  // The disassembly in AT&T syntax is shown as
  //      Segment:Disp(BaseReg, IndexReg, ScaleAmt).
  // or as
  //      Segment:[BaseReg + Disp + IndexReg * ScaleAmt]
  // in Intel syntax.
  // effective address is calculated to be Segment:[BaseReg + IndexReg *
  // ScaleAmt + Disp] Segment is typically X86::NoRegister.

  assert(mi.getOperand(memoryRefOpIndex + X86::AddrSegmentReg).getReg() ==
             X86::NoRegister &&
         "Expect no segment register");
  // Construct non-stack memory referencing value
  unsigned BaseReg = memRef.Base.Reg;
  unsigned IndexReg = memRef.IndexReg;
  unsigned ScaleAmt = memRef.Scale;
  int Disp = memRef.Disp;
  const MachineOperand &SegRegOperand =
      mi.getOperand(memoryRefOpIndex + X86::AddrSegmentReg);
  // For now, we assume default segment DS (and hence no specification of
  // Segment register.
  assert(SegRegOperand.isReg() && (SegRegOperand.getReg() == X86::NoRegister) &&
         "Unhandled memory reference instruction with non-zero segment "
         "register");

  // Non-stack memory address is supported by this function.
  uint64_t BaseSupReg = find64BitSuperReg(BaseReg);
  assert((BaseSupReg != x86RegisterInfo->getStackRegister()) &&
         (BaseSupReg != x86RegisterInfo->getFramePtr()) &&
         "Not yet supported: Abstraction of value representing stack-based "
         "address expression");
  // IndexReg * ScaleAmt
  // Generate mul scaleAmt, IndexRegVal, if IndexReg is not 0.
  if (IndexReg != X86::NoRegister) {
    Value *IndexRegVal = getRegValue(IndexReg);
    switch (ScaleAmt) {
    case 0:
      break;
    case 1:
      memrefValue = IndexRegVal;
      break;
    default: {
      Type *MulValTy = IndexRegVal->getType();
      Value *ScaleAmtValue = ConstantInt::get(MulValTy, ScaleAmt);
      Instruction *MulInst =
          BinaryOperator::CreateMul(ScaleAmtValue, IndexRegVal);
      curBlock->getInstList().push_back(MulInst);
      memrefValue = MulInst;
    } break;
    }
  }

  // BaseReg + IndexReg*ScaleAmt
  // Generate add BaseRegVal, memrefVal (if IndexReg*ScaleAmt was computed)

  if (BaseReg != X86::NoRegister) {
    Value *BaseRegVal = getRegValue(BaseReg);
    if (memrefValue != nullptr) {
      Instruction *AddInst = BinaryOperator::CreateAdd(BaseRegVal, memrefValue);
      curBlock->getInstList().push_back(AddInst);
      memrefValue = AddInst;
    } else {
      memrefValue = BaseRegVal;
    }
  }

  // BaseReg + Index*ScaleAmt + Disp
  //
  if (Disp != 0) {
    if (memrefValue != nullptr) {
      // Generate add memrefVal, Disp
      Type *DispTy = memrefValue->getType();
      Value *DispValue = ConstantInt::get(DispTy, Disp);
      Instruction *AddInst = BinaryOperator::CreateAdd(memrefValue, DispValue);
      curBlock->getInstList().push_back(AddInst);
      memrefValue = AddInst;
    } else {
      // Check that this is an instruction of the kind
      // mov %rax, 0x605798 which in reality is
      // mov %rax, 0x605798(X86::NoRegister, X86::NoRegister, 1)
      assert(((BaseReg == X86::NoRegister) && (IndexReg == X86::NoRegister) &&
              (ScaleAmt == 1)) &&
             "Unhandled addressing mode in memory addr expression calculation");
      memrefValue = getGlobalVariableValueAt(mi, Disp);
    }
  }
  assert((memrefValue != nullptr) && "Failed to get memory reference value");
  return memrefValue;
}

// Find the (SSA) Value currently mapped to to PhyRes.
// Return nullptr if none exists.
// NOTE : DO NOT call this directly unless you wish to check to
//        see if this is an argument register.
//        Use getRegValue(unsigned PReg) instead.

// Find SSA value associated with physical register PReg.
// If the PReg is an argument register and hence does not have a
// previous definition, function prototype is consulted to return
// the corresponding value. In that case, return argument value
// associated with physical register PReg according to C calling
// convention.
// NOTE : This is the preferred API to get the SSA value associated
//        with PReg. Do not use findPhysRegSSAValue(unsigned) as you
//        do not need to. See comment of that function for more details.

Value *X86MachineInstructionRaiser::getRegValue(unsigned PReg) {
  Value *PRegValue = findPhysRegSSAValue(PReg);

  // Just return the value associated with PReg, if one exists.
  if (PRegValue == nullptr) {
    int pos = getArgumentNumber(PReg);

    // If PReg is an argument register, get its value from function
    // argument list.
    if (pos > 0) {
      // Get the value only if the function has an argument at
      // pos.
      if (pos <= (int)raisedFunction->arg_size()) {
        Function::arg_iterator argIter = raisedFunction->arg_begin() + pos - 1;
        PRegValue = argIter;
      }
    }
  }
  return PRegValue;
}

// Get the 64-bit super register of PhysReg. Return PhysReg is it is
// a 64-bit register.
// Add a new PhysReg-Val pair if no mapping for PhysReg exists
// Replace the mapping to PhysReg-Val if one already exists.
Type *
X86MachineInstructionRaiser::getReturnTypeFromMBB(MachineBasicBlock &MBB) {
  Type *returnType = nullptr;

  // Check liveness of EAX in the return block. We assume that EAX (or
  // RAX) would have to be defined in the return block.
  // TODO : We may have to revisit this assumption, if needed.

  MachineBasicBlock::const_iterator I(MBB.back());
  if (I != MBB.begin()) {
    do {
      --I;
      // Check if any of RAX, EAX, AX or AL are defined
      if (I->getDesc().getNumDefs() != 0) {
        const MachineOperand &MO = I->getOperand(0);
        if (!MO.isReg()) {
          continue;
        }
        unsigned PReg = MO.getReg();
        if (!TargetRegisterInfo::isPhysicalRegister(PReg)) {
          continue;
        }
        if (PReg == X86::RAX) {
          if (returnType == nullptr) {
            returnType = Type::getInt64Ty(MF.getFunction().getContext());
            break;
          } else {
            assert(returnType->isIntegerTy() &&
                   returnType->getScalarSizeInBits() == 64 &&
                   "Inconsistency while discovering return type");
          }
        } else if (PReg == X86::EAX) {
          if (returnType == nullptr) {
            returnType = Type::getInt32Ty(MF.getFunction().getContext());
            break;
          } else {
            assert(returnType->isIntegerTy() &&
                   returnType->getScalarSizeInBits() == 32 &&
                   "Inconsistency while discovering return type");
          }
        } else if (PReg == X86::AX) {
          if (returnType == nullptr) {
            returnType = Type::getInt16Ty(MF.getFunction().getContext());
            break;
          } else {
            assert(returnType->isIntegerTy() &&
                   returnType->getScalarSizeInBits() == 16 &&
                   "Inconsistency while discovering return type");
          }
        } else if (PReg == X86::AL) {
          if (returnType == nullptr) {
            returnType = Type::getInt8Ty(MF.getFunction().getContext());
            break;
          } else {
            assert(returnType->isIntegerTy() &&
                   returnType->getScalarSizeInBits() == 8 &&
                   "Inconsistency while discovering return type");
          }
        }
      }
    } while (I != MBB.begin());
  }
  return returnType;
}

Type *X86MachineInstructionRaiser::getFunctionReturnType() {
  Type *returnType = nullptr;
  SmallVector<MachineBasicBlock *, 8> WorkList;
  BitVector BlockVisited(MF.getNumBlockIDs(), false);

  assert(x86TargetInfo.is64Bit() && "Only x86_64 binaries supported for now");

  // Find a return block. It is sufficient to get the dominator tree path
  // whose leaf is one of the return blocks to find the return type. This
  // type should be the same on any of the dominator paths from entry to
  // return block.
  MachineBasicBlock *RetBlock = nullptr;
  for (MachineBasicBlock &MBB : MF) {
    if (MBB.isReturnBlock()) {
      RetBlock = &MBB;
      break;
    }
  }

  WorkList.push_back(RetBlock);

  while (!WorkList.empty()) {
    MachineBasicBlock *N = WorkList.pop_back_val();
    assert(!BlockVisited[N->getNumber()] &&
           "Encountered previously visited block");
    // Mark block as visited
    BlockVisited.set(N->getNumber());
    returnType = getReturnTypeFromMBB(*N);
    if (returnType != nullptr) {
      return returnType;
    }
    for (auto P : N->predecessors()) {
      WorkList.push_back(P);
    }
  }
  return nullptr;
}

// Construct prototype of the Function for the MachineFunction being raised.
FunctionType *X86MachineInstructionRaiser::getRaisedFunctionPrototype() {

  if (raisedFunction == nullptr) {
    // Cleanup NOOP instructions from all MachineBasicBlocks
    deleteNOOPInstrMF();

    MF.getRegInfo().freezeReservedRegs(MF);
    Type *returnType = nullptr;
    std::vector<Type *> argTypeVector;

    // 1. Discover function arguments.
    // Build live-ins for all blocks
    LivePhysRegs liveInPhysRegs;
    for (MachineBasicBlock &MBB : MF) {
      computeAndAddLiveIns(liveInPhysRegs, MBB);
    }

    //    Get the live-in values of the entry block. These should be the
    //    arguments.
    MachineBasicBlock &MBB = MF.front();
    MBB.sortUniqueLiveIns();

    std::set<MCPhysReg> LiveInRegs;

    for (const auto &LI : MBB.liveins()) {
      LiveInRegs.emplace(LI.PhysReg);
    }
    buildFuncArgTypeVector(LiveInRegs, argTypeVector);
    // 2. Discover function return type
    returnType = getFunctionReturnType();
    // If we are unable to discover the return type assume that the return
    // type is void.
    // TODO : Refine this once support is added to discover arguments passed
    // on the stack??
    if (returnType == nullptr) {
      returnType = Type::getVoidTy(MF.getFunction().getContext());
    }

    // The Function object associated with current MachineFunction object
    // is only a place holder. It was created to facilitate creation of
    // MachineFunction object with a prototype void functionName(void).
    // The Module object contains this place-holder Function object in its
    // FunctionList. Since the return type and arguments are now discovered,
    // we need to replace this place holder Function object in module with the
    // correct Function object being created now.
    // 1. Get the current function name
    StringRef functionName = MF.getFunction().getName();
    Module &module = MR->getModule();
    // 2. Get the corresponding Function* registered in module
    Function *tempFunctionPtr = module.getFunction(functionName);
    assert(tempFunctionPtr != nullptr && "Function not found in module list");
    // 4. Delete the tempFunc from module list to allow for the creation of
    //    the real function to add the correct one to FunctionList of the
    //    module.
    module.getFunctionList().remove(tempFunctionPtr);
    // 3. Now create a function type using the discovered argument
    //    types and return value.
    FunctionType *FT =
        FunctionType::get(returnType, argTypeVector, false /* isVarArg*/);
    // 4. Create the real Function now that we have discovered the arguments.
    raisedFunction = Function::Create(FT, GlobalValue::ExternalLinkage,
                                      functionName, &module);

    // Set global linkage
    raisedFunction->setLinkage(GlobalValue::ExternalLinkage);
    // Set C calling convention
    raisedFunction->setCallingConv(CallingConv::C);
    // TODO : Set other function attributes as needed.
    // Add argument names to the function.
    // Note: Call to arg_begin() calls Function::BuildLazyArguments()
    // to build the arguments.
    Function::arg_iterator ArgIt = raisedFunction->arg_begin();
    unsigned numFuncArgs = raisedFunction->arg_size();
    StringRef prefix("arg");
    for (unsigned i = 0; i < numFuncArgs; ++i, ++ArgIt) {
      // Set the name.
      ArgIt->setName(prefix + std::to_string(i + 1));
    }

    // Insert the map of raised function to tempFunctionPointer.
    const_cast<ModuleRaiser *>(MR)->insertPlaceholderRaisedFunctionMap(
        raisedFunction, tempFunctionPtr);
  }

  return raisedFunction->getFunctionType();
}

// Find the index of the first memory reference operand.
int X86MachineInstructionRaiser::getMemoryRefOpIndex(const MachineInstr &mi) {
  const MCInstrDesc &Desc = mi.getDesc();
  int memOperandNo = X86II::getMemoryOperandNo(Desc.TSFlags);
  if (memOperandNo >= 0) {
    memOperandNo += X86II::getOperandBias(Desc);
  }
  return memOperandNo;
}

// Check the sizes of the operand register at SrcOpindex and that of the
// corresponding SSA value. Return a value that is either truncated or
// sign-extended version of the SSA Value if their sizes do not match.
// Return the SSA value of the operand register at SrcOpindex, if they match.
// This is handles the situation following pattern of instructions
//   rax <- ...
//   edx <- opcode eax, ...
bool X86MachineInstructionRaiser::insertAllocaInEntryBlock(
    Instruction *alloca) {

  // Avoid using BasicBlock InstrList iterators so that the tool can
  // use LLVM built with LLVM_ABI_BREAKING_CHECKS ON or OFF.
  BasicBlock &EntryBlock = getRaisedFunction()->getEntryBlock();

  BasicBlock::InstListType &InstList = EntryBlock.getInstList();
  if (InstList.size() == 0) {
    InstList.push_back(alloca);
  } else {
    Instruction *Inst = &EntryBlock.back();
    while (Inst != nullptr) {
      if (Inst->getOpcode() == Instruction::Alloca) {
        InstList.insert(Inst->getNextNode()->getIterator(), alloca);
        break;
      }
      Inst = Inst->getPrevNode();
    }
    // If there is no alloca instruction yet, push to front
    if (Inst == nullptr) {
      InstList.push_front(alloca);
    }
  }
  return true;
}

Value *X86MachineInstructionRaiser::matchSSAValueToSrcRegSize(
    const MachineInstr &mi, unsigned SrcOpIndex, BasicBlock *curBlock) {
  unsigned SrcOpSize = getPhysRegOperandSize(mi, SrcOpIndex);
  Value *SrcOpValue = getRegValue(mi.getOperand(SrcOpIndex).getReg());
  const DataLayout &dataLayout = MR->getModule().getDataLayout();

  // Generate the appropriate cast instruction if the sizes of the current
  // source value and that of the source register do not match.
  uint64_t SrcValueSize =
      dataLayout.getTypeSizeInBits(SrcOpValue->getType()) / sizeof(uint64_t);

  assert(SrcValueSize <= sizeof(uint64_t) && SrcOpSize <= sizeof(uint64_t) &&
         "Unexpected source Value size in move instruction");

  if (SrcOpSize != SrcValueSize) {
    Type *CastTy = getPhysRegOperandType(mi, SrcOpIndex);
    CastInst *CInst = CastInst::Create(
        CastInst::getCastOpcode(SrcOpValue, false, CastTy, false), SrcOpValue,
        CastTy);
    curBlock->getInstList().push_back(CInst);
    SrcOpValue = CInst;
  }
  return SrcOpValue;
}

// Record information to raise a terminator instruction in a later pass.
bool X86MachineInstructionRaiser::recordMachineInstrInfo(const MachineInstr &mi,
                                                         BasicBlock *curBlock) {
  // Return instruction is a Terminator. There is nothing to record.
  // Its raising is handled as a normal instruction. This function should not
  // be called when mi is a call instruction.
  assert(mi.isTerminator() && "Not a terminator instruction - can not record "
                              "control transfer information");
  assert(!mi.isReturn() &&
         "Unexpected attempt to record info for a return instruction");

  // Set common info of the record
  ControlTransferInfo *CurCTInfo = new ControlTransferInfo;
  CurCTInfo->CandidateMachineInstr = &mi;
  CurCTInfo->CandidateBlock = curBlock;

  const MCInstrDesc &MCID = mi.getDesc();
  // Save all values of implicitly used operands
  unsigned ImplUsesCount = MCID.getNumImplicitUses();
  if (ImplUsesCount > 0) {
    const MCPhysReg *ImplUses = MCID.getImplicitUses();
    for (unsigned i = 0; i < ImplUsesCount; i++) {
      Value *val = getRegValue(ImplUses[i]);
      CurCTInfo->RegValues.push_back(val);
    }
  }
  CurCTInfo->Raised = false;
  CTInfo.push_back(CurCTInfo);

  return true;
}

std::pair<std::map<unsigned int, Value *>::iterator, bool>
X86MachineInstructionRaiser::updatePhysRegSSAValue(unsigned int PhysReg,
                                                   Value *Val) {
  // Always convert PhysReg to the 64-bit version.
  unsigned int SuperReg = find64BitSuperReg(PhysReg);

  if (findPhysRegSSAValue(SuperReg)) {
    physToValueMap.erase(SuperReg);
  }
  return physToValueMap.emplace(SuperReg, Val);
}

bool X86MachineInstructionRaiser::raisePushInstruction(const MachineInstr &mi) {
  // TODO : Need to handle push instructions other than those that push bp to
  // stack.
  const MCInstrDesc &MCIDesc = mi.getDesc();
  uint64_t MCIDTSFlags = MCIDesc.TSFlags;

  if ((MCIDTSFlags & X86II::FormMask) == X86II::AddRegFrm) {
    // This is a register PUSH. If the source is base pointer,
    // not need to raise the instruction.
    if (find64BitSuperReg(mi.getOperand(0).getReg()) == X86::RBP) {
      return true;
    } else {
      assert(false && "Unhandled PUSH instruction that stores a register "
                      "other than frame pointer");
    }
  } else {
    assert(false && "Unhandled PUSH instruction with source operand other "
                    "than AddrRegFrm");
  }
  return false;
}

bool X86MachineInstructionRaiser::raisePopInstruction(const MachineInstr &mi) {
  // TODO : Need to handle pop instructions other than those that restore bp
  // from stack.
  const MCInstrDesc &MCIDesc = mi.getDesc();
  uint64_t MCIDTSFlags = MCIDesc.TSFlags;

  if ((MCIDTSFlags & X86II::FormMask) == X86II::AddRegFrm) {
    // This is a register POP. If the source is base pointer,
    // not need to raise the instruction.
    if (mi.definesRegister(X86::RBP) || mi.definesRegister(X86::EBP)) {
      return true;
    } else {
      assert(false && "Unhandled POP instruction that restores a register "
                      "other than frame pointer");
    }
  } else {
    if (getInstructionKind(mi.getOpcode()) == InstructionKind::LEAVE_OP) {
      return true;
    }
    assert(false && "Unhandled POP instruction with source operand other "
                    "than AddrRegFrm");
  }
  return false;
}

bool X86MachineInstructionRaiser::raiseConvertBWWDDQMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  const MCInstrDesc &MIDesc = mi.getDesc();
  unsigned int opcode = mi.getOpcode();
  LLVMContext &llvmContext(MF.getFunction().getContext());

  assert(MIDesc.getNumImplicitUses() == 1 && MIDesc.getNumImplicitDefs() == 1 &&
         "Unexpected number of implicit uses and defs in cbw/cwde/cdqe "
         "instruction");
  MCPhysReg UseReg = MIDesc.ImplicitUses[0];
  MCPhysReg DefReg = MIDesc.ImplicitDefs[0];
  Type *TargetTy = nullptr;

  if (opcode == X86::CDQE) {
    assert(is32BitPhysReg(UseReg) &&
           "Unexpected non-32-bit register in cdqe instruction");
    assert(is64BitPhysReg(DefReg) &&
           "Unexpected non-64-bit register in cdqe instruction");
    TargetTy = Type::getInt64Ty(llvmContext);
  } else if (opcode == X86::CBW) {
    assert(is8BitPhysReg(UseReg) &&
           "Unexpected non-32-bit register in cbw instruction");
    assert(is16BitPhysReg(DefReg) &&
           "Unexpected non-64-bit register in cbw instruction");
    TargetTy = Type::getInt16Ty(llvmContext);
  } else if (opcode == X86::CWDE) {
    assert(is16BitPhysReg(UseReg) &&
           "Unexpected non-32-bit register in cwde instruction");
    assert(is32BitPhysReg(DefReg) &&
           "Unexpected non-64-bit register in cwde instruction");
    TargetTy = Type::getInt32Ty(llvmContext);
  }
  assert(TargetTy != nullptr &&
         "Target type not set for cbw/cwde/cdqe instruction");
  Value *UseValue = getRegValue(UseReg);

  // Generate sign-extend instruction
  SExtInst *SextInst = new SExtInst(UseValue, TargetTy);
  curBlock->getInstList().push_back(SextInst);

  // Update the value mapping of DefReg
  updatePhysRegSSAValue(DefReg, SextInst);
  return true;
}

bool X86MachineInstructionRaiser::raiseConvertWDDQQOMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  const MCInstrDesc &MIDesc = mi.getDesc();
  unsigned int opcode = mi.getOpcode();
  LLVMContext &llvmContext(MF.getFunction().getContext());

  assert(MIDesc.getNumImplicitUses() == 1 && MIDesc.getNumImplicitDefs() == 2 &&
         "Unexpected number of implicit uses and defs in cwd/cdq/cqo "
         "instruction");
  MCPhysReg UseReg = MIDesc.ImplicitUses[0];
  MCPhysReg DefReg_0 = MIDesc.ImplicitDefs[0];
  MCPhysReg DefReg_1 = MIDesc.ImplicitDefs[1];
  Type *TargetTy = nullptr;
  Type *UseRegTy = nullptr;

  if (opcode == X86::CWD) {
    assert(
        is16BitPhysReg(UseReg) && is16BitPhysReg(DefReg_0) &&
        is16BitPhysReg(DefReg_1) && (UseReg == DefReg_0) &&
        "Unexpected characteristics of use/def registers in cwd instruction");
    TargetTy = Type::getInt32Ty(llvmContext);
    UseRegTy = Type::getInt16Ty(llvmContext);
  } else if (opcode == X86::CDQ) {
    assert(
        is32BitPhysReg(UseReg) && is32BitPhysReg(DefReg_0) &&
        is32BitPhysReg(DefReg_1) && (UseReg == DefReg_0) &&
        "Unexpected characteristics of use/def registers in cdq instruction");
    TargetTy = Type::getInt64Ty(llvmContext);
    UseRegTy = Type::getInt32Ty(llvmContext);
  } else if (opcode == X86::CQO) {
    assert(
        is64BitPhysReg(UseReg) && is16BitPhysReg(DefReg_0) &&
        is64BitPhysReg(DefReg_1) && (UseReg == DefReg_0) &&
        "Unexpected characteristics of use/def registers in cdo instruction");
    TargetTy = Type::getInt128Ty(llvmContext);
    UseRegTy = Type::getInt64Ty(llvmContext);
  }

  assert((TargetTy != nullptr) && (UseRegTy != nullptr) &&
         "Target type not set for cwd/cdq/cqo instruction");
  Value *UseValue = getRegValue(UseReg);

  // Generate sign-extend instruction
  SExtInst *TargetSextInst = new SExtInst(UseValue, TargetTy);
  assert(UseValue->getType()->getScalarSizeInBits() ==
             UseRegTy->getScalarSizeInBits() &&
         "Mismatched types in cwd/cdq/cqo instruction");
  curBlock->getInstList().push_back(TargetSextInst);

  // Logical Shift TargetSextInst by n-bits (where n is the size of UserRegTy)
  // to get the high bytes and set DefReg_1 to the resulting value.
  Value *ShiftAmount = ConstantInt::get(
      TargetTy, UseRegTy->getScalarSizeInBits(), false /* isSigned */);
  Instruction *LShrInst =
      BinaryOperator::CreateLShr(TargetSextInst, ShiftAmount);
  curBlock->getInstList().push_back(LShrInst);
  // Truncate LShrInst to get the high bytes
  Instruction *HighBytesInst =
      CastInst::Create(Instruction::Trunc, LShrInst, UseRegTy);
  curBlock->getInstList().push_back(HighBytesInst);
  // Update the value mapping of DefReg_1
  updatePhysRegSSAValue(DefReg_1, HighBytesInst);

  return true;
}

bool X86MachineInstructionRaiser::raiseMoveImmToRegMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  unsigned int opcode = mi.getOpcode();
  // LLVMContext &llvmContext(MF.getFunction().getContext());
  bool success = false;

  switch (opcode) {
  case X86::MOV8ri:
  case X86::MOV16ri:
  case X86::MOV32ri:
  case X86::MOV64ri: {
    unsigned DestOpIndex = 0, SrcOpIndex = 1;
    const MachineOperand &DestOp = mi.getOperand(DestOpIndex);
    const MachineOperand &SrcOp = mi.getOperand(SrcOpIndex);
    assert(mi.getNumExplicitOperands() == 2 && DestOp.isReg() &&
           SrcOp.isImm() &&
           "Expecting exactly two operands for move imm-to-reg instructions");

    unsigned int DstPReg = DestOp.getReg();
    int64_t SrcImm = SrcOp.getImm();

    unsigned int DstPRegSize = getPhysRegOperandSize(mi, DestOpIndex);

    Type *ImmTy = getImmOperandType(mi, 1);
    Value *srcValue = nullptr;

    assert(DstPRegSize ==
               (ImmTy->getPrimitiveSizeInBits() / sizeof(uint64_t)) &&
           "Mismatched imm and dest sizes in move imm to reg instruction.");
    if (opcode == X86::MOV64ri) {
      srcValue =
          const_cast<Value *>(getOrCreateGlobalRODataValueAtAt(mi, SrcImm));
    } else {
      srcValue = ConstantInt::get(ImmTy, SrcImm);
    }
    // Update the value mapping of dstReg
    updatePhysRegSSAValue(DstPReg, srcValue);
    success = true;
  } break;
  default:
    assert(false && "Unhandled move imm-to-reg instruction");
    break;
  }
  return success;
}

bool X86MachineInstructionRaiser::raiseMoveRegToRegMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  unsigned int opcode = mi.getOpcode();
  LLVMContext &llvmContext(MF.getFunction().getContext());
  bool success = false;
  unsigned DstIndex = 0;
  unsigned SrcIndex = 1;
  assert(mi.getNumExplicitOperands() == 2 && mi.getOperand(DstIndex).isReg() &&
         mi.getOperand(SrcIndex).isReg() &&
         "Expecting exactly two operands for move reg-to-reg instructions");

  unsigned int DstPReg = mi.getOperand(DstIndex).getReg();
  unsigned int SrcPReg = mi.getOperand(SrcIndex).getReg();

  // Get source value
  Value *srcValue = getRegValue(SrcPReg);

  switch (opcode) {
  case X86::MOVSX16rr8:
  case X86::MOVSX32rr8:
  case X86::MOVSX32rr16:
  case X86::MOVSX64rr8:
  case X86::MOVSX64rr16:
  case X86::MOVSX64rr32:
  case X86::MOVZX16rr8:
  case X86::MOVZX32rr8:
  case X86::MOVZX32rr16:
  case X86::MOVZX64rr8:
  case X86::MOVZX64rr16: {
    Type *Ty = nullptr;
    Instruction::CastOps Cast;
    // Check for sanity of source value
    assert(srcValue &&
           "Encountered instruction with undefined source register");

    switch (opcode) {
    case X86::MOVSX16rr8: {
      assert(is16BitPhysReg(DstPReg) &&
             "Not found expected 16-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt16Ty(llvmContext);
      Cast = Instruction::SExt;
    } break;
    case X86::MOVSX32rr8:
    case X86::MOVSX32rr16: {
      assert(is32BitPhysReg(DstPReg) &&
             "Not found expected 32-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt32Ty(llvmContext);
      Cast = Instruction::SExt;
    } break;
    case X86::MOVSX64rr8:
    case X86::MOVSX64rr16:
    case X86::MOVSX64rr32: {
      assert(is64BitPhysReg(DstPReg) &&
             "Not found expected 64-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt64Ty(llvmContext);
      Cast = Instruction::SExt;
    } break;
    case X86::MOVZX16rr8: {
      assert(is16BitPhysReg(DstPReg) &&
             "Not found expected 16-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt16Ty(llvmContext);
      Cast = Instruction::ZExt;
    } break;
    case X86::MOVZX32rr8:
    case X86::MOVZX32rr16: {
      assert(is32BitPhysReg(DstPReg) &&
             "Not found expected 32-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt32Ty(llvmContext);
      Cast = Instruction::ZExt;
    } break;
    case X86::MOVZX64rr8:
    case X86::MOVZX64rr16: {
      assert(is64BitPhysReg(DstPReg) &&
             "Not found expected 64-bit destination register - movsx "
             "instruction");
      Ty = Type::getInt64Ty(llvmContext);
      Cast = Instruction::ZExt;
    } break;
    default:
      assert(false && "Should not reach here! - movsx instruction");
    }

    assert(Ty != nullptr && "Failed to set type - movsx instruction");
    CastInst *CInst = CastInst::Create(Cast, srcValue, Ty);
    curBlock->getInstList().push_back(CInst);

    // Update the value mapping of dstReg
    updatePhysRegSSAValue(DstPReg, CInst);
    success = true;

  } break;

  case X86::MOV64rr:
  case X86::MOV32rr:
  case X86::MOV16rr:
  case X86::MOV8rr: {

    unsigned int DstPRegSize = getPhysRegOperandSize(mi, DstIndex);
    unsigned int SrcPRegSize = getPhysRegOperandSize(mi, SrcIndex);

    // Verify sanity of the instruction.
    assert(DstPRegSize != 0 && DstPRegSize == SrcPRegSize &&
           "Unexpected sizes of source and destination registers size differ "
           "in mov instruction");

    // Source register is either stack pointer or base pointer and destination
    // register is correspondingly not base pointer or stack pointer.
    if (((find64BitSuperReg(SrcPReg) == X86::RSP) &&
         !(find64BitSuperReg(DstPReg) == X86::RBP)) ||
        ((find64BitSuperReg(SrcPReg) == X86::RBP) &&
         !(find64BitSuperReg(DstPReg) == X86::RSP))) {
      // This is a mov (e|r)sp to reg or mov (e|r)bp to reg. This is treated
      // as a load effective address instruction lea (%rsp), reg or lea
      // (%rbp), reg
      X86AddressMode stackAddr;
      stackAddr.Base.Reg = SrcPReg;
      unsigned int stackFrameIndex;
      // If this is a new stack reference
      MemRefToFrameIndexMapType::iterator mapIter =
          memRefToFrameIndexMap.find(stackAddr);
      if (mapIter == memRefToFrameIndexMap.end()) {
        // Create an alloca instruction since this memory reference is
        // encountered for the first time.
        Type *Ty = nullptr;
        // Uninitialized stack object size
        uint64_t stackObjectSize = 0;
        unsigned int typeAlignment;
        const DataLayout &dataLayout = MR->getModule().getDataLayout();
        unsigned allocaAddrSpace = dataLayout.getAllocaAddrSpace();

        switch (SrcPReg) {
        case X86::RSP: {
          Ty = Type::getInt64Ty(llvmContext);
          stackObjectSize = 8;
        } break;
        case X86::ESP: {
          Ty = Type::getInt32Ty(llvmContext);
          stackObjectSize = 4;
        } break;
        case X86::SP: {
          Ty = Type::getInt16Ty(llvmContext);
          stackObjectSize = 2;
        } break;
        default:
          assert(false && "Encountered stack register of unexpected size");
          break;
        }

        assert(stackObjectSize != 0 && Ty != nullptr &&
               "Unknown type of operand in memory referencing instruction");
        typeAlignment = dataLayout.getPrefTypeAlignment(Ty);

        // Create alloca instruction to allocate stack slot
        AllocaInst *alloca =
            new AllocaInst(Ty, allocaAddrSpace, 0, typeAlignment);

        // Create a stack slot associated with the alloca instruction
        stackFrameIndex = MF.getFrameInfo().CreateStackObject(
            stackObjectSize, dataLayout.getPrefTypeAlignment(Ty),
            false /* isSpillSlot */, alloca);

        MF.getFrameInfo().setObjectOffset(stackFrameIndex, stackAddr.Disp);

        // Book-keeping: Add memory reference to the map
        memRefToFrameIndexMap.emplace(stackAddr, stackFrameIndex);
        // Add the alloca instruction to entry block
        insertAllocaInEntryBlock(alloca);
      }
      // Stack frame slot already allocated for this memRef
      else {
        stackFrameIndex = (*mapIter).second;
      }
      srcValue = const_cast<AllocaInst *>(
          MF.getFrameInfo().getObjectAllocation(stackFrameIndex));
      // Update the value mapping of dstReg
      updatePhysRegSSAValue(DstPReg, srcValue);
    }
    // Neither of the source nor the destination registers is stack pointer or
    // base pointer. Note that there is no need to raise move (e|r)sp to (e|r)bp
    else if (!((find64BitSuperReg(SrcPReg) == X86::RSP) &&
               (find64BitSuperReg(DstPReg) == X86::RBP))) {
      assert(srcValue &&
             "Encountered mov instruction with undefined source register");
      assert(srcValue->getType()->isSized() &&
             "Unsized source value in move instruction");
      srcValue = matchSSAValueToSrcRegSize(mi, SrcIndex, curBlock);
      // Update the value mapping of dstReg
      updatePhysRegSSAValue(DstPReg, srcValue);
    } else {
      // Make sure this is mov (r|e)sp, (r|e)bp or mov (r|e)bp, (r|e)sp
      assert((((find64BitSuperReg(SrcPReg) == X86::RSP) &&
               (find64BitSuperReg(DstPReg) == X86::RBP)) ||
              ((find64BitSuperReg(SrcPReg) == X86::RBP) &&
               (find64BitSuperReg(DstPReg) == X86::RSP))) &&
             "Unhandled reg-to-reg mov instruction");
    }

    success = true;
  } break;
  default:
    assert(false && "Unhandled move reg-to-reg instruction");
    break;
  }
  return success;
}

bool X86MachineInstructionRaiser::raiseLEAMachineInstr(const MachineInstr &mi,
                                                       BasicBlock *curBlock) {
  unsigned int opcode = mi.getOpcode();

  assert(mi.getNumExplicitOperands() == 6 &&
         "Unexpected number of arguments of lea instruction");
  // Get dest operand
  MachineOperand DestOp = mi.getOperand(0);
  assert(DestOp.isReg() &&
         "Unhandled non-register destination operand in lea instruction");
  unsigned int DestReg = DestOp.getReg();

  int OpIndex = X86II::getMemoryOperandNo(mi.getDesc().TSFlags);
  assert(OpIndex >= 0 && "Failed to get first operand of addressing-mode "
                         "expression in lea instruction");

  MachineOperand BaseRegOp = mi.getOperand(OpIndex + X86::AddrBaseReg);
  assert(BaseRegOp.isReg() &&
         "Unhandled non-register BaseReg operand in lea instruction");
  unsigned int BaseReg = BaseRegOp.getReg();
  Value *EffectiveAddrValue = nullptr;

  // If the basereg refers stack, get the stack allocated object value
  uint64_t BaseSupReg = find64BitSuperReg(BaseReg);
  if ((BaseSupReg == x86RegisterInfo->getStackRegister()) ||
      (BaseSupReg == x86RegisterInfo->getFramePtr())) {
    EffectiveAddrValue = getStackAllocatedValue(mi, curBlock, OpIndex);
  } else {
    MachineOperand ScaleAmtOp = mi.getOperand(OpIndex + X86::AddrScaleAmt);
    assert(ScaleAmtOp.isImm() &&
           "Unhandled non-immediate ScaleAmt operand in lea instruction");

    MachineOperand IndexRegOp = mi.getOperand(OpIndex + X86::AddrIndexReg);
    assert(IndexRegOp.isReg() &&
           "Unhandled non-register IndexReg operand in lea instruction");

    unsigned int IndexReg = IndexRegOp.getReg();

    MachineOperand SegmentRegOp = mi.getOperand(OpIndex + X86::AddrSegmentReg);
    assert(SegmentRegOp.getReg() == X86::NoRegister &&
           "Unhandled vaule of SegmentReg operand in lea instruction");

    MachineOperand Disp = mi.getOperand(OpIndex + X86::AddrDisp);
    assert(Disp.isImm() &&
           "Unhandled non-immediate Disp operand in lea instruction");

    // Check the sanity of register sizes
    if ((opcode == X86::LEA64r) || (opcode == X86::LEA64_32r)) {
      // lea64mem (see LEA64 and LEA64_32r description in
      // X86InstrArithmetic.td)
      assert((is64BitPhysReg(BaseReg) || BaseReg == X86::NoRegister) &&
             "Unexpected non-64 bit base register in lea instruction");
      assert(((IndexReg == X86::NoRegister) || is64BitPhysReg(IndexReg)) &&
             "Unexpected index register type in lea instruction");
      assert(IndexReg != x86RegisterInfo->getStackRegister() &&
             "Unexpected stack pointer register as indexReg operand of lea "
             "instruction");
      if (opcode == X86::LEA64_32r) {
        assert(is32BitPhysReg(DestReg) &&
               "Unexpected non-32 bit destination register in lea instruction");
      } else {
        assert(is64BitPhysReg(DestReg) &&
               "Unexpected non-32 bit dest register in lea instruction");
      }
    } else if (opcode == X86::LEA32r) {
      assert((is32BitPhysReg(BaseReg) || BaseReg == X86::NoRegister) &&
             "Unexpected non-32 bit base register in lea instruction");
      assert(((IndexReg == X86::NoRegister) || is32BitPhysReg(IndexReg)) &&
             "Unexpected indext register type in lea instruction");
      assert(is32BitPhysReg(DestReg) &&
             "Unexpected non-32 bit dest register in lea instruction");
    } else if (opcode == X86::LEA16r) {
      assert((is16BitPhysReg(BaseReg) || BaseReg == X86::NoRegister) &&
             "Unexpected non-16 bit source register in lea instruction");
      assert(((IndexReg == X86::NoRegister) || is16BitPhysReg(IndexReg)) &&
             "Unexpected indext register type in lea instruction");
      assert(is16BitPhysReg(DestReg) &&
             "Unexpected non-16 bit dest register in lea instruction");
    }

    EffectiveAddrValue = getMemoryAddressExprValue(mi, curBlock);
  }

  assert((EffectiveAddrValue != nullptr) &&
         "Failed to get effective address value");

  // Update the value mapping of DestReg
  updatePhysRegSSAValue(DestReg, EffectiveAddrValue);
  return true;
}

bool X86MachineInstructionRaiser::raiseBinaryOpRegToRegMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {

  auto MCID = mi.getDesc();
  std::vector<Value *> Uses;
  for (const MachineOperand &MO : mi.explicit_uses()) {
    assert(MO.isReg() &&
           "Unexpected non-register operand in binary op instruction");
    unsigned int SrcReg = MO.getReg();
    Value *srcValue = getRegValue(SrcReg);
    Uses.push_back(srcValue);
  }
  // Verify there are exactly 2 use operands.
  assert(Uses.size() == 2 &&
         "Expecting exactly two operands for register binary op instruction");

  // Figure out the destination register, corresponding value and the
  // binary operator.
  unsigned int dstReg = X86::NoRegister;
  Value *dstValue = nullptr;

  // Construct the appropriate binary operation instruction
  switch (mi.getOpcode()) {
  case X86::ADD64rr:
  case X86::ADD32rr:
    // Verify the def operand is a register.
    assert(mi.getOperand(0).isReg() &&
           "Expecting destination of add instruction to be a register operand");
    assert((MCID.getNumDefs() == 1) &&
           "Unexpected number of defines in an add instruction");
    assert((Uses.at(0) != nullptr) && (Uses.at(1) != nullptr) &&
           "Unhandled situation: register is used before initialization in "
           "add");
    dstReg = mi.getOperand(0).getReg();
    dstValue = BinaryOperator::CreateNSWAdd(Uses.at(0), Uses.at(1));
    break;
  case X86::IMUL32rr:
  case X86::IMUL64rr:
    // Verify the def operand is a register.
    assert(mi.getOperand(0).isReg() &&
           "Expecting destination of mul instruction to be a register operand");
    assert((MCID.getNumDefs() == 1) &&
           "Unexpected number of defines in a mul instruction");
    assert(
        (Uses.at(0) != nullptr) && (Uses.at(1) != nullptr) &&
        "Unhandled situation: register is used before initialization in mul");
    dstReg = mi.getOperand(0).getReg();
    dstValue = BinaryOperator::CreateNSWMul(Uses.at(0), Uses.at(1));
    break;
  case X86::XOR32rr:
  case X86::XOR64rr: {
    // Verify the def operand is a register.
    const MachineOperand &DestOp = mi.getOperand(0);
    // const MachineOperand &Use1Op = mi.getOperand(1);
    const MachineOperand &Use2Op = mi.getOperand(2);
    assert(DestOp.isReg() &&
           "Expecting destination of xor instruction to be a register operand");
    assert((MCID.getNumDefs() == 1) &&
           MCID.hasImplicitDefOfPhysReg(X86::EFLAGS) &&
           "Unexpected defines in a xor instruction");
    dstReg = DestOp.getReg();
    // Generate an or instruction to set the zero flag if the
    // operands are the same. An instruction such as 'xor $ecx, ecx' is
    // generated to set the register value to 0.
    if ((mi.findTiedOperandIdx(1) == 0) && (dstReg == Use2Op.getReg())) {
      // No instruction to generate. Just set destReg value to 0.
      Type *DestTy = getPhysRegOperandType(mi, 0);
      Value *Val = ConstantInt::get(DestTy, 0, false /* isSigned */);
      dstValue = Val;
    } else {
      assert((Uses.at(0) != nullptr) && (Uses.at(1) != nullptr) &&
             "Unhandled situation: register used before initialization in xor");
      dstValue = BinaryOperator::CreateXor(Uses.at(0), Uses.at(1));
    }
  } break;
  case X86::TEST32rr:
  case X86::TEST64rr:
    assert((MCID.getNumDefs() == 0) &&
           MCID.hasImplicitDefOfPhysReg(X86::EFLAGS) &&
           "Unexpected defines in a test instruction");
    assert((Uses.at(0) != nullptr) && (Uses.at(1) != nullptr) &&
           "Unhandled situation: register is used before initialization in "
           "test");
    dstReg = X86::EFLAGS;
    dstValue = BinaryOperator::CreateAnd(Uses.at(0), Uses.at(1));
    break;
  default:
    assert(false && "Unhandled binary instruction");
  }
  assert(dstValue != nullptr && (dstReg != X86::NoRegister) &&
         "Raising of instruction unimplemented");
  if (isa<Instruction>(dstValue)) {
    curBlock->getInstList().push_back(dyn_cast<Instruction>(dstValue));
  }
  updatePhysRegSSAValue(dstReg, dstValue);
  return true;
}

bool X86MachineInstructionRaiser::raiseBinaryOpMemToRegInstr(
    const MachineInstr &mi, BasicBlock *curBlock, Value *memRefValue) {
  unsigned int opcode = mi.getOpcode();
  const MCInstrDesc &MIDesc = mi.getDesc();

  assert((MIDesc.getNumDefs() == 1) &&
         "Encountered memory load instruction with more than 1 defs");
  unsigned int DestIndex = 0;
  const MachineOperand &DestOp = mi.getOperand(DestIndex);
  assert(DestOp.isReg() &&
         "Expect destination register operand in binary reg/mem instruction");
  unsigned int DestPReg = DestOp.getReg();
  unsigned int memAlignment = getInstructionMemOpSize(opcode);
  Type *DestopTy = getPhysRegOperandType(mi, DestIndex);
  Value *DestValue = getRegValue(DestPReg);
  assert(DestValue != nullptr &&
         "Encountered instruction with undefined register");

  // Verify sanity of the instruction.
  assert((DestValue->getType()->getPrimitiveSizeInBits() / sizeof(uint64_t)) ==
             memAlignment &&
         "Mismatched value type size and instruction size of binary op "
         "instruction");
  // Load the value from memory location of memRefValue.
  // memRefVal is either an AllocaInst (stack access) or GlobalValue (global
  // data access) or an LoadInst that loads an address in memory..
  assert((isa<AllocaInst>(memRefValue) || isa<LoadInst>(memRefValue) ||
          isa<GlobalValue>(memRefValue)) &&
         "Unexpected type of memory reference in binary mem op instruction");
  bool isMemRefGlobalVal = false;
  // If it is a load instruction, convert it to a pointer.
  if (isa<LoadInst>(memRefValue)) {
    LoadInst *ldInst = dyn_cast<LoadInst>(memRefValue);
    if (isa<GlobalValue>(ldInst->getPointerOperand())) {
      isMemRefGlobalVal = true;
    } else {
      // Cast it to a pointer of type of destination operand.
      PointerType *PtrTy = PointerType::get(DestopTy, 0);
      IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefValue, PtrTy);
      curBlock->getInstList().push_back(convIntToPtr);
      memRefValue = convIntToPtr;
    }
  }
  LoadInst *loadInst = nullptr;
  if (isMemRefGlobalVal) {
    // Load the global value.
    loadInst =
        new LoadInst(dyn_cast<LoadInst>(memRefValue)->getPointerOperand());
  } else {
    loadInst = new LoadInst(memRefValue);
  }
  // Insert the instruction that loads memory reference
  loadInst->setAlignment(memAlignment);
  curBlock->getInstList().push_back(loadInst);
  Instruction *BinOpInst = nullptr;

  switch (opcode) {
  case X86::ADD64rm:
  case X86::ADD32rm:
  case X86::ADD16rm:
  case X86::ADD8rm: {
    // Create add instruction
    BinOpInst = BinaryOperator::CreateAdd(DestValue, loadInst);
  } break;
  case X86::OR32rm: {
    // Create add instruction
    BinOpInst = BinaryOperator::CreateOr(DestValue, loadInst);
  } break;
  case X86::IMUL32rm:
  case X86::IMUL32rmi8: {
    // Create mul instruction
    BinOpInst = BinaryOperator::CreateMul(DestValue, loadInst);
  } break;
  default:
    assert(false && "Unhandled binary op mem to reg instruction ");
  }
  // Add instruction to block
  curBlock->getInstList().push_back(BinOpInst);

  // Update PhysReg to Value map
  updatePhysRegSSAValue(DestPReg, BinOpInst);
  return true;
}

bool X86MachineInstructionRaiser::raiseLoadIntToFloatRegInstr(
    const MachineInstr &mi, BasicBlock *curBlock, Value *memRefValue) {

  const unsigned int opcode = mi.getOpcode();
  const MCInstrDesc &MIDesc = mi.getDesc();
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  assert(memoryRefOpIndex == 0 &&
         "Expect memory operand of floating-point load instruction at index 0");
  assert(MIDesc.getNumDefs() == 0 &&
         "Expect no defs in floating-point load instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);
  uint64_t BaseSupReg = find64BitSuperReg(memRef.Base.Reg);
  bool isPCRelMemRef = (BaseSupReg == X86::RIP);

  // Load the value from memory location of memRefValue.
  // memRefVal is either an AllocaInst (stack access) or GlobalValue (global
  // data access) or an effective address value.
  assert((isa<AllocaInst>(memRefValue) || isEffectiveAddrValue(memRefValue) ||
          isa<GlobalValue>(memRefValue)) &&
         "Unexpected type of memory reference in FPU load op instruction");

  LLVMContext &llvmContext(MF.getFunction().getContext());
  if (isPCRelMemRef) {
    // If it is a PC-relative mem ref, memRefValue is a
    // global value loaded from PC-relative memory location. If it is a
    // derived type value, get its element pointer.
    Type *memRefValueTy = memRefValue->getType();
    if (!memRefValueTy->isFloatingPointTy()) {
      assert(memRefValueTy->isPointerTy() &&
             "Unhandled non-pointer type found while attempting to push value "
             "to FPU register stack.");
      Type *memRefValPtrElementTy = memRefValueTy->getPointerElementType();
      switch (memRefValPtrElementTy->getTypeID()) {
      case Type::ArrayTyID: {
        assert(memRefValPtrElementTy->getArrayNumElements() == 1 &&
               "Unexpected number of array elements in value being cast to "
               "float");
        // Make sure the array element type is integer or floating point type.
        Type *arrElemTy = memRefValPtrElementTy->getArrayElementType();
        assert((arrElemTy->isIntegerTy() || arrElemTy->isFloatingPointTy()) &&
               "Unexpected type of data referenced in FPU register stack "
               "load instruction");
        // Get the element
        Value *IndexOne = ConstantInt::get(llvmContext, APInt(32, 1));
        Instruction *GetElem = GetElementPtrInst::CreateInBounds(
            memRefValPtrElementTy, memRefValue, {IndexOne, IndexOne}, "",
            curBlock);
        memRefValue = GetElem;
      } break;
      // Primitive types that need not be reached into.
      case Type::IntegerTyID:
        break;
      default: {
        assert(false && "Encountered value with type whose cast to float is "
                        "not yet handled");
      } break;
      }
    }
  }
  // If it is an effective address value, convert it to a pointer to
  // the type of load reg.
  if (isEffectiveAddrValue(memRefValue)) {
    assert(false &&
           "*** Unhandled situation. Need to implement support correctly");
    Type *PtrTy = memRefValue->getType();
    IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefValue, PtrTy);
    curBlock->getInstList().push_back(convIntToPtr);
    memRefValue = convIntToPtr;
  }
  assert(memRefValue->getType()->isPointerTy() &&
         "Pointer type expected in load instruction");
  // Load the value from memory location
  LoadInst *loadInst = new LoadInst(memRefValue);
  unsigned int memAlignment = memRefValue->getType()
                                  ->getPointerElementType()
                                  ->getPrimitiveSizeInBits() /
                              8;
  loadInst->setAlignment(memAlignment);
  curBlock->getInstList().push_back(loadInst);

  switch (opcode) {
  default: {
    assert(false && "Unhandled load floating-point register instruction");
  } break;
  case X86::ILD_F32m:
  case X86::ILD_F64m: {
    Type *floatTy = Type::getFloatTy(llvmContext);
    assert(loadInst->getType()->isIntegerTy() &&
           "Unexpected non-integter type of source in fild instruction");
    // Cast source to float
    Instruction *CInst =
        CastInst::Create(CastInst::getCastOpcode(loadInst, true, floatTy, true),
                         loadInst, floatTy);
    curBlock->getInstList().push_back(CInst);
    // Push value to top of FPU register stack
    FPURegisterStackPush(CInst);
  } break;
  case X86::LD_F32m: {
    Type *floatTy = Type::getFloatTy(llvmContext);
    // Cast source to float
    Instruction *CInst =
        CastInst::Create(CastInst::getCastOpcode(loadInst, true, floatTy, true),
                         loadInst, floatTy);
    curBlock->getInstList().push_back(CInst);
    // Push value to top of FPU register stack
    FPURegisterStackPush(CInst);
  }
  }
  return true;
}

bool X86MachineInstructionRaiser::raiseStoreIntToFloatRegInstr(
    const MachineInstr &mi, BasicBlock *curBlock, Value *memRefValue) {

  const unsigned int opcode = mi.getOpcode();
  const MCInstrDesc &MIDesc = mi.getDesc();
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  assert(memoryRefOpIndex == 0 &&
         "Expect memory operand of floating-point load instruction at index 0");
  assert(MIDesc.getNumDefs() == 0 &&
         "Expect no defs in floating-point load instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);
  uint64_t BaseSupReg = find64BitSuperReg(memRef.Base.Reg);
  bool isPCRelMemRef = (BaseSupReg == X86::RIP);

  // Load the value from memory location of memRefValue.
  // memRefVal is either an AllocaInst (stack access) or GlobalValue (global
  // data access) or an effective address value.
  assert((isa<AllocaInst>(memRefValue) || isEffectiveAddrValue(memRefValue) ||
          isa<GlobalValue>(memRefValue)) &&
         "Unexpected type of memory reference in FPU store op instruction");

  LLVMContext &llvmContext(MF.getFunction().getContext());
  if (isPCRelMemRef) {
    // If it is a PC-relative mem ref, memRefValue is a global value loaded
    // from PC-relative memory location. If it is a derived type value, get
    // its element pointer.
    Type *memRefValueTy = memRefValue->getType();
    if (!memRefValueTy->isFloatingPointTy()) {
      assert(memRefValueTy->isPointerTy() &&
             "Unhandled non-pointer type found while attempting to load value "
             "from FPU register stack.");
      Type *memRefValPtrElementTy = memRefValueTy->getPointerElementType();
      switch (memRefValPtrElementTy->getTypeID()) {
      case Type::ArrayTyID: {
        assert(memRefValPtrElementTy->getArrayNumElements() == 1 &&
               "Unexpected number of array elements in value being cast to "
               "float");
        // Make sure the array element type is integer or floating point type.
        Type *arrElemTy = memRefValPtrElementTy->getArrayElementType();
        assert((arrElemTy->isIntegerTy() || arrElemTy->isFloatingPointTy()) &&
               "Unexpected type of data referenced in FPU register stack "
               "store instruction");
        // Get the element
        Value *IndexOne = ConstantInt::get(llvmContext, APInt(32, 1));
        Instruction *GetElem = GetElementPtrInst::CreateInBounds(
            memRefValPtrElementTy, memRefValue, {IndexOne, IndexOne}, "",
            curBlock);
        memRefValue = GetElem;
      } break;
      // Primitive types that need not be reached into.
      case Type::IntegerTyID:
        break;
      default: {
        assert(false && "Encountered value with type whose cast to float is "
                        "not yet handled");
      } break;
      }
    }
  }
  // If it is an effective address value, convert it to a pointer to
  // the type of load reg.
  if (isEffectiveAddrValue(memRefValue)) {
    assert(false &&
           "*** Unhandled situation. Need to implement support correctly");
    Type *PtrTy = memRefValue->getType();
    IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefValue, PtrTy);
    curBlock->getInstList().push_back(convIntToPtr);
    memRefValue = convIntToPtr;
  }
  assert(memRefValue->getType()->isPointerTy() &&
         "Pointer type expected in store instruction");

  switch (opcode) {
  default: {
    assert(false && "Unhandled store floating-point register instruction");
  } break;
  case X86::ST_FP32m:
  case X86::ST_FP64m: {
    Value *ST0Val = FPURegisterStackTop();
    Type *SrcTy = ST0Val->getType();
    // The value in ST0 is converted to single-precision or double precision
    // floating-point format. So, cast the memRefValue to the PointerType of
    // SrcTy.
    Type *DestElemTy = memRefValue->getType()->getPointerElementType();
    if (DestElemTy != SrcTy) {
      PointerType *SrcPtrTy = SrcTy->getPointerTo(0);
      Instruction *CInst = CastInst::Create(
          CastInst::getCastOpcode(memRefValue, true, SrcPtrTy, true),
          memRefValue, SrcPtrTy);
      curBlock->getInstList().push_back(CInst);
      memRefValue = CInst;
    }
    // Create the store
    StoreInst *StInst = new StoreInst(ST0Val, memRefValue);
    curBlock->getInstList().push_back(StInst);

    // Pop value to top of FPU register stack
    FPURegisterStackPop();
  }
  }
  return true;
}

bool X86MachineInstructionRaiser::raiseMoveFromMemInstr(const MachineInstr &mi,
                                                        BasicBlock *curBlock,
                                                        Value *memRefValue) {
  const unsigned int opcode = mi.getOpcode();
  const MCInstrDesc &MIDesc = mi.getDesc();
  unsigned LoadOpIndex = 0;
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  assert(memoryRefOpIndex == 1 &&
         "Expect memory operand of a mem move instruction at index 1");
  assert(MIDesc.getNumDefs() == 1 && mi.getOperand(LoadOpIndex).isReg() &&
         "Expect store operand register target");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);
  uint64_t BaseSupReg = find64BitSuperReg(memRef.Base.Reg);
  bool isPCRelMemRef = (BaseSupReg == X86::RIP);
  const MachineOperand &LoadOp = mi.getOperand(LoadOpIndex);
  unsigned int LoadPReg = LoadOp.getReg();
  assert(TargetRegisterInfo::isPhysicalRegister(LoadPReg) &&
         "Expect destination to be a physical register in move from mem "
         "instruction");

  // Load the value from memory location of memRefValue.
  // memRefVal is either an AllocaInst (stack access) or GlobalValue (global
  // data access) or an effective address value.
  assert((isa<AllocaInst>(memRefValue) || isEffectiveAddrValue(memRefValue) ||
          isa<GlobalValue>(memRefValue)) &&
         "Unexpected type of memory reference in binary mem op instruction");

  if (isPCRelMemRef) {
    // memRefValue already represents the global value loaded from PC-relative
    // memory location. It is incorrect to generate an additional load of this
    // value. It should be directly used.
    updatePhysRegSSAValue(LoadPReg, memRefValue);
  } else {
    // If it is an effective address value, convert it to a pointer to the
    // type of load reg.
    if (isEffectiveAddrValue(memRefValue)) {
      PointerType *PtrTy =
          PointerType::get(getPhysRegOperandType(mi, LoadOpIndex), 0);
      IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefValue, PtrTy);
      curBlock->getInstList().push_back(convIntToPtr);
      memRefValue = convIntToPtr;
    }
    assert(memRefValue->getType()->isPointerTy() &&
           "Pointer type expected in load instruction");
    // Load the value from memory location
    LoadInst *loadInst = new LoadInst(memRefValue);
    unsigned int memAlignment = memRefValue->getType()
                                    ->getPointerElementType()
                                    ->getPrimitiveSizeInBits() /
                                8;
    loadInst->setAlignment(memAlignment);
    curBlock->getInstList().push_back(loadInst);

    LLVMContext &llvmContext(MF.getFunction().getContext());
    Type *memTy = nullptr;
    Type *extTy = nullptr;
    switch (opcode) {
    default: {
      updatePhysRegSSAValue(LoadPReg, loadInst);
    } break;
    case X86::MOVSX64rm32: {
      extTy = Type::getInt64Ty(llvmContext);
      memTy = Type::getInt32Ty(llvmContext);
    } break;
    case X86::MOVZX64rm16: {
    case X86::MOVSX64rm16:
      extTy = Type::getInt64Ty(llvmContext);
      memTy = Type::getInt16Ty(llvmContext);
    } break;
    case X86::MOVZX64rm8:
    case X86::MOVSX64rm8: {
      extTy = Type::getInt64Ty(llvmContext);
      memTy = Type::getInt8Ty(llvmContext);
    } break;

    case X86::MOVZX32rm8:
    case X86::MOVZX32rm8_NOREX:
    case X86::MOVSX32rm8: {
      extTy = Type::getInt32Ty(llvmContext);
      memTy = Type::getInt8Ty(llvmContext);
    } break;
    case X86::MOVZX32rm16:
    case X86::MOVSX32rm16: {
      extTy = Type::getInt32Ty(llvmContext);
      memTy = Type::getInt16Ty(llvmContext);
    } break;

    case X86::MOVZX16rm8:
    case X86::MOVSX16rm8: {
      extTy = Type::getInt16Ty(llvmContext);
      memTy = Type::getInt8Ty(llvmContext);
    } break;
    case X86::MOVZX16rm16:
    case X86::MOVSX16rm16: {
      extTy = Type::getInt16Ty(llvmContext);
      memTy = Type::getInt16Ty(llvmContext);
    } break;
    }
    // Decide based on opcode value and not opcode name??
    bool isSextInst =
        x86InstrInfo->getName(MIDesc.getOpcode()).startswith("MOVSX");
    bool isZextInst =
        x86InstrInfo->getName(MIDesc.getOpcode()).startswith("MOVZX");

    if (isSextInst || isZextInst) {
      assert(((extTy != nullptr) && (memTy != nullptr)) &&
             "Unhandled move from memory instruction");

      // Load value of type memTy
      Instruction *CInst = loadInst;
      if (loadInst->getType() != memTy) {
        CInst = CastInst::Create(
            CastInst::getCastOpcode(loadInst, false, memTy, false), loadInst,
            memTy);
        curBlock->getInstList().push_back(CInst);
      }
      Instruction *extInst;

      // Now extend the value accordingly
      if (isSextInst) {
        // Sign extend
        extInst = new SExtInst(CInst, extTy);
      } else {
        // Zero extend
        extInst = new ZExtInst(CInst, extTy);
      }
      curBlock->getInstList().push_back(extInst);
      // Update PhysReg to Value map
      updatePhysRegSSAValue(LoadPReg, extInst);
    } else {
      // This is a normal mov instruction
      // Update PhysReg to Value map
      updatePhysRegSSAValue(LoadPReg, loadInst);
    }
  }

  return true;
}

bool X86MachineInstructionRaiser::raiseMoveToMemInstr(const MachineInstr &mi,
                                                      BasicBlock *curBlock,
                                                      Value *memRefVal) {
  unsigned int SrcOpIndex = getMemoryRefOpIndex(mi) + X86::AddrNumOperands;

  const MachineOperand &SrcOp = mi.getOperand(SrcOpIndex);

  assert((SrcOp.isImm() || SrcOp.isReg()) &&
         "Register or immediate value source expected in a move to mem "
         "instruction");

  unsigned int memAlignment = getInstructionMemOpSize(mi.getOpcode());
  Value *SrcValue = nullptr;
  Type *SrcOpTy = nullptr;

  // If Source op is immediate, create a constant int value
  // of type memory location.
  if (SrcOp.isImm()) {
    SrcOpTy = getImmOperandType(mi, SrcOpIndex);
    SrcValue = ConstantInt::get(SrcOpTy, SrcOp.getImm());
  } else {
    // If it is not an immediate value, get source value
    unsigned int PReg = SrcOp.getReg();
    assert(
        TargetRegisterInfo::isPhysicalRegister(PReg) &&
        "Expect source to be a physical register in move to mem instruction");
    SrcValue = getRegValue(PReg);
    SrcOpTy = getPhysRegOperandType(mi, SrcOpIndex);
  }
  assert(SrcValue != nullptr &&
         "Unable to get source value while raising move to mem instruction");
  // Load the value from memory location of memRefValue.
  // memRefVal is either an AllocaInst (stack access) or GlobalValue (global
  // data access) or an effective address value.
  assert((isa<AllocaInst>(memRefVal) || isEffectiveAddrValue(memRefVal) ||
          isa<GlobalValue>(memRefVal)) &&
         "Unexpected type of memory reference in mem-to-reg instruction");
  if (isEffectiveAddrValue(memRefVal)) {
    // If it is a load of global value use the global value
    // directly, else convert it to a pointer.
    if (isa<LoadInst>(memRefVal)) {
      LoadInst *ldInst = dyn_cast<LoadInst>(memRefVal);
      // If it is a load of global variable, use it directly.
      if (isa<GlobalValue>(ldInst->getPointerOperand())) {
        memRefVal = dyn_cast<GlobalValue>(ldInst->getPointerOperand());
      }
    }
  }

  // If memory reference is not a pointer type, cast it to a pointer
  Type *DstMemTy = memRefVal->getType();
  if (!DstMemTy->isPointerTy()) {
    // Cast it as pointer to SrcOpTy
    PointerType *PtrTy = PointerType::get(SrcOpTy, 0);
    IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefVal, PtrTy);
    curBlock->getInstList().push_back(convIntToPtr);
    memRefVal = convIntToPtr;
  }

  // This instruction moves a source value to memory. So, if the types of the
  // source value and that of the memory pointer content are not the same, it
  // is the source value that needs to be cast to match the type of
  // destination (i.e., memory). It needs to be sign extended as needed.
  Type *MatchTy = memRefVal->getType()->getPointerElementType();
  if (SrcValue->getType() != MatchTy) {
    Type *CastTy = MatchTy;
    CastInst *CInst = CastInst::Create(
        CastInst::getCastOpcode(SrcValue, false, CastTy, false), SrcValue,
        CastTy);
    curBlock->getInstList().push_back(CInst);
    SrcValue = CInst;
  }

  StoreInst *storeInst = new StoreInst(SrcValue, memRefVal);

  storeInst->setAlignment(memAlignment);
  curBlock->getInstList().push_back(storeInst);
  return true;
}

// Raise idiv instruction.
// TODO: Need to add support for div.
bool X86MachineInstructionRaiser::raiseDivideInstr(const MachineInstr &mi,
                                                   BasicBlock *curBlock,
                                                   Value *memRefValue) {
  const MCInstrDesc &MIDesc = mi.getDesc();
  unsigned int opcode = mi.getOpcode();
  LLVMContext &llvmContext(MF.getFunction().getContext());

  // idiv uses AX(AH:AL or DX:AX or EDX:EAX or RDX:RAX pairs as dividend and
  // stores the result in the same pair. Additionally, EFLAGS is an implicit
  // def.
  assert(MIDesc.getNumImplicitUses() == 2 && MIDesc.getNumImplicitDefs() == 3 &&
         MIDesc.hasImplicitDefOfPhysReg(X86::EFLAGS) &&
         "Unexpected number of implicit uses and defs in div instruction");
  MCPhysReg UseDefReg_0 = MIDesc.ImplicitUses[0];
  MCPhysReg UseDefReg_1 = MIDesc.ImplicitUses[1];
  assert((UseDefReg_0 == MIDesc.ImplicitDefs[0]) &&
         (UseDefReg_1 == MIDesc.ImplicitDefs[1]) &&
         "Unexpected use/def registers in div instruction");

  Value *DividendLowBytes = getRegValue(UseDefReg_0);
  Value *DividendHighBytes = getRegValue(UseDefReg_1);
  assert((DividendLowBytes != nullptr) && (DividendHighBytes != nullptr) &&
         "Unexpected use before definition in div instruction");
  // Divisor is memRefValue.
  // Create a Value representing the dividend.
  // TODO: Not sure how the implicit use registers of IDIV8m are encode. Does
  // the instruction have AX as a single use/def register or does it have 2
  // use/def registers, viz., AH:AL pair similar to the other IDIV
  // instructions? Handle it when it is encountered.
  assert((opcode != X86::IDIV8m) && "*** Need to handle IDIV8m");
  assert((DividendLowBytes->getType() == DividendHighBytes->getType()) &&
         "Unexpected types of dividend registers in idiv instruction");
  unsigned int UseDefRegSize =
      DividendLowBytes->getType()->getScalarSizeInBits();
  // Generate the following code
  // %h = lshl DividendHighBytes, UseDefRegSize
  // %f = or %h, DividendLowBytes
  // %quo = idiv %f, memRefValue
  // %rem = irem %f, memRefValue
  // UseDef_0 = %quo
  // UseDef_1 = %rem

  // Logical Shift left DividendHighBytes by n-bits (where n is the size of
  // UseDefRegSize) to get the high bytes and set DefReg_1 to the resulting
  // value.
  // DoubleTy type is of type twice the use reg size
  Type *DoubleTy = Type::getIntNTy(llvmContext, UseDefRegSize * 2);
  Value *ShiftAmountVal =
      ConstantInt::get(DoubleTy, UseDefRegSize, false /* isSigned */);
  // Cast DividendHighBytes and DividendLowBytes to types with double the
  // size.
  CastInst *DividendLowBytesDT = CastInst::Create(
      CastInst::getCastOpcode(DividendLowBytes, true, DoubleTy, true),
      DividendLowBytes, DoubleTy);
  curBlock->getInstList().push_back(DividendLowBytesDT);

  CastInst *DividendHighBytesDT = CastInst::Create(
      CastInst::getCastOpcode(DividendHighBytes, true, DoubleTy, true),
      DividendHighBytes, DoubleTy);
  curBlock->getInstList().push_back(DividendHighBytesDT);

  Instruction *LShlInst =
      BinaryOperator::CreateNUWShl(DividendHighBytesDT, ShiftAmountVal);
  curBlock->getInstList().push_back(LShlInst);

  // Combine the dividend values to get full dividend.
  // or instruction
  Instruction *FullDividend =
      BinaryOperator::CreateOr(LShlInst, DividendLowBytesDT);
  curBlock->getInstList().push_back(FullDividend);

  // Cast divisor (memRefValue) to double type
  CastInst *memRefValueDT = CastInst::Create(
      CastInst::getCastOpcode(memRefValue, true, DoubleTy, true), memRefValue,
      DoubleTy);
  curBlock->getInstList().push_back(memRefValueDT);

  // quotient
  Instruction *QuotientDT =
      BinaryOperator::CreateSDiv(FullDividend, memRefValueDT);
  curBlock->getInstList().push_back(QuotientDT);

  // Cast Quotient back to UseDef reg value type
  CastInst *Quotient =
      CastInst::Create(CastInst::getCastOpcode(
                           QuotientDT, true, DividendLowBytes->getType(), true),
                       QuotientDT, DividendLowBytes->getType());

  curBlock->getInstList().push_back(Quotient);
  // Update ssa val of UseDefReg_0
  updatePhysRegSSAValue(UseDefReg_0, Quotient);

  // remainder
  Instruction *RemainderDT =
      BinaryOperator::CreateSRem(FullDividend, memRefValueDT);
  curBlock->getInstList().push_back(RemainderDT);

  // Cast RemainderDT back to UseDef reg value type
  CastInst *Remainder = CastInst::Create(
      CastInst::getCastOpcode(RemainderDT, true, DividendHighBytes->getType(),
                              true),
      RemainderDT, DividendHighBytes->getType());

  curBlock->getInstList().push_back(Remainder);
  // Update ssa val of UseDefReg_1
  updatePhysRegSSAValue(UseDefReg_1, Remainder);

  return true;
}
// Raise compare instruction. If the the instruction is a memory compare, it
// is expected that this function is called from raiseMemRefMachineInstr after
// verifying the accessibility of memory location and with isMemCompare set
// true.If isMemCompare is true, memRefValue needs to be the non-null memory
// reference value representing the memory reference the instruction uses.

bool X86MachineInstructionRaiser::raiseCompareMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock, bool isMemCompare,
    Value *memRefValue) {
  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  assert((((memoryRefOpIndex != -1) && isMemCompare) ||
          ((memoryRefOpIndex == -1) && !isMemCompare)) &&
         "Inconsistent memory reference operand information specified for "
         "compare instruction");
  MCInstrDesc MCIDesc = mi.getDesc();
  // Is this a cmp instruction?
  bool isCMPInst = x86InstrInfo->getName(MCIDesc.getOpcode()).startswith("CMP");

  SmallVector<Value *, 2> OpValues = {nullptr, nullptr};

  // Get operand indices
  if (isMemCompare) {
    // This is a memory referencing instruction.
    Type *NonMemRefOpTy;
    const MachineOperand *NonMemRefOp;
    assert(memoryRefOpIndex >= 0 &&
           "Unexpected memory operand index in compare instruction");
    unsigned nonMemRefOpIndex =
        (memoryRefOpIndex == 0) ? X86::AddrNumOperands : 0;
    NonMemRefOp = &(mi.getOperand(nonMemRefOpIndex));
    if (NonMemRefOp->isReg()) {
      NonMemRefOpTy = getPhysRegOperandType(mi, nonMemRefOpIndex);
    } else if (NonMemRefOp->isImm()) {
      NonMemRefOpTy = getImmOperandType(mi, nonMemRefOpIndex);
    } else {
      mi.dump();
      assert(false && "Unhandled second operand type in compare instruction");
    }

    assert(memRefValue != nullptr && "Null memory reference value encountered "
                                     "while raising compare instruction");
    // Convert it to a pointer of type of non-memory operand
    if (isEffectiveAddrValue(memRefValue)) {
      PointerType *PtrTy = PointerType::get(NonMemRefOpTy, 0);
      IntToPtrInst *convIntToPtr = new IntToPtrInst(memRefValue, PtrTy);
      curBlock->getInstList().push_back(convIntToPtr);
      memRefValue = convIntToPtr;
    }
    // Load the value from memory location
    LoadInst *loadInst = new LoadInst(memRefValue);
    loadInst->setAlignment(
        memRefValue->getPointerAlignment(MR->getModule().getDataLayout()));
    curBlock->getInstList().push_back(loadInst);
    // save it at the appropriate index of operand value array
    if (memoryRefOpIndex == 0) {
      OpValues[0] = loadInst;
    } else {
      OpValues[1] = loadInst;
    }

    // Get value for non-memory operand of compare.
    Value *NonMemRefVal = nullptr;
    if (NonMemRefOp->isReg()) {
      NonMemRefVal = getRegValue(NonMemRefOp->getReg());
    } else if (NonMemRefOp->isImm()) {
      NonMemRefVal =
          ConstantInt::get(memRefValue->getType()->getPointerElementType(),
                           NonMemRefOp->getImm());
    } else {
      mi.dump();
      assert(false && "Unhandled first operand type in compare instruction");
    }
    // save non-memory reference value at the appropriate index of operand
    // value array
    if (memoryRefOpIndex == 0) {
      OpValues[1] = NonMemRefVal;
    } else {
      OpValues[0] = NonMemRefVal;
    }
  } else {
    // The instruction operands do not reference memory
    unsigned Op1Index = MCIDesc.getNumDefs() == 0 ? 0 : 1;

    MachineOperand CmpOp1 = mi.getOperand(Op1Index);
    MachineOperand CmpOp2 = mi.getOperand(Op1Index + 1);

    assert((CmpOp1.isReg() || CmpOp1.isImm()) &&
           "Unhandled first operand type in compare instruction");

    assert((CmpOp2.isReg() || CmpOp2.isImm()) &&
           "Unhandled second operand type in compare instruction");

    if (CmpOp1.isReg()) {
      OpValues[0] = getRegValue(CmpOp1.getReg());
    }

    if (CmpOp2.isReg()) {
      OpValues[1] = getRegValue(CmpOp2.getReg());
    }

    // Construct value if either of the operands is an immediate
    if (CmpOp1.isImm()) {
      assert((OpValues[1] != nullptr) &&
             "At least one value expected while raising compare instruction");
      OpValues[0] = ConstantInt::get(OpValues[1]->getType(), CmpOp1.getImm());
    }

    if (CmpOp2.isImm()) {
      assert((OpValues[0] != nullptr) &&
             "At least one value expected while raising compare instruction");
      OpValues[1] = ConstantInt::get(OpValues[0]->getType(), CmpOp2.getImm());
    }
  }
  assert(OpValues[0] != nullptr && OpValues[1] != nullptr &&
         "Unable to materialize compare operand values");

  Instruction *CmpInst = nullptr;
  // Sub instruction is marked as a compare instruction (MCID::Compare)
  switch (mi.getOpcode()) {
  case X86::SUB8mi:
  case X86::SUB8mr:
  case X86::SUB8rm:
  case X86::SUB16mi:
  case X86::SUB16mr:
  case X86::SUB16rm:
  case X86::SUB32mi:
  case X86::SUB32mr:
  case X86::SUB32rm:
  case X86::SUB64mi8:
  case X86::SUB64mi32:
  case X86::SUB64mr:
  case X86::SUB64rm:
  case X86::SUB32rr: {
    assert(MCIDesc.getNumDefs() == 1 &&
           "Unexpected number of def operands of sub memref instruction");
    const MachineOperand &MO = mi.getOperand(0);
    assert(mi.getOperand(0).isReg() && "Unexpected non-register def operand");
    // Make sure the source operand value types are the same as destination
    // register type.
    Type *DestTy = getPhysRegOperandType(mi, 0);
    for (int i = 0; i < 2; i++) {
      if (OpValues[i]->getType() != DestTy) {
        CastInst *CInst = CastInst::Create(
            CastInst::getCastOpcode(OpValues[i], false, DestTy, false),
            OpValues[i], DestTy);
        curBlock->getInstList().push_back(CInst);
        OpValues[i] = CInst;
      }
    }

    CmpInst = BinaryOperator::CreateSub(OpValues[0], OpValues[1]);
    updatePhysRegSSAValue(MO.getReg(), CmpInst);
  } break;
  default: {
    assert(isCMPInst &&
           "Expect compare instruction. Possibly an unhandled compare "
           "instruction?");
    if (OpValues[0]->getType()->isIntegerTy() &&
        OpValues[1]->getType()->isIntegerTy()) {
      // The predicate value used ICMP_EQ is temporary. This will be fixed
      // based on the condition of the branch using the effects of this
      // comparison.
      CmpInst =
          new ICmpInst(CmpInst::Predicate::ICMP_EQ, OpValues[0], OpValues[1]);
    } else if (OpValues[0]->getType()->isFloatTy() &&
               OpValues[1]->getType()->isFloatTy()) {
      // The predicate value used FCMP_OEQ is temporary. This will be fixed
      // based on the condition of the branch using the effects of this
      // comparison.
      CmpInst =
          new FCmpInst(CmpInst::Predicate::FCMP_OEQ, OpValues[0], OpValues[1]);
    } else {
      assert(false && "Incompatible types of comparison operands found");
    }
    assert(MCIDesc.getNumImplicitDefs() == 1 &&
           "Compare instruction does not have exactly one implicit def");
    MCPhysReg ImpDefReg = MCIDesc.ImplicitDefs[0];
    assert(ImpDefReg == X86::EFLAGS &&
           "Expected implicit EFLAGS def in compare instruction");
    updatePhysRegSSAValue(ImpDefReg, CmpInst);
  }
  }
  // Add the compare instruction
  curBlock->getInstList().push_back(CmpInst);
  return true;
}

// Raise a load/store instruction.
// Current implementation only raises instructions that load and store to
// stack.
bool X86MachineInstructionRaiser::raiseMemRefMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {

  // Handle the push instruction that is marked as a memory store instruction
  if (isPushToStack(mi)) {
    return raisePushInstruction(mi);
  }

  if (isPopFromStack(mi)) {
    return raisePopInstruction(mi);
  }

  const MCInstrDesc &MIDesc = mi.getDesc();
  unsigned int opcode = mi.getOpcode();

  int loadOrStoreOpIndex = -1;

  // Get index of memory reference in the instruction.
  int memoryRefOpIndex = getMemoryRefOpIndex(mi);
  // Should have found the index of the memory reference operand
  assert(memoryRefOpIndex != -1 &&
         "Unable to find memory reference operand of a load/store instruction");
  X86AddressMode memRef = llvm::getAddressFromInstr(&mi, memoryRefOpIndex);

  // Get the operand whose value is stored to memory or that is loaded from
  // memory.

  if (MIDesc.mayStore()) {
    // If the instruction stores to stack, find the register whose value is
    // being stored. It would be the operand at offset memRefOperandStartIndex
    // + X86::AddrNumOperands
    loadOrStoreOpIndex = memoryRefOpIndex + X86::AddrNumOperands;
  } else if (MIDesc.mayLoad()) {
    // If the instruction loads to memory to a register, it has 1 def.
    // Operand 0 is the loadOrStoreOp.
    assert(((MIDesc.getNumDefs() == 0) || (MIDesc.getNumDefs() == 1)) &&
           "Instruction that loads from memory expected to have only "
           "one target");
    if (MIDesc.getNumDefs() == 1) {
      loadOrStoreOpIndex = 0;
      assert(mi.getOperand(loadOrStoreOpIndex).isReg() &&
             "Target of instruction that loads from "
             "memory expected to be a register");
    } else if (!MIDesc.isCompare()) {
      switch (getInstructionKind(opcode)) {
      case InstructionKind::DIVIDE_OP:
      case InstructionKind::LOAD_FPU_REG:
        break;
      default:
        mi.print(errs());
        assert(false && "Encountered unhandled memory load instruction");
      }
    }
  } else {
    mi.print(errs());
    assert(false && "Encountered unhandled instruction that is not load/store");
  }

  Value *memoryRefValue = nullptr;

  if (memRef.BaseType == X86AddressMode::RegBase) {
    // If it is a stack reference, allocate a stack slot in case the current
    // memory reference is new. Else get the stack reference using the
    // stackslot index of the previously known stack ref.

    uint64_t BaseSupReg = find64BitSuperReg(memRef.Base.Reg);
    if (BaseSupReg == x86RegisterInfo->getStackRegister() ||
        BaseSupReg == x86RegisterInfo->getFramePtr()) {
      memoryRefValue = getStackAllocatedValue(mi, curBlock, loadOrStoreOpIndex);
    }
    // Handle PC-relative addressing.

    // NOTE: This tool now raises only shared libraries and executables - NOT
    // object files. So, instructions with 0 register (which typically are
    // seen in a relocatable object file for the linker to patch) are not
    // expected to be encountered.
    else if (BaseSupReg == X86::RIP) {
      memoryRefValue = createPCRelativeAccesssValue(mi, curBlock);
    } else {
      // Get load/store operand
      Value *memrefValue = getMemoryAddressExprValue(mi, curBlock);
      memoryRefValue = memrefValue;
    }
  } else {
    // TODO : Memory references with BaseType FrameIndexBase
    // (i.e., not RegBase type)
    outs() << "****** Unhandled memory reference in instruction\n\t";
    mi.dump();
    outs() << "****** reference of type FrameIndexBase";
    return false;
  }

  assert(memoryRefValue != nullptr &&
         "Unable to construct memory referencing value");

  // Raise a memory compare instruction
  if (mi.isCompare()) {
    return raiseCompareMachineInstr(mi, curBlock, true /* isMemRef */,
                                    memoryRefValue);
  }

  // Now that we have all necessary information about memory reference and the
  // load/store operand, we can raise the memory referencing instruction
  // according to the opcode.
  bool success = false;
  switch (getInstructionKind(opcode)) {
    // Move register or immediate to memory
  case InstructionKind::MOV_TO_MEM: {
    success = raiseMoveToMemInstr(mi, curBlock, memoryRefValue);
  } break;
    // Move register from memory
  case InstructionKind::MOV_FROM_MEM: {
    success = raiseMoveFromMemInstr(mi, curBlock, memoryRefValue);
  } break;
  case InstructionKind::BINARY_OP_RM: {
    success = raiseBinaryOpMemToRegInstr(mi, curBlock, memoryRefValue);
  } break;
  case InstructionKind::DIVIDE_OP: {
    success = raiseDivideInstr(mi, curBlock, memoryRefValue);
  } break;
  case InstructionKind::LOAD_FPU_REG:
    success = raiseLoadIntToFloatRegInstr(mi, curBlock, memoryRefValue);
    break;
  case InstructionKind::STORE_FPU_REG:
    success = raiseStoreIntToFloatRegInstr(mi, curBlock, memoryRefValue);
    break;
  default:
    outs() << "Unhandled memory referencing instruction.\n";
    mi.dump();
  }
  return success;
}

bool X86MachineInstructionRaiser::raiseSetCCMachineInstr(const MachineInstr &mi,
                                                         BasicBlock *curBlock) {
  const MCInstrDesc &MIDesc = mi.getDesc();
  LLVMContext &llvmContext(MF.getFunction().getContext());
  bool success = false;

  assert(MIDesc.getNumDefs() == 1 &&
         "Not found expected one destination operand of set instruction");
  assert(MIDesc.getNumImplicitUses() == 1 &&
         MIDesc.hasImplicitUseOfPhysReg(X86::EFLAGS) &&
         "Not found expected implicit use of eflags in set instruction.");

  const MachineOperand &DestOp = mi.getOperand(0);
  CmpInst::Predicate pred = CmpInst::Predicate::BAD_ICMP_PREDICATE;
  uint64_t EflagsCond = EFLAGS_UNDEFINED;

  switch (mi.getOpcode()) {
  case X86::SETNEm:
  case X86::SETNEr:
    pred = CmpInst::Predicate::ICMP_NE;
    EflagsCond = EFLAGS_ZF;
    break;
  case X86::SETEm:
  case X86::SETEr:
    pred = CmpInst::Predicate::ICMP_EQ;
    EflagsCond = EFLAGS_ZF;
    break;
  default:
    break;
  }

  assert(EflagsCond != EFLAGS_UNDEFINED && "Undefined EFLAGS");

  if (pred == CmpInst::Predicate::BAD_ICMP_PREDICATE) {
    mi.dump();
    assert(false && "Unhandled set instruction");
  }

  if (DestOp.isReg()) {
    // TODO : Using the eflags value seems very coarse. May be I should model
    // the constituent flags as seperate values ???
    Value *EflagsVal = getRegValue(X86::EFLAGS);
    Value *OneConstVal =
        ConstantInt::get(Type::getInt1Ty(llvmContext), 1, false /* isSigned */);
    CmpInst *cmp = new ICmpInst(pred, EflagsVal, OneConstVal);
    curBlock->getInstList().push_back(cmp);
    updatePhysRegSSAValue(DestOp.getReg(), cmp);
    success = true;
  } else {
    outs() << "Unhandled set instruction with memory destination\n";
    success = false;
  }
  return success;
}
// Raise a binary operation instruction with operand encoding I or RI
bool X86MachineInstructionRaiser::raiseBinaryOpImmToRegMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  unsigned int DstIndex = 0, SrcOp1Index = 1, SrcOp2Index = 2;
  const MCInstrDesc &MIDesc = mi.getDesc();
  // A binary operation instruction with encoding I specifies one operand -
  // using AL/AX/EAX/RAX as implicit register operand.
  // A binary operation instruction with encoding RI specifies two operands -
  // the first operand is a register and the second the immediate value
  //
  // The first operand is also as the destination operand.
  // X86::EFLAGS is the implicit def operand.
  unsigned NumOperands = mi.getNumExplicitOperands() +
                         MIDesc.getNumImplicitUses() +
                         MIDesc.getNumImplicitDefs();

  if (NumOperands == 4) {
    // No need to raise an instruction that adjusts stack pointer.
    if ((MIDesc.getNumDefs() == 1) &&
        (find64BitSuperReg(mi.getOperand(DstIndex).getReg()) == X86::RSP) &&
        (find64BitSuperReg(mi.getOperand(SrcOp1Index).getReg()) == X86::RSP) &&
        mi.getOperand(SrcOp2Index).isImm() &&
        MIDesc.hasImplicitDefOfPhysReg(X86::EFLAGS)) {
      return true;
    } else {
      Value *SrcOp1Value = nullptr;
      Value *SrcOp2Value = nullptr;
      unsigned int DstPReg = X86::NoRegister;

      assert(MIDesc.hasImplicitDefOfPhysReg(X86::EFLAGS) &&
             "Expected implicit def operand EFLAGS not found");

      if (MIDesc.getNumDefs() == 1) {
        const MachineOperand &DstOp = mi.getOperand(DstIndex);
        const MachineOperand &SrcOp1 = mi.getOperand(SrcOp1Index);
        const MachineOperand &SrcOp2 = mi.getOperand(SrcOp2Index);
        assert(DstOp.isReg() && "Not found expected register to be the "
                                "destination operand of BinOp instruction with "
                                "RI/I operand format");
        assert(SrcOp1.isReg() &&
               "Not found expected register to be the first "
               "operand of BinOp instruction with RI/I operand format");

        // Get value of SrcOp1
        unsigned int SrcOp1PReg = SrcOp1.getReg();
        SrcOp1Value = getRegValue(SrcOp1PReg);

        // Get value of SrcOp2
        assert(SrcOp2.isImm() && "Expect immediate operand in a BinOp "
                                 "instruction with RI/I operand format");
        // Create constant of type that matches that of the dest operand
        Type *Ty = getPhysRegOperandType(mi, DstIndex);
        SrcOp2Value = ConstantInt::get(Ty, SrcOp2.getImm());
        assert(SrcOp1Value != nullptr && SrcOp2Value != nullptr &&
               "Undefined source values encountered in BinOp instruction with "
               "RI/I operand format");

        // Get destination reg
        DstPReg = DstOp.getReg();

        // Generate any necessary trunc or sext instrs to match the sizes
        // of source and dest operands, as needed.
        SrcOp1Value = matchSSAValueToSrcRegSize(mi, SrcOp1Index, curBlock);
      } else if (MIDesc.getNumDefs() == 0) {
        SrcOp1Index = 0;
        // Uses implicit register AL/AX/EAX/RAX as source and dest
        assert(MIDesc.getNumImplicitUses() == 1 &&
               "Expected one implicit use operand of BinOp instruction with "
               "RI/I operand format");
        assert(MIDesc.getNumImplicitDefs() == 2 &&
               "Expected one implicit use operand of BinOp instruction with "
               "RI/I operand format");

        // Get the first (and only) operand
        const MachineOperand &SrcOp = mi.getOperand(SrcOp1Index);

        // Get dest reg
        DstPReg = MIDesc.ImplicitDefs[0];

        assert(((DstPReg == X86::AL) || (DstPReg == X86::AX) ||
                (DstPReg == X86::EAX) || (DstPReg == X86::RAX)) &&
               "Expected implicit use of operand AL/AX/EAX/RAX not found");

        assert(MIDesc.hasImplicitUseOfPhysReg(DstPReg) &&
               "Expected implicit use of operand AL/AX/EAX/RAX not found");

        // Get value of SrcOp1
        SrcOp1Value = getRegValue(DstPReg);

        // Get value of SrcOp2
        assert(SrcOp.isImm() && "Expect immediate operand in a BinOp "
                                "instruction with RI/I operand format");
        // Create constant of type that matches that of the dest operand
        Type *Ty = getImmOperandType(mi, SrcOp1Index);
        SrcOp2Value = ConstantInt::get(Ty, SrcOp.getImm());
      } else {
        mi.dump();
        assert(false && "Unhandled binary operation instruction with RI/I "
                        "operand format");
      }

      assert(DstPReg != X86::NoRegister &&
             "Failed to determine destination register of BinOp instruction "
             "with RI/I operand format");

      assert(SrcOp1Value != nullptr && SrcOp2Value != nullptr &&
             "Undefined source values encountered in BinOp instruction with "
             "RI/I operand format");

      Instruction *BinOpInstr = nullptr;
      switch (mi.getOpcode()) {
      case X86::ADD8i8:
      case X86::ADD32ri:
      case X86::ADD32ri8:
      case X86::ADD64ri8:
      case X86::ADD64ri32:
        // Generate add instruction
        BinOpInstr = BinaryOperator::CreateAdd(SrcOp1Value, SrcOp2Value);
        break;
      case X86::SUB32ri:
      case X86::SUB32ri8:
      case X86::SUB64ri8:
      case X86::SUB64ri32:
        // Generate sub instruction
        BinOpInstr = BinaryOperator::CreateSub(SrcOp1Value, SrcOp2Value);
        break;
      case X86::AND8i8:
      case X86::AND8ri:
      case X86::AND16ri:
      case X86::AND32ri:
      case X86::AND64ri8:
      case X86::AND64ri32:
        // Generate and instruction
        BinOpInstr = BinaryOperator::CreateAnd(SrcOp1Value, SrcOp2Value);
        break;
      case X86::XOR8ri:
        // Generate xor instruction
        BinOpInstr = BinaryOperator::CreateXor(SrcOp1Value, SrcOp2Value);
        break;
      case X86::IMUL32rri8:
      case X86::IMUL64rri32:
        BinOpInstr = BinaryOperator::CreateMul(SrcOp1Value, SrcOp2Value);
        break;
      default:
        assert(false && "Unhandled reg to imm binary operator instruction");
        break;
      }

      curBlock->getInstList().push_back(BinOpInstr);
      // Update PhysReg to Value map
      updatePhysRegSSAValue(DstPReg, BinOpInstr);
    }
  } else {
    mi.dump();
    assert(false && "Unhandled add imeediate instruction");
  }
  return true;
}

// Raise indirect branch instruction.
// TODO : NYI
bool X86MachineInstructionRaiser::raiseIndirectBranchMachineInstr(
    ControlTransferInfo *CTRec) {
  const MachineInstr *MI = CTRec->CandidateMachineInstr;
  // BasicBlock *CandBB = CTRec->CandidateBlock;

  const MCInstrDesc &MCID = MI->getDesc();

  // Make sure this function was called on a direct branch instruction.
  assert((MCID.TSFlags & X86II::ImmMask) == 0 &&
         "PC-Relative control transfer not expected");

  // TODO: Change this once implementation is complete
  return false;
}

// Raise direct branch instruction.
bool X86MachineInstructionRaiser::raiseDirectBranchMachineInstr(
    ControlTransferInfo *CTRec) {
  const MachineInstr *MI = CTRec->CandidateMachineInstr;
  BasicBlock *CandBB = CTRec->CandidateBlock;

  const MCInstrDesc &MCID = MI->getDesc();

  // Make sure this function was called on a direct branch instruction.
  assert(X86II::isImmPCRel(MCID.TSFlags) &&
         "PC-Relative control transfer expected");

  // Get branch offset of the branch instruction
  const MachineOperand &MO = MI->getOperand(0);
  assert(MO.isImm() && "Expected immediate operand not found");
  int64_t BranchOffset = MO.getImm();
  MCInstRaiser *MCIR = getMCInstRaiser();
  // Get MCInst offset - the offset of machine instruction in the binary
  uint64_t MCInstOffset = MCIR->getMCInstIndex(*MI);

  assert(MCIR != nullptr && "MCInstRaiser not initialized");
  int64_t BranchTargetOffset =
      MCInstOffset + MCIR->getMCInstSize(MCInstOffset) + BranchOffset;
  const uint64_t TgtMBBNo =
      MCIR->getMBBNumberOfMCInstOffset(BranchTargetOffset);
  auto iter = mbbToBBMap.find(TgtMBBNo);
  assert(iter != mbbToBBMap.end() &&
         "BasicBlock corresponding to MachineInstr branch not found");
  BasicBlock *TgtBB = (*iter).second;
  if (MI->isUnconditionalBranch()) {
    // Just create a branch instruction targeting TgtBB
    BranchInst *UncondBr = BranchInst::Create(TgtBB);
    CandBB->getInstList().push_back(UncondBr);
    CTRec->Raised = true;
  } else if (MI->isConditionalBranch()) {
    // Find the fall through basic block
    MCInstRaiser::const_mcinst_iter MCIter = MCIR->getMCInstAt(MCInstOffset);
    // Go to next instruction
    MCIter++;
    assert(MCIter != MCIR->const_mcinstr_end() &&
           "Attempt to go past MCInstr stream");
    // Get MBB number whose lead instruction is at the offset of next
    // instruction. This is the fall-through MBB.
    uint64_t FTMBBNum = MCIR->getMBBNumberOfMCInstOffset((*MCIter).first);
    // Find raised BasicBlock corresponding to fall-through MBB
    auto mapIter = mbbToBBMap.find(FTMBBNum);
    assert(mapIter != mbbToBBMap.end() &&
           "Fall-through BasicBlock corresponding to MachineInstr branch not "
           "found");
    BasicBlock *FTBB = (*mapIter).second;
    // Get the condition value
    assert(CTRec->RegValues.size() == 1 &&
           "Multiple implicit uses in conditional branch not handled");

    // If the Cond value is a compare, change the predicate of the compare
    // instruction based on condition of the branch.

    Value *Cond = CTRec->RegValues[0];
    Instruction *Inst = dyn_cast<Instruction>(Cond);
    if (isa<CmpInst>(Inst)) {
      if (ICmpInst *IntCmpInst = dyn_cast<ICmpInst>(Inst)) {
        // Detect the appropriate predicate
        switch (MI->getOpcode()) {
        case X86::JE_1:
        case X86::JE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_EQ);
          break;
        case X86::JNE_1:
        case X86::JNE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_NE);
          break;
        case X86::JA_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_UGT);
          break;
        case X86::JAE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_UGE);
          break;
        case X86::JB_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_ULT);
          break;
        case X86::JBE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_ULE);
          break;
        case X86::JG_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SGT);
          break;
        case X86::JGE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SGE);
          break;
        case X86::JL_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SLT);
          break;
        case X86::JLE_1:
        case X86::JLE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SLE);
          break;
        default:
          MI->dump();
          assert(false && "Unhandled conditional branch");
        }
      } else if (FCmpInst *FC = dyn_cast<FCmpInst>(Inst)) {
        assert(false && "Unhandled FCMP based branch raising");
      }
    }
    // If Cond is not a conditional instruction, construct one
    else {
      Value *CmpVal1 = Cond;
      Type *CmpType = Cond->getType();
      Value *CmpVal2 = ConstantInt::get(CmpType, 0);

      if (CmpType->isIntegerTy()) {
        CmpInst *IntCmpInst = new ICmpInst(
            CmpInst::Predicate::FIRST_ICMP_PREDICATE, CmpVal1, CmpVal2);
        CandBB->getInstList().push_back(IntCmpInst);
        // Set this value to be used as branch condition
        Cond = IntCmpInst;
        // Detect the appropriate predicate
        switch (MI->getOpcode()) {
        case X86::JE_1:
        case X86::JE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_EQ);
          break;
        case X86::JNE_1:
        case X86::JNE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_NE);
          break;
        case X86::JA_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_UGT);
          break;
        case X86::JAE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_UGE);
          break;
        case X86::JB_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_ULT);
          break;
        case X86::JBE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_ULE);
          break;
        case X86::JG_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SGT);
          break;
        case X86::JGE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SGE);
          break;
        case X86::JL_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SLT);
          break;
        case X86::JLE_4:
          IntCmpInst->setPredicate(CmpInst::Predicate::ICMP_SLE);
          break;
        default:
          MI->dump();
          assert(false && "Unhandled conditional branch");
        }
      } else if (CmpType->isFloatTy()) {
        assert(false &&
               "NYI - Generation of floating point compare instructions.");
      } else {
        assert(false && "Incompatible types of comparison operands found");
      }
    }
    // Set the predicate of the compare instruction according to the
    // branch condition

    // Create branch instruction
    BranchInst *CondBr = BranchInst::Create(TgtBB, FTBB, Cond);
    CandBB->getInstList().push_back(CondBr);
    CTRec->Raised = true;
  } else {
    assert(false && "Unhandled type of branch instruction");
  }
  return true;
}

// Raise a generic instruction. This is the catch all MachineInstr raiser
bool X86MachineInstructionRaiser::raiseGenericMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  unsigned int opcode = mi.getOpcode();
  bool success = false;

  // Now raise the instruction according to the opcode kind
  switch (getInstructionKind(opcode)) {
  case InstructionKind::BINARY_OP_WITH_IMM:
    success = raiseBinaryOpImmToRegMachineInstr(mi, curBlock);
    break;
  case InstructionKind::CONVERT_BWWDDQ:
    success = raiseConvertBWWDDQMachineInstr(mi, curBlock);
    break;
  case InstructionKind::CONVERT_WDDQQO:
    success = raiseConvertWDDQQOMachineInstr(mi, curBlock);
    break;
  case InstructionKind::LEA_OP:
    success = raiseLEAMachineInstr(mi, curBlock);
    break;
  case InstructionKind::MOV_RR:
    success = raiseMoveRegToRegMachineInstr(mi, curBlock);
    break;
  case InstructionKind::MOV_RI:
    success = raiseMoveImmToRegMachineInstr(mi, curBlock);
    break;
  case InstructionKind::BINARY_OP_RR:
    success = raiseBinaryOpRegToRegMachineInstr(mi, curBlock);
    break;
  case InstructionKind::SETCC:
    success = raiseSetCCMachineInstr(mi, curBlock);
    break;
  case InstructionKind::COMPARE:
    success = raiseCompareMachineInstr(mi, curBlock, false, nullptr);
    break;
  case InstructionKind::FPU_REG_OP:
    success = raiseFPURegisterOpInstr(mi, curBlock);
    break;
  default: {
    outs() << "*** Generic instruction not raised : ";
    mi.dump();
    success = false;
  }
  }
  return success;
}

// Raise a return instruction.
bool X86MachineInstructionRaiser::raiseReturnMachineInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {
  Type *retType = raisedFunction->getReturnType();
  Value *retValue = nullptr;

  if (!retType->isVoidTy()) {
    unsigned int retReg =
        (retType->getPrimitiveSizeInBits() == 64) ? X86::RAX : X86::EAX;
    retValue = findPhysRegSSAValue(retReg);
  }
  // Create return instruction
  Instruction *retInstr =
      ReturnInst::Create(MF.getFunction().getContext(), retValue);
  curBlock->getInstList().push_back(retInstr);

  return true;
}

bool X86MachineInstructionRaiser::raiseBranchMachineInstrs() {
  // Raise branch instructions with control transfer records
  bool success = true;
  for (ControlTransferInfo *CTRec : CTInfo) {
    if (CTRec->CandidateMachineInstr->isBranch()) {
      const MachineInstr *MI = CTRec->CandidateMachineInstr;
      const MCInstrDesc &MCID = MI->getDesc();
      uint64_t imm = MCID.TSFlags & X86II::ImmMask;

      if ((imm == X86II::Imm8PCRel) || (imm == X86II::Imm16PCRel) ||
          (imm == X86II::Imm32PCRel)) {
        success &= raiseDirectBranchMachineInstr(CTRec);
        assert(success && "Failed to raise direct branch instruction");
      } else {
        success &= raiseIndirectBranchMachineInstr(CTRec);
        assert(success && "Failed to raise indirect branch instruction");
      }
    }
  }

  // Delete all ControlTransferInfo records of branch instructions
  // that were raised.
  if (!CTInfo.empty()) {
    CTInfo.erase(
        std::remove_if(CTInfo.begin(), CTInfo.end(),
                       [](const ControlTransferInfo *r) { return r->Raised; }),
        CTInfo.end());
  }
  assert(CTInfo.empty() && "Unhandled branch instructions exist");

  // Note that for basic blocks that fall-through and have no terminator,
  // no control transfer record is created. Insert branch instructions
  // at the end of all such blocks.

  // Walk basic blocks of the MachineFunction.
  for (MachineFunction::iterator mfIter = MF.begin(), mfEnd = MF.end();
       mfIter != mfEnd; mfIter++) {
    MachineBasicBlock &MBB = *mfIter;
    // Get the number of MachineBasicBlock being looked at.
    // If MBB has no terminators, insert a branch to the fall through edge.
    if (MBB.getFirstTerminator() == MBB.end()) {
      assert(MBB.succ_size() == 1 && "Expect MachineBasicBlock with no "
                                     "terminators to have successors 1");
      // Find the BasicBlock corresponding to MBB
      auto iter = mbbToBBMap.find(MBB.getNumber());
      assert(iter != mbbToBBMap.end() &&
             "Unable to find BasicBlock to insert unconditional branch");
      BasicBlock *BB = iter->second;

      // Find the BasicBlock corresponding to the successor of MBB
      MachineBasicBlock *SuccMBB = *(MBB.succ_begin());
      iter = mbbToBBMap.find(SuccMBB->getNumber());
      assert(iter != mbbToBBMap.end() && "Unable to find successor BasicBlock");
      BasicBlock *SuccBB = iter->second;

      // Create a branch instruction targeting SuccBB
      BranchInst *UncondBr = BranchInst::Create(SuccBB);
      BB->getInstList().push_back(UncondBr);
    }
  }
  return true;
}

// Raise FPU instructions
bool X86MachineInstructionRaiser::raiseFPURegisterOpInstr(
    const MachineInstr &mi, BasicBlock *curBlock) {

  // Construct the appropriate instruction
  unsigned opcode = mi.getOpcode();
  switch (opcode) {
  case X86::MUL_FPrST0:
  case X86::DIV_FPrST0: {
    Value *st0Val = FPURegisterStackGetValueAt(0);
    assert((st0Val != nullptr) && "Failed to get ST(0) value");
    Type *st0ValTy = st0Val->getType();
    assert(st0ValTy->isFloatingPointTy() &&
           "Unexpected non-FP value on FPU register stack");
    assert((mi.getNumDefs() == 0) &&
           "Unexpected number of defs in FP register op instruction format");
    assert(
        (mi.getNumExplicitOperands() == 1) &&
        "Unexpected number of operands in FP register op instruction format");
    const MachineOperand &stRegOp = mi.getOperand(0);
    assert(stRegOp.isReg() &&
           "Unexpected non-register operand of FP register op instruction");
    int8_t FPRegIndex = stRegOp.getReg() - X86::ST0;
    assert((FPRegIndex >= 0) && (FPRegIndex < FPUSTACK_SZ) &&
           "Unexpected FPU register stack index computed");
    Value *stVal = FPURegisterStackGetValueAt(FPRegIndex);
    assert((stVal != nullptr) && "Failed to get value of FPU register");
    if (stVal->getType() != st0ValTy) {
      CastInst *CInst = CastInst::Create(
          CastInst::getCastOpcode(stVal, false, st0ValTy, false), stVal,
          st0ValTy);
      curBlock->getInstList().push_back(CInst);
      stVal = CInst;
    }
    // Create fmul
    Instruction *FPRegOpInstr = nullptr;
    if (opcode == X86::MUL_FPrST0) {
      FPRegOpInstr = BinaryOperator::CreateFMul(stVal, st0Val);
    } else if (opcode == X86::DIV_FPrST0) {
      FPRegOpInstr = BinaryOperator::CreateFDiv(stVal, st0Val);
    }
    curBlock->getInstList().push_back(FPRegOpInstr);
    // Update the FP register FPRegIndex with FPRegOpInstr
    FPURegisterStackSetValueAt(FPRegIndex, FPRegOpInstr);
    // Pop FPU register stack
    FPURegisterStackPop();
  } break;
  default: {
    assert(false && "Unhandled FPU instruction");
  } break;
  }

  return true;
}

// Raise Call instruction
bool X86MachineInstructionRaiser::raiseCallMachineInstr(
    const MachineInstr &CallMI, BasicBlock *curBlock) {
  unsigned int opcode = CallMI.getOpcode();
  switch (opcode) {
    // case X86::CALLpcrel16   :
    // case X86::CALLpcrel32   :
  case X86::CALL64pcrel32: {
    const MCInstrDesc &MCID = CallMI.getDesc();
    assert(X86II::isImmPCRel(MCID.TSFlags) &&
           "PC-Relative control transfer expected");

    // Get target offset of the call instruction
    const MachineOperand &MO = CallMI.getOperand(0);
    assert(MO.isImm() && "Expected immediate operand not found");
    int64_t RelCallTargetOffset = MO.getImm();

    // Compute the MCInst index of the call target
    MCInstRaiser *MCIR = getMCInstRaiser();
    // Get MCInst offset of the corresponding call instruction in the binary.
    uint64_t MCInstOffset = MCIR->getMCInstIndex(CallMI);
    assert(MCIR != nullptr && "MCInstRaiser not initialized");
    Function *CalledFunc = nullptr;
    uint64_t MCInstSize = MCIR->getMCInstSize(MCInstOffset);
    // First check if PC-relative call target embedded in the call instruction
    // can be used to get called function.
    int64_t CallTargetIndex = MCInstOffset + MR->getTextSectionAddress() +
                              MCInstSize + RelCallTargetOffset;
    // Get the function at index CalltargetIndex
    CalledFunc = MR->getFunctionAt(CallTargetIndex);
    // If not, use text section relocations to get the
    // call target function.
    if (CalledFunc == nullptr) {
      CalledFunc =
          MR->getCalledFunctionUsingTextReloc(MCInstOffset, MCInstSize);
    }
    // Look up the PLT to find called function
    if (CalledFunc == nullptr) {
      CalledFunc = getTargetFunctionAtPLTOffset(CallMI, CallTargetIndex);
    }

    std::vector<Value *> CallInstFuncArgs;
    unsigned NumArgs = CalledFunc->arg_size();
    Argument *CalledFuncArgs = CalledFunc->arg_begin();

    if (CalledFunc->isVarArg()) {
      // Discover argument registers that are live just before the CallMI.
      const MachineBasicBlock *CurMBB = CallMI.getParent();
      // Liveness of the blocks is already computed in
      // getRaisedFunctionPrototype(). So no need to run it again since no MBB
      // would be modified.
      MachineBasicBlock::const_reverse_iterator CallInstIter(CallMI);
      // Find the highest argument register that is defined in the block
      // before the CallMI. NOTE : We assume that all arguments are setup
      // prior to the call. This argument setup manifests as defines in the
      // block or a combination of argument registers that are live-in and
      // defines in the block. Additionally, if the block has more than one
      // calls, it is assumed that call setup for all calls other than the
      // first is done entirely in the block after the preceding call. In such
      // a situation, there is no need to look for argument registers in the
      // live-ins of the block. bool UseLiveIns = true;

      std::set<MCPhysReg> RegsLiveAtCall;
      // Bit mask to keep track of argument register positions already
      // discovered.
      uint8_t PositionMask = 0;

      // Find out the types of arguments set up before call instruction
      for (const MachineInstr &MI :
           make_range(++CallInstIter, CurMBB->rend())) {
        // Stop walking past the most recent call instruction in the block.
        if (MI.isCall()) {
          // UseLiveIns = false;
          break;
        }
        // If the instruction has a define
        if (MI.getNumDefs() > 0) {
          for (auto MO : MI.defs()) {
            // If the define is a register
            if (MO.isReg()) {
              unsigned Reg = MO.getReg();
              if (TargetRegisterInfo::isPhysicalRegister(Reg)) {
                int ArgNo = getArgumentNumber(Reg);
                if (ArgNo > 0) {
                  uint8_t ArgNoMask = (1 << ArgNo);
                  // Consider only the most recent definition
                  if ((PositionMask & ArgNoMask) == 0) {
                    RegsLiveAtCall.emplace(Reg);
                    PositionMask |= ArgNoMask;
                  }
                }
              }
            }
          }
        }
      }
#if 0
        // May be incorrect??
        // TODO : Do we need to look to see if any of the liveins are argument registers
        if (UseLiveIns) {
          for (auto LI : CurMBB->liveins()) {
            MCPhysReg Reg = LI.PhysReg;
            if (TargetRegisterInfo::isPhysicalRegister(Reg)) {
              int ArgNo = MIR.getArgumentNumber(Reg);
              if (ArgNo > 0) {
                uint8_t ArgNoMask = (1 << ArgNo);
                if ((PositionMask & ArgNoMask) == 0) {
                  RegsLiveAtCall.emplace(Reg);
                  PositionMask |= ArgNoMask;
                }
              }
            }
          }
        }
#endif
      // Find the number of arguments
      // NOTE: Handling register arguments - 6 in number. Need to handle
      // arguments passed on stack make sure bit 8 and bit 0 are not set
      assert(!(PositionMask & 1) && !(PositionMask & (1 << 7)) &&
             "Invalid argument numbers discovered");
      uint8_t ShftPositionMask = PositionMask >> 1;
      uint8_t NumArgsDiscovered = 0;
      // Consider only consecutive argument registers.
      while (ShftPositionMask & 1) {
        ShftPositionMask = ShftPositionMask >> 1;
        NumArgsDiscovered++;
      }
      // If number of arguments discovered is greater than CalledFunc
      // arguments use that as the number of arguments of the called function.
      if (NumArgsDiscovered > NumArgs) {
        NumArgs = NumArgsDiscovered;
      }
    }
    // Construct the argument list with values to be used to construct a new
    // CallInst. These values are those of the physical registers as defined
    // in C calling convention (the calling convention currently supported).
    for (unsigned i = 0; i < NumArgs; i++) {
      // Get the values of argument registers
      Value *ArgVal = getRegValue(GPR64ArgRegs64Bit[i]);
      if (i < CalledFunc->arg_size()) {
        // This condition will not be true for variadic functions.
        // In that case just add the value.
        if (ArgVal->getType() != CalledFuncArgs[i].getType()) {
          CastInst *CInst = CastInst::Create(
              CastInst::getCastOpcode(ArgVal, false,
                                      CalledFuncArgs[i].getType(), false),
              ArgVal, CalledFuncArgs[i].getType());
          curBlock->getInstList().push_back(CInst);
          ArgVal = CInst;
        }
      }
      CallInstFuncArgs.push_back(ArgVal);
    }
    // Construct call inst.
    CallInst *callInst =
        CallInst::Create(CalledFunc, ArrayRef<Value *>(CallInstFuncArgs));
    curBlock->getInstList().push_back(callInst);
    // A function call with a non-void return will modify
    // RAX.
    Type *RetType = CalledFunc->getReturnType();
    if (!RetType->isVoidTy()) {
      updatePhysRegSSAValue(X86::RAX, callInst);
    }
  } break;
  default: {
    assert(false && "Unhandled call instruction");
  } break;
  }

  return true;
}

// Top-level function that calls appropriate function that raises
// a MachineInstruction.
// Returns true upon success.

bool X86MachineInstructionRaiser::raiseMachineInstr(MachineInstr &mi,
                                                    BasicBlock *curBlock) {
  const MCInstrDesc &MIDesc = mi.getDesc();

  if (MIDesc.mayLoad() || MIDesc.mayStore()) {
    return raiseMemRefMachineInstr(mi, curBlock);
  } else if (MIDesc.isReturn()) {
    return raiseReturnMachineInstr(mi, curBlock);
  } else {
    return raiseGenericMachineInstr(mi, curBlock);
  }
  return false;
}

// Raise MachineInstr in MachineFunction to MachineInstruction

bool X86MachineInstructionRaiser::raiseMachineFunction() {
  Function *curFunction = getRaisedFunction();
  LLVMContext &llvmContext(curFunction->getContext());
  // Start with an assumption that values of EFLAGS and RSP are 0 at the entry
  // of each function.
  Value *Zero32BitValue =
      ConstantInt::get(Type::getInt32Ty(llvmContext), 0, false /* isSigned */);
  Value *Zero64BitValue =
      ConstantInt::get(Type::getInt64Ty(llvmContext), 0, false /* isSigned */);
  updatePhysRegSSAValue(X86::EFLAGS, Zero32BitValue);
  // Set values of some registers that appear to be used in main function to
  // 0.
  if (curFunction->getName().equals("main")) {
    updatePhysRegSSAValue(X86::RCX, Zero64BitValue);
  }

  // Walk basic blocks of the MachineFunction. Raise all non control
  // transfer MachineInstrs of each MachineBasicBlocks of MachineFunction,
  // except branch instructions.
  for (MachineFunction::iterator mfIter = MF.begin(), mfEnd = MF.end();
       mfIter != mfEnd; mfIter++) {
    MachineBasicBlock &MBB = *mfIter;
    // Get the number of MachineBasicBlock being looked at.
    int MBBNo = MBB.getNumber();
    // Name of the corresponding BasicBlock to be created
    StringRef BBName = MBBNo == 0 ? "entry" : "bb." + std::to_string(MBBNo);
    // Create a BasicBlock instance corresponding to MBB being looked at.
    // The raised form of MachineInstr of MBB will be added to curBlock.
    BasicBlock *CurIBB = BasicBlock::Create(llvmContext, BBName, curFunction);
    // Record the mapping of the number of MBB to corresponding BasicBlock.
    // This information is used to raise branch instructions, if any, of the
    // MBB in a later walk of MachineBasicBlocks of MF.
    mbbToBBMap.insert(std::make_pair(MBBNo, CurIBB));
    // Walk MachineInsts of the MachineBasicBlock
    for (MachineBasicBlock::iterator mbbIter = mfIter->instr_begin(),
                                     mbbEnd = mfIter->instr_end();
         mbbIter != mbbEnd; mbbIter++) {
      MachineInstr &mi = *mbbIter;
      // Ignore noop instructions.
      if (isNoop(mi.getOpcode())) {
        continue;
      }
      // If this is a terminator instruction, record
      // necessary information to raise it in a later pass.
      if (mi.isTerminator() && !mi.isReturn()) {
        recordMachineInstrInfo(mi, CurIBB);
        continue;
      }
      if (mi.isCall()) {
        if (!raiseCallMachineInstr(mi, CurIBB)) {
          return false;
        }
      } else if (!raiseMachineInstr(mi, CurIBB)) {
        return false;
      }
    }
  }
  return raiseBranchMachineInstrs();
}

bool X86MachineInstructionRaiser::raise() { return raiseMachineFunction(); }

#ifdef __cplusplus
extern "C" {
#endif
MachineInstructionRaiser *
InitializeX86MachineInstructionRaiser(MachineFunction &machFunc, Module &m,
                                      const ModuleRaiser *mr,
                                      MCInstRaiser *mcir) {
  return new X86MachineInstructionRaiser(machFunc, m, mr, mcir);
}
#ifdef __cplusplus
}
#endif
