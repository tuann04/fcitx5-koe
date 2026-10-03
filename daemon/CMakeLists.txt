find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(Threads REQUIRED)

add_executable(koe-daemon
    main.cpp
    config.cpp
    audio.cpp
    transcribe.cpp
)

target_include_directories(koe-daemon PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(koe-daemon PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    PkgConfig::PIPEWIRE
    Threads::Threads
)

install(TARGETS koe-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
