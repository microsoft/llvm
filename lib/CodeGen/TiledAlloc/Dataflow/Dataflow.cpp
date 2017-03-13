//===-- Dataflow/Dataflow.cpp - Dataflow Package ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

namespace Tiled
{
namespace Dataflow
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize the data flow packages.
//
//-----------------------------------------------------------------------------

void
Initialize::StaticInitialize()
{
   LivenessWalker::StaticInitialize();
   DefinedWalker::StaticInitialize();
   AvailableDefinitionWalker::StaticInitialize();
}

} // namespace Dataflow
} // namespace Tiled
