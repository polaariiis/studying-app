cmake_minimum_required(VERSION 3.25...3.31)

project(studyapp
    VERSION 0.1.0
    DESCRIPTION "StudyBoard - local-first notes, drawing and study planning"
    HOMEPAGE_URL "https://github.com/polaariiis/studying-app"
    LANGUAGES C CXX # C is needed for the vendored SQLite amalgamation
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Use a preset, e.g. `cmake --preset debug`.")
endif()

set(STUDYAPP_PRODUCT_NAME "StudyBoard")

# ---------------------------------------------------------------------------- options
option(STUDYAPP_BUILD_APP
    "Build the Qt-dependent targets (render_gl, platform, ui, studyapp). OFF = Qt-free modules only"
    ON)
option(STUDYAPP_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(STUDYAPP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(STUDYAPP_ENABLE_PROFILING
    "Compile STUDYAPP_PROFILE_SCOPE timers in (cheap; feeds the canvas debug HUD)" ON)
option(STUDYAPP_BUILD_BENCHMARKS "Build the Google Benchmark micro-benchmarks in bench/" OFF)
option(STUDYAPP_USE_SYSTEM_SQLITE
    "Use the system SQLite (find_package) instead of the pinned amalgamation" OFF)
option(STUDYAPP_DEPLOY_QT_TO_BUILD_TREE
    "Windows: copy the Qt runtime next to the executable after each build so it runs directly"
    ${WIN32})
set(STUDYAPP_SANITIZERS "" CACHE STRING
    "Sanitizers for GCC/Clang builds, e.g. \"address;undefined\" or \"thread\"")

# ---------------------------------------------------------------------------- global settings
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules (see docs/ARCHITECTURE.md §17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CompilerWarnings)
include(Sanitizers)
include(StudyAppModule)
include(Dependencies)
include(EmbedFiles)
include(Deploy)

if(STUDYAPP_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------- targets
add_subdirectory(src)

if(STUDYAPP_BUILD_APP)
    add_subdirectory(app)
endif()

if(STUDYAPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(STUDYAPP_BUILD_BENCHMARKS)
    if(NOT STUDYAPP_BUILD_TESTS)
        message(FATAL_ERROR "STUDYAPP_BUILD_BENCHMARKS requires STUDYAPP_BUILD_TESTS (shared test support)")
    endif()
    add_subdirectory(bench)
endif()

install(FILES LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT runtime)

message(STATUS "StudyBoard ${PROJECT_VERSION}: app=${STUDYAPP_BUILD_APP} tests=${STUDYAPP_BUILD_TESTS} "
               "werror=${STUDYAPP_WARNINGS_AS_ERRORS} sanitizers='${STUDYAPP_SANITIZERS}' "
               "system-sqlite=${STUDYAPP_USE_SYSTEM_SQLITE}")
