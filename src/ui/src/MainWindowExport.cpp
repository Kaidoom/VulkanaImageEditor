#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <deque>
#include <set>
#include <thread>

namespace imageeditor::ui {
namespace {
    bool usesMatte(const ExportSettings& s)
    {
        return s.format == ExportFormat::Jpeg || (s.format == ExportFormat::Png && s.pngMatte);
    }
    bool sameRendering(const ExportSettings& a, const ExportSettings& b)
    {
        return a.size == b.size && usesMatte(a) == usesMatte(b)
            && (!usesMatte(a) || exportMatteColor(a).rgb() == exportMatteColor(b).rgb());
    }
    bool sameEncoding(const ExportSettings& a, const ExportSettings& b)
    {
        if (a.format != b.format || !sameRendering(a, b))
            return false;
        switch (a.format) {
        case ExportFormat::Png:
            return a.pngEffort == b.pngEffort;
        case ExportFormat::Jpeg:
            return a.jpegQuality == b.jpegQuality;
        case ExportFormat::WebP:
            return a.webpLossless == b.webpLossless && a.webpEffort == b.webpEffort
                && (a.webpLossless || a.webpQuality == b.webpQuality);
        case ExportFormat::Pdf:
            return a.pdf==b.pdf;
        case ExportFormat::Psd:
            return a.psd==b.psd;
        }
        return false;
    }
    struct EncodeJob {
        std::atomic_bool cancel { false }, done { false };
        EncodedExport result;
        std::thread worker;
        ~EncodeJob()
        {
            if (worker.joinable())
                worker.join();
        }
    };
    struct WriteJob {
        std::atomic_bool cancel { false }, done { false };
        ExportWriteResult result;
        ExportSettings settings;
        qint64 bytes { 0 };
        std::thread worker;
        ~WriteJob()
        {
            if (worker.joinable())
                worker.join();
        }
    };
    struct PdfJob {
        enum class Kind { Plan, Preview, Thumbnail, Write } kind{Kind::Plan};
        std::atomic_bool cancel{false},done{false};
        std::atomic_int progressPage{0},progressTotal{0},progressStage{0};
        PdfExportPlan plan;
        ExportSettings settings;
        FlattenedDocumentResult preview;
        PdfExportResult written;
        int page{0};core::LayerId thumbnail{0};
        std::thread worker;
        ~PdfJob(){if(worker.joinable())worker.join();}
    };
    struct PsdJob {
        bool writing{false}, renderPreview{true};
        std::atomic_bool cancel{false},done{false};
        std::atomic_int progress{0},total{0};
        ExportSettings settings;
        PsdExportPlan plan;
        FlattenedDocumentResult preview;
        PsdExportResult result;
        std::thread worker;
        ~PsdJob(){if(worker.joinable())worker.join();}
    };

