# Schema migrations

Numbered SQL migrations for `workspace.db` live in this directory, starting in Phase 3:

```
0001_initial.sql
0002_<short_description>.sql
...
```

Rules (see [docs/DATABASE_SCHEMA.md §8](../../../docs/DATABASE_SCHEMA.md#8-migrations--versioning)):

* `PRAGMA user_version` stores the schema version; migration `NNNN` upgrades from
  version `NNNN - 1` to `NNNN`.
* Each migration runs in its own transaction, after an automatic `VACUUM INTO` backup.
* Files are embedded into the binary at build time by a CMake script (no Qt resources),
  so `studyapp_persistence` stays Qt-free.
* Migrations are never edited after release; fixes are new migrations.
* Every migration gets a test that upgrades a fixture database from the previous version.

No migrations exist yet: the schema is created in Phase 3.
