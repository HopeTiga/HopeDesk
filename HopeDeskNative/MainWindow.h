#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QListWidgetItem>
#include <QJsonObject>
#include <atomic>
#include <QSystemTrayIcon>
#include "CustomDialogs.h" // 确保引用了你的弹窗头文件

QT_BEGIN_NAMESPACE
class QTabWidget;
class QCheckBox;
class QLineEdit;
class QSpinBox;
QT_END_NAMESPACE

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
    void onShowFpsChecked(bool checked);

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
    int webrtcEnableD3D11 = 0;  // 硬件解码(MF/D3D11),Native 本地

    // 编码配置:UI 用 Mbps,WebrtcDeskConfig 里存 bps(1 Mbps = 1000000 bps)
    // 请求组:随 REQUEST 发给远端 System;本地组:仅存,由本地 TCP 给本机 System
    int requestMaxBitrateMbps = 15;
    int requestMinBitrateMbps = 15;
    int requestMaxFramerate   = 144;
    int localMaxBitrateMbps   = 15;
    int localMinBitrateMbps   = 15;
    int localMaxFramerate     = 144;

    // Hope Vitrual Display 虚拟显示器配置(独立于 WebRTC 帧率)
    int desktopWidth       = 1920;
    int desktopHeight      = 1080;
    int desktopRefreshRate = 144;

    bool showRttEnabled = false;  // 是否在连接徽章上显示网络 RTT(可配置)
    bool showFpsEnabled = false;  // 是否显示实际渲染帧率(可配置)
    QTimer* fpsDisplayTimer = nullptr;  // 周期拉取 VideoWidget 帧率刷新 labelFps

    void startFpsDisplay();  // 连接建立 + 设置开启时启用
    void stopFpsDisplay();   // 断开/关闭设置时停止并清空显示

    // 系统设置 tab(垂直同步/信号服务器/STUN/TURN/WebRTC 程序)
    void buildSystemSettingsTab();   // 构建「系统设置」tab 并把原设置移入「通用设置」tab
    void loadSystemSettings();       // 从 config.ini 填充系统设置控件
    void applyVSyncToFormat();       // 把 verticalSyncEnabled 写入 QSurfaceFormat::defaultFormat
    void onApplySystemSettings();     // 保存 + 同步 Qt VSync + webrtcManagerConfig
    void onWebrtcServiceExeBrowse(); // 选择 WebRTC 系统程序可执行文件

    QTabWidget* settingsTabWidget = nullptr;
    QCheckBox*  verticalSyncCheckBox = nullptr;
    QCheckBox*  webrtcDebugLogCheckBox = nullptr;   // WebRTC 调试日志开关(Webrtc.DebugLog)
    QLineEdit*  signalServerHostEdit = nullptr;
    QSpinBox*   signalServerPortSpin = nullptr;
    QLineEdit*  stunHostEdit = nullptr;
    QLineEdit*  turnHostEdit = nullptr;
    QLineEdit*  turnUsernameEdit = nullptr;
    QLineEdit*  turnPasswordEdit = nullptr;
    QLineEdit*  webrtcServiceExeEdit = nullptr;
    QLineEdit*  webrtcServiceNameEdit = nullptr;

    // 虚拟显示器设置(系统设置 tab,代码构建)
    QLabel*  labelVddNameValue = nullptr;
    QSpinBox* spinDesktopWidth = nullptr;
    QSpinBox* spinDesktopHeight = nullptr;
    QSpinBox* spinDesktopRefreshRate = nullptr;

    bool verticalSyncEnabled = false;  // 当前垂直同步开关(运行期同步,下次会话生效)
};

}
}
