cmake_minimum_required(VERSION 3.20)
project(FoxholeChatTranslator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# MinHook - lightweight x64/x86 hooking library
# ============================================================
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
    add_library(minhook STATIC
        ${minhook_SOURCE_DIR}/src/buffer.c
        ${minhook_SOURCE_DIR}/src/hook.c
        ${minhook_SOURCE_DIR}/src/trampoline.c
        ${minhook_SOURCE_DIR}/src/hde/hde32.c
        ${minhook_SOURCE_DIR}/src/hde/hde64.c
    )
    target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
    # Suppress warnings in third-party code
    target_compile_options(minhook PRIVATE /W0)
endif()

# ============================================================
# Dear ImGui - immediate mode GUI for DX11 overlay
# ============================================================
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_compile_options(imgui PRIVATE /W0 /utf-8)
endif()

# ============================================================
# ============================================================
# Loader DLL (version.dll) - プロキシ + ワーカーローダー
# ゲーム起動中は変更不要。安定版。
# ============================================================
add_library(version SHARED
    src/dllmain.cpp
    src/scanner.cpp
    version.def
)

target_include_directories(version PRIVATE src)
target_link_libraries(version PRIVATE minhook d3d11 dxgi)

target_compile_definitions(version PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(version PRIVATE /utf-8)

set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)

# ============================================================
# Worker DLL (chat_translator.dll) - 実ロジック
# F9 キーでホットリロード可能。
# ============================================================
add_library(chat_translator SHARED
    src/worker_main.cpp
    src/hooks.cpp
    src/gnames.cpp
    src/log.cpp
    src/config.cpp
    src/overlay.cpp
)

target_include_directories(chat_translator PRIVATE src)
target_link_libraries(chat_translator PRIVATE imgui d3d11 dxgi winmm)

target_compile_definitions(chat_translator PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(chat_translator PRIVATE /utf-8)

set_target_properties(chat_translator PROPERTIES
    OUTPUT_NAME "chat_translator"
    PREFIX ""
    SUFFIX ".dll"
)

# ============================================================
# Post-build: copy DLLs to game binary directory
# ============================================================
set(GAME_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../War/Binaries/Win64")

# version.dll はゲーム起動前にコピー必要 (ゲーム中は更新不可)
add_custom_command(TARGET version POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:version>"
        "${GAME_BIN_DIR}/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "${GAME_BIN_DIR}/config.ini"
    COMMENT "Copying version.dll and config.ini to ${GAME_BIN_DIR}"
)

# chat_translator.dll はゲーム起動中でもコピー可能 (ホットリロード対応)
add_custom_command(TARGET chat_translator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:chat_translator>"
        "${GAME_BIN_DIR}/chat_translator.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "${GAME_BIN_DIR}/config.ini"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${GAME_BIN_DIR}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/radio_on.wav"
        "${GAME_BIN_DIR}/assets/radio_on.wav"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/radio_off.wav"
        "${GAME_BIN_DIR}/assets/radio_off.wav"
    COMMENT "Copying chat_translator.dll + assets to ${GAME_BIN_DIR} (F9でリロード可能)"
)

# ============================================================
# テスト用ローダー (DLL動作確認)
# ============================================================
add_executable(test_loader test/test_loader.cpp)
target_compile_definitions(test_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(test_loader PRIVATE /utf-8)

add_custom_command(TARGET test_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:version>"
        "$<TARGET_FILE_DIR:test_loader>/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:chat_translator>"
        "$<TARGET_FILE_DIR:test_loader>/chat_translator.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "$<TARGET_FILE_DIR:test_loader>/config.ini"
    COMMENT "Copying DLLs and config.ini for test_loader"
)
