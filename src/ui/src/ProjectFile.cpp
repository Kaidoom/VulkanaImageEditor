#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "LayerEffectCodec.hpp"
#include "AdjustmentCodec.hpp"
#include "SpatialFilterCodec.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/Shape.hpp"
#include "imageeditor/ui/TextClipboard.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTemporaryFile>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mz.h>
#include <mz_crypt.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace imageeditor::ui {
namespace {
    constexpr quint64 manifestBudget = 8ULL * 1024 * 1024;
    constexpr quint64 pixelLimit = 64ULL * 1024 * 1024;
    constexpr int layerLimit = 1024, chunkSize = 256 * 1024;
    constexpr quint64 entryLimit = quint64(layerLimit) * (1 + core::adjustmentCount + core::spatialFilterCount) + 1;
    struct Cancelled { };
    void require(bool ok, const char* message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
    void checked(int result, const char* message) { require(result == MZ_OK, message); }
    quint64 checkedBytes(quint64 a, quint64 b)
    {
        require(b <= std::numeric_limits<quint64>::max() - a, "Project byte count overflow");
        return a + b;
    }
    struct Progress {
        ProjectProgress callback;
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
    Entries entries(Archive& zip)
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
                    && quint64(info->uncompressed_size) <= pixelLimit * 4,
                "Archive payload exceeds limits");
            metadata += quint64(info->filename_size) + info->extrafield_size + info->comment_size;
            require(metadata <= 2 * manifestBudget, "Archive metadata exceeds memory budget");
            require(result.emplace(path, Entry { mz_zip_get_entry(zip.handle), quint64(info->uncompressed_size), info->crc, quint64(info->compressed_size) }).second,
                "Duplicate archive entry");
            const auto next = mz_zip_goto_next_entry(zip.handle);
            require(next == (i + 1 == count ? MZ_END_OF_LIST : MZ_OK), "Inconsistent ZIP entry count");
        }
        return result;
    }
    void readEntry(Archive& zip, const Entry& entry, const std::function<void(const char*, int)>& consume, Progress& progress)
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
    void writeEntry(Archive& zip, const QString& path, quint64 size,
        const std::function<void(const std::function<void(const char*, int)>&)>& produce, Progress& progress)
    {
        const auto name = path.toUtf8();
        mz_zip_file info { };
        info.filename = name.constData();
        info.filename_size = uint16_t(name.size());
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.zip64 = MZ_ZIP64_AUTO;
        info.uncompressed_size = int64_t(size);
        checked(mz_zip_entry_write_open(zip.handle, &info, 3, 0, nullptr), "Cannot create project payload");
        quint64 written = 0;
        produce([&](const char* data, int n) {
            require(mz_zip_entry_write(zip.handle, data, n) == n, "Cannot write project payload (disk full or I/O failure)");
            checked(zip.stream.error, "Cannot write project device");
            written += quint64(n);
            progress.tick(quint64(n));
        });
        require(written == size, "Inconsistent project snapshot size");
        checked(mz_zip_entry_write_close(zip.handle, 0, -1, -1), "Cannot finish compressed project payload");
    }

    double number(const QJsonValue& value, double low, double high, const char* message)
    {
        const double d = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        require(value.isDouble() && std::isfinite(d) && d >= low && d <= high, message);
        return d;
    }
    quint64 integer(const QJsonValue& v, quint64 low, quint64 high, const char* message)
    {
        const auto d = number(v, double(low), double(high), message);
        require(std::floor(d) == d, message);
        return quint64(d);
    }
    core::Extent2u extent(const QJsonObject& o)
    {
        const auto w = integer(o["width"], 1, 32768, "Unsupported raster/canvas width");
        const auto h = integer(o["height"], 1, 32768, "Unsupported raster/canvas height");
        require(w * h <= pixelLimit, "Raster/canvas exceeds 64 megapixel limit");
        return { uint32_t(w), uint32_t(h) };
    }
    core::LayerId layerId(const QJsonValue& v)
    {
        require(v.isString(), "Layer ID must be a decimal string");
        bool ok = false;
        const auto id = v.toString().toULongLong(&ok);
        require(ok && id > 0 && id <= quint64(std::numeric_limits<int64_t>::max())
                && QString::number(id) == v.toString(),
            "Invalid layer ID");
        return id;
    }
    QJsonValue overlayJson(const QJsonValue& original, const QJsonValue& known)
    {
        if (known.isObject()) {
            auto o = original.toObject();
            const auto k = known.toObject();
            for (auto i = k.begin(); i != k.end(); ++i)
                o[i.key()] = overlayJson(o[i.key()], i.value());
            return o;
        }
        if (known.isArray()) {
            const auto old = original.toArray();
            auto now = known.toArray();
            for (qsizetype i = 0; i < now.size(); ++i)
                now[i] = overlayJson(i < old.size() ? old[i] : QJsonValue { }, now[i]);
            return now;
        }
        return known;
    }
    constexpr std::array<const char*, 6> shapeKinds {
        "rectangle", "rounded-rectangle", "ellipse", "triangle", "line", "polygon"
    };
    QJsonArray shapeColor(core::Rgba8 color)
    {
        return { color.red, color.green, color.blue, color.alpha };
    }
    core::Rgba8 shapeColor(const QJsonValue& value)
    {
        const auto rgba = value.toArray();
        require(value.isArray() && rgba.size() == 4, "Invalid shape RGBA color");
        return { uint8_t(integer(rgba[0], 0, 255, "Invalid shape color channel")),
            uint8_t(integer(rgba[1], 0, 255, "Invalid shape color channel")),
            uint8_t(integer(rgba[2], 0, 255, "Invalid shape color channel")),
            uint8_t(integer(rgba[3], 0, 255, "Invalid shape color channel")) };
    }
    QJsonObject encodeShape(const core::ShapeLayer& shape)
    {
        require(core::validShape(shape), "Invalid shape geometry or style");
        QJsonArray points;
        for (const auto& point : shape.points)
            points.append(QJsonArray { point.x, point.y });
        return { { "version", 1 }, { "kind", shapeKinds.at(size_t(shape.kind)) },
            { "size", QJsonArray { shape.size.width, shape.size.height } }, { "points", points },
            { "cornerRadius", shape.cornerRadius }, { "fillEnabled", shape.fillEnabled },
            { "fillRgba", shapeColor(shape.fillColor) }, { "strokeEnabled", shape.strokeEnabled },
            { "strokeRgba", shapeColor(shape.strokeColor) }, { "strokeWidth", shape.strokeWidth },
            { "strokePlacement", "center" },
            { "cap", shape.strokeCap==core::ShapeCap::Butt?"butt":shape.strokeCap==core::ShapeCap::Square?"square":"round" },
            { "join", shape.strokeJoin==core::ShapeJoin::Miter?"miter":shape.strokeJoin==core::ShapeJoin::Bevel?"bevel":"round" },
            { "miterLimit", shape.strokeMiterLimit } };
    }
    core::ShapeLayer decodeShape(const QJsonObject& object)
    {
        require(object["version"].isDouble() && object["version"].toDouble() == 1,
            "Unsupported shape descriptor version");
        const auto kindName = object["kind"].toString();
        const auto kind = std::find_if(shapeKinds.begin(), shapeKinds.end(),
            [&](const char* name) { return kindName == QLatin1String(name); });
        require(kind != shapeKinds.end(), "Unsupported shape kind");
        core::ShapeLayer shape;
        shape.kind = core::ShapeKind(std::distance(shapeKinds.begin(), kind));
        const auto size = object["size"].toArray();
        require(object["size"].isArray() && size.size() == 2, "Missing or invalid shape size");
        shape.size = { number(size[0], 0, core::maximumShapeDimension, "Invalid shape width"),
            number(size[1], 0, core::maximumShapeDimension, "Invalid shape height") };
        require(object["points"].isArray(), "Missing shape points");
        const auto points = object["points"].toArray();
        require(size_t(points.size()) <= core::maximumShapePoints, "Shape exceeds polygon vertex limit");
        shape.points.reserve(size_t(points.size()));
        for (const auto& value : points) {
            const auto point = value.toArray();
            require(value.isArray() && point.size() == 2, "Invalid shape vertex");
            shape.points.push_back({ number(point[0], 0, shape.size.width, "Invalid shape vertex X"),
                number(point[1], 0, shape.size.height, "Invalid shape vertex Y") });
        }
        shape.cornerRadius = number(object["cornerRadius"], 0, core::maximumShapeStyleDimension, "Invalid shape corner radius");
        require(object["fillEnabled"].isBool() && object["strokeEnabled"].isBool(),
            "Missing or invalid shape fill/stroke enable state");
        shape.fillEnabled = object["fillEnabled"].toBool();
        shape.strokeEnabled = object["strokeEnabled"].toBool();
        shape.fillColor = shapeColor(object["fillRgba"]);
        shape.strokeColor = shapeColor(object["strokeRgba"]);
        shape.strokeWidth = number(object["strokeWidth"], 0, core::maximumShapeStyleDimension, "Invalid shape stroke width");
        require(object["strokePlacement"] == "center"
                && (object["cap"]=="round"||object["cap"]=="butt"||object["cap"]=="square")
                && (object["join"]=="round"||object["join"]=="miter"||object["join"]=="bevel"),
            "Unsupported shape stroke placement, cap or join");
        shape.strokeCap=object["cap"]=="butt"?core::ShapeCap::Butt:object["cap"]=="square"?core::ShapeCap::Square:core::ShapeCap::Round;
        shape.strokeJoin=object["join"]=="miter"?core::ShapeJoin::Miter:object["join"]=="bevel"?core::ShapeJoin::Bevel:core::ShapeJoin::Round;
        if(object.contains("miterLimit"))shape.strokeMiterLimit=number(object["miterLimit"],.5,1000,"Invalid shape miter limit");
        require(core::validShape(shape), "Invalid shape geometry or style");
        return shape;
    }
    QJsonObject makeManifest(const core::Document& doc, const QJsonObject& metadata)
    {
        QJsonArray layers;
        bool containsShapes = false;
        bool containsShapeStrokeV2 = false;
        bool containsLabels = false;
        bool containsBlendModes = false;
        bool containsExtendedBlendModes = false;
        bool containsAdjustments = false;
        bool containsSpatialFilters = false;
        bool containsLayerEffects = false;
        bool containsLayerMasks = false;
        bool containsProjective = false;
        bool containsRasterFrame = false;
        bool containsLayerCrop = false;
        bool containsChamfer = false;
        std::map<core::LayerId, QJsonObject> oldLayers;
        for (const auto& value : metadata["layers"].toArray()) {
            bool ok = false;
            const auto id = value.toObject()["id"].toString().toULongLong(&ok);
            if (ok)
                oldLayers[id] = value.toObject();
        }
        for (const auto& layer : doc.layers()) {
            require(core::isValidBlendMode(layer.blendMode), "Unsupported layer blend mode; project was not saved");
            containsBlendModes |= layer.blendMode != core::BlendMode::Normal;
            containsExtendedBlendModes |= layer.blendMode >= core::BlendMode::ColorDodge;
            if(layer.adjustments)for(const auto& a:layer.adjustments->items)containsProjective|=a.mask&&!a.mask->localToMask.isAffine();
            if(layer.filters)for(const auto& f:layer.filters->items)containsProjective|=f.mask&&!f.mask->localToMask.isAffine();
            const auto& t = layer.localToDocument;
            QJsonObject o { { "id", QString::number(layer.id) }, { "name", QString::fromStdString(layer.name) },
                { "visible", layer.visible }, { "opacity", double(layer.opacity) },
                { "blendMode", QString::fromUtf8(core::blendModeId(layer.blendMode)) },
                { "transform", QJsonArray { t.m00, t.m01, t.m02, t.m10, t.m11, t.m12 } } };
            if(!t.isAffine()) {
                containsProjective=true;
                o["transform"]=QJsonArray{t.m00,t.m01,t.m02,t.m10,t.m11,t.m12,t.m20,t.m21,t.m22};
            }
            if (const auto* raster = std::get_if<core::RasterLayer>(&layer.payload)) {
                require(bool(raster->surface), "Raster layer has no surface");
                const auto e = raster->surface->extent();
                o["type"] = "raster";
                o["raster"] = QJsonObject { { "width", int(e.width) }, { "height", int(e.height) },
                    { "path", QStringLiteral("rasters/%1.rgba").arg(layer.id) } };
            } else if (const auto* text = std::get_if<core::TextLayer>(&layer.payload)) {
                o["type"] = "text";
                o["text"] = QJsonDocument::fromJson(encodeTextClipboard(*text)).object();
            } else {
                o["type"] = "shape";
                o["shape"] = encodeShape(std::get<core::ShapeLayer>(layer.payload));
                containsShapes = true;
                const auto& shape=std::get<core::ShapeLayer>(layer.payload);
                containsShapeStrokeV2 |= shape.strokeJoin!=core::ShapeJoin::Round || shape.strokeCap!=core::ShapeCap::Round;
            }
            require(layer.colorLabel <= uint8_t(core::ColorLabel::Purple), "Invalid layer color label");
            auto saved = overlayJson(oldLayers[layer.id], o).toObject();
            saved.remove("layerMask");
            if (layer.mask) {
                require(core::validLayerMask(layer.mask), "Invalid layer mask");
                containsLayerMasks = true;
                const auto& m = *layer.mask;
                const auto e = m.coverage->extent();
                const auto& a = m.localToMask;
                containsProjective |= !a.isAffine();
                saved["layerMask"] = QJsonObject{{"version",1},{"enabled",m.enabled},{"outside",int(m.outside)},
                    {"width",int(e.width)},{"height",int(e.height)},
                    {"path",QStringLiteral("layer-masks/%1.r8").arg(layer.id)},
                    {"transform",QJsonArray{a.m00,a.m01,a.m02,a.m10,a.m11,a.m12,a.m20,a.m21,a.m22}}};
            }
            saved.remove("rasterLocalFrame");
            if(std::holds_alternative<core::RasterLayer>(layer.payload)&&(layer.rasterOrigin!=core::Vec2d{}||layer.rasterEffectFrame)) {
                containsRasterFrame=true;
                QJsonObject frame{{"origin",QJsonArray{layer.rasterOrigin.x,layer.rasterOrigin.y}}};
                if(layer.rasterEffectFrame){const auto r=*layer.rasterEffectFrame;frame["effectReference"]=QJsonArray{r.x,r.y,r.width,r.height};}
                saved["rasterLocalFrame"]=frame;
            }
            // Cropping is owned geometry, not opaque metadata. Clearing it
            // must reveal the full source without reviving a prior crop.
            saved.remove("crop");
            if (layer.crop) {
                require(core::validLayerCrop(*layer.crop), "Invalid layer crop; project was not saved");
                const auto& crop = *layer.crop;
                saved["crop"] = QJsonObject { { "version", 1 }, { "x", crop.x }, { "y", crop.y },
                    { "width", crop.width }, { "height", crop.height } };
                if (crop.hasChamfer()) {
                    auto descriptor = saved["crop"].toObject();
                    descriptor["version"] = 2;
                    QJsonArray corners;
                    for (double value : crop.corners)
                        corners.append(value);
                    descriptor["corners"] = corners;
                    saved["crop"] = descriptor;
                    containsChamfer = true;
                }
                containsLayerCrop = true;
            }
            // Adjustment settings and mask references are owned state. Reset
            // All must not revive data from the original manifest metadata.
            saved.remove("adjustments");
            saved.remove("spatialFilters");
            saved.remove("layerEffects");
            if(layer.effects) {
                saved["layerEffects"]=detail::encodeLayerEffects(*layer.effects);
                containsLayerEffects=true;
            }
            if(layer.filters) {
                saved["spatialFilters"]=detail::encodeSpatialFilters(*layer.filters,layer.id);
                containsSpatialFilters=true;
            }
            if (layer.adjustments) {
                saved["adjustments"] = detail::encodeAdjustments(*layer.adjustments, layer.id);
                containsAdjustments = true;
            }
            // This is owned data, not optional metadata: clearing a label must
            // not resurrect an old value from the source manifest.
            saved.remove("colorLabel");
            if (layer.colorLabel != 0) {
                saved["colorLabel"] = int(layer.colorLabel);
                containsLabels = true;
            }
            layers.append(saved);
        }
        auto result = overlayJson(metadata, QJsonObject { { "format", "org.vulkana.project" }, { "version", 1 }, { "required", QJsonArray { "rgba8", "rich-text-v1" } }, { "canvas", QJsonObject { { "width", int(doc.canvas().extent.width) }, { "height", int(doc.canvas().extent.height) }, { "ppi", doc.canvas().dotsPerInch }, { "colorSpace", "srgb" }, { "pixelFormat", "rgba8-straight" } } } }).toObject();
        if (containsLayerCrop) {
            auto required = result["required"].toArray();
            required.append("layer-crop-v1");
            if(containsChamfer)required.append("layer-crop-chamfer-v1");
            result["required"] = required;
        }
        if (containsProjective) {
            auto required=result["required"].toArray();
            required.append("projective-transform-v1");result["required"]=required;
        }
        if(containsRasterFrame){auto required=result["required"].toArray();required.append("raster-local-frame-v1");result["required"]=required;}
        if (containsLayerMasks) { auto required=result["required"].toArray();required.append("layer-mask-v1");result["required"]=required; }
        if (containsLayerEffects) {
            auto required=result["required"].toArray();
            required.append("layer-effects-v1");result["required"]=required;
        }
        if (containsSpatialFilters) {
            auto required=result["required"].toArray();
            required.append("spatial-filters-v1");result["required"]=required;
        }
        if (containsAdjustments) {
            auto required = result["required"].toArray();
            required.append("adjustments-v1");
            result["required"] = required;
        }
        if (containsShapes) {
            auto required = result["required"].toArray();
            required.append("shape-v1");
            if(containsShapeStrokeV2)required.append("shape-stroke-v2");
            result["required"] = required;
        }
        if (containsBlendModes) {
            auto required = result["required"].toArray();
            required.append("layer-blend-modes-v1");
            if (containsExtendedBlendModes)
                required.append("layer-blend-modes-v2");
            result["required"] = required;
        }
        result["layers"] = layers;
        result.remove("lastSelection");
        const auto remembered = doc.selection() ? doc.selection() : doc.lastSelection();
        if (remembered) {
            auto required = result["required"].toArray(); required.append("selection-recall-v1"); result["required"] = required;
            result["lastSelection"] = QJsonObject{{"version",1},{"path","selections/last.r8"},
                {"width",int(remembered->extent().width)},{"height",int(remembered->extent().height)}};
        }
        result.remove("hierarchy");
        if (!doc.tree().containers.empty() || containsLabels) {
            auto required = result["required"].toArray();
            required.append("hierarchy-v1");
            if (std::any_of(doc.tree().containers.begin(), doc.tree().containers.end(),
                    [](const auto& container) { return !container.visible; }))
                required.append("container-visibility-v1");
            result["required"] = required;
            auto hierarchy = metadata["hierarchy"].toObject();
            std::map<core::LayerId, QJsonObject> oldContainers;
            for (const auto& value : hierarchy["containers"].toArray()) {
                bool ok = false;
                const auto id = value.toObject()["id"].toString().toULongLong(&ok);
                if (ok)
                    oldContainers[id] = value.toObject();
            }
            const auto encodeIds = [](const auto& ids) {
                QJsonArray array;
                for (const auto id : ids)
                    array.append(QString::number(id));
                return array;
            };
            QJsonArray containers;
            for (const auto& container : doc.tree().containers) {
                auto saved = oldContainers[container.id];
                saved["id"] = QString::number(container.id);
                saved["name"] = QString::fromStdString(container.name);
                saved["kind"] = container.kind == core::ContainerKind::Folder ? "folder" : "group";
                saved["colorLabel"] = int(container.colorLabel);
                saved["children"] = encodeIds(container.children);
                saved["visible"] = container.visible;
                containers.append(saved);
            }
            hierarchy["version"] = 1;
            hierarchy["roots"] = encodeIds(doc.tree().roots);
            hierarchy["containers"] = containers;
            result["hierarchy"] = hierarchy;
        }
        return result;
    }
    struct LayerPlan {
        core::Layer layer;
        core::Extent2u extent;
        QString path;
        QString maskPath;
        detail::AdjustmentPlan adjustments;
        detail::SpatialFilterPlan filters;
    };
    struct Plan {
        core::CanvasSpec canvas;
        bool hasLastSelection {false};
        std::vector<LayerPlan> layers;
        core::LayerTree tree;
        quint64 bytes { 0 };
    };
    core::LayerTree decodeHierarchy(const QJsonValue& value, std::span<const core::LayerId> leaves)
    {
        require(value.isObject(), "Missing or invalid layer hierarchy");
        const auto object = value.toObject();
        require(object["version"].isDouble() && object["version"].toDouble() == 1,
            "Unsupported layer hierarchy version");
        require(object["containers"].isArray(), "Missing layer containers");
        const auto records = object["containers"].toArray();
        require(size_t(records.size()) + leaves.size() <= core::LayerTree::maxItems,
            "Layer hierarchy exceeds item limit");
        size_t membershipCount = 0;
        const auto decodeIds = [&](const QJsonValue& ids) {
            require(ids.isArray() && size_t(ids.toArray().size()) <= core::LayerTree::maxItems,
                "Missing or oversized hierarchy membership list");
            std::vector<core::LayerId> result;
            const auto array = ids.toArray();
            membershipCount += size_t(array.size());
            require(membershipCount <= core::LayerTree::maxItems, "Layer hierarchy exceeds membership limit");
            result.reserve(size_t(array.size()));
            for (const auto& item : array)
                result.push_back(layerId(item));
            return result;
        };
        core::LayerTree tree;
        tree.roots = decodeIds(object["roots"]);
        tree.containers.reserve(size_t(records.size()));
        for (const auto& record : records) {
            require(record.isObject(), "Invalid container record");
            const auto item = record.toObject();
            core::LayerContainer container;
            container.id = layerId(item["id"]);
            require(item["name"].isString() && !item["name"].toString().isEmpty()
                    && item["name"].toString().toUtf8().size() <= 4096,
                "Invalid container name");
            container.name = item["name"].toString().toStdString();
            require(item["kind"] == "folder" || item["kind"] == "group", "Unsupported container kind");
            container.kind = item["kind"] == "folder" ? core::ContainerKind::Folder : core::ContainerKind::Group;
            container.colorLabel = core::ColorLabel(integer(item.contains("colorLabel") ? item["colorLabel"] : QJsonValue(0),
                0, int(core::ColorLabel::Purple), "Invalid container color label"));
            require(!item.contains("visible") || item["visible"].isBool(), "Invalid container visibility");
            container.visible = item.value("visible").toBool(true);
            container.children = decodeIds(item["children"]);
            tree.containers.push_back(std::move(container));
        }
        // Validate the full common-ID namespace and membership graph before
        // allocating or decoding any raster payload. The tree is authoritative;
        // the leaf payload array need not already be in traversal order.
        (void)tree.orderedLeaves(leaves);
        return tree;
    }
    // Explicit version boundary: future migrations belong here, never rewrite a
    // source file on load. Version one has no historical in-place upgrade path.
    Plan validateManifest(const QJsonObject& o, const Entries& files)
    {
        require(o["format"] == "org.vulkana.project", "Not a Vulkana project");
        require(o["version"].isDouble() && o["version"].toDouble() == 1, "Unsupported Vulkana schema version");
        require(!o.contains("required") || o["required"].isArray(), "Invalid required capabilities");
        const auto required = o["required"].toArray();
        for (const auto& capability : required)
            require(capability == "rgba8" || capability == "rich-text-v1" || capability == "shape-v1" || capability == "shape-stroke-v2"
                    || capability == "hierarchy-v1" || capability == "container-visibility-v1"
                    || capability == "layer-blend-modes-v1" || capability == "layer-blend-modes-v2"
                    || capability == "adjustments-v1" || capability == "spatial-filters-v1" || capability == "layer-crop-v1" || capability == "layer-crop-chamfer-v1"
                    || capability == "selection-recall-v1" || capability == "layer-effects-v1" || capability == "projective-transform-v1" || capability == "raster-local-frame-v1" || capability == "layer-mask-v1",
                "Project requires unsupported capabilities");
        require(o["canvas"].isObject() && o["layers"].isArray(), "Missing canvas or layers");
        const auto canvas = o["canvas"].toObject();
        require((!canvas.contains("colorSpace") || canvas["colorSpace"] == "srgb")
                && (!canvas.contains("pixelFormat") || canvas["pixelFormat"] == "rgba8-straight"),
            "Unsupported document color pipeline");
        Plan result;
        result.canvas = { extent(canvas), number(canvas.value("ppi").isUndefined() ? QJsonValue(96) : canvas["ppi"], 1, 1200, "Invalid canvas resolution") };
        const auto layers = o["layers"].toArray();
        require(!layers.empty() && layers.size() <= layerLimit, "Invalid layer count");
        std::set<core::LayerId> ids;
        std::vector<core::LayerId> leafOrder;
        std::set<QString> used { "manifest.json" };
        if (o.contains("lastSelection")) {
            const auto s = o["lastSelection"].toObject();
            require(required.contains("selection-recall-v1") && s["version"] == 1
                    && s["path"] == "selections/last.r8" && extent(s) == result.canvas.extent,
                "Invalid remembered selection descriptor");
            const auto size = quint64(result.canvas.extent.width) * result.canvas.extent.height;
            require(size <= 64ULL*1024*1024, "Remembered selection exceeds the 64 megapixel limit");
            const auto entry = files.find("selections/last.r8");
            require(entry != files.end() && entry->second.bytes == size, "Missing or incorrectly sized remembered selection");
            used.insert("selections/last.r8"); result.bytes = checkedBytes(result.bytes, size); result.hasLastSelection = true;
        }
        for (const auto& value : layers) {
            require(value.isObject(), "Invalid layer record");
            const auto l = value.toObject();
            require(!l.contains("rasterLocalFrame")||l["type"]=="raster","Raster local frame is only supported on raster layers");
            LayerPlan p;
            p.layer.id = layerId(l["id"]);
            if (l.contains("layerMask")) {
                const auto m = l["layerMask"].toObject();
                require(required.contains("layer-mask-v1") && m["version"] == 1 && m["enabled"].isBool(),
                    "Unsupported or invalid layer mask");
                const auto e = extent(m);
                const auto size = quint64(e.width)*e.height;
                require(size <= 64ULL*1024*1024, "Layer mask exceeds 64 megapixels");
                p.maskPath = QStringLiteral("layer-masks/%1.r8").arg(p.layer.id);
                require(m["path"] == p.maskPath && used.insert(p.maskPath).second,
                    "Invalid layer mask payload path");
                const auto entry = files.find(p.maskPath);
                require(entry != files.end() && entry->second.bytes == size, "Missing or incorrectly sized layer mask");
                result.bytes = checkedBytes(result.bytes, size);
                const auto t = m["transform"].toArray();
                require(t.size() == 9, "Invalid layer mask transform");
                std::array<double,9> a{};
                for (int i=0;i<9;++i) a[size_t(i)]=number(t[i],-1e12,1e12,"Invalid mask transform coefficient");
                auto mask = std::make_shared<core::LayerMask>();
                mask->coverage = core::SelectionMask::filled(e,255);
                mask->localToMask = {a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]};
                mask->outside = uint8_t(integer(m["outside"],0,255,"Invalid layer mask outside coverage"));
                mask->enabled = m["enabled"].toBool();
                require(core::validLayerMask(mask) && (mask->localToMask.isAffine() || required.contains("projective-transform-v1")),
                    "Invalid or undeclared projective layer mask");
                p.layer.mask = std::move(mask);
            }
            require(ids.insert(p.layer.id).second, "Duplicate layer ID");
            leafOrder.push_back(p.layer.id);
            require(l["name"].isString() && !l["name"].toString().isEmpty() && l["name"].toString().size() <= 4096,
                "Invalid layer name");
            p.layer.name = l["name"].toString().toStdString();
            if (l.contains("colorLabel")) {
                require(required.contains("hierarchy-v1"), "Layer labels require declared hierarchy-v1 capability");
                p.layer.colorLabel = uint8_t(integer(l["colorLabel"], 0, int(core::ColorLabel::Purple), "Invalid layer color label"));
            }
            require(!l.contains("visible") || l["visible"].isBool(), "Invalid visibility");
            p.layer.visible = l.value("visible").toBool(true);
            p.layer.opacity = float(number(l.contains("opacity") ? l["opacity"] : QJsonValue(1), 0, 1, "Invalid layer opacity"));
            if (l.contains("crop")) {
                require(required.contains("layer-crop-v1"), "Layer crop requires declared layer-crop-v1 capability");
                require(l["crop"].isObject(), "Missing or invalid layer crop descriptor");
                const auto crop = l["crop"].toObject();
                const auto version = integer(crop["version"], 1, 2, "Unsupported layer crop version; project was not opened");
                const core::RectD rectangle {
                    number(crop["x"], -1e9, 1e9, "Invalid layer crop X"),
                    number(crop["y"], -1e9, 1e9, "Invalid layer crop Y"),
                    number(crop["width"], 0, 1e9, "Invalid layer crop width"),
                    number(crop["height"], 0, 1e9, "Invalid layer crop height")
                };
                require(core::validLayerCrop(rectangle), "Invalid layer crop bounds");
                p.layer.crop = rectangle;
                if (version == 2) {
                    require(required.contains("layer-crop-chamfer-v1"), "Chamfer crop requires declared layer-crop-chamfer-v1 capability");
                    require(crop["corners"].isArray() && crop["corners"].toArray().size() == 4, "Chamfer crop requires four corner distances");
                    const auto corners = crop["corners"].toArray();
                    for (int i = 0; i < 4; ++i)
                        p.layer.crop->corners[std::size_t(i)] = number(corners[i], 0, 1e9, "Invalid chamfer distance");
                } else
                    require(!crop.contains("corners"), "Chamfer corners require crop version 2");
            }
            if (l.contains("blendMode")) {
                require(l["blendMode"].isString(), "Invalid layer blend mode; expected a stable mode name");
                const auto mode = core::blendModeFromId(l["blendMode"].toString().toStdString());
                require(mode.has_value(), "Unsupported layer blend mode; project was not opened");
                p.layer.blendMode = *mode;
                require(*mode == core::BlendMode::Normal || required.contains("layer-blend-modes-v1"),
                    "Non-Normal blend mode requires declared layer-blend-modes-v1 capability");
                require(*mode < core::BlendMode::ColorDodge || required.contains("layer-blend-modes-v2"),
                    "Extended blend mode requires declared layer-blend-modes-v2 capability");
            }
            if (l.contains("transform")) {
                const auto t = l["transform"].toArray();
                require(t.size() == 6 || (t.size()==9 && required.contains("projective-transform-v1")), "Invalid or undeclared projective transform");
                std::array<double, 9> a {0,0,0,0,0,0,0,0,1};
                for (int i = 0; i < t.size(); ++i)
                    a[size_t(i)] = number(t[i], -1e12, 1e12, "Invalid transform coefficient");
                p.layer.localToDocument = { a[0], a[1], a[2], a[3], a[4], a[5],a[6],a[7],a[8] };
                require(p.layer.localToDocument.inverted().has_value(), "Singular or invalid layer transform");
            }
            if(l.contains("layerEffects")) {
                require(required.contains("layer-effects-v1")&&l["layerEffects"].isObject(),"Invalid layer-effects capability or descriptor");
                p.layer.effects=detail::decodeLayerEffects(l["layerEffects"].toObject());
            }
            if(l.contains("spatialFilters")) {
                require(required.contains("spatial-filters-v1"),"Spatial filters require declared spatial-filters-v1 capability");
                require(l["spatialFilters"].isObject(),"Invalid spatial-filter descriptor");
                p.filters=detail::decodeSpatialFilters(l["spatialFilters"].toObject(),p.layer.id);
                for(const auto& mask:p.filters.masks) {
                    require(mask.localToMask.isAffine()||required.contains("projective-transform-v1"),"Undeclared projective filter mask");
                    const quint64 size=quint64(mask.extent.width)*mask.extent.height;
                    result.bytes = checkedBytes(result.bytes, size);
                    const auto entry=files.find(mask.path);
                    require(entry!=files.end() && entry->second.bytes==size && used.insert(mask.path).second,
                        "Missing, duplicate or incorrectly sized spatial-filter mask");
                }
            }
            if (l.contains("adjustments")) {
                require(required.contains("adjustments-v1"), "Layer adjustments require declared adjustments-v1 capability");
                require(l["adjustments"].isObject(), "Missing or invalid adjustment descriptor");
                p.adjustments = detail::decodeAdjustments(l["adjustments"].toObject(), p.layer.id);
                for (const auto& mask : p.adjustments.masks) {
                    require(mask.localToMask.isAffine()||required.contains("projective-transform-v1"),"Undeclared projective adjustment mask");
                    const quint64 size = quint64(mask.extent.width) * mask.extent.height;
                    result.bytes = checkedBytes(result.bytes, size);
                    const auto entry = files.find(mask.path);
                    require(entry != files.end() && entry->second.bytes == size && used.insert(mask.path).second,
                        "Missing, duplicate or incorrectly sized adjustment-mask payload");
                }
            }
            if (l["type"] == "raster") {
                require(l["raster"].isObject(), "Missing raster descriptor");
                const auto r = l["raster"].toObject();
                p.extent = extent(r);
                if(l.contains("rasterLocalFrame")) {
                    require(required.contains("raster-local-frame-v1")&&l["rasterLocalFrame"].isObject(),"Invalid or undeclared raster local frame");
                    const auto frame=l["rasterLocalFrame"].toObject();const auto origin=frame["origin"].toArray();
                    require(origin.size()==2,"Invalid raster storage origin");
                    p.layer.rasterOrigin={number(origin[0],-1e9,1e9,"Invalid raster origin X"),number(origin[1],-1e9,1e9,"Invalid raster origin Y")};
                    require(std::floor(p.layer.rasterOrigin.x)==p.layer.rasterOrigin.x&&std::floor(p.layer.rasterOrigin.y)==p.layer.rasterOrigin.y,"Raster storage origin must be pixel aligned");
                    if(frame.contains("effectReference")) {
                        const auto a=frame["effectReference"].toArray();require(a.size()==4,"Invalid effect reference frame");
                        p.layer.rasterEffectFrame=core::RectD{number(a[0],-1e9,1e9,"Invalid effect frame X"),number(a[1],-1e9,1e9,"Invalid effect frame Y"),number(a[2],1e-9,32768,"Invalid effect frame width"),number(a[3],1e-9,32768,"Invalid effect frame height")};
                    }
                }
                require(p.layer.localToDocument.validOver({p.layer.rasterOrigin.x,p.layer.rasterOrigin.y,double(p.extent.width),double(p.extent.height)}),"Projective horizon crosses raster content");
                p.path = r["path"].toString();
                require(p.path == QStringLiteral("rasters/%1.rgba").arg(p.layer.id), "Invalid raster payload path");
                const quint64 size = quint64(p.extent.width) * p.extent.height * 4;
                result.bytes = checkedBytes(result.bytes, size);
                const auto entry = files.find(p.path);
                require(entry != files.end() && entry->second.bytes == size && used.insert(p.path).second,
                    "Missing, duplicate or incorrectly sized raster payload");
            } else if (l["type"] == "text") {
                require(l["text"].isObject(), "Missing text descriptor");
                auto text = decodeTextClipboard(QJsonDocument(l["text"].toObject()).toJson(QJsonDocument::Compact));
                require(text.has_value(), "Invalid rich-text data or formatting ranges");
                p.layer.payload = std::move(*text);
            } else if (l["type"] == "shape") {
                require(required.contains("shape-v1"), "Shape layer requires declared shape-v1 capability");
                require(l["shape"].isObject(), "Missing shape descriptor");
                auto shape=decodeShape(l["shape"].toObject());
                require((shape.strokeJoin==core::ShapeJoin::Round && shape.strokeCap==core::ShapeCap::Round)
                        || required.contains("shape-stroke-v2"),
                    "Non-round shape stroke requires declared shape-stroke-v2 capability");
                p.layer.payload = std::move(shape);
            } else
                throw std::runtime_error("Unsupported layer type; project was not opened");
            result.layers.push_back(std::move(p));
        }
        if (o.contains("hierarchy")) {
            require(required.contains("hierarchy-v1"), "Hierarchy requires declared hierarchy-v1 capability");
            result.tree = decodeHierarchy(o["hierarchy"], leafOrder);
            for (const auto& container : result.tree.containers)
                require(container.visible || required.contains("container-visibility-v1"),
                    "Hidden containers require declared container-visibility-v1 capability");
        } else {
            // Original version-one files are exactly the root-level leaf order.
            result.tree.roots = std::move(leafOrder);
        }
        require(used.size() == files.size(), "Unrecognized project payload; required data cannot be discarded");
        return result;
    }
    QJsonObject readManifest(Archive& zip, const Entries& files, Progress& progress)
    {
        const auto found = files.find("manifest.json");
        require(found != files.end() && found->second.bytes <= manifestBudget, "Missing or oversized manifest.json");
        QByteArray json;
        json.reserve(qsizetype(found->second.bytes));
        readEntry(zip, found->second, [&](const char* b, int n) { json.append(b, n); }, progress);
        QJsonParseError error;
        const auto parsed = QJsonDocument::fromJson(json, &error);
        require(error.error == QJsonParseError::NoError && parsed.isObject(), "Invalid project JSON manifest");
        return parsed.object();
    }
}

