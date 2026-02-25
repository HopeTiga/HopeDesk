#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "VideoWidget.h"
#include "WebRTCManager.h"
#include <QApplication>
#include <QCloseEvent>
#include <QSplitter>
#include <QFrame>
#include <QKeySequence>
#include <QScreen>
#include <QDebug>
#include <QInputDialog>
#include <QGraphicsDropShadowEffect>
#include <QDir>
#include <QIntValidator>
#include <QStatusBar>
#include <QLabel>
#include <QTimer>
#include <QMessageBox>
#include "ConfigManager.h"

namespace hope{

    namespace rtc{

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , videoWidget(nullptr)
    , manager(nullptr)
    , settings(nullptr)
    , isConnected(false)
    , isFullScreen(false)
    , isRemoteConnected(false)
    , reConnectNums(0)
    , reConnectTimes(5000)
    , background(nullptr)
{
    ui->setupUi(this);

    QIcon appIcon(":/logo/res/hope.jpg");
    if (!appIcon.isNull()) {
        setWindowIcon(appIcon);
        QApplication::setWindowIcon(appIcon);
        qDebug() << "应用程序图标设置成功";
    } else {
        qDebug() << "警告：无法加载应用程序图标：:/logo/res/hope.jpg";
    }

    createTrayIcon();
    // 创建背景标签
    background = new QLabel(this);
    QPixmap bg(":/logo/res/windows.jpg");
    background->setPixmap(bg);
    background->setScaledContents(true);  // 关键：让图片自动缩放填充
    background->setGeometry(0, 0, width(), height());
    background->lower(); // 放到最底层
    // 初始化设置
    settings = new QSettings("WebRTCmanager", "Settings", this);

    // 初始化远程连接超时定时器
    remoteConnectionTimer = new QTimer(this);
    remoteConnectionTimer->setSingleShot(true);
    connect(remoteConnectionTimer, &QTimer::timeout, this, &MainWindow::onRemoteConnectionTimeout);

    // 初始化账号列表
    loadAccounts();

    // 设置端口验证器
    QIntValidator *portValidator = new QIntValidator(1, 65535, this);
    ui->portEdit->setValidator(portValidator);

    ui->refreshDeviceButton->setEnabled(false);

    // 初始化设备列表
    initializeDeviceLists();

    setupConnections();
    applyModernStyles();

    // 加载设置
    loadSettings();

    // 创建WebRTC客户端
    manager = new WebRTCManager(WebRTCRemoteState::nullRemote);

    // 设置WebRTC回调
    setupWebRTCCallbacks();

    // 读取配置文件
    loadConfigFile();

    // 初始化状态
    updateConnectionState(false);

    qDebug() << "MainWindow initialized";
}

MainWindow::~MainWindow()
{
    saveSettings();
    saveAccounts();

    if (videoWidget) {
        videoWidget->close();
        delete videoWidget;
    }

    if (manager) {
        delete manager;
    }

    delete ui;
}

void MainWindow::setupWebRTCCallbacks()
{
    manager->followRemoteHandle = ([this]() {
        // 放到UI线程执行
        QMetaObject::invokeMethod(this, "onRemoteControlStarted", Qt::QueuedConnection);
    });

    manager->remoteSuccessFulHandle = [this]() {
        // 放到UI线程执行
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer && remoteConnectionTimer->isActive()) {
                remoteConnectionTimer->stop();
            }

            if (!videoWidget) {
                createVideoWidget();
            }

            if (videoWidget) {
                videoWidget->showMaximized();
                videoWidget->raise();
                videoWidget->activateWindow();
            }

            ui->showVideoButton->setEnabled(true);
            ui->hideVideoButton->setEnabled(true);
            isRemoteConnected = true;
            ui->disconnectRemoteButton->setEnabled(true);

            QString targetId = ui->deviceListWidget->currentItem() ?
                                   ui->deviceListWidget->currentItem()->text() : "未知";
            ui->remoteStatusLabel->setText(QString("已成功连接到 %1").arg(targetId));
            ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("success"));

