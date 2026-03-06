#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "VideoWidget.h"
#include "WebRTCManager.h"
#include "ConfigManager.h"
#include <QApplication>
#include <QMessageBox>
#include <QDebug>
#include <QTimer>
#include <QMenu>
#include <QDateTime>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QClipboard>
#include <QPixmap>

namespace hope{
    namespace rtc{

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , videoWidget(nullptr)
    , manager(nullptr)
    , settings(nullptr)
    , isSignalConnected(false)
    , isRemoteConnected(false)
    , reConnectNums(0)
{

    ui->setupUi(this);

    settings = new QSettings("WebRTCmanager", "Settings", this);

    manager = new WebRTCManager(WebRTCRemoteState::nullRemote);

    // 2. 初始化
    initConfigAndSettings();
    setupUI();
    setupSignalSlots();

    // 3. 检查登录 (不使用 Timer，直接调用，因为对象已创建)
    checkLoginStatus();

    manager->onSignalServerConnectHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            this->onConnectionStateChanged(true);
        }, Qt::QueuedConnection);
    };

    manager->onSignalServerDisConnectHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if(isSignalConnected) this->onConnectionStateChanged(false);
            else QTimer::singleShot(3000, this, &MainWindow::startSignalServerConnection);
        }, Qt::QueuedConnection);
    };

    manager->onRemoteSuccessFulHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer) remoteConnectionTimer->stop();
            if (!videoWidget) {
                videoWidget = new VideoWidget();
                videoWidget->setWebRTCManager(manager);
                manager->setVideoFrameCallback([this](std::shared_ptr<VideoFrame> frame) {
                    if (videoWidget) videoWidget->displayFrame(frame);
                });
                connect(videoWidget, &VideoWidget::disConnectRemote, this, [this](){
                    isRemoteConnected = false;
                    ui->btnStartControl->setEnabled(true);
                    ui->btnStartControl->setText("立即连接");
                    ui->remoteStatusLabel->setText("远程连接已结束");
                    ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");
                    ui->networkTypeBadge->setVisible(false);
                    if(videoWidget) { videoWidget->hide(); videoWidget->deleteLater(); videoWidget = nullptr; }
                    this->showNormal(); this->activateWindow();

                    if(manager) manager->disConnectRemote();

                });
                connect(videoWidget, &QWidget::destroyed, this, [this](){ videoWidget = nullptr; });
            }
            videoWidget->showMaximized();
            videoWidget->raise();
            videoWidget->activateWindow();
            isRemoteConnected = true;
            ui->btnStartControl->setText("控制中...");
            ui->btnStartControl->setEnabled(false);
            ui->remoteStatusLabel->setText("🟢 远程连接已建立");
            ui->remoteStatusLabel->setStyleSheet("color: #10B981;");
            addToHistory(ui->remoteIdEdit->text());
        }, Qt::QueuedConnection);
    };

    manager->onRemoteFailedHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer) remoteConnectionTimer->stop();
            ui->btnStartControl->setEnabled(true);
            ui->btnStartControl->setText("立即连接");
            ui->remoteStatusLabel->setText("🔴 连接失败：对方不在线或拒绝");
            ui->remoteStatusLabel->setStyleSheet("color: #EF4444;");
        }, Qt::QueuedConnection);
    };

    manager->onRTCStatsCollectorHandle = [this](int type) {
        QMetaObject::invokeMethod(this, [this, type]() {
            updateNetworkTypeUI(type);
        }, Qt::QueuedConnection);
    };

    manager->onDisConnectRemoteHandle = [this]() { QMetaObject::invokeMethod(this, "onRemoteDisconnectedByPeer", Qt::QueuedConnection); };
    manager->onFollowRemoteHandle = [this]() { QMetaObject::invokeMethod(this, "onRemoteControlStarted", Qt::QueuedConnection); };
    manager->onResetCursorHandle = [this]() { QMetaObject::invokeMethod(this, [this](){
        #ifdef Q_OS_WIN
        SystemParametersInfo(SPI_SETCURSORS, 0, NULL, 0);
        #endif
    }, Qt::QueuedConnection); };
}

