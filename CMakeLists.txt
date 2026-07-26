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

add_executable(relay
    src/app.c
    src/event.c
    src/font_asset.c
    src/game.c
    src/job_system.c
    src/logger.c
    src/main.c
    src/node.c
    src/node_renderer.c
    src/terminal.c
)

add_dependencies(relay relay_assets)

target_link_libraries(relay PRIVATE Threads::Threads)

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
        tests/game_test.c
        tests/runtime_test.c
        src/event.c
        src/game.c
        src/job_system.c
        src/node.c
        src/node_renderer.c
    )
    target_include_directories(relay_runtime_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(relay_runtime_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(relay_runtime_test PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(relay_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME relay_runtime COMMAND relay_runtime_test)
endif()