            if (statusBar()) {
                statusBar()->showMessage("远程连接已建立");
            }
        }, Qt::QueuedConnection);
    };

    manager->remoteFailedHandle = [this]() {
        // 放到UI线程执行
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer && remoteConnectionTimer->isActive()) {
                remoteConnectionTimer->stop();
            }

            ui->sendRequestButton->setEnabled(ui->deviceListWidget->currentItem() != nullptr);

            // 远程连接失败后，重置连接类型标签为服务器连接状态
            if (isConnected) {
                ui->connectionTypeBadge->setProperty("status", "connecting");
                ui->connectionTypeBadge->setText("已连接");
            } else {
                ui->connectionTypeBadge->setProperty("status", "idle");
                ui->connectionTypeBadge->setText("未连接");
            }
            updateConnectionTypeBadge();

            QString targetId = ui->deviceListWidget->currentItem() ?
                                   ui->deviceListWidget->currentItem()->text() : "未知";
            ui->remoteStatusLabel->setText(QString("无法连接到 %1：对方可能不在线或拒绝了连接").arg(targetId));
            ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("error"));

            if (statusBar()) {
                statusBar()->showMessage("远程连接失败");
            }

            showErrorMessage("连接失败", QString("无法连接到 %1\n对方可能不在线或拒绝了连接请求").arg(targetId));
        }, Qt::QueuedConnection);
    };

    manager->disConnectRemoteHandle = ([this]() {
        // 放到UI线程执行
        QMetaObject::invokeMethod(this, "onRemoteDisconnectedByPeer", Qt::QueuedConnection);
    });

    manager->msquicSocketConnectedHandle = [this](bool success) {
        QMetaObject::invokeMethod(this, [this, success]() {
            static bool hasEverConnected = false;

            if (success) {
                hasEverConnected = true;
                this->onConnectionStateChanged(true);
                this->reConnectNums = 0;
                LOG_INFO("MsquicServer Connected successfully.");

                // 连接成功，但尚未知道连接类型
                ui->connectionTypeBadge->setProperty("status", "connecting");
                ui->connectionTypeBadge->setText("已连接");
                updateConnectionTypeBadge();
            } else {
                if (!hasEverConnected) {
                    this->ui->connectButton->setEnabled(true);
                    this->ui->connectButton->setText("连接服务器");
                    this->ui->connectionStatusLabel->setText("连接异常");
                    this->ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("error"));

                    // 初始连接失败
                    ui->connectionTypeBadge->setProperty("status", "disconnected");
                    ui->connectionTypeBadge->setText("连接失败");
                    updateConnectionTypeBadge();

                    LOG_INFO("Initial connection failed.");
                } else {
                    this->isConnected = false;
                    this->updateConnectionState(false);
                    this->reConnectNums++;

                    LOG_INFO("Connection lost, attempting reconnect #%d" ,this->reConnectNums);

                    this->ui->connectionStatusLabel->setText(
                        QString("连接断开，将在15秒后进行第%1次重连...").arg(this->reConnectNums));
                    this->ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

                    // 断开重连状态
                    ui->connectionTypeBadge->setProperty("status", "reconnecting");
                    ui->connectionTypeBadge->setText("重连中");
                    updateConnectionTypeBadge();

                    if (statusBar()) {
                        statusBar()->showMessage(QString("将在15秒后进行第%1次重连").arg(this->reConnectNums));
                    }

                    const int reconnectDelayMs = 15000;
                    QTimer::singleShot(reconnectDelayMs, this, [this]() {
                        if (!this->isConnected) {
                            QString serverAddress = this->ui->serverAddressEdit->text().trimmed();
                            int port = this->ui->portEdit->text().toInt();
                            QString currentAccount = this->ui->accountComboBox->currentText();

                            if (!serverAddress.isEmpty() && !currentAccount.isEmpty() && currentAccount != "请添加账号") {
                                this->ui->connectionStatusLabel->setText(
                                    QString("正在进行第%1次重连...").arg(this->reConnectNums));
                                this->ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("info"));

                                // 重连状态
                                ui->connectionTypeBadge->setProperty("status", "reconnecting");
                                ui->connectionTypeBadge->setText("重连中");
                                updateConnectionTypeBadge();

                                if (statusBar()) {
                                    statusBar()->showMessage("正在重连...");
                                }

                                LOG_INFO("Retrying connect... #%d" ,this->reConnectNums);

                                this->manager->setAccountId(currentAccount.toStdString());
                                QString url = QString("%1:%2").arg(serverAddress).arg(port);

                                this->manager->connect(QString("%1:%2").arg(serverAddress).arg(port).toStdString());

                            } else {
                                this->ui->connectionStatusLabel->setText("连接断开（配置不完整）");
                                this->ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("error"));
                                if (statusBar()) {
                                    statusBar()->showMessage("连接已断开");
                                }
                                this->ui->connectButton->setEnabled(true);
                                this->ui->connectButton->setText("连接服务器");
                                this->reConnectNums = 0;

                                // 重置为未连接状态
                                ui->connectionTypeBadge->setProperty("status", "idle");
                                ui->connectionTypeBadge->setText("未连接");
                                updateConnectionTypeBadge();

                                LOG_INFO("Config incomplete, stop retry.");
                            }
                        }
                    });
                }
            }
        }, Qt::QueuedConnection);
    };


    manager->resetCursorHandle = [this]() {
        // 务必使用 QueuedConnection 确保在 UI 线程执行
        QMetaObject::invokeMethod(this, [this]() {
            // 在 UI 线程调用 Windows API，这是安全的，因为不需要跨线程广播等待
            SystemParametersInfo(SPI_SETCURSORS, 0, NULL, 0);
        }, Qt::QueuedConnection);
    };

    manager->onRTCStatsCollectorHandle = [this](int connectionType) {
        QMetaObject::invokeMethod(this, [this, connectionType]() {
            if (connectionType == 0) { // P2P
                ui->connectionTypeBadge->setProperty("status", "connected-p2p");
                ui->connectionTypeBadge->setText(" ⚡ 直连 ");
            } else { // TURN/Relay
                ui->connectionTypeBadge->setProperty("status", "connected-relay");
                ui->connectionTypeBadge->setText(" 🔄 中继 ");
            }
            updateConnectionTypeBadge();
        }, Qt::QueuedConnection);
    };

}

void MainWindow::updateConnectionTypeBadge()
{
    // 强制刷新样式
    ui->connectionTypeBadge->style()->unpolish(ui->connectionTypeBadge);
    ui->connectionTypeBadge->style()->polish(ui->connectionTypeBadge);
    ui->connectionTypeBadge->update();
}

