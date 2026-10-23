# clang-tidy and cppcheck, run as part of the build on this project's own
# sources (not on doctest's), when enabled.

option(LITE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(LITE_ENABLE_CPPCHECK "Run cppcheck while compiling" OFF)

set(LITE_CLANG_TIDY_COMMAND "")
if(LITE_ENABLE_CLANG_TIDY)
  find_program(LITE_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-19 REQUIRED)
  set(LITE_CLANG_TIDY_COMMAND ${LITE_CLANG_TIDY_EXE} --quiet)
  if(LITE_WARNINGS_AS_ERRORS)
    list(APPEND LITE_CLANG_TIDY_COMMAND --warnings-as-errors=*)
  endif()
endif()

set(LITE_CPPCHECK_COMMAND "")
if(LITE_ENABLE_CPPCHECK)
  find_program(LITE_CPPCHECK_EXE NAMES cppcheck REQUIRED)
  set(LITE_CPPCHECK_COMMAND
    ${LITE_CPPCHECK_EXE}
    --enable=style,performance,warning,portability
    --inline-suppr
    --std=c++20
    --suppress=missingIncludeSystem
    --suppress=unmatchedSuppression
    # clang-tidy already enforces these, with fewer false positives:
    --suppress=passedByValue
    --suppress=useStlAlgorithm
    # json::Value's converting constructors are implicit on purpose (json.h).
    --suppress=noExplicitConstructor:*/json.h
    # cppcheck reports a chained call after `.set(K, std::move(V))` as a use of
    # the moved-from V; clang-tidy's bugprone-use-after-move checks this
    # properly.
    --suppress=accessMoved
    --error-exitcode=2)
endif()

# Applies the enabled analyzers to one of this project's targets.
function(lite_analyze target)
  if(LITE_CLANG_TIDY_COMMAND)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${LITE_CLANG_TIDY_COMMAND}")
  endif()
  if(LITE_CPPCHECK_COMMAND)
    set_target_properties(${target} PROPERTIES CXX_CPPCHECK "${LITE_CPPCHECK_COMMAND}")
  endif()
endfunction()
