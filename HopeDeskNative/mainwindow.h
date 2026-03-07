#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QListWidgetItem>
#include <QJsonObject>
#include <atomic>
#include <QSystemTrayIcon>
#include "CustomDialogs.h" // 确保引用了你的弹窗头文件

namespace Ui {
class MainWindow;
}

namespace hope{
namespace rtc{

class VideoWidget;
class WebRTCManager;

struct DeviceInfo {
    QString name;
    QString id;
    qint64 lastAccess;
    QJsonObject toJson() const { return {{"name", name}, {"id", id}, {"lastAccess", lastAccess}}; }
    static DeviceInfo fromJson(const QJsonObject& json) { return {json["name"].toString(), json["id"].toString(), (qint64)json["lastAccess"].toDouble()}; }
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    // UI交互
    void onNavHomeClicked();
    void onNavDevicesClicked();
    void onNavSettingsClicked();
    void onBtnConnectClicked();
    void onBtnCopyCodeClicked();
    void onClearHistoryClicked();
    void onDeviceItemClicked(QListWidgetItem* item);
    void onAddDeviceClicked();
    void onDeviceGroupChanged(QListWidgetItem* current, QListWidgetItem* previous);

    // 核心逻辑
    void startSignalServerConnection();
    void onUserAvatarClicked();
    void onLogoutClicked();

    // WebRTC回调
    void onConnectionStateChanged(bool connected);
    void onRemoteControlStarted();
    void onRemoteDisconnectedByPeer();
    void onRemoteConnectionTimeout();

    // 系统
    void onSystemTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void showWindow();
    void quitApplication();

    // 设置
    void onModeChanged(int index);
    void onCodecChanged(int index);
    void onAccelerationChanged(int index);
    void onAudioChecked(bool checked);
    void onAutoStartChecked(bool checked);

private:
    void setupUI();
    void setupSignalSlots();
    void initConfigAndSettings();

    void checkLoginStatus();
    // 【修复】：补全了这两个函数的声明
    void updateLocalAccountUI();
    void setColorAvatar();

    void loadHistoryData();
    void loadFavoritesData();
    void addToHistory(const QString& id, const QString& name = "未知设备");

    void updateRecentListUI();
    void updateDeviceListUI(bool showFavorites);
    void updateStatusUI(const QString& status, const QString& styleClass);
    void updateNetworkTypeUI(int type);

    void moveToCenter();
private:
    Ui::MainWindow *ui;
    VideoWidget* videoWidget;
    WebRTCManager* manager;
    QSettings* settings;
    QSystemTrayIcon *trayIcon = nullptr;
    QMenu *trayMenu = nullptr;

    bool isSignalConnected;
    bool isRemoteConnected;
    std::atomic<bool> reallyExit {false};

    int reConnectNums;
    QTimer* reconnectTimer;
    QTimer* remoteConnectionTimer;
    static const int REMOTE_CONNECTION_TIMEOUT = 15000;

    QString defaultServerHost;
    int defaultServerPort;

    // 用户信息
    QString currentDeviceId;
    QString currentUserPwd;
    QString currentUserName;
    int currentAvatarIndex = 0;
    QString customAvatarPath;

    QList<DeviceInfo> historyList;
    QList<DeviceInfo> favoritesList;

    // WebRTC参数
    int webrtcModulesType = 0;
    int webrtcLevels = 2;
    int videoCodec = 4;
    int webrtcAudioEnable = 0;
};

}
}
