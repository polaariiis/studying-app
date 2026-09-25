#include <studyapp/persistence/AssetStore.hpp>

#include "FileIo.hpp"
#include "RowDecoder.hpp"
#include "StoreSupport.hpp"
#include "Utf8Path.hpp"

#include <studyapp/persistence/Transaction.hpp>
#include <studyapp/persistence/WorkspaceLayout.hpp>

#include <array>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;
using detail::utf8;

namespace {

constexpr std::size_t kChunkSize = 64 * 1024;

/// Removes a staged temporary file unless it was moved into place.
class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void release() noexcept { path_.clear(); }

private:
    std::filesystem::path path_;
};

Result<Sha256Digest> hashFile(const std::filesystem::path& file, std::uint64_t& size) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::IoError, "cannot read '" + utf8(file) + "'");
    }
    Sha256 hash;
    std::array<char, kChunkSize> buffer{};
    size = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        hash.update({reinterpret_cast<const std::uint8_t*>(buffer.data()), count});
        size += count;
    }
    if (in.bad()) {
        return makeError(ErrorCode::IoError, "reading '" + utf8(file) + "' failed");
    }
    return hash.finish();
}

Sha256Digest digestFromBlob(std::span<const std::uint8_t> blob) {
    Sha256Digest digest{};
    for (std::size_t i = 0; i < digest.size() && i < blob.size(); ++i) {
        digest[i] = blob[i];
    }
    return digest;
}

} // namespace

std::string_view extensionFor(std::string_view mediaType) noexcept {
    struct Entry {
        std::string_view type;
        std::string_view extension;
    };
    static constexpr std::array<Entry, 8> kTypes{{
        {"application/pdf", "pdf"},
        {"image/png", "png"},
        {"image/jpeg", "jpg"},
        {"image/gif", "gif"},
        {"image/webp", "webp"},
        {"image/bmp", "bmp"},
        {"image/tiff", "tif"},
        {"image/svg+xml", "svg"},
    }};
    for (const Entry& entry : kTypes) {
        if (entry.type == mediaType) {
            return entry.extension;
        }
    }
    return "bin";
}

AssetStore::AssetStore(Database& database, std::filesystem::path workspaceRoot)
    : database_(&database), root_(std::move(workspaceRoot)) {}

std::filesystem::path AssetStore::relativePath(const Sha256Digest& sha256,
                                               std::string_view mediaType) {
    const std::string hex = toHex(sha256);
    return std::filesystem::path("assets") / hex.substr(0, 2) / hex.substr(2, 2) /
           (hex + "." + std::string(extensionFor(mediaType)));
}

