cmake_minimum_required(VERSION 3.29)
project(GrailTab LANGUAGES CXX RC)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-xwin.cmake -DCMAKE_BUILD_TYPE=Release
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(PluginSDK)

add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/grail_layout.h
	COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/gen_layout.py
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_layout.py ${CMAKE_CURRENT_SOURCE_DIR}/layout_v1.tsv)
add_library(GrailTab SHARED grail_tab.cpp grail_tab.rc grail_layout.h)
target_link_libraries(GrailTab PRIVATE D2RLPlugin::D2RLPlugin)
set_target_properties(GrailTab PROPERTIES OUTPUT_NAME "grail-tab")
target_compile_definitions(GrailTab PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(GrailTab PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W3;/permissive-;/EHsc>")
