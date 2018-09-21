# Check if llvm-mctoll is built as a standalone project.
if( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR )
  project(BinaryRaiser)
  cmake_minimum_required(VERSION 3.4.3)

  set(LLVM_MCTOLL_BUILT_STANDALONE TRUE)

  find_program(LLVM_CONFIG_PATH "llvm-config" DOC "Path to llvm-config binary")
  if(NOT LLVM_CONFIG_PATH)
    message(FATAL_ERROR "llvm-config not found: specify LLVM_CONFIG_PATH")
  endif()

  execute_process(COMMAND "${LLVM_CONFIG_PATH}"
                  "--assertion-mode"
                  "--obj-root"
                  "--bindir"
                  "--src-root"
                  "--cmakedir"
                  "--cxxflags"
                  RESULT_VARIABLE HAD_ERROR
                  OUTPUT_VARIABLE LLVM_CONFIG_OUTPUT
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(HAD_ERROR)
    message(FATAL_ERROR "llvm-config failed with status ${HAD_ERROR}")
  endif()

  string(REGEX REPLACE "[ \t]*[\r\n]+[ \t]*" ";" LLVM_CONFIG_OUTPUT "${LLVM_CONFIG_OUTPUT}")

  list(GET LLVM_CONFIG_OUTPUT 0 ENABLE_ASSERTIONS)
  list(GET LLVM_CONFIG_OUTPUT 1 OBJ_ROOT)
  list(GET LLVM_CONFIG_OUTPUT 2 TOOLS_BINARY_DIR)
  list(GET LLVM_CONFIG_OUTPUT 3 MAIN_SRC_DIR)
  list(GET LLVM_CONFIG_OUTPUT 4 LLVM_CMAKE_PATH)
  list(GET LLVM_CONFIG_OUTPUT 5 LLVM_CXX_FLAGS)

  if(NOT MSVC_IDE)
    set(LLVM_ENABLE_ASSERTIONS ${ENABLE_ASSERTIONS}
      CACHE BOOL "Enable assertions")
    # Assertions should follow llvm-config's.
    mark_as_advanced(LLVM_ENABLE_ASSERTIONS)
  endif()

  set(LLVM_OBJ_ROOT ${OBJ_ROOT} CACHE PATH "Path to LLVM build tree")
  set(LLVM_MAIN_SRC_DIR ${MAIN_SRC_DIR} CACHE PATH "Path to LLVM source tree")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXX_FLAGS}")
  set(LLVM_TOOLS_BINARY_DIR ${TOOLS_BINARY_DIR} CACHE PATH "Path to llvm/bin")
  set(LLVM_EXTERNAL_LIT ${LLVM_TOOLS_BINARY_DIR}/llvm-lit CACHE PATH "Path to llvm-lit")

  file(TO_CMAKE_PATH ${LLVM_OBJ_ROOT} LLVM_BINARY_DIR)

  if(NOT EXISTS "${LLVM_CMAKE_PATH}/LLVMConfig.cmake")
    message(FATAL_ERROR "LLVMConfig.cmake not found")
  endif()
  include("${LLVM_CMAKE_PATH}/LLVMConfig.cmake")
  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_PATH}")

  # They are used as destination of target generators.
  set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin)

  option(LLVM_ENABLE_WARNINGS "Enable compiler warnings." ON)

  include (AddLLVM)
  include_directories(${LLVM_INCLUDE_DIRS})

  include(CheckLibraryExists)
  check_library_exists(xar xar_open "" HAVE_LIBXAR)
  if(HAVE_LIBXAR)
    set(XAR_LIB xar)
  endif()

endif()

set(LLVM_MCTOLL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LLVM_MCTOLL_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

llvm_map_components_to_libnames(llvm_libs
  ${LLVM_TARGETS_TO_BUILD}
  DebugInfoDWARF
  DebugInfoPDB
  Symbolize
)

set(LLVM_MCTOLL_LIB_DEPS ${llvm_libs})

if(LLVM_TARGETS_TO_BUILD MATCHES "X86")
  add_subdirectory(X86)
  list(APPEND LLVM_MCTOLL_LIB_DEPS X86Raiser)
endif()

if(LLVM_TARGETS_TO_BUILD MATCHES "ARM")
  add_subdirectory(ARM)
  list(APPEND LLVM_MCTOLL_LIB_DEPS ARMRaiser)
endif()

add_subdirectory(test)

add_llvm_tool(llvm-mctoll
  llvm-mctoll.cpp
  COFFDump.cpp
  ELFDump.cpp
  ExternalFunctions.cpp
  MachODump.cpp
  WasmDump.cpp
  MachineFunctionRaiser.cpp
  MCInstOrData.cpp
  MCInstRaiser.cpp
  EmitRaisedOutputPass.cpp
)

# Link against LLVM libraries and target-specific Raiser libraries
target_link_libraries(llvm-mctoll PRIVATE ${LLVM_MCTOLL_LIB_DEPS})

if(HAVE_LIBXAR)
  message(STATUS "Linking xar")
  target_link_libraries(llvm-mctoll PRIVATE ${XAR_LIB})
endif()