MainWindow::~MainWindow()
{
    if (settings) {
        settings->setValue("lastRemoteId", ui->remoteIdEdit->text());
        settings->setValue("webrtcModulesType", webrtcModulesType);
        settings->setValue("videoCodec", videoCodec);
        settings->setValue("webrtcLevels", webrtcLevels);
        settings->setValue("webrtcAudioEnable", webrtcAudioEnable);
    }
    if (manager) { manager->disConnect(); delete manager; }
    if (videoWidget) delete videoWidget;
    delete ui;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // 只有已登录才自动连接
    if (!isSignalConnected && !currentDeviceId.isEmpty()) {
        QTimer::singleShot(200, this, &MainWindow::startSignalServerConnection);
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == ui->userFrame && event->type() == QEvent::MouseButtonRelease) {
        onUserAvatarClicked();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::initConfigAndSettings()
{
    defaultServerHost = "121.5.37.53";
    std::string cfgHost = ConfigManager::Instance().GetString("WebRTCSignalServer.Host");
    if(!cfgHost.empty()) defaultServerHost = QString::fromStdString(cfgHost);
    defaultServerPort = ConfigManager::Instance().GetInt("WebRTCSignalServer.Port");
    if(defaultServerPort <= 0) defaultServerPort = 8088;

    ui->remoteIdEdit->setText(settings->value("lastRemoteId", "").toString());

    loadHistoryData();
    loadFavoritesData();

    webrtcModulesType = settings->value("webrtcModulesType", 0).toInt();
    videoCodec = settings->value("videoCodec", 4).toInt();
    webrtcLevels = settings->value("webrtcLevels", 2).toInt();
    webrtcAudioEnable = settings->value("webrtcAudioEnable", 0).toInt();

    ui->modeComboBox->setCurrentIndex(webrtcModulesType);
    ui->codecComboBox->setCurrentIndex(videoCodec);
    ui->accelerationComboBox->setCurrentIndex(webrtcLevels);
    ui->checkAudio->setChecked(webrtcAudioEnable == 1);
}

// ==========================================
// 核心修复区域：登录逻辑
// ==========================================

void MainWindow::checkLoginStatus() {
    // 1. 先尝试从本地加载
    currentDeviceId = settings->value("localDeviceId", "").toString();
    currentUserPwd = settings->value("localUserPwd", "").toString();
    currentUserName = settings->value("localUserName", "").toString();
    customAvatarPath = settings->value("customAvatarPath", "").toString();

    if (currentDeviceId.isEmpty()) {
        // 无账号，弹出登录框
        LoginDialog dlg(this, false); // 登录模式

        // exec() 阻塞直到关闭
        if (dlg.exec() == QDialog::Accepted) {
            // 获取数据
            currentDeviceId = dlg.accountEdit->text().trimmed();
            currentUserPwd = dlg.passwordEdit->text().trimmed();
            currentUserName = dlg.nickEdit->text().trimmed();
            if(currentUserName.isEmpty()) currentUserName = currentDeviceId;
            customAvatarPath = dlg.customAvatarPath;

            // 保存到本地
            settings->setValue("localDeviceId", currentDeviceId);
            settings->setValue("localUserPwd", currentUserPwd);
            settings->setValue("localUserName", currentUserName);
            settings->setValue("customAvatarPath", customAvatarPath);

            // 立即刷新UI (不要再读Settings了，直接用变量)
            updateLocalAccountUI();
        } else {
            // 用户点击了关闭，设为未登录状态
            currentDeviceId = "";
            currentUserName = "未登录";
            customAvatarPath = "";
            updateLocalAccountUI();
        }
    } else {
        // 已有账号，刷新UI
        updateLocalAccountUI();
    }
}

// 此函数只负责刷新 UI，不负责读 Settings，避免状态不同步
void MainWindow::updateLocalAccountUI() {
    if (currentDeviceId.isEmpty()) {
        ui->myDeviceCodeLabel->setText("--- --- ---");
        ui->userNameLabel->setText("未登录");
        ui->userStatusLabel->setText("● 离线 (点击登录)");
        ui->userStatusLabel->setStyleSheet("color: #6B7280;"); // 灰色

        // 默认头像
        ui->userAvatar->setPixmap(QPixmap());
        ui->userAvatar->setText("?");
        ui->userAvatar->setStyleSheet("background-color: #374151; border-radius: 19px; color: #9CA3AF; font-weight: bold; font-size: 16px;");
        return;
    }

    // 已登录状态
    QString displayCode = currentDeviceId;
    if(displayCode.length() > 6) displayCode.insert(4, " ");
    ui->myDeviceCodeLabel->setText(displayCode);

    ui->userNameLabel->setText(currentUserName);
    ui->userStatusLabel->setText("● 在线 (点击编辑)");
    ui->userStatusLabel->setStyleSheet("color: #10B981;"); // 绿色

    // 设置头像
    if(!customAvatarPath.isEmpty()) {
        QPixmap pixmap;
        QByteArray byteArray = QByteArray::fromBase64(customAvatarPath.toLatin1());
        pixmap.loadFromData(byteArray);

        if(!pixmap.isNull()) {
            QPixmap circular = createCircularAvatar(pixmap, 38);
            ui->userAvatar->setPixmap(circular);
            ui->userAvatar->setText("");
            ui->userAvatar->setStyleSheet("background-color: transparent; border: none;");
        } else {
            // 图片坏了，回退到文字
            ui->userAvatar->setPixmap(QPixmap());
            ui->userAvatar->setText(currentUserName.left(1).toUpper());
            ui->userAvatar->setStyleSheet("background-color: #337AFF; border-radius: 19px; color: white; font-weight: bold; font-size: 14px;");
        }
    } else {
        // 无图片，显示文字
        ui->userAvatar->setPixmap(QPixmap());
        ui->userAvatar->setText(currentUserName.left(1).toUpper());
        ui->userAvatar->setStyleSheet("background-color: #337AFF; border-radius: 19px; color: white; font-weight: bold; font-size: 14px;");
    }
}

// 点击头像 -> 登录或编辑
void MainWindow::onUserAvatarClicked() {
    bool isLoginMode = currentDeviceId.isEmpty();
    LoginDialog dlg(this, !isLoginMode); // true=编辑, false=登录

    if(!isLoginMode) {
        // 编辑模式：回填数据
        dlg.setValues(currentDeviceId, currentUserPwd, currentUserName, customAvatarPath);
    }

    connect(&dlg, &LoginDialog::logoutRequested, this, &MainWindow::onLogoutClicked);

    if (dlg.exec() == QDialog::Accepted) {
        if(isLoginMode) {
            currentDeviceId = dlg.accountEdit->text().trimmed();
        }

        currentUserPwd = dlg.passwordEdit->text().trimmed();
        currentUserName = dlg.nickEdit->text().trimmed();
        if(currentUserName.isEmpty()) currentUserName = currentDeviceId;
        customAvatarPath = dlg.customAvatarPath;

        // 保存
        settings->setValue("localDeviceId", currentDeviceId);
        settings->setValue("localUserPwd", currentUserPwd);
        settings->setValue("localUserName", currentUserName);
        settings->setValue("customAvatarPath", customAvatarPath);

        // 刷新
        updateLocalAccountUI();

        // 如果刚登录成功，尝试连接服务器
        if(isLoginMode && !isSignalConnected) {
            startSignalServerConnection();
        }
    }
}

// ==========================================

void MainWindow::setupUI()
{
    QIcon appIcon(":/logo/res/hope.jpg");
    setWindowIcon(appIcon);

    connect(ui->btnNavHome, &QPushButton::clicked, this, &MainWindow::onNavHomeClicked);
    connect(ui->btnNavDevices, &QPushButton::clicked, this, &MainWindow::onNavDevicesClicked);
    connect(ui->btnNavSettings, &QPushButton::clicked, this, &MainWindow::onNavSettingsClicked);

    ui->userFrame->installEventFilter(this);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayMenu = new QMenu(this);
        trayMenu->addAction("显示主界面", this, &MainWindow::showWindow);
        trayMenu->addSeparator();
        trayMenu->addAction("退出", this, &MainWindow::quitApplication);
        trayIcon = new QSystemTrayIcon(this);
        trayIcon->setIcon(appIcon);
        trayIcon->setContextMenu(trayMenu);
        trayIcon->show();
        connect(trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onSystemTrayActivated);
    }

    updateStatusUI("等待连接...", "normal");
    ui->btnStartControl->setEnabled(false);

    updateRecentListUI();
    ui->deviceGroupList->setCurrentRow(0);
    updateDeviceListUI(true);
}

// ... (loadHistoryData, loadFavoritesData, addToHistory 保持不变) ...
void MainWindow::loadHistoryData() {
    if(!settings) return;
    historyList.clear();
    QByteArray data = settings->value("historyList").toByteArray();
    if(!data.isEmpty()) {
        QJsonArray jsonArray = QJsonDocument::fromJson(data).array();
        for(const auto& val : jsonArray) historyList.append(DeviceInfo::fromJson(val.toObject()));
    }
}

void MainWindow::loadFavoritesData() {
    if(!settings) return;
    favoritesList.clear();
    QByteArray data = settings->value("favoritesList").toByteArray();
    if(!data.isEmpty()) {
        QJsonArray jsonArray = QJsonDocument::fromJson(data).array();
        for(const auto& val : jsonArray) favoritesList.append(DeviceInfo::fromJson(val.toObject()));
    }
}

void MainWindow::addToHistory(const QString& id, const QString& name) {
    if(!settings) return;
    bool found = false;
    for(int i=0; i<historyList.size(); ++i) {
        if(historyList[i].id == id) {
            historyList[i].lastAccess = QDateTime::currentMSecsSinceEpoch();
            historyList.move(i, 0);
            found = true;
            break;
        }
    }
    if (!found) {
        DeviceInfo info;
        info.id = id;
        info.name = name;
        for(const auto& fav : favoritesList) {
            if(fav.id == id) { info.name = fav.name; break; }
        }
        info.lastAccess = QDateTime::currentMSecsSinceEpoch();
        historyList.insert(0, info);
        if(historyList.size() > 20) historyList.removeLast();
    }

    QJsonArray array;
    for(const auto& dev : historyList) array.append(dev.toJson());
    settings->setValue("historyList", QJsonDocument(array).toJson());

    updateRecentListUI();
    if(ui->deviceGroupList->currentRow() == 1) updateDeviceListUI(false);
}

// ---------------- UI 更新 ----------------

void MainWindow::updateRecentListUI() {
    ui->recentListWidget->clear();
    if (historyList.isEmpty()) return;

    for(const auto& dev : historyList) {
        QString text = QString("%1\n%2").arg(dev.id).arg(dev.name);
        QListWidgetItem* item = new QListWidgetItem(QIcon(":/logo/res/hope.jpg"), text);
        item->setSizeHint(QSize(160, 60));
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, dev.id);
        ui->recentListWidget->addItem(item);
    }
}

