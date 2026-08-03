#include "MainWindow.h"
#include "ui_mainwindow.h"
#include "VideoWidget.h"
#include "WebrtcManager.h"
#include "ConfigManager.h"
#include <QApplication>
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
#include <QTabWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QSurfaceFormat>

namespace hope{
    namespace rtc{

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , videoWidget(nullptr)
    , webrtcManager(nullptr)
    , settings(nullptr)
    , isSignalConnected(false)
    , isRemoteConnected(false)
    , reConnectNums(0)
{

    ui->setupUi(this);

    settings = new QSettings("WebrtcManager", "Settings", this);

    webrtcManager = std::make_shared<WebrtcManager>();

    webrtcManager->asyncEvent();

    // 操控端:解码状态 -> label(只显示解码;编码状态在被控端 Native 显示)
    webrtcManager->onCodecStatusHandle = [this](const std::string& codec, bool hardDecode) {
        QString codecQ = QString::fromStdString(codec).toUpper();
        QString text = QString("解码: %1 %2").arg(codecQ).arg(hardDecode ? "硬解" : "软解");
        QMetaObject::invokeMethod(this, [this, text]() {
            if (ui && ui->labelCodecStatus) ui->labelCodecStatus->setText(text);
        }, Qt::QueuedConnection);
    };

    // 被控端:本机 System 经本地 TCP(ENCODE_STATUS)上报当前编码 codec + 硬编/软编 -> label
    webrtcManager->onEncodeStatusHandle = [this](const std::string& codec, bool hardEncode) {
        QString codecQ = QString::fromStdString(codec).toUpper();
        QString text = QString("编码: %1 %2").arg(codecQ).arg(hardEncode ? "硬编" : "软编");
        QMetaObject::invokeMethod(this, [this, text]() {
            if (ui && ui->labelCodecStatus) ui->labelCodecStatus->setText(text);
        }, Qt::QueuedConnection);
    };

    // 2. 初始化
    initConfigAndSettings();
    setupUI();
    setupSignalSlots();

    // 3. 检查登录 (不使用 Timer，直接调用，因为对象已创建)
    checkLoginStatus();

    webrtcManager->onSignalServerConnectHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            this->onConnectionStateChanged(true);
        }, Qt::QueuedConnection);
    };

    webrtcManager->onSignalServerDisConnectHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if(isSignalConnected) this->onConnectionStateChanged(false);
            else QTimer::singleShot(3000, this, &MainWindow::startSignalServerConnection);
        }, Qt::QueuedConnection);
    };

    webrtcManager->onRemoteSuccessFulHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer) remoteConnectionTimer->stop();
            if (!videoWidget) {
                videoWidget = new VideoWidget();
                videoWidget->setWebrtcManager(webrtcManager);
                webrtcManager->setOnVideoFrameHanlder([this](std::shared_ptr<VideoFrame> frame) {
                    if (videoWidget) videoWidget->displayFrame(frame);
                });
                connect(videoWidget, &VideoWidget::disConnectRemote, this, [this](){
                    isRemoteConnected = false;
                    hideNetworkBadge();
                    ui->btnStartControl->setEnabled(true);
                    ui->btnStartControl->setText("立即连接");
                    ui->btnSendCtrlAltF->setEnabled(false);
                    ui->remoteStatusLabel->setText("远程连接已结束");
                    ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");
                    if (ui->labelCodecStatus) ui->labelCodecStatus->setText("");
                    stopFpsDisplay();
                    if(videoWidget) { videoWidget->hide(); disconnect(videoWidget, nullptr, this, nullptr); delete videoWidget; videoWidget = nullptr; }

                    if(webrtcManager) webrtcManager->disConnectRemote();

                });
                connect(videoWidget, &QWidget::destroyed, this, [this](){ videoWidget = nullptr; });
            }
            // 按 verticalSyncEnabled 设置 defaultFormat,使 videoWidget 的 backingstore
            // swapchain 在 show 创建窗口时按此 swapInterval 创建(每次会话生效,无需重启)
            applyVSyncToFormat();
            videoWidget->showMaximized();
            videoWidget->raise();
            videoWidget->activateWindow();
            isRemoteConnected = true;
            ui->btnStartControl->setText("控制中...");
            ui->btnStartControl->setEnabled(false);
            ui->btnSendCtrlAltF->setEnabled(true);
            ui->remoteStatusLabel->setText("🟢 远程连接已建立");
            ui->remoteStatusLabel->setStyleSheet("color: #10B981;");
            addToHistory(ui->remoteIdEdit->text());

            // 开始周期刷新 RTT,并立即拉一次(仅在设置开启时)
            lastRttMs = -1.0;
            if (showRttEnabled) {
                if (statsTimer) statsTimer->start();
                if (webrtcManager) webrtcManager->requestStats();
            }
            // 渲染帧率显示(仅在设置开启时):开启统计 + 启动刷新定时器
            if (showFpsEnabled) {
                startFpsDisplay();
            }
        }, Qt::QueuedConnection);
    };

    webrtcManager->onRemoteFailedHandle = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (remoteConnectionTimer) remoteConnectionTimer->stop();
            ui->btnStartControl->setEnabled(true);
            ui->btnStartControl->setText("立即连接");
            ui->remoteStatusLabel->setText("🔴 连接失败：对方不在线或拒绝");
            ui->remoteStatusLabel->setStyleSheet("color: #EF4444;");
        }, Qt::QueuedConnection);
    };

    webrtcManager->onRTCStatsCollectorHandle = [this](int type, double rttMs) {
        QMetaObject::invokeMethod(this, [this, type, rttMs]() {
            updateNetworkTypeUI(type, rttMs);
        }, Qt::QueuedConnection);
    };

    webrtcManager->onDisConnectRemoteHandle = [this]() { QMetaObject::invokeMethod(this, "onRemoteDisconnectedByPeer", Qt::QueuedConnection); };
    webrtcManager->onFollowRemoteHandle = [this]() { QMetaObject::invokeMethod(this, "onRemoteControlStarted", Qt::QueuedConnection); };
    webrtcManager->onResetCursorHandle = [this]() { QMetaObject::invokeMethod(this, [this](){
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
        settings->setValue("showRtt", showRttEnabled);
        settings->setValue("showFps", showFpsEnabled);
        settings->setValue("webrtcEnableNvenc", webrtcEnableNvenc);
        settings->setValue("webrtcEnableNvdec", webrtcEnableNvdec);
        saveEncodeConfig();
    }
    if (webrtcManager) {
        webrtcManager->disConnect();
        webrtcManager->closeEvent();
    }
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
    std::string signalServerHost = ConfigManager::Instance().GetString("WebrtcSignalServer.Host");
    if(!signalServerHost.empty()) defaultServerHost = QString::fromStdString(signalServerHost);
    defaultServerPort = ConfigManager::Instance().GetInt("WebrtcSignalServer.Port");
    if(defaultServerPort <= 0) defaultServerPort = 8088;

    // 读取 WebrtcManager 启动期配置并注入(WebrtcManager 自身不再读 config.ini)
    WebrtcManagerConfig webrtcManagerConfig;
    webrtcManagerConfig.systemService    = ConfigManager::Instance().GetString("Webrtc.SystemService", "HopeDeskSystem");
    webrtcManagerConfig.systemServiceExe = ConfigManager::Instance().GetString("Webrtc.SystemServiceExe");
    webrtcManagerConfig.stunHost          = ConfigManager::Instance().GetString("Stun.Host");
    webrtcManagerConfig.turnHost          = ConfigManager::Instance().GetString("Turn.Host");
    webrtcManagerConfig.turnUsername      = ConfigManager::Instance().GetString("Turn.Username");
    webrtcManagerConfig.turnPassword      = ConfigManager::Instance().GetString("Turn.Password");
    if (webrtcManager) webrtcManager->setWebrtcManagerConfig(webrtcManagerConfig);

    ui->remoteIdEdit->setText(settings->value("lastRemoteId", "").toString());

    loadHistoryData();
    loadFavoritesData();

    webrtcModulesType = settings->value("webrtcModulesType", 0).toInt();
    videoCodec = settings->value("videoCodec", 4).toInt();
    webrtcLevels = settings->value("webrtcLevels", 2).toInt();
    webrtcAudioEnable = settings->value("webrtcAudioEnable", 0).toInt();
    // 拆分后的硬编/硬解开关;新键不存在时从旧 webrtcEnableNvidia 迁移
    webrtcEnableNvenc = settings->value("webrtcEnableNvenc",
        settings->value("webrtcEnableNvidia", 0).toInt()).toInt();
    webrtcEnableNvdec = settings->value("webrtcEnableNvdec",
        settings->value("webrtcEnableNvidia", 0).toInt()).toInt();

    ui->modeComboBox->setCurrentIndex(webrtcModulesType);
    ui->codecComboBox->setCurrentIndex(videoCodec);
    ui->accelerationComboBox->setCurrentIndex(webrtcLevels);
    ui->checkAudio->setChecked(webrtcAudioEnable == 1);
    ui->checkHwAccel->setChecked(webrtcEnableNvenc == 1);
    ui->checkHwDec->setChecked(webrtcEnableNvdec == 1);

    showRttEnabled = settings->value("showRtt", false).toBool();
    ui->checkShowRtt->setChecked(showRttEnabled);
    showFpsEnabled = settings->value("showFps", false).toBool();
    ui->checkShowFps->setChecked(showFpsEnabled);
    verticalSyncEnabled = ConfigManager::Instance().GetBool("Render.VSync", false);

    // 编码配置:UI 存 Mbps,WebrtcDeskConfig 用 bps(读取时不转换,填入 config 时再 *1000000)
    requestMaxBitrateMbps = settings->value("requestMaxBitrateMbps", 15).toInt();
    requestMinBitrateMbps = settings->value("requestMinBitrateMbps", 15).toInt();
    requestMaxFramerate   = settings->value("requestMaxFramerate", 144).toInt();
    localMaxBitrateMbps   = settings->value("localMaxBitrateMbps", 15).toInt();
    localMinBitrateMbps   = settings->value("localMinBitrateMbps", 15).toInt();
    localMaxFramerate     = settings->value("localMaxFramerate", 144).toInt();
    // 兜底:历史配置可能 min>max,强制拉回 min<=max
    if (requestMinBitrateMbps > requestMaxBitrateMbps) requestMinBitrateMbps = requestMaxBitrateMbps;
    if (localMinBitrateMbps > localMaxBitrateMbps) localMinBitrateMbps = localMaxBitrateMbps;
    ui->spinRequestMaxBitrate->setValue(requestMaxBitrateMbps);
    ui->spinRequestMinBitrate->setValue(requestMinBitrateMbps);
    ui->spinRequestFramerate->setValue(requestMaxFramerate);
    ui->spinLocalMaxBitrate->setValue(localMaxBitrateMbps);
    ui->spinLocalMinBitrate->setValue(localMinBitrateMbps);
    ui->spinLocalFramerate->setValue(localMaxFramerate);

    syncConfigToManager();  // 启动即把已加载配置同步给 manager
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

            moveToCenter();
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

        moveToCenter();
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

        moveToCenter();
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

    // 设置页改为 tab 结构:「通用设置」(原内容) + 「系统设置」
    buildSystemSettingsTab();
    loadSystemSettings();
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

void MainWindow::updateNetworkTypeUI(int type, double rttMs) {
    lastConnectionType = type;
    // 服务器 STATS 路径不携带 RTT(传 -1),沿用上次值,避免把显示刷掉
    if (rttMs >= 0) lastRttMs = rttMs;

    ui->networkTypeBadge->setVisible(true);
    ui->networkTypeBadge->style()->unpolish(ui->networkTypeBadge);

    QString baseText;
    if (type == 0) { // P2P
        ui->networkTypeBadge->setProperty("type", "p2p");
        baseText = "⚡ P2P直连";
    } else { // Relay
        ui->networkTypeBadge->setProperty("type", "relay");
        baseText = "🔄 中继转发";
    }

    // 仅在开启 RTT 显示时拼接延迟数字
    if (showRttEnabled) {
        if (lastRttMs >= 0) {
            baseText += QString(" · %1 ms").arg(static_cast<int>(lastRttMs + 0.5));
        } else {
            baseText += " · -- ms";
        }
    }
    ui->networkTypeBadge->setText(baseText);
    ui->networkTypeBadge->style()->polish(ui->networkTypeBadge);
}

void MainWindow::refreshNetworkBadge()
{
    // 用缓存的类型与 RTT 重绘:用于设置开关切换后立即生效(不触发新的统计请求)
    updateNetworkTypeUI(lastConnectionType, -1.0);
}

void MainWindow::hideNetworkBadge()
{
    // 操控端与被控端共用同一个徽章:断开时必须清掉 RTT 缓存,
    // 否则下一次会话(或换角色)会带上一次的残留 RTT 显示
    if (statsTimer) statsTimer->stop();
    lastRttMs = -1.0;
    lastConnectionType = 0;
    ui->networkTypeBadge->setVisible(false);
    ui->networkTypeBadge->setText("");
}

void MainWindow::saveEncodeConfig()
{
    if (!settings) return;
    settings->setValue("requestMaxBitrateMbps", requestMaxBitrateMbps);
    settings->setValue("requestMinBitrateMbps", requestMinBitrateMbps);
    settings->setValue("requestMaxFramerate",   requestMaxFramerate);
    settings->setValue("localMaxBitrateMbps",   localMaxBitrateMbps);
    settings->setValue("localMinBitrateMbps",   localMinBitrateMbps);
    settings->setValue("localMaxFramerate",     localMaxFramerate);
    syncConfigToManager();  // 编码改动也同步到 manager
}

void MainWindow::syncConfigToManager()
{
    if (!webrtcManager) return;
    // 码率 Mbps->bps,并兜底 min<=max
    int reqMaxBps = requestMaxBitrateMbps * 1000000;
    int reqMinBps = requestMinBitrateMbps * 1000000;
    if (reqMinBps > reqMaxBps) reqMinBps = reqMaxBps;
    int locMaxBps = localMaxBitrateMbps * 1000000;
    int locMinBps = localMinBitrateMbps * 1000000;
    if (locMinBps > locMaxBps) locMinBps = locMaxBps;
    WebrtcDeskConfig cfg{
        webrtcModulesType, webrtcLevels, videoCodec,
        webrtcAudioEnable, webrtcEnableNvenc, webrtcEnableNvdec,
        reqMaxBps, reqMinBps, requestMaxFramerate,
        locMaxBps, locMinBps, localMaxFramerate,
        desktopWidth, desktopHeight, desktopRefreshRate
    };
    webrtcManager->setWebrtcDeskConfig(cfg);
}

void MainWindow::moveToCenter()
{
    move(QGuiApplication::primaryScreen()->availableGeometry().center() - rect().center());
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

    // 周期性拉取 WebRTC 统计以刷新网络 RTT 显示
    statsTimer = new QTimer(this);
    statsTimer->setInterval(3000);
    connect(statsTimer, &QTimer::timeout, this, [this]() {
        if (webrtcManager && isRemoteConnected) webrtcManager->requestStats();
    });

    connect(ui->modeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onModeChanged);
    connect(ui->codecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onCodecChanged);
    connect(ui->accelerationComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onAccelerationChanged);
    connect(ui->checkAudio, &QCheckBox::clicked, this, &MainWindow::onAudioChecked);
    connect(ui->btnSendCtrlAltF, &QPushButton::clicked, this, [this]() {
        if (webrtcManager && isRemoteConnected) webrtcManager->sendKeyComboCtrlAltF();
    });
    connect(ui->checkAutoStart, &QCheckBox::clicked, this, &MainWindow::onAutoStartChecked);
    connect(ui->checkShowRtt, &QCheckBox::clicked, this, &MainWindow::onShowRttChecked);
    connect(ui->checkShowFps, &QCheckBox::clicked, this, &MainWindow::onShowFpsChecked);

    // 周期拉取 VideoWidget 实际渲染帧率刷新 labelFps(仅显示,不参与控制)
    fpsDisplayTimer = new QTimer(this);
    fpsDisplayTimer->setInterval(500);
    connect(fpsDisplayTimer, &QTimer::timeout, this, [this]() {
        if (!videoWidget || !isRemoteConnected) return;
        double fps = videoWidget->getFrameRate();
        if (ui->labelFps) ui->labelFps->setText(QString("渲染: %1 FPS").arg(fps, 0, 'f', 0));
    });

    // 编码配置:spinbox 改动即更新成员并落盘(QSettings)。码率 UI 为 Mbps。
    // 单值绑定(帧率,无 min/max 关系)
    auto bindEncodeSpin = [this](QSpinBox* spin, int* member) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, member](int v) {
            *member = v;
            saveEncodeConfig();
        });
    };
    // 码率对绑定:保证 min<=max,改最小>最大时把最大顶上来,改最大<最小时把最小压下去
    auto bindBitratePair = [this](QSpinBox* minSpin, QSpinBox* maxSpin, int* minMember, int* maxMember) {
        connect(minSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, maxSpin, minMember, maxMember](int v) {
                *minMember = v;
                if (*maxMember < v) {
                    *maxMember = v;
                    QSignalBlocker b(maxSpin);  // 阻断回弹,防止递归
                    maxSpin->setValue(v);
                }
                saveEncodeConfig();
            });
        connect(maxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, minSpin, minMember, maxMember](int v) {
                *maxMember = v;
                if (*minMember > v) {
                    *minMember = v;
                    QSignalBlocker b(minSpin);
                    minSpin->setValue(v);
                }
                saveEncodeConfig();
            });
    };
    bindBitratePair(ui->spinRequestMinBitrate, ui->spinRequestMaxBitrate,
                    &requestMinBitrateMbps, &requestMaxBitrateMbps);
    bindBitratePair(ui->spinLocalMinBitrate,   ui->spinLocalMaxBitrate,
                    &localMinBitrateMbps,   &localMaxBitrateMbps);
    bindEncodeSpin(ui->spinRequestFramerate, &requestMaxFramerate);
    bindEncodeSpin(ui->spinLocalFramerate,   &localMaxFramerate);
}

