cmake_minimum_required(VERSION 3.24)

project(relay VERSION 0.1.0 LANGUAGES C)

find_package(Threads REQUIRED)
include(CTest)

set(RELAY_FONT_FILE
    "${PROJECT_SOURCE_DIR}/assets/fonts/DepartureMonoNerdFontMono-Regular.otf")
set(RELAY_GENERATED_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
set(RELAY_FONT_HEADER "${RELAY_GENERATED_DIRECTORY}/departure_mono_font.h")

add_executable(relay_asset_embedder tools/embed_asset.c)

add_custom_command(
    OUTPUT "${RELAY_FONT_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_GENERATED_DIRECTORY}"
    COMMAND "$<TARGET_FILE:relay_asset_embedder>" "${RELAY_FONT_FILE}" "${RELAY_FONT_HEADER}"
    DEPENDS relay_asset_embedder "${RELAY_FONT_FILE}"
    VERBATIM
)

add_custom_target(relay_assets DEPENDS "${RELAY_FONT_HEADER}")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT WIN32 AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Relay supports Windows, macOS, and Linux only.")
endif()

add_library(relay_lua STATIC
    vendors/lua/src/lapi.c
    vendors/lua/src/lauxlib.c
    vendors/lua/src/lbaselib.c
    vendors/lua/src/lcode.c
    vendors/lua/src/lctype.c
    vendors/lua/src/ldebug.c
    vendors/lua/src/ldo.c
    vendors/lua/src/ldump.c
    vendors/lua/src/lfunc.c
    vendors/lua/src/lgc.c
    vendors/lua/src/llex.c
    vendors/lua/src/lmem.c
    vendors/lua/src/lobject.c
    vendors/lua/src/lopcodes.c
    vendors/lua/src/lparser.c
    vendors/lua/src/lstate.c
    vendors/lua/src/lstring.c
    vendors/lua/src/lstrlib.c
    vendors/lua/src/ltable.c
    vendors/lua/src/ltablib.c
    vendors/lua/src/ltm.c
    vendors/lua/src/lundump.c
    vendors/lua/src/lutf8lib.c
    vendors/lua/src/lvm.c
    vendors/lua/src/lzio.c
)

target_include_directories(relay_lua
    PUBLIC
        "${PROJECT_SOURCE_DIR}/vendors/lua/src"
)

set_target_properties(relay_lua PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_compile_definitions(relay_lua PRIVATE
    $<$<CONFIG:Debug>:LUA_USE_APICHECK>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(relay_lua PUBLIC m)
endif()

add_executable(relay
    src/app.c
    src/blueprint.c
    src/event.c
    src/font_asset.c
    src/game.c
    src/job_system.c
    src/logger.c
    src/main.c
    src/node.c
    src/node_renderer.c
    src/script_runtime.c
    src/script_language.c
    src/terminal.c
)

add_dependencies(relay relay_assets)

target_link_libraries(relay PRIVATE Threads::Threads relay_lua)

target_include_directories(relay
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/vendors/termbox2"
        "${RELAY_GENERATED_DIRECTORY}"
)

set_target_properties(relay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin$<0:>"
)

if(MSVC)
    target_compile_options(relay PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(relay PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(UNIX)
    target_sources(relay PRIVATE src/termbox2_impl.c)
    target_compile_definitions(relay PRIVATE _DEFAULT_SOURCE _XOPEN_SOURCE=700)
endif()

if(APPLE)
    target_compile_definitions(relay PRIVATE _DARWIN_C_SOURCE)
endif()

if(NOT MSVC)
    set_source_files_properties(src/termbox2_impl.c PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-function"
    )
endif()

if(BUILD_TESTING)
    add_executable(relay_runtime_test
        src/blueprint.c
        tests/blueprint_test.c
        tests/game_test.c
        tests/runtime_test.c
        src/event.c
        src/game.c
        src/job_system.c
        src/node.c
        src/node_renderer.c
        src/script_runtime.c
        src/script_language.c
        tests/script_runtime_test.c
        tests/script_language_test.c
    )
    target_include_directories(relay_runtime_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(relay_runtime_test PRIVATE Threads::Threads relay_lua)
    if(MSVC)
        target_compile_options(relay_runtime_test PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(relay_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME relay_runtime COMMAND relay_runtime_test)
endif()