ProjectLoadResult loadProject(const QString& path, ProjectProgress callback, ProjectLoadLimits limits)
{
    ProjectLoadResult result;
    try {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "Cannot open project file");
        Archive zip(file, false);
        const auto files = entries(zip);
        Progress progress { std::move(callback) };
        for (const auto& [name, entry] : files) {
            Q_UNUSED(name);
            progress.total += entry.bytes;
        }
        progress.tick();
        result.metadata = readManifest(zip, files, progress);
        auto plan = validateManifest(result.metadata, files);
        // Saving streams data already owned by the document. Loading must admit
        // new resident pixels, mask decoding, upload staging and driver backing.
        // Do not apply this admission to the save-time streaming CRC pass.
        const auto twicePixels = checkedBytes(plan.bytes, plan.bytes);
        const auto estimate = checkedBytes(checkedBytes(twicePixels, twicePixels), 64ULL * 1024 * 1024);
        const auto allowance = limits.workingBytes ? limits.workingBytes : platform::availableWorkingMemoryBytes();
        if (estimate > allowance)
            throw std::runtime_error(QStringLiteral(
                "Project needs about %1 MiB of working memory; available load allowance is %2 MiB. "
                "Close other documents/apps and try again.")
                .arg(double(estimate) / 1048576, 0, 'f', 0)
                .arg(double(allowance) / 1048576, 0, 'f', 0).toStdString());
        auto doc = std::make_unique<core::Document>(plan.canvas);
        if (plan.hasLastSelection) {
            const auto e = plan.canvas.extent;
            std::vector<std::uint8_t> coverage(std::size_t(e.width)*e.height);
            std::size_t offset=0;
            readEntry(zip,files.at("selections/last.r8"),[&](const char* b,int n) {
                std::memcpy(coverage.data()+offset,b,std::size_t(n)); offset+=std::size_t(n);
            },progress);
            doc->setLastSelection(core::SelectionMask::fromR8(e,coverage,e.width));
        }
        const auto families = QFontDatabase::families();
        std::set<QString> missing;
        for (auto& p : plan.layers) {
            if (p.layer.mask) {
                auto mask = std::make_shared<core::LayerMask>(*p.layer.mask);
                const auto e = mask->coverage->extent();
                std::vector<uint8_t> coverage(size_t(e.width)*e.height);
                size_t offset = 0;
                readEntry(zip,files.at(p.maskPath),[&](const char* b,int n) {
                    std::memcpy(coverage.data()+offset,b,size_t(n)); offset+=size_t(n);
                },progress);
                mask->coverage = core::SelectionMask::fromR8(e,coverage,e.width);
                p.layer.mask = std::move(mask);
            }
            if(p.filters.stack) {
                for(const auto& mask:p.filters.masks) {
                    std::vector<std::uint8_t> coverage(std::size_t(mask.extent.width)*mask.extent.height);
                    std::size_t offset=0;
                    readEntry(zip,files.at(mask.path),[&](const char* b,int n){
                        std::memcpy(coverage.data()+offset,b,std::size_t(n));offset+=std::size_t(n);
                    },progress);
                    p.filters.stack->items[mask.index].mask=core::AdjustmentMask{
                        core::SelectionMask::fromR8(mask.extent,coverage,mask.extent.width),mask.localToMask};
                }
                require(core::validSpatialFilters(*p.filters.stack),"Invalid restored spatial-filter stack");
                p.layer.filters=std::move(p.filters.stack);
            }
            if (p.adjustments.stack) {
                for (const auto& mask : p.adjustments.masks) {
                    std::vector<std::uint8_t> coverage(std::size_t(mask.extent.width) * mask.extent.height);
                    std::size_t offset = 0;
                    readEntry(zip, files.at(mask.path), [&](const char* b, int n) {
                        std::memcpy(coverage.data() + offset, b, std::size_t(n)); offset += std::size_t(n);
                    }, progress);
                    p.adjustments.stack->items[mask.index].mask = core::AdjustmentMask {
                        core::SelectionMask::fromR8(mask.extent, coverage, mask.extent.width), mask.localToMask};
                }
                require(core::validAdjustments(*p.adjustments.stack), "Invalid restored adjustment stack");
                p.layer.adjustments = std::move(p.adjustments.stack);
            }
            if (!p.path.isEmpty()) {
                std::vector<std::byte> pixels(size_t(quint64(p.extent.width) * p.extent.height * 4));
                size_t offset = 0;
                readEntry(zip, files.at(p.path), [&](const char* b, int n) {
                    std::memcpy(pixels.data()+offset,b,size_t(n)); offset += size_t(n); }, progress);
                p.layer.payload = core::RasterLayer { std::make_shared<core::ContiguousRasterSurface>(p.extent, std::move(pixels)) };
            } else if (const auto* text = std::get_if<core::TextLayer>(&p.layer.payload)) {
                auto checkFont = [&](const core::TextStyle& style) {
                    const auto family = QString::fromStdString(style.font.family);
                    if (!families.contains(family, Qt::CaseInsensitive) && family != "Sans Serif"
                        && family != "Serif" && family != "Monospace")
                        missing.insert(family);
                };
                checkFont(text->defaultStyle);
                for (const auto& run : text->runs)
                    checkFont(run.style);
            }
            require(doc->insertLayer(doc->layers().size(), std::move(p.layer)), "Cannot restore layer");
        }
        require(doc->tree() == plan.tree || doc->replaceStructure(doc->tree(), std::move(plan.tree)),
            "Cannot restore layer hierarchy");
        zip.finish();
        progress.tick();
        doc->markSaved();
        for (const auto& family : missing)
            result.warnings.append(QStringLiteral("Font '%1' is unavailable; a substitute will render it. The requested font is preserved.").arg(family));
        // Reserve neither leaf nor container IDs until decoding, validation,
        // cancellation checks and warning allocation have all succeeded.
        for (const auto& layer : doc->layers())
            core::reserveLayerId(layer.id);
        for (const auto& container : doc->tree().containers)
            core::reserveLayerId(container.id);
        result.document = std::move(doc);
    } catch (const Cancelled&) {
        result.cancelled = true;
    } catch (const std::exception& e) {
        result.error = QString::fromUtf8(e.what());
    }
    return result;
}