void MainWindow::updateDeviceListUI(bool showFavorites) {
    ui->deviceDetailList->clear();
    const QList<DeviceInfo>& list = showFavorites ? favoritesList : historyList;

    for(const auto& dev : list) {
        QString text = QString("%1 - %2").arg(dev.name).arg(dev.id);
        QListWidgetItem* item = new QListWidgetItem(QIcon(":/logo/res/hope.jpg"), text);
        item->setData(Qt::UserRole, dev.id);
        QString dateStr = QDateTime::fromMSecsSinceEpoch(dev.lastAccess).toString("yyyy-MM-dd HH:mm");
        item->setToolTip(QString("ID: %1\n上次访问: %2").arg(dev.id).arg(dateStr));
        ui->deviceDetailList->addItem(item);
    }
}

void MainWindow::updateNetworkTypeUI(int type) {
    ui->networkTypeBadge->setVisible(true);
    ui->networkTypeBadge->style()->unpolish(ui->networkTypeBadge);

    if (type == 0) { // P2P
        ui->networkTypeBadge->setProperty("type", "p2p");
        ui->networkTypeBadge->setText("⚡ P2P直连");
    } else { // Relay
        ui->networkTypeBadge->setProperty("type", "relay");
        ui->networkTypeBadge->setText("🔄 中继转发");
    }
    ui->networkTypeBadge->style()->polish(ui->networkTypeBadge);
}

