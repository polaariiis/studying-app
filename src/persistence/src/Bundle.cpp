#include <studyapp/persistence/Bundle.hpp>

#include "Utf8Path.hpp"

#include <studyapp/persistence/AssetStore.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Migrations.hpp>
#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/persistence/WorkspaceLayout.hpp>
#include <studyapp/persistence/Zip.hpp>

#include <algorithm>
#include <charconv>
#include <string>
#include <tuple>
#include <vector>

namespace studyapp::persistence {

namespace {

using core::ErrorCode;
using core::makeError;

constexpr std::string_view kManifestName = "studyboard-bundle.txt";
constexpr std::string_view kDatabaseName = "workspace.db";
constexpr std::string_view kFormat = "studyboard-bundle";
constexpr int kBundleVersion = 1;
constexpr std::size_t kMaxManifestBytes = 4096;

core::Error invalid(const std::string& what) {
    return {ErrorCode::ParseError, "not a valid StudyBoard bundle: " + what};
}

bool isLowerHex(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// "assets/ab/cd/<64 hex, starting abcd>.<1..8 lower-case letters or digits>"
bool isAssetName(std::string_view name) {
    constexpr std::string_view prefix = "assets/";
    if (!name.starts_with(prefix)) {
        return false;
    }
    name.remove_prefix(prefix.size());
    if (name.size() < 6 + 64 + 2 || name[2] != '/' || name[5] != '/') {
        return false;
    }
    const std::string_view file = name.substr(6);
    const std::string_view hash = file.substr(0, 64);
    const std::string_view extension = file.substr(64);
    if (!isLowerHex(hash) || hash.substr(0, 2) != name.substr(0, 2) ||
        hash.substr(2, 2) != name.substr(3, 2) || extension.size() < 2 || extension.size() > 9 ||
        extension[0] != '.') {
        return false;
    }
    return std::all_of(extension.begin() + 1, extension.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

core::Result<BundleManifest> parseManifest(std::string_view text) {
    BundleManifest manifest;
    bool format = false;
    bool version = false;
    bool kind = false;
    bool schema = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return tl::unexpected(invalid("manifest line"));
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        const auto number = [&](int& out) {
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
            return ec == std::errc{} && ptr == value.data() + value.size();
        };
        if (key == "format") {
            format = value == kFormat;
        } else if (key == "version") {
            int v = 0;
            if (!number(v)) {
                return tl::unexpected(invalid("manifest version"));
            }
            if (v > kBundleVersion) {
                return makeError(ErrorCode::Unsupported,
                                 "the bundle was made by a newer version of StudyBoard");
            }
            version = v == kBundleVersion;
        } else if (key == "kind") {
            kind = value == "workspace" || value == "notebook";
            manifest.kind = value == "notebook" ? BundleKind::Notebook : BundleKind::Workspace;
        } else if (key == "schema") {
            schema = number(manifest.schemaVersion);
        }
        // Unknown keys are ignored (later versions may add information).
    }
    if (!format || !version || !kind || !schema) {
        return tl::unexpected(invalid("manifest"));
    }
    if (manifest.schemaVersion > currentSchemaVersion()) {
        return makeError(ErrorCode::Unsupported,
                         "the bundle was made by a newer version of StudyBoard");
    }
    if (manifest.schemaVersion < 1) {
        return tl::unexpected(invalid("schema version"));
    }
    return manifest;
}

using SchemaRow = std::tuple<std::string, std::string, std::string, std::string>;

core::Result<std::vector<SchemaRow>> schemaOf(Database& database) {
    auto statement =
        database.prepare("SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema "
                         "ORDER BY type, name");
    if (!statement) {
        return tl::unexpected(statement.error());
    }
    std::vector<SchemaRow> rows;
    for (;;) {
        auto more = statement->step();
        if (!more) {
            return tl::unexpected(more.error());
        }
        if (!*more) {
            break;
        }
        rows.emplace_back(
            std::string(statement->columnText(0)), std::string(statement->columnText(1)),
            std::string(statement->columnText(2)), std::string(statement->columnText(3)));
    }
    return rows;
}

/// The bundle's database is exactly a StudyBoard database of `version`.
core::Result<void> checkDatabase(const std::filesystem::path& file, int version,
                                 const std::filesystem::path& root) {
    auto database = Database::open(file, OpenMode::ReadOnly);
    if (!database) {
        return tl::unexpected(invalid("the database cannot be opened"));
    }
    if (auto hardened = database->hardenForUntrustedFile(); !hardened) {
        return tl::unexpected(hardened.error());
    }
    if (database->queryInt("PRAGMA application_id").value_or(0) != kApplicationId ||
        database->queryInt("PRAGMA user_version").value_or(-1) != version) {
        return tl::unexpected(invalid("not a StudyBoard database of the manifest's version"));
    }
    if (database->queryText("PRAGMA integrity_check").value_or("") != "ok") {
        return tl::unexpected(invalid("the database is damaged"));
    }
    auto fresh = Database::openInMemory();
    if (!fresh) {
        return tl::unexpected(fresh.error());
    }
    const auto migrations = builtinMigrations().first(static_cast<std::size_t>(version));
    if (auto migrated = migrate(*fresh, migrations); !migrated) {
        return tl::unexpected(migrated.error());
    }
    auto expected = schemaOf(*fresh);
    auto actual = schemaOf(*database);
    if (!expected || !actual) {
        return tl::unexpected(!expected ? expected.error() : actual.error());
    }
    if (*expected != *actual) {
        return tl::unexpected(invalid("the database schema is not StudyBoard's"));
    }
    AssetStore assets(*database, root);
    auto problems = assets.verify();
    if (!problems) {
        return tl::unexpected(problems.error());
    }
    if (!problems->empty()) {
        return tl::unexpected(invalid("an asset is missing or damaged"));
    }
    return database->close();
}

} // namespace

core::Result<void> writeBundle(const std::filesystem::path& target, BundleKind kind,
                               int schemaVersion, const std::filesystem::path& database,
                               std::span<const BundleAsset> assets) {
    auto zip = ZipWriter::create(target);
    if (!zip) {
        return tl::unexpected(zip.error());
    }
    const std::string manifest = std::string("format=") + std::string(kFormat) +
                                 "\nversion=" + std::to_string(kBundleVersion) + "\nkind=" +
                                 (kind == BundleKind::Notebook ? "notebook" : "workspace") +
                                 "\nschema=" + std::to_string(schemaVersion) + "\n";
    if (auto added = zip->addBytes(
            kManifestName,
            std::span(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
        !added) {
        return added;
    }
    if (auto added = zip->addFile(kDatabaseName, database); !added) {
        return added;
    }
    for (const BundleAsset& asset : assets) {
        const std::string name = asset.relative.generic_string();
        if (!isAssetName(name)) {
            return makeError(ErrorCode::InvalidArgument, "not an asset path: " + name);
        }
        if (auto added = zip->addFile(name, asset.file); !added) {
            return added;
        }
    }
    return zip->finish();
}

core::Result<BundleManifest> extractBundle(const std::filesystem::path& bundle,
                                           const std::filesystem::path& root) {
    if (auto usable = WorkspaceFile::checkCanCreate(root); !usable) {
        return tl::unexpected(usable.error());
    }
    auto zip = ZipReader::open(bundle);
    if (!zip) {
        return tl::unexpected(zip.error());
    }
    const ZipEntry* manifestEntry = nullptr;
    const ZipEntry* databaseEntry = nullptr;
    for (const ZipEntry& entry : zip->entries()) {
        if (entry.name == kManifestName) {
            manifestEntry = &entry;
        } else if (entry.name == kDatabaseName) {
            databaseEntry = &entry;
        } else if (!isAssetName(entry.name)) {
            return tl::unexpected(invalid("unexpected entry '" + entry.name + "'"));
        }
    }
    if (manifestEntry == nullptr || databaseEntry == nullptr) {
        return tl::unexpected(invalid("no manifest or no database"));
    }
    auto manifestBytes = zip->read(*manifestEntry, kMaxManifestBytes);
    if (!manifestBytes) {
        return tl::unexpected(manifestBytes.error());
    }
    auto manifest = parseManifest(std::string_view(
        reinterpret_cast<const char*>(manifestBytes->data()), manifestBytes->size()));
    if (!manifest) {
        return tl::unexpected(manifest.error());
    }

    // From here on, a failure removes what was extracted (root was missing or empty).
    std::error_code ec;
    const bool existed = std::filesystem::exists(root, ec);
    const WorkspaceLayout layout{root};
    struct Cleanup {
        const std::filesystem::path* root;
        bool existed;
        bool keep = false;
        ~Cleanup() {
            if (keep) {
                return;
            }
            std::error_code error;
            if (existed) { // it was empty (checkCanCreate): empty it again
                for (const auto& child : std::filesystem::directory_iterator(*root, error)) {
                    std::filesystem::remove_all(child.path(), error);
                }
            } else {
                std::filesystem::remove_all(*root, error);
            }
        }
    } cleanup{&root, existed};
    std::filesystem::create_directories(layout.assets(), ec);
    std::filesystem::create_directories(layout.backups(), ec);
    std::filesystem::create_directories(layout.temporary(), ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot create '" + detail::utf8(root) + "'");
    }
    for (const ZipEntry& entry : zip->entries()) {
        if (&entry == manifestEntry) {
            continue;
        }
        // The name was validated above: a fixed file or assets/xx/yy/<hash>.<ext>.
        const std::filesystem::path target = root / std::filesystem::path(entry.name);
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "cannot create '" + detail::utf8(target) + "'");
        }
        if (auto extracted = zip->extract(entry, target); !extracted) {
            return tl::unexpected(extracted.error());
        }
    }
    if (auto checked = checkDatabase(layout.database(), manifest->schemaVersion, root); !checked) {
        return tl::unexpected(checked.error());
    }
    cleanup.keep = true;
    return manifest;
}

} // namespace studyapp::persistence
