#include "imageeditor/ui/PdfImport.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPdfDocument>
#include <QRegularExpression>
#include <QRunnable>
#include <QSettings>
#include <QThreadPool>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace imageeditor::ui {
struct PdfSource {
    QByteArray bytes;
    QString password, name;
};
namespace {
constexpr qint64 maximumFileBytes = 128LL * 1024 * 1024;
constexpr int maximumPages = 4096;
// PDFium has a global mutex in Qt PDF. Serializing whole jobs also bounds decoder
// scratch and prevents an arbitrary queue of final-page allocations.
struct Worker {
    QThreadPool pool;
    std::atomic<int> queued {0};
    Worker() { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
    ~Worker() { pool.waitForDone(); }
};
Worker& worker() { static Worker value; return value; }
template<class F> auto submit(F work)
{
    using Result = decltype(work());
    auto& w = worker();
    if (w.queued.fetch_add(1) >= 4) {
        --w.queued;
        std::promise<Result> promise;
        Result result; result.error = QStringLiteral("PDF import is busy. Try again after the current page finishes.");
        promise.set_value(std::move(result));
        return promise.get_future();
    }
    auto task = std::make_shared<std::packaged_task<Result()>>([work = std::move(work), &w]() mutable {
        struct Release { Worker& w; ~Release() { --w.queued; } } release {w};
        try { return work(); }
        catch (const std::bad_alloc&) {
            Result r; r.error = QStringLiteral("Not enough memory to import this PDF. Select fewer pages or lower the PPI."); return r;
        } catch (const std::exception&) {
            // Parser exception strings may contain document/password data. Keep
            // recoverable errors useful without logging or exposing that data.
            Result r; r.error = QStringLiteral("The PDF could not be read safely. It may be damaged or unsupported."); return r;
        }
    });
    auto result = task->get_future();
    w.pool.start(QRunnable::create([task] { (*task)(); }));
    return result;
}
QString pdfError(QPdfDocument::Error error)
{
    switch (error) {
    case QPdfDocument::Error::None: return {};
    case QPdfDocument::Error::IncorrectPassword: return QStringLiteral("Enter the PDF password. If you already entered one, it was not accepted.");
    case QPdfDocument::Error::UnsupportedSecurityScheme: return QStringLiteral("This PDF uses unsupported encryption.");
    case QPdfDocument::Error::FileNotFound: return QStringLiteral("The PDF file is no longer available.");
    default: return QStringLiteral("The PDF is damaged, incomplete or unsupported.");
    }
}
struct OpenPdf {
    QBuffer buffer;
    QPdfDocument document;
    explicit OpenPdf(const PdfSource& source) {
        buffer.setData(source.bytes); buffer.open(QIODevice::ReadOnly);
        document.setPassword(source.password); document.load(&buffer);
    }
};
QImage render(QPdfDocument& document, int page, QSize size, const PdfOptions& options)
{
    QPdfDocumentRenderOptions settings;
    if (options.annotations) settings.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    // No LCD subpixel rendering, viewport scaling, color keying or second rotation.
    auto image = document.render(page, size, settings);
    if (image.isNull() || image.size() != size) throw std::runtime_error("PDF render failed");
    image.setColorSpace(QColorSpace::SRgb); // PDFium's device RGB output; not print proofing.
    if (options.whitePaper) {
        QImage paper(size, QImage::Format_RGB32);
        if (paper.isNull()) throw std::bad_alloc();
        paper.fill(Qt::white);
        QPainter painter(&paper); painter.drawImage(0, 0, image); painter.end();
        paper.setColorSpace(QColorSpace::SRgb);
        return paper;
    }
    return image;
}
QSizeF effectivePoints(QPdfDocument& pdf, int index, QPDFPageObjectHelper& page)
{
    const auto rendererPoints = pdf.pagePointSize(index);
    auto unitObject = page.getObjectHandle().getKey("/UserUnit"); // Not an inheritable PDF attribute.
    const double unit = unitObject.isNull() ? 1.0 : unitObject.getNumericValue();
    if (!std::isfinite(unit) || unit <= 0 || unit > 75000) throw std::runtime_error("Invalid UserUnit");
    {
        // Qt/PDFium reports float-rounded dimensions and currently ignores
        // UserUnit. Use qpdf's precise box numbers with the SAME intersection /
        // rotation contract; check agreement before trusting that contract.
        const auto box = [](QPDFObjectHandle handle) {
            if (!handle.isRectangle()) throw std::runtime_error("Invalid page box");
            const auto r = handle.getArrayAsRectangle();
            return QRectF(QPointF(r.llx, r.lly), QPointF(r.urx, r.ury)).normalized();
        };
        auto area = box(page.getMediaBox()).intersected(box(page.getCropBox()));
        if (area.isEmpty()) throw std::runtime_error("Empty effective page");
        auto unscaled = area.size();
        auto rotation = page.getAttribute("/Rotate", false);
        if (rotation.isInteger() && (std::abs(rotation.getIntValue() / 90) % 2)) unscaled.transpose();
        const auto near = [](QSizeF a, QSizeF b) {
            return std::abs(a.width()-b.width()) <= std::max(0.01, b.width()*1e-5)
                && std::abs(a.height()-b.height()) <= std::max(0.01, b.height()*1e-5);
        };
        if (!near(rendererPoints, unscaled) && !near(rendererPoints, unscaled * unit))
            throw std::runtime_error("Page geometry disagreement");
        const auto points = unscaled * unit;
        if (!std::isfinite(points.width()) || !std::isfinite(points.height()) || points.isEmpty())
            throw std::runtime_error("Invalid page geometry");
        return points;
    }
}
}

bool isPdfFile(const QString& path)
{
    if (QFileInfo(path).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) return true;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(1024).contains("%PDF-");
}
std::vector<int> parsePdfPageRange(const QString& text, int count, QString& error)
{
    error.clear(); std::vector<int> result;
    if (text.trimmed().isEmpty()) return result;
    if (count <= 0 || count > maximumPages || text.size() > 32768) {
        error = QStringLiteral("Invalid page range."); return {};
    }
    const QRegularExpression item(QStringLiteral("^\\s*([0-9]+)\\s*(?:-\\s*([0-9]+)\\s*)?$"));
    for (const auto& part : text.split(',')) {
        auto match = item.match(part);
        bool firstOk = false, lastOk = false;
        const auto first = match.captured(1).toInt(&firstOk);
        const auto last = match.captured(2).isEmpty() ? first : match.captured(2).toInt(&lastOk);
        if (!match.hasMatch() || !firstOk || (!match.captured(2).isEmpty() && !lastOk)
            || first < 1 || last < first || last > count) {
            error = QStringLiteral("Use page numbers from 1 to %1, for example 1, 3-5. Ranges must run forwards.").arg(count);
            return {};
        }
        for (int i = first-1; i < last; ++i) result.push_back(i);
    }
    std::ranges::sort(result); result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
QString formatPdfPageRange(const std::vector<int>& pages)
{
    QStringList parts;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto first = pages[i];
        while (i+1 < pages.size() && pages[i+1] == pages[i]+1) ++i;
        parts.append(first == pages[i] ? QString::number(first+1)
            : QStringLiteral("%1-%2").arg(first+1).arg(pages[i]+1));
    }
    return parts.join(QStringLiteral(", "));
}
PdfPlan planPdfImport(const PdfMetadata& metadata, const PdfOptions& options, const PdfLimits& limits)
{
    PdfPlan result;
    const auto fail = [&](QString message) { result.error = std::move(message); return result; };
    if (options.pages.empty()) return fail(QStringLiteral("Select at least one page."));
    if (!std::isfinite(options.ppi) || options.ppi < 1 || options.ppi > 2400)
        return fail(QStringLiteral("Choose a resolution from 1 to 2400 PPI."));
    const bool current = options.destination == PdfDestination::CurrentDocument;
    if (options.pages.size() > (current ? std::min(limits.availableLayers, limits.currentDocumentLayers) : limits.availableLayers))
        return fail(QStringLiteral("Too many pages for the destination layer limit."));
    int previous = -1; std::uint64_t largest = 0;
    for (int page : options.pages) {
        if (page <= previous || page < 0 || std::size_t(page) >= metadata.pages.size())
            return fail(QStringLiteral("Invalid page selection."));
        previous = page;
        const auto points = metadata.pages[std::size_t(page)].points;
        if (!std::isfinite(points.width()) || !std::isfinite(points.height()) || points.isEmpty())
            return fail(QStringLiteral("Invalid PDF page dimensions."));
        const auto w = std::max(1.0, std::floor(points.width() * options.ppi / 72.0 + 0.5));
        const auto h = std::max(1.0, std::floor(points.height() * options.ppi / 72.0 + 0.5));
        if (points.isEmpty() || !std::isfinite(w) || !std::isfinite(h)
            || w > limits.maximumDimension || h > limits.maximumDimension
            || w*h > double(kMaximumRasterPixels))
            return fail(QStringLiteral("Page %1 exceeds the %2-pixel side / 40-megapixel limit. Lower the PPI.")
                .arg(page+1).arg(limits.maximumDimension));
        const QSize size{int(w), int(h)};
        result.sizes.push_back(size); result.canvas = result.canvas.expandedTo(size);
        const auto bytes = std::uint64_t(size.width()) * std::uint64_t(size.height()) * 4;
        result.rasterBytes += bytes; largest = std::max(largest, bytes);
    }
    if (options.destination == PdfDestination::NewDocument && !rasterExtentWithinLimits(
        std::uint32_t(result.canvas.width()), std::uint32_t(result.canvas.height())))
        return fail(QStringLiteral("The combined canvas exceeds the raster limit. Choose separate document tabs or fewer pages."));
    // Retained pixels + potential GPU upload/storage, two page-sized conversion
    // buffers, PDF snapshot, and a decoder reserve. History shares inserted data.
    result.estimatedWorkingBytes = limits.existingBytes + result.rasterBytes * 2 + largest * 2
        + 128ULL*1024*1024 + (metadata.source ? std::uint64_t(metadata.source->bytes.size()) : 0);
    const auto rasterBudget = current ? std::min(limits.rasterBytes, limits.currentDocumentRasterBytes) : limits.rasterBytes;
    if (result.rasterBytes > rasterBudget || result.estimatedWorkingBytes > limits.workingBytes)
        return fail(QStringLiteral("Import exceeds the memory budget (%1 MiB of page pixels; %2 MiB estimated working memory). Select fewer pages or lower the PPI.")
            .arg(result.rasterBytes/1048576).arg(result.estimatedWorkingBytes/1048576));
    return result;
}
std::future<PdfMetadata> readPdfMetadata(QString path, QString password, std::shared_ptr<PdfJobState> state)
{
    return submit([path = std::move(path), password = std::move(password), state] {
        PdfMetadata result;
        if (state->cancelled) return result;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Cannot open the PDF. Check its location and permissions."); return result; }
        if (file.size() <= 0 || file.size() > maximumFileBytes) {
            result.error = QStringLiteral("PDF input is limited to 128 MiB."); return result;
        }
        auto source = std::make_shared<PdfSource>();
        source->bytes = file.read(maximumFileBytes+1); source->password = password;
        source->name = QFileInfo(path).completeBaseName();
        if (source->bytes.size() != file.size() || source->bytes.size() > maximumFileBytes) {
            result.error = QStringLiteral("The PDF changed or could not be read completely. Try again."); return result;
        }
        OpenPdf pdf(*source);
        result.error = pdfError(pdf.document.error());
        result.passwordRequired = pdf.document.error() == QPdfDocument::Error::IncorrectPassword;
        if (!result.error.isEmpty()) return result;
        const int count = pdf.document.pageCount();
        if (count <= 0 || count > maximumPages) { result.error = QStringLiteral("PDFs must contain between 1 and 4096 pages."); return result; }
        QPDF metadata; metadata.setSuppressWarnings(true); metadata.setAttemptRecovery(false);
        const auto utf8Password = password.toUtf8();
        metadata.processMemoryFile("PDF import", source->bytes.constData(), std::size_t(source->bytes.size()), utf8Password.constData());
        auto pages = QPDFPageDocumentHelper(metadata).getAllPages();
        if (pages.size() != std::size_t(count)) throw std::runtime_error("Page count disagreement");
        for (int i = 0; i < count; ++i) {
            if (state->cancelled) return PdfMetadata{};
            result.pages.push_back({effectivePoints(pdf.document, i, pages[std::size_t(i)]), pdf.document.pageLabel(i)});
        }
        result.source = std::move(source);
        return result;
    });
}
std::future<PdfThumbnail> renderPdfThumbnail(std::shared_ptr<const PdfSource> source, int page,
    QSize size, PdfOptions options, std::shared_ptr<PdfJobState> state)
{
    return submit([source, page, size, options = std::move(options), state] {
        PdfThumbnail result;
        if (!source || state->cancelled) return result;
        if (size.width() <= 0 || size.height() <= 0 || size.width() > 256 || size.height() > 256)
            throw std::runtime_error("Invalid thumbnail size");
        OpenPdf pdf(*source);
        if (pdf.document.error() != QPdfDocument::Error::None) throw std::runtime_error("PDF load failed");
        result.image = render(pdf.document, page, size, options);
        if (state->cancelled) result.image = {};
        return result;
    });
}
std::future<PdfRenderedPages> renderPdfPages(PdfMetadata metadata, PdfOptions options,
    PdfLimits limits, std::shared_ptr<PdfJobState> state)
{
    return submit([metadata = std::move(metadata), options = std::move(options), limits, state] {
        PdfRenderedPages result;
        const auto plan = planPdfImport(metadata, options, limits);
        if (!plan || !metadata.source) { result.error = plan.error; return result; }
        if (state->cancelled) { result.cancelled = true; return result; }
        OpenPdf pdf(*metadata.source);
        if (pdf.document.error() != QPdfDocument::Error::None) throw std::runtime_error("PDF load failed");
        for (std::size_t i = 0; i < options.pages.size(); ++i) {
            if (state->cancelled) { result.pages.clear(); result.cancelled = true; return result; }
            const auto page = options.pages[i]; state->physicalPage = page+1;
            QElapsedTimer timer; timer.start();
            auto image = render(pdf.document, page, plan.sizes[i], options);
            if (state->cancelled) { result.pages.clear(); result.cancelled = true; return result; }
            auto layer = rasterLayerFromImage(std::move(image), QStringLiteral("%1 — Page %2").arg(metadata.source->name).arg(page+1));
            if (!layer) { result.pages.clear(); result.error = layer.error; return result; }
            result.pages.push_back({page, std::move(*layer.layer), plan.sizes[i], timer.elapsed()});
            state->completed = int(i+1);
        }
        return result;
    });
}
PdfOptions pdfImportPreferences()
{
    QSettings settings; PdfOptions options;
    const auto ppi = settings.value(QStringLiteral("pdfImport/ppi"), 300).toDouble();
    if (std::isfinite(ppi) && ppi >= 1 && ppi <= 2400) options.ppi = ppi;
    options.whitePaper = settings.value(QStringLiteral("pdfImport/whitePaper"), true).toBool();
    options.annotations = settings.value(QStringLiteral("pdfImport/annotations"), true).toBool();
    return options;
}
void savePdfImportPreferences(const PdfOptions& options)
{
    QSettings settings;
    settings.setValue(QStringLiteral("pdfImport/ppi"), options.ppi);
    settings.setValue(QStringLiteral("pdfImport/whitePaper"), options.whitePaper);
    settings.setValue(QStringLiteral("pdfImport/annotations"), options.annotations);
}
} // namespace imageeditor::ui
