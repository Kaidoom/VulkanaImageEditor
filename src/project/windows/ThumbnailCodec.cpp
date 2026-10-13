#include "ThumbnailProvider.hpp"
#include "imageeditor/project/ProjectPreview.hpp"
#include <limits>
#include <cstring>

namespace {
class StreamDevice final : public QIODevice {
    IStream* stream_;
    qint64 length_;
public:
    StreamDevice(IStream* stream, qint64 length) : stream_(stream), length_(length)
    { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
    qint64 size() const override { return length_; }
    bool isSequential() const override { return false; }
    bool seek(qint64 offset) override
    {
        if (offset < 0 || offset > length_) return false;
        LARGE_INTEGER target; target.QuadPart = offset;
        return SUCCEEDED(stream_->Seek(target, STREAM_SEEK_SET, nullptr)) && QIODevice::seek(offset);
    }
protected:
    qint64 readData(char* destination, qint64 maximum) override
    {
        ULONG count {};
        const auto result = stream_->Read(destination, ULONG(std::min<qint64>(maximum, std::numeric_limits<ULONG>::max())), &count);
        return SUCCEEDED(result) ? qint64(count) : -1;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
};
}
extern "C" __declspec(dllexport) HRESULT WINAPI VulkanaReadThumbnail(
    IStream* stream, UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    if (!stream || !bitmap || !alpha || !size || size > 4096) return E_INVALIDARG;
    *bitmap = nullptr; *alpha = WTSAT_UNKNOWN;
    try {
        STATSTG info {};
        if (FAILED(stream->Stat(&info, STATFLAG_NONAME)) || info.cbSize.QuadPart > quint64(INT64_MAX)) return E_FAIL;
        StreamDevice source(stream, qint64(info.cbSize.QuadPart));
        const auto image = imageeditor::project::readPreview(source, int(size))
            .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) return E_FAIL; // Missing/invalid/stale previews use the project icon.
        BITMAPINFO description {};
        description.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        description.bmiHeader.biWidth = image.width();
        description.bmiHeader.biHeight = -image.height(); // Top-down premultiplied BGRA.
        description.bmiHeader.biPlanes = 1;
        description.bmiHeader.biBitCount = 32;
        description.bmiHeader.biCompression = BI_RGB;
        void* pixels {};
        const auto output = CreateDIBSection(nullptr, &description, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!output) return E_OUTOFMEMORY;
        for (int y = 0; y < image.height(); ++y)
            std::memcpy(static_cast<char*>(pixels) + y * image.width() * 4, image.constScanLine(y), size_t(image.width()) * 4);
        *bitmap = output; *alpha = WTSAT_ARGB; return S_OK;
    } catch (...) { return E_FAIL; } // Never unwind through the COM host.
}
