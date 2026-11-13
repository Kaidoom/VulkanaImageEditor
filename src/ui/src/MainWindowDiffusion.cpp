#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/diffusion/WorkerClient.h"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionSpatial.hpp"
#include "imageeditor/ui/DiffusionTransaction.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QAction>
#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <optional>

namespace imageeditor::ui {
namespace {
QImage srgb(QImage image) {
  if (image.isNull())
    throw std::runtime_error("The input image is empty.");
  if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb)
    image = image.convertedToColorSpace(QColorSpace::SRgb);
  image = image.convertToFormat(QImage::Format_RGBA8888);
  image.setColorSpace(QColorSpace::SRgb);
  auto copy = image.copy();
  if (copy.isNull())
    throw std::runtime_error("Not enough memory to prepare the input image.");
  return copy;
}
QTransform qtTransform(const core::AffineTransform &m) {
  return QTransform(m.m00, m.m10, m.m20, m.m01, m.m11, m.m21, m.m02, m.m12,
                    m.m22);
}
core::AffineTransform coreTransform(const QTransform &m) {
  return {.m00 = m.m11(),
          .m01 = m.m21(),
          .m02 = m.dx(),
          .m10 = m.m12(),
          .m11 = m.m22(),
          .m12 = m.dy(),
          .m20 = m.m13(),
          .m21 = m.m23(),
          .m22 = m.m33()};
}
QString discoverWorker(const QString &backend) {
  const auto find = [](const QString &name) {
#ifdef Q_OS_WIN
    const auto executableName = name + QStringLiteral(".exe");
#else
    const auto &executableName = name;
#endif
    const auto adjacent =
        QDir(QCoreApplication::applicationDirPath()).filePath(executableName);
    return QFileInfo(adjacent).isExecutable()
               ? adjacent
               : QStandardPaths::findExecutable(executableName);
  };
  const auto genericOverride = qEnvironmentVariable("VULKANA_DIFFUSION_WORKER");
  if (!genericOverride.isEmpty())
    return genericOverride; // Verified against the worker hello.
  const auto variant = [&](const QString &name) {
    const auto override = qEnvironmentVariable(
        qPrintable("VULKANA_DIFFUSION_WORKER_" + name.toUpper()));
    if (!override.isEmpty())
      return override;
    const auto installed = find("vulkana-diffusion-worker-" + name);
    if (!installed.isEmpty())
      return installed;
#ifdef VULKANA_DEVELOPMENT_DIFFUSION_WORKER
    if (name == "cuda") {
      const QString development =
          QString::fromUtf8(VULKANA_DEVELOPMENT_DIFFUSION_WORKER);
      if (QFileInfo(development).isExecutable())
        return development;
    }
#endif
    return QString{};
  };
  if (backend == "auto") {
    for (const auto &candidate : {QString("cuda"), QString("vulkan")}) {
      const auto path = variant(candidate);
      if (!path.isEmpty())
        return path;
    }
  } else {
    const auto path = variant(backend);
    if (!path.isEmpty())
      return path;
  }
  return find("vulkana-diffusion-worker");
}
} // namespace

struct DiffusionWorkspace {
  DiffusionPanel *panel{};
  QWidget *shell{};
  struct ResultContext {
    std::weak_ptr<DocumentContext> owner;
    std::optional<DiffusionTarget> target;
    QImage before;
    DiffusionPanel::Mode mode{DiffusionPanel::Mode::Generate};
    QTransform inputToDocument;
    QSize inputSize;
    bool croppedEdit{};
  };
  QMap<QString, ResultContext> resultContexts;
  ResultContext jobContext;
  QSet<DocumentInstanceId> jobDocuments;
  QFrame *notice{};
  QLabel *noticeLabel{};
  QProgressBar *noticeProgress{};
  QToolButton *noticeOpen{};
  bool noticeDismissed{}, noticeFinished{}, panelOpen{};
  QString pendingExecutable, activeExecutable, pendingBackend, pendingConfigurationName;
  QString comparisonKey;
  std::unique_ptr<diffusion::WorkerClient> worker;
  QTemporaryDir files{QDir::tempPath() + "/vulkana-diffusion-XXXXXX"};
  std::shared_ptr<DocumentContext> owner;
  std::optional<DiffusionTarget> target;
  QImage before;
  DiffusionPanel::Mode mode{DiffusionPanel::Mode::Generate};
  QJsonObject pendingConfiguration, loadedConfiguration, pendingGeneration;
  QJsonArray residentModels;
  bool unloadingModel{};
  QString loadId, generateId;
  QStringList sessionFiles;
  bool wanted{}, cleanup{}, shuttingDown{};
  std::uint64_t epoch{}, requestEpoch{}, variationCounter{};
  void refreshLoadedModels() {
    panel->setLoadedModels(residentModels,
                          !wanted && worker->state() ==
                                         diffusion::WorkerClient::State::Idle);
  }
  void receiveLoadedModels(const QJsonObject &event) {
    if (!event.value("loadedModels").isArray())
      return;
    residentModels = {};
    for (const auto &value : event.value("loadedModels").toArray()) {
      auto model = value.toObject();
      // Context counters restart with the process. Scope the row's action to
      // this exact worker as well, so an old action cannot unload a new model.
      model["id"] = QString("%1:%2")
                        .arg(worker->processId())
                        .arg(model.value("contextGeneration").toInteger());
      residentModels.append(model);
    }
    refreshLoadedModels();
  }
  ~DiffusionWorkspace() { worker.reset(); }
};

