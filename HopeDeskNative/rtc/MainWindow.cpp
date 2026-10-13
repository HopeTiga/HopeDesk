#include "MainWindow.h"
#include "../utils/Utils.h"
#include "ui_mainwindow.h"
#include "widget/VideoWidget.h"
#include "widget/Theme.h"
#include "widget/FramelessWindowAgent.h"
#include "WebrtcManager.h"
#include "../utils/ConfigManager.h"
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
#include <QPainter>
#include <QPaintEvent>
#include <QTabWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QFileDialog>
#include <QColorDialog>
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

    // 被控端:本机 System 经本地 TCP(ENCODE_STATUS)上报当前编码 codec + 硬编/软编 + 采集技术 -> label
    webrtcManager->onEncodeStatusHandle = [this](const std::string& codec, bool hardEncode, const std::string& captureTech) {
        QString codecQ = QString::fromStdString(codec).toUpper();
        QString captureQ = QString::fromStdString(captureTech);
        QMetaObject::invokeMethod(this, [this, codecQ, hardEncode, captureQ]() {
            if (ui) {
                if (ui->labelCodecStatus)
                    ui->labelCodecStatus->setText(QString("编码: %1 %2").arg(codecQ).arg(hardEncode ? "硬编" : "软编"));
                if (ui->labelCaptureTech)
                    ui->labelCaptureTech->setText(QString("采集: %1").arg(captureQ));
            }
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
                    setRemoteStatus("远程连接已结束", "ended");
                    if (ui->labelCodecStatus) ui->labelCodecStatus->setText("");
                    if (ui->labelCaptureTech) ui->labelCaptureTech->setText("");
                    hidePeerCard();
                    stopFpsDisplay();

                    if(webrtcManager && videoWidget){

                        webrtcManager->setOnVideoFrameHanlder(nullptr);

                        if(videoWidget) {

                            videoWidget->hide();

                            disconnect(videoWidget, nullptr, this, nullptr);

                            delete  videoWidget;

                            videoWidget = nullptr;

                        }

                        if(webrtcManager){

                            webrtcManager->disConnectRemote();

                        }
                    }

                });
                connect(videoWidget, &QWidget::destroyed, this, [this](){ videoWidget = nullptr; });
            }
            // 按 verticalSyncEnabled 设置 defaultFormat,使 videoWidget 的 backingstore
            // swapchain 在 show 创建窗口时按此 swapInterval 创建(每次会话生效,无需重启)
            applyVSyncToFormat();
            videoWidget->showMaximized();
            videoWidget->raise();
            videoWidget->activateWindow();
            videoWidget->injectD3D11DeviceToManager();
            isRemoteConnected = true;
            ui->btnStartControl->setText("控制中...");
            ui->btnStartControl->setEnabled(false);
            ui->btnSendCtrlAltF->setEnabled(true);
            setRemoteStatus("远程连接已建立", "success");
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

    webrtcManager->onRemoteFailedHandle = [this](int state) {
        QMetaObject::invokeMethod(this, [this, state]() {
            if (remoteConnectionTimer) remoteConnectionTimer->stop();
            ui->btnStartControl->setEnabled(true);
            ui->btnStartControl->setText("立即连接");
            if (state == 403) {
                setRemoteStatus("连接失败：对方已关闭允许控制", "danger");
            } else if (state == 408) {
                setRemoteStatus("连接请求超时", "danger");
            } else {
                setRemoteStatus("连接失败：对方不在线或拒绝", "danger");
            }
            hidePeerCard();
        }, Qt::QueuedConnection);
    };

    webrtcManager->onSessionInfoHandle = [this](int myRole, const std::string& peerId) {
        QString idQ = QString::fromStdString(peerId);
        QMetaObject::invokeMethod(this, [this, myRole, idQ]() {
            showPeerCard(myRole, idQ);
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
        settings->setValue("showEncodeInfo", showEncodeInfoEnabled);
        settings->setValue("showHotkey", showHotkeyEnabled);
        settings->setValue("allowControl", allowControlEnabled);
        settings->setValue("webrtcEnableNvenc", webrtcEnableNvenc);
        settings->setValue("webrtcEnableNvdec", webrtcEnableD3D11);
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
    // 托盘恢复时平台窗口会重建,重新挂上无边框窗口的事件过滤器
    if (windowAgent) windowAgent->attachToWindow();
    // 只有已登录才自动连接
    if (!isSignalConnected && !currentDeviceId.isEmpty()) {
        QTimer::singleShot(200, this, &MainWindow::startSignalServerConnection);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    ui->sideBar->setFixedWidth(qBound(200, qRound(event->size().width() * 0.247), 300));
    ui->titleBar->update();
}

void MainWindow::paintEvent(QPaintEvent* event) {
    QMainWindow::paintEvent(event);
    if (windowBackgroundPixmap.isNull()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setOpacity(1.0 - backgroundOpacity / 100.0);
    painter.drawPixmap(rect(), windowBackgroundPixmap);
}

void MainWindow::updateWindowButtons() {
    if (!ui || !ui->btnWinMenu) return;
    ui->btnWinMenu->setText(QStringLiteral("\uE700"));
    ui->btnWinMin->setText(QStringLiteral("\uE921"));
    ui->btnWinClose->setText(QStringLiteral("\uE8BB"));
}

static QWidget* enclosingScrollViewport(QWidget* widget)
{
    QWidget* parent = widget->parentWidget();
    while (parent) {
        QAbstractScrollArea* area = qobject_cast<QAbstractScrollArea*>(parent);
        if (area) return area->viewport();
        parent = parent->parentWidget();
    }
    return nullptr;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == ui->titleBar && event->type() == QEvent::Paint) {
        QPainter painter(ui->titleBar);
        painter.fillRect(QRect(0, 0, ui->sideBar->width(), ui->titleBar->height()),
                         theme::surfaceColor("bgSideBar", backgroundOpacity / 100.0));
        return true;
    }
    if (watched == ui->userFrame && event->type() == QEvent::MouseButtonRelease) {
        onUserAvatarClicked();
        return true;
    }
    if (watched == ui->remoteIdEdit && event->type() == QEvent::MouseButtonPress) {
        if (ui->remoteIdEdit->text().trimmed().isEmpty()) showRecentPopup();
    }
    if (event->type() == QEvent::Wheel) {
        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (widget && !widget->hasFocus()
            && (qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QComboBox*>(widget))) {
            QWidget* viewport = enclosingScrollViewport(widget);
            if (viewport) QApplication::sendEvent(viewport, event);
            return true;
        }
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
    webrtcManagerConfig.webrtcDebugLog    = ConfigManager::Instance().GetBool("Webrtc.DebugLog", false);   // 启动即加载,首包 REGISTER 即携带正确开关
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
    webrtcEnableD3D11 = settings->value("webrtcEnableNvdec",
        settings->value("webrtcEnableNvidia", 0).toInt()).toInt();

    ui->modeComboBox->setCurrentIndex(webrtcModulesType);
    ui->codecComboBox->setCurrentIndex(videoCodec);
    ui->accelerationComboBox->setCurrentIndex(webrtcLevels);
    ui->checkAudio->setChecked(webrtcAudioEnable == 1);
    ui->checkHwAccel->setChecked(webrtcEnableNvenc == 1);
    ui->checkHwDec->setChecked(webrtcEnableD3D11 == 1);

    showRttEnabled = settings->value("showRtt", false).toBool();
    ui->checkShowRtt->setChecked(showRttEnabled);
    showFpsEnabled = settings->value("showFps", false).toBool();
    ui->checkShowFps->setChecked(showFpsEnabled);
    showEncodeInfoEnabled = settings->value("showEncodeInfo", true).toBool();
    ui->checkShowEncodeInfo->setChecked(showEncodeInfoEnabled);
    ui->labelCodecStatus->setVisible(showEncodeInfoEnabled);
    ui->labelCaptureTech->setVisible(showEncodeInfoEnabled);
    showHotkeyEnabled = settings->value("showHotkey", true).toBool();
    ui->checkShowHotkey->setChecked(showHotkeyEnabled);
    ui->btnSendCtrlAltF->setVisible(showHotkeyEnabled);
    allowControlEnabled = settings->value("allowControl", true).toBool();
    if (webrtcManager) webrtcManager->setOpenBeControl(allowControlEnabled);
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

    windowBackgroundPath = settings->value("backgroundImagePath", "").toString();
    backgroundOpacity    = settings->value("backgroundImageOpacity", 100).toInt();
    textColorName        = settings->value("textColor", "").toString();
    deviceCodeSize       = settings->value("deviceCodeSize", 24).toInt();
    ui->lineEditWindowBackground->setText(windowBackgroundPath);
    ui->spinBackgroundOpacity->setValue(backgroundOpacity);
    ui->lineEditTextColor->setText(textColorName);
    ui->spinDeviceCodeSize->setValue(deviceCodeSize);
    ui->spinWindowWidth->setValue(settings->value("windowWidth", 1080).toInt());
    ui->spinWindowHeight->setValue(settings->value("windowHeight", 800).toInt());

    const int deskMode = settings->value("deskMode", 0).toInt();
    ui->radioDeskRemote->setChecked(deskMode == 0);
    ui->radioDeskFile->setChecked(deskMode == 1);
    ui->radioDeskView->setChecked(deskMode == 2);
    ui->radioDeskCoop->setChecked(deskMode == 3);

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
        ui->myDeviceCodeLabel->setText("---------");
        ui->userNameLabel->setText("未登录");
        ui->userStatusLabel->setText("● 离线 (点击登录)");
        theme::setState(ui->userStatusLabel, "state", QStringLiteral("offline"));

        ui->userAvatar->setPixmap(QPixmap());
        ui->userAvatar->setText("?");
        theme::setState(ui->userAvatar, "state", QStringLiteral("empty"));

        updateRecentListUI();
        if(ui->deviceGroupList->currentRow() == 1) updateDeviceListUI(false);
        return;
    }

    // 已登录状态
    ui->myDeviceCodeLabel->setText(currentDeviceId);

    ui->userNameLabel->setText(currentUserName);
    ui->userStatusLabel->setText("● 在线 (点击编辑)");
    theme::setState(ui->userStatusLabel, "state", QStringLiteral("online"));

    if(!customAvatarPath.isEmpty()) {
        QPixmap pixmap;
        QByteArray byteArray = QByteArray::fromBase64(customAvatarPath.toLatin1());
        pixmap.loadFromData(byteArray);

        if(!pixmap.isNull()) {
            QPixmap circular = createCircularAvatar(pixmap, 38);
            ui->userAvatar->setPixmap(circular);
            ui->userAvatar->setText("");
            theme::setState(ui->userAvatar, "state", QStringLiteral("image"));
        } else {
            ui->userAvatar->setPixmap(QPixmap());
            ui->userAvatar->setText(currentUserName.left(1).toUpper());
            theme::setState(ui->userAvatar, "state", QStringLiteral("letter"));
        }
    } else {
        ui->userAvatar->setPixmap(QPixmap());
        ui->userAvatar->setText(currentUserName.left(1).toUpper());
        theme::setState(ui->userAvatar, "state", QStringLiteral("letter"));
    }

    // 本机识别码变了,「本机」标记要跟着重算
    updateRecentListUI();
    if(ui->deviceGroupList->currentRow() == 1) updateDeviceListUI(false);
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
    setWindowFlag(Qt::FramelessWindowHint, true);

    windowAgent = new FramelessWindowAgent(this, this);
    windowAgent->addDraggableArea(ui->titleBar);
    windowAgent->addInteractiveWidget(ui->btnWinMenu);
    windowAgent->addInteractiveWidget(ui->btnWinMin);
    windowAgent->addInteractiveWidget(ui->btnWinClose);

    connect(ui->btnWinMenu, &QPushButton::clicked, this, &MainWindow::showTitleMenu);
    connect(ui->btnWinMin, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(ui->btnWinClose, &QPushButton::clicked, this, &QWidget::close);

    updateWindowButtons();

    QIcon appIcon(":/logo/res/hope.jpg");
    setWindowIcon(appIcon);

    ui->titleBar->setAttribute(Qt::WA_StyledBackground, true);
    ui->sideBar->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerMyDevice->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerRemote->setAttribute(Qt::WA_StyledBackground, true);

    allowControlSwitch = new SwitchButton(this);
    allowControlSwitch->setChecked(allowControlEnabled);
    ui->horizontalLayout_AllowControl->insertWidget(1, allowControlSwitch);

    ui->verticalLayout_Home->setStretchFactor(ui->widgetHomeFill, 1);

    ui->btnNavHome->setIconSize(QSize(18, 18));
    ui->btnNavDevices->setIconSize(QSize(18, 18));
    ui->btnNavSettings->setIconSize(QSize(18, 18));

    connect(ui->btnNavHome, &QPushButton::clicked, this, &MainWindow::onNavHomeClicked);
    connect(ui->btnNavDevices, &QPushButton::clicked, this, &MainWindow::onNavDevicesClicked);
    connect(ui->btnNavSettings, &QPushButton::clicked, this, &MainWindow::onNavSettingsClicked);

    connect(ui->btnNavHome, &QPushButton::toggled, this, [this](bool checked) {
        ui->btnNavHome->setIcon(theme::icon(QStringLiteral(":/icons/monitor.svg"),
            checked ? theme::color("primary") : theme::color("textFaint"), 18));
    });
    connect(ui->btnNavDevices, &QPushButton::toggled, this, [this](bool checked) {
        ui->btnNavDevices->setIcon(theme::icon(QStringLiteral(":/icons/link.svg"),
            checked ? theme::color("primary") : theme::color("textFaint"), 18));
    });
    connect(ui->btnNavSettings, &QPushButton::toggled, this, [this](bool checked) {
        ui->btnNavSettings->setIcon(theme::icon(QStringLiteral(":/icons/settings.svg"),
            checked ? theme::color("primary") : theme::color("textFaint"), 18));
    });

    ui->btnNavHome->setIcon(theme::icon(QStringLiteral(":/icons/monitor.svg"), theme::color("primary"), 18));
    ui->btnNavDevices->setIcon(theme::icon(QStringLiteral(":/icons/link.svg"), theme::color("textFaint"), 18));
    ui->btnNavSettings->setIcon(theme::icon(QStringLiteral(":/icons/settings.svg"), theme::color("textFaint"), 18));

    ui->deviceGroupList->setIconSize(QSize(16, 16));
    if (ui->deviceGroupList->item(0)) {
        ui->deviceGroupList->item(0)->setIcon(theme::icon(QStringLiteral(":/icons/star.svg"), theme::color("textSub"), 16));
    }
    if (ui->deviceGroupList->item(1)) {
        ui->deviceGroupList->item(1)->setIcon(theme::icon(QStringLiteral(":/icons/clock.svg"), theme::color("textSub"), 16));
    }

    ui->btnCopyCode->setIconSize(QSize(14, 14));
    ui->btnCopyCode->setIcon(theme::icon(QStringLiteral(":/icons/copy.svg"), theme::color("textFaint"), 14));
    ui->btnAddDevice->setIconSize(QSize(14, 14));
    ui->btnAddDevice->setIcon(theme::icon(QStringLiteral(":/icons/plus.svg"), theme::color("primary"), 14));
    ui->btnCopyPeerId->setIconSize(QSize(14, 14));
    ui->btnCopyPeerId->setIcon(theme::icon(QStringLiteral(":/icons/copy.svg"), theme::color("textFaint"), 14));

    ui->containerPeer->setVisible(false);
    theme::setState(ui->peerRoleBadge, "role", QStringLiteral("controller"));
    ui->remoteStatusLabel->setVisible(false);
    theme::setState(ui->remoteStatusLabel, "state", QStringLiteral("idle"));
    theme::setState(ui->connectionStatusLabel, "state", QStringLiteral("normal"));

    buildRecentPopup();
    QAction* recentAction = ui->remoteIdEdit->addAction(
        theme::icon(QStringLiteral(":/icons/chevron-down.svg"), theme::color("textFaint"), 22),
        QLineEdit::TrailingPosition);
    connect(recentAction, &QAction::triggered, this, [this]() {
        if (recentPopup && recentPopup->isVisible()) hideRecentPopup();
        else showRecentPopup();
    });

    ui->userFrame->installEventFilter(this);
    ui->remoteIdEdit->installEventFilter(this);

    trayMenu = new QMenu(this);
    QAction* showAction = trayMenu->addAction("显示主界面", this, &MainWindow::showWindow);
    showAction->setIcon(theme::icon(QStringLiteral(":/icons/monitor.svg"), theme::color("textSub"), 16));
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction("退出", this, &MainWindow::quitApplication);
    quitAction->setIcon(theme::icon(QStringLiteral(":/icons/close.svg"), theme::color("textSub"), 16));

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
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

    const QList<QAbstractSpinBox*> spinBoxes = findChildren<QAbstractSpinBox*>();
    for (QAbstractSpinBox* spin : spinBoxes) spin->setFocusPolicy(Qt::StrongFocus);
    const QList<QComboBox*> comboBoxes = findChildren<QComboBox*>();
    for (QComboBox* combo : comboBoxes) combo->setFocusPolicy(Qt::StrongFocus);
    ui->spinWindowWidth->setKeyboardTracking(false);
    ui->spinWindowHeight->setKeyboardTracking(false);
    ui->spinDeviceCodeSize->setKeyboardTracking(false);
    qApp->installEventFilter(this);

    applyWindowSize();
    applyBackgrounds();
}

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

    for(DeviceInfo& favorite : favoritesList) {
        for(const DeviceInfo& device : historyList) {
            if(normalizeDeviceId(device.id) == normalizeDeviceId(favorite.id)) {
                if(device.lastAccess > favorite.lastAccess) favorite.lastAccess = device.lastAccess;
                break;
            }
        }
    }
}

void MainWindow::addToHistory(const QString& id, const QString& name) {
    if(!settings) return;
    QString deviceName = name;
    if(deviceName.isEmpty()) deviceName = nameFromFavorites(id);

    const QString normalized = normalizeDeviceId(id);
    const qint64 lastAccess = QDateTime::currentMSecsSinceEpoch();

    bool found = false;
    for(int i=0; i<historyList.size(); ++i) {
        if(normalizeDeviceId(historyList[i].id) == normalized) {
            historyList[i].lastAccess = lastAccess;
            if(!deviceName.isEmpty()) historyList[i].name = deviceName;
            historyList.move(i, 0);
            found = true;
            break;
        }
    }
    if (!found) {
        DeviceInfo info;
        info.id = id;
        info.name = deviceName.isEmpty() ? QStringLiteral("未知设备") : deviceName;
        info.lastAccess = lastAccess;
        historyList.insert(0, info);
        if(historyList.size() > 20) historyList.removeLast();
    }

    QJsonArray array;
    for(const auto& dev : historyList) array.append(dev.toJson());
    settings->setValue("historyList", QJsonDocument(array).toJson());

    bool favoritesTouched = false;
    for(DeviceInfo& favorite : favoritesList) {
        if(normalizeDeviceId(favorite.id) == normalized) {
            favorite.lastAccess = lastAccess;
            favoritesTouched = true;
            break;
        }
    }
    if (favoritesTouched) {
        QJsonArray favoritesArray;
        for(const auto& dev : favoritesList) favoritesArray.append(dev.toJson());
        settings->setValue("favoritesList", QJsonDocument(favoritesArray).toJson());
    }

    updateRecentListUI();
    updateDeviceListUI(ui->deviceGroupList->currentRow() == 0);
}

void MainWindow::editDeviceInfo(const QString& id, bool isFavorite) {
    QList<DeviceInfo>& list = isFavorite ? favoritesList : historyList;
    QList<DeviceInfo>& other = isFavorite ? historyList : favoritesList;

    const QString oldId = normalizeDeviceId(id);
    DeviceInfo* target = nullptr;
    for(DeviceInfo& device : list) {
        if(normalizeDeviceId(device.id) == oldId) {
            target = &device;
            break;
        }
    }
    if(!target) return;

    AddDeviceDialog dlg(this);
    dlg.setEditMode(target->id, target->name);
    if(dlg.exec() != QDialog::Accepted) return;

    const QString newId = dlg.getID();
    if(newId.isEmpty()) return;
    const QString newName = dlg.getName().isEmpty() ? target->name : dlg.getName();

    target->id = newId;
    target->name = newName;

    for(DeviceInfo& device : other) {
        if(normalizeDeviceId(device.id) == oldId) {
            device.id = newId;
            device.name = newName;
            break;
        }
    }

    for(QList<DeviceInfo>* devices : { &favoritesList, &historyList }) {
        for(int i = devices->size() - 1; i > 0; --i) {
            for(int j = 0; j < i; ++j) {
                if(normalizeDeviceId(devices->at(i).id) == normalizeDeviceId(devices->at(j).id)) {
                    devices->removeAt(i);
                    break;
                }
            }
        }
    }

    QJsonArray favoritesArray;
    for(const auto& dev : favoritesList) favoritesArray.append(dev.toJson());
    settings->setValue("favoritesList", QJsonDocument(favoritesArray).toJson());
    QJsonArray historyArray;
    for(const auto& dev : historyList) historyArray.append(dev.toJson());
    settings->setValue("historyList", QJsonDocument(historyArray).toJson());

    updateRecentListUI();
    updateDeviceListUI(ui->deviceGroupList->currentRow() == 0);
}

void MainWindow::removeDevice(const QString& id, bool isFavorite) {
    QList<DeviceInfo>& list = isFavorite ? favoritesList : historyList;
    const QString normalized = normalizeDeviceId(id);

    for(int i=0; i<list.size(); ++i) {
        if(normalizeDeviceId(list[i].id) == normalized) {
            list.removeAt(i);
            break;
        }
    }

    QJsonArray array;
    for(const auto& dev : list) array.append(dev.toJson());
    settings->setValue(isFavorite ? "favoritesList" : "historyList", QJsonDocument(array).toJson());

    updateRecentListUI();
    updateDeviceListUI(ui->deviceGroupList->currentRow() == 0);
}

void MainWindow::showDeviceMenu(QListWidget* list, const QPoint& pos, bool isFavorite) {
    QListWidgetItem* item = list->itemAt(pos);
    if(!item) return;

    const QString id = item->data(Qt::UserRole).toString();
    if(id.isEmpty()) return;

    QMenu menu(list);
    QAction* editAction = menu.addAction(
        theme::icon(QStringLiteral(":/icons/edit.svg"), theme::color("primaryInk"), 16), "编辑设备");
    QAction* delAction = menu.addAction(
        theme::icon(QStringLiteral(":/icons/trash.svg"), theme::color("danger"), 16), "删除此设备");

    connect(editAction, &QAction::triggered, [this, id, isFavorite](){
        editDeviceInfo(id, isFavorite);
    });
    connect(delAction, &QAction::triggered, [this, id, isFavorite](){
        removeDevice(id, isFavorite);
    });

    menu.exec(list->mapToGlobal(pos));
}

// ---------------- UI 更新 ----------------

// 识别码可能带空白(手输或粘贴),比对前去掉
QString MainWindow::normalizeDeviceId(const QString& id) {
    QString normalized;
    normalized.reserve(id.size());
    for (const QChar& ch : id) {
        if (!ch.isSpace()) normalized.append(ch);
    }
    return normalized;
}

QString MainWindow::displayNameFor(const DeviceInfo& device) const {
    if (!currentDeviceId.isEmpty() && normalizeDeviceId(device.id) == normalizeDeviceId(currentDeviceId)) {
        return QStringLiteral("本机");
    }
    if (device.name.isEmpty() || device.name == QStringLiteral("未知设备")) {
        const QString favoriteName = nameFromFavorites(device.id);
        if (!favoriteName.isEmpty()) return favoriteName;
    }
    return device.name;
}

QString MainWindow::nameFromFavorites(const QString& id) const {
    const QString normalized = normalizeDeviceId(id);
    for (const DeviceInfo& favorite : favoritesList) {
        if (normalizeDeviceId(favorite.id) == normalized) return favorite.name;
    }
    return QString();
}

void MainWindow::updateRecentListUI() {
    if (!recentPopupList) return;
    recentPopupList->clear();
    if (historyList.isEmpty()) return;

    for(const DeviceInfo& dev : historyList) {
        QString label = dev.id;
        const QString name = displayNameFor(dev);
        if (!name.isEmpty() && name != dev.id) label += QString("  ·  %1").arg(name);

        QListWidgetItem* item = new QListWidgetItem(
            theme::icon(QStringLiteral(":/icons/clock.svg"), theme::color("textFaint"), 16), label);
        item->setSizeHint(QSize(0, 34));
        item->setData(Qt::UserRole, dev.id);
        item->setToolTip(QString("识别码: %1\n上次访问: %2").arg(dev.id)
            .arg(QDateTime::fromMSecsSinceEpoch(dev.lastAccess).toString("yyyy-MM-dd HH:mm")));
        recentPopupList->addItem(item);
    }
}

void MainWindow::updateDeviceListUI(bool showFavorites) {
    ui->deviceDetailList->clear();
    const QList<DeviceInfo>& list = showFavorites ? favoritesList : historyList;

    for(const auto& dev : list) {
        QString text = QString("%1 - %2").arg(displayNameFor(dev)).arg(dev.id);
        QListWidgetItem* item = new QListWidgetItem(
            theme::icon(QStringLiteral(":/icons/monitor.svg"), theme::color("textSub"), 18), text);
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
        webrtcAudioEnable, webrtcEnableNvenc, webrtcEnableD3D11,
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
    connect(ui->btnCopyPeerId, &QPushButton::clicked, this, [this]() {
        const QString peerId = ui->peerIdLabel->text();
        if (peerId.isEmpty()) return;
        QApplication::clipboard()->setText(peerId);
        ui->btnCopyPeerId->setText("已复制");
        ui->btnCopyPeerId->setIcon(theme::icon(QStringLiteral(":/icons/check.svg"), theme::color("success"), 14));
        QTimer::singleShot(2000, this, [this]() {
            ui->btnCopyPeerId->setText("复制");
            ui->btnCopyPeerId->setIcon(theme::icon(QStringLiteral(":/icons/copy.svg"), theme::color("textFaint"), 14));
        });
    });
    connect(ui->deviceGroupList, &QListWidget::currentItemChanged, this, &MainWindow::onDeviceGroupChanged);
    connect(ui->deviceDetailList, &QListWidget::itemClicked, this, &MainWindow::onDeviceItemClicked);

    connect(ui->btnAddDevice, &QPushButton::clicked, this, &MainWindow::onAddDeviceClicked);

    ui->deviceDetailList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->deviceDetailList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos){
        showDeviceMenu(ui->deviceDetailList, pos, ui->deviceGroupList->currentRow() == 0);
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
    connect(ui->checkShowEncodeInfo, &QCheckBox::clicked, this, &MainWindow::onShowEncodeInfoChecked);
    connect(ui->checkShowHotkey, &QCheckBox::clicked, this, &MainWindow::onShowHotkeyChecked);
    connect(allowControlSwitch, &SwitchButton::toggled, this, &MainWindow::onAllowControlToggled);

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

    connect(ui->btnWindowBackgroundBrowse, &QPushButton::clicked, this, &MainWindow::pickBackgroundImage);
    connect(ui->btnWindowBackgroundClear, &QPushButton::clicked, this, &MainWindow::clearBackgroundImage);
    connect(ui->spinBackgroundOpacity, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onBackgroundOpacityChanged);
    connect(ui->btnTextColorPick, &QPushButton::clicked, this, &MainWindow::pickTextColor);
    connect(ui->btnTextColorReset, &QPushButton::clicked, this, &MainWindow::resetTextColor);
    connect(ui->lineEditTextColor, &QLineEdit::editingFinished, this, &MainWindow::onTextColorEdited);
    connect(ui->spinDeviceCodeSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onDeviceCodeSizeChanged);
    connect(ui->btnDeviceCodeSizeReset, &QPushButton::clicked, this, &MainWindow::resetDeviceCodeSize);
    connect(ui->spinWindowWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onWindowSizeChanged);
    connect(ui->spinWindowHeight, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onWindowSizeChanged);
    connect(ui->radioDeskRemote, &QRadioButton::toggled, this, &MainWindow::onDeskModeChanged);
    connect(ui->radioDeskFile, &QRadioButton::toggled, this, &MainWindow::onDeskModeChanged);
    connect(ui->radioDeskView, &QRadioButton::toggled, this, &MainWindow::onDeskModeChanged);
    connect(ui->radioDeskCoop, &QRadioButton::toggled, this, &MainWindow::onDeskModeChanged);
}

void MainWindow::applyBackgrounds() {
    windowBackgroundPixmap = windowBackgroundPath.isEmpty() ? QPixmap() : QPixmap(windowBackgroundPath);
    qApp->setStyleSheet(theme::loadStyleSheet(QStringLiteral(":/styles/theme.qss"), backgroundOpacity / 100.0, QColor(textColorName), deviceCodeSize));
    update();
}

void MainWindow::pickBackgroundImage() {
    const QString selectedPath = QFileDialog::getOpenFileName(this, tr("选择背景图"), windowBackgroundPath,
        tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (selectedPath.isEmpty()) return;
    windowBackgroundPath = selectedPath;
    ui->lineEditWindowBackground->setText(selectedPath);
    settings->setValue("backgroundImagePath", selectedPath);
    applyBackgrounds();
}

void MainWindow::clearBackgroundImage() {
    windowBackgroundPath.clear();
    ui->lineEditWindowBackground->clear();
    settings->setValue("backgroundImagePath", QString());
    applyBackgrounds();
}

void MainWindow::onBackgroundOpacityChanged(int value) {
    backgroundOpacity = value;
    settings->setValue("backgroundImageOpacity", value);
    applyBackgrounds();
}

void MainWindow::pickTextColor() {
    const QColor initial = QColor(textColorName).isValid() ? QColor(textColorName) : theme::color("text");
    const QColor picked = QColorDialog::getColor(initial, this, tr("选择文字颜色"));
    if (!picked.isValid()) return;
    textColorName = picked.name();
    ui->lineEditTextColor->setText(textColorName);
    settings->setValue("textColor", textColorName);
    applyBackgrounds();
}

void MainWindow::resetTextColor() {
    textColorName.clear();
    ui->lineEditTextColor->clear();
    settings->setValue("textColor", QString());
    applyBackgrounds();
}

void MainWindow::onTextColorEdited() {
    const QString typed = ui->lineEditTextColor->text().trimmed();
    const QColor color(typed);

    if (!typed.isEmpty() && !color.isValid()) {
        ui->lineEditTextColor->setText(textColorName);
        return;
    }

    const QString normalized = color.isValid() ? color.name() : QString();
    ui->lineEditTextColor->setText(normalized);
    if (normalized == textColorName) return;

    textColorName = normalized;
    settings->setValue("textColor", textColorName);
    applyBackgrounds();
}

void MainWindow::onDeviceCodeSizeChanged(int value) {
    deviceCodeSize = value;
    settings->setValue("deviceCodeSize", value);
    applyBackgrounds();
}

void MainWindow::resetDeviceCodeSize() {
    ui->spinDeviceCodeSize->setValue(24);
}

void MainWindow::applyWindowSize() {
    const int requestedWidth  = ui->spinWindowWidth->value();
    const int requestedHeight = ui->spinWindowHeight->value();
    resize(requestedWidth, requestedHeight);

    const int actualWidth  = width();
    const int actualHeight = height();
    if (actualWidth != requestedWidth) {
        QSignalBlocker blocker(ui->spinWindowWidth);
        ui->spinWindowWidth->setValue(actualWidth);
    }
    if (actualHeight != requestedHeight) {
        QSignalBlocker blocker(ui->spinWindowHeight);
        ui->spinWindowHeight->setValue(actualHeight);
    }
}

void MainWindow::onWindowSizeChanged() {
    settings->setValue("windowWidth", ui->spinWindowWidth->value());
    settings->setValue("windowHeight", ui->spinWindowHeight->value());
    applyWindowSize();
}

void MainWindow::onDeskModeChanged() {
    int mode = 0;
    if (ui->radioDeskFile->isChecked()) mode = 1;
    else if (ui->radioDeskView->isChecked()) mode = 2;
    else if (ui->radioDeskCoop->isChecked()) mode = 3;
    settings->setValue("deskMode", mode);
}

void MainWindow::showTitleMenu() {
    if (!trayMenu) return;
    trayMenu->popup(ui->btnWinMenu->mapToGlobal(QPoint(0, ui->btnWinMenu->height())));
}

void MainWindow::onAddDeviceClicked() {
    // 检查是否登录
    if(currentDeviceId.isEmpty()) {
        ConfirmDialog(tr("提示"), tr("请先登录"), tr("知道了"), QString(), this).exec();
        onUserAvatarClicked();
        return;
    }

    AddDeviceDialog dlg(this);
    if(dlg.exec() != QDialog::Accepted) return;

    const QString id = dlg.getID();
    if(id.isEmpty()) return;
    const QString name = dlg.getName();

    const QString normalized = normalizeDeviceId(id);
    qint64 lastAccess = QDateTime::currentMSecsSinceEpoch();
    for(const DeviceInfo& device : historyList) {
        if(normalizeDeviceId(device.id) == normalized) {
            lastAccess = device.lastAccess;
            break;
        }
    }

    const bool toFavorites = (ui->mainStackedWidget->currentIndex() != 0) && (ui->deviceGroupList->currentRow() == 0);
    QList<DeviceInfo>& list = toFavorites ? favoritesList : historyList;

    bool found = false;
    for(DeviceInfo& device : list) {
        if(normalizeDeviceId(device.id) == normalized) {
            if(!name.isEmpty()) device.name = name;
            device.lastAccess = lastAccess;
            found = true;
            break;
        }
    }
    if(!found) {
        DeviceInfo info;
        info.id = id;
        info.name = name;
        info.lastAccess = lastAccess;
        if(info.name.isEmpty()) info.name = toFavorites ? "我的设备" : "未知设备";
        if(toFavorites) list.append(info);
        else list.insert(0, info);
    }
    if(!toFavorites && list.size() > 20) list.removeLast();

    QJsonArray array;
    for(const auto& device : list) array.append(device.toJson());
    settings->setValue(toFavorites ? "favoritesList" : "historyList", QJsonDocument(array).toJson());

    updateRecentListUI();
    updateDeviceListUI(ui->deviceGroupList->currentRow() == 0);
}

void MainWindow::onClearHistoryClicked() {
    if(ConfirmDialog(tr("确认"), tr("确定清空所有历史记录吗？"), tr("清空"), tr("取消"), this).exec() == QDialog::Accepted) {
        historyList.clear();
        settings->remove("historyList");
        updateRecentListUI();
        updateDeviceListUI(ui->deviceGroupList->currentRow() == 0);
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

    if (showRttEnabled) {
        if (isRemoteConnected && statsTimer) {
            statsTimer->start();
            if (webrtcManager) webrtcManager->requestStats();
        }
    } else {
        if (statsTimer) statsTimer->stop();
    }

    if (!ui->networkTypeBadge->isHidden()) {
        refreshNetworkBadge();
    }
}

void MainWindow::onShowFpsChecked(bool checked)
{
    showFpsEnabled = checked;
    if (settings) settings->setValue("showFps", showFpsEnabled);

    if (showFpsEnabled) {
        startFpsDisplay();
    } else {
        stopFpsDisplay();
    }
}

void MainWindow::onShowEncodeInfoChecked(bool checked)
{
    showEncodeInfoEnabled = checked;
    if (settings) settings->setValue("showEncodeInfo", showEncodeInfoEnabled);
    ui->labelCodecStatus->setVisible(showEncodeInfoEnabled);
    ui->labelCaptureTech->setVisible(showEncodeInfoEnabled);
}

void MainWindow::onShowHotkeyChecked(bool checked)
{
    showHotkeyEnabled = checked;
    if (settings) settings->setValue("showHotkey", showHotkeyEnabled);
    ui->btnSendCtrlAltF->setVisible(showHotkeyEnabled);
}

void MainWindow::onAllowControlToggled(bool checked)
{
    allowControlEnabled = checked;
    if (settings) settings->setValue("allowControl", allowControlEnabled);
    if (webrtcManager) webrtcManager->setOpenBeControl(allowControlEnabled);
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
    settingsTabWidget->setObjectName("settingsTabWidget");
    // documentMode 去掉默认厚重边框;显式继承设置页字体,避免 QTabWidget 自带字体影响内容
    settingsTabWidget->setDocumentMode(true);
    settingsTabWidget->setFont(ui->pageSettings->font());

    // 「通用设置」tab:把现有滚动区整体移入(addTab 会自动 reparent,并从原布局移除)
    settingsTabWidget->addTab(ui->scrollAreaSettings, tr("通用设置"));

    // 「系统设置」tab
    QWidget* systemTab = new QWidget(settingsTabWidget);
    systemTab->setObjectName("systemTab");
    systemTab->setAttribute(Qt::WA_StyledBackground, true);
    QVBoxLayout* systemLayout = new QVBoxLayout(systemTab);
    systemLayout->setSpacing(12);
    systemLayout->setContentsMargins(20, 20, 20, 20);

    QFormLayout* systemFormLayout = new QFormLayout();
    systemFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    systemFormLayout->setSpacing(10);

    verticalSyncCheckBox = new QCheckBox(tr("启用"), systemTab);
    systemFormLayout->addRow(tr("垂直同步"), verticalSyncCheckBox);

    webrtcDebugLogCheckBox = new QCheckBox(tr("启用"), systemTab);
    systemFormLayout->addRow(tr("调试日志"), webrtcDebugLogCheckBox);

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
    labelVddNameValue->setObjectName("labelVddNameValue");
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
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::onApplySystemSettings);
    systemLayout->addWidget(applyButton, 0, Qt::AlignLeft);
    systemLayout->addStretch();

    QScrollArea* systemScrollArea = new QScrollArea(settingsTabWidget);
    systemScrollArea->setObjectName("systemScrollArea");
    systemScrollArea->setWidgetResizable(true);
    systemScrollArea->setFrameShape(QFrame::NoFrame);
    systemScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    systemScrollArea->setWidget(systemTab);
    settingsTabWidget->addTab(systemScrollArea, tr("系统设置"));

    // 把 tab 控件放进设置页原布局(scrollAreaSettings 已被 reparent 走)
    ui->verticalLayout_Settings->addWidget(settingsTabWidget);
}

void MainWindow::loadSystemSettings()
{
    ConfigManager& configManager = ConfigManager::Instance();
    verticalSyncCheckBox->setChecked(configManager.GetBool("Render.VSync", false));
    webrtcDebugLogCheckBox->setChecked(configManager.GetBool("Webrtc.DebugLog", false));
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
    bool    webrtcDebugLog      = webrtcDebugLogCheckBox->isChecked();
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
    configManager.Set("Webrtc.DebugLog", webrtcDebugLog);
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
    // WebRTC 调试日志立即生效(native 自身也受开关控制)
    if (webrtcDebugLog) initWebrtcLogging();
    else closeWebrtcLogging();
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
    webrtcManagerConfig.webrtcDebugLog    = webrtcDebugLog;   // 随 registerJson 传给 System
    if (webrtcManager) webrtcManager->setWebrtcManagerConfig(webrtcManagerConfig);
    // debugLog 立即推送:若本地 System 的 TCP 已连,重发 REGISTER 让开关马上生效(无需等下次 TCP accept)
    if (webrtcManager) webrtcManager->applyWebrtcDebugLog(webrtcDebugLog);

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
        setRemoteStatus("远程连接已结束", "ended");
        if (ui->labelCodecStatus) ui->labelCodecStatus->setText("");
        if (ui->labelCaptureTech) ui->labelCaptureTech->setText("");
        hidePeerCard();

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

    setRemoteStatus("正在建立安全连接...", "connecting");
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
                                    webrtcAudioEnable, webrtcEnableNvenc, webrtcEnableD3D11,
                                    reqMaxBps, reqMinBps, requestMaxFramerate,
                                    locMaxBps, locMinBps, localMaxFramerate,
                                    desktopWidth, desktopHeight, desktopRefreshRate});
}

void MainWindow::onBtnCopyCodeClicked()
{
    if(currentDeviceId.isEmpty()) return;
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(currentDeviceId);
    ui->btnCopyCode->setText("已复制");
    ui->btnCopyCode->setIcon(theme::icon(QStringLiteral(":/icons/check.svg"), theme::color("success"), 14));
    QTimer::singleShot(2000, [this](){
        ui->btnCopyCode->setText("复制");
        ui->btnCopyCode->setIcon(theme::icon(QStringLiteral(":/icons/copy.svg"), theme::color("textFaint"), 14));
    });
}

void MainWindow::onRemoteControlStarted()
{
    if (remoteConnectionTimer->isActive()) remoteConnectionTimer->stop();
    isRemoteConnected = true;
    ui->btnStartControl->setText("断开连接");
    ui->btnStartControl->setEnabled(true);
    setRemoteStatus("正在被远程控制中", "danger");
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
    setRemoteStatus("远程连接已结束", "ended");
    hidePeerCard();
    // 断开清空状态:编/解码 + 采集 label + VideoWidget 显示状态(避免残留上一帧/旧状态)
    if (ui->labelCodecStatus) ui->labelCodecStatus->setText("");
    if (ui->labelCaptureTech) ui->labelCaptureTech->setText("");
    stopFpsDisplay();
    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
        disconnect(videoWidget, nullptr, this, nullptr);
        delete videoWidget;
        videoWidget = nullptr;
    }

    if(webrtcManager){

        webrtcManager->resetCursorCache();

        webrtcManager->post([webrtcManager = webrtcManager]()mutable{

             webrtcManager->disConnectHandle();

        });

    }

}

void MainWindow::onRemoteConnectionTimeout()
{
    hideNetworkBadge();
    // 超时必须强制重置 WebrtcManager 连接态(关 tcpSocket/peerConnection、停服务、重建空白 peerConnection),
    // 否则 tcpSocket 残留会导致下一次连接发起不成功。post 到 ioContext 执行,线程安全。
    if (webrtcManager) webrtcManager->abortPendingConnection();
    ui->btnStartControl->setEnabled(true);
    ui->btnStartControl->setText("立即连接");
    setRemoteStatus("连接请求超时", "danger");
    hidePeerCard();
}

void MainWindow::showPeerCard(int myRole, const QString& peerId)
{
    if (!ui || !ui->containerPeer || peerId.isEmpty()) return;
    ui->peerIdLabel->setText(peerId);
    if (myRole == 1) {
        ui->peerRoleBadge->setText("被控端");
        theme::setState(ui->peerRoleBadge, "role", QStringLiteral("controlled"));
    } else {
        ui->peerRoleBadge->setText("操控端");
        theme::setState(ui->peerRoleBadge, "role", QStringLiteral("controller"));
    }
    ui->containerPeer->setVisible(true);
}

void MainWindow::hidePeerCard()
{
    if (!ui || !ui->containerPeer) return;
    ui->containerPeer->setVisible(false);
    ui->peerIdLabel->setText("");
}

void MainWindow::setRemoteStatus(const QString& text, const char* state)
{
    ui->remoteStatusLabel->setText(text);
    ui->remoteStatusLabel->setVisible(!text.isEmpty());
    theme::setState(ui->remoteStatusLabel, "state", QString::fromLatin1(state));
}

void MainWindow::buildRecentPopup()
{
    recentPopup = new QFrame(this);
    recentPopup->setObjectName("recentPopup");
    recentPopup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    recentPopup->setAttribute(Qt::WA_StyledBackground, true);

    QVBoxLayout* popupLayout = new QVBoxLayout(recentPopup);
    popupLayout->setContentsMargins(6, 6, 6, 6);
    popupLayout->setSpacing(2);

    recentPopupList = new QListWidget(recentPopup);
    recentPopupList->setObjectName("recentPopupList");
    recentPopupList->setFrameShape(QFrame::NoFrame);
    recentPopupList->setFocusPolicy(Qt::NoFocus);
    recentPopupList->setIconSize(QSize(16, 16));
    recentPopupList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recentPopupList->setContextMenuPolicy(Qt::CustomContextMenu);
    popupLayout->addWidget(recentPopupList);

    labelPopupEmpty = new QLabel("暂无最近设备", recentPopup);
    labelPopupEmpty->setObjectName("labelPopupEmpty");
    labelPopupEmpty->setAlignment(Qt::AlignCenter);
    popupLayout->addWidget(labelPopupEmpty);

    QWidget* popupFooter = new QWidget(recentPopup);
    QHBoxLayout* footerLayout = new QHBoxLayout(popupFooter);
    footerLayout->setContentsMargins(8, 4, 8, 2);
    footerLayout->setSpacing(8);
    btnPopupAdd = new QPushButton("添加设备", popupFooter);
    btnPopupClear = new QPushButton("清空", popupFooter);
    for (QPushButton* button : { btnPopupAdd, btnPopupClear }) {
        button->setObjectName("btnPopupAction");
        button->setCursor(Qt::PointingHandCursor);
        button->setIconSize(QSize(14, 14));
    }
    btnPopupAdd->setIcon(theme::icon(QStringLiteral(":/icons/plus.svg"), theme::color("textFaint"), 14));
    btnPopupClear->setIcon(theme::icon(QStringLiteral(":/icons/trash.svg"), theme::color("textFaint"), 14));
    footerLayout->addWidget(btnPopupAdd);
    footerLayout->addStretch();
    footerLayout->addWidget(btnPopupClear);
    popupLayout->addWidget(popupFooter);

    connect(recentPopupList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        hideRecentPopup();
        onDeviceItemClicked(item);
    });
    connect(recentPopupList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showDeviceMenu(recentPopupList, pos, false);
    });
    connect(btnPopupAdd, &QPushButton::clicked, this, [this]() {
        hideRecentPopup();
        QTimer::singleShot(0, this, &MainWindow::onAddDeviceClicked);
    });
    connect(btnPopupClear, &QPushButton::clicked, this, [this]() {
        hideRecentPopup();
        QTimer::singleShot(0, this, &MainWindow::onClearHistoryClicked);
    });

    updateRecentListUI();
}

