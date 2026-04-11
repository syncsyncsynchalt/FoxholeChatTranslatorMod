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
# Main DLL target (outputs version.dll)
# ============================================================
add_library(version SHARED
    src/dllmain.cpp
    src/scanner.cpp
    src/hooks.cpp
    version.def
)

target_include_directories(version PRIVATE src)
target_link_libraries(version PRIVATE minhook)

target_compile_definitions(version PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# MSVC: ソースファイルをUTF-8として扱う
target_compile_options(version PRIVATE /utf-8)

set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)

# ============================================================
# Post-build: copy DLL to game binary directory
# ============================================================
set(GAME_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../War/Binaries/Win64")
add_custom_command(TARGET version POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:version>"
        "${GAME_BIN_DIR}/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "${GAME_BIN_DIR}/config.ini"
    COMMENT "Copying version.dll and config.ini to ${GAME_BIN_DIR}"
)

# ============================================================
# テスト用ローダー (DLL動作確認)
# ============================================================
add_executable(test_loader test/test_loader.cpp)
target_compile_definitions(test_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(test_loader PRIVATE /utf-8)

# テスト実行時にversion.dllとconfig.iniを同じディレクトリに配置
add_custom_command(TARGET test_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:version>"
        "$<TARGET_FILE_DIR:test_loader>/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "$<TARGET_FILE_DIR:test_loader>/config.ini"
    COMMENT "Copying version.dll and config.ini for test_loader"
)
