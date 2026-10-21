cmake_minimum_required(VERSION 3.13)
project(sweep_tests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

foreach(test_name sweep_tests sweep_windows_macros)
    add_executable(${test_name} sweep_tests.cpp)
    target_compile_features(${test_name} PRIVATE cxx_std_17)
    target_include_directories(${test_name} PRIVATE ../../core/src ../../core/src/imgui)
    target_link_libraries(${test_name} PRIVATE Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
target_compile_definitions(sweep_windows_macros PRIVATE SWEEP_TEST_WINDOWS_MACROS)

add_executable(sweep_gui_tests sweep_gui_tests.cpp
    ../../core/src/imgui/imgui.cpp
    ../../core/src/imgui/imgui_draw.cpp
    ../../core/src/imgui/imgui_tables.cpp
    ../../core/src/imgui/imgui_widgets.cpp)
target_compile_features(sweep_gui_tests PRIVATE cxx_std_17)
target_include_directories(sweep_gui_tests PRIVATE ../../core/src ../../core/src/imgui)
target_link_libraries(sweep_gui_tests PRIVATE Threads::Threads)
add_test(NAME sweep_gui_tests COMMAND sweep_gui_tests)
