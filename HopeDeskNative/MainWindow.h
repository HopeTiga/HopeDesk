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
class WebrtcManager;

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
    void onShowRttChecked(bool checked);

    void on_checkHwAccel_checkStateChanged(const Qt::CheckState &arg1);
    void on_checkHwDec_checkStateChanged(const Qt::CheckState &arg1);

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
    void updateNetworkTypeUI(int type, double rttMs);
    void refreshNetworkBadge();  // 用缓存的类型/RTT 重绘徽章(开关切换时调用)
    void hideNetworkBadge();     // 断开/超时时统一隐藏徽章并清空 RTT 缓存
    void saveEncodeConfig();     // 把 6 个编码配置成员落盘(QSettings)
    void syncConfigToManager();  // 把当前全部桌面配置同步到 WebrtcManager(RESTART 等内部复用路径需要)

    void moveToCenter();
private:
    Ui::MainWindow *ui;
    VideoWidget* videoWidget;
    std::shared_ptr<WebrtcManager> webrtcManager;
    QSettings* settings;
    QSystemTrayIcon *trayIcon = nullptr;
    QMenu *trayMenu = nullptr;

    bool isSignalConnected;
    bool isRemoteConnected;
    std::atomic<bool> reallyExit {false};

    int reConnectNums;
    QTimer* reconnectTimer;
    QTimer* remoteConnectionTimer;
    QTimer* statsTimer = nullptr;        // 周期性拉取 WebRTC 统计以刷新 RTT
    double lastRttMs = -1.0;             // 缓存上一次 RTT(服务器 STATS 路径无 RTT 时沿用)
    int lastConnectionType = 0;          // 缓存上一次连接类型,开关切换时用于重绘徽章
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
    int webrtcEnableNvenc = 0;  // 硬件编码(NVENC),连接时发给 System
    int webrtcEnableNvdec = 0;  // 硬件解码(MF/D3D11),Native 本地

    // 编码配置:UI 用 Mbps,WebrtcDeskConfig 里存 bps(1 Mbps = 1000000 bps)
    // 请求组:随 REQUEST 发给远端 System;本地组:仅存,由本地 TCP 给本机 System
    int requestMaxBitrateMbps = 15;
    int requestMinBitrateMbps = 15;
    int requestMaxFramerate   = 144;
    int localMaxBitrateMbps   = 15;
    int localMinBitrateMbps   = 15;
    int localMaxFramerate     = 144;

    bool showRttEnabled = false;  // 是否在连接徽章上显示网络 RTT(可配置)
};

}
}
