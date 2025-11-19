#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{

    QApplication app(argc, argv);
    app.setApplicationName("WebRTC远程桌面");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("WebRTCClient");
    app.setOrganizationDomain("webrtcclient.local");

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
