# The leaf requires Core only. Locate notices from the selected Qt source,
# including copyrights and bundled third-party license texts.
get_target_property(kimi_qmake Qt6::qmake IMPORTED_LOCATION)
if(NOT kimi_qmake)
    get_target_property(kimi_qmake_configs Qt6::qmake IMPORTED_CONFIGURATIONS)
    list(GET kimi_qmake_configs 0 kimi_qmake_config)
    get_target_property(kimi_qmake Qt6::qmake "IMPORTED_LOCATION_${kimi_qmake_config}")
endif()
execute_process(COMMAND "${kimi_qmake}" -query QT_INSTALL_PREFIX
    OUTPUT_VARIABLE kimi_qt_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(KIMI_QTBASE_SOURCE "" CACHE PATH "qtbase source matching the selected Qt runtime (notices)")
if(NOT KIMI_QTBASE_SOURCE AND EXISTS "${kimi_qt_prefix}/../Src/qtbase/LICENSES")
    get_filename_component(KIMI_QTBASE_SOURCE "${kimi_qt_prefix}/../Src/qtbase" ABSOLUTE)
endif()
if(NOT KIMI_QTBASE_SOURCE)
    # This tag identifies license material for the runtime already selected by
    # find_package; it is not a ceiling or a pin on supported Qt releases.
    include(FetchContent)
    FetchContent_Declare(kimi_qt_notices
        GIT_REPOSITORY https://code.qt.io/qt/qtbase.git
        GIT_TAG "v${Qt6Core_VERSION}"
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _kimi_notices_only)
    FetchContent_MakeAvailable(kimi_qt_notices)
    set(KIMI_QTBASE_SOURCE "${kimi_qt_notices_SOURCE_DIR}")
endif()
if(NOT EXISTS "${KIMI_QTBASE_SOURCE}/LICENSES/LGPL-3.0-only.txt")
    message(FATAL_ERROR "Set KIMI_QTBASE_SOURCE to the selected Qt's qtbase source; runtime notices are required.")
endif()
file(STRINGS "${KIMI_QTBASE_SOURCE}/.cmake.conf" kimi_qt_source_version
    REGEX [=[^set\(QT_REPO_MODULE_VERSION ]=])
if(NOT kimi_qt_source_version MATCHES "\"${Qt6Core_VERSION}\"")
    message(FATAL_ERROR "KIMI_QTBASE_SOURCE must describe the selected Qt ${Qt6Core_VERSION} runtime.")
endif()
set(KIMI_RUNTIME_NOTICES_DIRECTORY "" CACHE PATH
    "Copyright/license files for external runtimes, named <library filename>.txt")
file(GLOB_RECURSE kimi_qt_notice_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
    "${KIMI_QTBASE_SOURCE}/LICENSES/*.txt"
    "${KIMI_QTBASE_SOURCE}/src/3rdparty/*LICENSE*"
    "${KIMI_QTBASE_SOURCE}/src/3rdparty/*LICENCE*"
    "${KIMI_QTBASE_SOURCE}/src/3rdparty/*COPYING*"
    "${KIMI_QTBASE_SOURCE}/src/3rdparty/*COPYRIGHT*"
    "${KIMI_QTBASE_SOURCE}/src/3rdparty/*qt_attribution*.json")
file(GLOB kimi_qt_sbom_inputs "${kimi_qt_prefix}/sbom/qtbase-*.spdx")
list(APPEND kimi_qt_notice_inputs ${kimi_qt_sbom_inputs})
set(kimi_runtime_search_dirs "$<TARGET_FILE_DIR:Qt6::Core>")
set(kimi_compiler_runtime_files "")
set(kimi_debug_runtime_files "")
if(MSVC)
    # This module resolves redistributables from the active compiler, not PATH.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    set(kimi_compiler_runtime_files "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
    function(kimi_find_debug_runtime_files output)
        # Debug CRT and UCRT are private debugging/testing inputs, not release
        # redistributables. Keep their discovery settings local to this call.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
        set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
        set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
        include(InstallRequiredSystemLibraries)
        set(${output} "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}" PARENT_SCOPE)
    endfunction()
    kimi_find_debug_runtime_files(kimi_debug_runtime_files)
    foreach(runtime IN LISTS kimi_compiler_runtime_files kimi_debug_runtime_files)
        get_filename_component(runtime_dir "${runtime}" DIRECTORY)
        list(APPEND kimi_runtime_search_dirs "${runtime_dir}")
    endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(KIMI_PATCHELF_EXECUTABLE patchelf REQUIRED)
    foreach(target kimi_adapter kimi_helper kimi_launch_bridge kimi_quota_probe)
        # Keep the build tree runnable and give the package an exactly known
        # rpath to replace. DT_RPATH resolves transitive dependencies too.
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN;$<TARGET_FILE_DIR:Qt6::Core>"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
        target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
    endforeach()
endif()
function(kimi_configure_package_assembler)
    # Cache overrides can retain native Windows separators. Once substituted
    # into quoted CMake source, those separators become escape sequences.
    # Normalize every substituted filesystem path/list in this local scope;
    # preserve list separators and the caller's selected cache/toolchain values.
    if(WIN32)
        foreach(path_variable IN ITEMS
                KIMI_PACKAGE_DIRECTORY CMAKE_CURRENT_SOURCE_DIR CMAKE_CURRENT_BINARY_DIR
                CMAKE_CXX_COMPILER CMAKE_OBJDUMP CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND
                kimi_runtime_search_dirs kimi_compiler_runtime_files kimi_debug_runtime_files
                kimi_qt_notice_inputs
                kimi_toml_source KIMI_QTBASE_SOURCE KIMI_RUNTIME_NOTICES_DIRECTORY
                KIMI_PATCHELF_EXECUTABLE)
            string(REPLACE "\\" "/" ${path_variable} "${${path_variable}}")
        endforeach()
    endif()
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assemble_kimi_package.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/assemble-package.cmake.in" @ONLY)
    if(MSVC)
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_kimi_runtime_closure.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure.cmake.in" @ONLY)
    endif()
endfunction()
kimi_configure_package_assembler()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assemble-package-$<CONFIG>.cmake"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/assemble-package.cmake.in")
if(MSVC)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure.cmake.in")
endif()
