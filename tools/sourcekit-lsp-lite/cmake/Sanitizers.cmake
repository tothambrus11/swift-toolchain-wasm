# AddressSanitizer (with LeakSanitizer, its default on Linux) and
# UndefinedBehaviorSanitizer, native only: there is no sanitizer runtime for
# wasm32-wasip1.

option(LITE_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(lite_sanitizers INTERFACE)
if(LITE_ENABLE_SANITIZERS)
  set(LITE_SANITIZE_FLAGS
    -fsanitize=address,undefined
    -fno-sanitize-recover=all   # a UB report fails the test, not just prints
    -fno-omit-frame-pointer)
  target_compile_options(lite_sanitizers INTERFACE ${LITE_SANITIZE_FLAGS})
  target_link_options(lite_sanitizers INTERFACE ${LITE_SANITIZE_FLAGS})
endif()
