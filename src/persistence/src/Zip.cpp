#include <studyapp/persistence/Zip.hpp>

#include "Utf8Path.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <utility>

namespace studyapp::persistence {

namespace {

using core::ErrorCode;
using core::makeError;

constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndSize = 22;
constexpr std::uint16_t kUtf8Flag = 0x0800;
constexpr std::uint16_t kVersion = 20;                      // 2.0: stored entries, directories
constexpr std::uint16_t kDosDate = (0 << 9) | (1 << 5) | 1; // 1980-01-01: reproducible
constexpr std::uint64_t kMaxZipSize = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kCopyBuffer = 1U << 16;

const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16);
}

std::uint32_t get16(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::uint32_t get32(const std::uint8_t* p) {
    return get16(p) | (get16(p + 2) << 16);
}

core::Error parseError(const std::string& what) {
    return {ErrorCode::ParseError, "not a supported bundle archive: " + what};
}

bool readAt(std::ifstream& in, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.good() || (in.eof() && static_cast<std::size_t>(in.gcount()) == size);
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) noexcept {
    const auto& table = crcTable();
    crc = ~crc;
    for (const std::uint8_t b : bytes) {
        crc = table[(crc ^ b) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

// ---------------------------------------------------------------------------- writer

core::Result<ZipWriter> ZipWriter::create(const std::filesystem::path& target) {
    ZipWriter writer;
    writer.target_ = target;
    writer.partial_ = target;
    writer.partial_ += ".part";
    writer.out_.open(writer.partial_, std::ios::binary | std::ios::trunc);
    if (!writer.out_) {
        return makeError(ErrorCode::IoError,
                         "cannot create '" + detail::utf8(writer.partial_) + "'");
    }
    return writer;
}

ZipWriter::ZipWriter(ZipWriter&& other) noexcept
    : target_(std::move(other.target_)), partial_(std::exchange(other.partial_, {})),
      out_(std::move(other.out_)), entries_(std::move(other.entries_)), finished_(other.finished_) {
    other.finished_ = true; // the moved-from writer owns no file
}

ZipWriter::~ZipWriter() {
    if (!finished_ && !partial_.empty()) {
        out_.close();
        std::error_code ec;
        std::filesystem::remove(partial_, ec);
    }
}

core::Result<void> ZipWriter::beginEntry(std::string_view name, Entry& entry) {
    if (name.empty() || name.size() > 0xFFFF || entries_.size() >= ZipReader::kMaxEntries) {
        return makeError(ErrorCode::InvalidArgument, "invalid zip entry name or too many entries");
    }
    const auto offset = static_cast<std::uint64_t>(out_.tellp());
    if (!out_ || offset > kMaxZipSize) {
        return makeError(ErrorCode::IoError, "the archive is too large (over 4 GiB)");
    }
    entry.name = std::string(name);
    entry.offset = static_cast<std::uint32_t>(offset);
    // The CRC and size are patched in endEntry().
    std::vector<std::uint8_t> header;
    put32(header, kLocalSignature);
    put16(header, kVersion);
    put16(header, kUtf8Flag);
    put16(header, 0); // stored
    put16(header, 0); // time
    put16(header, kDosDate);
    put32(header, 0);
    put32(header, 0);
    put32(header, 0);
    put16(header, static_cast<std::uint32_t>(name.size()));
    put16(header, 0); // extra
    header.insert(header.end(), name.begin(), name.end());
    out_.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    return {};
}

core::Result<void> ZipWriter::endEntry(Entry entry) {
    const auto end = static_cast<std::uint64_t>(out_.tellp());
    if (!out_ || end > kMaxZipSize) {
        return makeError(ErrorCode::IoError, "the archive is too large (over 4 GiB)");
    }
    std::vector<std::uint8_t> fields;
    put32(fields, entry.crc);
    put32(fields, entry.size);
    put32(fields, entry.size);
    out_.seekp(static_cast<std::streamoff>(entry.offset + 14));
    out_.write(reinterpret_cast<const char*>(fields.data()),
               static_cast<std::streamsize>(fields.size()));
    out_.seekp(static_cast<std::streamoff>(end));
    if (!out_) {
        return makeError(ErrorCode::IoError, "writing the archive failed");
    }
    entries_.push_back(std::move(entry));
    return {};
}

core::Result<void> ZipWriter::addBytes(std::string_view name, std::span<const std::uint8_t> bytes) {
    Entry entry;
    if (auto begun = beginEntry(name, entry); !begun) {
        return begun;
    }
    if (bytes.size() > kMaxZipSize) {
        return makeError(ErrorCode::IoError, "the archive is too large (over 4 GiB)");
    }
    out_.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    entry.crc = crc32(bytes);
    entry.size = static_cast<std::uint32_t>(bytes.size());
    return endEntry(std::move(entry));
}

core::Result<void> ZipWriter::addFile(std::string_view name, const std::filesystem::path& source) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::IoError, "cannot read '" + detail::utf8(source) + "'");
    }
    Entry entry;
    if (auto begun = beginEntry(name, entry); !begun) {
        return begun;
    }
    std::vector<std::uint8_t> buffer(kCopyBuffer);
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        size += got;
        if (size > kMaxZipSize) {
            return makeError(ErrorCode::IoError, "the archive is too large (over 4 GiB)");
        }
        crc = crc32(std::span(buffer.data(), got), crc);
        out_.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(got));
    }
    if (in.bad()) {
        return makeError(ErrorCode::IoError, "reading '" + detail::utf8(source) + "' failed");
    }
    entry.crc = crc;
    entry.size = static_cast<std::uint32_t>(size);
    return endEntry(std::move(entry));
}

