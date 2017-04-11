//===-- GraphColor/Graph.h --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHCOLOR_GRAPH_H
#define TILED_GRAPHCOLOR_GRAPH_H

#include "../Graphs/BitGraph.h"
#include "../GraphColor/Preference.h"
#include "../Cost.h"

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/MachineLoopInfo.h"

#include <list>
#include <set>

namespace Tiled
{

namespace Expression
{
class Occurrence;
class Value;
typedef std::map<unsigned, Expression::Occurrence*> IdToOccurrenceMap;
typedef std::vector<Expression::Value*> ExpressValueVector;
}

namespace RegisterAllocator
{
namespace GraphColor
{

class Allocator;
class BlockInfo;
class Controls;
class LiveRange;
class Tile;

typedef std::vector<unsigned> IdVector;
typedef std::vector<GraphColor::LiveRange*> LiveRangeVector;
typedef std::vector<GraphColor::LiveRangePreference> LiveRangePreferenceVector;
typedef std::vector<Tiled::RegisterAllocator::Preference> PreferenceVector;
typedef std::list<GraphColor::Tile*> TileList;
typedef std::vector<GraphColor::Tile*> TileVector;
typedef std::vector<llvm::SparseBitVector<>* > SparseBitVectorVector;
typedef std::vector<llvm::MachineOperand*> OperandVector;
typedef std::list<llvm::MachineLoop*> LoopList;
typedef std::vector<GraphColor::PhysicalRegisterPreference> PhysicalRegisterPreferenceVector;
typedef std::vector<GraphColor::PhysicalRegisterPreferenceVector*> PhysicalRegisterPreferenceVectorVector;
typedef std::vector<Tiled::CostValue> CostValueVector;
typedef std::vector<Tiled::Cost> CostVector;
typedef std::vector<bool> BooleanVector;
typedef std::vector<GraphColor::BlockInfo> BlockInfoVector;
typedef std::map<unsigned,unsigned> AliasTagToIdMap;
typedef std::set<unsigned> TagSet;

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Live range enumerator.
//
// Remarks:
//
//    Currently we use the vector implementation to allow enumeration.
//
//-------------------------------------------------------------------------------------------------------------

typedef GraphColor::LiveRangeVector LiveRangeEnumerator;


//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Register allocation graph used for conflict and preference graph representation.
//
//-------------------------------------------------------------------------------------------------------------

class Graph
{

public:

   void Delete() { this->BitGraph->Delete(); } //[nyi] 

public:

   void Build();

public:

   unsigned EdgeCount;
   unsigned NodeCount;

protected:

   virtual void BuildAdjacencyVector() = 0;

   virtual void BuildBitGraph() = 0;

   virtual void InitializeAdjacencyVector() = 0;

   virtual void InitializeBitGraph() = 0;

protected:

   GraphColor::Allocator *                Allocator;
   Tiled::BitGraphs::UndirectedBitGraph * BitGraph;
   GraphColor::LiveRangeVector *          LiveRangeVector;
   GraphColor::Tile *                     Tile;
};

/*
comment Graph::Allocator
{
   // Allocator using this graph
}

comment Graph::BitGraph
{
   // Undirected bit graph representation of the graph.
}

comment Graph::IsTraceEnabled
{
   // Debug flag to enable tracing during building of graph
}

comment Graph::LiveRangeVector
{
   // Cached live range vector from allocator.  Indexed by live range id.
}

comment Graph::Lifetime
{
   // Lifetime for graph and dependent objects
}

comment Graph::Tile
{
   // Allocator tile associated with this connflict graph.
}
*/

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Iterator for the graph. 
//
//-------------------------------------------------------------------------------------------------------------

class GraphIterator
{

public:

   unsigned Count;
   unsigned Index;
};

} // namespace GraphColor
} // namespace RegisterAllocator
} // namespace Tiled