Result<core::AssetId> AssetStore::import(const std::filesystem::path& source,
                                         std::string_view mediaType, core::IdGenerator& ids,
                                         const core::Clock& clock) {
    if (mediaType.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an asset needs a media type");
    }
    std::error_code sourceError;
    const auto sourceStatus = std::filesystem::status(source, sourceError);
    if (!std::filesystem::exists(sourceStatus)) {
        return makeError(ErrorCode::NotFound, "cannot read '" + utf8(source) + "'");
    }
    // Only regular files (or symlinks to them): opening a directory with ifstream succeeds on
    // POSIX and would import it as an empty asset.
    if (!std::filesystem::is_regular_file(sourceStatus)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'" + utf8(source) + "' is not a regular file");
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::NotFound, "cannot read '" + utf8(source) + "'");
    }
    const WorkspaceLayout layout{root_};
    std::error_code ec;
    std::filesystem::create_directories(layout.temporary(), ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot create '" + utf8(layout.temporary()) + "': " + ec.message());
    }

    // 1. Copy to temporary/ while hashing, then flush to disk.
    const core::AssetId id = core::AssetId::generate(ids);
    TemporaryFile staged(layout.temporary() / (id.toString() + ".part"));
    auto out = detail::OutputFile::create(staged.path());
    if (!out) {
        return forward(out);
    }
    Sha256 hash;
    std::uint64_t byteSize = 0;
    std::array<char, kChunkSize> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::span<const std::uint8_t> chunk(
            reinterpret_cast<const std::uint8_t*>(buffer.data()),
            static_cast<std::size_t>(in.gcount()));
        hash.update(chunk);
        byteSize += chunk.size();
        if (auto written = out->write(chunk); !written) {
            return forward(written);
        }
    }
    if (in.bad()) {
        return makeError(ErrorCode::IoError, "reading '" + utf8(source) + "' failed");
    }
    if (auto synced = out->syncAndClose(); !synced) {
        return forward(synced);
    }
    const Sha256Digest digest = hash.finish();

    // 2. Dedupe: identical content already imported.
    std::optional<core::AssetId> existing;
    {
        auto statement = database_->cached("SELECT id FROM asset WHERE sha256 = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindBlob(1, digest);
        auto row = (*statement)->step();
        if (!row) {
            return forward(row);
        }
        if (*row) {
            detail::RowDecoder decode(**statement, "asset");
            existing = decode.id<core::AssetId>(0);
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
        }
    }

    // 3. Move into place (atomic rename within the workspace volume). If the final file
    //    already exists (a duplicate, or an orphan left by an interrupted import) its
    //    content is identical by construction, so the staged copy is dropped.
    std::filesystem::path relative;
    if (existing) {
        auto info = find(*existing);
        if (!info) {
            return forward(info);
        }
        relative = relativePath(digest, (*info)->mediaType);
    } else {
        relative = relativePath(digest, mediaType);
    }
    const std::filesystem::path target = root_ / relative;
    if (!std::filesystem::exists(target, ec)) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            return makeError(ErrorCode::IoError,
                             "cannot create '" + utf8(target.parent_path()) + "': " + ec.message());
        }
        std::filesystem::rename(staged.path(), target, ec);
        if (ec) {
            return makeError(ErrorCode::IoError,
                             "cannot move asset into '" + utf8(target) + "': " + ec.message());
        }
        staged.release();
    }
    if (existing) {
        return *existing;
    }

    // 4. Only now, with the file durable in place, insert the row.
    auto transaction = Transaction::begin(*database_, Transaction::Kind::Immediate);
    if (!transaction) {
        return forward(transaction);
    }
    auto insert = database_->cached(
        "INSERT INTO asset (id, sha256, media_type, byte_size, original_name, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!insert) {
        return forward(insert);
    }
    (*insert)
        ->bindId(1, id)
        .bindBlob(2, digest)
        .bindText(3, mediaType)
        .bindInt(4, static_cast<std::int64_t>(byteSize))
        .bindText(5, utf8(source.filename()))
        .bindInt(6, detail::millis(clock.now()));
    if (auto inserted = (*insert)->run(); !inserted) {
        return forward(inserted);
    }
    if (auto committed = transaction->commit(); !committed) {
        return forward(committed);
    }
    return id;
}

