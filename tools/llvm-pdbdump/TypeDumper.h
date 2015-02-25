//===- TypeDumper.h - PDBSymDumper implementation for types *- C++ ------*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMPDBDUMP_TYPEDUMPER_H
#define LLVM_TOOLS_LLVMPDBDUMP_TYPEDUMPER_H

#include "llvm/DebugInfo/PDB/PDBSymDumper.h"

namespace llvm {

class TypeDumper : public PDBSymDumper {
public:
  TypeDumper(bool Inline, bool ClassDefs);

  void start(const PDBSymbolExe &Exe, raw_ostream &OS, int Indent);

  void dump(const PDBSymbolTypeEnum &Symbol, raw_ostream &OS,
            int Indent) override;
  void dump(const PDBSymbolTypeFunctionSig &Symbol, raw_ostream &OS,
            int Indent) override;
  void dump(const PDBSymbolTypeTypedef &Symbol, raw_ostream &OS,
            int Indent) override;
  void dump(const PDBSymbolTypeUDT &Symbol, raw_ostream &OS,
            int Indent) override;

private:
  bool InlineDump;
  bool FullClassDefs;
};
}

#endif
