#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QListWidgetItem>
#include <QCloseEvent>
#include <QLabel>

class VideoWidget;
class WebRTCRemoteClient;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

    void resizeEvent(QResizeEvent* event) override;  // 添加这一行

private Q_SLOTS:
    // 连接控制
    void onConnectClicked();
    void onDisconnectClicked();

    // 账号管理
    void onAccountChanged(int index);
    void onAddAccountClicked();
    void onRemoveAccountClicked();
    void onTargetSelectionChanged();
    void onSendRequestClicked();

    // 远程操控
    void onDisconnectRemoteControl();

    // WebRTC客户端事件
    void onConnectionStateChanged(bool connected);
    void onClientError(const QString& error);

    // 菜单操作
    void onAbout();
    void onSettings();
    void onFullScreen();

    // 视频窗口控制
    void onShowVideoWindow();
    void onHideVideoWindow();

    // 状态更新
    void updateStatus();

    void onRemoteControlStarted();

    // 被远程端断开连接的处理
    void onRemoteDisconnectedByPeer();

    // 远程连接超时处理
    void onRemoteConnectionTimeout();

    void onDeviceItemClicked(QListWidgetItem* item);


private:
    // UI初始化
    void setupConnections();
    void applyModernStyles();

    // 设置管理
    void loadSettings();
    void saveSettings();
    void loadAccounts();
    void saveAccounts();

    void createVideoWidget();

    QString createStatusLabelStyle(const QString& type);

    // 状态管理
    void updateConnectionState(bool connected);
    void showErrorMessage(const QString& title, const QString& message);
    void updateTargetList();

    // 初始化设备列表
    void initializeDeviceLists();

    // WebRTC回调设置
    void setupWebRTCCallbacks();

    // 配置文件加载
    void loadConfigFile();


private:
    Ui::MainWindow *ui;

    // 主要组件
    VideoWidget* videoWidget;
    WebRTCRemoteClient* webRTCRemoteClient;

    // 设置
    QSettings* settings;

    // 状态
    bool isConnected;
    bool isFullScreen;
    bool isRemoteConnected;
    QString lastError;

    // 账号列表
    QStringList accountList;
    QStringList targetList;

    int reConnectNums;
    int reConnectTimes;

    // 远程连接超时定时器
    QTimer* remoteConnectionTimer;
    static const int REMOTE_CONNECTION_TIMEOUT = 15000;

    QLabel* background;
};
