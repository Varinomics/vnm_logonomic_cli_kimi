# Reuse the embedding build's selected leaf without changing its framework options.
set(KIMI_FRAMEWORK_SOURCE "" CACHE PATH "Local vnm_framework source override")
if(NOT TARGET vnm_framework::vnm_filesystem_observation)
    if(KIMI_FRAMEWORK_SOURCE)
        add_subdirectory("${KIMI_FRAMEWORK_SOURCE}/cpp/filesystem_observation"
            "${CMAKE_CURRENT_BINARY_DIR}/filesystem_observation")
    else()
        find_package(vnm_filesystem_observation CONFIG QUIET)
        if(NOT TARGET vnm_framework::vnm_filesystem_observation)
            include(FetchContent)
            FetchContent_Declare(kimi_framework
                GIT_REPOSITORY https://github.com/Varinomics/vnm_framework.git
                GIT_TAG master
                GIT_SHALLOW TRUE
                SOURCE_SUBDIR cpp/filesystem_observation)
            FetchContent_MakeAvailable(kimi_framework)
        endif()
    endif()
endif()
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)
# Capture actual source selections for the isolated recipe, including an
# embedded target acquired by its parent rather than by this directory.
get_target_property(kimi_observation_imported
    vnm_framework::vnm_filesystem_observation IMPORTED)
if(NOT kimi_observation_imported)
    get_target_property(kimi_observation_source
        vnm_framework::vnm_filesystem_observation SOURCE_DIR)
    get_filename_component(kimi_framework_source "${kimi_observation_source}/../.." ABSOLUTE)
endif()
get_target_property(kimi_dispatch_imported vnm_qt_dispatch::vnm_qt_dispatch IMPORTED)
if(NOT kimi_dispatch_imported)
    get_target_property(kimi_dispatch_source vnm_qt_dispatch::vnm_qt_dispatch SOURCE_DIR)
endif()
include(FetchContent)
FetchContent_GetProperties(vnm_cmake SOURCE_DIR kimi_vnm_cmake_source)
