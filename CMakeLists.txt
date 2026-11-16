cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# A build that adds this package as a subproject sets the normal variable
# LOGONOMIC_CLI_ADDON_EMBEDDED. That build owns the cache, try_compile()
# settings, the SDK and testing (the SDK's docs/package_contract.md,
# "Embedded build"), so only a standalone build sets them up here.
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_kimi VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR
            "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

set(KIMI_TOMLPLUSPLUS_SOURCE "" CACHE PATH "Local toml++ source override")
if(KIMI_TOMLPLUSPLUS_SOURCE)
    add_subdirectory("${KIMI_TOMLPLUSPLUS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/tomlplusplus")
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

include(cmake/kimi_observation_dependency.cmake)

function(kimi_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    # The Windows API selection belongs to the platform, so a MinGW build gets
    # the same one as MSVC.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

set(kimi_provider_sources
    src/kimi_accounts.cpp src/kimi_usage.cpp src/kimi_usage_correction.cpp
    src/kimi_acp.cpp src/kimi_config.cpp src/kimi_execution_context.cpp src/kimi_guidance.cpp
    src/kimi_interactions.cpp src/kimi_launcher.cpp src/kimi_observer.cpp src/kimi_preparation.cpp
    src/kimi_quota.cpp src/kimi_quota_direct.cpp src/kimi_resume.cpp
    src/kimi_wire.cpp src/kimi_wire_record.cpp)
function(kimi_add_provider target)
    add_library(${target} STATIC ${kimi_provider_sources})
    kimi_target(${target})
    target_include_directories(${target} PUBLIC src)
    target_link_libraries(${target} PUBLIC logonomic_cli_addon_sdk::sdk
        vnm_framework::vnm_filesystem_observation)
    target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus)
endfunction()
kimi_add_provider(kimi_provider)