// ---------------- 交互 ----------------

void MainWindow::setupSignalSlots()
{
    connect(ui->btnStartControl, &QPushButton::clicked, this, &MainWindow::onBtnConnectClicked);
    connect(ui->btnCopyCode, &QPushButton::clicked, this, &MainWindow::onBtnCopyCodeClicked);
    connect(ui->recentListWidget, &QListWidget::itemClicked, this, &MainWindow::onDeviceItemClicked);
    connect(ui->btnClearHistory, &QPushButton::clicked, this, &MainWindow::onClearHistoryClicked);

    connect(ui->deviceGroupList, &QListWidget::currentItemChanged, this, &MainWindow::onDeviceGroupChanged);
    connect(ui->deviceDetailList, &QListWidget::itemClicked, this, &MainWindow::onDeviceItemClicked);

    connect(ui->btnAddDevice, &QPushButton::clicked, this, &MainWindow::onAddDeviceClicked);

    ui->deviceDetailList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->deviceDetailList, &QWidget::customContextMenuRequested, [this](const QPoint& pos){
        QListWidgetItem* item = ui->deviceDetailList->itemAt(pos);
        if(!item) return;

        QMenu menu(this);
        QAction* delAction = menu.addAction("删除此设备");
        connect(delAction, &QAction::triggered, [this, item](){
            QString id = item->data(Qt::UserRole).toString();
            bool isFav = (ui->deviceGroupList->currentRow() == 0);

            if(isFav) {
                for(int i=0; i<favoritesList.size(); ++i) {
                    if(favoritesList[i].id == id) { favoritesList.removeAt(i); break; }
                }
                QJsonArray array; for(const auto& d : favoritesList) array.append(d.toJson());
                settings->setValue("favoritesList", QJsonDocument(array).toJson());
                updateDeviceListUI(true);
            } else {
                for(int i=0; i<historyList.size(); ++i) {
                    if(historyList[i].id == id) { historyList.removeAt(i); break; }
                }
                QJsonArray array; for(const auto& d : historyList) array.append(d.toJson());
                settings->setValue("historyList", QJsonDocument(array).toJson());
                updateDeviceListUI(false);
                updateRecentListUI();
            }
        });
        menu.exec(ui->deviceDetailList->mapToGlobal(pos));
    });

    reconnectTimer = new QTimer(this);
    connect(reconnectTimer, &QTimer::timeout, this, &MainWindow::startSignalServerConnection);
    remoteConnectionTimer = new QTimer(this);
    remoteConnectionTimer->setSingleShot(true);
    connect(remoteConnectionTimer, &QTimer::timeout, this, &MainWindow::onRemoteConnectionTimeout);

    connect(ui->modeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onModeChanged);
    connect(ui->codecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onCodecChanged);
    connect(ui->accelerationComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onAccelerationChanged);
    connect(ui->checkAudio, &QCheckBox::clicked, this, &MainWindow::onAudioChecked);
    connect(ui->checkAutoStart, &QCheckBox::clicked, this, &MainWindow::onAutoStartChecked);
}

