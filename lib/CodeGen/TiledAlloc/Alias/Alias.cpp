//===-- Alias/Alias.cpp - Alias Information ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Alias.h"

namespace Tiled
{

namespace VR
{
 
unsigned Tiled::VR::Info::HighPhysicalRegister = 0;

//-----------------------------------------------------------------------------
//
// Description:
//
//    First, compute, as a BitVector, those Tags that have a MUST total
//    overlap with the specified Tag.  Then OR this result into the supplied
//    Tags BitVector.
//
// Arguments:
//
//    tag    - The Tag of interest.
//    tagsBitVector - [inout] The BitVector to OR with tag's total-overlap set.
//
// Remarks:
//
//    If tag maps to an Alias::Location, then tag's overlap is computed against
//    other Locs in tag's Alias::Layout.
//
//    If tag maps to an Alias::Reference, then that tag can refer to several
//    different Locs, each contained in a different Layout.  In this case,
//    the overlap is computed against all those referred-to Locs.
//
//    This is a dataflow analysis utility to include the set of tags that
//    must be totally overlapped by the operand but not allowing for pointers
//    which may point to several different Tags.  One example of using this
//    method is to compute the set of Tags killed by an operand definition
//    during liveness analyss.
//
//-----------------------------------------------------------------------------

void
Info::OrMustTotalTags
(
   unsigned                 tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   assert(tagsBitVector != nullptr);
   
   // The no-identity tags may represent different locations for
   // different dereferences at different points.
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.

   // LLVM's VRs hold single values, NO locations with layouts, and NO references to locations

   // <place for code supporting architectures with sub-registers>

   tagsBitVector->set(tag);
}


//-----------------------------------------------------------------------------
//
// Description:
//
//    Subtract a Tag's must-total-overlap set from the supplied Tags
//    BitVector.
//
// Arguments:
//
//    tag    - Tag of interest.
//    tagsBitVector - [inout] BitVector, representing the must-total-overlap set of
//             Tags, from which to clear aliased Tags.
//
// Remarks:
//
//    This is a utility method for dataflow analysis.  It's used in building
//    the set of Tags that must totally overlap a given Tag.  One example use
//    is to compute the set of Tags killed by an operand definition during
//    liveness analysis.
//
//-----------------------------------------------------------------------------

void
Info::MinusMustTotalTags
(
   unsigned                 tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   assert(tagsBitVector != nullptr);
   
   // The no-identity tags may represent different locations for
   // different dereferences at different points.
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.

   // LLVM's VRs hold single values, NO locations with layouts, and NO references to locations

   // <place for code supporting architectures with sub-registers>

   tagsBitVector->reset(tag);
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    First, compute, as a BitVector, the member tags of the given tag (those
//    tags that would be iterated by GetFirstMember/GetNextMember), then OR
//    this result into the supplied Tags  BitVector.
//
// Arguments:
//
//    tag    - The Tag of interest.
//    tagsBitVector - [inout] The BitVector to OR with tag's member set.
//
// Remarks:
//
//    The member set of a location tag is the singleton set including that tag.
//    The member set of a reference tag is the set of its directly-referenced
//    locations.
//
//-----------------------------------------------------------------------------

void
Info::OrMemberTags
(
   unsigned                 tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   // LLVM's VRs hold single values, NO locations with layouts, and NO references to locations

   // <place for code supporting architectures with sub-registers>

   tagsBitVector->set(tag);
}

bool
Info::CommonMayPartialTags
(
   unsigned                 tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   bool  hasCommon;
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.

   // LLVM's VRs hold single values, NO locations with layouts, and NO references to locations

   // <place for code supporting architectures with sub-registers>

   hasCommon = tagsBitVector->test(tag);
   
   return hasCommon;
}

bool
Info::CommonMayPartialTags
(
   llvm::SparseBitVector<>* tagsBitVector1,
   llvm::SparseBitVector<>* tagsBitVector2
)
{
   bool  hasCommon;

   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.

   // LLVM's VRs hold single values, NO locations with layouts, and NO references to locations

   // <place for code supporting architectures with sub-registers>

   hasCommon = tagsBitVector1->intersects(*tagsBitVector2);

   return hasCommon;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Subtract the specified Tag's may-partial-overlap set from the supplied
//    Tags BitVector.
//
// Arguments:
//
//    tag    - Tag of interest.
//    tagsBitVector - [inout] BitVector, representing the may-partial-overlap set of
//             Tags, from which to clear aliased Tags.
//
// Remarks:
//
//    This is a utility method for dataflow analysis.  It's used in building
//    the set of Tags that may partially overlap another, specified Tag.  One
//    example use is to compute the set of Tags that may be killed by an
//    operand definition in calculating available expressions.
//
//-----------------------------------------------------------------------------

void
Info::MinusMayPartialTags
(
   unsigned                 tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   //VR::Info*  info = this;
   
   assert(tagsBitVector != nullptr);
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.
   
   //if (info->IsLocationTag(tag)) {
   //   Alias::Location ^ location = info->LocationMap(tag);
   
   //   if (location == nullptr) {
   //      // The Location has no fields so must only alias with itself.
   
   tagsBitVector->reset(tag);
   //   } else {
   //      // The Location has aliases with other tags within its layout.
   
   //      tagsBitVector->Minus(location->GetPartialTags());
   //   }
   //} else {
   //   Alias::Reference ^ reference = info->ReferenceMap(tag);
   
   //   tagsBitVector->Minus(reference->GetPartialTags());
   //}
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    First, compute, as a BitVector, those Tags that MAY have a total overlap
//    with the specified Tag, then OR this result into the supplied Tags
//    BitVector.
//
// Arguments:
//
//    tag    - The Tag of interest.
//    tagsBitVector - [inout] The BitVector to OR with tag's total-overlap set.
//
// Remarks:
//
//    If tag maps to an Alias::Location, then tag's overlap is computed against
//    other Locs in tag's Alias::Layout.
//
//    If tag maps to an Alias::Reference, then that tag can refer to several
//    different Locs, each contained in a different Layout.  In this case,
//    the overlap is computed across all those referred-to Locs.
//
//    This is a dataflow analysis utility to compute the set of Tags
//    that must be totally overlapped by the operand and allowing for
//    pointers which may point to several different Tags.  One example
//    of using this method is to compute the set of Tags made live by an
//    operand useage during liveness analyses.
//
//-----------------------------------------------------------------------------

void
Info::OrMayTotalTags
(
   unsigned int                tag,
   llvm::SparseBitVector<>* tagsBitVector
)
{
   //VR::Info* info = this;
   
   assert(tagsBitVector != nullptr);
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.
   
   //if (info->IsLocationTag(tag)) {
   //   Alias::Location ^ location = info->LocationMap(tag);
   
   //   if (location == nullptr) {
   //      // The Location has no fields so must only alias with itself.
   tagsBitVector->set(tag);
   //   } else {
   //      // The Location has aliases with other tags within its layout.
   //      tagsBitVector->Or(location->GetTotalTags());
   //   }
   //} else {
   //   Alias::Reference ^ reference = info->ReferenceMap(tag);
   
   //   // Collect the aliases of all Location in the Reference.
   //   tagsBitVector->Or(reference->GetTotalTags());
   //}
}
   
//-----------------------------------------------------------------------------
//
// Description:
//
//    First, compute, as a BitVector, those Tags that MAY have a partial
//    overlap with a specified Tag.  Then OR this result into the supplied
//    Tags BitVector.
//
// Arguments:
//
//    tag    - The Tag of interest.
//    tagsBitVector - [inout] The BitVector to OR with tag's total-overlap set.
//
// Remarks:
//
//    If tag defines an Alias::Location, then tag's overlap is computed against
//    other Locs in tag's Alias::Layout.
//
//    If tag defines an Alias::Reference, then that tag can refer to several
//    different Locs, each contained in a different Layout.  In this case,
//    the overlap is computed across all those referred-to Locs.
//
//    This is a dataflow analysis utility to compute the set of Tags
//    that may be partially overlapped by the operand and allowing for
//    pointers which may point to several different Tags.  One example
//    of using this method is to compute the set of Tags that may
//    be killed by an operand definition during available expressions.
//
//-----------------------------------------------------------------------------

void
Info::OrMayPartialTags
(
   unsigned                  tag,
   llvm::SparseBitVector<> * tagsBitVector
)
{
   //Alias::Info* info = this;
   assert(tagsBitVector != nullptr);
   
   // The alias tags are computed depending upon whether the operand
   // is a direct reference Location or an indirect reference Reference.
   
   //if (info->IsLocationTag(tag)) {
   //   Alias::Location ^ location = info->LocationMap(tag);
   
   //   if (location == nullptr) {
   //      // The Location has no fields so must only alias with itself.
   tagsBitVector->set(tag);
   //   } else {
   //      // The Location has aliases with other tags within its layout.
   //      tagsBitVector->Or(location->GetPartialTags());
   //   }
   //} else {
   //   Alias::Reference ^ reference = info->ReferenceMap(tag);
   
   //   tagsBitVector->Or(reference->GetPartialTags());
   //}
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    A convenience method for getting may-partially-overlapping tags for each
//    tag in a bit vector of tags.
//
// Arguments:
//
//    tagsBitVector   - A bit vector of the Tags of interest.
//    resultBitVector - [inout] The BitVector to OR with the total-overlap set
//                      of each tag in the tagsBitVector.
//
//-----------------------------------------------------------------------------

void
Info::OrMayPartialTags
(
   llvm::SparseBitVector<> * tagsBitVector,
   llvm::SparseBitVector<> * resultBitVector
)
{
   //Alias::Info * info = this;

   if (!tagsBitVector->empty()) {
      //Alias::Tag tag;

      //foreach_sparse_bv_bit(tag, tagsBitVector)
      //{
      //   info->OrMayPartialTags(tag, resultBitVector);
      //}
      //next_sparse_bv_bit;

      // in an architecture with no sub-registers the above can be simplified
      *resultBitVector |= *tagsBitVector;
   }
}

bool
Info::MustTotallyOverlap(unsigned tag1, unsigned tag2)
{
   //This function is quite complex in Alias::Info/Alias::Tag
   if (tag1 == VR::Constants::InvalidTag || tag2 == VR::Constants::InvalidTag)
      return false;
   return (tag1 == tag2);
   //  The above equality can only be used when the tags represent registers on
   //  an architecture without subregisters, it can be extended to sub-registers
   //  quite easily but NOT to tags representing Locations/References/Types.
   //  Equality works for stack indexes too (TBD: what if the stack slots are shared???) 
}

bool
Info::MayPartiallyOverlap(unsigned tag1, unsigned tag2)
{
   //TBD:  !!!
   //This function is quite complex in Alias::Info/Alias::Tag
   return (tag1 == tag2);
}

llvm::MachineOperand*
Info::DestinationMayPartiallyOverlap(llvm::MachineInstr * instruction, llvm::MachineOperand * operand)
{
   assert(instruction != nullptr);
   assert(operand != nullptr);
   assert(operand->isReg());
   unsigned tag = this->GetTag(operand->getReg());

   llvm::MachineInstr::mop_iterator dstOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(), instruction->defs().end());

   // foreach_dataflow_destination_opnd
   for (dstOperand = drange.begin(); dstOperand != drange.end(); ++dstOperand)
   {
      if (dstOperand->isReg()) {
         if (this->MayPartiallyOverlap(this->GetTag(dstOperand->getReg()), tag)) {
            return dstOperand;
         }
      }
   }

   return nullptr;
}

llvm::MachineOperand*
Info::SourceMayPartiallyOverlap(llvm::MachineInstr * instruction, llvm::MachineOperand * operand)
{
   assert(instruction != nullptr);
   assert(operand != nullptr);
   assert(operand->isReg());
   unsigned tag = this->GetTag(operand->getReg());

   llvm::MachineInstr::mop_iterator srcOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(), instruction->explicit_operands().end());

   // foreach_dataflow_source_opnd
   for (srcOperand = uses.begin(); srcOperand != uses.end(); ++srcOperand)
   {
      if (srcOperand->isReg()) {
         if (this->MayPartiallyOverlap(this->GetTag(srcOperand->getReg()), tag)) {
            return srcOperand;
         }
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Summarizes tags defined within a range.
//
// Arguments:
//
//    firstInstruction - First instruction in range
//    lastInstruction - Last instruction in range
//    type - Type of tags to summarize
//    summaryOptions - Restrictions on the set of tags to return
//    defTagSet - [in, out] Summary of def tags
//
// Returns:
//
//    Summary of def tags in the range.  Return value is the same as defTagSet.
//
//-----------------------------------------------------------------------------

llvm::SparseBitVector<>*
Info::SummarizeDefinitionTagsInRange
(
   llvm::MachineInstr *            firstInstruction,
   llvm::MachineInstr *            lastInstruction,
   VR::AliasType                   type,
   VR::SideEffectSummaryOptions    summaryOptions,
   llvm::SparseBitVector<> *       definitionTagSet
)
{
   assert(firstInstruction && lastInstruction && firstInstruction->getParent() == lastInstruction->getParent());
   assert(definitionTagSet != nullptr);

   llvm::MachineInstr * instruction = nullptr;

   // foreach_instr_in_range
   for (instruction = firstInstruction; instruction != nullptr; instruction = instruction->getNextNode())
   {
      //note that PHIs were eliminated before the Tiled RA invocation

      llvm::MachineInstr::mop_iterator destinationOperand;
      llvm::iterator_range<llvm::MachineInstr::mop_iterator> opnd_range(instruction->defs().begin(),
                                                                        instruction->defs().end() );

      // foreach_dataflow_destination_opnd  (TODO: interpret the 'dataflow' filter)
      for (destinationOperand = opnd_range.begin(); destinationOperand != opnd_range.end(); ++destinationOperand)
      {
         if (destinationOperand->isReg()) {
#ifdef FUTURE_IMPL
            if (destinationOperand->IsVirtualProxy) {
               // This represents a merge, not an actual store
               continue;
            }
#endif
            unsigned destinationTag = this->GetTag(destinationOperand->getReg());

            if (type == VR::AliasType::Must) {
               this->OrMustTotalTags(destinationTag, definitionTagSet);
            } else {
               this->OrMayTotalTags(destinationTag, definitionTagSet);
            }
         }
      }
   }

   return definitionTagSet;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search forward through the specified range of IR instructions for any
//    that has a data dependency on a specified instruction.
//
// Arguments:
//
//    instruction     - The IR::Instruction to check against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Remarks:
//
//    The method checks first for data dependencies on instruction's destination
//    ("Definition") operands, and then for data dependencies on instruction's source
//    source ("Use") operands.  The search will not extend beyond the 
//    current basic block.
//    
//    This method is not suitable for intensive use. For serious code motion 
//    the client should use other methods to build a data dependence graph.
//
// Returns:
//
//    The source ("Use") or destination ("Definition") IR::Operand that may have a 
//    partial overlap with any of instruction's operands.  If no dependencies
//    are found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::DependencyForward
(
   llvm::MachineInstr *  instruction,
   llvm::MachineInstr *  fromInstruction,
   llvm::MachineInstr *  toInstruction
)
{
   Tiled::VR::Info * info = this;

   llvm::MachineInstr::mop_iterator dstOperand, srcOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(), instruction->defs().end());
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(), instruction->explicit_operands().end());

   // Search the code stream forwards in the given range.

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * fInstruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getNextNode() : toInstruction;

   // foreach_instr_in_range
   for (fInstruction = fromInstruction; fInstruction != end; fInstruction = fInstruction->getNextNode())
   {
      //the stack slots (dataflow operands) are not tracked here. (TBD ??)

      // Search dataflow destination operands for a dependence.

      // foreach_dataflow_destination_opnd
      for (dstOperand = drange.begin(); dstOperand != drange.end(); ++dstOperand)
      {
         if (dstOperand->isReg()) {
            if (info->PartialUseOrDefinitionForward(dstOperand, fInstruction, fInstruction) != nullptr) {
               return dstOperand;
            }
         }
      }

      // Search dataflow source operands for a dependence.

      // foreach_dataflow_source_opnd
      for (srcOperand = uses.begin(); srcOperand != uses.end(); ++srcOperand)
      {
         if (dstOperand->isReg()) {
            if (info->PartialDefinitionForward(srcOperand, fInstruction, fInstruction) != nullptr) {
               return srcOperand;
            }
         }
      }

      if (fInstruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search the code stream forward, to find whether the specified operand
//    may have a partial overlap with any of the destination ("Definition") operands in
//    that code stream.
//
//    The search will not extend beyond the current basic block.
//
// Arguments:
//
//    operand         - Operand to match against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Returns:
//
//    The destination ("Definition") IR::Operand that may partially overlap operand.  If 
//    no match is found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::PartialDefinitionForward
(
   llvm::MachineOperand *  operand,
   llvm::MachineInstr *    fromInstruction,
   llvm::MachineInstr *    toInstruction
)
{
   Tiled::VR::Info * info = this;

   // Search the code stream forwards in the given range.

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * instruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getNextNode() : toInstruction;

   // foreach_instr_in_range
   for (instruction = fromInstruction; instruction != end; instruction = instruction->getNextNode())
   {
      // Search dataflow operands for a partial overlap.

      llvm::MachineOperand * destinationOperand = info->DestinationMayPartiallyOverlap(instruction, operand);
      if (destinationOperand != nullptr) {
         return destinationOperand;
      }

      if (instruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search backwards through the specified range of IR instructions to find
//    whether the specified operand may have a partial overlap with any of the
//    destination ("Definition") operands in that code stream.  The search will not
//    extend beyond the current basic block.
//
// Arguments:
//
//    operand         - Operand to match against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Returns:
//
//    The destination ("Definition") IR::Operand that may partially overlap operand.  If no
//    match is found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::PartialDefinitionBackward
(
   llvm::MachineOperand *  operand,
   llvm::MachineInstr *    fromInstruction,
   llvm::MachineInstr *    toInstruction
)
{
   Tiled::VR::Info * info = this;

   // Search the code stream forwards in the given range.

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * instruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getPrevNode() : toInstruction;

   // foreach_instr_in_range_backward
   for (instruction = fromInstruction; instruction != end; instruction = instruction->getPrevNode())
   {
      // Search dataflow operands for a partial overlap.

      llvm::MachineOperand * destinationOperand = info->DestinationMayPartiallyOverlap(instruction, operand);
      if (destinationOperand != nullptr) {
         return destinationOperand;
      }

      if (instruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search the code stream forward, to find whether the specified operand
//    may have a partial overlap with any source ("Use") or destination ("Definition")
//    operands in that code stream.  The search will not extend beyond the 
//    current basic block.
//
// Arguments:
//
//    operand         - Operand to match against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Returns:
//
//    The source ("Use") or destination ("Definition") IR::Operand that may have a 
//    partial overlap with operand.  If no match is found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::PartialUseOrDefinitionForward
(
   llvm::MachineOperand *  operand,
   llvm::MachineInstr *    fromInstruction,
   llvm::MachineInstr *    toInstruction
)
{
   Tiled::VR::Info * info = this;

   // Search the code stream forwards in the given range.

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * instruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getNextNode() : toInstruction;

   // foreach_instr_in_range
   for (instruction = fromInstruction; instruction != end; instruction = instruction->getNextNode())
   {
      // Search dataflow source operands for a partial overlap.

      llvm::MachineOperand * sourceOperand = info->SourceMayPartiallyOverlap(instruction, operand);
      if (sourceOperand != nullptr) {
         return sourceOperand;
      }

      // Search dataflow dest operands for a partial overlap.

      llvm::MachineOperand * destinationOperand = info->DestinationMayPartiallyOverlap(instruction, operand);
      if (destinationOperand != nullptr) {
         return destinationOperand;
      }

      if (instruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search the code stream backwards, to find whether the specified operand
//    may have a partial overlap with any source ("Use") or destination ("Definition")
//    operands in that code stream.  The search will not extend beyond the
//    current basic block.
//
// Arguments:
//
//    operand      - The IR::Operand to match against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Returns:
//
//    The source ("Use") or destination ("Definition") IR::Operand that may have a 
//    partial overlap with operand.  If no match is found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::PartialUseOrDefinitionBackward
(
   llvm::MachineOperand *  operand,
   llvm::MachineInstr *    fromInstruction,
   llvm::MachineInstr *    toInstruction
)
{
   Tiled::VR::Info * info = this;

   // Search the code stream forwards in the given range.

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * instruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getPrevNode() : toInstruction;

   // foreach_instr_in_range_backward
   for (instruction = fromInstruction; instruction != end; instruction = instruction->getPrevNode())
   {
      // Search dataflow dest operands for a partial overlap.

      llvm::MachineOperand * destinationOperand = info->DestinationMayPartiallyOverlap(instruction, operand);
      if (destinationOperand != nullptr) {
         return destinationOperand;
      }

      // Search dataflow source operands for a partial overlap.

      llvm::MachineOperand * sourceOperand = info->SourceMayPartiallyOverlap(instruction, operand);
      if (sourceOperand != nullptr) {
         return sourceOperand;
      }

      if (instruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Search backwards through the specified range of IR instructions for any
//    that has a data dependency on a specified instruction.
//
// Arguments:
//
//    instruction     - The IR::Instruction to check against.
//    fromInstruction - Start of code range to search.
//    toInstruction   - End of code range to search.
//
// Remarks:
//
//    The method checks first for data dependencies on instruction's destination
//    ("Definition") operands, and then for data dependencies on instruction's source
//    source ("Use") operands.  The search will not extend beyond the 
//    current basic block.
//    
//    This method is not suitable for intensive use. For serious code motion 
//    the client should use other methods to build a data dependence graph.
//
// Returns:
//
//    The source ("Use") or destination ("Definition") IR::Operand that may have a 
//    partial overlap with any of instruction's operands.  If no dependencies
//    are found, then nullptr.
//
//-----------------------------------------------------------------------------

llvm::MachineOperand *
Info::DependencyBackward
(
   llvm::MachineInstr *  instruction,
   llvm::MachineInstr *  fromInstruction,
   llvm::MachineInstr *  toInstruction
)
{
   Tiled::VR::Info * info = this;

   // Search the code stream backwards in the given range.

   llvm::MachineInstr::mop_iterator srcOperand, dstOperand;
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> uses(instruction->uses().begin(), instruction->explicit_operands().end());
   llvm::iterator_range<llvm::MachineInstr::mop_iterator> drange(instruction->defs().begin(), instruction->defs().end());

   assert(toInstruction == nullptr || toInstruction->getParent() == fromInstruction->getParent());
   llvm::MachineInstr * bInstruction;
   llvm::MachineInstr * end = (toInstruction) ? toInstruction->getNextNode() : toInstruction;

   // foreach_instr_in_range
   for (bInstruction = fromInstruction; bInstruction != end; bInstruction = bInstruction->getPrevNode())
   {
      //the stack slots (dataflow operands) are not tracked here. (TBD ??)

      // Search dataflow destination operands for a dependence.

      // foreach_dataflow_destination_opnd
      for (dstOperand = drange.begin(); dstOperand != drange.end(); ++dstOperand)
      {
         if (dstOperand->isReg()) {
            if (info->PartialUseOrDefinitionBackward(dstOperand, bInstruction, bInstruction) != nullptr) {
               return dstOperand;
            }
         }
      }

      // Search dataflow source operands for a dependence.

      // foreach_dataflow_source_opnd
      for (srcOperand = uses.begin(); srcOperand != uses.end(); ++srcOperand)
      {
         if (srcOperand->isReg()) {
            if (info->PartialDefinitionBackward(srcOperand, bInstruction, bInstruction) != nullptr) {
               return srcOperand;
            }
         }
      }

      if (bInstruction->isLabel()) {
         break;
      }
   }

   return nullptr;
}

} // namespace VR
   
} // namespace Tiled
