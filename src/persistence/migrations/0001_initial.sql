-- StudyBoard workspace schema, version 1 (docs/DATABASE_SCHEMA.md §5).
--
-- Migration 0001 creates the schema of a new workspace. The migration runner executes it in
-- one transaction and then sets PRAGMA user_version = 1. Released migrations are never
-- edited; changes are new migrations (see README.md in this directory).
--
-- Database-level settings (application_id, page_size, journal_mode) are applied by the
-- runner before this script, because they cannot be changed inside a transaction.

-- ============================================================ metadata
CREATE TABLE workspace_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;
-- keys: workspace_id, name, created_at, created_by_version, last_written_by_version

CREATE TABLE setting (                 -- workspace-scoped settings only
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL CHECK (json_valid(value))
) WITHOUT ROWID;

-- ============================================================ assets
CREATE TABLE asset (
    id            BLOB PRIMARY KEY,
    sha256        BLOB NOT NULL UNIQUE CHECK (length(sha256) = 32),
    media_type    TEXT NOT NULL,       -- image/png, image/jpeg, application/pdf, …
    byte_size     INTEGER NOT NULL,
    original_name TEXT,
    pixel_width   INTEGER,             -- images
    pixel_height  INTEGER,
    page_count    INTEGER,             -- paged documents
    created_at    INTEGER NOT NULL
);

-- ============================================================ study (referenced by notes)
CREATE TABLE course (
    id          BLOB PRIMARY KEY,
    title       TEXT NOT NULL,
    code        TEXT NOT NULL DEFAULT '',
    term        TEXT NOT NULL DEFAULT '',
    instructor  TEXT NOT NULL DEFAULT '',
    color       INTEGER,
    start_date  TEXT,
    end_date    TEXT,
    sort_key    TEXT NOT NULL,
    archived_at INTEGER,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL
);

-- ============================================================ notes hierarchy
CREATE TABLE notebook (
    id         BLOB PRIMARY KEY,
    title      TEXT NOT NULL,
    color      INTEGER,
    icon       TEXT NOT NULL DEFAULT '',
    course_id  BLOB REFERENCES course(id) ON DELETE SET NULL,
    sort_key   TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    deleted_at INTEGER
);

CREATE TABLE section (
    id          BLOB PRIMARY KEY,
    notebook_id BLOB NOT NULL REFERENCES notebook(id) ON DELETE CASCADE,
    parent_id   BLOB REFERENCES section(id) ON DELETE CASCADE,
    title       TEXT NOT NULL,
    color       INTEGER,
    sort_key    TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    deleted_at  INTEGER
);
CREATE INDEX section_children ON section(notebook_id, parent_id, sort_key);

CREATE TABLE page (
    id                    BLOB PRIMARY KEY,
    section_id            BLOB NOT NULL REFERENCES section(id) ON DELETE CASCADE,
    title                 TEXT NOT NULL DEFAULT '',
    sort_key              TEXT NOT NULL,
    extent                INTEGER NOT NULL CHECK (extent IN (0, 1)), -- 0 infinite, 1 bounded
    width                 REAL,
    height                REAL,
    bg_color              INTEGER NOT NULL DEFAULT 0xFFFFFFFF,
    bg_pattern            INTEGER NOT NULL DEFAULT 0,  -- 0 none, 1 ruled, 2 grid, 3 dots
    bg_spacing            REAL NOT NULL DEFAULT 32,
    bg_asset_id           BLOB REFERENCES asset(id) ON DELETE RESTRICT,
    bg_page_index         INTEGER,
    content_version       INTEGER NOT NULL DEFAULT 0,  -- thumbnail/cache invalidation
    created_at            INTEGER NOT NULL,
    updated_at            INTEGER NOT NULL,
    deleted_at            INTEGER,
    CHECK (extent = 0 OR (width > 0 AND height > 0)),
    CHECK ((bg_asset_id IS NULL) = (bg_page_index IS NULL))
);
CREATE INDEX page_children ON page(section_id, sort_key) WHERE deleted_at IS NULL;
CREATE INDEX page_trash    ON page(deleted_at)           WHERE deleted_at IS NOT NULL;

CREATE TABLE layer (
    id       BLOB PRIMARY KEY,
    page_id  BLOB NOT NULL REFERENCES page(id) ON DELETE CASCADE,
    name     TEXT NOT NULL,
    sort_key TEXT NOT NULL,
    visible  INTEGER NOT NULL DEFAULT 1 CHECK (visible IN (0, 1)),
    locked   INTEGER NOT NULL DEFAULT 0 CHECK (locked IN (0, 1)),
    opacity  REAL NOT NULL DEFAULT 1.0 CHECK (opacity BETWEEN 0 AND 1)
);
CREATE INDEX layer_by_page ON layer(page_id, sort_key);

