# Application sessions end to end against real SQLite workspaces in temporary directories.
# persistence is linked directly only for fault injection (a second connection).
studyapp_add_gtest(application_tests
    SOURCES
        CanvasSessionTest.cpp
        ComponentVersionsTest.cpp
        WorkspaceSessionTest.cpp
    LIBS
        studyapp::application
        studyapp::persistence
        studyapp::document_test_support
        studyapp::canvas
        studyapp::render_test_support
    LABELS integration)
