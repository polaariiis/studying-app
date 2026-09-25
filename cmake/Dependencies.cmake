# Third-party dependencies.
#
# Small libraries are fetched at configure time, pinned by version and SHA-256, and
# declared SYSTEM so their headers never trigger our warnings. Qt is an external
# prerequisite located through CMAKE_PREFIX_PATH (see docs/BUILDING.md).
#
# Licenses are recorded in THIRD_PARTY_NOTICES.md; update it when adding a dependency.

include_guard(GLOBAL)
include(FetchContent)

# ---------------------------------------------------------------------------- tl::expected
# Header-only (CC0-1.0). `SOURCE_SUBDIR` points at a directory without a CMakeLists.txt so
# only the headers are used; the project's own CMake (install rules, CTest) is not added.
FetchContent_Declare(tl_expected
    URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.1.0.tar.gz
    URL_HASH SHA256=1db357f46dd2b24447156aaf970c4c40a793ef12a8a9c2ad9e096d9801368df6
    SOURCE_SUBDIR studyapp-headers-only
    SYSTEM)
FetchContent_MakeAvailable(tl_expected)

add_library(studyapp_tl_expected INTERFACE)
add_library(tl::expected ALIAS studyapp_tl_expected)
target_include_directories(studyapp_tl_expected SYSTEM INTERFACE "${tl_expected_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------- SQLite
# Private dependency of studyapp_persistence only.
if(STUDYAPP_USE_SYSTEM_SQLITE)
    find_package(SQLite3 3.43 REQUIRED)
    add_library(studyapp_sqlite3 INTERFACE)
    target_link_libraries(studyapp_sqlite3 INTERFACE SQLite::SQLite3)
else()
    # Public domain amalgamation, compiled with the options documented in
    # docs/DATABASE_SCHEMA.md §2.
    FetchContent_Declare(sqlite_amalgamation
        URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
        URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
        SOURCE_SUBDIR studyapp-no-cmake
        SYSTEM)
    FetchContent_MakeAvailable(sqlite_amalgamation)

    find_package(Threads REQUIRED)

    add_library(studyapp_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(studyapp_sqlite3 SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
    target_compile_definitions(studyapp_sqlite3 PRIVATE
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_RTREE
        SQLITE_DQS=0
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
        SQLITE_LIKE_DOESNT_MATCH_BLOBS
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_SHARED_CACHE
        SQLITE_THREADSAFE=2)
    target_link_libraries(studyapp_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(studyapp_sqlite3 PROPERTIES FOLDER "third_party")
endif()
add_library(studyapp::sqlite3 ALIAS studyapp_sqlite3)

# ---------------------------------------------------------------------------- GoogleTest
if(STUDYAPP_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # match the MSVC /MD runtime

    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        SYSTEM)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)

    foreach(gtest_target IN ITEMS gtest gtest_main gmock gmock_main)
        if(TARGET ${gtest_target})
            set_target_properties(${gtest_target} PROPERTIES FOLDER "third_party")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------- Qt
if(STUDYAPP_BUILD_APP)
    set(_studyapp_qt_components Core Gui Widgets OpenGL)
    if(STUDYAPP_BUILD_TESTS)
        list(APPEND _studyapp_qt_components Test)
    endif()
    find_package(Qt6 6.8 COMPONENTS ${_studyapp_qt_components})
    if(NOT Qt6_FOUND)
        message(FATAL_ERROR
            "Qt 6.8 or newer was not found (components: ${_studyapp_qt_components}).\n"
            "Point CMake at your Qt installation in one of these ways:\n"
            "  * set the QT_ROOT_DIR environment variable, e.g. C:/Qt/6.8.3/msvc2022_64\n"
            "  * create CMakeUserPresets.json from cmake/CMakeUserPresets.example.json\n"
            "  * pass -DCMAKE_PREFIX_PATH=<qt-dir>\n"
            "or build without Qt using the `core-only` preset. See docs/BUILDING.md.")
    endif()
    message(STATUS "Using Qt ${Qt6_VERSION} from ${Qt6_DIR}")
endif()
