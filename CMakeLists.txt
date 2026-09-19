cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture configure environment" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_kimi VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

set(KIMI_TOMLPLUSPLUS_SOURCE "" CACHE PATH "Local toml++ source override")
if(KIMI_TOMLPLUSPLUS_SOURCE)
    add_subdirectory("${KIMI_TOMLPLUSPLUS_SOURCE}" "${CMAKE_BINARY_DIR}/tomlplusplus")
    set(kimi_toml_source "${KIMI_TOMLPLUSPLUS_SOURCE}")
else()
    include(FetchContent)
    FetchContent_Declare(kimi_toml
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG master
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(kimi_toml)
    set(kimi_toml_source "${kimi_toml_SOURCE_DIR}")
endif()

function(kimi_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(kimi_provider STATIC
    src/kimi_accounts.cpp src/kimi_usage.cpp
    src/kimi_acp.cpp src/kimi_config.cpp src/kimi_launcher.cpp
    src/kimi_observer.cpp src/kimi_preparation.cpp src/kimi_quota.cpp src/kimi_wire.cpp)
kimi_target(kimi_provider)
target_include_directories(kimi_provider PUBLIC src)
target_link_libraries(kimi_provider PUBLIC logonomic_cli_addon_sdk::sdk)
target_link_libraries(kimi_provider PRIVATE tomlplusplus::tomlplusplus)

add_executable(kimi_adapter src/adapter.cpp)
target_link_libraries(kimi_adapter PRIVATE kimi_provider)
kimi_target(kimi_adapter)
add_executable(kimi_helper src/kimi_helper.cpp)
target_link_libraries(kimi_helper PRIVATE kimi_provider)
kimi_target(kimi_helper)
logonomic_cli_addon_add_launch_bridge(kimi_launch_bridge)
kimi_target(kimi_launch_bridge)
add_executable(kimi_quota_probe src/kimi_quota_probe.cpp)
target_link_libraries(kimi_quota_probe PRIVATE kimi_provider)
kimi_target(kimi_quota_probe)
if(WIN32)
    target_link_libraries(kimi_quota_probe PRIVATE ws2_32)
endif()

logonomic_cli_addon_package_platform(KIMI_PLATFORM)
logonomic_cli_addon_package_directory(logonomic.cli.kimi KIMI_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${KIMI_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KIMI_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KIMI_PACKAGE_DIRECTORY}/adapter" "${KIMI_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${KIMI_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${KIMI_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${KIMI_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${KIMI_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy "${kimi_toml_source}/LICENSE" "${KIMI_PACKAGE_DIRECTORY}/notices/tomlplusplus-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${KIMI_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:kimi_adapter>" "${KIMI_PACKAGE_DIRECTORY}/adapter/kimi_adapter${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:kimi_helper>" "${KIMI_PACKAGE_DIRECTORY}/helpers/kimi_helper${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:kimi_launch_bridge>" "${KIMI_PACKAGE_DIRECTORY}/helpers/kimi_launch_bridge${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:kimi_quota_probe>" "${KIMI_PACKAGE_DIRECTORY}/helpers/kimi_quota_probe${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${KIMI_PACKAGE_DIRECTORY}"
        --third-party "assets/kimi.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "assets/kimi-color.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "adapter/kimi_adapter${CMAKE_EXECUTABLE_SUFFIX}"
            "notices/tomlplusplus-LICENSE.txt" "provenance/tomlplusplus.json"
        --third-party "helpers/kimi_helper${CMAKE_EXECUTABLE_SUFFIX}"
            "notices/tomlplusplus-LICENSE.txt" "provenance/tomlplusplus.json"
    DEPENDS kimi_adapter kimi_helper kimi_launch_bridge kimi_quota_probe logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
        "${kimi_toml_source}/LICENSE"
    VERBATIM)
add_custom_target(kimi_package ALL DEPENDS "${KIMI_PACKAGE_DIRECTORY}/package.files.json")

enable_testing()
add_executable(kimi_observer_tests tests/kimi_observer_tests.cpp)
target_link_libraries(kimi_observer_tests PRIVATE kimi_provider)
kimi_target(kimi_observer_tests)
add_test(NAME kimi_observer_tests COMMAND kimi_observer_tests)
set_tests_properties(kimi_observer_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_preparation_tests tests/kimi_preparation_tests.cpp)
target_link_libraries(kimi_preparation_tests PRIVATE kimi_provider)
kimi_target(kimi_preparation_tests)
add_test(NAME kimi_preparation_tests COMMAND kimi_preparation_tests)
set_tests_properties(kimi_preparation_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_contract_tests tests/kimi_contract_tests.cpp)
target_link_libraries(kimi_contract_tests PRIVATE kimi_provider)
kimi_target(kimi_contract_tests)
add_test(NAME kimi_contract_tests COMMAND kimi_contract_tests)
add_executable(kimi_usage_tests tests/kimi_usage_tests.cpp)
target_link_libraries(kimi_usage_tests PRIVATE kimi_provider)
kimi_target(kimi_usage_tests)
add_test(NAME kimi_usage_tests COMMAND kimi_usage_tests)
add_test(NAME kimi_package_validation COMMAND logonomic_cli_addon_validate "${KIMI_PACKAGE_DIRECTORY}" --platform "${KIMI_PLATFORM}")
add_test(NAME kimi_package_conformance COMMAND logonomic_cli_addon_conformance "${KIMI_PACKAGE_DIRECTORY}")
set_tests_properties(kimi_contract_tests kimi_usage_tests kimi_package_validation kimi_package_conformance PROPERTIES TIMEOUT 120)
logonomic_cli_addon_install_package("${KIMI_PACKAGE_DIRECTORY}" logonomic.cli.kimi)

add_executable(kimi_acp_causal_tests tests/kimi_acp_causal_tests.cpp)
target_link_libraries(kimi_acp_causal_tests PRIVATE kimi_provider)
kimi_target(kimi_acp_causal_tests)
add_test(NAME kimi_acp_causal_tests COMMAND kimi_acp_causal_tests)
set_tests_properties(kimi_acp_causal_tests PROPERTIES TIMEOUT 120 LABELS "addon;protocol;causality")

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    get_filename_component(_membership_toml_source "${kimi_toml_source}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME kimi_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--tomlplusplus-source=${_membership_toml_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=kimi_package" "--package-id=logonomic.cli.kimi"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${KIMI_PLATFORM}")
    set_tests_properties(kimi_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
