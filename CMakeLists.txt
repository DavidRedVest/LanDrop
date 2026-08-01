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

# Server executable
add_executable(landrop_server
    server/main.cpp
    server/ftpserver.cpp
    server/session.cpp
    server/filemanager.cpp
    server/datachannelserver.cpp
    server/serverwindow.cpp
)

target_link_libraries(landrop_server common Qt6::Core Qt6::Network Qt6::Widgets)

# Client executable
add_executable(landrop_client
    client/main.cpp
    client/mainwindow.cpp
    client/connection.cpp
    client/transfer.cpp
    client/filebrowser.cpp
    client/transferwidget.cpp
    client/sitemanager.cpp
)

target_link_libraries(landrop_client common Qt6::Core Qt6::Network Qt6::Widgets)