ProjectIoResult saveProject(const QString& path, const core::Document& doc, const QJsonObject& metadata, ProjectProgress callback)
{
    try {
        require(!path.isEmpty() && !path.contains(QChar::Null), "Invalid project path");
        const auto manifest = makeManifest(doc, metadata);
        const auto json = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        require(quint64(json.size()) <= manifestBudget, "Project manifest exceeds 8 MiB limit");
        Entries expected { { "manifest.json", { 0, quint64(json.size()), 0 } } };
        quint64 bytes = quint64(json.size());
        const auto remembered = doc.selection() ? doc.selection() : doc.lastSelection();
        if (remembered) {
            const auto e=remembered->extent(); const auto n=quint64(e.width)*e.height;
            expected.emplace("selections/last.r8",Entry{0,n,0}); bytes+=n;
        }
        for (const auto& layer : doc.layers())
            if (const auto* r = std::get_if<core::RasterLayer>(&layer.payload)) {
                const auto e = r->surface->extent();
                const auto n = quint64(e.width) * e.height * 4;
                expected.emplace(QStringLiteral("rasters/%1.rgba").arg(layer.id), Entry { 0, n, 0 });
                bytes += n;
            }
        for (const auto& layer : doc.layers()) {
            if (layer.mask) {
                const auto e=layer.mask->coverage->extent();const auto n=quint64(e.width)*e.height;
                expected.emplace(QStringLiteral("layer-masks/%1.r8").arg(layer.id),Entry{0,n,0});bytes+=n;
            }
            if (!layer.adjustments) continue;
            for (const auto& adjustment : layer.adjustments->items) {
                if (!adjustment.mask) continue;
                const auto e = adjustment.mask->coverage->extent();
                const auto n = quint64(e.width) * e.height;
                expected.emplace(detail::adjustmentMaskPath(layer.id, adjustment.type), Entry {0, n, 0});
                bytes += n;
            }
        }
        for(const auto& layer:doc.layers()) {
            if(!layer.filters)continue;
            for(const auto& filter:layer.filters->items) {
                if(!filter.mask)continue;
                const auto e=filter.mask->coverage->extent();const auto n=quint64(e.width)*e.height;
                expected.emplace(detail::spatialFilterMaskPath(layer.id,filter.type),Entry{0,n,0});bytes+=n;
            }
        }
        (void)validateManifest(manifest, expected); // Same structural checks for writer and reader.
        Progress progress { std::move(callback), 0, bytes * 2 };
        progress.tick();
        const QFileInfo target(path);
        QTemporaryFile temp(target.absolutePath() + "/.vulkana-save-XXXXXX");
        require(temp.open(), "Cannot create temporary project beside destination");
        {
            Archive zip(temp, true);
            writeEntry(zip, "manifest.json", quint64(json.size()), [&](const auto& write) { write(json.constData(), int(json.size())); }, progress);
            if (remembered) {
                const auto e=remembered->extent();
                const auto rows=std::max<std::size_t>(1,chunkSize/e.width);
                std::vector<std::uint8_t> buffer(rows*e.width);
                writeEntry(zip,"selections/last.r8",quint64(e.width)*e.height,[&](const auto& write) {
                    for (std::uint32_t y=0;y<e.height;) {
                        const auto h=std::uint32_t(std::min<std::size_t>(rows,e.height-y));
                        for (std::uint32_t r=0;r<h;++r) for (std::uint32_t x=0;x<e.width;++x)
                            buffer[std::size_t(r)*e.width+x]=remembered->coverageAtDocumentPixel(int(x),int(y+r));
                        write(reinterpret_cast<const char*>(buffer.data()),int(std::size_t(h)*e.width)); y+=h;
                    }
                },progress);
            }
            for (const auto& layer : doc.layers())
                if (const auto* r = std::get_if<core::RasterLayer>(&layer.payload)) {
                    const auto e = r->surface->extent();
                    const size_t stride = size_t(e.width) * 4;
                    const auto rows = std::max<size_t>(1, chunkSize / stride);
                    std::vector<std::byte> buffer(rows * stride);
                    writeEntry(zip, QStringLiteral("rasters/%1.rgba").arg(layer.id), quint64(e.width) * e.height * 4, [&](const auto& write) {
                    for (uint32_t y = 0; y < e.height;) {
                        const auto h = uint32_t(std::min<size_t>(rows,e.height-y));
                        r->surface->copyRgba8({0,int32_t(y),int32_t(e.width),int32_t(h)},buffer,stride);
                        write(reinterpret_cast<const char*>(buffer.data()),int(h*stride)); y += h;
                    } }, progress);
                }
            for (const auto& layer : doc.layers()) {
                if (layer.mask) {
                    const auto& coverage=*layer.mask->coverage;
                    const auto e=coverage.extent();const auto rows=std::max<size_t>(1,chunkSize/e.width);
                    std::vector<uint8_t> buffer(rows*e.width);
                    writeEntry(zip,QStringLiteral("layer-masks/%1.r8").arg(layer.id),quint64(e.width)*e.height,[&](const auto& write) {
                        for(uint32_t y=0;y<e.height;) {
                            const auto h=uint32_t(std::min<size_t>(rows,e.height-y));
                            for(uint32_t r=0;r<h;++r)for(uint32_t x=0;x<e.width;++x)
                                buffer[size_t(r)*e.width+x]=coverage.coverageAtDocumentPixel(int(x),int(y+r));
                            write(reinterpret_cast<const char*>(buffer.data()),int(size_t(h)*e.width));y+=h;
                        }
                    },progress);
                }
                if (!layer.adjustments) continue;
                for (const auto& adjustment : layer.adjustments->items) {
                    if (!adjustment.mask) continue;
                    const auto& coverage = *adjustment.mask->coverage;
                    const auto e = coverage.extent();
                    const auto rows = std::max<std::size_t>(1, chunkSize / e.width);
                    std::vector<std::uint8_t> buffer(rows * e.width);
                    writeEntry(zip, detail::adjustmentMaskPath(layer.id, adjustment.type), quint64(e.width) * e.height,
                        [&](const auto& write) {
                            for (std::uint32_t y = 0; y < e.height;) {
                                const auto height = std::uint32_t(std::min<std::size_t>(rows, e.height - y));
                                for (std::uint32_t row = 0; row < height; ++row)
                                    for (std::uint32_t x = 0; x < e.width; ++x)
                                        buffer[std::size_t(row) * e.width + x] = coverage.coverageAtDocumentPixel(int(x), int(y + row));
                                write(reinterpret_cast<const char*>(buffer.data()), int(std::size_t(height) * e.width));
                                y += height;
                            }
                        }, progress);
                }
            }
            for(const auto& layer:doc.layers()) {
                if(!layer.filters)continue;
                for(const auto& filter:layer.filters->items) {
                    if(!filter.mask)continue;
                    const auto& coverage=*filter.mask->coverage;const auto e=coverage.extent();
                    const auto rows=std::max<std::size_t>(1,chunkSize/e.width);
                    std::vector<std::uint8_t> buffer(rows*e.width);
                    writeEntry(zip,detail::spatialFilterMaskPath(layer.id,filter.type),quint64(e.width)*e.height,
                        [&](const auto& write){
                            for(std::uint32_t y=0;y<e.height;) {
                                const auto h=std::uint32_t(std::min<std::size_t>(rows,e.height-y));
                                for(std::uint32_t r=0;r<h;++r)for(std::uint32_t x=0;x<e.width;++x)
                                    buffer[std::size_t(r)*e.width+x]=coverage.coverageAtDocumentPixel(int(x),int(y+r));
                                write(reinterpret_cast<const char*>(buffer.data()),int(std::size_t(h)*e.width));y+=h;
                            }
                        },progress);
                }
            }
            zip.finish();
        }
        require(temp.flush() && ::fsync(temp.handle()) == 0, "Cannot flush project (disk full or I/O failure)");
        require(temp.seek(0), "Cannot validate temporary project");
        {
            Archive verify(temp, false);
            const auto files = entries(verify);
            require(files.size() == expected.size(), "Written archive directory is incomplete");
            for (const auto& [name, entry] : files) {
                require(expected.contains(name) && expected.at(name).bytes == entry.bytes, "Written archive payload mismatch");
                readEntry(verify, entry, [](const char*, int) { }, progress);
            }
            verify.finish();
        }
        progress.tick(); // Cancellation remains safe until the atomic rename.
        const auto source = QFile::encodeName(temp.fileName()), destination = QFile::encodeName(target.absoluteFilePath());
        temp.close();
        // Linux-first, same-directory POSIX rename is atomic and replaces only
        // after complete CRC verification. There is deliberately no direct-write fallback.
        if (::rename(source.constData(), destination.constData()) != 0)
            throw std::runtime_error(std::string("Cannot replace project: ") + std::strerror(errno));
        temp.setAutoRemove(false);
        return { };
    } catch (const Cancelled&) {
        return { { }, true };
    } catch (const std::exception& e) {
        return { QString::fromUtf8(e.what()), false };
    }
}
}