void MainWindow::showRecentPopup()
{
    if (!recentPopup || !ui->remoteIdEdit) return;
    updateRecentListUI();

    const int rowCount = recentPopupList->count();
    recentPopupList->setVisible(rowCount > 0);
    labelPopupEmpty->setVisible(rowCount == 0);
    recentPopupList->setFixedHeight(qBound(1, rowCount, 6) * 34 + 2);

    recentPopup->setFixedWidth(qMax(ui->remoteIdEdit->width(), 280));
    recentPopup->adjustSize();
    recentPopup->move(ui->remoteIdEdit->mapToGlobal(QPoint(0, ui->remoteIdEdit->height() + 6)));
    recentPopup->show();
    recentPopup->raise();
}

void MainWindow::hideRecentPopup()
{
    if (recentPopup && recentPopup->isVisible()) recentPopup->hide();
}

void MainWindow::updateStatusUI(const QString& status, const QString& styleClass)
{
    ui->connectionStatusLabel->setText(status);
    if (styleClass == "success") {
        theme::setState(ui->connectionStatusLabel, "state", QStringLiteral("success"));
    } else if (styleClass == "error") {
        theme::setState(ui->connectionStatusLabel, "state", QStringLiteral("error"));
    } else {
        theme::setState(ui->connectionStatusLabel, "state", QStringLiteral("normal"));
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
    hidePeerCard();
    hideNetworkBadge();
    setRemoteStatus("", "idle");

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
    webrtcEnableD3D11 = (state != Qt::Unchecked) ? 1 : 0;
    if (settings) settings->setValue("webrtcEnableNvdec", webrtcEnableD3D11);
    syncConfigToManager();
}


    } // namespace rtc
} // namespace hope