void MainWindow::onAddDeviceClicked() {
    // 检查是否登录
    if(currentDeviceId.isEmpty()) {
        QMessageBox::information(this, "提示", "请先登录");
        onUserAvatarClicked();
        return;
    }

    AddDeviceDialog dlg(this);
    if(dlg.exec() == QDialog::Accepted) {
        QString id = dlg.getID();
        QString name = dlg.getName();

        if(!id.isEmpty()) {
            DeviceInfo info;
            info.id = id;
            info.name = name.isEmpty() ? "我的设备" : name;
            info.lastAccess = QDateTime::currentMSecsSinceEpoch();

            favoritesList.append(info);

            QJsonArray array; for(const auto& d : favoritesList) array.append(d.toJson());
            settings->setValue("favoritesList", QJsonDocument(array).toJson());

            if(ui->deviceGroupList->currentRow() == 0) updateDeviceListUI(true);
        }
    }
}

void MainWindow::onClearHistoryClicked() {
    if(QMessageBox::question(this, "确认", "确定清空所有历史记录吗？") == QMessageBox::Yes) {
        historyList.clear();
        settings->remove("historyList");
        updateRecentListUI();
        if(ui->deviceGroupList->currentRow() == 1) updateDeviceListUI(false);
    }
}

void MainWindow::onDeviceGroupChanged(QListWidgetItem* current, QListWidgetItem* previous) {
    Q_UNUSED(previous);
    if(!current) return;
    int row = ui->deviceGroupList->row(current);
    updateDeviceListUI(row == 0);
    ui->labelDevTitle->setText(row == 0 ? "我的收藏" : "最近访问");
}