#define foreach_source_and_destination_opnd_editing(operand, instruction, end_iter) \
   llvm::MachineInstr::mop_iterator end_iter = instruction->explicit_operands().end();  \
   llvm::MachineInstr::mop_iterator operand;  \
   llvm::MachineInstr::mop_iterator begin_iter;  \
   bool iteratingSources;  \
   if (instruction->uses().begin() == instruction->uses().end()) {  \
      iteratingSources = false; begin_iter = instruction->defs().begin();  \
   } else {  \
      iteratingSources = true; begin_iter = instruction->uses().begin();  \
   }  \
   for (operand = begin_iter; operand != end_iter;)

#define next_source_and_destination_opnd_editing(operand, instruction, end_iter) \
   ++operand; \
   if (iteratingSources && operand == end_iter) { \
      iteratingSources = false; \
      operand = instruction->defs().begin();  \
      end_iter = instruction->defs().end(); \
   }

//TBD: when const-ness is enforced throughout the code, replace with: const_mop_iterator
#define foreach_source_and_destination_opnd(operand, instruction, end_iter)  \
   llvm::MachineInstr::mop_iterator end_iter = instruction->explicit_operands().end();  \
   llvm::MachineInstr::mop_iterator begin_iter = instruction->uses().begin();  \
   llvm::MachineInstr::mop_iterator operand;  \
   bool iteratingSources;  \
   if (begin_iter == end_iter) {  \
      iteratingSources = false; begin_iter = instruction->defs().begin();  \
      if (begin_iter == instruction->defs().end()) {  \
         end_iter = instruction->implicit_operands().end(); begin_iter = instruction->implicit_operands().begin();  \
      } else {  \
         end_iter = instruction->defs().end();  \
      }  \
   } else {  \
      iteratingSources = true;  \
   }  \
   for (operand = begin_iter; operand != end_iter;)

#define next_source_and_destination_opnd(operand, instruction, end_iter) \
   ++operand; \
   if (iteratingSources && operand == end_iter) { \
      iteratingSources = false; \
      operand = instruction->defs().begin();  \
      end_iter = instruction->defs().end();  \
      if (operand == end_iter) {  \
         end_iter = instruction->implicit_operands().end(); operand = instruction->implicit_operands().begin();  \
      }  \
   }


#define foreach_source_and_destination_opnd_v2(operand, instruction, end_iter)  \
   llvm::MachineInstr::mop_iterator end_iter = instruction->operands_end();  \
   llvm::MachineInstr::mop_iterator begin_iter = instruction->uses().begin();  \
   llvm::MachineInstr::mop_iterator operand;  \
   bool iteratingSources;  \
   if (begin_iter == end_iter) {  \
      iteratingSources = false; begin_iter = instruction->defs().begin();  \
      if (begin_iter == instruction->defs().end()) {  \
         end_iter = instruction->implicit_operands().end(); begin_iter = instruction->implicit_operands().begin();  \
      } else {  \
         end_iter = instruction->defs().end();  \
      }  \
   } else {  \
      iteratingSources = true;  \
   }  \
   for (operand = begin_iter; operand != end_iter;) {  \
       if (!operand->isReg() || (iteratingSources && !operand->isUse()) || (!iteratingSources && !operand->isDef())) {  \
          next_source_and_destination_opnd_v2(operand, instruction, end_iter);  \
          continue;  \
       }  \


#define next_source_and_destination_opnd_v2(operand, instruction, end_iter) \
   ++operand; \
   if (iteratingSources && operand == end_iter) { \
      iteratingSources = false; \
      operand = instruction->defs().begin();  \
      end_iter = instruction->defs().end();  \
      if (operand == end_iter) {  \
         end_iter = instruction->implicit_operands().end(); operand = instruction->implicit_operands().begin();  \
      }  \
   } else if (operand == end_iter && operand == instruction->defs().end()) {  \
      operand = instruction->implicit_operands().begin(); end_iter = instruction->implicit_operands().end();  \
   }

#endif // end TILED_GRAPHCOLOR_GRAPH_H
