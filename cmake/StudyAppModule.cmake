# Helpers that give every module and test the same shape.
#
# studyapp_add_module(<name>
#     SOURCES      <private .cpp/.hpp files>
#     PUBLIC_DEPS  <targets exposed through the module's public headers>
#     PRIVATE_DEPS <implementation-only targets>)
#
#   Creates static library `studyapp_<name>` with alias `studyapp::<name>`.
#   Public headers:  <module dir>/include/studyapp/<name>/...   (PUBLIC include root)
#   Private sources: <module dir>/src/...                        (PRIVATE include root)
#
#   Because each module has its own include root, a target can only include headers of
#   modules it links. tools/check_boundaries.py verifies the remaining rules.
#
# studyapp_add_gtest(<name> SOURCES ... LIBS ... [LABELS ...])
#   Creates a GoogleTest executable and registers its tests with CTest.

include_guard(GLOBAL)

function(studyapp_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "studyapp_add_module(${name}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "studyapp_add_module(${name}): SOURCES is required")
    endif()

    set(target studyapp_${name})
    add_library(${target} STATIC ${arg_SOURCES})
    add_library(studyapp::${name} ALIAS ${target})

    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target}
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target}
        PUBLIC ${arg_PUBLIC_DEPS}
        PRIVATE ${arg_PRIVATE_DEPS} studyapp_project_options studyapp_warnings)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "modules")
endfunction()

# Qt-specific settings applied to targets that use Qt.
function(studyapp_enable_qt target)
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
    target_compile_definitions(${target} PRIVATE
        QT_NO_KEYWORDS
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
        QT_NO_URL_CAST_FROM_STRING
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
        QT_DISABLE_DEPRECATED_UP_TO=0x060800)
endfunction()

function(studyapp_add_gtest name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBS;LABELS")
    if(NOT arg_LABELS)
        set(arg_LABELS unit)
    endif()

    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE
        ${arg_LIBS} GTest::gtest GTest::gtest_main studyapp_project_options studyapp_warnings)
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES FOLDER "tests")

    # PRE_TEST discovery runs the executable at test time, not at build time, so builds
    # don't depend on runtime DLL paths.
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "${arg_LABELS}")
endfunction()
