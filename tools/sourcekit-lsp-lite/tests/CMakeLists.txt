# Unit tests (doctest, no sourcekitd), the reactor test and the LSP
# integration tests (the real server binary, the real sourcekitd, over stdio).

include(FetchContent)
# doctest 2.4.11, pinned by commit. A find_package first, so a machine with
# it installed (or an offline build pointing FETCHCONTENT_SOURCE_DIR_DOCTEST
# at a checkout) needs no network.
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG ae7a13539fb71f270b87eb2e874fbac80bc8dda2 # v2.4.11
  SYSTEM
  FIND_PACKAGE_ARGS 2.4.11)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

# Sanitizer runtime settings for every test. sourcekitd itself is not
# instrumented and keeps process-lifetime caches; the leak suppressions name
# only its libraries, so a leak in this project still fails.
set(LITE_TEST_ENVIRONMENT
  "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:strict_string_checks=1:detect_stack_use_after_return=1"
  "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
  "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")

add_executable(lite_unit_tests
  unit/main.cpp
  unit/json_test.cpp
  unit/text_test.cpp
  unit/framing_test.cpp
  unit/options_test.cpp
  unit/requests_test.cpp
  unit/translate_test.cpp)
target_link_libraries(lite_unit_tests PRIVATE lite_core doctest::doctest)
target_compile_definitions(lite_unit_tests PRIVATE
  LITE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME unit COMMAND lite_unit_tests)

add_executable(lite_reactor_test unit/main.cpp integration/reactor_test.cpp)
target_link_libraries(lite_reactor_test PRIVATE lite_reactor doctest::doctest)
target_compile_definitions(lite_reactor_test PRIVATE
  LITE_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/project")
set_target_properties(lite_reactor_test PROPERTIES BUILD_RPATH "${SOURCEKITD_LIBRARY_DIR}")
add_test(NAME reactor COMMAND lite_reactor_test)

# One ctest test per scenario, so a failure names what failed.
set(LITE_SCENARIOS
  lifecycle diagnostics completion hover definition references highlights
  symbols signature_help incremental_edits cross_file_edits utf16 errors)
foreach(scenario IN LISTS LITE_SCENARIOS)
  add_test(NAME lsp.${scenario}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/integration/lsp_test.py
      --server $<TARGET_FILE:sourcekit-lsp-lite>
      --project ${CMAKE_CURRENT_SOURCE_DIR}/project
      --only ${scenario})
endforeach()

set_tests_properties(unit reactor PROPERTIES ENVIRONMENT "${LITE_TEST_ENVIRONMENT}" TIMEOUT 120)
foreach(scenario IN LISTS LITE_SCENARIOS)
  set_tests_properties(lsp.${scenario} PROPERTIES
    ENVIRONMENT "${LITE_TEST_ENVIRONMENT}" TIMEOUT 180 LABELS integration)
endforeach()
