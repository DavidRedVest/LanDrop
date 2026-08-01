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
    common/protocol.cpp
    common/utils.cpp
)

target_link_libraries(common Qt6::Core Qt6::Network)

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
    server/session.cpp
    server/filemanager.cpp
    server/datachannelserver.cpp
    server/serverwindow.cpp
    resources/landrop.qrc
    ${ICON_SOURCES}
)

target_link_libraries(landrop_server common Qt6::Core Qt6::Network Qt6::Widgets)

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

target_link_libraries(landrop_client common Qt6::Core Qt6::Network Qt6::Widgets)

if(APPLE)
    set_target_properties(landrop_server landrop_client PROPERTIES
        MACOSX_BUNDLE_ICON_FILE LanDrop.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER com.landrop.app
    )
endif()