void MainWindow::createDiffusionActions(QMenu *editMenu) {
  auto state = std::make_shared<DiffusionWorkspace>();
  diffusion_ = state;
  state->panel = new DiffusionPanel;
  state->panel->setObjectName("AIImageEditorPanel");
  state->panel->setProfiles(diffusion::modelProfiles());
  auto *shell = new QFrame(workspace_->panelOverlay());
  state->shell = shell;
  shell->setObjectName("AIImageEditorWorkspace");
  shell->setAttribute(Qt::WA_StyledBackground);
  shell->setStyleSheet(QString("QFrame#AIImageEditorWorkspace { background: "
                               "%1; border: 1px solid %2; }")
                           .arg(themeColor(ThemeColor::Surface).name(),
                                themeColor(ThemeColor::Border).name()));
  auto *shellLayout = new QVBoxLayout(shell);
  shellLayout->setContentsMargins(0, 0, 0, 0);
  shellLayout->addWidget(state->panel);
  workspace_->setSideToolOverlay(shell, false);
  state->worker = std::make_unique<diffusion::WorkerClient>();
  const auto active = [this, weak = std::weak_ptr(state)] {
    auto shared = weak.lock();
    return shared && diffusion_ == shared && !shared->shuttingDown;
  };
  auto *ai = i18n::addMenu(editMenu, i18n::text("diffusion.ui.ai"));
  ai->setObjectName("AIImageEditorMenu");
  ai->setIcon(toolGlyph(ToolGlyph::LayerRaster));
  const auto action = [&](const i18n::Message &name, const char *objectName, int mode) {
    auto *item = i18n::addAction(ai, name);
    item->setObjectName(objectName);
    item->setIcon(toolGlyph(ToolGlyph::LayerRaster));
    connect(item, &QAction::triggered, this,
            [this, mode] { openDiffusion(mode); });
    return item;
  };
  action(i18n::text("diffusion.ui.generate.image"), "AIGenerateImageAction",
         int(DiffusionPanel::Mode::Generate));
  auto *edit = action(i18n::text("diffusion.ui.edit.current.layer"), "AIEditLayerAction",
                      int(DiffusionPanel::Mode::Edit));
  auto *fill = action(i18n::text("diffusion.ui.generative.fill.2"), "AIGenerativeFillAction",
                      int(DiffusionPanel::Mode::Fill));
  ai->addSeparator();
  action(i18n::text("diffusion.ui.ai.image.editor.2"), "AIImageEditorAction", -1);
  connect(ai, &QMenu::aboutToShow, this, [this, edit, fill] {
    auto *doc = session().document();
    auto *layer = doc && session().activeLayer()
                      ? doc->layer(*session().activeLayer())
                      : nullptr;
    const auto *raster =
        layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    edit->setEnabled(diffusion_->panel->supportsMode(DiffusionPanel::Mode::Edit) && raster && raster->surface
        && raster->surface->dataKind() == core::RasterDataKind::Color && !fileBusy_ &&
                     !workspaceDialog_);
    fill->setEnabled(diffusion_->panel->supportsMode(DiffusionPanel::Mode::Fill) && edit->isEnabled() && doc->selection() &&
                     !doc->selection()->bounds().empty());
  });
  state->panel->onGenerate = [this, active] {
    if (active())
      generateDiffusion();
  };
  state->panel->onApply = [this, active] {
    if (active())
      applyDiffusion();
  };
  state->panel->onPresentPreview = [this, active](QDialog &dialog) {
    if (!active()) return;
    refreshDiffusion(); // Copy thumbnails prepared by the Layers panel since its last refresh.
    auto *presenter = new WorkspaceDialog(*workspace_, *this);
    const QPointer<WorkspaceDialog> previous(workspaceDialog_);
    workspaceDialog_ = presenter;
    const auto size = dialog.size();
    dialog.setParent(presenter, Qt::Widget | Qt::FramelessWindowHint);
    dialog.setProperty("workspacePreferredSize", size);
    presenter->open(dialog);
    connect(&dialog, &QDialog::finished, this, [this, presenter, previous] {
      if (workspaceDialog_ == presenter) workspaceDialog_ = previous;
      presenter->deleteLater();
    });
  };
  state->panel->onDiscard = [this, active] {
    if (!active() || !diffusion_->panel->selectedResult())
      return;
    auto &s = *diffusion_;
    s.panel->removeSelectedResult();
    QSet<QString> retained;
    for (const auto &result : s.panel->results())
      retained.insert(result.metadata["contextId"].toString());
    for (auto it = s.resultContexts.begin(); it != s.resultContexts.end();)
      if (!retained.contains(it.key()))
        it = s.resultContexts.erase(it);
      else
        ++it;
    s.cleanup = true;
    cleanupDiffusionFiles();
    refreshDiffusion();
  };
  state->panel->onCancel = [this, active] {
    if (active())
      cancelDiffusion();
  };
  state->panel->onClose = [this, active] {
    if (active())
      hideDiffusion();
  };
  state->panel->onUnloadModel = [this, active](const QString &id) {
    if (!active() || diffusion_->panel->isBusy() || diffusion_->wanted ||
        diffusion_->worker->state() != diffusion::WorkerClient::State::Idle)
      return;
    const auto &models = diffusion_->residentModels;
    if (std::none_of(models.begin(), models.end(), [&](const QJsonValue &item) {
          return item.toObject().value("id").toString() == id;
        }))
      return;
    diffusion_->loadedConfiguration = {};
    diffusion_->unloadingModel = true;
    diffusion_->panel->setStatus(i18n::text("diffusion.ui.unloading.model"));
    diffusion_->worker->stop();
  };
  state->panel->onResetSize = [this, active] {
    if (active())
      resetDiffusionSize();
  };
  state->panel->onMatchSelectedSize = [this, active] {
    if (active()) matchSelectedDiffusionSize();
  };
  state->panel->onCompareRequest = [this, active] {
    if (active())
      compareDiffusionLayer();
  };
  state->panel->onInputsChanged = [this, active] {
    if (active())
      refreshDiffusion();
  };
  state->panel->onAddInput = [this, active](auto source) {
    if (active())
      addDiffusionInput(int(source));
  };
  state->panel->onResultSelected = [this, active](int) {
    if (active()) {
      refreshDiffusion();
      refreshDiffusionPreview();
      if (diffusion_->panel->comparisonActive())
        compareDiffusionLayer();
    }
  };
  state->panel->onDestinationChanged = [this, active] {
    if (active())
      refreshDiffusionPreview();
  };
  state->panel->onModeChanged = [this, active](auto mode) {
    if (active()) {
      diffusion_->mode = mode;
      refreshDiffusion();
    }
  };
  connect(state->worker.get(), &diffusion::WorkerClient::ready, this,
          [this, active](const QJsonObject &hello) {
            if (!active())
              return;
            diffusion_->loadedConfiguration = {};
            diffusion_->residentModels = {};
            diffusion_->refreshLoadedModels();
            diffusion_->panel->setProfiles(hello.value("profiles").toArray());
            diffusion_->panel->setDevices(hello.value("devices").toArray());
            diffusion_->panel->setSamplers(hello.value("samplers").toArray(),
                                           hello.value("schedulers").toArray());
            continueDiffusionJob();
          });
  connect(state->worker.get(), &diffusion::WorkerClient::eventReceived, this,
          [this, active](const QJsonObject &event) {
            if (!active())
              return;
            if (event.value("event") == "state")
              diffusion_->receiveLoadedModels(event);
            if (!diffusion_->wanted)
              return;
            const auto job = event.value("jobId").toString();
            if (job != diffusion_->generateId && job != diffusion_->loadId)
              return;
            if (event.value("event") == "progress") {
              updateDiffusionNotice(i18n::text("diffusion.ui.generating"), event.value("step").toInt(),
                                    event.value("steps").toInt());
              diffusion_->panel->setProgress(
                  event.value("step").toInt(), event.value("steps").toInt(),
                  event.value("elapsedMs").toDouble());
            }
            if (event.value("event") == "preview") {
              const QImage image(event.value("path").toString());
              if (!image.isNull())
                diffusion_->panel->setProgressPreview(image);
            }
          });
  connect(
      state->worker.get(), &diffusion::WorkerClient::requestFinished, this,
      [this, active](const QString &id, const QJsonObject &result) {
        if (!active())
          return;
        auto &s = *diffusion_;
        try {
          if (id == s.loadId) {
            s.loadId.clear();
            if (s.wanted && s.requestEpoch == s.epoch) {
              s.loadedConfiguration = s.pendingConfiguration;
              continueDiffusionJob();
            } else {
              s.panel->setBusy(false);
              cleanupDiffusionFiles();
            }
          } else if (id == s.generateId) {
            s.generateId.clear();
            s.panel->setBusy(false);
            const auto images = result.value("images").toArray();
            for (const auto &item : images) {
              const auto path = item.toObject().value("path").toString();
              if (!path.isEmpty())
                s.sessionFiles.push_back(QFileInfo(path).absolutePath());
            }
            if (!s.wanted || s.requestEpoch != s.epoch) {
              cleanupDiffusionFiles();
              return;
            }
            s.wanted = false;
            // Each output keeps its frozen inputs independently of later
            // jobs, document edits, or a switch to another editing mode.
            s.resultContexts[id] = s.jobContext;
            const auto priorCount = s.panel->results().size();
            for (const auto &item : images) {
              const auto object = item.toObject();
              QImage image(object.value("path").toString());
              if (image.isNull()) {
                s.panel->setStatus("The generated image could not be read.",
                                   true);
                refreshDiffusion();
                continue;
              }
              auto metadata = result;
              metadata["image"] = object;
              metadata["contextId"] = id;
              metadata["request"] = s.pendingGeneration;
              metadata["configuration"] = s.pendingConfiguration;
              metadata["configurationName"] = s.pendingConfigurationName;
              s.panel->appendResult(
                  {srgb(image), s.jobContext.before,
                   i18n::text("diffusion.ui.variation.1").arg(++s.variationCounter), metadata,
                   s.jobContext.mode == DiffusionPanel::Mode::Edit &&
                           s.jobContext.inputSize.isValid()
                       ? QTransform::fromScale(
                             double(s.jobContext.inputSize.width()) /
                                 image.width(),
                             double(s.jobContext.inputSize.height()) /
                                 image.height()) *
                             s.jobContext.inputToDocument
                       : QTransform{}, s.jobContext.mode});
            }
            if (s.panel->results().size() == priorCount)
              throw std::runtime_error(
                  "The generated images could not be read.");
            s.panel->generationAccepted();
            updateDiffusionNotice(i18n::text("diffusion.ui.generation.done"), 0, 0, true);
            const auto seconds =
                result.value("timings").toObject().value("totalMs").toDouble() /
                1000.;
            refreshDiffusion();
            s.panel->setStatus(
                i18n::text("diffusion.ui.ready.1.s.inspect.the.result.before")
                    .arg(seconds, 0, 'f', 1));
          }
        } catch (const std::exception &error) {
          s.wanted = false;
          s.loadId.clear();
          s.generateId.clear();
          s.panel->setBusy(false);
          refreshDiffusion();
          s.panel->setStatus(QString::fromUtf8(error.what()), true);
          updateDiffusionNotice(i18n::text("diffusion.ui.generation.failed"), 0, 0, true);
        }
      });
  connect(state->worker.get(), &diffusion::WorkerClient::requestFailed, this,
          [this, active](const QString &id, const QString &code,
                         const QString &message) {
            if (!active())
              return;
            auto &s = *diffusion_;
            if (id != s.loadId && id != s.generateId)
              return;
            const bool loading = id == s.loadId;
            s.loadId.clear();
            s.generateId.clear();
            s.wanted = false;
            s.panel->setBusy(false);
            if (code != "invalid_arguments" && code != "busy" &&
                !(code == "cancelled" && !loading))
              s.loadedConfiguration = {};
            refreshDiffusion();
            s.panel->setStatus(
                code == "cancelled"
                    ? i18n::text("diffusion.ui.cancelled.previous.successful.variations.are.still.available")
                    : message,
                code != "cancelled");
            updateDiffusionNotice(code == "cancelled" ? i18n::text("diffusion.ui.generation.cancelled")
                                                      : i18n::text("diffusion.ui.generation.failed"),
                                  0, 0, true);
            cleanupDiffusionFiles();
          });
  connect(state->worker.get(), &diffusion::WorkerClient::errorOccurred, this,
          [this, active](const QString &, const QString &message) {
            if (!active())
              return;
            auto &s = *diffusion_;
            s.loadedConfiguration = {};
            s.wanted = false;
            s.panel->setBusy(false);
            refreshDiffusion();
            s.panel->setStatus(
                message + i18n::text("diffusion.ui.generate.again.to.restart.the.worker"), true);
            updateDiffusionNotice(i18n::text("diffusion.ui.generation.failed"), 0, 0, true);
            cleanupDiffusionFiles();
          });
  connect(state->worker.get(), &diffusion::WorkerClient::stateChanged, this,
          [this, active](auto value) {
            if (!active())
              return;
            diffusion_->refreshLoadedModels();
            if (value == diffusion::WorkerClient::State::Idle)
              QTimer::singleShot(0, this, [this, active] {
                if (active()) {
                  cleanupDiffusionFiles();
                  continueDiffusionJob();
                }
              });
            if (value == diffusion::WorkerClient::State::Stopped ||
                value == diffusion::WorkerClient::State::Error) {
              diffusion_->loadedConfiguration = {};
              diffusion_->residentModels = {};
              diffusion_->refreshLoadedModels();
              if (diffusion_->unloadingModel &&
                  value == diffusion::WorkerClient::State::Stopped &&
                  !diffusion_->wanted)
                diffusion_->panel->setStatus(
                    i18n::text("diffusion.ui.model.unloaded.generate.again.to.load.a"));
              diffusion_->unloadingModel = false;
              cleanupDiffusionFiles();
              QTimer::singleShot(0, this, [this, active] {
                if (active())
                  continueDiffusionJob();
              });
            }
          });
}

