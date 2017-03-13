//===-- Graphs/UnionFind.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This module provides an implementation of the standard union-find tree
// algorithm featuring balancing and path compression found in most standard
// algorithm books.
//
// This implementation features support for non-contiguous range of member
// names and customization of the UnionFind::Member. It may be subclassed 
// to contain user-defined fields to speed up the back-mapping from equivalence
// class to user data-structures.
//
//===----------------------------------------------------------------------===//

#include "UnionFind.h"
#include <assert.h>

namespace Tiled
{
namespace UnionFind
{

//-----------------------------------------------------------------------------
//
// Description:
//
//    Create a UnionFind::Member that represents a member within the
//    equivalence classes.
//
// Arguments:
//
//    lifetime - Allocation lifetime
//    name - Member name
//
// Returns:
//
//    The new UnionFind::Member *.
//
//-----------------------------------------------------------------------------

UnionFind::Member *
Member::New
(
   int name
)
{
   UnionFind::Member * member = new UnionFind::Member();

   // Initialize values.
   member->Initialize(name);

   return member;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Initialize the base fields of this UnionFind::Member
//
// Arguments:
//
//    name - Member name
//
//-----------------------------------------------------------------------------

void
Member::Initialize
(
   int name
)
{
   UnionFind::Member * member = this;

   member->Name = name;
   member->Size = 1;
   member->Parent = nullptr;
   member->FirstMember = member;
   member->LastMember = member;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Create a UnionFind tree implementation that is initially empty.
//
// Arguments:
//
//    lifetime - Allocation lifetime
//
// Returns:
//
//    The new UnionFind *.
//
//-----------------------------------------------------------------------------

UnionFind::Tree *
Tree::New()
{
   UnionFind::Tree * unionFindTree = new UnionFind::Tree();

   unionFindTree->Size = 0;
   unionFindTree->MemberMap = UnionFind::IntToMemberMap::New(256);

   return unionFindTree;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Insert the given unique Member into the UnionFind tree.
//
// Arguments:
//
//    insertMember - Member to insert
//
//-----------------------------------------------------------------------------

void
Tree::InsertUnique
(
   Member * insertMember
)
{
   assert(insertMember != nullptr);

   UnionFind::IntToMemberMap::value_type entry(insertMember->Name, insertMember);
   this->MemberMap->insert(entry);
   this->Size++;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Insert the given Member into the UnionFind tree.
//
// Arguments:
//
//    insertMember - Member to insert
//
// Returns:
//
//    False if their already exists a member in the UnionFind tree
//    with the same name.
//
//-----------------------------------------------------------------------------

bool
Tree::Insert
(
   Member * insertMember
)
{
   assert(insertMember != nullptr);

   Member * member = this->MemberMap->Lookup(insertMember->Name);

   // Member is already member of UnionFind so nothing to do.
   if (member != nullptr) {
      return true;
   }

   this->InsertUnique(insertMember);

   return false;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Perform a union operation on the two members.
//
// Arguments:
//
//    member1 - First member to union
//    member2 - Second member to union
//
// Returns:
//
//    The member that represents the union
//
//-----------------------------------------------------------------------------

Member *
Tree::Union
(
   Member * member1,
   Member * member2
)
{
   assert(member1 != nullptr);
   assert(member2 != nullptr);

   // Each member must be the header of the equivalence tree.
   assert(member1->Parent == nullptr);
   assert(member2->Parent == nullptr);

   // The same member naturally belongs to the same equivalence.

   if (member1 != member2) {
      // Make the smaller equivalence tree a member of the larger
      // equivalence tree.

      if (member1->Size < member2->Size) {
         member1->Parent = member2;
         member2->Size += member1->Size;

         // Concatenate each doubley-linked circular list.
         member2->FirstMember->LastMember = member1->LastMember;
         member1->LastMember->FirstMember = member2->FirstMember;
         member2->FirstMember = member1;
         member1->LastMember = member2;

         return member2;
      } else {
         member2->Parent = member1;
         member1->Size += member2->Size;

         // Concatenate each doubley-linked circular list.
         member1->FirstMember->LastMember = member2->LastMember;
         member2->LastMember->FirstMember = member1->FirstMember;
         member1->FirstMember = member2;
         member2->LastMember = member1;
      }
   }

   return member1;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Lookup the union-find node that corresponds to the given name.
//
// Arguments:
//
//    name - Member name
//
// Remarks:
//
//    The node returned is not guaranteed to be the base of the
//    equivalence class.
//
// Returns:
//
//    UnionFind::Member *
//
//-----------------------------------------------------------------------------

Member *
Tree::Lookup
(
   int name
)
{
   // Find this names member in the equivalence tree.

   Member * member = this->MemberMap->Lookup(name);

   return member;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Find the corresponding equivalence class representative to which
//    the given equivalence member belongs to.
//
// Arguments:
//
//    leafMember - Leaf member
//
// Remarks:
//
//    This implementation uses the standard path compression
//    technique.
//
// Returns:
//
//    The Member * that represents the equivalence class of the given
//    member, or nullptr if their is no such member in the tree.
//
//-----------------------------------------------------------------------------

Member *
Tree::Find
(
   Member * leafMember
)
{
   assert(leafMember != nullptr);

   Member * parentMember = leafMember->Parent;

   // Check the statistically common zero and one-level cases.

   if (parentMember == nullptr) {
      return leafMember;
   }

   Member * rootMember = parentMember->Parent;

   if (rootMember == nullptr) {
      return parentMember;
   }

   // Find the root Member that represents the class.
   while (rootMember->Parent != nullptr)
   {
      rootMember = rootMember->Parent;
   }

   // Now go back and path compress the search path.
   while (leafMember != rootMember)
   {
      Member * parentMember = leafMember->Parent;
      leafMember->Parent = rootMember;
      leafMember = parentMember;
   }

   return rootMember;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Find the corresponding equivalence class representative to which
//    the given name belongs to.
//
// Arguments:
//
//    name - Member name
//
// Remarks:
//
//    This implementation uses the standard path compression
//    technique.
//
// Returns:
//
//    The Member * that represents the equivalence class of the given
//    member, or nullptr if their is no such member in the tree.
//
//-----------------------------------------------------------------------------

Member *
Tree::Find
(
   int name
)
{
   Member *    leafMember;

   // Find this names member in the equivalence tree.
   leafMember = this->Lookup(name);

   if (leafMember == nullptr) {
      return nullptr;
   }

   Member * rootMember = this->Find(leafMember);

   return rootMember;
}


} // namespace UnionFind
} // namespace Tiled
