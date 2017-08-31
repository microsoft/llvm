//===-- Graphs/UnionFind.h --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Package to handle union-find equivalence operations.
//
//===----------------------------------------------------------------------===//

#ifndef TILED_GRAPHS_UNIONFIND_H
#define TILED_GRAPHS_UNIONFIND_H

#include <map>
#include <unordered_map>

namespace Tiled
{
namespace UnionFind
{

//------------------------------------------------------------------------------
//
// Description:
//
//    The Member represents a single member in the equivalence tree
//    linked together by their Next field.
//
// Remarks:
//
//    The UnionFind::Member may be subclassed to keep extra fields to
//    improve the efficiency of mapping classes back user
//    data-structures.
//
//------------------------------------------------------------------------------

class Member
{

public:

   static UnionFind::Member *
   New
   (
      int name
   );

public:
   void Dump();

public:

   UnionFind::Member * FirstMember;
   UnionFind::Member * LastMember;
   int                 Name;
   UnionFind::Member * Parent;
   int                 Size;

   bool                IsRoot() { return (this->Parent == nullptr); }
   bool                IsSingleton() { return (this->Size == 1); }

protected:

   void
   Initialize
   (
      int name
   );
};

#if 0
comment Member::FirstMember
{
   // The first child in equiv class.
}

comment Member::IsRoot
{
   // Tells whether this Member is the current leader of the class.
}

comment Member::IsSingleton
{
   // Tells whether this Member is the sole member of the class.
}

comment Member::LastMember
{
   // The last child in equiv class.
}

comment Member::Name
{
   // The members name.
}

comment Member::Parent
{
   // The leader of the equiv class.
}

comment Member::Size
{
   // Number of members in subtree.
}
#endif


typedef std::unordered_map<int, UnionFind::Member*> UnorderedMap;

// IntToMemberMap as defined here doesn't add value to UnorderedMap,
// it's just a wrapper to contain (potential) future extra functionality.

class IntToMemberMap : public UnionFind::UnorderedMap
{
public:

   static UnionFind::IntToMemberMap *
   New
   (
      unsigned size
   )
   {
      UnionFind::IntToMemberMap * map = new IntToMemberMap(size);
      return map;
   }

   UnionFind::Member *
   Lookup
   (
      unsigned key
   )
   {
      UnorderedMap::iterator i = this->find(key);
      if (i == this->end()) {
         return nullptr;
      }

      return i->second;
   }

private:

   IntToMemberMap
   (
      unsigned size
   ) : UnionFind::UnorderedMap(size) {}

};

//------------------------------------------------------------------------------
//
// Description:
//
//    The UnionFind::Tree is used to quickly form equivalences classes
//    among sets of integers. The primary operations are to find the
//    equivalence class of a member and union two equivalence classes
//    together.
//
//------------------------------------------------------------------------------

class Tree
{

public:

   static UnionFind::Tree * New();

public:

   UnionFind::Member *
   Find
   (
      UnionFind::Member * leafMember
   );

   UnionFind::Member *
   Find
   (
      int member
   );

   bool
   Insert
   (
      UnionFind::Member * member
   );

   void
   InsertUnique
   (
      UnionFind::Member * member
   );

   UnionFind::Member *
   Lookup
   (
      int member
   );

   UnionFind::Member *
   Union
   (
      UnionFind::Member * member1,
      UnionFind::Member * member2
   );

public:

   UnionFind::IntToMemberMap *  MemberMap;
   int                          Size;
};

#if 0
comment Tree::Size
{
   // Original member count.
}
#endif

} // namespace UnionFind
} // namespace Tiled

#endif // end TILED_GRAPHS_NODEWALKER_H

