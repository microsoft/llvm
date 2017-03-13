//===-- GraphColor/TileCheck.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

namespace Tiled
{
namespace RegisterAllocator
{
namespace GraphColor
{

#if defined(TILED_DEBUG_CHECKS)

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Helper utility that tests for nested tile on the uninitialized TileGraph.
//
//-------------------------------------------------------------------------------------------------------------

Tiled::Boolean
TileGraph::IsNestedTileMinimal
(
   GraphColor::Tile ^ tile,
   GraphColor::Tile ^ parentTile
)
{
   GraphColor::Tile ^ currentTile = tile;

   Assert(currentTile != nullptr);

   // Search back up the tile tree and see if we arrive at the parent
   // tile.  If we reach the root we're not nested.

   while ((currentTile != nullptr) && (currentTile != parentTile))
   {
      Assert(currentTile != currentTile->ParentTile);

      currentTile = currentTile->ParentTile;
   }

   // return most nested tile or nullptr

   return (currentTile != nullptr) ? true : false;
}

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Find loop to update with new boundary block.
//
// Arguments:
//
//    edge    - Edge to split for tile boundary.
//    isEntry - true if edge is to be split for an entry block.
//
// Returns:
//
//    target loop to update with new block, nullptr if no loop.
//
//-------------------------------------------------------------------------------------------------------------

Loops::Loop ^
TileGraph::FindBoundaryBlockLoop
(
   Graphs::FlowEdge ^ edge,
   Tiled::Boolean       isEntry
)
{
   Graphs::BasicBlock ^ outerBlock = (isEntry) ? edge->PredecessorNode : edge->SuccessorNode;
   Graphs::BasicBlock ^ innerBlock = (isEntry) ? edge->SuccessorNode : edge->PredecessorNode;
   Loops::Loop ^        outerLoop = outerBlock->Loop;
   Loops::Loop ^        innerLoop = innerBlock->Loop;

   if ((outerLoop == nullptr) || outerLoop->IsNestedLoop(innerLoop))
   {
      return outerBlock->Loop;
   }
   else if (innerLoop == nullptr)
   {
      return nullptr;
   }
   else
   {
      if (outerLoop->NestLevel > innerLoop->NestLevel)
      {
         while (outerLoop->NestLevel > innerLoop->NestLevel)
         {
            outerLoop = outerLoop->ParentLoop;
         }
      }
      else if (innerLoop->NestLevel > outerLoop->NestLevel)
      {
         while (innerLoop->NestLevel > outerLoop->NestLevel)
         {
            innerLoop = innerLoop->ParentLoop;
         }
      }

      Assert(innerLoop->NestLevel == outerLoop->NestLevel);

      while ((outerLoop != nullptr) && (outerLoop != innerLoop))
      {
         outerLoop = outerLoop->ParentLoop;
         innerLoop = innerLoop->ParentLoop;
      }

      if (outerLoop == nullptr)
      {
         Assert(innerLoop == nullptr);

         return nullptr;
      }
      else
      {
         return outerLoop;
      }
   }
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Check the tile graph (i.e. tree) for the function.
//
//-----------------------------------------------------------------------------

void
TileGraph::CheckGraphMinimal()
{
   foreach_tile_in_tilegraph(tile, this)
   {
      foreach_tile_in_tilegraph(otherTile, this)
      {
         if (tile == otherTile)
         {
            continue;
         }

         if (tile->ParentTile == otherTile)
         {
            // Ensure that all child tiles are a subset of their parents.

            Assert(otherTile->BodyBlockSet->IsSubset(tile->BodyBlockSet));

            // Check parent/child relationship

            Assert(otherTile->NestedTileSet->GetBit(tile->Id));

            foreach_tile_entry_edge(entryEdge, tile)
            {
               Assert(!(entryEdge->IsBack && tile->BodyBlockSet->GetBit(entryEdge->PredecessorNode->Id)));

               if (entryEdge->IsSplittable)
               {
                  Graphs::BasicBlock ^ entryPredecessorBlock = entryEdge->PredecessorNode;
                  GraphColor::Tile ^   predecessorTile = this->GetTile(entryPredecessorBlock);
                  Tiled::Boolean         isNestedTile = this->IsNestedTileMinimal(predecessorTile, otherTile);

                  if (isNestedTile)
                  {
                     Assert(predecessorTile->BodyBlockSet->GetBit(entryPredecessorBlock->Id));

                     while (predecessorTile != otherTile)
                     {
                        Assert(predecessorTile->ExitEdgeList->Contains(entryEdge));

                        predecessorTile = predecessorTile->ParentTile;
                     }
                  }
                  else
                  {
                     Assert(otherTile->EntryEdgeList->Contains(entryEdge));
                  }
               }
            }
            next_tile_entry_edge;

            foreach_tile_exit_edge(exitEdge, tile)
            {
               Assert(!(exitEdge->IsBack && tile->BodyBlockSet->GetBit(exitEdge->SuccessorNode->Id)));

               if (exitEdge->IsSplittable)
               {
                  Graphs::BasicBlock ^ exitSuccessorBlock = exitEdge->SuccessorNode;
                  GraphColor::Tile ^   successorTile = this->GetTile(exitSuccessorBlock);
                  Tiled::Boolean         isNestedTile = this->IsNestedTileMinimal(successorTile, otherTile);

                  if (isNestedTile)
                  {
                     Assert(successorTile->BodyBlockSet->GetBit(exitSuccessorBlock->Id));

                     // Assert we have an entry for any child tiles.

                     while (successorTile != otherTile)
                     {
                        Assert(successorTile->EntryEdgeList->Contains(exitEdge));

                        successorTile = successorTile->ParentTile;
                     }
                  }
                  else
                  {
                     // Assert we have an exit 

                     Assert(otherTile->ExitEdgeList->Contains(exitEdge));
                  }
               }
            }
            next_tile_exit_edge;
         }
      }
      next_tile_in_tilegraph;
   }
   next_tile_in_tilegraph;
}

//-----------------------------------------------------------------------------
//
// Description:
//
//    Check the tile graph (i.e. tree) for the function.
//
//-----------------------------------------------------------------------------

void
TileGraph::CheckGraph()
{
   // Check invariants of tile tree.

   foreach_tile_in_dfs_postorder(tile, this)
   {
      GraphColor::Tile ^ parentTile = tile->ParentTile;
      GraphColor::Tile ^ fromTile;
      GraphColor::Tile ^ toTile;

      Assert((parentTile == nullptr) || (!tile->EntryBlockList->IsEmpty));

      toTile = tile;
      foreach_BasicBlock_in_List(block, tile->EntryBlockList)
      {
         fromTile = this->GetTile(block);
         Assert(fromTile == parentTile);
         Assert(block->HasUniqueSuccessor);
      }
      next_BasicBlock_in_List;

      fromTile = tile;
      foreach_BasicBlock_in_List(block, tile->ExitBlockList)
      {
         toTile = this->GetTile(block);
         Assert(toTile == parentTile);
      }
      next_BasicBlock_in_List;

      foreach_block_in_tile(block, tile)
      {
         Assert(tile->IsBodyBlockInclusive(block));

         foreach_block_pred_edge(predecessorEdge, block)
         {
            Graphs::BasicBlock ^ predecessorBlock = predecessorEdge->PredecessorNode;
            Tiled::Boolean         isValidTileGraph = false;

            if (tile->IsBodyBlockInclusive(predecessorBlock))
            {
               isValidTileGraph = true;
            }
            else if (tile->IsEntryBlock(predecessorBlock))
            {
               isValidTileGraph = true;
            }
            else if (this->CanBeTileExitEdge(predecessorEdge))
            {
               isValidTileGraph = true;
            }
            else
            {
               foreach_nested_tile_in_tile(nestedTile, tile)
               {
                  if (nestedTile->IsBodyBlockInclusive(predecessorBlock))
                  {
                     if (nestedTile->IsExitBlock(block) || this->CanBeTileExitEdge(predecessorEdge))
                     {
                        isValidTileGraph = true;
                        break;
                     }
                  }
               }
               next_nested_tile_in_tile;
            }
            Assert(isValidTileGraph);
         }
         next_block_pred_edge;

         foreach_block_succ_edge(successorEdge, block)
         {
            Graphs::BasicBlock ^ successorBlock = successorEdge->SuccessorNode;
            Tiled::Boolean         isValidTileGraph = false;

            if (tile->IsBodyBlockInclusive(successorBlock))
            {
               isValidTileGraph = true;
            }
            else if (tile->IsExitBlock(successorBlock))
            {
               isValidTileGraph = true;
            }
            else if (this->CanBeTileExitEdge(successorEdge))
            {
               isValidTileGraph = true;
            }
            else
            {
               foreach_nested_tile_in_tile(nestedTile, tile)
               {
                  if (nestedTile->IsBodyBlockInclusive(successorBlock))
                  {
                     if (nestedTile->IsEntryBlock(block) || this->CanBeTileExitEdge(successorEdge))
                     {
                        isValidTileGraph = true;
                        break;
                     }
                  }
               }
               next_nested_tile_in_tile;
            }

            Assert(isValidTileGraph);
         }
         next_block_succ_edge;
      }
      next_block_in_tile;
   }
   next_tile_in_dfs_postorder;
}

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//    Check that the tile relationships across flow are correct.  For any non-EH successor/predecessor
//    relationship either the blocks must be in the same tile or one of the pair must be a tile transition
//    block.
//
//-------------------------------------------------------------------------------------------------------------

void
TileGraph::CheckGraphFlow()
{
   GraphColor::Allocator ^       allocator = this->Allocator;
   Tiled::FunctionUnit ^           functionUnit = allocator->FunctionUnit;
   Tiled::Lifetime ^               lifetime = this->Lifetime;
   Collections::BasicBlockList ^ blockList = Collections::BasicBlockList::New(lifetime);
   BitVector::Sparse ^           visitedBitVector = this->VisitedBitVector;

   visitedBitVector->ClearAll();

   // Collect root set of blocks - these should all start in the
   // root tile.

   foreach_block_in_func(block, functionUnit)
   {
      Tiled::Boolean hasNonEhPredecessorEdge = false;

      foreach_block_pred_edge(predecessorEdge, block)
      {
         if (!Tile::IsExceptionEdge(predecessorEdge))
         {
            hasNonEhPredecessorEdge = true;
            break;
         }
      }
      next_block_pred_edge;

      if (!hasNonEhPredecessorEdge)
      {
         visitedBitVector->SetBit(block->Id);
         blockList->Append(block);
      }
   }
   next_block_in_func;

   GraphColor::Tile ^   rootTile = this->RootTile;
   GraphColor::Tile ^   currentTile;
   GraphColor::Tile ^   matchTile = nullptr;
   Graphs::BasicBlock ^ unwindBlock = functionUnit->UnwindInstruction->BasicBlock;

   while (!blockList->IsEmpty)
   {
      Graphs::BasicBlock ^  block = blockList->RemoveHead();
      GraphColor::BlockKind blockKind = this->GetBlockKind(block);

      if (block == unwindBlock)
      {
         continue;
      }

      currentTile = this->GetTile(block);

      switch(blockKind)
      {
         case GraphColor::BlockKind::EnterTile:
         {
            GraphColor::Tile ^ nestedTile = this->GetNestedTile(block);
            GraphColor::Tile ^ parentTile = nestedTile->ParentTile;

            AssertM(currentTile == parentTile, "Parent of nested tile not current tile.");
            AssertM(currentTile->NestedTileSet->GetBit(nestedTile->Id),
               "Nested tile not a child of current tile.");

            // All tile predecessors should match the current tile.

            matchTile = currentTile;
         }
         break;

         case GraphColor::BlockKind::ExitTile:
         {
            currentTile = this->GetTile(block);

            GraphColor::Tile ^ nestedTile = this->GetNestedTile(block);
            GraphColor::Tile ^ parentTile = nestedTile->ParentTile;

            AssertM(currentTile == parentTile, "Parent of nested tile not current tile.");
            AssertM(currentTile->NestedTileSet->GetBit(nestedTile->Id),
               "Nested tile not a child of current tile.");

            // all predecessors should match the parent tile.

            matchTile = nestedTile;
         }
         break;

         case GraphColor::BlockKind::Interior:
         {
            // all predecessors should match current tile

            matchTile = currentTile;
         }
         break;

         default:
         {
            AssertM(false, "unknown block kind");
         }
      };

      foreach_block_pred_edge(predecessorEdge, block)
      {
         if (predecessorEdge->IsException)
         {
            continue;
         }

         Graphs::BasicBlock ^  predecessorBlock = predecessorEdge->PredecessorNode;
         GraphColor::BlockKind blockKind = this->GetBlockKind(predecessorBlock);
         GraphColor::Tile ^    predecessorTile = this->GetTile(predecessorBlock);

         if (blockKind == GraphColor::BlockKind::EnterTile)
         {
            AssertM(predecessorTile == matchTile->ParentTile, "Enter tile should match parent tile");
         }
         else
         {
            AssertM(predecessorTile == matchTile, "Predecessor tile doesn't match!");
         }
      }
      next_block_pred_edge;

      if (block->HasSuccessor)
      {
         foreach_block_succ_block(successorBlock, block)
         {
            if (!visitedBitVector->GetBit(successorBlock->Id))
            {
               visitedBitVector->SetBit(successorBlock->Id);
               blockList->Append(successorBlock);

            }
         }
         next_block_succ_block;
      }
      else
      {
         AssertM(currentTile == rootTile, "No successor block should be in root tile.");
      }
   }

   blockList->Delete();
}

//-------------------------------------------------------------------------------------------------------------
//
// Description:
//
//   Check that the entry/exit edges are valid for a tile.
//
// Notes:
//   This function should only be called before tile initialization
//   while tile graph shape is still being determined.
//
//-------------------------------------------------------------------------------------------------------------


void
Tile::CheckBoundaryEdges()
{
   GraphColor::TileGraph ^ tileGraph = this->TileGraph;

   foreach_tile_entry_edge(edge, this)
   {
      if (edge->IsException)
      {
         continue;
      }

      Assert(tileGraph->CanSplitEdge(edge));
   }
   next_tile_entry_edge;

   foreach_tile_exit_edge(edge, this)
   {
      if (edge->IsException)
      {
         continue;
      }

      Assert(tileGraph->CanSplitEdge(edge));
   }
   next_tile_exit_edge;
}

#endif // TILED_DEBUG_CHECKS

} // GraphColor
} // RegisterAllocator
} // Phx