-- ============================================================ canvas elements
-- Common header in `element`; kind-specific data in one table per kind (1:1).
CREATE TABLE element (
    id         BLOB PRIMARY KEY,
    page_id    BLOB NOT NULL REFERENCES page(id)  ON DELETE CASCADE,
    layer_id   BLOB NOT NULL REFERENCES layer(id) ON DELETE CASCADE,
    kind       INTEGER NOT NULL CHECK (kind BETWEEN 1 AND 5), -- 1 stroke 2 text 3 shape 4 image 5 connector
    z_key      TEXT NOT NULL,
    pos_x      REAL NOT NULL,
    pos_y      REAL NOT NULL,
    rotation   REAL NOT NULL DEFAULT 0,
    scale_x    REAL NOT NULL DEFAULT 1,
    scale_y    REAL NOT NULL DEFAULT 1,
    min_x      REAL NOT NULL,          -- cached world AABB (derived; for spatial queries,
    min_y      REAL NOT NULL,          --  thumbnails and future partial loading)
    max_x      REAL NOT NULL,
    max_y      REAL NOT NULL,
    locked     INTEGER NOT NULL DEFAULT 0 CHECK (locked IN (0, 1)),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX element_by_page ON element(page_id, layer_id, z_key);

CREATE TABLE stroke (
    element_id   BLOB PRIMARY KEY REFERENCES element(id) ON DELETE CASCADE,
    brush        INTEGER NOT NULL,     -- 0 pen 1 pencil 2 highlighter 3 marker
    color        INTEGER NOT NULL,
    base_width   REAL NOT NULL,
    point_count  INTEGER NOT NULL CHECK (point_count > 0),
    point_format INTEGER NOT NULL,     -- codec version (DATA_MODEL.md §8)
    points       BLOB NOT NULL
);

CREATE TABLE text_box (
    element_id BLOB PRIMARY KEY REFERENCES element(id) ON DELETE CASCADE,
    width      REAL NOT NULL,
    height     REAL NOT NULL,
    sizing     INTEGER NOT NULL,       -- 0 auto-width 1 fixed-width 2 fixed
    content    TEXT NOT NULL CHECK (json_valid(content)),
    plain_text TEXT NOT NULL,          -- derived; previews and search
    bg_color   INTEGER,
    padding    REAL NOT NULL DEFAULT 4
);

CREATE TABLE shape (
    element_id    BLOB PRIMARY KEY REFERENCES element(id) ON DELETE CASCADE,
    shape_kind    INTEGER NOT NULL,    -- 0 rect 1 ellipse 2 line 3 triangle 4 polygon 5 polyline
    width         REAL NOT NULL,
    height        REAL NOT NULL,
    stroke_color  INTEGER,
    stroke_width  REAL NOT NULL DEFAULT 2,
    dash          INTEGER NOT NULL DEFAULT 0,
    fill_color    INTEGER,
    corner_radius REAL NOT NULL DEFAULT 0,
    vertex_format INTEGER,
    vertices      BLOB                 -- polygon/polyline only
);

CREATE TABLE image (
    element_id BLOB PRIMARY KEY REFERENCES element(id) ON DELETE CASCADE,
    asset_id   BLOB NOT NULL REFERENCES asset(id) ON DELETE RESTRICT,
    width      REAL NOT NULL,
    height     REAL NOT NULL,
    crop_x     REAL NOT NULL DEFAULT 0,
    crop_y     REAL NOT NULL DEFAULT 0,
    crop_w     REAL NOT NULL DEFAULT 1,
    crop_h     REAL NOT NULL DEFAULT 1,
    opacity    REAL NOT NULL DEFAULT 1
);
CREATE INDEX image_by_asset ON image(asset_id);

CREATE TABLE connector (
    element_id       BLOB PRIMARY KEY REFERENCES element(id) ON DELETE CASCADE,
    start_x          REAL NOT NULL,
    start_y          REAL NOT NULL,
    start_element_id BLOB REFERENCES element(id) ON DELETE SET NULL,
    start_anchor_x   REAL,
    start_anchor_y   REAL,
    start_cap        INTEGER NOT NULL DEFAULT 0,
    end_x            REAL NOT NULL,
    end_y            REAL NOT NULL,
    end_element_id   BLOB REFERENCES element(id) ON DELETE SET NULL,
    end_anchor_x     REAL,
    end_anchor_y     REAL,
    end_cap          INTEGER NOT NULL DEFAULT 1,
    routing          INTEGER NOT NULL DEFAULT 0,
    color            INTEGER NOT NULL,
    width            REAL NOT NULL DEFAULT 2,
    dash             INTEGER NOT NULL DEFAULT 0,
    label            TEXT NOT NULL DEFAULT ''
);
CREATE INDEX connector_by_start ON connector(start_element_id) WHERE start_element_id IS NOT NULL;
CREATE INDEX connector_by_end   ON connector(end_element_id)   WHERE end_element_id   IS NOT NULL;

-- ============================================================ tags
CREATE TABLE tag (
    id         BLOB PRIMARY KEY,
    name       TEXT NOT NULL UNIQUE COLLATE NOCASE,
    color      INTEGER,
    created_at INTEGER NOT NULL
);

CREATE TABLE page_tag (
    page_id BLOB NOT NULL REFERENCES page(id) ON DELETE CASCADE,
    tag_id  BLOB NOT NULL REFERENCES tag(id)  ON DELETE CASCADE,
    PRIMARY KEY (page_id, tag_id)
) WITHOUT ROWID;
CREATE INDEX page_tag_by_tag ON page_tag(tag_id);

-- ============================================================ planning
CREATE TABLE project (
    id           BLOB PRIMARY KEY,
    title        TEXT NOT NULL,
    description  TEXT NOT NULL DEFAULT '',
    status       INTEGER NOT NULL DEFAULT 0, -- 0 planned 1 active 2 on-hold 3 done 4 archived
    course_id    BLOB REFERENCES course(id) ON DELETE SET NULL,
    start_date   TEXT,
    due_date     TEXT,
    color        INTEGER,
    sort_key     TEXT NOT NULL,
    created_at   INTEGER NOT NULL,
    updated_at   INTEGER NOT NULL,
    completed_at INTEGER
);

CREATE TABLE task (
    id               BLOB PRIMARY KEY,
    title            TEXT NOT NULL,
    notes            TEXT NOT NULL DEFAULT '',
    status           INTEGER NOT NULL DEFAULT 0, -- 0 todo 1 in-progress 2 done 3 cancelled
    priority         INTEGER NOT NULL DEFAULT 0,
    course_id        BLOB REFERENCES course(id)  ON DELETE SET NULL,
    project_id       BLOB REFERENCES project(id) ON DELETE SET NULL,
    parent_id        BLOB REFERENCES task(id)    ON DELETE CASCADE,
    due_date         TEXT,                        -- floating YYYY-MM-DD
    due_time_minutes INTEGER,                     -- floating minutes after midnight
    scheduled_start  INTEGER,                     -- UTC ms (time blocking)
    scheduled_end    INTEGER,
    estimate_minutes INTEGER,
    recurrence       TEXT,                        -- RRULE subset (later phase)
    sort_key         TEXT NOT NULL,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL,
    completed_at     INTEGER,
    CHECK (scheduled_start IS NULL OR scheduled_end IS NULL OR scheduled_end >= scheduled_start)
);
CREATE INDEX task_open_by_due ON task(due_date) WHERE status IN (0, 1);
CREATE INDEX task_by_project  ON task(project_id, sort_key);
CREATE INDEX task_by_course   ON task(course_id);
CREATE INDEX task_by_parent   ON task(parent_id, sort_key);
CREATE INDEX task_scheduled   ON task(scheduled_start) WHERE scheduled_start IS NOT NULL;

CREATE TABLE task_tag (
    task_id BLOB NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    tag_id  BLOB NOT NULL REFERENCES tag(id)  ON DELETE CASCADE,
    PRIMARY KEY (task_id, tag_id)
) WITHOUT ROWID;
CREATE INDEX task_tag_by_tag ON task_tag(tag_id);

CREATE TABLE task_page (
    task_id BLOB NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    page_id BLOB NOT NULL REFERENCES page(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, page_id)
) WITHOUT ROWID;
CREATE INDEX task_page_by_page ON task_page(page_id);  -- backlinks from a page

-- ============================================================ search
-- Mapping table gives each searchable object a stable integer rowid for FTS.
CREATE TABLE search_doc (
    rowid      INTEGER PRIMARY KEY,
    owner_kind INTEGER NOT NULL,        -- 1 page title, 2 text box, 3 task, 4 notebook, …
    owner_id   BLOB NOT NULL UNIQUE,
    page_id    BLOB                     -- for jump-to-result
);

CREATE VIRTUAL TABLE search_index USING fts5(
    title,
    body,
    content = '',                       -- contentless: text already lives in source tables
    contentless_delete = 1,             -- SQLite ≥ 3.43
    tokenize = 'unicode61 remove_diacritics 2'
);