void MainWindow::openDiffusion(int requested) {
  if (!diffusion_ || fileBusy_ || workspaceDialog_)
    return;
  auto &s = *diffusion_;
  s.panelOpen = true;
  workspace_->setSideToolOverlay(s.shell, true);
  if (s.notice) {
    if (s.noticeFinished) s.noticeDismissed = true;
    workspace_->setTaskNotificationOverlay(s.notice, false);
  }
  if (requested >= 0)
    s.panel->showGeneration();
  if (s.panel->isBusy())
    return;
  const auto mode = requested < 0
                        ? s.panel->mode()
                        : static_cast<DiffusionPanel::Mode>(requested);
  s.panel->setMode(mode);
  s.mode = s.panel->mode();
  if (requested >= 0 && s.mode != DiffusionPanel::Mode::Generate)
    captureDiffusionSession();
  if (s.mode != mode)
    s.panel->setStatus(i18n::text("diffusion.ui.this.model.generates.from.text.only.choose.2"));
  refreshDiffusion();
}

bool MainWindow::captureDiffusionSession() {
  if (!diffusion_)
    return false;
  auto &s = *diffusion_;
  try {
    if (!settleForFileOperation())
      return false;
    if (!activeDocument_ || !session().document() || !session().activeLayer())
      throw std::runtime_error(i18n::text("diffusion.ui.select.a.raster.layer.to.start.an").toStdString());
    if (s.mode == DiffusionPanel::Mode::Fill &&
        (!session().document()->selection() ||
         session().document()->selection()->bounds().empty()))
      throw std::runtime_error(i18n::text("diffusion.ui.make.a.selection.before.using.generative.fill").toStdString());
    s.owner = activeDocument_;
    s.target =
        captureDiffusionTarget(*session().document(), *session().activeLayer(),
                               s.mode == DiffusionPanel::Mode::Fill);
    if (s.mode == DiffusionPanel::Mode::Fill) {
      const auto rendered = flattenDocument(*s.owner->session.document());
      if (!rendered)
        throw std::runtime_error(rendered.error.toStdString());
      s.before = srgb(rendered.image);
    } else
      s.before = s.target->originalPixels;
    auto inputs = s.panel->inputs();
    inputs.removeIf([](const auto &input) { return input.target; });
    const auto *layer = s.owner->session.document()->layer(s.target->layerId);
    inputs.prepend({s.before,
                    QString::fromStdString(layer->name),
                    s.mode == DiffusionPanel::Mode::Fill
                        ? i18n::text("diffusion.ui.target.visible.canvas")
                        : i18n::text("diffusion.ui.target.raw.layer.pixels"),
                    true,
                    {{"documentId", QString::number(s.owner->id)},
                     {"layerId", QString::number(s.target->layerId)},
                     {"visibleCanvas", s.mode == DiffusionPanel::Mode::Fill}},
                    s.mode == DiffusionPanel::Mode::Fill
                        ? QTransform{}
                        : QTransform::fromTranslate(s.target->rasterOrigin.x,
                                                    s.target->rasterOrigin.y) *
                              qtTransform(s.target->localToDocument)});
    if (s.mode == DiffusionPanel::Mode::Edit)
      inputs.front().sourceCanvas = QSize(int(s.target->canvas.extent.width), int(s.target->canvas.extent.height));
    s.panel->setInputs(std::move(inputs));
    s.panel->setResolution(s.before.size());
    s.panel->setTargetCompatible(true);
    s.panel->setStatus(i18n::text("diffusion.ui.target.captured.you.can.keep.editing.changed"));
    return true;
  } catch (const std::exception &e) {
    refreshDiffusion();
    s.target.reset();
    s.owner.reset();
    auto inputs = s.panel->inputs();
    inputs.removeIf([](const auto &input) { return input.target; });
    s.panel->setInputs(std::move(inputs));
    s.cleanup = true;
    cleanupDiffusionFiles();
    s.panel->setStatus(QString::fromUtf8(e.what()), true);
    return false;
  }
}

