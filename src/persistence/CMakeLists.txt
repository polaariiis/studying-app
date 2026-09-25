# SQLite is a PRIVATE dependency: only this module may include <sqlite3.h>.
# Schema, migrations, stores and the asset store: docs/DATABASE_SCHEMA.md.
studyapp_add_module(persistence
    SOURCES
        include/studyapp/persistence/AssetStore.hpp
        include/studyapp/persistence/CatalogStore.hpp
        include/studyapp/persistence/Database.hpp
        include/studyapp/persistence/Migrations.hpp
        include/studyapp/persistence/PageStore.hpp
        include/studyapp/persistence/Sha256.hpp
        include/studyapp/persistence/SqliteLibrary.hpp
        include/studyapp/persistence/StrokeCodec.hpp
        include/studyapp/persistence/Transaction.hpp
        include/studyapp/persistence/WorkspaceFile.hpp
        include/studyapp/persistence/WorkspaceLayout.hpp
        include/studyapp/persistence/WorkspaceStore.hpp
        src/AssetStore.cpp
        src/CatalogStore.cpp
        src/Database.cpp
        src/EmbeddedFiles.hpp
        src/FileIo.cpp
        src/FileIo.hpp
        src/Migrations.cpp
        src/PageStore.cpp
        src/RowDecoder.hpp
        src/Sha256.cpp
        src/SqliteError.hpp
        src/SqliteLibrary.cpp
        src/StoreSupport.hpp
        src/StrokeCodec.cpp
        src/Transaction.cpp
        src/Utf8Path.hpp
        src/WorkspaceFile.cpp
        src/WorkspaceStore.cpp
    PUBLIC_DEPS
        studyapp::document
        studyapp::study
    PRIVATE_DEPS
        studyapp::sqlite3
)

# Schema migrations, embedded at build time (no Qt resources; docs/DATABASE_SCHEMA.md §8).
# Keep the list in version order.
studyapp_embed_files(studyapp_persistence
    FUNCTION studyapp::persistence::detail::migrationFiles
    HEADER EmbeddedFiles.hpp
    FILES
        migrations/0001_initial.sql
)
