cmake_minimum_required(VERSION 3.16)

project(chat_zip VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineCore WebEngineWidgets)

add_library(miniz STATIC
    src/third-party/miniz.c
)
target_include_directories(miniz PUBLIC src/third-party)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(chat_zip
    src/archive/patch_archive.cpp
    src/archive/zip_archive.cpp
    src/ui/directory_pane.cpp
    src/ui/main_window.cpp
    src/ui/patch_review_dialog.cpp
    src/main.cpp
)

target_include_directories(chat_zip PRIVATE src)
target_link_libraries(
    chat_zip
    PRIVATE
        Qt6::Widgets
        Qt6::WebEngineCore
        Qt6::WebEngineWidgets
        miniz
)
set_target_properties(chat_zip PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(directory_pane_tests
        tests/directory_pane_tests.cpp
        src/ui/directory_pane.cpp
    )
    target_include_directories(directory_pane_tests PRIVATE src)
    target_link_libraries(directory_pane_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME directory_pane COMMAND directory_pane_tests)

    add_executable(zip_archive_tests
        tests/zip_archive_tests.cpp
        src/archive/zip_archive.cpp
    )
    target_include_directories(zip_archive_tests PRIVATE src)
    target_link_libraries(zip_archive_tests PRIVATE Qt6::Test miniz)
    add_test(NAME zip_archive COMMAND zip_archive_tests)

    add_executable(patch_archive_tests
        tests/patch_archive_tests.cpp
        src/archive/patch_archive.cpp
    )
    target_include_directories(patch_archive_tests PRIVATE src)
    target_link_libraries(patch_archive_tests PRIVATE Qt6::Test miniz)
    add_test(NAME patch_archive COMMAND patch_archive_tests)
endif()