void MainWindow::generateDiffusion() {
  if (!diffusion_ || diffusion_->panel->isBusy())
    return;
  auto &s = *diffusion_;
  try {
    s.mode = s.panel->mode();
    const auto inputs = s.panel->supportsImageInputs() ? s.panel->inputs() : QVector<DiffusionPanel::Input>{};
    if (s.mode != DiffusionPanel::Mode::Generate && inputs.isEmpty())
      throw std::runtime_error(i18n::text("diffusion.ui.add.an.input.image.before.generating.an").toStdString());
    s.target.reset();
    s.owner.reset();
    s.before = {};
    if (s.mode == DiffusionPanel::Mode::Fill) {
      if (!session().document() || !session().activeLayer())
        throw std::runtime_error(i18n::text("diffusion.ui.select.a.raster.layer.and.a.selection").toStdString());
      s.owner = activeDocument_;
      s.target = captureDiffusionTarget(*session().document(),
                                        *session().activeLayer(), true);
      const auto rendered = flattenDocument(*session().document());
      if (!rendered)
        throw std::runtime_error(rendered.error.toStdString());
      s.before = srgb(rendered.image);
    } else if (s.mode == DiffusionPanel::Mode::Edit) {
      const auto &input = inputs.front();
      s.before = input.image;
      const auto id = input.metadata["documentId"].toString().toULongLong();
      for (const auto &owner : documents_)
        if (owner->id == id && !owner->closed) {
          s.owner = owner;
          break;
        }
    }
    s.jobContext = {s.owner, s.target, s.before, s.mode, {}, {}};
    if (s.mode == DiffusionPanel::Mode::Edit) {
      s.jobContext.inputToDocument = inputs.front().toDocument;
      s.jobContext.inputSize = s.before.size();
      s.jobContext.croppedEdit = inputs.front().cropToCanvas;
    }
    s.jobDocuments.clear();
    if (s.owner)
      s.jobDocuments.insert(s.owner->id);
    for (const auto &input : inputs) {
      bool ok = false;
      const auto id = input.metadata["documentId"].toString().toULongLong(&ok);
      if (ok)
        s.jobDocuments.insert(id);
    }
    s.pendingConfiguration = s.panel->configuration();
    s.pendingConfigurationName = s.panel->configurationName();
    // LoRA defaults belong to the saved configuration. Effective adapters are
    // sent per job so changing an override can reuse the loaded model.
    s.pendingConfiguration.remove("loras");
    s.pendingGeneration = s.panel->prepareGeneration();
    const auto size = QSize(s.pendingGeneration.value("width").toInt(1024),
                            s.pendingGeneration.value("height").toInt(1024));
    if (s.pendingGeneration.value("prompt").toString().trimmed().isEmpty())
      throw std::runtime_error(i18n::text("diffusion.ui.enter.a.prompt.before.generating").toStdString());
    if (!s.files.isValid())
      throw std::runtime_error("Cannot create a private directory for "
                               "diffusion inputs and results.");
    const auto inputDir =
        QDir(s.files.path())
            .filePath("inputs-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(inputDir))
      throw std::runtime_error("Cannot prepare the input image directory.");
    s.sessionFiles.push_back(inputDir);
    const auto save = [&](const QImage &image, const QString &name) {
      const auto path = QDir(inputDir).filePath(name + ".png");
      if (!image.save(path, "PNG"))
        throw std::runtime_error("Cannot write an inference input image.");
      return path;
    };
    QImage fillMask;
    QImage initial = s.before;
    if (s.mode == DiffusionPanel::Mode::Fill) {
      fillMask = diffusionSelectionMask(*s.target, {}, true);
      initial = diffusionPrepareFillInput(s.before, fillMask);
    }
    QJsonArray refs;
    for (int i = 0; i < inputs.size(); ++i) {
      const auto &input = inputs[i];
      auto image = srgb(input.image);
      if (!fillMask.isNull() && input.metadata["visibleCanvas"].toBool() &&
          input.metadata["documentId"].toString() == QString::number(s.owner->id) &&
          input.fullToDocument.isIdentity() && input.fullImage.size() == fillMask.size())
        image = diffusionPrepareFillInput(image, fillMask.scaled(image.size(), Qt::IgnoreAspectRatio,
                                                                Qt::SmoothTransformation));
      refs.append(save(image, QString("image-%1").arg(i + 1)));
    }
    if (!refs.isEmpty())
      s.pendingGeneration["referenceImages"] = refs;
    const bool imageToImage = s.panel->imageToImageEnabled();
    if (s.mode == DiffusionPanel::Mode::Fill ||
        (s.mode == DiffusionPanel::Mode::Edit && imageToImage)) {
      s.pendingGeneration["initialImage"] =
          save(initial.scaled(size, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation),
               "initial");
      if (s.mode == DiffusionPanel::Mode::Fill)
        s.pendingGeneration["mask"] =
            save(fillMask.scaled(size, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation), "mask");
    }
    const auto regions = s.panel->regionsJson();
    auto spatial = s.panel->spatialParameters();
    if (!spatial.isEmpty()) {
      const auto profile = diffusion::modelProfile(s.pendingConfiguration.value("profile").toString());
      if (!regions.isEmpty() && profile.value("family") == "qwen-image-2.1") {
        const QImage base = s.panel->inputs().isEmpty()
                                ? QImage{}
                                : s.panel->inputs().front().image;
        spatial["annotationImage"] =
            save(diffusionRegionAnnotation(base, regions, size), "regions");
      }
      s.pendingGeneration["spatial"] = spatial;
    }
    s.requestEpoch = ++s.epoch;
    s.wanted = true;
    s.panel->setBusy(true);
    s.panel->setStatus(i18n::text("diffusion.ui.preparing.inference"));
    s.noticeDismissed = false;
    updateDiffusionNotice(i18n::text("diffusion.ui.preparing.generation"));
    s.pendingBackend = s.panel->backend();
    s.pendingExecutable = s.panel->workerExecutable();
    if (s.pendingExecutable.isEmpty())
      s.pendingExecutable = discoverWorker(s.pendingBackend);
    if (s.pendingExecutable.isEmpty())
      throw std::runtime_error(
          i18n::text("diffusion.ui.1.inference.worker.is.not.installed.choose").arg(i18n::ltrToken(s.pendingBackend.toUpper()))
              .toStdString());
    if (s.worker->isRunning() && s.activeExecutable != s.pendingExecutable)
      s.worker->stop();
    else
      continueDiffusionJob();
  } catch (const std::exception &e) {
    s.wanted = false;
    s.panel->setBusy(false);
    refreshDiffusion();
    s.panel->setStatus(QString::fromUtf8(e.what()), true);
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.failed"), 0, 0, true);
  }
}

void MainWindow::continueDiffusionJob() {
  if (!diffusion_ || !diffusion_->wanted)
    return;
  auto &s = *diffusion_;
  if (!s.worker->isRunning()) {
    QString error;
    s.activeExecutable = s.pendingExecutable;
    if (!s.worker->start(s.pendingExecutable, s.files.path(), &error)) {
      s.wanted = false;
      s.panel->setBusy(false);
      s.panel->setStatus(error, true);
      updateDiffusionNotice(i18n::text("diffusion.ui.generation.failed"), 0, 0, true);
    }
    return;
  }
  if (s.worker->state() == diffusion::WorkerClient::State::Error) {
    s.worker->stop();
    return;
  }
  if (s.worker->state() != diffusion::WorkerClient::State::Idle ||
      !s.loadId.isEmpty() || !s.generateId.isEmpty())
    return;
  const auto actualBackend = s.worker->hello()["backend"].toString();
  if (!s.pendingGeneration.value("loras").toArray().isEmpty() &&
      !s.worker->hello().value("loraParameters").toBool()) {
    s.wanted = false;
    s.panel->setBusy(false);
    s.panel->setStatus(i18n::text("diffusion.ui.update.the.inference.worker.to.use.loras"), true);
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.could.not.start"), 0, 0, true);
    return;
  }
  if (diffusion::modelProfile(s.pendingConfiguration.value("profile").toString()).value("family") == "flux3" &&
      !s.worker->hello().value("flux3NativeRegions").toBool()) {
    s.wanted = false;
    s.panel->setBusy(false);
    s.panel->setStatus(i18n::text("diffusion.ui.update.the.inference.worker.to.use.native"), true);
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.could.not.start"), 0, 0, true);
    return;
  }
  if (s.pendingGeneration.value("referenceMaxSize").toInt() > 0 &&
      !s.worker->hello().value("referenceSizeLimit").toBool()) {
    s.wanted = false;
    s.panel->setBusy(false);
    s.panel->setStatus(i18n::text("diffusion.ui.update.the.inference.worker.to.use.the"), true);
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.could.not.start"), 0, 0, true);
    return;
  }
  const int spatialVersion = s.pendingGeneration.value("spatial").toObject().value("version").toInt();
  if (spatialVersion >= 2 && !s.worker->hello().value("spatialVersions").toArray().contains(spatialVersion)) {
    s.wanted = false;
    s.panel->setBusy(false);
    s.panel->setStatus(spatialVersion == 3
        ? i18n::text("diffusion.ui.update.the.inference.worker.to.use.region")
        : i18n::text("diffusion.ui.the.installed.inference.worker.does.not.support"), true);
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.could.not.start"), 0, 0, true);
    return;
  }
  if (s.pendingBackend != "auto" && actualBackend != s.pendingBackend) {
    s.wanted = false;
    s.panel->setBusy(false);
    s.panel->setStatus(
        i18n::text("diffusion.ui.1.is.unavailable.the.installed.worker.provides")
            .arg(s.pendingBackend.toUpper(), actualBackend.toUpper()),
        true);
    s.worker->stop();
    updateDiffusionNotice(i18n::text("diffusion.ui.generation.failed"), 0, 0, true);
    return;
  }
  if (s.pendingConfiguration != s.loadedConfiguration) {
    s.panel->setStatus(i18n::text("diffusion.ui.loading.the.selected.model"));
    updateDiffusionNotice(i18n::text("diffusion.ui.loading.ai.model"));
    s.loadId = s.worker->request("load", s.pendingConfiguration);
  } else {
    s.panel->setStatus(
        i18n::text("diffusion.ui.generating.you.can.inspect.previous.variations.while"));
    s.generateId = s.worker->request("generate", s.pendingGeneration);
  }
}

void MainWindow::applyDiffusion() {
  if (!diffusion_ || diffusion_->panel->isBusy() ||
      !diffusion_->panel->selectedResult())
    return;
  auto &s = *diffusion_;
  QImage image = s.panel->selectedResult()->image;
  const auto layerName = s.panel->selectedResult()->layerName();
  const auto rawPath = s.panel->selectedResult()
                           ->metadata.value("image")
                           .toObject()
                           .value("path")
                           .toString();
  if (!rawPath.isEmpty())
    image = QImage(rawPath);
  auto destination = s.panel->destination();
  const auto context = s.resultContexts.value(
      s.panel->selectedResult()->metadata["contextId"].toString());
  auto target = context.target;
  auto originalOwner = context.owner.lock();
  try {
    if (!settleForFileOperation())
      return;
    QString error;
    if (image.isNull())
      throw std::runtime_error(i18n::text("diffusion.ui.the.generated.image.is.no.longer.available").toStdString());
    const bool selection = context.mode == DiffusionPanel::Mode::Fill;
    const bool documentSpace = selection;
    const bool available = originalOwner && !originalOwner->closed &&
                           originalOwner->session.document();
    if (destination == DiffusionPanel::Destination::NewDocument) {
      QImage documentImage = image;
      if (selection && target) {
        const auto coverage = diffusionSelectionMask(*target, {}, true);
        documentImage = diffusionCompositeSelection(
            context.before,
            image.scaled(context.before.size(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation),
            coverage);
      }
      auto loaded = rasterLayerFromImage(documentImage, layerName);
      if (!loaded)
        throw std::runtime_error(loaded.error.toStdString());
      auto document = std::make_unique<core::Document>(
          core::CanvasSpec{loaded.extent, 96.0});
      if (!document->insertLayer(0, std::move(*loaded.layer)))
        throw std::runtime_error("Cannot create the generated document.");
      document->markUnsaved();
      refreshDiffusion();
      if (!initializeDocument(std::move(document), i18n::text("diffusion.ui.ai.image")))
        throw std::runtime_error("Cannot activate the generated document.");
    } else {
      auto owner = activeDocument_;
      if ((!owner || owner->closed) &&
          destination == DiffusionPanel::Destination::NewLayer && available)
        owner = originalOwner;
      if (!owner || owner->closed || !owner->session.document())
        throw std::runtime_error(i18n::text("diffusion.ui.open.a.document.or.choose.create.new").toStdString());
      if (owner != activeDocument_ && !activateDocument(owner->id))
        throw std::runtime_error(i18n::text("diffusion.ui.finish.the.current.document.interaction.before.applying").toStdString());
      std::unique_ptr<core::Command> command;
      std::optional<core::LayerId> newLayer;
      if (destination == DiffusionPanel::Destination::ReplaceSelected) {
        if (destination == DiffusionPanel::Destination::ReplaceSelected) {
          if (!session().activeLayer())
            throw std::runtime_error(i18n::text("diffusion.ui.select.a.raster.layer.to.replace").toStdString());
          const auto originalSelection =
              target ? target->selection : core::SelectionState{};
          target = captureDiffusionTarget(*session().document(),
                                          *session().activeLayer(), false);
          target->selection = originalSelection;
        }
        command = context.croppedEdit
            ? diffusionReplacePlacedCommand(*session().document(), *target, image,
                                             s.panel->selectedResult()->toDocument, &error)
            : diffusionReplaceCommand(*session().document(), *target, image,
                                      documentSpace, selection, &error);
      } else {
        auto layer =
            diffusionResultLayer(target ? &*target : nullptr, image,
                                 documentSpace, selection, layerName, &error);
        if (layer) {
          if (!selection)
            layer->localToDocument =
                coreTransform(s.panel->resultPlacement());
          newLayer = layer->id;
          command = std::make_unique<core::AddLayerCommand>(
              std::move(*layer), session().document()->layers().size(),
              session().activeLayer());
        }
      }
      if (!command)
        throw std::runtime_error(error.isEmpty()
                                     ? "Cannot prepare the AI result."
                                     : error.toStdString());
      refreshDiffusion();
      if (!executeDocumentCommand(std::move(command)))
        throw std::runtime_error(
            "Cannot apply the AI result. The document may have changed.");
      if (newLayer)
        session().setActiveLayer(*newLayer);
      fileState().untouched = false;
    }
    refreshDiffusion();
    // Applying is not discarding: keep the result and its source context in
    // history.
    refreshDiffusion();
    synchronizeUi(true, false);
    updateDocumentTitle();
    statusBar()->showMessage(i18n::text("diffusion.ui.ai.result.applied"), 4000);
  } catch (const std::exception &e) {
    refreshDiffusion();
    s.panel->setStatus(QString::fromUtf8(e.what()), true);
  }
}

void MainWindow::cancelDiffusion() {
  if (!diffusion_)
    return;
  auto &s = *diffusion_;
  const bool wasBusy = s.panel->isBusy();
  ++s.epoch;
  s.wanted = false;
  refreshDiffusion();
  if (s.worker->isRunning() && s.worker->pendingRequestCount()) {
    s.worker->cancel();
    s.panel->setStatus(i18n::text("diffusion.ui.cancelling"));
  } else {
    s.panel->setBusy(false);
    s.panel->setStatus(i18n::text("diffusion.ui.generation.cancelled.2"));
    if (wasBusy)
      updateDiffusionNotice(i18n::text("diffusion.ui.generation.cancelled"), 0, 0, true);
  }
}
void MainWindow::cleanupDiffusionFiles() {
  if (!diffusion_ || !diffusion_->cleanup || diffusion_->wanted ||
      diffusion_->worker->pendingRequestCount() ||
      !diffusion_->panel->results().isEmpty())
    return;
  for (const auto &path : diffusion_->sessionFiles)
    QDir(path).removeRecursively();
  diffusion_->sessionFiles.clear();
  diffusion_->cleanup = false;
}
void MainWindow::discardDiffusion(bool closePanel) {
  if (!diffusion_)
    return;
  auto &s = *diffusion_;
  cancelDiffusion();
  s.target.reset();
  s.owner.reset();
  s.before = {};
  auto inputs = s.panel->inputs();
  inputs.removeIf([](const auto &input) { return input.target; });
  s.panel->setInputs(std::move(inputs));
  s.cleanup = true;
  cleanupDiffusionFiles();
  if (closePanel)
    workspace_->setSideToolOverlay(s.shell, false);
}
void MainWindow::refreshDiffusion() {
  if (!diffusion_)
    return;
  auto &s = *diffusion_;
  const auto *doc = session().document();
  s.panel->setOutputCanvasSize(doc ? QSize(int(doc->canvas().extent.width), int(doc->canvas().extent.height)) : QSize{});
  const auto *layer = doc && session().activeLayer()
                          ? doc->layer(*session().activeLayer())
                          : nullptr;
  const bool selected =
      layer && std::holds_alternative<core::RasterLayer>(layer->payload) &&
      std::get<core::RasterLayer>(layer->payload).surface &&
      std::get<core::RasterLayer>(layer->payload).surface->dataKind() == core::RasterDataKind::Color;
  s.panel->setTargetCompatible(selected);
  QVector<DiffusionPanel::ComparisonLayer> choices;
  if (doc) for (auto it = doc->layers().rbegin(); it != doc->layers().rend(); ++it) {
    const auto &candidate = *it;
    QString type;
    if (const auto *raster = std::get_if<core::RasterLayer>(&candidate.payload)) {
      if (!raster->surface || raster->surface->dataKind() != core::RasterDataKind::Color) continue;
      type = i18n::text("psd.review.type.raster");
    } else if (std::holds_alternative<core::TextLayer>(candidate.payload)) type = i18n::text("psd.review.type.text");
    else if (std::holds_alternative<core::ShapeLayer>(candidate.payload)) type = i18n::text("psd.review.type.shape");
    else continue;
    choices.push_back({QString("%1:%2").arg(activeDocumentId()).arg(candidate.id),
                       QString::fromStdString(candidate.name), type, layerModel_->cachedContentThumbnail(candidate.id)});
  }
  s.panel->setComparisonLayers(std::move(choices));
  if (s.panel->comparisonActive()) {
    const auto key = QString("%1:%2:%3")
                         .arg(s.panel->comparisonLayerId())
                         .arg(doc ? doc->revision() : 0)
                         .arg(doc ? doc->contentState() : 0);
    if (key != s.comparisonKey) compareDiffusionLayer();
  }
}
void MainWindow::resetDiffusionSize() {
  if (!diffusion_)
    return;
  if (diffusion_->panel->mode() == DiffusionPanel::Mode::Edit &&
      !diffusion_->panel->inputs().isEmpty())
    diffusion_->panel->setResolution(
        diffusion_->panel->inputs().front().outputSize);
  else if (const auto *doc = session().document())
    diffusion_->panel->setResolution(QSize(int(doc->canvas().extent.width),
                                           int(doc->canvas().extent.height)));
}
void MainWindow::matchSelectedDiffusionSize() {
  if (!diffusion_ || diffusion_->panel->isBusy()) return;
  try {
    if (const auto *input = diffusion_->panel->selectedInput()) {
      if (input->image.isNull()) throw std::runtime_error(i18n::text("diffusion.ui.this.input.has.no.pixels.inside.its").toStdString());
      diffusion_->panel->setResolution(input->outputSize);
      return;
    }
    const auto *doc = session().document();
    if (!doc || !session().activeLayer()) throw std::runtime_error(i18n::text("diffusion.ui.select.an.input.image.or.a.layer").toStdString());
    if (!doc->layer(*session().activeLayer())) throw std::runtime_error(i18n::text("diffusion.ui.select.an.image.layer.rather.than.a").toStdString());
    for (const auto &input : diffusion_->panel->inputs())
      if (input.cropToCanvas && !input.image.isNull() &&
          input.metadata["documentId"].toString() == QString::number(activeDocumentId()) &&
          input.metadata["layerId"].toString() == QString::number(*session().activeLayer())) {
        diffusion_->panel->setResolution(input.outputSize);
        return;
      }
    auto copy = *doc;
    copy.layer(*session().activeLayer())->visible = true;
    const std::array ids{*session().activeLayer()};
    const auto bounds = evaluatedLayerItemsBounds(copy, ids);
    if (!bounds.error.isEmpty()) throw std::runtime_error(bounds.error.toStdString());
    if (bounds.rect.empty()) throw std::runtime_error(i18n::text("diffusion.ui.the.selected.layer.has.no.visible.bounds").toStdString());
    diffusion_->panel->setResolution(QSize(bounds.rect.width, bounds.rect.height));
  } catch (const std::exception &e) {
    diffusion_->panel->setStatus(QString::fromUtf8(e.what()), true);
  }
}
void MainWindow::compareDiffusionLayer() {
  if (!diffusion_)
    return;
  auto &s = *diffusion_;
  const auto *doc = session().document();
  const auto identity = s.panel->comparisonLayerId().split(':');
  const auto id = identity.size() == 2 ? identity[1].toULongLong() : 0;
  const auto *layer = doc && identity.size() == 2 && identity[0] == QString::number(activeDocumentId())
                          ? doc->layer(id) : nullptr;
  if (!layer || std::holds_alternative<core::AdjustmentLayer>(layer->payload)) {
    s.panel->setComparisonReference({}, {}, {});
    return;
  }
  try {
    // A comparison is an immutable isolated appearance, including masks and
    // effects. Hidden sources remain inspectable without changing visibility.
    auto copy = *doc;
    copy.layer(id)->visible = true;
    const auto rendered = diffusionRenderedLayer(copy, id);
    if (!rendered)
      throw std::runtime_error(rendered.error.toStdString());
    s.comparisonKey = QString("%1:%2:%3:%4")
                          .arg(activeDocumentId())
                          .arg(id)
                          .arg(doc->revision())
                          .arg(doc->contentState());
    s.panel->setComparisonReference(
        srgb(rendered.image),
        QTransform::fromTranslate(rendered.origin.x, rendered.origin.y),
        QString::fromStdString(layer->name));
  } catch (const std::exception &e) {
    s.panel->setStatus(QString::fromUtf8(e.what()), true);
    s.panel->setComparisonReference({}, {}, {});
  }
}

void MainWindow::refreshDiffusionPreview() {
  if (!diffusion_)
    return;
  auto &s = *diffusion_;
  const auto *result = s.panel->selectedResult();
  if (!result)
    return;
  const auto context =
      s.resultContexts.value(result->metadata["contextId"].toString());
  if (context.mode != DiffusionPanel::Mode::Fill || !context.target)
    return;
  try {
    const auto path =
        result->metadata.value("image").toObject().value("path").toString();
    const QImage generated(path);
    if (generated.isNull())
      return;
    const auto preview = diffusionFillPreview(
        *context.target, context.before, generated,
        s.panel->destination() == DiffusionPanel::Destination::NewLayer);
    s.panel->setResultPreview(s.panel->selectedResultIndex(), preview);
  } catch (const std::exception &error) {
    s.panel->setStatus(QString::fromUtf8(error.what()), true);
  }
}
void MainWindow::shutdownDiffusion() {
  if (!diffusion_)
    return;
  auto state = diffusion_;
  workspace_->setTaskNotificationOverlay(nullptr, false);
  state->shuttingDown = true;
  if (persistWindowState_)
    state->panel->saveSettings();
  state->panel->onGenerate = {};
  state->panel->onApply = {};
  state->panel->onCancel = {};
  state->panel->onDiscard = {};
  state->panel->onClose = {};
  state->panel->onAddInput = {};
  state->panel->onModeChanged = {};
  state->panel->onUnloadModel = {};
  state->panel->onResultSelected = {};
  state->panel->onDestinationChanged = {};
  state->panel->onInputsChanged = {};
  state->panel->onResetSize = {};
  state->panel->onMatchSelectedSize = {};
  state->panel->onCompareRequest = {};
  state->panel->onPresentPreview = {};
  // Finish the preview's workspace shield while the canvas/focus bridge still
  // exists; QObject child destruction happens after those surfaces tear down.
  state->panel->clearResults();
  state->worker.reset();
  diffusion_.reset();
}

void MainWindow::addDiffusionInput(int sourceValue) {
  if (!diffusion_ || diffusion_->panel->isBusy() || !diffusion_->panel->supportsImageInputs())
    return;
  auto &s = *diffusion_;
  const auto source = static_cast<DiffusionPanel::InputSource>(sourceValue);
  try {
    DiffusionPanel::Input input;
    if (source == DiffusionPanel::InputSource::File) {
      QFileDialog chooser(
          s.panel, i18n::text("diffusion.ui.add.reference.image"), {},
          i18n::text("diffusion.ui.images.png.jpg.jpeg.webp.bmp.tif"));
      chooser.setFileMode(QFileDialog::ExistingFile);
      preparePopupOwnership(&chooser, s.panel);
      if (chooser.exec() != QDialog::Accepted ||
          chooser.selectedFiles().isEmpty())
        return;
      const auto path = chooser.selectedFiles().front();
      QImageReader reader(path);
      reader.setAutoTransform(true);
      input.image = srgb(reader.read());
      input.label = QFileInfo(path).fileName();
      input.source = i18n::text("diffusion.ui.file");
    } else {
      auto owner = activeDocument_;
      std::optional<core::LayerId> id =
          owner ? owner->session.activeLayer() : std::optional<core::LayerId>{};
      if (source == DiffusionPanel::InputSource::DocumentLayerAppearance ||
          source == DiffusionPanel::InputSource::DocumentLayerPixels) {
        QStringList labels;
        std::vector<std::pair<std::shared_ptr<DocumentContext>, core::LayerId>>
            choices;
        for (const auto &document : documents_)
          for (const auto &layer : document->session.document()->layers()) {
            labels.append(document->displayName + " / " +
                          QString::fromStdString(layer.name) +
                          QString(" [%1]").arg(layer.id));
            choices.emplace_back(document, layer.id);
          }
        WorkspaceDialog presenter(*workspace_, *this);
        QInputDialog chooser(&presenter,
                             Qt::SubWindow | Qt::FramelessWindowHint);
        chooser.setWindowFlag(Qt::WindowMinMaxButtonsHint, false);
        chooser.setWindowTitle(i18n::text("diffusion.ui.reference.layer"));
        chooser.setLabelText(i18n::text("diffusion.ui.document.layer"));
        chooser.setComboBoxItems(labels);
        chooser.setComboBoxEditable(false);
        if (presenter.exec(chooser) != QDialog::Accepted)
          return;
        const auto index = labels.indexOf(chooser.textValue());
        if (index < 0)
          return;
        owner = choices[std::size_t(index)].first;
        id = choices[std::size_t(index)].second;
      }
      if (!owner || owner->closed || !owner->session.document())
        throw std::runtime_error(i18n::text("diffusion.ui.open.a.document.before.adding.this.input").toStdString());
      auto *doc = owner->session.document();
      if (source == DiffusionPanel::InputSource::VisibleCanvas) {
        const auto rendered = flattenDocument(*doc);
        if (!rendered)
          throw std::runtime_error(rendered.error.toStdString());
        input.image = srgb(rendered.image);
        input.label = owner->displayName;
        input.source = i18n::text("diffusion.ui.visible.canvas");
        input.metadata = {{"documentId", QString::number(owner->id)},
                          {"visibleCanvas", true}};
      } else {
        const auto *layer = id ? doc->layer(*id) : nullptr;
        if (!layer)
          throw std::runtime_error(i18n::text("diffusion.ui.select.a.layer.first").toStdString());
        const bool raw =
            source == DiffusionPanel::InputSource::CurrentLayerPixels ||
            source == DiffusionPanel::InputSource::DocumentLayerPixels;
        if (raw) {
          input.image = diffusionRawLayer(*layer);
          input.toDocument = QTransform::fromTranslate(layer->rasterOrigin.x,
                                                       layer->rasterOrigin.y) *
                             qtTransform(layer->localToDocument);
        } else {
          const auto rendered = diffusionRenderedLayer(*doc, *id);
          if (!rendered)
            throw std::runtime_error(rendered.error.toStdString());
          input.image = srgb(rendered.image);
          input.toDocument =
              QTransform::fromTranslate(rendered.origin.x, rendered.origin.y);
        }
        input.label = QString::fromStdString(layer->name);
        input.source =
            raw ? i18n::text("diffusion.ui.raw.pixels") : i18n::text("diffusion.ui.rendered.appearance.isolated.layer");
        input.metadata = {{"documentId", QString::number(owner->id)},
                          {"layerId", QString::number(*id)}};
        input.sourceCanvas = QSize(int(doc->canvas().extent.width), int(doc->canvas().extent.height));
      }
    }
    s.panel->addInput(std::move(input));
  } catch (const std::exception &e) {
    s.panel->setStatus(QString::fromUtf8(e.what()), true);
  }
}

bool MainWindow::diffusionEvent(QObject *watched, QEvent *event) {
  // The owned preview has ordinary Qt keyboard focus, but the native Vulkan
  // host may still carry IME events. Follow that focus without routing input
  // to a different dialog or back to the canvas.
  if (diffusion_ && event->type() == QEvent::InputMethod) {
    auto *focus = QApplication::focusWidget();
    auto *native = qobject_cast<QWindow *>(watched);
    if (focus && native && focus->isEnabled() && focus->isVisible() &&
        focus->window()->objectName() == "DiffusionPreviewDialog" &&
        popupBelongsTo(focus, diffusion_->panel) &&
        (native == windowHandle() || native == canvasWindow_ ||
         native == workspace_->panelOverlay()->windowHandle() || native == focus->window()->windowHandle())) {
      QCoreApplication::sendEvent(focus, event);
      return true;
    }
  }
  if (diffusion_ && watched == diffusion_->shell &&
      event->type() == QEvent::Close && !diffusion_->shuttingDown) {
    hideDiffusion();
    event->accept();
    return true;
  }
  return false;
}
void MainWindow::hideDiffusion() {
  if (!diffusion_)
    return;
  diffusion_->panelOpen = false;
  workspace_->setSideToolOverlay(diffusion_->shell, false);
  if (diffusion_->notice)
    workspace_->setTaskNotificationOverlay(diffusion_->notice,
                                           !diffusion_->noticeDismissed);
}
bool MainWindow::guardDiffusionClose(std::optional<DocumentInstanceId> id) {
  if (!diffusion_ || !diffusion_->panel->isBusy() ||
      (id && !diffusion_->jobDocuments.contains(*id)))
    return true;
  if (workspaceDialog_)
    return false;
  WorkspaceDialog presenter(*workspace_, *this);
  QMessageBox dialog(
      QMessageBox::Question, id ? i18n::text("diffusion.ui.close.document") : i18n::text("diffusion.ui.quit.vulkana"),
      id ? i18n::text("diffusion.ui.ai.generation.is.using.an.image.captured")
         : i18n::text("diffusion.ui.ai.generation.is.still.running.quitting.vulkana"),
      QMessageBox::NoButton, &presenter);
  dialog.setObjectName("DiffusionClosePrompt");
  dialog.setWindowFlags(Qt::SubWindow | Qt::FramelessWindowHint);
  dialog.setWindowFlag(Qt::WindowMinMaxButtonsHint, false);
  auto *proceed =
      dialog.addButton(id ? i18n::text("diffusion.ui.close.document.2") : i18n::text("diffusion.ui.quit"), QMessageBox::AcceptRole);
  auto *cancel = dialog.addButton(QMessageBox::Cancel);
  dialog.setDefaultButton(cancel);
  dialog.setEscapeButton(cancel);
  const QScopedValueRollback active(workspaceDialog_, &presenter);
  presenter.exec(dialog);
  return dialog.clickedButton() == proceed;
}
void MainWindow::updateDiffusionNotice(const i18n::Message &message, int step,
                                       int total, bool finished) {
  if (!diffusion_ || diffusion_->shuttingDown)
    return;
  auto &s = *diffusion_;
  s.noticeFinished = finished;
  if (finished)
    s.noticeDismissed = s.panelOpen; // An observed completion must not reappear on closing the panel.
  if (!s.notice) {
    s.notice = new QFrame(workspace_->panelOverlay());
    s.notice->setObjectName("DiffusionProgressNotification");
    s.notice->setAttribute(Qt::WA_ShowWithoutActivating);
    s.notice->setStyleSheet(
        QString(
            "QFrame#DiffusionProgressNotification { background: %1; border: "
            "1px solid %2; border-left: 3px solid %3; border-radius: 4px; }")
            .arg(themeColor(ThemeColor::Surface).name(),
                 themeColor(ThemeColor::Border).name(),
                 themeColor(ThemeColor::Accent).name()));
    auto *layout = new QVBoxLayout(s.notice);
    layout->setContentsMargins(10, 8, 8, 8);
    layout->setSpacing(6);
    auto *row = new QHBoxLayout;
    s.noticeLabel = new QLabel;
    s.noticeLabel->setWordWrap(true);
    row->addWidget(s.noticeLabel, 1);
    s.noticeOpen = new QToolButton;
    s.noticeOpen->setObjectName("DiffusionNoticeOpen");
    s.noticeOpen->setIcon(toolGlyph(ToolGlyph::ZoomActual));
    i18n::setToolTip(s.noticeOpen, i18n::text("diffusion.ui.view.latest.preview"));
    i18n::setAccessibleName(s.noticeOpen, i18n::text("diffusion.ui.view.latest.preview"));
    row->addWidget(s.noticeOpen);
    auto *close = new QToolButton;
    close->setObjectName("DiffusionNoticeDismiss");
    close->setIcon(toolGlyph(ToolGlyph::Close));
    i18n::setToolTip(close, i18n::text("diffusion.ui.dismiss.notification"));
    i18n::setAccessibleName(close, i18n::text("diffusion.ui.dismiss.notification"));
    row->addWidget(close);
    layout->addLayout(row);
    s.noticeProgress = new QProgressBar;
    s.noticeProgress->setFixedHeight(5);
    s.noticeProgress->setTextVisible(false);
    layout->addWidget(s.noticeProgress);
    connect(close, &QToolButton::clicked, this, [this] {
      diffusion_->noticeDismissed = true;
      workspace_->setTaskNotificationOverlay(diffusion_->notice, false);
    });
    connect(s.noticeOpen, &QToolButton::clicked, this, [this] {
      if (workspaceDialog_) return;
      openDiffusion(-1);
      diffusion_->panel->showLatestPreview();
    });
  }
  i18n::setText(s.noticeLabel, total > 0 ? message + " " + QString::number(std::clamp(step * 100 / total, 0, 100)) + "%" : message);
  s.noticeProgress->setVisible(!finished);
  s.noticeProgress->setRange(0, total);
  s.noticeProgress->setValue(step);
  workspace_->setTaskNotificationOverlay(
      s.notice, !s.noticeDismissed && !s.panelOpen);
  if (finished && !s.panelOpen)
    statusBar()->showMessage(message, 5000);
}
} // namespace imageeditor::ui
