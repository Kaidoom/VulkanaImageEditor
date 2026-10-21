#pragma once
// Shared bounded ZIP transport for project I/O and the thumbnail-only reader.
// No entry is extracted to disk and no project renderer is linked by the reader.
#include <QIODevice>
#include <QString>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <mz.h>
#include <mz_crypt.h>
#include <mz_strm.h>
#include <mz_zip.h>

namespace imageeditor::project {
inline constexpr quint64 manifestBudget = 8ULL * 1024 * 1024;
inline constexpr quint64 pixelLimit = 64ULL * 1024 * 1024;
inline constexpr int chunkSize = 256 * 1024;
struct Cancelled { };
inline void require(bool ok, const char* message)
{
    if (!ok)
        throw std::runtime_error(message);
}
inline void checked(int result, const char* message) { require(result == MZ_OK, message); }
inline quint64 checkedBytes(quint64 a, quint64 b)
{
    require(b <= std::numeric_limits<quint64>::max() - a, "Project byte count overflow");
    return a + b;
}
struct Progress {
    std::function<bool(quint64, quint64)> callback;
    quint64 done { 0 }, total { 1 };
    void tick(quint64 amount = 0)
    {
        done += amount;
        if (callback && !callback(std::min(done, total), total))
            throw Cancelled { };
    }
};

// A small seekable public minizip stream. Never throws through C, never owns
// or commits the device. Sticky errors catch compressor-finalization failures.
struct DeviceStream {
    mz_stream base { };
    QIODevice* device;
    int error { MZ_OK };
    static DeviceStream& self(void* p) { return *static_cast<DeviceStream*>(p); }
    static int32_t open(void*, const char*, int32_t) { return MZ_OK; }
    static int32_t isOpen(void* p) { return self(p).device->isOpen() ? MZ_OK : MZ_OPEN_ERROR; }
    static int32_t read(void* p, void* data, int32_t size)
    {
        auto& s = self(p);
        const auto n = s.device->read(static_cast<char*>(data), size);
        if (n < 0)
            s.error = MZ_READ_ERROR;
        return n < 0 ? MZ_READ_ERROR : int32_t(n);
    }
    static int32_t write(void* p, const void* data, int32_t size)
    {
        auto& s = self(p);
        const auto n = s.device->write(static_cast<const char*>(data), size);
        if (n != size)
            s.error = MZ_WRITE_ERROR;
        return n == size ? size : MZ_WRITE_ERROR;
    }
    static int64_t tell(void* p) { return self(p).device->pos(); }
    static int32_t seek(void* p, int64_t offset, int32_t origin)
    {
        auto& s = self(p);
        const auto start = origin == MZ_SEEK_SET ? 0 : origin == MZ_SEEK_CUR ? s.device->pos()
                                                                             : s.device->size();
        if ((offset < 0 && offset < -start) || (offset > 0 && start > INT64_MAX - offset)
            || !s.device->seek(start + offset)) {
            s.error = MZ_SEEK_ERROR;
            return s.error;
        }
        return MZ_OK;
    }
    static int32_t close(void*) { return MZ_OK; }
    static int32_t getError(void* p) { return self(p).error; }
    static int32_t get(void*, int32_t, int64_t*) { return MZ_EXIST_ERROR; }
    static int32_t set(void*, int32_t, int64_t) { return MZ_EXIST_ERROR; }
    explicit DeviceStream(QIODevice& io)
        : device(&io)
    {
        static mz_stream_vtbl table { open, isOpen, read, write, tell, seek, close, getError, nullptr, nullptr, get, set };
        base.vtbl = &table;
    }
};
struct Archive {
    DeviceStream stream;
    void* handle { mz_zip_create() };
    bool opened { false };
    std::map<QString, uint32_t> writtenCrc;
    Archive(QIODevice& io, bool writing)
        : stream(io)
    {
        require(handle, "Cannot allocate ZIP codec");
        mz_zip_set_recover(handle, 0);
        const auto result = mz_zip_open(handle, &stream, writing ? MZ_OPEN_MODE_WRITE | MZ_OPEN_MODE_CREATE : MZ_OPEN_MODE_READ);
        if (result != MZ_OK) {
            mz_zip_delete(&handle);
            throw std::runtime_error("Invalid or unreadable ZIP archive");
        }
        opened = true;
    }
    ~Archive()
    {
        if (opened)
            mz_zip_close(handle);
        mz_zip_delete(&handle);
    }
    void finish()
    {
        const auto result = mz_zip_close(handle);
        opened = false;
        checked(result, "Cannot finish ZIP archive");
        checked(stream.error, "Project device I/O failed");
    }
};
struct Entry {
    int64_t position;
    quint64 bytes;
    uint32_t crc;
    quint64 compressed { 0 };
};
using Entries = std::map<QString, Entry>;
inline Entries entries(Archive& zip, quint64 entryLimit = 65536,
    quint64 payloadLimit = pixelLimit * 4)
{
    uint64_t count = 0;
    uint32_t disk = 0;
    checked(mz_zip_get_number_entry(zip.handle, &count), "Missing ZIP directory");
    checked(mz_zip_get_disk_number_with_cd(zip.handle, &disk), "Missing ZIP disk information");
    require(disk == 0 && count > 0 && count <= entryLimit, "Unsupported multipart archive or excessive entry count");
    Entries result;
    quint64 metadata = 0;
    checked(mz_zip_goto_first_entry(zip.handle), "Empty ZIP archive");
    for (quint64 i = 0; i < count; ++i) {
        mz_zip_file* info = nullptr;
        checked(mz_zip_entry_get_info(zip.handle, &info), "Invalid ZIP entry");
        require(info && info->filename && info->filename_size > 0 && info->filename_size <= 256,
            "Invalid archive path");
        const QByteArray raw(info->filename, info->filename_size);
        const auto path = QString::fromUtf8(raw);
        require(!raw.contains('\0') && path.toUtf8() == raw && !path.startsWith('/') && !path.contains('\\')
                && !path.contains(':') && !path.split('/').contains("..") && !path.split('/').contains(".")
                && !path.split('/').contains(""),
            "Unsafe archive entry path");
        require(info->disk_number == 0 && !(info->flag & 1) && info->aes_version == 0
                && (!info->linkname || !*info->linkname) && mz_zip_entry_is_symlink(zip.handle) != MZ_OK
                && mz_zip_entry_is_dir(zip.handle) != MZ_OK,
            "Encrypted, linked, directory or multipart entry is unsupported");
        require(info->compression_method == MZ_COMPRESS_METHOD_STORE || info->compression_method == MZ_COMPRESS_METHOD_DEFLATE,
            "Unsupported archive compression");
        require(info->compressed_size >= 0 && info->uncompressed_size >= 0
                && quint64(info->uncompressed_size) <= payloadLimit,
            "Archive payload exceeds limits");
        metadata += quint64(info->filename_size) + info->extrafield_size + info->comment_size
            + sizeof(Entry) + sizeof(QString) + 4 * sizeof(void*);
        require(metadata <= 2 * manifestBudget, "Archive metadata exceeds memory budget");
        require(result.emplace(path, Entry { mz_zip_get_entry(zip.handle), quint64(info->uncompressed_size), info->crc, quint64(info->compressed_size) }).second,
            "Duplicate archive entry");
        const auto next = mz_zip_goto_next_entry(zip.handle);
        require(next == (i + 1 == count ? MZ_END_OF_LIST : MZ_OK), "Inconsistent ZIP entry count");
    }
    return result;
}
inline void readEntry(Archive& zip, const Entry& entry, const std::function<void(const char*, int)>& consume, Progress& progress)
{
    checked(mz_zip_goto_entry(zip.handle, entry.position), "Missing project payload");
    checked(mz_zip_entry_read_open(zip.handle, 0, nullptr), "Cannot decompress project payload");
    std::array<char, chunkSize> buffer;
    quint64 size = 0;
    uint32_t crc = 0;
    while (true) {
        const auto n = mz_zip_entry_read(zip.handle, buffer.data(), chunkSize);
        require(n >= 0, "Damaged compressed project payload");
        if (!n)
            break;
        size += quint64(n);
        require(size <= entry.bytes, "Payload expands beyond its declared size");
        crc = mz_crypt_crc32_update(crc, reinterpret_cast<const uint8_t*>(buffer.data()), n);
        consume(buffer.data(), n);
        progress.tick(quint64(n));
    }
    void* compressed = nullptr;
    int64_t consumed = -1;
    checked(mz_zip_entry_get_compress_stream(zip.handle, &compressed), "Missing decompression stream");
    checked(mz_stream_get_prop_int64(compressed, MZ_STREAM_PROP_TOTAL_IN, &consumed), "Missing compressed-byte count");
    require(consumed >= 0 && quint64(consumed) == entry.compressed, "Compressed payload size mismatch");
    checked(mz_zip_entry_read_close(zip.handle, nullptr, nullptr, nullptr), "Invalid project payload checksum");
    require(size == entry.bytes && crc == entry.crc, "Project payload size or CRC mismatch");
    checked(zip.stream.error, "Cannot read project device");
}
inline void writeEntry(Archive& zip, const QString& path, quint64 size,
    const std::function<void(const std::function<void(const char*, int)>&)>& produce, Progress& progress,
    bool stored = false)
{
    const auto name = path.toUtf8();
    mz_zip_file info { };
    info.filename = name.constData();
    info.filename_size = uint16_t(name.size());
    info.flag = MZ_ZIP_FLAG_UTF8;
    info.compression_method = stored ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
    info.zip64 = MZ_ZIP64_AUTO;
    info.uncompressed_size = int64_t(size);
    checked(mz_zip_entry_write_open(zip.handle, &info, 3, 0, nullptr), "Cannot create project payload");
    quint64 written = 0;
    uint32_t crc = 0;
    produce([&](const char* data, int n) {
        require(mz_zip_entry_write(zip.handle, data, n) == n, "Cannot write project payload (disk full or I/O failure)");
        checked(zip.stream.error, "Cannot write project device");
        written += quint64(n);
        crc = mz_crypt_crc32_update(crc, reinterpret_cast<const uint8_t*>(data), n);
        progress.tick(quint64(n));
    });
    require(written == size, "Inconsistent project snapshot size");
    checked(mz_zip_entry_write_close(zip.handle, 0, -1, -1), "Cannot finish compressed project payload");
    zip.writtenCrc[path] = crc;
}


} // namespace imageeditor::project

