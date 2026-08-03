cmake_minimum_required(VERSION 3.16)
project(LanDrop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

# Common library
add_library(common STATIC
    common/utils.cpp
)

target_link_libraries(common Qt6::Core Qt6::Network)

# ---------------------------------------------------------------------------
# ftpcore: pure C++ (no Qt) networking/concurrency core — see core/. Native
# sockets, std::thread-based heartbeat, self-contained SHA-256. Nothing in
# this target links against any Qt module; this is the "core logic" half of
# the strict core/UI split — Qt only appears in the GUI executables below.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(ftpcore STATIC
    core/platform/socket.cpp
    core/platform/udp_socket.cpp
    core/heartbeat.cpp
    core/hash/sha256.cpp
    core/ftp/ftp_client.cpp
    core/ftp/ftp_paths.cpp
    core/ftp/ftp_session.cpp
    core/ftp/ftp_server.cpp
    core/ftp/ftp_transfer_manager.cpp
    core/discovery/lan_discovery.cpp
)
target_include_directories(ftpcore PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ftpcore PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(ftpcore PUBLIC ws2_32)
endif()
# Qt's AUTOMOC/AUTORCC/AUTOUIC are ON project-wide (for the GUI targets below);
# explicitly turn them off here so this stays a real, verifiable "no Qt at all"
# target rather than silently getting empty moc stubs generated into it.
set_target_properties(ftpcore PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

# Standalone (no Qt) verification binaries for ftpcore — see core/tests/.
add_executable(socket_smoke_test core/tests/socket_smoke_test.cpp)
target_link_libraries(socket_smoke_test ftpcore)
set_target_properties(socket_smoke_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(ftp_client_parse_test core/tests/ftp_client_parse_test.cpp)
target_link_libraries(ftp_client_parse_test ftpcore)
set_target_properties(ftp_client_parse_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(ftp_client_test core/tests/ftp_client_test.cpp)
target_link_libraries(ftp_client_test ftpcore)
set_target_properties(ftp_client_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(ftp_paths_test core/tests/ftp_paths_test.cpp)
target_link_libraries(ftp_paths_test ftpcore)
set_target_properties(ftp_paths_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(ftp_server_test core/tests/ftp_server_test.cpp)
target_link_libraries(ftp_server_test ftpcore)
set_target_properties(ftp_server_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(ftp_transfer_manager_test core/tests/ftp_transfer_manager_test.cpp)
target_link_libraries(ftp_transfer_manager_test ftpcore)
set_target_properties(ftp_transfer_manager_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

add_executable(lan_discovery_test core/tests/lan_discovery_test.cpp)
target_link_libraries(lan_discovery_test ftpcore)
set_target_properties(lan_discovery_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

# Headless Qt integration test for the Phase G GUI-core wiring — deliberately
# NOT Qt-free (AUTOMOC stays ON, unlike everything else above): it links the
# real client/connection.cpp, client/transfer.cpp, server/ftpserver.cpp
# directly (they have no QWidget dependency) and drives them against a real
# core::FtpServer over 127.0.0.1, continuing this project's established
# "headless Qt console program" verification approach (see CLAUDE.md).
add_executable(gui_core_integration_test
    tests/gui_core_integration_test.cpp
    client/connection.cpp
    client/transfer.cpp
    server/ftpserver.cpp
)
target_link_libraries(gui_core_integration_test common ftpcore Qt6::Core)

# App icon: resources/icons/landrop.png is embedded as a Qt resource and set as
# the runtime window icon (title bar / dock / taskbar) on every platform.
# Additionally: on macOS the .icns is bundled as the .app's Finder/Dock icon; on
# Windows the .ico is compiled in as the .exe's file icon via a resource script.
set(ICON_SOURCES "")
if(APPLE)
    set(APP_ICON_MACOS ${CMAKE_SOURCE_DIR}/resources/icons/LanDrop.icns)
    set_source_files_properties(${APP_ICON_MACOS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND ICON_SOURCES ${APP_ICON_MACOS})
elseif(WIN32)
    list(APPEND ICON_SOURCES ${CMAKE_SOURCE_DIR}/resources/landrop.rc)
endif()

# Server executable
# MACOSX_BUNDLE: produces a proper landrop_server.app (needed for macdeployqt / a real
# release build); WIN32: links the GUI subsystem on Windows so no console window pops
# up next to the Qt window. Both are no-ops on Linux.
add_executable(landrop_server MACOSX_BUNDLE WIN32
    server/main.cpp
    server/ftpserver.cpp
    server/serverwindow.cpp
    resources/landrop.qrc
    ${ICON_SOURCES}
)

target_link_libraries(landrop_server common ftpcore Qt6::Core Qt6::Network Qt6::Widgets)

# Client executable
add_executable(landrop_client MACOSX_BUNDLE WIN32
    client/main.cpp
    client/mainwindow.cpp
    client/connection.cpp
    client/transfer.cpp
    client/filebrowser.cpp
    client/transferwidget.cpp
    client/sitemanager.cpp
    resources/landrop.qrc
    ${ICON_SOURCES}
)

target_link_libraries(landrop_client common ftpcore Qt6::Core Qt6::Network Qt6::Widgets)

if(APPLE)
    set_target_properties(landrop_server landrop_client PROPERTIES
        MACOSX_BUNDLE_ICON_FILE LanDrop.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER com.landrop.app
    )
endif()
