# Warnings, after cpp-best-practices' cmake_template (Jason Turner). Every
# target of this project links lite_warnings. The common list is
# warnings.txt, which the wasm build (scripts/36-sourcekit-lsp-lite-wasm.sh)
# reads too.

option(LITE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/warnings.txt" LITE_COMMON_WARNINGS REGEX "^-")

set(LITE_GCC_WARNINGS
  ${LITE_COMMON_WARNINGS}
  -Wmisleading-indentation # indentation implies blocks where blocks do not exist
  -Wduplicated-cond        # if / else chain has duplicated conditions
  -Wduplicated-branches    # if / else branches have duplicated code
  -Wlogical-op             # logical operations used where bitwise were probably wanted
  -Wuseless-cast           # a cast to the type the value already has
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(LITE_WARNINGS ${LITE_COMMON_WARNINGS})
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(LITE_WARNINGS ${LITE_GCC_WARNINGS})
else()
  message(WARNING "No warning flags for compiler '${CMAKE_CXX_COMPILER_ID}'")
  set(LITE_WARNINGS "")
endif()

if(LITE_WARNINGS_AS_ERRORS)
  list(APPEND LITE_WARNINGS -Werror)
endif()

add_library(lite_warnings INTERFACE)
target_compile_options(lite_warnings INTERFACE ${LITE_WARNINGS})
