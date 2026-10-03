find_package(Fcitx5Module REQUIRED COMPONENTS Notifications)

add_library(koe MODULE koe.cpp)

target_include_directories(koe PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(koe PRIVATE Fcitx5::Core Fcitx5::Module::Notifications)

install(TARGETS koe LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)

configure_file(koe.conf.in koe.conf @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/koe.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
