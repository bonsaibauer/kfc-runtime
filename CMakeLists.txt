cmake_minimum_required(VERSION 3.24)
project(kfc-runtime VERSION 2.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The current Enshrouded provider requires Windows x64")
endif()

enable_language(RC)

# Production build profiles and their component data are the only compatibility
# files embedded in the runtime. Function catalogs belong to the dev workflow.
file(GLOB_RECURSE profile_json_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/*.json")
set(runtime_profiles)
set(runtime_component_catalogs)
foreach(profile_file IN LISTS profile_json_files)
    if(profile_file MATCHES "\\.components\\.json$")
        list(APPEND runtime_component_catalogs "${profile_file}")
    else()
        file(READ "${profile_file}" profile_contents)
        string(JSON profile_status ERROR_VARIABLE profile_status_error
            GET "${profile_contents}" provenance status)
        if(profile_status_error STREQUAL "NOTFOUND" AND NOT profile_status STREQUAL "approved")
            message(FATAL_ERROR "Only approved profiles may be embedded: ${profile_file}")
        endif()
        list(APPEND runtime_profiles "${profile_file}")
    endif()
endforeach()
if(NOT runtime_profiles)
    message(FATAL_ERROR "No production profiles found under profiles/")
endif()

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated_dir}")
set(profile_resource_rc "${generated_dir}/embedded_profiles.rc")
set(profile_resource_header "${generated_dir}/embedded_profile_ids.h")
set(profile_rc_content "#include <windows.h>\n")
set(profile_header_content "#pragma once\nnamespace KfcRuntimeEmbedded {\ninline constexpr int profile_resource_ids[] = {")

set(profile_resource_id 101)
foreach(profile IN LISTS runtime_profiles)
    file(TO_CMAKE_PATH "${profile}" profile_rc_path)
    string(APPEND profile_rc_content "${profile_resource_id} RCDATA \"${profile_rc_path}\"\n")
    string(APPEND profile_header_content "${profile_resource_id},")
    math(EXPR profile_resource_id "${profile_resource_id} + 1")
endforeach()
string(APPEND profile_header_content "};\n")

set(component_header_content "inline constexpr int component_resource_ids[] = {")
set(component_name_content "inline constexpr const char* component_resource_names[] = {")
if(runtime_component_catalogs)
    set(component_resource_id 1001)
    foreach(catalog IN LISTS runtime_component_catalogs)
        file(TO_CMAKE_PATH "${catalog}" catalog_rc_path)
        get_filename_component(catalog_name "${catalog}" NAME)
        string(APPEND profile_rc_content "${component_resource_id} RCDATA \"${catalog_rc_path}\"\n")
        string(APPEND component_header_content "${component_resource_id},")
        string(APPEND component_name_content "\"${catalog_name}\",")
        math(EXPR component_resource_id "${component_resource_id} + 1")
    endforeach()
else()
    string(APPEND component_header_content "0,")
    string(APPEND component_name_content "nullptr,")
endif()
string(APPEND profile_header_content "${component_header_content}};\n${component_name_content}};\n}\n")

file(WRITE "${profile_resource_rc}" "${profile_rc_content}")
file(WRITE "${profile_resource_header}" "${profile_header_content}")
set_source_files_properties("${profile_resource_rc}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${runtime_profiles};${runtime_component_catalogs}")

add_library(kfc-runtime SHARED
    src/windows/ecs_runtime.cpp
    src/windows/game_thread_dispatcher.cpp
    src/windows/profile.cpp
    src/windows/provider.cpp
    src/windows/world_runtime.cpp
    src/windows/patch_runtime.cpp
    "${profile_resource_rc}")
target_include_directories(kfc-runtime PRIVATE
    "${generated_dir}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(kfc-runtime PRIVATE cxx_std_20)
target_compile_definitions(kfc-runtime PRIVATE
    KFC_RUNTIME_BUILD WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(kfc-runtime PRIVATE bcrypt)
set_property(TARGET kfc-runtime PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(kfc-runtime PRIVATE /W4 /EHsc)
endif()

# Development tools never run from the game or from the modloader.
add_executable(kfc-runtime-dev dev/tools/dev-console/main.cpp)
target_compile_features(kfc-runtime-dev PRIVATE cxx_std_20)
target_compile_definitions(kfc-runtime-dev PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(kfc-runtime-dev PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(kfc-runtime-dev PRIVATE bcrypt)
if(MSVC)
    target_compile_options(kfc-runtime-dev PRIVATE /W4 /EHsc)
endif()

add_executable(kfc-runtime-capture-ecs dev/tools/dev-console/live-ecs-capture.cpp)
target_compile_features(kfc-runtime-capture-ecs PRIVATE cxx_std_20)
target_compile_definitions(kfc-runtime-capture-ecs PRIVATE NOMINMAX)
if(MSVC)
    target_compile_options(kfc-runtime-capture-ecs PRIVATE /W4 /EHsc)
endif()

# Game-specific profile investigation tools. These ship only in the development package.
foreach(tool inspect-component-metadata live-entity-manager-sample live-entity-managers live-type-references)
    add_executable(kfc-runtime-${tool} dev/tools/enshrouded/native/${tool}.cpp)
    target_compile_features(kfc-runtime-${tool} PRIVATE cxx_std_20)
    set_property(TARGET kfc-runtime-${tool} PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
add_executable(kfc-runtime-audit-live-component-registry
    dev/tools/enshrouded/native/audit-live-component-registry.cpp)
target_compile_features(kfc-runtime-audit-live-component-registry PRIVATE cxx_std_20)
target_include_directories(kfc-runtime-audit-live-component-registry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set_property(TARGET kfc-runtime-audit-live-component-registry PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Runtime package: only the DLL, stable host SDK, API inventory, and license.
install(TARGETS kfc-runtime
    RUNTIME DESTINATION bin COMPONENT Runtime
    ARCHIVE DESTINATION lib COMPONENT Runtime)
install(FILES include/kfc_runtime/runtime.h DESTINATION include/kfc_runtime COMPONENT Runtime)
install(FILES README.md DESTINATION share/kfc-runtime COMPONENT Runtime)
install(FILES sdk/runtime-api.json DESTINATION share/kfc-runtime/sdk COMPONENT Runtime)
install(FILES docs/SDK.md docs/Architecture.md docs/Profiles.md
    DESTINATION share/kfc-runtime/docs COMPONENT Runtime)
install(FILES dev/README.md DESTINATION share/kfc-runtime/dev COMPONENT Runtime)
install(FILES third_party/nlohmann/LICENSE.MIT
    DESTINATION licenses/nlohmann-json COMPONENT Runtime)

# Optional maintainer package: profile sources and the developer-only workflow.
install(TARGETS kfc-runtime-dev kfc-runtime-capture-ecs
    kfc-runtime-inspect-component-metadata
    kfc-runtime-live-entity-manager-sample
    kfc-runtime-live-entity-managers
    kfc-runtime-live-type-references
    kfc-runtime-audit-live-component-registry
    RUNTIME DESTINATION tools COMPONENT Development)
install(DIRECTORY profiles/ DESTINATION share/kfc-runtime/profiles COMPONENT Development)
install(DIRECTORY dev/function-catalogs/ DESTINATION share/kfc-runtime/dev/function-catalogs COMPONENT Development)
install(DIRECTORY dev/schemas/ DESTINATION share/kfc-runtime/dev/schemas COMPONENT Development)
install(FILES README.md DESTINATION share/kfc-runtime COMPONENT Development)
install(FILES include/kfc_runtime/runtime.h DESTINATION include/kfc_runtime COMPONENT Development)
install(FILES sdk/runtime-api.json DESTINATION share/kfc-runtime/sdk COMPONENT Development)
install(FILES dev/README.md DESTINATION share/kfc-runtime/dev COMPONENT Development)
install(FILES docs/SDK.md docs/Architecture.md docs/Profiles.md
    DESTINATION share/kfc-runtime/docs COMPONENT Development)
install(FILES dev/tools/enshrouded/run-live-component-registry-audit.ps1
    DESTINATION share/kfc-runtime/dev/tools/enshrouded COMPONENT Development)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "kfc-runtime")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "kfc-runtime-${PROJECT_VERSION}-windows-x64")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL Runtime Development)
include(CPack)
