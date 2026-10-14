#include "imageeditor/render/GraphicsDeviceProbe.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/platform/PerformanceSettings.hpp"
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVulkanInstance>

namespace imageeditor::render {
int runGraphicsDeviceProbe(QGuiApplication& app,const QString& device)
{
    platform::PerformanceSettings requested;requested.graphicsDevice=device;
    if(device.isEmpty()||platform::sanitizePerformanceSettings(requested).graphicsDevice!=device){qWarning()<<"Invalid GPU probe identity";return 2;}
    QTemporaryDir temporary;
    if(!temporary.isValid()){qWarning()<<"Cannot create GPU probe settings directory";return 2;}
    app.setOrganizationName(QStringLiteral("VulkanaGpuProbe"));
    app.setApplicationName(QStringLiteral("VulkanaGpuProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,temporary.path());
    QSettings settings;if(!platform::savePerformanceSettings(settings,requested)){qWarning()<<"Cannot save isolated GPU probe settings";return 2;}
    QVulkanInstance instance;instance.setApiVersion(QVersionNumber(1,2));
    if(!instance.create()){qWarning()<<"Cannot initialize GPU probe instance"<<instance.errorCode();return 2;}
    CanvasWindow window;window.setVulkanInstance(&instance);
    window.setTitle(QStringLiteral("Vulkana GPU compatibility check"));
    window.setFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus);
    window.resize(32,32);window.show();
    QTimer poll,deadline;bool presented=false;
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        const auto stats=window.rendererStats();
        if(!stats.presentQueuedFrames||presented)return;
        presented=true;
        if(stats.deviceId!=device.toStdString()||!stats.compositionError.empty()){app.exit(2);return;}
        // Queue-present success alone is insufficient: let the compositor
        // import the buffer and process any protocol error before reporting OK.
        QTimer::singleShot(300,&app,[&] {QGuiApplication::sync();app.exit(0);});
    });
    QObject::connect(&deadline,&QTimer::timeout,&app,[&] {app.exit(3);});
    poll.start(20);deadline.start(10000);
    return app.exec();
}
bool verifyStartupGraphicsDevice(const QString& executable)
{
    const auto device=platform::startupPerformanceSettings().graphicsDevice;
    if(device.isEmpty()||!QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))return true;
    // The parent owns the temporary root so even a driver/Qt fatal exit in the
    // child cannot strand its settings files after repeated launches.
    QTemporaryDir temporary;
    if(!temporary.isValid()) {
        qWarning()<<"Cannot prepare GPU compatibility check; using Auto for this launch";
        platform::useAutomaticGraphicsDeviceForProcess();return false;
    }
    QProcess probe;probe.setProcessChannelMode(QProcess::MergedChannels);
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TMPDIR"),temporary.path());probe.setProcessEnvironment(environment);
    probe.start(executable.isEmpty()?QCoreApplication::applicationFilePath():executable,
        {QString::fromLatin1(graphicsDeviceProbeOption),device,QStringLiteral("-platform"),QGuiApplication::platformName()});
    const bool finished=probe.waitForFinished(15000);
    if(!finished&&probe.state()!=QProcess::NotRunning){probe.kill();probe.waitForFinished(1000);}
    if(finished&&probe.exitStatus()==QProcess::NormalExit&&probe.exitCode()==0)return true;
    qWarning().noquote()<<"Preferred GPU presentation check failed; using Auto for this launch."
        <<"exit"<<probe.exitCode()<<"finished"<<finished<<QString::fromLocal8Bit(probe.readAll().right(8192));
    // Do not let Auto immediately retry the adapter that just failed. A helper
    // launch failure is not evidence against the GPU itself.
    platform::useAutomaticGraphicsDeviceForProcess(probe.error()!=QProcess::FailedToStart);
    return false;
}
}
