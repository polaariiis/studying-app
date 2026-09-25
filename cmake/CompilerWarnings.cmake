# Compiler options shared by all first-party targets.
#
#   studyapp_project_options  language/ABI-level flags (UTF-8 source, conformance mode, ...)
#   studyapp_warnings         warning set; errors when STUDYAPP_WARNINGS_AS_ERRORS is ON
#
# Both are INTERFACE targets linked PRIVATE by every module, so they never leak to
# third-party code. Third-party targets are added as SYSTEM so their headers don't warn.

include_guard(GLOBAL)

add_library(studyapp_project_options INTERFACE)

if(MSVC)
    target_compile_options(studyapp_project_options INTERFACE
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /external:anglebrackets
        /external:W0)
endif()

if(WIN32)
    # Keep <windows.h> (pulled in by some system headers) from defining min/max macros.
    target_compile_definitions(studyapp_project_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_library(studyapp_warnings INTERFACE)

if(MSVC)
    target_compile_options(studyapp_warnings INTERFACE
        /W4
        /w14265 # class has virtual functions, but destructor is not virtual
        /w14555 # expression has no effect
        /w14826 # conversion is sign-extended
        /w14905 # wide string literal cast to LPSTR
        /w14928 # illegal copy-initialization
        $<$<BOOL:${STUDYAPP_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(studyapp_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wcast-align
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmissing-declarations
        $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wlogical-op>
        $<$<BOOL:${STUDYAPP_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
