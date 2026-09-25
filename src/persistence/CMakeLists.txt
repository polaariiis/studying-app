# SQLite is a PRIVATE dependency: only this module may include <sqlite3.h>.
# The schema, stores and migrations are implemented in Phase 3 (docs/DATABASE_SCHEMA.md).
studyapp_add_module(persistence
    SOURCES
        include/studyapp/persistence/SqliteLibrary.hpp
        src/SqliteLibrary.cpp
    PUBLIC_DEPS
        studyapp::document
        studyapp::study
    PRIVATE_DEPS
        studyapp::sqlite3
)