core::Result<void> ZipWriter::finish() {
    const auto directoryOffset = static_cast<std::uint64_t>(out_.tellp());
    std::vector<std::uint8_t> directory;
    for (const Entry& entry : entries_) {
        put32(directory, kCentralSignature);
        put16(directory, kVersion); // made by (MS-DOS/FAT attributes)
        put16(directory, kVersion);
        put16(directory, kUtf8Flag);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, kDosDate);
        put32(directory, entry.crc);
        put32(directory, entry.size);
        put32(directory, entry.size);
        put16(directory, static_cast<std::uint32_t>(entry.name.size()));
        put16(directory, 0); // extra
        put16(directory, 0); // comment
        put16(directory, 0); // disk
        put16(directory, 0); // internal attributes
        put32(directory, 0); // external attributes
        put32(directory, entry.offset);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
    }
    if (directoryOffset + directory.size() + kEndSize > kMaxZipSize) {
        return makeError(ErrorCode::IoError, "the archive is too large (over 4 GiB)");
    }
    const auto count = static_cast<std::uint32_t>(entries_.size());
    const auto directorySize = static_cast<std::uint32_t>(directory.size());
    put32(directory, kEndSignature);
    put16(directory, 0); // disk
    put16(directory, 0); // disk of the directory
    put16(directory, count);
    put16(directory, count);
    put32(directory, directorySize);
    put32(directory, static_cast<std::uint32_t>(directoryOffset));
    put16(directory, 0); // comment
    out_.write(reinterpret_cast<const char*>(directory.data()),
               static_cast<std::streamsize>(directory.size()));
    out_.close();
    if (!out_) {
        return makeError(ErrorCode::IoError, "writing '" + detail::utf8(partial_) + "' failed");
    }
    std::error_code ec;
    std::filesystem::rename(partial_, target_, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot move the archive to '" +
                                                 detail::utf8(target_) + "': " + ec.message());
    }
    finished_ = true;
    return {};
}

// ---------------------------------------------------------------------------- reader

