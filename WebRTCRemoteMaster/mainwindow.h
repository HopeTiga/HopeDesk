#pragma once

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QSpinBox>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <QMessageBox>
#include <QTimer>
#include <QSettings>
#include <QComboBox>
#include <QListWidget>

class VideoWidget;
class WebRTCRemoteClient;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

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

private:
    // UI初始化
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void setupConnections();
    void applyModernStyles();

    // 设置管理
    void loadSettings();
    void saveSettings();
    void loadAccounts();
    void saveAccounts();

    QString createStatusLabelStyle(const QString& type);

    QString createRegularLabelStyle();

    // 状态管理
    void updateConnectionState(bool connected);
    void showErrorMessage(const QString& title, const QString& message);
    void updateTargetList();

    // 样式辅助函数
    QString createButtonStyle(const QString& bgColor, const QString& hoverColor, const QString& textColor = "#FFFFFF");
    QString createGroupBoxStyle();
    QString createInputStyle();
    QString createListWidgetStyle();

private:
    // 主要组件
    VideoWidget* videoWidget;
    WebRTCRemoteClient* webRTCRemoteClient;

    // UI控件
    QWidget* centralWidget;
    QVBoxLayout* mainLayout;

    // 连接控制面板
    QGroupBox* connectionGroup;
    QGridLayout* connectionLayout;
    QLineEdit* serverAddressEdit;
    QSpinBox* portSpinBox;

    // 账号选择
    QLabel* accountLabel;
    QComboBox* accountComboBox;
    QPushButton* addAccountButton;
    QPushButton* removeAccountButton;

    QPushButton* connectButton;
    QPushButton* disconnectButton;
    QLabel* connectionStatusLabel;

    // 目标账号面板
    QGroupBox* targetGroup;
    QVBoxLayout* targetLayout;
    QListWidget* targetListWidget;
    QPushButton* sendRequestButton;
    QPushButton* disconnectRemoteButton;  // 新增断开远程操控按钮
    QLabel* targetStatusLabel;

    // 视频窗口控制按钮
    QPushButton* showVideoButton;
    QPushButton* hideVideoButton;

    // 菜单和工具栏
    QMenuBar* menuBars;
    QAction* aboutAction;
    QAction* settingsAction;
    QAction* fullScreenAction;
    QAction* exitAction;

    // 状态栏
    QStatusBar* statusBars;
    QLabel* statusLabel;
    QLabel* fpsLabel;
    QTimer* statusTimer;

    // 设置
    QSettings* settings;

    // 状态
    bool isConnected;
    bool isFullScreen;
    bool isRemoteConnected;  // 新增远程连接状态
    QString lastError;

    // 账号列表
    QStringList accountList;
    QStringList targetList; // 模拟的目标账号列表
};
