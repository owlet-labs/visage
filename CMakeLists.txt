cmake_minimum_required(VERSION 3.17)
set(CMAKE_POLICY_DEFAULT_CMP0177 NEW)

if (APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment version")
endif ()

if (NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 20)
  set(CMAKE_CXX_STANDARD 20)
endif ()

if (NOT DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

project(visage VERSION 0.1.0)

if (APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif ()

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)

option(VISAGE_AMALGAMATED_BUILD "Compile sources together" ON)
option(VISAGE_ENABLE_WIDGETS "Add widgets library" ON)
option(VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING "Shows graphics debug log in console in debug mode" OFF)
option(VISAGE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)
option(VISAGE_LIVE_SHADERS "Link the shader compiler into the app for live shader editing" OFF)

# Feathers (re-port of the fork's two ship options, 2026-11-05). `-DFEATHERS_DIAGNOSTIC=ON` on a
# superproject's configure line reaches the plugin's own diagnostics through this fork's PUBLIC
# definition, exactly as before v1.0.0; v1.0.0 has none of the old bgfx traces, so in this tree it
# gates nothing of visage's own.
option(FEATHERS_DIAGNOSTIC "Propagate the Feathers diagnostic build to every visage consumer" OFF)

# THE COARSE FIX, re-ported onto v1.0.0's compositor: every layer is invalidated every frame, so
# every swap-chain buffer is composited whole and no buffer can present a region it missed. v1.0.0
# composites per-buffer damage (854b313); MEASURED 2026-11-05 that venue-xvfb (lavapipe) cannot
# express a fault in that history — it acquires one image at age 1 every frame — so whether partial
# present is sound on a real GPU swap chain is unmeasured, and the install config keeps this ON until
# it is. See `Canvas::submit`.
option(FEATHERS_FULL_REDRAW "Default Canvas::fullRedraw() ON (repaint every layer every frame; a runtime switch)" OFF)

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
target_compile_features(visage PUBLIC cxx_std_20)
if (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  target_link_options(visage INTERFACE
                      "LINKER:--exclude-libs,$<TARGET_FILE_NAME:freetype>")
elseif (EMSCRIPTEN)
  target_link_options(visage INTERFACE "-sMAX_WEBGL_VERSION=2")
endif ()
target_link_libraries(visage PRIVATE
  VisageApp
  VisageGraphics
  VisageUtils
  VisageUi
  VisageWidgets
  VisageWindowing
)

set_target_properties(visage PROPERTIES FOLDER "visage")

if (FEATHERS_DIAGNOSTIC)
  target_compile_definitions(VisageGraphics PUBLIC FEATHERS_DIAGNOSTIC)
  target_compile_definitions(visage PUBLIC FEATHERS_DIAGNOSTIC)
  message(STATUS "VISAGE: FEATHERS_DIAGNOSTIC build")
endif ()

if (FEATHERS_FULL_REDRAW)
  target_compile_definitions(VisageGraphics PUBLIC FEATHERS_FULL_REDRAW)
  target_compile_definitions(visage PUBLIC FEATHERS_FULL_REDRAW)
  message(STATUS "VISAGE: FEATHERS_FULL_REDRAW build - Canvas::fullRedraw() defaults ON (every layer repaints every frame)")
endif ()

# Feathers: A KNOWN-BAD, AS A BUILD OPTION so a good and a broken build can sit side by side on a
# venue that is not this machine. It deletes the per-swap-chain-buffer damage history (each buffer
# gets only the current frame's damage), which is the fault class FEATHERS_FULL_REDRAW guards against.
# A venue whose swap chain reuses one image (lavapipe/Xvfb, MEASURED 2026-11-05) cannot see it; a real
# GPU must show it RED before its green on the good build counts. NEVER for a shipped build.
option(FEATHERS_KNOWN_BAD_NO_DAMAGE_HISTORY "KNOWN-BAD: drop per-buffer damage history (test venues only)" OFF)
if (FEATHERS_KNOWN_BAD_NO_DAMAGE_HISTORY)
  target_compile_definitions(VisageGraphics PRIVATE FEATHERS_KNOWN_BAD_NO_DAMAGE_HISTORY)
  message(WARNING "VISAGE: KNOWN-BAD build - per-buffer damage history deleted. Not for shipping.")
endif ()

if (VISAGE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()

if (VISAGE_BUILD_TESTS)
  add_subdirectory(manual_tests)
endif ()