void MainWindow::loadConfigFile()
{

}

void MainWindow::createTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "错误", "系统不支持托盘图标");
        return;
    }

    // 创建托盘菜单
    trayMenu = new QMenu(this);

    QAction *showAction = new QAction("显示主窗口", this);
    QAction *quitAction = new QAction("退出程序", this);

    connect(showAction, &QAction::triggered, this, &MainWindow::showWindow);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);

   trayMenu->addAction(showAction);
    trayMenu->addSeparator();
    trayMenu->addAction(quitAction);

    // 创建托盘图标
    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(QIcon(":/logo/res/hope.jpg"));  // 你的应用图标
    trayIcon->setToolTip("WebRTC-Native-Manager");
    trayIcon->setContextMenu(trayMenu);

    connect(trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::onSystemTrayActivated);

    trayIcon->show();
}

void MainWindow::onSystemTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::DoubleClick:    // 双击
        showWindow();
        break;
    default:
        break;
    }
}

void MainWindow::showWindow()
{
    show();           // 显示窗口
    raise();          // 提升到最前
    activateWindow(); // 激活窗口（任务栏高亮）
    setWindowState(Qt::WindowActive);  // 从最小化恢复
}

// ==================== 真正退出 ====================
void MainWindow::quitApplication()
{
    reallyExit = true;  // 设置标志位
    close();                   // 再次触发 closeEvent，这次会真正退出
    // 或者直接用：QApplication::quit();
}

void MainWindow::initializeDeviceLists()
{

    // 清空设备列表
    ui->deviceListWidget->clear();

    // 添加我的设备分组（可折叠）
    QListWidgetItem* myDeviceHeader = new QListWidgetItem("▶ 我的设备");
    myDeviceHeader->setData(Qt::UserRole, "my_devices_header"); // 标记为分组标题
    myDeviceHeader->setFlags(myDeviceHeader->flags() & ~Qt::ItemIsSelectable);
    myDeviceHeader->setBackground(QColor(240, 245, 255));
    myDeviceHeader->setForeground(QColor(66, 153, 225));
    QFont headerFont = myDeviceHeader->font();
    headerFont.setBold(true);
    myDeviceHeader->setFont(headerFont);
    ui->deviceListWidget->addItem(myDeviceHeader);


    // 添加云设备分组（可折叠）
    QListWidgetItem* cloudDeviceHeader = new QListWidgetItem("▶ 云设备");
    cloudDeviceHeader->setData(Qt::UserRole, "cloud_devices_header"); // 标记为分组标题
    cloudDeviceHeader->setFlags(cloudDeviceHeader->flags() & ~Qt::ItemIsSelectable);
    cloudDeviceHeader->setBackground(QColor(240, 245, 255));
    cloudDeviceHeader->setForeground(QColor(66, 153, 225));
    cloudDeviceHeader->setFont(headerFont);
    ui->deviceListWidget->addItem(cloudDeviceHeader);


    for (int i = 0; i < ui->deviceListWidget->count(); ++i) {
        QListWidgetItem* item = ui->deviceListWidget->item(i);
        QString headerType = item->data(Qt::UserRole).toString();
        if (!headerType.isEmpty()) {
            item->setText(item->text().replace("▶", "▼"));
        }
    }

}


void MainWindow::setupConnections()
{
    // 连接按钮
    connect(ui->connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(ui->disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);

    connect(ui->deviceListWidget, &QListWidget::itemClicked, this, &MainWindow::onDeviceItemClicked);
    // 账号管理
    connect(ui->accountComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onAccountChanged);

    // 设备列表
    connect(ui->refreshDeviceButton, &QPushButton::clicked, this, [this]() {
        if (isConnected) {
            updateTargetList();
        }
    });

    connect(ui->deviceListWidget, &QListWidget::itemSelectionChanged,
            this, &MainWindow::onTargetSelectionChanged);

    // 远程控制
    connect(ui->sendRequestButton, &QPushButton::clicked, this, &MainWindow::onSendRequestClicked);
    connect(ui->disconnectRemoteButton, &QPushButton::clicked, this, &MainWindow::onDisconnectRemoteControl);

    // 视频控制
    connect(ui->showVideoButton, &QPushButton::clicked, this, &MainWindow::onShowVideoWindow);
    connect(ui->hideVideoButton, &QPushButton::clicked, this, &MainWindow::onHideVideoWindow);

    // 菜单操作
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onAbout);
    connect(ui->actionSettings, &QAction::triggered, this, &MainWindow::onSettings);
    connect(ui->actionFullScreen, &QAction::triggered, this, &MainWindow::onFullScreen);
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);

    // 回车键连接
    connect(ui->serverAddressEdit, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);

    // 状态更新计时器
    QTimer* statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    statusTimer->start(1000);
}

void MainWindow::applyModernStyles()
{

    ui->connectionTypeBadge->setProperty("status", "idle");

    ui->connectionTypeBadge->setText("未连接");

    updateConnectionTypeBadge();

}

