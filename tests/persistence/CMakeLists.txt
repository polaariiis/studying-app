# Persistence tests run against real SQLite databases in temporary directories (no mocks;
# docs/TESTING.md §1).
studyapp_add_gtest(persistence_tests
    SOURCES
        AssetStoreTest.cpp
        CodecTest.cpp
        DatabaseTest.cpp
        MigrationsTest.cpp
        SqliteLibraryTest.cpp
        WorkspaceFileTest.cpp
        WorkspaceStoreTest.cpp
    LIBS
        studyapp::persistence
        studyapp::document_test_support
    LABELS integration)