void MainWindow::onAddDeviceClicked() {
    // 检查是否登录
    if(currentDeviceId.isEmpty()) {
        ConfirmDialog(tr("提示"), tr("请先登录"), tr("知道了"), QString(), this).exec();
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
    if(ConfirmDialog(tr("确认"), tr("确定清空所有历史记录吗？"), tr("清空"), tr("取消"), this).exec() == QDialog::Accepted) {
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

void MainWindow::onModeChanged(int index) {
    webrtcModulesType = index;
    if (settings) settings->setValue("webrtcModulesType", webrtcModulesType);
    syncConfigToManager();
}
void MainWindow::onCodecChanged(int index) {
    videoCodec = index;
    if (settings) settings->setValue("videoCodec", videoCodec);
    syncConfigToManager();
}
void MainWindow::onAccelerationChanged(int index) {
    webrtcLevels = index;
    if (settings) settings->setValue("webrtcLevels", webrtcLevels);
    syncConfigToManager();
}
void MainWindow::onAudioChecked(bool checked) {
    webrtcAudioEnable = checked ? 1 : 0;
    if (settings) settings->setValue("webrtcAudioEnable", webrtcAudioEnable);
    syncConfigToManager();
}
void MainWindow::onAutoStartChecked(bool checked) { Q_UNUSED(checked); }

void MainWindow::onShowRttChecked(bool checked)
{
    showRttEnabled = checked;
    if (settings) settings->setValue("showRtt", showRttEnabled);

    // 立即生效:已连接时按需启停 RTT 轮询
    if (showRttEnabled) {
        if (isRemoteConnected && statsTimer) {
            statsTimer->start();
            if (webrtcManager) webrtcManager->requestStats();
        }
    } else {
        if (statsTimer) statsTimer->stop();
    }

    // 重绘徽章:去掉或补上 RTT
    if (ui->networkTypeBadge->isVisible()) {
        refreshNetworkBadge();
    }
}

void MainWindow::onShowFpsChecked(bool checked)
{
    showFpsEnabled = checked;
    if (settings) settings->setValue("showFps", showFpsEnabled);

    // 立即生效:已连接时按需启停帧率统计与显示
    if (showFpsEnabled) {
        startFpsDisplay();
    } else {
        stopFpsDisplay();
    }
}

void MainWindow::startFpsDisplay()
{
    if (!isRemoteConnected || !videoWidget) return;
    videoWidget->setFpsEnabled(true);
    if (fpsDisplayTimer) fpsDisplayTimer->start();
}

void MainWindow::stopFpsDisplay()
{
    if (fpsDisplayTimer) fpsDisplayTimer->stop();
    if (videoWidget) videoWidget->setFpsEnabled(false);
    if (ui->labelFps) ui->labelFps->setText("");
}

// ===== 系统设置 tab =====

void MainWindow::buildSystemSettingsTab()
{
    settingsTabWidget = new QTabWidget(ui->pageSettings);
    // documentMode 去掉默认厚重边框;显式继承设置页字体,避免 QTabWidget 自带字体影响内容
    settingsTabWidget->setDocumentMode(true);
    settingsTabWidget->setFont(ui->pageSettings->font());
    settingsTabWidget->setStyleSheet(R"(
        QTabWidget::pane {
            border: none;
            background: transparent;
        }
        QTabBar {
            background: transparent;
        }
        QTabBar::tab {
            background: transparent;
            color: #8C9AA8;
            padding: 8px 22px;
            font-size: 14px;
            border: none;
            border-bottom: 2px solid transparent;
            margin-right: 4px;
        }
        QTabBar::tab:selected {
            color: #0072FF;
            border-bottom: 2px solid #0072FF;
            font-weight: bold;
        }
        QTabBar::tab:hover:!selected {
            color: #338CFF;
        }
    )");

    // 「通用设置」tab:把现有滚动区整体移入(addTab 会自动 reparent,并从原布局移除)
    settingsTabWidget->addTab(ui->scrollAreaSettings, tr("通用设置"));

    // 「系统设置」tab
    QWidget* systemTab = new QWidget(settingsTabWidget);
    systemTab->setStyleSheet(R"(
        QLabel { color: #5A6C7D; font-size: 14px; }
        QLineEdit, QSpinBox {
            background-color: #F5F7FA;
            border: 1px solid #D6E3F0;
            border-radius: 8px;
            padding: 8px 10px;
            color: #333333;
            font-size: 14px;
        }
        QLineEdit:focus, QSpinBox:focus {
            border: 1px solid #0072FF;
            background-color: #FFFFFF;
        }
        QLineEdit:hover, QSpinBox:hover {
            background-color: #FFFFFF;
        }
        QSpinBox::up-button, QSpinBox::down-button {
            width: 20px;
            border: none;
            border-left: 1px solid #E1E8ED;
            background: transparent;
        }
        QSpinBox::up-button { border-top-right-radius: 8px; subcontrol-position: top right; }
        QSpinBox::down-button { border-bottom-right-radius: 8px; subcontrol-position: bottom right; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover {
            background: #E6F8FF;
        }
        QSpinBox::up-arrow {
            image: url(:/icons/res/arrow-up.png);
            width: 12px; height: 12px;
        }
        QSpinBox::down-arrow {
            image: url(:/icons/res/arrow-down.png);
            width: 12px; height: 12px;
        }
        QCheckBox {
            color: #5A6C7D;
            font-size: 14px;
            spacing: 8px;
        }
        QPushButton#browseButton {
            background-color: #FFFFFF;
            color: #0072FF;
            border: 1px solid #D6E3F0;
            border-radius: 8px;
            padding: 8px 14px;
            font-size: 14px;
        }
        QPushButton#browseButton:hover {
            border-color: #0072FF;
            background-color: rgba(0, 114, 255, 0.05);
        }
    )");
    QVBoxLayout* systemLayout = new QVBoxLayout(systemTab);
    systemLayout->setSpacing(12);
    systemLayout->setContentsMargins(20, 20, 20, 20);

    QFormLayout* systemFormLayout = new QFormLayout();
    systemFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    systemFormLayout->setSpacing(10);

    verticalSyncCheckBox = new QCheckBox(tr("启用"), systemTab);
    systemFormLayout->addRow(tr("垂直同步"), verticalSyncCheckBox);

    signalServerHostEdit = new QLineEdit(systemTab);
    signalServerPortSpin = new QSpinBox(systemTab);
    signalServerPortSpin->setRange(1, 65535);
    QHBoxLayout* signalServerLayout = new QHBoxLayout();
    signalServerLayout->setSpacing(8);
    signalServerLayout->addWidget(signalServerHostEdit, 3);
    signalServerLayout->addWidget(new QLabel(tr("端口"), systemTab), 0);
    signalServerLayout->addWidget(signalServerPortSpin, 1);
    systemFormLayout->addRow(tr("信号服务器"), signalServerLayout);

    stunHostEdit = new QLineEdit(systemTab);
    systemFormLayout->addRow(tr("STUN 服务器"), stunHostEdit);

    turnHostEdit = new QLineEdit(systemTab);
    systemFormLayout->addRow(tr("TURN 服务器"), turnHostEdit);
    turnUsernameEdit = new QLineEdit(systemTab);
    systemFormLayout->addRow(tr("TURN 用户名"), turnUsernameEdit);
    turnPasswordEdit = new QLineEdit(systemTab);
    turnPasswordEdit->setEchoMode(QLineEdit::Password);
    systemFormLayout->addRow(tr("TURN 密码"), turnPasswordEdit);

    webrtcServiceExeEdit = new QLineEdit(systemTab);
    QPushButton* browseButton = new QPushButton(tr("浏览..."), systemTab);
    browseButton->setObjectName("browseButton");
    browseButton->setCursor(Qt::PointingHandCursor);
    connect(browseButton, &QPushButton::clicked, this, &MainWindow::onWebrtcServiceExeBrowse);
    QHBoxLayout* webrtcServiceExeLayout = new QHBoxLayout();
    webrtcServiceExeLayout->setSpacing(8);
    webrtcServiceExeLayout->addWidget(webrtcServiceExeEdit, 3);
    webrtcServiceExeLayout->addWidget(browseButton, 0);
    systemFormLayout->addRow(tr("WebRTC 系统程序"), webrtcServiceExeLayout);

    // 服务名独立设置(不自动从 exe 推导,避免与已安装的同名服务冲突)
    webrtcServiceNameEdit = new QLineEdit(systemTab);
    webrtcServiceNameEdit->setPlaceholderText(tr("如 HopeDeskSystem"));
    systemFormLayout->addRow(tr("WebRTC 服务名"), webrtcServiceNameEdit);

    // ===== 虚拟显示器设置(驱动名称写死,只能看不能改) =====
    labelVddNameValue = new QLabel(tr("Hope Vitrual Display"), systemTab);
    labelVddNameValue->setStyleSheet("font-weight: bold; color: #0072FF;");
    systemFormLayout->addRow(tr("虚拟显示器"), labelVddNameValue);

    spinDesktopWidth = new QSpinBox(systemTab);
    spinDesktopWidth->setRange(640, 7680);
    spinDesktopWidth->setSingleStep(160);
    spinDesktopWidth->setSuffix(tr(" px"));
    systemFormLayout->addRow(tr("宽度"), spinDesktopWidth);

    spinDesktopHeight = new QSpinBox(systemTab);
    spinDesktopHeight->setRange(360, 4320);
    spinDesktopHeight->setSingleStep(90);
    spinDesktopHeight->setSuffix(tr(" px"));
    systemFormLayout->addRow(tr("高度"), spinDesktopHeight);

    spinDesktopRefreshRate = new QSpinBox(systemTab);
    spinDesktopRefreshRate->setRange(24, 240);
    spinDesktopRefreshRate->setSuffix(tr(" Hz"));
    systemFormLayout->addRow(tr("刷新率"), spinDesktopRefreshRate);

    systemLayout->addLayout(systemFormLayout);

    QPushButton* applyButton = new QPushButton(tr("应用系统设置"), systemTab);
    applyButton->setObjectName("applyButton");
    applyButton->setCursor(Qt::PointingHandCursor);
    applyButton->setStyleSheet(
        "QPushButton#applyButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #0072FF, stop:1 #00B4FF); color: white; border: none; border-radius: 8px; "
        "padding: 10px 22px; font-weight: bold; font-size: 14px; }"
        "QPushButton#applyButton:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #338CFF, stop:1 #33C3FF); }"
        "QPushButton#applyButton:pressed { background: #0056CC; }");
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::onApplySystemSettings);
    systemLayout->addWidget(applyButton, 0, Qt::AlignLeft);
    systemLayout->addStretch();

    settingsTabWidget->addTab(systemTab, tr("系统设置"));

    // 把 tab 控件放进设置页原布局(scrollAreaSettings 已被 reparent 走)
    ui->verticalLayout_Settings->addWidget(settingsTabWidget);
}

void MainWindow::loadSystemSettings()
{
    ConfigManager& configManager = ConfigManager::Instance();
    verticalSyncCheckBox->setChecked(configManager.GetBool("Render.VSync", false));
    signalServerHostEdit->setText(QString::fromStdString(configManager.GetString("WebrtcSignalServer.Host")));
    int signalServerPort = configManager.GetInt("WebrtcSignalServer.Port", 8088);
    signalServerPortSpin->setValue(signalServerPort > 0 ? signalServerPort : 8088);
    stunHostEdit->setText(QString::fromStdString(configManager.GetString("Stun.Host")));
    turnHostEdit->setText(QString::fromStdString(configManager.GetString("Turn.Host")));
    turnUsernameEdit->setText(QString::fromStdString(configManager.GetString("Turn.Username")));
    turnPasswordEdit->setText(QString::fromStdString(configManager.GetString("Turn.Password")));
    webrtcServiceExeEdit->setText(QString::fromStdString(configManager.GetString("Webrtc.SystemServiceExe")));
    webrtcServiceNameEdit->setText(QString::fromStdString(configManager.GetString("Webrtc.SystemService", "HopeDeskSystem")));

    // 虚拟显示器设置(驱动名称写死:Hope Vitrual Display,不可改)
    spinDesktopWidth->setValue(configManager.GetInt("VitrualDisplay.DesktopWidth", 1920));
    spinDesktopHeight->setValue(configManager.GetInt("VitrualDisplay.DesktopHeight", 1080));
    spinDesktopRefreshRate->setValue(configManager.GetInt("VitrualDisplay.DesktopRefreshRate", 144));
    desktopWidth = spinDesktopWidth->value();
    desktopHeight = spinDesktopHeight->value();
    desktopRefreshRate = spinDesktopRefreshRate->value();
}

void MainWindow::applyVSyncToFormat()
{
    // 写入 defaultFormat:之后新建的窗口(QWindow 构造时读取 defaultFormat)
    // 即 videoWidget 下一次显示时按此 swapInterval 创建 backingstore swapchain。
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSwapInterval(verticalSyncEnabled ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);
}

void MainWindow::onWebrtcServiceExeBrowse()
{
    QString selectedPath = QFileDialog::getOpenFileName(
        this, tr("选择 WebRTC 系统程序"), webrtcServiceExeEdit->text(), tr("可执行程序 (*.exe)"));
    if (!selectedPath.isEmpty()) webrtcServiceExeEdit->setText(selectedPath);
}

void MainWindow::onApplySystemSettings()
{
    // 远程连接中不允许修改系统设置(避免删/注册服务导致正在运行的会话中断)
    if (isRemoteConnected) {
        ConfirmDialog(tr("无法修改"), tr("远程连接中，请先断开连接后再修改系统设置。"),
                      tr("知道了"), QString(), this).exec();
        return;
    }

    bool    verticalSyncEnabled = verticalSyncCheckBox->isChecked();
    QString signalServerHost    = signalServerHostEdit->text().trimmed();
    int     signalServerPort    = signalServerPortSpin->value();
    QString stunHost           = stunHostEdit->text().trimmed();
    QString turnHost           = turnHostEdit->text().trimmed();
    QString turnUsername       = turnUsernameEdit->text().trimmed();
    QString turnPassword       = turnPasswordEdit->text().trimmed();
    QString webrtcServiceExePath = webrtcServiceExeEdit->text().trimmed();
    QString webrtcServiceName   = webrtcServiceNameEdit->text().trimmed();

    // 服务名由用户自行设置(不从 exe 推导,避免与已安装的同名服务冲突);
    // 留空时兜底用 exe 文件名。
    QFileInfo webrtcServiceExeFileInfo(webrtcServiceExePath);
    if (webrtcServiceName.isEmpty()) {
        webrtcServiceName = webrtcServiceExeFileInfo.completeBaseName();  // 兜底:留空时仍用 exe 名
    }

    // 读取旧值用于检测服务名/可执行路径是否变更
    ConfigManager& configManager = ConfigManager::Instance();
    QString oldServiceName = QString::fromStdString(configManager.GetString("Webrtc.SystemService", "HopeDeskSystem"));
    QString oldServiceExePath = QString::fromStdString(configManager.GetString("Webrtc.SystemServiceExe"));
    bool serviceNameChanged = (webrtcServiceName != oldServiceName);
    bool serviceExeChanged = (webrtcServiceExePath != oldServiceExePath);

    // 服务配置变更:需要处理已注册的系统服务(SCM)
    if (serviceNameChanged) {
        // 服务名变了:旧名对应的服务将不再使用,询问是否删除旧服务
        ConfirmDialog confirmDialog(
            tr("服务名已变更"),
            tr("检测到服务名已由「%1」改为「%2」。\n是否删除旧服务「%1」?").arg(oldServiceName).arg(webrtcServiceName),
            tr("删除并注册新服务"), tr("保留旧服务"), this);
        if (confirmDialog.exec() == QDialog::Accepted) {
            WindowsServiceManager::deleteService(oldServiceName.toStdString());
        }
        // 注册新服务(新名 + 新可执行路径)
        WindowsServiceManager::registerService(webrtcServiceName.toStdString(), webrtcServiceExePath.toStdString());
    } else if (serviceExeChanged) {
        // 仅可执行路径变了:删旧(同名)后重新注册,使 ImagePath 指向新 exe
        WindowsServiceManager::deleteService(webrtcServiceName.toStdString());
        WindowsServiceManager::registerService(webrtcServiceName.toStdString(), webrtcServiceExePath.toStdString());
    }

    // 1. 持久化到 config.ini(键名对齐 WebrtcManagerConfig 的字段)
    configManager.Set("Render.VSync", verticalSyncEnabled);
    configManager.Set("WebrtcSignalServer.Host", signalServerHost.toStdString());
    configManager.Set("WebrtcSignalServer.Port", signalServerPort);
    configManager.Set("Stun.Host", stunHost.toStdString());
    configManager.Set("Turn.Host", turnHost.toStdString());
    configManager.Set("Turn.Username", turnUsername.toStdString());
    configManager.Set("Turn.Password", turnPassword.toStdString());
    configManager.Set("Webrtc.SystemServiceExe", webrtcServiceExePath.toStdString());
    configManager.Set("Webrtc.SystemService", webrtcServiceName.toStdString());
    // 虚拟显示器设置(独立于 WebRTC 帧率)
    configManager.Set("VitrualDisplay.DesktopWidth", spinDesktopWidth->value());
    configManager.Set("VitrualDisplay.DesktopHeight", spinDesktopHeight->value());
    configManager.Set("VitrualDisplay.DesktopRefreshRate", spinDesktopRefreshRate->value());
    configManager.Save();

    // 2. 同步内存成员
    this->verticalSyncEnabled = verticalSyncEnabled;
    if (!signalServerHost.isEmpty()) defaultServerHost = signalServerHost;
    defaultServerPort = signalServerPort;
    desktopWidth = spinDesktopWidth->value();
    desktopHeight = spinDesktopHeight->value();
    desktopRefreshRate = spinDesktopRefreshRate->value();
    syncConfigToManager();

    // 3. 同步 Qt VSync(下次创建窗口/videoWidget 生效)
    applyVSyncToFormat();

    // 4. 同步 WebrtcManagerConfig(运行期生效,下次建连/ICE 使用)
    WebrtcManagerConfig webrtcManagerConfig;
    webrtcManagerConfig.systemService    = webrtcServiceName.toStdString();
    webrtcManagerConfig.systemServiceExe = webrtcServiceExePath.toStdString();
    webrtcManagerConfig.stunHost          = stunHost.toStdString();
    webrtcManagerConfig.turnHost          = turnHost.toStdString();
    webrtcManagerConfig.turnUsername      = turnUsername.toStdString();
    webrtcManagerConfig.turnPassword      = turnPassword.toStdString();
    if (webrtcManager) webrtcManager->setWebrtcManagerConfig(webrtcManagerConfig);

    QString summary = tr("已保存并应用。\n垂直同步将在下一次远程连接时生效。");
    if (serviceNameChanged || serviceExeChanged) {
        summary += tr("\n系统服务已按新配置重新注册。");
    }
    ConfirmDialog noticeDialog(tr("系统设置"), summary, tr("知道了"), QString(), this);
    noticeDialog.exec();
}

void MainWindow::onNavHomeClicked() { ui->mainStackedWidget->setCurrentIndex(0); }
void MainWindow::onNavDevicesClicked() { ui->mainStackedWidget->setCurrentIndex(1); }
void MainWindow::onNavSettingsClicked() { ui->mainStackedWidget->setCurrentIndex(2); }

void MainWindow::startSignalServerConnection()
{
    if (isSignalConnected || currentDeviceId.isEmpty()) return;
    updateStatusUI("正在连接服务器...", "normal");
    webrtcManager->setAccountId(currentDeviceId.toStdString());
    QString url = QString("%1:%2").arg(defaultServerHost).arg(defaultServerPort);
    webrtcManager->connect(url.toStdString());
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
        hideNetworkBadge();
        ui->btnStartControl->setEnabled(true);
        ui->btnStartControl->setText("立即连接");
        ui->btnSendCtrlAltF->setEnabled(false);   // 手动断开:禁用
        ui->remoteStatusLabel->setText("远程连接已结束");
        ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");

        this->showNormal();
        this->activateWindow();

        if (webrtcManager) webrtcManager->disConnectRemote();
        return;
    }

    if (currentDeviceId.isEmpty()) {
        ConfirmDialog(tr("提示"), tr("请先登录"), tr("知道了"), QString(), this).exec();
        onUserAvatarClicked();
        return;
    }

    QString targetId = ui->remoteIdEdit->text().trimmed();
    if (targetId.isEmpty()) {
        ui->remoteIdEdit->setFocus();
        return;
    }
    if (targetId == currentDeviceId) {
         ConfirmDialog(tr("提示"), tr("无法连接本机"), tr("知道了"), QString(), this).exec();
         return;
    }

    ui->remoteStatusLabel->setText("正在建立安全连接...");
    ui->remoteStatusLabel->setStyleSheet("color: #F59E0B;");
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("连接中...");

    webrtcManager->setTargetId(targetId.toStdString());
    remoteConnectionTimer->start(REMOTE_CONNECTION_TIMEOUT);
    // 编码配置:码率由 Mbps 转 bps(1 Mbps = 1000000 bps),与 RtpEncodingParameters 单位对齐
    // 编码配置:码率由 Mbps 转 bps,并兜底保证 min<=max(防御 UI/历史配置异常)
    int reqMaxBps = requestMaxBitrateMbps * 1000000;
    int reqMinBps = requestMinBitrateMbps * 1000000;
    if (reqMinBps > reqMaxBps) reqMinBps = reqMaxBps;
    int locMaxBps = localMaxBitrateMbps * 1000000;
    int locMinBps = localMinBitrateMbps * 1000000;
    if (locMinBps > locMaxBps) locMinBps = locMaxBps;
    webrtcManager->asyncRemoteDesk({webrtcModulesType, webrtcLevels, videoCodec,
                                    webrtcAudioEnable, webrtcEnableNvenc, webrtcEnableNvdec,
                                    reqMaxBps, reqMinBps, requestMaxFramerate,
                                    locMaxBps, locMinBps, localMaxFramerate,
                                    desktopWidth, desktopHeight, desktopRefreshRate});
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
    // 被控端不发 Ctrl+Alt+F(那是操控端发给被控端的),禁用按钮
    ui->btnSendCtrlAltF->setEnabled(false);
    // 编码状态由 System 经本地 TCP(ENCODE_STATUS)上报后写入 label,这里不重复设置
}

void MainWindow::onRemoteDisconnectedByPeer()
{
    isRemoteConnected = false;
    hideNetworkBadge();
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("立即连接");
    ui->btnSendCtrlAltF->setEnabled(false);   // 断开:无连接可发,禁用
    ui->remoteStatusLabel->setText("远程连接已结束");
    ui->remoteStatusLabel->setStyleSheet("color: #9CA3AF;");
    // 断开清空状态:编/解码 label + VideoWidget 显示状态(避免残留上一帧/旧状态)
    if (ui->labelCodecStatus) ui->labelCodecStatus->setText("");
    stopFpsDisplay();
    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
    }
    // 清空本地光标缓存(下次新连接会重新从 index 0 同步)
    if (webrtcManager) webrtcManager->resetCursorCache();
}

