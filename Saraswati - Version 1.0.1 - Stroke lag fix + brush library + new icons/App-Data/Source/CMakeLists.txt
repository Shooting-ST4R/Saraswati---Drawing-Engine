cmake_minimum_required(VERSION 3.24)

# Static CRT on MSVC so Saraswati.exe is a single self-contained file.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Saraswati VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS ${TP}/SDL/CMakeLists.txt)
  message(FATAL_ERROR "third_party/ is missing: run fetch-deps.sh (Linux) or fetch-deps.ps1 (Windows) first.")
endif()

# ---- SDL3 (static) ----
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/SDL EXCLUDE_FROM_ALL)

# ---- libwebp (decoder only, for WebP import) ----
foreach(opt WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP
            WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${TP}/libwebp EXCLUDE_FROM_ALL)

# ---- glslang (build tool only: compiles our shaders to SPIR-V) ----
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# A prebuilt glslang can be given for cross builds (the shader compiler must run on the host).
set(SARASWATI_GLSLANG "" CACHE FILEPATH "Prebuilt glslang executable (optional, for cross compiling)")
if(SARASWATI_GLSLANG)
  set(GLSLANG_CMD ${SARASWATI_GLSLANG})
  set(GLSLANG_DEP "")
else()
  add_subdirectory(${TP}/glslang EXCLUDE_FROM_ALL)
  set(GLSLANG_CMD glslang-standalone)
  set(GLSLANG_DEP glslang-standalone)
endif()

# ---- shaders -> embedded SPIR-V headers ----
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
file(GLOB SHADER_INCLUDES ${SHADER_DIR}/*.glsl)
set(SHADER_HEADERS "")
# compile_shader(<source file> <variable name> [defines...])
function(compile_shader src name)
  set(out ${SHADER_OUT}/${name}.h)
  set(defs "")
  foreach(d ${ARGN})
    list(APPEND defs "-D${d}")
  endforeach()
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLANG_CMD} -V --target-env vulkan1.1 ${defs} -I${SHADER_DIR} --vn ${name} -o ${out} ${SHADER_DIR}/${src}
    DEPENDS ${SHADER_DIR}/${src} ${SHADER_INCLUDES} ${GLSLANG_DEP}
    COMMENT "glslang ${src} -> ${name}.h"
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

compile_shader(fullscreen.vert  spv_fullscreen_vert)
compile_shader(present.frag     spv_present_frag)
compile_shader(frame.comp       spv_frame_r16)
compile_shader(frame.comp       spv_frame_r32f       MASK_R32F)
compile_shader(dabs.comp        spv_dabs_r16)
compile_shader(dabs.comp        spv_dabs_r32f        MASK_R32F)
compile_shader(commit.comp      spv_commit_r16)
compile_shader(commit.comp      spv_commit_r32f      MASK_R32F)
compile_shader(cache.comp       spv_cache_r16)
compile_shader(cache.comp       spv_cache_r32f       MASK_R32F)
compile_shader(gradient.comp    spv_gradient_r16)
compile_shader(gradient.comp    spv_gradient_r32f    MASK_R32F)
compile_shader(stamp.comp       spv_stamp)
compile_shader(adjust.comp      spv_adjust_r16)
compile_shader(adjust.comp      spv_adjust_r32f      MASK_R32F)
add_custom_command(
  OUTPUT ${SHADER_OUT}/font_ui.h
  COMMAND ${CMAKE_COMMAND} -DIN=${TP}/imgui/misc/fonts/Roboto-Medium.ttf -DOUT=${SHADER_OUT}/font_ui.h -DNAME=kFontUi -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${TP}/imgui/misc/fonts/Roboto-Medium.ttf ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  COMMENT "embedding UI font")
file(GLOB ICON_SVGS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg)
add_custom_command(
  OUTPUT ${SHADER_OUT}/icons_lucide.h
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/icons -DOUT=${SHADER_OUT}/icons_lucide.h -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_icons.cmake
  DEPENDS ${ICON_SVGS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_icons.cmake
  COMMENT "embedding icons")
add_custom_target(saraswati_shaders DEPENDS ${SHADER_HEADERS} ${SHADER_OUT}/font_ui.h ${SHADER_OUT}/icons_lucide.h)

# ---- the app ----
set(IMGUI ${TP}/imgui)
add_executable(Saraswati WIN32
  src/main.cpp
  src/app.cpp
  src/renderer.cpp
  src/renderer_tools.cpp
  src/tools.cpp
  src/shortcuts.cpp
  src/app_shortcuts.cpp
  src/panels.cpp
  src/edit_ops.cpp
  src/text.cpp
  src/text_tool.cpp
  src/bubble.cpp
  src/brush.cpp
  src/vk_util.cpp
  src/png_write.cpp
  src/fileio.cpp
  src/image_impl.cpp
  src/volk_impl.c
  ${IMGUI}/imgui.cpp
  ${IMGUI}/imgui_draw.cpp
  ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/imgui_demo.cpp
  ${IMGUI}/backends/imgui_impl_sdl3.cpp
  ${IMGUI}/backends/imgui_impl_vulkan.cpp)
add_dependencies(Saraswati saraswati_shaders)
set_target_properties(Saraswati PROPERTIES OUTPUT_NAME $<IF:$<PLATFORM_ID:Windows>,Saraswati,saraswati>)
target_include_directories(Saraswati PRIVATE
  src ${SHADER_OUT} ${IMGUI} ${IMGUI}/backends ${TP}/volk ${TP}/Vulkan-Headers/include
  ${TP}/stb ${TP}/nanosvg/src ${TP}/libwebp/src)
target_compile_definitions(Saraswati PRIVATE
  VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  SARASWATI_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_compile_definitions(Saraswati PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(Saraswati PRIVATE SDL3::SDL3-static webpdecoder ${CMAKE_DL_LIBS})
if(MSVC)
  target_compile_options(Saraswati PRIVATE /W3 /utf-8)
else()
  target_compile_options(Saraswati PRIVATE -Wall -Wno-unused-parameter)
endif()
