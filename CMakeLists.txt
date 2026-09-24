cmake_minimum_required(VERSION 3.24)
project(kfc-runtime VERSION 1.0.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The current engine provider requires Windows x64")
endif()

enable_language(RC)
file(GLOB compatibility_profiles CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/compatibility/profiles/*.json")
if(NOT compatibility_profiles)
    message(FATAL_ERROR "No compatibility profiles found to embed")
endif()

set(profile_resource_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${profile_resource_dir}")
set(profile_resource_rc "${profile_resource_dir}/embedded_compatibility_profiles.rc")
set(profile_resource_header "${profile_resource_dir}/embedded_compatibility_profile_ids.h")
set(profile_rc_content "#include <windows.h>\n")
set(profile_header_content "#pragma once\nnamespace ShroudforgeEmbeddedProfiles {\ninline constexpr int resource_ids[] = {")
set(profile_resource_id 101)
foreach(profile IN LISTS compatibility_profiles)
    file(TO_CMAKE_PATH "${profile}" profile_rc_path)
    string(APPEND profile_rc_content "${profile_resource_id} RCDATA \"${profile_rc_path}\"\n")
    string(APPEND profile_header_content "${profile_resource_id},")
    math(EXPR profile_resource_id "${profile_resource_id} + 1")
endforeach()
string(APPEND profile_header_content "};\n}\n")
file(WRITE "${profile_resource_rc}" "${profile_rc_content}")
file(WRITE "${profile_resource_header}" "${profile_header_content}")
set_source_files_properties("${profile_resource_rc}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${compatibility_profiles}")

add_library(kfc-runtime SHARED
    src/windows/ecs_runtime.cpp src/windows/game_thread_dispatcher.cpp
    src/windows/profile.cpp src/windows/provider.cpp "${profile_resource_rc}")
target_include_directories(kfc-runtime PRIVATE "${profile_resource_dir}")
target_compile_features(kfc-runtime PRIVATE cxx_std_20)
target_compile_definitions(kfc-runtime PRIVATE KFC_RUNTIME_BUILD WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET kfc-runtime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(kfc-runtime PRIVATE /W4 /EHsc)
endif()
install(TARGETS kfc-runtime RUNTIME DESTINATION .)