void MainWindow::loadSettings()
{
    restoreGeometry(settings->value("geometry").toByteArray());
    restoreState(settings->value("windowState").toByteArray());

    ui->serverAddressEdit->setText(QString::fromStdString(ConfigManager::Instance().GetString("WebRTCSignalServer.Host")));
    ui->portEdit->setText(QString::number(ConfigManager::Instance().GetInt("WebRTCSignalServer.Port")));

    QString lastAccount = settings->value("lastAccount", "").toString();
    if (!lastAccount.isEmpty()) {
        int index = ui->accountComboBox->findText(lastAccount);
        if (index >= 0) {
            ui->accountComboBox->setCurrentIndex(index);
        }
    }
}

void MainWindow::saveSettings()
{
    settings->setValue("geometry", saveGeometry());
    settings->setValue("windowState", saveState());

    if (ui->accountComboBox->currentIndex() >= 0 && !accountList.isEmpty()) {
        settings->setValue("lastAccount", ui->accountComboBox->currentText());
    }

    if (videoWidget) {
        settings->setValue("videoWindowGeometry", videoWidget->saveGeometry());
    }
}

void MainWindow::loadAccounts()
{
    int size = settings->beginReadArray("accounts");
    for (int i = 0; i < size; ++i) {
        settings->setArrayIndex(i);
        accountList << settings->value("email").toString();
    }
    settings->endArray();

    // 更新账号下拉框 - 添加默认账号
    ui->accountComboBox->clear();
    if (accountList.isEmpty()) {
        // 添加默认账号
        accountList << "913140924@qq.com";
        accountList << "2044580040@qq.com";
        accountList << "396887208@qq.com";
        accountList << "147718387@qq.com";
        saveAccounts(); // 保存默认账号到设置
    }
    ui->accountComboBox->addItems(accountList);

    // 默认选择第一个账号
    if (!accountList.isEmpty()) {
        ui->accountComboBox->setCurrentIndex(0);
    }
}

void MainWindow::saveAccounts()
{
    settings->beginWriteArray("accounts");
    for (int i = 0; i < accountList.size(); ++i) {
        settings->setArrayIndex(i);
        settings->setValue("email", accountList.at(i));
    }
    settings->endArray();
}

void MainWindow::createVideoWidget()
{
    if (videoWidget) {
        return;
    }

    videoWidget = new VideoWidget();
    videoWidget->setWindowTitle("WebRTC远程桌面");

    videoWidget->setWindowFlags(Qt::Window |
                                Qt::WindowTitleHint |
                                Qt::WindowSystemMenuHint |
                                Qt::WindowMinMaxButtonsHint |
                                Qt::WindowCloseButtonHint);

    videoWidget->setWindowState(Qt::WindowNoState);
    videoWidget->setMinimumSize(640, 480);

    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        QRect screenGeometry = screen->geometry();
        int x = (screenGeometry.width() - 1280) / 2;
        int y = (screenGeometry.height() - 720) / 2;
        videoWidget->move(x, y);
    }

    videoWidget->setWebRTCManager(manager);

    manager->setVideoFrameCallback([this](std::shared_ptr<VideoFrame> frame) {
        if (videoWidget) {
            videoWidget->displayFrame(frame);
        }
    });

    if (settings->contains("videoWindowGeometry")) {
        videoWidget->restoreGeometry(settings->value("videoWindowGeometry").toByteArray());
    }

    connect(videoWidget, &QWidget::destroyed, this, [this]() {
        videoWidget = nullptr;
        ui->showVideoButton->setEnabled(false);
        ui->hideVideoButton->setEnabled(false);
        ui->disconnectRemoteButton->setEnabled(false);
    });
}

void MainWindow::onConnectClicked()
{
    if (isConnected || accountList.isEmpty()) {
        return;
    }

    QString serverAddress = ui->serverAddressEdit->text().trimmed();
    int port = ui->portEdit->text().toInt();
    QString currentAccount = ui->accountComboBox->currentText();

    if (serverAddress.isEmpty()) {
        showErrorMessage("连接错误", "请输入服务器地址");
        return;
    }

    if (currentAccount.isEmpty() || currentAccount == "请添加账号") {
        showErrorMessage("连接错误", "请先添加并选择一个账号");
        return;
    }

    // 更新连接类型标签为连接中状态
    ui->connectionTypeBadge->setProperty("status", "connecting");
    ui->connectionTypeBadge->setText("连接中");
    updateConnectionTypeBadge();

    manager->setAccountId(currentAccount.toStdString());

    ui->connectButton->setEnabled(false);
    ui->connectButton->setText("连接中...");
    ui->connectionStatusLabel->setText("正在连接...");
    ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    QString url = QString("%1:%2").arg(serverAddress).arg(port);

    QTimer::singleShot(0, [this, url]() {
        this->manager->connect(url.toStdString());
    });
}

