//===-- Dataflow/Dataflow.h - Dataflow Package ------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_DATAFLOW_DATAFLOW_H
#define TILED_DATAFLOW_DATAFLOW_H

namespace Tiled
{
namespace Dataflow
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Data flow package initialization class.
//
//-----------------------------------------------------------------------------

class Initialize
{

protected:

   static void StaticInitialize();
};
} // namespace Dataflow
} // namespace Tiled

#include "Traverser.h"
#include "Liveness.h"
#include "Defined.h"
#include "AvailableDefinitions.h"

#endif // end TILED_DATAFLOW_DATAFLOW_H
