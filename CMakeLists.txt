cmake_minimum_required(VERSION 3.16)

project(fcitx5-koe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)

find_package(Fcitx5Core REQUIRED)

add_subdirectory(addon)
add_subdirectory(daemon)

# Distro packaging: user units, an annotated example config, and the license.
install(FILES packaging/koe-daemon.service packaging/koe-llama-server.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES nix/config.example.toml LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