void MainWindow::onDisconnectClicked()
{
    if (!isConnected) {
        return;
    }

    if (remoteConnectionTimer->isActive()) {
        remoteConnectionTimer->stop();
    }

    // 1. 先切断回调，防止后台线程继续调用 UI
    if (manager) {
        manager->setVideoFrameCallback(nullptr);
    }

    // 2. 发起异步断开 (后台线程开始清理 WebRTC 资源)
    manager->disConnect();

    // 3. 更新连接类型标签
    ui->connectionTypeBadge->setProperty("status", "disconnected");
    ui->connectionTypeBadge->setText("断开中");
    updateConnectionTypeBadge();

    // 4. 更新 UI 状态
    onConnectionStateChanged(false);

    // 5. 安全处理 videoWidget
    if (videoWidget) {
        videoWidget->hide(); // 先隐藏，让用户感觉"关掉了"
        videoWidget->deleteLater();
        videoWidget = nullptr; // 置空指针，防止后续逻辑误用
    }

    ui->showVideoButton->setEnabled(false);
    ui->hideVideoButton->setEnabled(false);
    ui->disconnectRemoteButton->setEnabled(false);
    isRemoteConnected = false;

    // 更新连接类型标签为未连接状态
    QTimer::singleShot(500, [this]() {
        ui->connectionTypeBadge->setProperty("status", "idle");
        ui->connectionTypeBadge->setText("未连接");
        updateConnectionTypeBadge();
    });
}

void MainWindow::onAccountChanged(int index)
{
    Q_UNUSED(index)
    bool hasValidAccount = !accountList.isEmpty() &&
                           ui->accountComboBox->currentText() != "请添加账号";
    ui->connectButton->setEnabled(hasValidAccount && !isConnected);
}


void MainWindow::onTargetSelectionChanged()
{
    QListWidgetItem* currentItem = ui->deviceListWidget->currentItem();

    // 如果是提示项或分组标题，禁用发送请求按钮
    if (!currentItem ||
        !(currentItem->flags() & Qt::ItemIsSelectable) ||
        currentItem->text() == "请先连接服务器") {
        ui->sendRequestButton->setEnabled(false);
        ui->remoteStatusLabel->setText("请选择目标设备");
        ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("default"));
        return;
    }

    // 正常的选择逻辑
    QString targetId = currentItem->text();
    ui->remoteStatusLabel->setText(QString("已选择: %1").arg(targetId));
    ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("info"));
    manager->setTargetId(targetId.toStdString());
    ui->sendRequestButton->setEnabled(true);
}

void MainWindow::onSendRequestClicked()
{
    if (!isConnected || !ui->deviceListWidget->currentItem()) {
        return;
    }

    QString targetId = ui->deviceListWidget->currentItem()->text();

    ui->remoteStatusLabel->setText(QString("正在向 %1 发送连接请求...").arg(targetId));
    ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    // 发送请求时显示"请求中"
    ui->connectionTypeBadge->setProperty("status", "connecting");
    ui->connectionTypeBadge->setText("请求中");
    updateConnectionTypeBadge();

    ui->sendRequestButton->setEnabled(false);
    remoteConnectionTimer->start(REMOTE_CONNECTION_TIMEOUT);
    manager->sendRequestToTarget(this->webrtcModulesType,this->webrtcLevels,this->videoCodec,this->webrtcAudioEnable);

    statusBar()->showMessage("正在建立远程连接...");
}

void MainWindow::onDisconnectRemoteControl()
{
    if (remoteConnectionTimer->isActive()) {
        remoteConnectionTimer->stop();
    }

    bool isBeingControlled = (ui->disconnectRemoteButton->text() == "断开被控制");

    if (isBeingControlled) {
        qDebug() << "User (controlled side) initiated disconnect";
        if (manager) {
            manager->disConnectRemote();
        }

        // 远程连接断开后，重置连接类型标签为服务器连接状态
        if (isConnected) {
            ui->connectionTypeBadge->setProperty("status", "connecting");
            ui->connectionTypeBadge->setText("已连接");
        } else {
            ui->connectionTypeBadge->setProperty("status", "idle");
            ui->connectionTypeBadge->setText("未连接");
        }
        updateConnectionTypeBadge();

        ui->remoteStatusLabel->setText("已主动断开被控制");
        ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("success"));
        ui->disconnectRemoteButton->setText("断开远程连接");
        ui->sendRequestButton->setEnabled(ui->deviceListWidget->currentItem() != nullptr);
    } else {
        qDebug() << "User (controller side) initiated disconnect";
        if (manager) {
            manager->disConnectRemote();
        }

        // 远程连接断开后，重置连接类型标签为服务器连接状态
        if (isConnected) {
            ui->connectionTypeBadge->setProperty("status", "connecting");
            ui->connectionTypeBadge->setText("已连接");
        } else {
            ui->connectionTypeBadge->setProperty("status", "idle");
            ui->connectionTypeBadge->setText("未连接");
        }
        updateConnectionTypeBadge();

        ui->remoteStatusLabel->setText("已主动断开远程操控");
        ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("info"));
    }

    isRemoteConnected = false;
    ui->disconnectRemoteButton->setEnabled(false);

    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
        videoWidget->setWindowTitle("WebRTC远程桌面");
        ui->showVideoButton->setEnabled(false);
        ui->hideVideoButton->setEnabled(false);
    }

    statusBar()->showMessage("已断开远程连接");
}

void MainWindow::onRemoteControlStarted()
{
    qDebug() << "Remote control started - someone is controlling this machine";

    if (remoteConnectionTimer->isActive()) {
        remoteConnectionTimer->stop();
    }

    isRemoteConnected = true;
    ui->disconnectRemoteButton->setEnabled(true);
    ui->disconnectRemoteButton->setText("断开被控制");
    ui->sendRequestButton->setEnabled(false);

    // 注意：这里不需要更新连接类型标签
    // 被控制的状态不需要在连接类型标签中显示

    ui->remoteStatusLabel->setText("正在被远程控制中");
    ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    if (videoWidget) {
        videoWidget->setWindowTitle("WebRTC远程桌面 - 正在被控制");
    }

    statusBar()->showMessage("正在被远程控制");
}