void MainWindow::onDeviceItemClicked(QListWidgetItem* item) {
    if(item) {
        QString id = item->data(Qt::UserRole).toString();
        if(id.isEmpty()) {
            QString text = item->text();
            QStringList parts = text.split("\n");
            id = (parts.size() > 1) ? parts[0] : text;
        }
        ui->remoteIdEdit->setText(id);
        ui->mainStackedWidget->setCurrentIndex(0);
        ui->btnNavHome->setChecked(true);
        ui->remoteIdEdit->setFocus();
    }
}

void MainWindow::onModeChanged(int index) { webrtcModulesType = index; }
void MainWindow::onCodecChanged(int index) { videoCodec = index; }
void MainWindow::onAccelerationChanged(int index) { webrtcLevels = index; }
void MainWindow::onAudioChecked(bool checked) { webrtcAudioEnable = checked ? 1 : 0; }
void MainWindow::onAutoStartChecked(bool checked) { Q_UNUSED(checked); }

void MainWindow::onNavHomeClicked() { ui->mainStackedWidget->setCurrentIndex(0); }
void MainWindow::onNavDevicesClicked() { ui->mainStackedWidget->setCurrentIndex(1); }
void MainWindow::onNavSettingsClicked() { ui->mainStackedWidget->setCurrentIndex(2); }

void MainWindow::startSignalServerConnection()
{
    if (isSignalConnected || currentDeviceId.isEmpty()) return;
    updateStatusUI("正在连接服务器...", "normal");
    manager->setAccountId(currentDeviceId.toStdString());
    QString url = QString("%1:%2").arg(defaultServerHost).arg(defaultServerPort);
    manager->connect(url.toStdString());
}

void MainWindow::onConnectionStateChanged(bool connected)
{
    isSignalConnected = connected;
    if (connected) {
        reConnectNums = 0;
        reconnectTimer->stop();
        updateStatusUI("● P2P网络就绪", "success");
        ui->btnStartControl->setEnabled(true);
    } else {
        updateStatusUI("● 网络断开", "error");
        ui->btnStartControl->setEnabled(false);
        if (!reallyExit && !currentDeviceId.isEmpty()) {
            updateStatusUI(QString("网络断开，重连中 (%1)...").arg(++reConnectNums), "error");
            reconnectTimer->start(5000);
        }
    }
}

