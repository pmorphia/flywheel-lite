# Flywheel Lite: a bytecode VM that runs Flywheel programs. Only a C11 compiler is required.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(flywheel-lite VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A Flywheel checkout (github.com/fwlang/flywheel) supplies the library modules and the
# conformance suite; by default flywheel-lite is cloned inside it.
set(FLYWHEEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Flywheel checkout")
set(LITE_STDLIB_DIR "${FLYWHEEL_DIR}/stdlib/fw" CACHE PATH
    "Directory holding the Flywheel library modules embedded into flywheel-lite")
set(LITE_LIBRARY_MODULES csv url log args bigint decimal)

# The library modules written in Flywheel are compiled into the executable as text.
add_executable(lite_embed tools/embed.c)
set(embed_args "")
set(embed_deps "")
foreach(mod IN LISTS LITE_LIBRARY_MODULES)
  list(APPEND embed_args "${mod}=${LITE_STDLIB_DIR}/${mod}.fw")
  list(APPEND embed_deps "${LITE_STDLIB_DIR}/${mod}.fw")
endforeach()
set(embedded "${CMAKE_CURRENT_BINARY_DIR}/stdlib_embed.c")
add_custom_command(
  OUTPUT "${embedded}"
  COMMAND lite_embed "${embedded}" ${embed_args}
  DEPENDS lite_embed ${embed_deps}
  COMMENT "Embedding Flywheel library modules"
  VERBATIM)

file(GLOB LITE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_executable(flywheel-lite ${LITE_SOURCES} "${embedded}")
target_include_directories(flywheel-lite PRIVATE src)

if(MSVC)
  target_compile_options(flywheel-lite PRIVATE /W3 /utf-8)
  target_compile_definitions(flywheel-lite PRIVATE _CRT_SECURE_NO_WARNINGS)
  # Nested calls from native code into the VM (sort comparators, to_string, ...) use the C
  # stack; Windows defaults to 1 MB.
  target_link_options(flywheel-lite PRIVATE /STACK:16777216)
else()
  target_compile_options(flywheel-lite PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(flywheel-lite PRIVATE m)
  if(MINGW)
    target_link_options(flywheel-lite PRIVATE -Wl,--stack,16777216)
  endif()
endif()

install(TARGETS flywheel-lite RUNTIME DESTINATION bin)
# fwl is a short alias: a symlink, or on Windows a hard link (a copy where links are unavailable).
if(WIN32)
  install(CODE [[
    set(bin "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin")
    file(REMOVE "${bin}/fwl.exe")
    file(CREATE_LINK "${bin}/flywheel-lite.exe" "${bin}/fwl.exe" COPY_ON_ERROR)
  ]])
else()
  install(CODE [[
    set(bin "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin")
    file(REMOVE "${bin}/fwl")
    file(CREATE_LINK flywheel-lite "${bin}/fwl" SYMBOLIC)
  ]])
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME lite_tests
           COMMAND ${CMAKE_COMMAND} -E env "FLYWHEEL_DIR=${FLYWHEEL_DIR}"
           Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py" $<TARGET_FILE:flywheel-lite>
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
