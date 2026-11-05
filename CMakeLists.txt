# Turbine: a bytecode VM that runs Flywheel programs. Only a C11 compiler is required.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(turbine VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A Flywheel checkout (github.com/fwlang/flywheel) supplies the library modules and the
# conformance suite; by default turbine is cloned inside it.
set(FLYWHEEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Flywheel checkout")
set(TURBINE_STDLIB_DIR "${FLYWHEEL_DIR}/stdlib/fw" CACHE PATH
    "Directory holding the Flywheel library modules embedded into turbine")
set(TURBINE_LIBRARY_MODULES csv url log args bigint decimal)

# The library modules written in Flywheel are compiled into the executable as text.
add_executable(turbine_embed tools/embed.c)
set(embed_args "")
set(embed_deps "")
foreach(mod IN LISTS TURBINE_LIBRARY_MODULES)
  list(APPEND embed_args "${mod}=${TURBINE_STDLIB_DIR}/${mod}.fw")
  list(APPEND embed_deps "${TURBINE_STDLIB_DIR}/${mod}.fw")
endforeach()
set(embedded "${CMAKE_CURRENT_BINARY_DIR}/stdlib_embed.c")
add_custom_command(
  OUTPUT "${embedded}"
  COMMAND turbine_embed "${embedded}" ${embed_args}
  DEPENDS turbine_embed ${embed_deps}
  COMMENT "Embedding Flywheel library modules"
  VERBATIM)

file(GLOB TURBINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_executable(turbine ${TURBINE_SOURCES} "${embedded}")
target_include_directories(turbine PRIVATE src)

if(MSVC)
  target_compile_options(turbine PRIVATE /W3 /utf-8)
  target_compile_definitions(turbine PRIVATE _CRT_SECURE_NO_WARNINGS)
  # Nested calls from native code into the VM (sort comparators, to_string, ...) use the C
  # stack; Windows defaults to 1 MB.
  target_link_options(turbine PRIVATE /STACK:16777216)
else()
  target_compile_options(turbine PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(turbine PRIVATE m)
  if(MINGW)
    target_link_options(turbine PRIVATE -Wl,--stack,16777216)
  endif()
endif()

install(TARGETS turbine RUNTIME DESTINATION bin)

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME turbine_tests
           COMMAND ${CMAKE_COMMAND} -E env "FLYWHEEL_DIR=${FLYWHEEL_DIR}"
           Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py" $<TARGET_FILE:turbine>
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