void MainWindow::onBtnConnectClicked()
{
    if (ui->btnStartControl->text() == "断开连接") {

        isRemoteConnected = false;
        ui->btnStartControl->setEnabled(true);
        ui->btnStartControl->setText("立即连接");
        ui->remoteStatusLabel->setText("远程连接已结束");
        ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");
        ui->networkTypeBadge->setVisible(false);

        this->showNormal();
        this->activateWindow();

        if (manager) manager->disConnectRemote();
        return;
    }

    if (currentDeviceId.isEmpty()) {
        QMessageBox::information(this, "提示", "请先登录");
        onUserAvatarClicked();
        return;
    }

    QString targetId = ui->remoteIdEdit->text().trimmed();
    if (targetId.isEmpty()) {
        ui->remoteIdEdit->setFocus();
        return;
    }
    if (targetId == currentDeviceId) {
         QMessageBox::warning(this, "提示", "无法连接本机");
         return;
    }

    ui->remoteStatusLabel->setText("正在建立安全连接...");
    ui->remoteStatusLabel->setStyleSheet("color: #F59E0B;");
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("连接中...");

    manager->setTargetId(targetId.toStdString());
    remoteConnectionTimer->start(REMOTE_CONNECTION_TIMEOUT);
    manager->sendRequestToTarget(webrtcModulesType, webrtcLevels, videoCodec, webrtcAudioEnable);
}

void MainWindow::onBtnCopyCodeClicked()
{
    if(currentDeviceId.isEmpty()) return;
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(currentDeviceId);
    ui->btnCopyCode->setText("✅ 已复制");
    QTimer::singleShot(2000, [this](){ ui->btnCopyCode->setText("📋 复制"); });
}

void MainWindow::onRemoteControlStarted()
{
    if (remoteConnectionTimer->isActive()) remoteConnectionTimer->stop();
    isRemoteConnected = true;
    ui->btnStartControl->setText("断开连接");
    ui->btnStartControl->setEnabled(true);
    ui->remoteStatusLabel->setText("⚠️ 正在被远程控制中");
    ui->remoteStatusLabel->setStyleSheet("color: #EF4444; font-weight: bold;");
    if(!this->isMinimized()) this->showMinimized();
}

void MainWindow::onRemoteDisconnectedByPeer()
{
    isRemoteConnected = false;
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("立即连接");
    ui->remoteStatusLabel->setText("远程连接已结束");
    ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");
    ui->networkTypeBadge->setVisible(false);

    if (videoWidget) videoWidget->hide();
    this->showNormal();
    this->activateWindow();
}

void MainWindow::onRemoteConnectionTimeout()
{
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("立即连接");
    ui->remoteStatusLabel->setText("连接请求超时");
    ui->remoteStatusLabel->setStyleSheet("color: #EF4444;");
}

void MainWindow::updateStatusUI(const QString& status, const QString& styleClass)
{
    ui->connectionStatusLabel->setText(status);
    if(styleClass == "success") {
        ui->connectionStatusLabel->setStyleSheet("color: #10B981; font-size: 12px; background: rgba(16, 185, 129, 0.1); padding: 4px 8px; border-radius: 4px;");
    } else if (styleClass == "error") {
        ui->connectionStatusLabel->setStyleSheet("color: #EF4444; font-size: 12px; background: rgba(239, 68, 68, 0.1); padding: 4px 8px; border-radius: 4px;");
    } else {
        ui->connectionStatusLabel->setStyleSheet("color: #9CA3AF; font-size: 12px; background: rgba(156, 163, 175, 0.1); padding: 4px 8px; border-radius: 4px;");
    }
}

void MainWindow::showWindow() {
    showNormal();
    activateWindow();
}

void MainWindow::onLogoutClicked() {
    if (manager) manager->disConnect();
    isSignalConnected = false;
    isRemoteConnected = false;

    if (videoWidget) {
        videoWidget->hide();
        delete videoWidget;
        videoWidget = nullptr;
    }

    settings->remove("localDeviceId");
    settings->remove("localUserPwd");
    settings->remove("localUserName");
    settings->remove("customAvatarPath");

    currentDeviceId.clear();
    currentUserPwd.clear();
    currentUserName.clear();
    customAvatarPath.clear();

    updateLocalAccountUI();
    // 重新登录
    checkLoginStatus();
}

void MainWindow::quitApplication() {
    reallyExit = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (reallyExit) {
        if(videoWidget) delete videoWidget;
        if(manager) {
            delete manager;
            manager = nullptr;
        }
        event->accept();
    } else {
        event->ignore();
        hide();
    }
}

void MainWindow::onSystemTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) showWindow();
}

    } // namespace rtc
} // namespace hope
