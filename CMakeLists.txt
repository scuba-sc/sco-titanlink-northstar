cmake_minimum_required(VERSION 3.21)
project(sco_titanlink_northstar VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TitanLink builds with MSVC for x64 only (Titanfall 2 is a 64-bit Windows game)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The sco-core SDK pin, logged at load and in the link's first message.
file(STRINGS "${CMAKE_SOURCE_DIR}/third_party/sco-sdk/SCO_SDK_PIN" _pin_lines REGEX "^[^#]")
list(FILTER _pin_lines EXCLUDE REGEX "^[ \t]*$")
list(GET _pin_lines 0 SCO_SDK_PIN)
string(STRIP "${SCO_SDK_PIN}" SCO_SDK_PIN)
if(NOT SCO_SDK_PIN MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "third_party/sco-sdk/SCO_SDK_PIN: no valid pin")
endif()
message(STATUS "sco-core SDK pin: ${SCO_SDK_PIN}")

# /EHa: a fault caught by the frame guard (plugin.cpp) still unwinds the locks it held.
set(TL_OPTIONS /W4 /WX /permissive- /utf-8 /EHa)
set(TL_DEFS WIN32_LEAN_AND_MEAN NOMINMAX SCO_SDK_PIN="${SCO_SDK_PIN}")

add_library(titanlink_link STATIC src/link.cpp)
target_include_directories(titanlink_link PUBLIC src)
# The vendored headers are sco-core's, byte for byte: their own warnings aren't ours to fix here.
target_include_directories(titanlink_link SYSTEM PUBLIC third_party/sco-sdk)
target_compile_options(titanlink_link PRIVATE ${TL_OPTIONS})
target_compile_definitions(titanlink_link PUBLIC ${TL_DEFS})

add_library(TitanLink SHARED src/plugin.cpp)
target_link_libraries(TitanLink PRIVATE titanlink_link)
target_compile_options(TitanLink PRIVATE ${TL_OPTIONS})

add_executable(link_selftest tests/link_selftest.cpp)
target_link_libraries(link_selftest PRIVATE titanlink_link)
target_compile_options(link_selftest PRIVATE ${TL_OPTIONS})

add_executable(wire_golden tests/wire_golden.cpp)
target_link_libraries(wire_golden PRIVATE titanlink_link)
target_compile_options(wire_golden PRIVATE ${TL_OPTIONS})

enable_testing()
add_test(NAME link_selftest COMMAND link_selftest)
add_test(NAME wire_golden COMMAND wire_golden)
