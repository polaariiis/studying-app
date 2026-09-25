# Headless document-model tests: no Qt, GPU, SQLite, filesystem, network or wall clock.
studyapp_add_gtest(document_tests
    SOURCES
        CommandsTest.cpp
        EditorTest.cpp
        ElementTest.cpp
        PatchTest.cpp
        TestWorkspace.hpp
        WorkspaceTest.cpp
    LIBS
        studyapp::document
        studyapp::test_support
    LABELS unit)
