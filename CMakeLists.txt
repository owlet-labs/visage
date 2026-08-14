cmake_minimum_required(VERSION 3.17)
set(CMAKE_POLICY_DEFAULT_CMP0177 NEW)

if (APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment version")
endif ()

if (NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif ()

project(visage VERSION 0.1.0)

if (APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif ()

option(VISAGE_AMALGAMATED_BUILD "Compile sources together" ON)
option(VISAGE_ENABLE_WIDGETS "Add widgets library" ON)
option(VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD "Offloads graphics rendering to a background thread" OFF)
option(VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING "Shows graphics debug log in console in debug mode" OFF)
option(VISAGE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)

# THE DIAGNOSTIC CONFIGURATION — the atlas and batch traces compiled ON rather than waiting on an
# environment variable. See `diagnosticBuild()` in visage_graphics/graphics_utils.h for why a fault
# that only reproduces under somebody's hands needs a build that cannot be launched uninstrumented.
#
# Deliberately NOT prefixed VISAGE_: it is named for the downstream project that asks for it, so one
# `-DFEATHERS_DIAGNOSTIC=ON` on a superproject's configure line reaches both this fork and the
# plugin's own diagnostics through a single cache entry.
option(FEATHERS_DIAGNOSTIC "Compile the atlas and batch traces in unconditionally" OFF)

# THE COARSE FIX for an intermediate layer whose contents are not reliably preserved between frames:
# repaint everything, every frame, so there is no unrepainted remainder for undefined contents to
# occupy. Proven by experiment — reproduced twice under a control arm, absent across three cycles
# with this on. Gives up partial damage, which is an optimisation rather than a correctness
# requirement. A compile flag for the same reason the diagnostic build uses one: a fix that a
# launcher can drop is not a fix. See fullRedrawBuild() in visage_graphics/graphics_utils.h.
option(FEATHERS_FULL_REDRAW "Repaint every layer every frame" OFF)

if (VISAGE_ADDRESS_SANITIZER)
  if (MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address)
  else ()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif ()
endif ()

set(VISAGE_APPLICATION_NAME ${CMAKE_PROJECT_NAME} CACHE STRING "Application name used for default window title")

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  option(VISAGE_BUILD_EXAMPLES "Build examples" ON)
  option(VISAGE_BUILD_TESTS "Build tests" ON)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else ()
  option(VISAGE_BUILD_EXAMPLES "Build examples" OFF)
  option(VISAGE_BUILD_TESTS "Build tests" OFF)
endif ()

set(VISAGE_INCLUDE_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include(FetchContent)
include(cmake/compile_flags.cmake)
include(cmake/testing_framework.cmake)

set(VISAGE_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)

function(amalgamate_headers AMALGAMATED_HEADER_NAME FILE_HEADERS)
  set(FILE_CONTENTS "// Auto generated amalgamated header.\n#pragma once\n")
  foreach (FILE ${FILE_HEADERS})
    set(FILE_CONTENTS "${FILE_CONTENTS}#include \"${FILE}\"\n")
  endforeach ()
  file(CONFIGURE OUTPUT "${AMALGAMATED_HEADER_NAME}" CONTENT "${FILE_CONTENTS}")
endfunction()

function(amalgamate_sources GENERATED_FILES AMALGAMATED_SOURCE_BASE FILE_SOURCES)
  set(IDX 0)
  set(PART 0)
  set(GENERATED "")

  foreach (FILE ${FILE_SOURCES})
    set_source_files_properties("${FILE}" PROPERTIES HEADER_FILE_ONLY TRUE)
    if (IDX EQUAL 0)
      if (PART GREATER 0)
        file(CONFIGURE OUTPUT "${AMALGAMATED_SOURCE_BASE}${PART}.cpp" CONTENT "${FILE_CONTENTS}")
      endif ()

      math(EXPR PART "${PART} + 1")
      list(APPEND GENERATED "${AMALGAMATED_SOURCE_BASE}${PART}.cpp")
      set(FILE_CONTENTS "// Auto generated amalgamated source.\n")
    endif ()
    set(FILE_CONTENTS "${FILE_CONTENTS}#include \"${FILE}\"\n")

    math(EXPR IDX "${IDX} + 1")
    if (IDX GREATER 6)
      set(IDX 0)
    endif ()
  endforeach ()

  set(${GENERATED_FILES} "${GENERATED}" PARENT_SCOPE)
  file(CONFIGURE OUTPUT "${AMALGAMATED_SOURCE_BASE}${PART}.cpp" CONTENT "${FILE_CONTENTS}")
endfunction()

add_subdirectory(visage_file_embed)
add_subdirectory(visage_graphics)
add_subdirectory(visage_ui)
add_subdirectory(visage_utils)
add_subdirectory(visage_widgets)
add_subdirectory(visage_windowing)
add_subdirectory(visage_app)

set(DUMMY_FILE ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp)
if (NOT EXISTS ${DUMMY_FILE})
  file(WRITE ${DUMMY_FILE} "")
endif ()

add_library(visage STATIC
  ${DUMMY_FILE}
)

target_include_directories(visage SYSTEM PUBLIC ${VISAGE_INCLUDE_PATH} ${VISAGE_INCLUDE})
target_link_libraries(visage PRIVATE
  VisageApp
  VisageGraphics
  VisageUtils
  VisageUi
  VisageWidgets
  VisageWindowing
)

set_target_properties(visage PROPERTIES FOLDER "visage")

# BOTH TARGETS, and neither one alone is enough. VisageGraphics is where font.cpp and the batcher are
# compiled, so it needs the definition to emit the traces at all; but it is linked into `visage`
# PRIVATELY, so its usage requirements stop there. Consumers — the plugin, and the windowless control
# tools that include graphics_utils.h directly — take the definition from `visage` instead, and they
# have to have it: the gates are inline functions, and a consumer compiling a different body for
# `batchTraceEnabled` than the library did is an ODR violation whose symptom is a trace that is on in
# one translation unit and off in another.
if (FEATHERS_DIAGNOSTIC)
  target_compile_definitions(VisageGraphics PUBLIC FEATHERS_DIAGNOSTIC)
  target_compile_definitions(visage PUBLIC FEATHERS_DIAGNOSTIC)
  message(STATUS "VISAGE: FEATHERS_DIAGNOSTIC build - atlas and batch traces compiled in")
endif ()

if (FEATHERS_FULL_REDRAW)
  target_compile_definitions(VisageGraphics PUBLIC FEATHERS_FULL_REDRAW)
  target_compile_definitions(visage PUBLIC FEATHERS_FULL_REDRAW)
  message(STATUS "VISAGE: FEATHERS_FULL_REDRAW build - every layer repaints every frame")
endif ()

if (VISAGE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()
