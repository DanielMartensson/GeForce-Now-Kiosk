cmake_minimum_required(VERSION 3.16)
project(GeForceNowKiosk VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(IMFN_TARGET "Build for STM32MP257F (DRM/KMS only, no X11/GL)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPEWEBKIT REQUIRED IMPORTED_TARGET wpe-webkit-2.0>=2.52.6)
pkg_check_modules(WPEFDO REQUIRED IMPORTED_TARGET wpebackend-fdo-1.0)
pkg_check_modules(LIBWPE REQUIRED IMPORTED_TARGET wpe-1.0)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

if(NOT IMFN_TARGET)
    pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()

add_executable(geforce-now-kiosk src/main.c)
target_compile_options(geforce-now-kiosk PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(IMFN_TARGET)
    target_compile_definitions(geforce-now-kiosk PRIVATE TARGET)
endif()

target_link_libraries(geforce-now-kiosk PRIVATE
    PkgConfig::WPEWEBKIT
    PkgConfig::WPEFDO
    PkgConfig::LIBWPE
    PkgConfig::XKBCOMMON
    PkgConfig::EGL
    PkgConfig::GBM
    PkgConfig::DRM
)

if(NOT IMFN_TARGET)
    target_link_libraries(geforce-now-kiosk PRIVATE PkgConfig::GLESV2 PkgConfig::X11)
endif()

install(TARGETS geforce-now-kiosk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
