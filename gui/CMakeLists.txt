cmake_minimum_required(VERSION 3.21)
project(remclaude_gui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2)

# ── OpenSSL (crypto backend for libssh2) ──────────────────────────────────────
# Desktop: system libssl-dev. Android: prebuilt KDAB android_openssl (Phase B).
if(ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/android/openssl.cmake)
else()
    find_package(OpenSSL 3 REQUIRED)
endif()

# ── libssh2 ───────────────────────────────────────────────────────────────────
include(FetchContent)
set(BUILD_SHARED_LIBS        OFF     CACHE BOOL   "" FORCE)
set(BUILD_STATIC_LIBS        ON      CACHE BOOL   "" FORCE)
set(BUILD_EXAMPLES           OFF     CACHE BOOL   "" FORCE)
set(BUILD_TESTING            OFF     CACHE BOOL   "" FORCE)
set(ENABLE_ZLIB_COMPRESSION  OFF     CACHE BOOL   "" FORCE)
set(CRYPTO_BACKEND           OpenSSL CACHE STRING "" FORCE)
FetchContent_Declare(libssh2
    GIT_REPOSITORY https://github.com/libssh2/libssh2.git
    GIT_TAG        libssh2-1.11.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(libssh2)

# ── Shared sources (used by both the desktop and the Android target) ─────────
set(REMCLAUDE_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/SshWorker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/SshWorker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/RemoteSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/RemoteSession.h
)
set(REMCLAUDE_QML_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/qml/ChatScreen.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/qml/SessionsScreen.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/qml/SettingsScreen.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/qml/Icon.qml
)

# Adds core sources, QML resources, includes and libs to an app target.
function(remclaude_setup_target tgt)
    target_sources(${tgt} PRIVATE ${REMCLAUDE_CORE_SOURCES})
    qt_add_resources(${tgt} "qml_res"
        PREFIX "/"
        BASE   "${PROJECT_SOURCE_DIR}"
        FILES  ${REMCLAUDE_QML_FILES})
    target_include_directories(${tgt} PRIVATE ${PROJECT_SOURCE_DIR}/core)
    target_link_libraries(${tgt} PRIVATE
        libssh2::libssh2_static
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2)
endfunction()

if(ANDROID)
    add_subdirectory(android)
else()
    add_subdirectory(desktop)
endif()