void MainWindow::onRemoteDisconnectedByPeer()
{
    qDebug() << "Remote connection closed by peer";

    if (remoteConnectionTimer->isActive()) {
        remoteConnectionTimer->stop();
    }

    isRemoteConnected = false;
    ui->disconnectRemoteButton->setEnabled(false);
    ui->disconnectRemoteButton->setText("断开远程连接");
    ui->sendRequestButton->setEnabled(ui->deviceListWidget->currentItem() != nullptr);

    // 远程连接被对方断开后，重置连接类型标签为服务器连接状态
    if (isConnected) {
        ui->connectionTypeBadge->setProperty("status", "connecting");
        ui->connectionTypeBadge->setText("已连接");
    } else {
        ui->connectionTypeBadge->setProperty("status", "idle");
        ui->connectionTypeBadge->setText("未连接");
    }
    updateConnectionTypeBadge();

    ui->remoteStatusLabel->setText("远程连接已被对方断开");
    ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("info"));

    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
        videoWidget->setWindowTitle("WebRTC远程桌面");
        ui->showVideoButton->setEnabled(false);
        ui->hideVideoButton->setEnabled(false);
    }

    statusBar()->showMessage("远程连接已断开");
}

void MainWindow::onRemoteConnectionTimeout()
{
    qDebug() << "Remote connection timeout";

    ui->sendRequestButton->setEnabled(ui->deviceListWidget->currentItem() != nullptr);

    // 远程连接超时后，重置连接类型标签为服务器连接状态
    if (isConnected) {
        ui->connectionTypeBadge->setProperty("status", "connecting");
        ui->connectionTypeBadge->setText("已连接");
    } else {
        ui->connectionTypeBadge->setProperty("status", "idle");
        ui->connectionTypeBadge->setText("未连接");
    }
    updateConnectionTypeBadge();

    QString targetId = ui->deviceListWidget->currentItem() ?
                           ui->deviceListWidget->currentItem()->text() : "未知";
    ui->remoteStatusLabel->setText(QString("连接到 %1 超时：请求超时或对方未响应").arg(targetId));
    ui->remoteStatusLabel->setStyleSheet(createStatusLabelStyle("error"));

    statusBar()->showMessage("远程连接超时");
    showErrorMessage("连接超时", QString("连接到 %1 超时\n请求超时或对方未响应").arg(targetId));
}

void MainWindow::onDeviceItemClicked(QListWidgetItem *item)
{
    QString headerType = item->data(Qt::UserRole).toString();

    if (headerType == "my_devices_header" || headerType == "cloud_devices_header") {

        int headerIndex = ui->deviceListWidget->row(item);
        QString headerText = item->text();

        // 检查是展开还是折叠
        bool isExpanded = headerText.startsWith("▼");

        if (isExpanded) {
            // 当前是展开状态，要折叠
            item->setText(headerText.replace("▼", "▶"));
            int nextIndex = headerIndex + 1;
            while (nextIndex < ui->deviceListWidget->count()) {
                QListWidgetItem* item = ui->deviceListWidget->item(nextIndex);
                QString itemType = item->data(Qt::UserRole).toString();

                // 如果遇到下一个分组标题，停止
                if (!itemType.isEmpty()) {
                    break;
                }

                // 隐藏设备项
                item->setHidden(true);
                nextIndex++;
            }
        } else {
            // 当前是折叠状态，要展开
            item->setText(headerText.replace("▶", "▼"));
            int nextIndex = headerIndex + 1;
            while (nextIndex < ui->deviceListWidget->count()) {
                QListWidgetItem* item = ui->deviceListWidget->item(nextIndex);
                QString itemType = item->data(Qt::UserRole).toString();

                // 如果遇到下一个分组标题，停止
                if (!itemType.isEmpty()) {
                    break;
                }

                // 显示设备项
                item->setHidden(false);
                nextIndex++;
            }
        }
    }
}

void MainWindow::onShowVideoWindow()
{
    if (videoWidget && !videoWidget->isVisible()) {
        videoWidget->showNormal();
        videoWidget->raise();
        videoWidget->activateWindow();
    }
}

void MainWindow::onHideVideoWindow()
{
    if (videoWidget && videoWidget->isVisible()) {
        videoWidget->hide();
    }
}

void MainWindow::onFullScreen()
{
    if (!videoWidget) {
        return;
    }

    if (isFullScreen) {
        isFullScreen = false;
        ui->actionFullScreen->setText("视频窗口全屏(&F)");
        ui->actionFullScreen->setChecked(false);
        videoWidget->showMaximized();
        videoWidget->raise();
        videoWidget->activateWindow();
    } else {
        isFullScreen = true;
        ui->actionFullScreen->setText("退出全屏(&F)");
        ui->actionFullScreen->setChecked(true);
        videoWidget->showFullScreen();
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "关于",
                       "<h3>WebRTC-Native-Manager</h3>"
                       "<p>版本 1.0</p>"
                       "<p>基于WebRTC-Native和Qt开发的WebRTC远程桌面客户端</p>"
                       "<p>支持多账号管理和点对点视频传输</p>"
                       "<br>"
                       "<p>使用的技术：</p>"
                       "<ul>"
                       "<li>WebRTC-Native - WebRTC通信</li>"
                       "<li>Qt6 - 用户界面</li>"
                       "<li>Boost IO操作</li>"
                       "</ul>");
}

