# 1. Link smoke test: every Qt-free module links into a plain C++ executable without Qt.
studyapp_add_gtest(architecture_tests
    SOURCES
        ModuleLinkTest.cpp
    LIBS
        studyapp::core
        studyapp::document
        studyapp::study
        studyapp::render
        studyapp::canvas
        studyapp::persistence
        studyapp::application
    LABELS architecture)

# 2. Include-boundary scan (tools/check_boundaries.py) over all first-party sources.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME architecture.include_boundaries
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_boundaries.py"
                "${PROJECT_SOURCE_DIR}")
    set_tests_properties(architecture.include_boundaries PROPERTIES LABELS architecture)
else()
    message(WARNING "Python 3 not found: the include-boundary check is not registered with CTest "
                    "(it still runs in CI).")
endif()
