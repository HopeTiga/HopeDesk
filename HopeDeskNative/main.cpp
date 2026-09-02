#include <mimalloc/mimalloc.h>

#include "rtc/MainWindow.h"
#include <QApplication>
#include <QSurfaceFormat>
#include "utils/ConfigManager.h"
#include "utils/Utils.h"

int main(int argc, char *argv[])
{

    mi_version();

    ConfigManager::Instance().Load();
    bool verticalSyncEnabled = ConfigManager::Instance().GetBool("Render.VSync", false);

    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSwapInterval(verticalSyncEnabled ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QApplication app(argc, argv);
    app.setApplicationName("HopeDesk");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("HopeDesk");
    app.setOrganizationDomain("HopeDesk.local");
    app.setQuitOnLastWindowClosed(false);

    ConfigManager::Instance().Load();

    // WebRTC 调试日志:仅当「系统设置」勾选 Webrtc.DebugLog 时开启(native 自身也受开关控制)
    if (ConfigManager::Instance().GetBool("Webrtc.DebugLog", false)) {
        initWebrtcLogging();
    }

    // ===== 设置全局应用程序图标 =====
    QIcon appIcon(":/logo/res/hope.jpg");
    if (!appIcon.isNull()) {
        app.setWindowIcon(appIcon);
        qDebug() << "全局应用程序图标设置成功";
    } else {
        qDebug() << "警告：无法加载全局应用程序图标：:/logo/res/hope.jpg";
        qDebug() << "请检查资源文件是否正确添加到项目中";
    }
    setbuf(stdout, NULL);
    hope::rtc::MainWindow w;
    w.show();
    return app.exec();
}
