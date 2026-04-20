cmake_minimum_required(VERSION 3.20)
project(bots C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Allow older cmake_minimum_required in fetched projects (cJSON, etc.)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

include(FetchContent)

# ── raylib ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

# ── raygui (header-only; we just need the source directory) ─────────────────
FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG        4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raygui)

# ── pthreads ────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── executable ──────────────────────────────────────────────────────────────
add_executable(bots
    src/main.c
    src/game_logic.c
    src/rendering.c
    src/start_menu.c
    src/llm_agent.c
    src/message_log.c
    src/http_post.c
    src/cJSON.c
)

target_include_directories(bots PRIVATE
    src
    ${raygui_SOURCE_DIR}/src
)

target_link_libraries(bots PRIVATE
    raylib
    Threads::Threads
    m
)