Result<std::optional<AssetInfo>> AssetStore::find(core::AssetId id) {
    auto statement = database_->cached(
        "SELECT id, sha256, media_type, byte_size, original_name, created_at FROM asset "
        "WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindId(1, id);
    auto row = (*statement)->step();
    if (!row) {
        return forward(row);
    }
    if (!*row) {
        return std::optional<AssetInfo>{};
    }
    const Statement& s = **statement;
    detail::RowDecoder decode(s, "asset");
    AssetInfo info{.id = decode.id<core::AssetId>(0),
                   .sha256 = {},
                   .mediaType = decode.text(2),
                   .byteSize = static_cast<std::uint64_t>(decode.integer(3)),
                   .originalName = {},
                   .created = decode.timestamp(5)};
    if (s.columnType(1) != ColumnType::Blob || s.columnBlob(1).size() != info.sha256.size()) {
        decode.failRow("sha256 is not a 32-byte BLOB");
    } else {
        info.sha256 = digestFromBlob(s.columnBlob(1));
    }
    if (!s.columnIsNull(4)) {
        info.originalName = decode.text(4);
    }
    if (auto status = decode.status(); !status) {
        return forward(status);
    }
    return std::optional<AssetInfo>(std::move(info));
}

Result<bool> AssetStore::exists(core::AssetId id) {
    auto statement = database_->cached("SELECT 1 FROM asset WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindId(1, id);
    return (*statement)->step();
}

Result<std::filesystem::path> AssetStore::pathOf(core::AssetId id) {
    auto info = find(id);
    if (!info) {
        return forward(info);
    }
    if (!*info) {
        return makeError(ErrorCode::NotFound, "asset " + id.toString() + " does not exist");
    }
    return root_ / relativePath((*info)->sha256, (*info)->mediaType);
}

Result<AssetGcReport> AssetStore::collectGarbage(core::Timestamp now,
                                                 std::chrono::milliseconds grace) {
    AssetGcReport report;

    // 1. Unreferenced, old rows (committed before any file is touched).
    {
        auto transaction = Transaction::begin(*database_, Transaction::Kind::Immediate);
        if (!transaction) {
            return forward(transaction);
        }
        auto statement = database_->cached(
            "DELETE FROM asset WHERE created_at < ?1 "
            "AND NOT EXISTS (SELECT 1 FROM image WHERE image.asset_id = asset.id) "
            "AND NOT EXISTS (SELECT 1 FROM page WHERE page.bg_asset_id = asset.id)");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindInt(1, detail::millis(now - grace));
        if (auto deleted = (*statement)->run(); !deleted) {
            return forward(deleted);
        }
        report.rowsDeleted = static_cast<std::size_t>(database_->changes());
        if (auto committed = transaction->commit(); !committed) {
            return forward(committed);
        }
    }

    // 2. Files that no row names.
    std::set<std::string> expected;
    {
        auto statement = database_->cached("SELECT sha256, media_type FROM asset");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            const Statement& s = **statement;
            expected.insert(
                relativePath(digestFromBlob(s.columnBlob(0)), s.columnText(1)).generic_string());
        }
    }
    const std::filesystem::path assets = WorkspaceLayout{root_}.assets();
    std::error_code ec;
    if (!std::filesystem::exists(assets, ec)) {
        return report;
    }
    std::vector<std::filesystem::path> orphans;
    for (auto it = std::filesystem::recursive_directory_iterator(assets, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) &&
            !expected.contains(std::filesystem::relative(it->path(), root_, ec).generic_string())) {
            orphans.push_back(it->path());
        }
    }
    if (ec) {
        return makeError(ErrorCode::IoError, "scanning '" + utf8(assets) + "': " + ec.message());
    }
    for (const auto& orphan : orphans) {
        if (std::filesystem::remove(orphan, ec)) {
            ++report.filesDeleted;
        }
    }
    return report;
}

Result<std::vector<AssetProblem>> AssetStore::verify() {
    std::vector<AssetProblem> problems;
    std::vector<AssetInfo> assets;
    {
        auto statement = database_->cached("SELECT id FROM asset");
        if (!statement) {
            return forward(statement);
        }
        std::vector<core::AssetId> ids;
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            detail::RowDecoder decode(**statement, "asset");
            ids.push_back(decode.id<core::AssetId>(0));
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
        }
        for (const auto id : ids) {
            auto info = find(id);
            if (!info) {
                return forward(info);
            }
            assets.push_back(std::move(**info));
        }
    }
    for (const AssetInfo& asset : assets) {
        const auto file = root_ / relativePath(asset.sha256, asset.mediaType);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            problems.push_back({AssetProblem::Kind::MissingFile, asset.id, file});
            continue;
        }
        std::uint64_t size = 0;
        auto digest = hashFile(file, size);
        if (!digest) {
            return forward(digest);
        }
        if (size != asset.byteSize) {
            problems.push_back({AssetProblem::Kind::WrongSize, asset.id, file});
        } else if (*digest != asset.sha256) {
            problems.push_back({AssetProblem::Kind::WrongHash, asset.id, file});
        }
    }
    return problems;
}

} // namespace studyapp::persistence
