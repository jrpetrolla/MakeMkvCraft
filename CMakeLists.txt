cmake_minimum_required(VERSION 3.21)
project(MakeMkvCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt5 (MXE provides Qt 5.15.x for Windows cross-compile)
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# Use pkg-config for media libs when available (Linux native build)
# For MXE cross-compile, pkg-config is provided by the toolchain
find_package(PkgConfig REQUIRED)

pkg_check_modules(BLURAY REQUIRED libbluray)
pkg_check_modules(DVDREAD REQUIRED dvdread)
pkg_check_modules(DVDNAV REQUIRED dvdnav)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE REQUIRED libswscale)

add_executable(MakeMkvCraft WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/DiscInspector.cpp
    src/DiscInspector.h
    src/MkvRemuxer.cpp
    src/MkvRemuxer.h
)

target_include_directories(MakeMkvCraft PRIVATE
    ${BLURAY_INCLUDE_DIRS}
    ${DVDREAD_INCLUDE_DIRS}
    ${DVDNAV_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(MakeMkvCraft PRIVATE
    Qt5::Widgets
    ${BLURAY_LIBRARIES}
    ${DVDREAD_LIBRARIES}
    ${DVDNAV_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