add_executable(kimi_adapter src/adapter.cpp)
target_link_libraries(kimi_adapter PRIVATE kimi_provider)
kimi_target(kimi_adapter)
add_executable(kimi_helper src/kimi_helper.cpp)
target_link_libraries(kimi_helper PRIVATE kimi_provider)
kimi_target(kimi_helper)
logonomic_cli_addon_add_launch_bridge(kimi_launch_bridge)
kimi_target(kimi_launch_bridge)
add_executable(kimi_quota_probe src/kimi_quota_probe.cpp src/kimi_quota_http.cpp)
target_link_libraries(kimi_quota_probe PRIVATE kimi_provider)
kimi_target(kimi_quota_probe)
if(WIN32)
    target_link_libraries(kimi_quota_probe PRIVATE ws2_32 winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(kimi_quota_probe PRIVATE CURL::libcurl)
endif()
if(MINGW)
    # The probe's Windows entry point is wmain, which MinGW links only with
    # -municode.
    target_link_options(kimi_quota_probe PRIVATE -municode)
endif()

logonomic_cli_addon_package_platform(KIMI_PLATFORM)
logonomic_cli_addon_package_directory(logonomic.cli.kimi KIMI_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_package_indexer(kimi_package_indexer kimi_index_dependencies)
include(cmake/kimi_package_runtime.cmake)
# Check exact source and package bytes on every build: external runtimes can
# change without a relink. Reuse PE import discovery only while all resolution
# inputs remain unchanged, and leave a complete unchanged package untouched.
add_custom_target(kimi_package ALL
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/assemble-package-$<CONFIG>.cmake"
    BYPRODUCTS "${KIMI_PACKAGE_DIRECTORY}/package.files.json"
    DEPENDS kimi_adapter kimi_helper kimi_launch_bridge kimi_quota_probe logonomic_cli_addon_index
    VERBATIM)
logonomic_cli_addon_install_package("${KIMI_PACKAGE_DIRECTORY}" logonomic.cli.kimi TARGET kimi_package)

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(kimi_model_tests tests/kimi_model_tests.cpp src/kimi_interactions.cpp src/kimi_wire_record.cpp)
target_include_directories(kimi_model_tests PRIVATE src)
kimi_target(kimi_model_tests)
add_test(NAME kimi_model_tests COMMAND kimi_model_tests)
set_tests_properties(kimi_model_tests PROPERTIES TIMEOUT 15)

add_executable(kimi_quota_startup_tests tests/kimi_quota_startup_tests.cpp)
target_link_libraries(kimi_quota_startup_tests PRIVATE kimi_provider)
kimi_target(kimi_quota_startup_tests)
if(WIN32)
    target_link_libraries(kimi_quota_startup_tests PRIVATE ws2_32 psapi)
    add_test(NAME kimi_quota_output_tests COMMAND kimi_quota_startup_tests noise)
endif()
add_dependencies(kimi_quota_startup_tests kimi_quota_probe)
add_test(NAME kimi_quota_startup_tests COMMAND kimi_quota_startup_tests)
option(LOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS
    "Register full-size usage history and capacity regressions" OFF)
kimi_add_provider(kimi_observer_test_provider)
target_compile_definitions(kimi_observer_test_provider PUBLIC KIMI_OBSERVER_TEST_HOOKS)
add_executable(kimi_observer_tests tests/kimi_observer_tests.cpp)
target_link_libraries(kimi_observer_tests PRIVATE kimi_observer_test_provider)
kimi_target(kimi_observer_tests)
add_dependencies(kimi_observer_tests kimi_package)
add_test(NAME kimi_observer_tests COMMAND kimi_observer_tests "${KIMI_PACKAGE_DIRECTORY}")
set_tests_properties(kimi_observer_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_preparation_tests tests/kimi_preparation_tests.cpp)
target_link_libraries(kimi_preparation_tests PRIVATE
    kimi_provider logonomic_cli_addon_sdk::test_support)
kimi_target(kimi_preparation_tests)
add_test(NAME kimi_preparation_tests COMMAND kimi_preparation_tests)
set_tests_properties(kimi_preparation_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_resume_tests tests/kimi_resume_tests.cpp)
target_link_libraries(kimi_resume_tests PRIVATE
    kimi_provider logonomic_cli_addon_sdk::test_support)
kimi_target(kimi_resume_tests)
add_dependencies(kimi_resume_tests kimi_package)
add_test(NAME kimi_resume_tests COMMAND kimi_resume_tests)
add_test(NAME kimi_resume_packaged_tests COMMAND kimi_resume_tests "${KIMI_PACKAGE_DIRECTORY}")
set_tests_properties(kimi_resume_tests kimi_resume_packaged_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_contract_tests tests/kimi_contract_tests.cpp)
target_link_libraries(kimi_contract_tests PRIVATE kimi_provider)
kimi_target(kimi_contract_tests)
add_test(NAME kimi_contract_tests COMMAND kimi_contract_tests)
add_executable(kimi_permission_tests tests/kimi_permission_tests.cpp)
target_link_libraries(kimi_permission_tests PRIVATE kimi_provider tomlplusplus::tomlplusplus)
kimi_target(kimi_permission_tests)
add_dependencies(kimi_permission_tests kimi_package)
add_test(NAME kimi_permission_tests COMMAND kimi_permission_tests "${KIMI_PACKAGE_DIRECTORY}")
set_tests_properties(kimi_permission_tests PROPERTIES TIMEOUT 120)
add_executable(kimi_wire_cancellation_tests tests/kimi_wire_cancellation_tests.cpp)
target_link_libraries(kimi_wire_cancellation_tests PRIVATE kimi_provider)
kimi_target(kimi_wire_cancellation_tests)
add_test(NAME kimi_wire_cancellation_tests COMMAND kimi_wire_cancellation_tests)
set_tests_properties(kimi_wire_cancellation_tests PROPERTIES TIMEOUT 30)
add_executable(kimi_quota_direct_tests tests/kimi_quota_direct_tests.cpp)
target_link_libraries(kimi_quota_direct_tests PRIVATE kimi_provider)
kimi_target(kimi_quota_direct_tests)
add_test(NAME kimi_quota_direct_tests COMMAND kimi_quota_direct_tests)
add_executable(kimi_usage_tests tests/kimi_usage_tests.cpp)
target_link_libraries(kimi_usage_tests PRIVATE kimi_provider)
kimi_target(kimi_usage_tests)
add_test(NAME kimi_usage_tests COMMAND kimi_usage_tests)
if(LOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS)
    add_test(NAME kimi_usage_large_data_tests COMMAND kimi_usage_tests --large-data)
    # The real 100001-file regression includes filesystem setup and teardown.
    set_tests_properties(kimi_usage_large_data_tests PROPERTIES
        TIMEOUT 2400 LABELS "usage;large-data")
endif()
add_executable(kimi_usage_correction_tests tests/kimi_usage_correction_tests.cpp)
target_link_libraries(kimi_usage_correction_tests PRIVATE kimi_provider)
kimi_target(kimi_usage_correction_tests)
add_test(NAME kimi_usage_correction_tests COMMAND kimi_usage_correction_tests)
add_test(NAME kimi_package_validation COMMAND logonomic_cli_addon_validate "${KIMI_PACKAGE_DIRECTORY}" --platform "${KIMI_PLATFORM}")
add_test(NAME kimi_package_conformance COMMAND logonomic_cli_addon_conformance "${KIMI_PACKAGE_DIRECTORY}")
add_test(NAME kimi_package_runtime_layout
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${KIMI_PACKAGE_DIRECTORY}"
        "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_runtime_layout_tests.cmake")
set_tests_properties(kimi_contract_tests kimi_package_validation kimi_package_conformance PROPERTIES TIMEOUT 120)
if(MSVC OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_test(NAME kimi_package_runtime_closure
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure-$<CONFIG>.cmake")
    set_tests_properties(kimi_package_runtime_closure PROPERTIES TIMEOUT 120)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_test(NAME kimi_macos_runtime_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_runtime_tests.py"
            "${KIMI_MACOS_RUNTIME_DEPLOYER}")
    set_tests_properties(kimi_macos_runtime_tests PROPERTIES TIMEOUT 30)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME kimi_linux_runtime_provenance_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_runtime_provenance_tests.py"
            "${KIMI_LINUX_RUNTIME_DEPLOYER}" "${CMAKE_COMMAND}")
    set_tests_properties(kimi_linux_runtime_provenance_tests PROPERTIES TIMEOUT 30)
endif()
set_tests_properties(kimi_usage_tests PROPERTIES TIMEOUT 180)
add_executable(kimi_usage_cache_tests tests/kimi_usage_cache_tests.cpp)
target_link_libraries(kimi_usage_cache_tests PRIVATE
    kimi_provider logonomic_cli_addon_sdk::test_support)
kimi_target(kimi_usage_cache_tests)
add_dependencies(kimi_usage_cache_tests kimi_package)
set(KIMI_USAGE_CACHE_PHASE_TESTS)
foreach(phase IN ITEMS cache temporal refresh capacity owned observation reader)
    set(test_name kimi_usage_cache_tests_${phase})
    add_test(NAME ${test_name}
        COMMAND kimi_usage_cache_tests "${KIMI_PACKAGE_DIRECTORY}" ${phase})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")
    list(APPEND KIMI_USAGE_CACHE_PHASE_TESTS ${test_name})
endforeach()

add_executable(kimi_continuation_tests tests/kimi_continuation_tests.cpp)
target_link_libraries(kimi_continuation_tests PRIVATE kimi_provider)
kimi_target(kimi_continuation_tests)
add_test(NAME kimi_continuation_tests COMMAND kimi_continuation_tests)
set_tests_properties(kimi_continuation_tests PROPERTIES TIMEOUT 120 LABELS "addon;protocol;continuation")

add_executable(kimi_acp_causal_tests tests/kimi_acp_causal_tests.cpp)
target_link_libraries(kimi_acp_causal_tests PRIVATE kimi_provider)
kimi_target(kimi_acp_causal_tests)
add_test(NAME kimi_acp_causal_tests COMMAND kimi_acp_causal_tests)
set_tests_properties(kimi_acp_causal_tests PROPERTIES TIMEOUT 120 LABELS "addon;protocol;causality")
if(WIN32)
    if(TEST kimi_usage_large_data_tests)
        set_tests_properties(kimi_usage_large_data_tests PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    # These executables run from the build tree, outside the assembled package.
    # Clean copied-package qualification supplies its own runtime environment.
    set_tests_properties(kimi_observer_tests kimi_preparation_tests kimi_resume_tests kimi_resume_packaged_tests
        kimi_contract_tests kimi_permission_tests
        kimi_quota_startup_tests kimi_quota_output_tests
        kimi_usage_tests kimi_usage_correction_tests ${KIMI_USAGE_CACHE_PHASE_TESTS} kimi_acp_causal_tests kimi_quota_direct_tests
        kimi_continuation_tests
        kimi_wire_cancellation_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(MSVC)
        get_filename_component(kimi_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_test(NAME kimi_runtime_dependency_cache
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_dependency_cache_tests.py"
                "--module=${CMAKE_CURRENT_SOURCE_DIR}/cmake/kimi_runtime_dependencies.cmake"
                "--cmake=${CMAKE_COMMAND}" "--inspector=${kimi_compiler_directory}/dumpbin.exe"
                "--binary=$<TARGET_FILE:kimi_adapter>" "--qt-runtime=$<TARGET_FILE:Qt6::Core>")
        set_tests_properties(kimi_runtime_dependency_cache PROPERTIES
            TIMEOUT 180 LABELS "cli-addon;delivery;recipe")
    endif()
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    get_filename_component(_membership_toml_source "${kimi_toml_source}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME kimi_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--tomlplusplus-source=${_membership_toml_source}"
            "--framework-source=${kimi_framework_source}"
            "--dispatch-source=${kimi_dispatch_source}"
            "--vnm-cmake-source=${kimi_vnm_cmake_source}"
            "--observation-prefix=${vnm_filesystem_observation_DIR}"
            "--dispatch-prefix=${vnm_qt_dispatch_DIR}"
            "--qtbase-source=${KIMI_QTBASE_SOURCE}"
            "--runtime-notices=${KIMI_RUNTIME_NOTICES_DIRECTORY}"
            "--linux-runtime-material=${KIMI_LINUX_RUNTIME_MATERIAL}"
            "--patchelf=${KIMI_PATCHELF_EXECUTABLE}"
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

add_executable(kimi_error_boundary_tests tests/kimi_error_boundary_tests.cpp)
target_link_libraries(kimi_error_boundary_tests PRIVATE kimi_provider logonomic_cli_addon_sdk::test_support)
kimi_target(kimi_error_boundary_tests)
add_test(NAME kimi_error_boundary_tests COMMAND kimi_error_boundary_tests)