void MainWindow::onRemoteConnectionTimeout()
{
    hideNetworkBadge();
    // 超时必须强制重置 WebrtcManager 连接态(关 tcpSocket/peerConnection、停服务、重建空白 peerConnection),
    // 否则 tcpSocket 残留会导致下一次连接发起不成功。post 到 ioContext 执行,线程安全。
    if (webrtcManager) webrtcManager->abortPendingConnection();
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
    if (webrtcManager) webrtcManager->disConnect();
    isSignalConnected = false;
    isRemoteConnected = false;

    if (videoWidget) {
        stopFpsDisplay();
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
        if(videoWidget) {
            delete videoWidget;
            videoWidget = nullptr;
        }
        if (trayIcon) {
            trayIcon->hide();
            delete trayIcon;
            trayIcon = nullptr;
        }
        if (reconnectTimer) reconnectTimer->stop();
        if (remoteConnectionTimer) remoteConnectionTimer->stop();

        // 先停 acceptor(置 asyncAccpets=false + accept.cancel/close),让 acceptor 协程
        // 在 ~MainWindow 释放 shared_ptr 之前退出并释放强引用,析构即在 GUI 线程触发,不进 ioContext 死锁。
        if (webrtcManager) webrtcManager->closeEvent();

        event->accept();

        QTimer::singleShot(0, qApp, &QApplication::quit);
    } else {
        event->ignore();
        hide();
    }
}

void MainWindow::onSystemTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) showWindow();
}

void MainWindow::on_checkHwAccel_checkStateChanged(const Qt::CheckState &state)
{
    webrtcEnableNvenc = (state != Qt::Unchecked) ? 1 : 0;
    if (settings) settings->setValue("webrtcEnableNvenc", webrtcEnableNvenc);
    syncConfigToManager();
}

void MainWindow::on_checkHwDec_checkStateChanged(const Qt::CheckState &state)
{
    webrtcEnableNvdec = (state != Qt::Unchecked) ? 1 : 0;
    if (settings) settings->setValue("webrtcEnableNvdec", webrtcEnableNvdec);
    syncConfigToManager();
}


    } // namespace rtc
} // namespace hope
