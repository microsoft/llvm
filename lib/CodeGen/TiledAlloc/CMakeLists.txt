set(INLFILES cost.inl)
  
set_source_files_properties(${INLFILES} PROPERTIES LANGUAGE CXX)

add_llvm_library(LLVMTiled
  Cost.cpp
  Preference.cpp
  Alias/Alias.cpp
  Dataflow/AvailableDefinitions.cpp
  Dataflow/Dataflow.cpp
  Dataflow/Defined.cpp
  Dataflow/LivenessWalker.cpp
  Dataflow/Traverser.cpp
  GraphColor/Allocator.cpp
  GraphColor/AvailableExpressions.cpp
  GraphColor/ConflictGraph.cpp
  GraphColor/CostModel.cpp
  GraphColor/GraphColorGraph.cpp
  GraphColor/Liveness.cpp
  GraphColor/LiveRange.cpp
  GraphColor/PreferenceGraph.cpp
  GraphColor/SpillOptimizer.cpp
  GraphColor/TargetRegisterAllocInfo.cpp
  GraphColor/Tile.cpp
  Graphs/Graph.cpp
  Graphs/BitGraph.cpp
  Graphs/NodeWalker.cpp
  Graphs/NumberNodeVisitor.cpp
  Graphs/NodeFlowOrder.cpp
  Graphs/UnionFind.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/CodeGen
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/Graphs
  ${CMAKE_CURRENT_SOURCE_DIR}/GraphColor
  ${CMAKE_CURRENT_SOURCE_DIR}/BitVector
  ${CMAKE_CURRENT_SOURCE_DIR}/Dataflow

  DEPENDS
  intrinsics_gen
)

include_directories(${LLVM_MAIN_SRC_DIR}/lib)
