studyapp_add_gtest(persistence_tests
    SOURCES
        SqliteLibraryTest.cpp
    LIBS
        studyapp::persistence
    LABELS integration)
