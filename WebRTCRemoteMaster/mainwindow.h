#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QListWidgetItem>
#include <QCloseEvent>
#include <QLabel>

namespace Ui {
class MainWindow;
}

namespace hope{

    namespace rtc{

    class VideoWidget;
    class WebRTCManager;

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
        void onManagerError(const QString& error);

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


        void on_modeComboBox_currentIndexChanged(int index);

        void on_gpuCheckBox_clicked(bool checked);

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
        WebRTCManager* manager;

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

        int webrtcModulesType = 0;

        int webrtcUseGPU = 0;
    };

    }

}