core::Result<ZipReader> ZipReader::open(const std::filesystem::path& file) {
    ZipReader reader;
    reader.in_.open(file, std::ios::binary);
    if (!reader.in_) {
        return makeError(ErrorCode::IoError, "cannot open '" + detail::utf8(file) + "'");
    }
    reader.in_.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uint64_t>(reader.in_.tellg());
    if (fileSize < kEndSize || fileSize > kMaxZipSize) {
        return tl::unexpected(parseError("size"));
    }
    // The end record is the last 22 bytes (the writer adds no comment; one is tolerated).
    const std::size_t tail =
        static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, kEndSize + 0xFFFF));
    std::vector<std::uint8_t> end(tail);
    if (!readAt(reader.in_, fileSize - tail, end.data(), tail)) {
        return makeError(ErrorCode::IoError, "reading '" + detail::utf8(file) + "' failed");
    }
    std::size_t at = tail - kEndSize + 1;
    bool found = false;
    while (at-- > 0) {
        if (get32(end.data() + at) == kEndSignature &&
            at + kEndSize + get16(end.data() + at + 20) == tail) {
            found = true;
            break;
        }
    }
    if (!found) {
        return tl::unexpected(parseError("no end of central directory"));
    }
    const std::uint8_t* e = end.data() + at;
    const std::uint32_t count = get16(e + 10);
    const std::uint32_t directorySize = get32(e + 12);
    const std::uint32_t directoryOffset = get32(e + 16);
    if (get16(e + 4) != 0 || get16(e + 6) != 0 || get16(e + 8) != count || count > kMaxEntries ||
        static_cast<std::uint64_t>(directoryOffset) + directorySize > fileSize - tail + at) {
        return tl::unexpected(parseError("central directory"));
    }
    std::vector<std::uint8_t> directory(directorySize);
    if (!readAt(reader.in_, directoryOffset, directory.data(), directory.size())) {
        return makeError(ErrorCode::IoError, "reading '" + detail::utf8(file) + "' failed");
    }
    std::unordered_set<std::string> names;
    std::uint64_t total = 0;
    std::size_t pos = 0;
    reader.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + kCentralHeaderSize > directory.size() ||
            get32(directory.data() + pos) != kCentralSignature) {
            return tl::unexpected(parseError("central directory entry"));
        }
        const std::uint8_t* c = directory.data() + pos;
        const std::uint32_t flags = get16(c + 8);
        const std::uint32_t method = get16(c + 10);
        const std::uint32_t crc = get32(c + 16);
        const std::uint32_t compressed = get32(c + 20);
        const std::uint32_t size = get32(c + 24);
        const std::uint32_t nameLength = get16(c + 28);
        const std::uint32_t extraLength = get16(c + 30);
        const std::uint32_t commentLength = get16(c + 32);
        const std::uint32_t localOffset = get32(c + 42);
        const std::size_t next =
            pos + kCentralHeaderSize + nameLength + extraLength + commentLength;
        if (next > directory.size() || nameLength == 0) {
            return tl::unexpected(parseError("central directory entry"));
        }
        if ((flags & 0x0001U) != 0 || method != 0 || compressed != size) {
            return tl::unexpected(parseError("compressed or encrypted entry"));
        }
        std::string name(reinterpret_cast<const char*>(c + kCentralHeaderSize), nameLength);
        if (!names.insert(name).second) {
            return tl::unexpected(parseError("duplicate entry '" + name + "'"));
        }
        // The local header must agree and the data must lie before the directory.
        std::array<std::uint8_t, kLocalHeaderSize> local{};
        if (static_cast<std::uint64_t>(localOffset) + kLocalHeaderSize > directoryOffset ||
            !readAt(reader.in_, localOffset, local.data(), local.size()) ||
            get32(local.data()) != kLocalSignature || get16(local.data() + 8) != 0 ||
            get16(local.data() + 26) != nameLength) {
            return tl::unexpected(parseError("local header of '" + name + "'"));
        }
        const std::uint64_t dataOffset = static_cast<std::uint64_t>(localOffset) +
                                         kLocalHeaderSize + nameLength + get16(local.data() + 28);
        if (dataOffset + size > directoryOffset) {
            return tl::unexpected(parseError("data of '" + name + "'"));
        }
        // Stored entries cannot add up to more than the archive (no overlapping entries
        // multiplying the data).
        total += size;
        if (total > fileSize) {
            return tl::unexpected(parseError("entries larger than the archive"));
        }
        reader.entries_.push_back(
            {.name = std::move(name), .size = size, .crc = crc, .dataOffset = dataOffset});
        pos = next;
    }
    return reader;
}

core::Result<std::vector<std::uint8_t>> ZipReader::read(const ZipEntry& entry,
                                                        std::size_t maxBytes) {
    if (entry.size > maxBytes) {
        return tl::unexpected(parseError("'" + entry.name + "' is too large"));
    }
    std::vector<std::uint8_t> bytes(entry.size);
    if (!readAt(in_, entry.dataOffset, bytes.data(), bytes.size())) {
        return makeError(ErrorCode::IoError, "reading '" + entry.name + "' failed");
    }
    if (crc32(bytes) != entry.crc) {
        return tl::unexpected(parseError("'" + entry.name + "' is corrupt (CRC)"));
    }
    return bytes;
}

core::Result<void> ZipReader::extract(const ZipEntry& entry, const std::filesystem::path& target) {
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return makeError(ErrorCode::AlreadyExists, "'" + detail::utf8(target) + "' exists");
    }
    const auto fail = [&](core::Error error) -> core::Result<void> {
        std::filesystem::remove(target, ec);
        return tl::unexpected(std::move(error));
    };
    std::ofstream out(target, std::ios::binary);
    if (!out) {
        return makeError(ErrorCode::IoError, "cannot create '" + detail::utf8(target) + "'");
    }
    std::vector<std::uint8_t> buffer(kCopyBuffer);
    std::uint64_t left = entry.size;
    std::uint64_t offset = entry.dataOffset;
    std::uint32_t crc = 0;
    while (left > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
        if (!readAt(in_, offset, buffer.data(), chunk)) {
            out.close();
            return fail({ErrorCode::IoError, "reading '" + entry.name + "' failed"});
        }
        crc = crc32(std::span(buffer.data(), chunk), crc);
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk));
        left -= chunk;
        offset += chunk;
    }
    out.close();
    if (!out) {
        return fail({ErrorCode::IoError, "writing '" + detail::utf8(target) + "' failed"});
    }
    if (crc != entry.crc) {
        return fail(parseError("'" + entry.name + "' is corrupt (CRC)"));
    }
    return {};
}

} // namespace studyapp::persistence