    // Only this owner-thread coordinator sees Document. Codec tasks own
    // immutable output images/bytes, never shared mutable RasterSurfaces or UI.
    // At most one encoder runs at once. Stale jobs cannot publish preview or files.
    class ExportCoordinator final : public QObject {
    public:
        ExportCoordinator(const core::Document& document, ExportDialog& dialog, QString projectPath, QString sourcePath,
            std::uint64_t instanceId,std::vector<core::LayerId> selection,PdfExportLimits pdfLimits,
            bool persist, std::function<void(const ExportSettings&, qint64)> success)
            : document_(document)
            , dialog_(dialog)
            , projectPath_(std::move(projectPath))
            , sourcePath_(std::move(sourcePath)),instanceId_(instanceId),selection_(std::move(selection))
            , pdfLimits_(pdfLimits)
            , persist_(persist)
            , success_(std::move(success))
        {
            debounce_.setSingleShot(true);
            debounce_.setInterval(180);
            poll_.setInterval(20);
            connect(&debounce_, &QTimer::timeout, this, [this] { prepare(); });
            connect(&poll_, &QTimer::timeout, this, [this] { poll(); });
            dialog_.onSettingsChanged = [this] { changed(); };
            dialog_.onExportRequested = [this] { requestWrite(); };
            dialog_.onCancelRequested = [this] { cancel(false); };
            dialog_.onCloseRequested = [this] { cancel(true); };
            dialog_.onPdfPageChanged=[this]{
                if (closing_ || cancelling_) return;
                if(pdfJob_&&pdfJob_->kind!=PdfJob::Kind::Write)pdfJob_->cancel=true;
                pdfPreview_={};debounce_.start();
            };
            dialog_.onPdfThumbnailsRequested=[this]{prepareThumbnail();};
            dialog_.onPsdPreviewChanged=[this]{changed();};
            debounce_.start();
        }
        ~ExportCoordinator() override
        {
            renderCancel_ = true;
            if (encode_)
                encode_->cancel = true;
            if (write_)
                write_->cancel = true;
            if(pdfJob_)pdfJob_->cancel=true;
            if(psdJob_)psdJob_->cancel=true;
            dialog_.onSettingsChanged = { };
            dialog_.onExportRequested = { };
            dialog_.onCancelRequested = { };
            dialog_.onCloseRequested = { };
            dialog_.onPdfPageChanged={};dialog_.onPdfThumbnailsRequested={};
            dialog_.onPsdPreviewChanged={};
        }
        void requestWrite()
        {
            if (write_ || closing_ || cancelling_ || pendingWrite_ || (pdfJob_&&pdfJob_->kind==PdfJob::Kind::Write) || (psdJob_&&psdJob_->writing))
                return;
            auto s = dialog_.settings();
            if(s.format==ExportFormat::Pdf&&!dialog_.pdfInputError().isEmpty()){
                dialog_.setError(dialog_.pdfInputError());return;
            }
            const auto error = validateExport(s, canvasSize());
            if (!error.isEmpty()) {
                dialog_.setError(error);
                return;
            }
            // Refuse project destinations before extension reconciliation too.
            if (const auto destinationError = validateExportDestination(s.destination, projectPath_);
                !destinationError.isEmpty()) {
                dialog_.setError(destinationError);
                return;
            }
            const auto proposed = exportPathForFormat(s.destination, s.format);
            if (proposed != s.destination) {
                if (QMessageBox::question(popupTopLevelOwner(&dialog_), tr("Match image extension?"),
                        tr("The selected encoding is %1. Use the matching filename %2?")
                            .arg(QString::fromLatin1(exportFormatName(s.format)).toUpper(),
                                QFileInfo(proposed).fileName()),
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    != QMessageBox::Yes)
                    return;
                s.destination = proposed;
                dialog_.setDestination(proposed);
            }
            s.destination = QFileInfo(s.destination).absoluteFilePath();
            if(s.format==ExportFormat::Pdf&&!sourcePath_.isEmpty()
                &&QFileInfo(sourcePath_).suffix().compare("pdf",Qt::CaseInsensitive)==0
                &&(QFileInfo(s.destination).absoluteFilePath()==QFileInfo(sourcePath_).absoluteFilePath()
                    ||(!QFileInfo(sourcePath_).canonicalFilePath().isEmpty()
                        &&QFileInfo(s.destination).canonicalFilePath()==QFileInfo(sourcePath_).canonicalFilePath()))){
                dialog_.setError(tr("Choose a different destination; the imported source PDF is never overwritten."));return;
            }
            if(s.format==ExportFormat::Psd&&!sourcePath_.isEmpty()
                &&QFileInfo(sourcePath_).suffix().compare("psd",Qt::CaseInsensitive)==0
                &&(QFileInfo(s.destination).absoluteFilePath()==QFileInfo(sourcePath_).absoluteFilePath()
                    ||(!QFileInfo(sourcePath_).canonicalFilePath().isEmpty()&&QFileInfo(s.destination).canonicalFilePath()==QFileInfo(sourcePath_).canonicalFilePath()))
                &&QMessageBox::question(popupTopLevelOwner(&dialog_),tr("Replace source PSD?"),
                    tr("This is the imported source PSD. Vulkana exports its supported subset, not a lossless PSD round-trip. Replace the original?"),
                    QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
            if (const auto destinationError = validateExportDestination(s.destination, projectPath_);
                !destinationError.isEmpty()) {
                dialog_.setError(destinationError);
                return;
            }
            if (QFileInfo::exists(s.destination)
                && QMessageBox::question(popupTopLevelOwner(&dialog_), tr("Replace exported image?"),
                       tr("Replace %1? The existing file remains intact "
                          "unless export finishes successfully.")
                           .arg(s.destination),
                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    != QMessageBox::Yes)
                return;
            pendingWrite_ = s;
            dialog_.setProgress(tr("Preparing export…"), true);
            if(s.format==ExportFormat::Pdf||s.format==ExportFormat::Psd){debounce_.stop();prepare();return;}
            if (ready_ && sameEncoding(readySettings_, s))
                publish();
            else {
                debounce_.stop();
                prepare();
            }
        }

    private:
        QSize canvasSize() const
        {
            const auto e = document_.canvas().extent;
            return { int(e.width), int(e.height) };
        }
        void changed()
        {
            if (closing_ || cancelling_ || write_ || (pdfJob_&&pdfJob_->kind==PdfJob::Kind::Write) || (psdJob_&&psdJob_->writing))
                return;
            pendingWrite_.reset(); // An overwrite confirmation never authorizes changed
                                   // settings/path.
            const auto s = dialog_.settings();
            if(pdfJob_)pdfJob_->cancel=true;
            if(psdJob_)psdJob_->cancel=true;
            if (ready_ && sameEncoding(readySettings_, s)) {
                showReadyPreview();
                return;
            }
            if (rendering_)
                renderCancel_ = true;
            if (encode_ && !sameEncoding(encodeSettings_, s))
                encode_->cancel = true;
            debounce_.start();
        }
        void prepare()
        {
            if (closing_ || cancelling_ || write_ || (pdfJob_&&pdfJob_->kind==PdfJob::Kind::Write) || (psdJob_&&psdJob_->writing))
                return;
            if (rendering_) {
                renderCancel_ = true;
                debounce_.start();
                return;
            }
            const auto s = dialog_.settings();
            if(pdfJob_){pdfJob_->cancel=true;poll_.start();return;}
            if(psdJob_){psdJob_->cancel=true;poll_.start();return;}
            if(s.format==ExportFormat::Psd){
                if(encode_){encode_->cancel=true;poll_.start();return;}
                preparePsd();return;
            }
            if(s.format==ExportFormat::Pdf){
                if(encode_){encode_->cancel=true;poll_.start();return;}
                preparePdf();return;
            }
            psdSource_.reset();psdPreview_={};psdPlan_={};
            pdfSource_.reset();pdfPreview_={};pdfPlan_={};pdfThumbnails_.clear();
            if (ready_ && sameEncoding(readySettings_, s)) {
                showReadyPreview();
                if (pendingWrite_)
                    publish();
                return;
            }
            if (encode_) {
                if (!sameEncoding(s, encodeSettings_))
                    encode_->cancel = true;
                poll_.start();
                return; // Coalesce, never launch competing full-image workers.
            }
            if (const auto error = validateExport(s, canvasSize()); !error.isEmpty()) {
                pendingWrite_.reset();
                dialog_.setError(error);
                return;
            }
            if (rendered_.isNull() || !sameRendering(renderSettings_, s)) {
                rendered_ = { };
                ready_ = { };
                dialog_.setProgress(tr("Rendering document…"), pendingWrite_.has_value());
                rendering_ = true;
                renderCancel_ = false;
                QElapsedTimer cadence;
                cadence.start();
                const auto result = renderExport(document_, s, [&](quint64, quint64) {
                    if (cadence.elapsed() >= 8) {
                        QApplication::processEvents(QEventLoop::AllEvents, 4);
                        cadence.restart();
                    }
                    return !renderCancel_ && !closing_;
                });
                rendering_ = false;
                if (cancelling_) {
                    finishCancellation();
                    return;
                }
                if (!result) {
                    if (result.cancelled) {
                        debounce_.start();
                        return;
                    }
                    pendingWrite_.reset();
                    dialog_.setError(result.error);
                    return;
                }
                rendered_ = result.image;
                renderSettings_ = s;
                if (!sameEncoding(s, dialog_.settings())) {
                    debounce_.start();
                    return;
                }
            }
            ready_ = { };
            encode_ = std::make_shared<EncodeJob>();
            encodeSettings_ = s;
            auto* job = encode_.get();
            const auto pixels = rendered_;
            dialog_.setProgress(tr("Encoding %1 and decoding preview…")
                    .arg(QString::fromLatin1(exportFormatName(s.format)).toUpper()), pendingWrite_.has_value());
            try {
                job->worker = std::thread([job, pixels, s] {
                    job->result = encodeExport(pixels, s, job->cancel);
                    job->done.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                encode_.reset();
                pendingWrite_.reset();
                dialog_.setError(QString::fromUtf8(e.what()));
                return;
            }
            poll_.start();
        }
        void poll()
        {
            if(psdJob_){pollPsd();return;}
            if(pdfJob_){pollPdf();return;}
            if (write_) {
                if (!write_->done.load(std::memory_order_acquire))
                    return;
                auto completed = std::move(write_);
                poll_.stop();
                if (completed->result) {
                    cancelling_ = false; // A file already atomically committed is a successful export.
                    if (persist_)
                        saveExportPreferences(completed->settings);
                    dialog_.setExported(
                        completed->settings.destination, completed->settings.size, completed->bytes);
                    success_(completed->settings, completed->bytes);
                } else if (cancelling_) {
                    finishCancellation();
                    return;
                } else if (!completed->result.cancelled)
                    dialog_.setError(completed->result.error);
                if (closing_)
                    dialog_.done(QDialog::Rejected);
                return;
            }
            if (!encode_) {
                poll_.stop();
                return;
            }
            if (!encode_->done.load(std::memory_order_acquire))
                return;
            auto job = std::move(encode_);
            poll_.stop();
            if (cancelling_) {
                finishCancellation();
                return;
            }
            if (job->cancel || !sameEncoding(encodeSettings_, dialog_.settings())) {
                debounce_.start();
                return;
            }
            if (!job->result) {
                pendingWrite_.reset();
                dialog_.setError(job->result.error);
                return;
            }
            ready_ = std::move(job->result);
            readySettings_ = encodeSettings_;
            showReadyPreview();
            if (pendingWrite_)
                publish();
        }
        void launchPdf(std::shared_ptr<PdfJob> job,std::function<void(PdfJob&)> work)
        {
            pdfJob_=std::move(job);auto* current=pdfJob_.get();
            try{current->worker=std::thread([current,work=std::move(work)]{
                try{work(*current);}catch(const std::exception& e){
                    if(current->kind==PdfJob::Kind::Write)current->written.error=QString::fromUtf8(e.what());
                    else current->preview.error=QString::fromUtf8(e.what());
                }
                current->done.store(true,std::memory_order_release);
            });poll_.start();}
            catch(const std::exception& e){pdfJob_.reset();pendingWrite_.reset();dialog_.setError(QString::fromUtf8(e.what()));}
        }
        void preparePsd()
        {
            if(!psdSource_){
                // Do not retain a second frozen format snapshot unnecessarily.
                pdfSource_.reset();pdfPreview_={};pdfPlan_={};pdfThumbnails_.clear();
                rendered_={};ready_={};
                try{PsdExportLimits limits;limits.existingBytes=pdfLimits_.existingBytes;
                    limits.workingBytes=limits.existingBytes+platform::availableWorkingMemoryBytes();
                    psdSource_=std::make_shared<PsdExportSnapshot>(capturePsdExport(document_,instanceId_,limits));}
                catch(const std::exception& e){pendingWrite_.reset();dialog_.setError(QString::fromUtf8(e.what()));return;}
            }
            auto s=dialog_.settings();const auto& cached=psdPlan_.options;
            if(psdPlan_.instanceId&&cached==s.psd&&(!dialog_.psdPreviewEnabled()||!psdPreview_.isNull())){
                if(!pendingWrite_){dialog_.setPsdPlan(psdPlan_,psdPreview_);return;}
            }
            auto job=std::make_shared<PsdJob>();job->settings=s;
            job->renderPreview=dialog_.psdPreviewEnabled();
            job->writing=pendingWrite_.has_value()&&psdPlan_.instanceId&&cached==s.psd&&bool(psdPlan_);
            if(job->writing){job->settings=*pendingWrite_;job->plan=psdPlan_;pendingWrite_.reset();}
            dialog_.setProgress(job->writing?tr("Writing PSD…"):tr("Preparing PSD export review…"),job->writing);
            psdJob_=job;const auto source=psdSource_;
            auto *current=job.get();
            try{job->worker=std::thread([job=current,source]{
                try{
                    if(job->writing)job->result=writePsdExport(*source,job->plan,job->settings.destination,job->cancel,
                        [job](int n,int total,const QString&){job->progress=n;job->total=total;return !job->cancel.load();});
                    else{job->plan=planPsdExport(*source,job->settings.psd,job->cancel);if(job->plan&&job->renderPreview)job->preview=previewPsdExport(*source,job->plan,job->cancel);}
                }catch(const std::exception& e){if(job->writing)job->result.error=QString::fromUtf8(e.what());else job->preview.error=QString::fromUtf8(e.what());}
                job->done.store(true,std::memory_order_release);
            });poll_.start();}
            catch(const std::exception& e){psdJob_.reset();pendingWrite_.reset();dialog_.setError(QString::fromUtf8(e.what()));}
        }
        void pollPsd()
        {
            if(!psdJob_->done.load(std::memory_order_acquire)){
                if(psdJob_->writing&&!closing_&&!cancelling_)dialog_.setProgress(tr("Exporting PSD · layer %1 of %2…").arg(psdJob_->progress.load()).arg(psdJob_->total.load()),true);
                return;
            }
            auto job=std::move(psdJob_);poll_.stop();
            if(job->writing){
                if(job->result){cancelling_=false;if(persist_)saveExportPreferences(job->settings);dialog_.setExported(job->settings.destination,canvasSize(),job->result.bytes);success_(job->settings,job->result.bytes);}
                else if(cancelling_){finishCancellation();return;}
                else if(!job->result.cancelled)dialog_.setError(job->result.error);
                if(closing_)dialog_.done(QDialog::Rejected);
                return;
            }
            if(cancelling_){finishCancellation();return;}
            if(job->cancel||dialog_.settings().format!=ExportFormat::Psd||job->settings.psd!=dialog_.settings().psd){debounce_.start();return;}
            psdPlan_=std::move(job->plan);psdPreview_=std::move(job->preview.image);dialog_.setPsdPlan(psdPlan_,psdPreview_);
            if(!psdPlan_||(dialog_.psdPreviewEnabled()&&psdPreview_.isNull())){pendingWrite_.reset();if(psdPlan_)dialog_.setError(job->preview.error);return;}
            if(pendingWrite_){pendingWrite_->psd=psdPlan_.options;preparePsd();}
        }
        void preparePdf()
        {
            if(!dialog_.pdfInputError().isEmpty()){pendingWrite_.reset();dialog_.setError(dialog_.pdfInputError());return;}
            if(!pdfSource_){
                psdSource_.reset();psdPreview_={};psdPlan_={};
                try{pdfSource_=std::make_shared<PdfExportSnapshot>(capturePdfExport(document_,instanceId_,selection_,pdfLimits_));
                    dialog_.configurePdf(*pdfSource_);debounce_.stop();}
                catch(const std::exception& e){pendingWrite_.reset();dialog_.setError(QString::fromUtf8(e.what()));return;}
            }
            auto s=dialog_.settings();
            if(pendingWrite_){pendingWrite_->pdf=s.pdf;}
            auto job=std::make_shared<PdfJob>();job->settings=s;job->page=dialog_.pdfPreviewPage();
            const auto source=pdfSource_;
            if(pdfPlan_&&pdfPlan_.options==s.pdf){
                if(pendingWrite_){
                    job->kind=PdfJob::Kind::Write;job->settings=*pendingWrite_;job->plan=pdfPlan_;pendingWrite_.reset();
                    dialog_.setProgress(tr("Writing PDF…"),true);
                    launchPdf(job,[source](PdfJob& j){j.written=writePdfExport(*source,j.plan,j.settings.destination,j.cancel,
                        [&j](int page,int total,const QString& stage){j.progressPage=page;j.progressTotal=total;j.progressStage=stage==QStringLiteral("Writing")?2:1;return !j.cancel.load();});});return;
                }
                if(!pdfPreview_.isNull()&&pdfPreviewPage_==job->page){
                    dialog_.setPdfPreview(pdfPlan_,job->page,pdfPreview_);prepareThumbnail();return;
                }
                job->kind=PdfJob::Kind::Preview;job->plan=pdfPlan_;
            }
            dialog_.setProgress(tr("Preparing PDF page preview…"), pendingWrite_.has_value());
            launchPdf(job,[source](PdfJob& j){
                if(j.kind==PdfJob::Kind::Plan)j.plan=planPdfExport(*source,j.settings.pdf,j.cancel);
                if(!j.plan)return;
                j.page=std::clamp(j.page,0,int(j.plan.pages.size())-1);
                auto size=j.plan.pages[std::size_t(j.page)].pixels;
                if(size.width()>800||size.height()>600)size.scale(800,600,Qt::KeepAspectRatio);
                j.preview=renderPdfExportPage(*source,j.plan,std::size_t(j.page),j.cancel,size);
            });
        }
        void prepareThumbnail()
        {
            if(closing_||cancelling_||pdfJob_||psdJob_||encode_||write_||debounce_.isActive()||pendingWrite_||!pdfSource_
                ||dialog_.settings().format!=ExportFormat::Pdf||!pdfPlan_)return;
            for(auto id:dialog_.neededPdfThumbnails()){
                if(std::ranges::find(pdfThumbnails_,id)!=pdfThumbnails_.end())continue;
                auto job=std::make_shared<PdfJob>();job->kind=PdfJob::Kind::Thumbnail;job->thumbnail=id;
                const auto source=pdfSource_;
                launchPdf(job,[source,id](PdfJob& j){
                    PdfExportOptions options;options.mode=PdfExportMode::Pages;options.selection=PdfExportSelection::Custom;
                    options.chosen.push_back(id);options.ignoreHidden=false;options.text=PdfExportText::Rasterize;
                    j.plan=planPdfExport(*source,std::move(options),j.cancel);
                    if(j.plan){const auto size=j.plan.pages.front().pixels.scaled(38,38,Qt::KeepAspectRatio);
                        j.preview=renderPdfExportPage(*source,j.plan,0,j.cancel,size);}
                });return;
            }
        }
        void pollPdf()
        {
            auto& current=*pdfJob_;
            if(!current.done.load(std::memory_order_acquire)){
                if(current.kind==PdfJob::Kind::Write&&!closing_&&!cancelling_)
                    dialog_.setProgress(tr("%1 PDF · page %2 of %3…").arg(current.progressStage==2?tr("Writing"):tr("Rendering"))
                        .arg(current.progressPage.load()).arg(current.progressTotal.load()),true);
                return;
            }
            auto job=std::move(pdfJob_);poll_.stop();
            if(job->kind==PdfJob::Kind::Write){
                if(job->written){cancelling_=false;if(persist_)saveExportPreferences(job->settings);
                    if(!job->written.textWarnings.isEmpty()){
                        auto fallback=pdfPlan_;fallback.rasterized=job->written.rasterized;fallback.preserved=job->written.preserved;
                        for(auto index:job->written.rasterizedPages){auto& page=fallback.pages.at(index);page.preservedText.clear();page.textReasons.append(job->written.textWarnings);}
                        dialog_.setPdfPlan(fallback);
                    }
                    dialog_.setExported(job->settings.destination,{},job->written.bytes);success_(job->settings,job->written.bytes);}
                else if(cancelling_){finishCancellation();return;}
                else if(!job->written.cancelled)dialog_.setError(job->written.error);
                if(closing_)dialog_.done(QDialog::Rejected);
                return;
            }
            if(cancelling_){finishCancellation();return;}
            if(job->cancel||dialog_.settings().format!=ExportFormat::Pdf){debounce_.start();return;}
            if(job->kind==PdfJob::Kind::Thumbnail){
                // A bounded icon cache; failed thumbnail work isn't repeatedly retried.
                if(pdfThumbnails_.size()>=32){dialog_.setPdfThumbnail(pdfThumbnails_.front(),{});pdfThumbnails_.pop_front();}
                pdfThumbnails_.push_back(job->thumbnail);dialog_.setPdfThumbnail(job->thumbnail,job->preview.image);
                prepareThumbnail();return;
            }
            if(job->settings.pdf!=dialog_.settings().pdf){debounce_.start();return;}
            if(!job->plan||!job->preview){pendingWrite_.reset();dialog_.setError(!job->plan.error.isEmpty()?job->plan.error:job->preview.error);return;}
            pdfPlan_=std::move(job->plan);pdfPreview_=std::move(job->preview.image);pdfPreviewPage_=job->page;
            dialog_.setPdfPlan(pdfPlan_);dialog_.setPdfPreview(pdfPlan_,pdfPreviewPage_,pdfPreview_);
            if(pendingWrite_)preparePdf();else prepareThumbnail();
        }
        void showReadyPreview()
        {
            // Path changes can reuse encoded bytes, but not permission to
            // overwrite the open project (including aliases to its path).
            if (const auto error = validateExportDestination(dialog_.settings().destination, projectPath_);
                !error.isEmpty()) {
                dialog_.setError(error);
                return;
            }
            dialog_.setPreview(ready_.decoded, ready_.bytes.size());
        }
        void publish()
        {
            if (!pendingWrite_ || !ready_ || !sameEncoding(*pendingWrite_, readySettings_))
                return;
            // Bytes shown in the preview are precisely the bytes atomically saved.
            write_ = std::make_shared<WriteJob>();
            write_->settings = *pendingWrite_;
            write_->bytes = ready_.bytes.size();
            pendingWrite_.reset();
            debounce_.stop();
            auto* job = write_.get();
            const auto bytes = ready_.bytes;
            dialog_.setProgress(tr("Writing image…"), true);
            try {
                job->worker = std::thread([job, bytes] {
                    job->result = writeExportAtomically(job->settings.destination, bytes, job->cancel);
                    job->done.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                write_.reset();
                dialog_.setError(QString::fromUtf8(e.what()));
                return;
            }
            poll_.start();
        }
        void finishCancellation()
        {
            // Called only after the in-flight worker/render has acknowledged
            // cancellation. Keep reusable previews, but never restart a write.
            cancelling_ = false;
            if (closing_) {
                dialog_.done(QDialog::Rejected);
                return;
            }
            const auto s = dialog_.settings();
            const bool previewReady = s.format == ExportFormat::Pdf
                ? pdfPlan_ && pdfPlan_.options == s.pdf && !pdfPreview_.isNull()
                    && pdfPreviewPage_ == dialog_.pdfPreviewPage() && dialog_.pdfInputError().isEmpty()
                : s.format==ExportFormat::Psd
                    ? psdPlan_&&psdPlan_.options==s.psd&&(!dialog_.psdPreviewEnabled()||!psdPreview_.isNull())
                    : ready_ && sameEncoding(readySettings_, s);
            dialog_.setCancelled(previewReady
                && validateExport(s, canvasSize()).isEmpty()
                && validateExportDestination(s.destination, projectPath_).isEmpty());
            if (!previewReady)
                debounce_.start(); // Preview only; a retry requires a fresh Export click.
        }
        void cancel(bool close)
        {
            closing_ |= close;
            if (cancelling_)
                return;
            cancelling_ = true;
            debounce_.stop();
            pendingWrite_.reset();
            renderCancel_ = true;
            if(pdfJob_){pdfJob_->cancel=true;dialog_.setProgress(tr("Cancelling PDF export…"),true);return;}
            if(psdJob_){psdJob_->cancel=true;dialog_.setProgress(tr("Cancelling PSD export…"),true);return;}
            if (encode_) {
                encode_->cancel = true;
                dialog_.setProgress(tr("Cancelling encoding…"), true);
                return;
            }
            if (write_) {
                write_->cancel = true;
                dialog_.setProgress(tr("Cancelling write…"), true);
                return;
            }
            if (!rendering_)
                finishCancellation();
        }
        const core::Document& document_;
        ExportDialog& dialog_;
        QString projectPath_;
        QString sourcePath_;
        std::uint64_t instanceId_;
        std::vector<core::LayerId> selection_;
        std::shared_ptr<PdfExportSnapshot> pdfSource_;
        std::shared_ptr<PdfJob> pdfJob_;
        std::shared_ptr<PsdExportSnapshot> psdSource_;
        std::shared_ptr<PsdJob> psdJob_;
        PsdExportPlan psdPlan_;
        QImage psdPreview_;
        PdfExportLimits pdfLimits_;
        PdfExportPlan pdfPlan_;
        QImage pdfPreview_;int pdfPreviewPage_{-1};
        std::deque<core::LayerId> pdfThumbnails_;
        bool persist_, rendering_ { false }, renderCancel_ { false }, closing_ { false }, cancelling_ { false };
        std::function<void(const ExportSettings&, qint64)> success_;
        QTimer debounce_, poll_;
        QImage rendered_;
        ExportSettings renderSettings_, encodeSettings_, readySettings_;
        EncodedExport ready_;
        std::optional<ExportSettings> pendingWrite_;
        std::shared_ptr<EncodeJob> encode_;
        std::shared_ptr<WriteJob> write_;
    };
} // namespace

void MainWindow::exportImage(bool again)
{
    if (fileBusy_ || workspaceDialog_ || !session().document())
        return;
    if (smartInteractionActive() || colorSelectionEditing_ || selectionGesture_ || selectionRasterizer_
        || shapeResize_) {
        statusBar()->showMessage(tr("Finish or cancel the current selection/shape "
                                    "interaction before exporting."),
            4000);
        return;
    }
    if (again && !fileState().exportSettings) {
        statusBar()->showMessage(tr("Export an image first to establish Export Again settings."), 4000);
        return;
    }
    if (!settleForFileOperation())
        return;
    const auto extent = session().document()->canvas().extent;
    const QSize canvas(int(extent.width), int(extent.height));
    const auto owner = activeDocument_;
    auto initial = owner->exportSettings.value_or(loadExportPreferences(canvas, fileState().displayName));
    if (!owner->exportSettings) {
        initial.format = ExportFormat::Png;
        initial.destination = exportPathForFormat(initial.destination, initial.format);
    }
    // The workspace shield excludes edits and target switches. PDF work owns a
    // frozen snapshot; image-codec work owns immutable rendered buffers.
    const QScopedValueRollback busy(fileBusy_, true);
    WorkspaceDialog presenter(*workspace_, *this);
    ExportDialog dialog(initial, canvas, &presenter);
    PdfExportLimits pdfLimits;
    const auto memory=documentMemory();
    pdfLimits.existingBytes=memory.sourceBytes+memory.derivedBytes+memory.historyBytes;
#ifdef Q_OS_LINUX
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if(meminfo.open(QIODevice::ReadOnly))for(const auto& line:meminfo.readAll().split('\n')){
        if(!line.startsWith("MemAvailable:"))continue;
        bool ok=false;const auto kib=line.simplified().split(' ').value(1).toULongLong(&ok);
        if(ok&&kib<(1ULL<<40))pdfLimits.workingBytes=std::min<std::uint64_t>(pdfLimits.workingBytes,pdfLimits.existingBytes+kib*768);
        break;
    }
#endif
    ExportCoordinator coordinator(*session().document(), dialog, fileState().projectPath, fileState().sourcePath,
        owner->id,session().selectedLayers(),pdfLimits,persistWindowState_,
        [this, owner](const ExportSettings& s, qint64 bytes) {
            if (owner->closed) return;
            owner->exportSettings = s;
            if (owner != activeDocument_) return;
            statusBar()->showMessage((s.format==ExportFormat::Pdf||s.format==ExportFormat::Psd)?tr("Exported %1 · %2 bytes").arg(QFileInfo(s.destination).fileName()).arg(bytes):tr("Exported %1 · %2 × %3 px · %4 bytes")
                                         .arg(QFileInfo(s.destination).fileName())
                                         .arg(s.size.width())
                                         .arg(s.size.height())
                                         .arg(bytes),
                8000);
        });
    const QScopedValueRollback active(workspaceDialog_, &presenter);
    if (again)
        QTimer::singleShot(0, &dialog, [&coordinator] { coordinator.requestWrite(); });
    presenter.exec(dialog);
    if (!owner->closed&&dialog.settings().format!=ExportFormat::Pdf&&dialog.settings().format!=ExportFormat::Psd) owner->exportSettings = dialog.settings();
    // No checkpoint, association, selection, viewport or history changes here.
}
} // namespace imageeditor::ui
