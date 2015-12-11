//===- Linker.h - Module Linker Interface -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LINKER_LINKER_H
#define LLVM_LINKER_LINKER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/FunctionInfo.h"

namespace llvm {
class Module;
class StructType;
class Type;

/// This class provides the core functionality of linking in LLVM. It keeps a
/// pointer to the merged module so far. It doesn't take ownership of the
/// module since it is assumed that the user of this class will want to do
/// something with it after the linking.
class Linker {
public:
  struct StructTypeKeyInfo {
    struct KeyTy {
      ArrayRef<Type *> ETypes;
      bool IsPacked;
      KeyTy(ArrayRef<Type *> E, bool P);
      KeyTy(const StructType *ST);
      bool operator==(const KeyTy &that) const;
      bool operator!=(const KeyTy &that) const;
    };
    static StructType *getEmptyKey();
    static StructType *getTombstoneKey();
    static unsigned getHashValue(const KeyTy &Key);
    static unsigned getHashValue(const StructType *ST);
    static bool isEqual(const KeyTy &LHS, const StructType *RHS);
    static bool isEqual(const StructType *LHS, const StructType *RHS);
  };

  typedef DenseSet<StructType *, StructTypeKeyInfo> NonOpaqueStructTypeSet;
  typedef DenseSet<StructType *> OpaqueStructTypeSet;

  struct IdentifiedStructTypeSet {
    // The set of opaque types is the composite module.
    OpaqueStructTypeSet OpaqueStructTypes;

    // The set of identified but non opaque structures in the composite module.
    NonOpaqueStructTypeSet NonOpaqueStructTypes;

    void addNonOpaque(StructType *Ty);
    void switchToNonOpaque(StructType *Ty);
    void addOpaque(StructType *Ty);
    StructType *findNonOpaque(ArrayRef<Type *> ETypes, bool IsPacked);
    bool hasType(StructType *Ty);
  };

  enum Flags {
    None = 0,
    OverrideFromSrc = (1 << 0),
    LinkOnlyNeeded = (1 << 1),
    InternalizeLinkedSymbols = (1 << 2)
  };

  Linker(Module &M, DiagnosticHandlerFunction DiagnosticHandler);

  /// \brief Link \p Src into the composite. The source is destroyed.
  ///
  /// Passing OverrideSymbols as true will have symbols from Src
  /// shadow those in the Dest.
  /// For ThinLTO function importing/exporting the \p FunctionInfoIndex
  /// is passed. If \p FunctionsToImport is provided, only the functions that
  /// are part of the set will be imported from the source module.
  ///
  /// Returns true on error.
  bool linkInModule(Module &Src, unsigned Flags = Flags::None,
                    const FunctionInfoIndex *Index = nullptr,
                    DenseSet<const GlobalValue *> *FunctionsToImport = nullptr);

  static bool linkModules(Module &Dest, Module &Src,
                          DiagnosticHandlerFunction DiagnosticHandler,
                          unsigned Flags = Flags::None);

  DiagnosticHandlerFunction getDiagnosticHandler() const {
    return DiagnosticHandler;
  }

private:
  Module &Composite;

  IdentifiedStructTypeSet IdentifiedStructTypes;

  DiagnosticHandlerFunction DiagnosticHandler;
};

/// Create a new module with exported local functions renamed and promoted
/// for ThinLTO.
std::unique_ptr<Module>
renameModuleForThinLTO(std::unique_ptr<Module> &M,
                       const FunctionInfoIndex *Index,
                       DiagnosticHandlerFunction DiagnosticHandler);

} // End llvm namespace

#endif