void MainWindow::onSettings()
{
    QMessageBox::information(this, "设置", "设置功能开发中...");
}

void MainWindow::onConnectionStateChanged(bool connected)
{
    isConnected = connected;
    updateConnectionState(connected);

    if (connected) {
        // 连接成功但还未确定连接类型
        // 注意：这里只在服务器连接成功时显示"已连接"
        ui->connectionTypeBadge->setProperty("status", "connecting");
        ui->connectionTypeBadge->setText("已连接");
        updateConnectionTypeBadge();
        statusBar()->showMessage("已连接到服务器");
        updateTargetList();  // 连接后从服务器获取设备列表
    } else {
        // 断开连接时重置为idle状态
        ui->connectionTypeBadge->setProperty("status", "idle");
        ui->connectionTypeBadge->setText("未连接");
        updateConnectionTypeBadge();
        statusBar()->showMessage("连接已断开");

        // 断开连接时重新初始化设备列表（显示未连接提示）
        initializeDeviceLists();

        if (videoWidget) {
            videoWidget->clearDisplay();
        }

        if (remoteConnectionTimer->isActive()) {
            remoteConnectionTimer->stop();
        }

        // 断开连接时禁用相关按钮
        ui->sendRequestButton->setEnabled(false);
        ui->disconnectRemoteButton->setEnabled(false);
        ui->showVideoButton->setEnabled(false);
        ui->hideVideoButton->setEnabled(false);
    }
}

void MainWindow::updateConnectionState(bool connected)
{
    isConnected = connected;

    ui->connectButton->setEnabled(!connected && !accountList.isEmpty() &&
                                  ui->accountComboBox->currentText() != "请添加账号");
    ui->connectButton->setText("连接服务器");
    ui->disconnectButton->setEnabled(connected);

    // 控制刷新按钮状态
    ui->refreshDeviceButton->setEnabled(connected);

    if (connected) {
        ui->connectionStatusLabel->setText(QString("已连接 (账号: %1)").arg(ui->accountComboBox->currentText()));
        ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("success"));
    } else {
        ui->connectionStatusLabel->setText("未连接");
        ui->connectionStatusLabel->setStyleSheet(createStatusLabelStyle("error"));
    }
}

void MainWindow::updateTargetList()
{
    if (!isConnected) {
        // 未连接时，清空并显示提示
        ui->deviceListWidget->clear();
        QListWidgetItem* hintItem = new QListWidgetItem("请先连接服务器");
        hintItem->setFlags(hintItem->flags() & ~Qt::ItemIsSelectable);
        hintItem->setForeground(Qt::gray);
        hintItem->setTextAlignment(Qt::AlignCenter);
        ui->deviceListWidget->addItem(hintItem);
        statusBar()->showMessage("未连接服务器，无法获取设备列表");
        return;
    }

    // 连接状态下，从服务器获取设备列表
    // 先清空现有列表
    ui->deviceListWidget->clear();

    // 添加我的设备分组
    QListWidgetItem* myDeviceHeader = new QListWidgetItem("▼ 我的设备");
    myDeviceHeader->setData(Qt::UserRole, "my_devices_header");
    myDeviceHeader->setFlags(myDeviceHeader->flags() & ~Qt::ItemIsSelectable);
    myDeviceHeader->setBackground(QColor(240, 245, 255));
    myDeviceHeader->setForeground(QColor(66, 153, 225));
    QFont headerFont = myDeviceHeader->font();
    headerFont.setBold(true);
    myDeviceHeader->setFont(headerFont);
    ui->deviceListWidget->addItem(myDeviceHeader);

    // 这里应该从服务器获取真实的设备列表
    // 暂时使用示例数据
    QStringList myDevices;
    myDevices << "913140924@qq.com" << "2044580040@qq.com" << "396887208@qq.com" << "147718387@qq.com";

    QStringList onlineDevices;
    onlineDevices << "913140924@qq.com" << "2044580040@qq.com" << "396887208@qq.com" << "147718387@qq.com"<< "cloud_device1@qq.com" << "cloud_device2@qq.com"; // 示例在线设备

    // 添加我的设备
    for (const QString& device : myDevices) {
        QListWidgetItem* item = new QListWidgetItem(device);
        if (onlineDevices.contains(device)) {
            item->setForeground(Qt::black);
            // 可以添加在线图标
        } else {
            item->setForeground(Qt::gray);
        }
        ui->deviceListWidget->addItem(item);
    }

    // 添加云设备分组
    QListWidgetItem* cloudDeviceHeader = new QListWidgetItem("▼ 云设备");
    cloudDeviceHeader->setData(Qt::UserRole, "cloud_devices_header");
    cloudDeviceHeader->setFlags(cloudDeviceHeader->flags() & ~Qt::ItemIsSelectable);
    cloudDeviceHeader->setBackground(QColor(240, 245, 255));
    cloudDeviceHeader->setForeground(QColor(66, 153, 225));
    cloudDeviceHeader->setFont(headerFont);
    ui->deviceListWidget->addItem(cloudDeviceHeader);

    // 添加云设备
    QStringList cloudDevices;
    cloudDevices << "cloud_device1@qq.com" << "cloud_device2@qq.com";

    for (const QString& device : cloudDevices) {
        QListWidgetItem* item = new QListWidgetItem(device);
        if (onlineDevices.contains(device)) {
            item->setForeground(Qt::black);
        } else {
            item->setForeground(Qt::gray);
        }
        ui->deviceListWidget->addItem(item);
    }

    statusBar()->showMessage("设备列表已从服务器获取");
}


void MainWindow::updateStatus()
{
    if (videoWidget) {
        double fps = videoWidget->getFrameRate();
        // 如果需要显示FPS，可以在这里添加到状态栏
        // statusBar()->showMessage(QString("FPS: %1").arg(fps, 0, 'f', 1));
    }
}

void MainWindow::onManagerError(const QString& error)
{
    lastError = error;
    statusBar()->showMessage(QString("错误: %1").arg(error));

    if (isConnected) {
        showErrorMessage("连接错误", error);
    }

    // 更新连接类型标签为错误状态
    ui->connectionTypeBadge->setProperty("status", "disconnected");
    ui->connectionTypeBadge->setText("连接错误");
    updateConnectionTypeBadge();

    updateConnectionState(false);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if(reallyExit){

        saveSettings();

        if (videoWidget) {

            videoWidget->close();

        }

        event->accept();

        return;
    }

    event->ignore();

    hide();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    // 只更新背景标签的大小，不重新加载图片
    if (background) {
        background->setGeometry(0, 0, width(), height());
    }
}

QString MainWindow::createStatusLabelStyle(const QString& type)
{
    if (type == "success") {
        return "QLabel { background: #48BB78; color: white; border: none; border-radius: 10px; padding: 5px 8px; font-size: 14px; font-weight: 600; }";
    } else if (type == "error") {
        return "QLabel { background: #F56565; color: white; border: none; border-radius: 10px; padding: 5px 8px; font-size: 14px; font-weight: 600; }";
    } else if (type == "warning") {
        return "QLabel { background: #ED8936; color: white; border: none; border-radius: 10px; padding: 5px 8px; font-size: 14px; font-weight: 600; }";
    } else if (type == "info") {
        return "QLabel { background: #4299E1; color: white; border: none; border-radius: 10px; padding: 5px 8px; font-size: 14px; font-weight: 600; }";
    } else {
        return "QLabel { background: #A0AEC0; color: white; border: none; border-radius: 10px; padding: 5px 8px; font-size: 14px; font-weight: 600; }";
    }
}

void MainWindow::showErrorMessage(const QString& title, const QString& message)
{
    QMessageBox::warning(this, title, message);
}
void MainWindow::onAddAccountClicked()
{
    bool ok;
    QString email = QInputDialog::getText(this, "添加账号",
                                          "请输入邮箱地址:",
                                          QLineEdit::Normal,
                                          "", &ok);

    if (ok && !email.isEmpty()) {
        // 简单的邮箱格式验证
        if (email.contains("@") && email.contains(".")) {
            if (!accountList.contains(email)) {
                accountList.append(email);

                // 更新ComboBox
                ui->accountComboBox->clear();
                ui->accountComboBox->addItems(accountList);
                ui->accountComboBox->setCurrentText(email);

                // 如果UI中有删除账号按钮，启用它
                // ui->removeAccountButton->setEnabled(true);
                ui->connectButton->setEnabled(!isConnected);

                saveAccounts();

                // 同时更新设备列表
                updateTargetList();
            } else {
                showErrorMessage("添加失败", "该账号已存在");
            }
        } else {
            showErrorMessage("格式错误", "请输入有效的邮箱地址");
        }
    }
}

void MainWindow::onRemoveAccountClicked()
{
    if (accountList.isEmpty()) {
        return;
    }

    QString currentAccount = ui->accountComboBox->currentText();
    if (currentAccount.isEmpty()) {
        return;
    }

    int ret = QMessageBox::question(this, "确认删除",
                                    QString("确定要删除账号 %1 吗？").arg(currentAccount),
                                    QMessageBox::Yes | QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        accountList.removeOne(currentAccount);

        // 更新ComboBox
        ui->accountComboBox->clear();
        if (!accountList.isEmpty()) {
            ui->accountComboBox->addItems(accountList);
        } else {
            ui->connectButton->setEnabled(false);
        }

        saveAccounts();

        // 更新设备列表
        updateTargetList();
    }
}

void MainWindow::on_modeComboBox_currentIndexChanged(int index)
{
    webrtcModulesType = index;
}

void MainWindow::on_codecComboBox_currentIndexChanged(int index)
{
    videoCodec = index;
}

void MainWindow::on_audioCheckBox_clicked(bool checked)
{
    webrtcAudioEnable = checked ? 1 : 0;
}

void MainWindow::on_accelerationComboBox_currentIndexChanged(int index)
{
    webrtcLevels = index;
}

    }

}
