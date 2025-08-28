#include "MainWindow.h"
#include "VideoWidget.h"
#include "webrtcremoteclient.h"
#include <QApplication>
#include <QCloseEvent>
#include <QSplitter>
#include <QFrame>
#include <QKeySequence>
#include <QScreen>
#include <QDebug>
#include <QInputDialog>
#include <QGraphicsDropShadowEffect>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , videoWidget(nullptr)
    , webRTCRemoteClient(nullptr)
    , centralWidget(nullptr)
    , mainLayout(nullptr)
    , connectionGroup(nullptr)
    , connectionLayout(nullptr)
    , serverAddressEdit(nullptr)
    , portSpinBox(nullptr)
    , accountLabel(nullptr)
    , accountComboBox(nullptr)
    , addAccountButton(nullptr)
    , removeAccountButton(nullptr)
    , connectButton(nullptr)
    , disconnectButton(nullptr)
    , connectionStatusLabel(nullptr)
    , targetGroup(nullptr)
    , targetLayout(nullptr)
    , targetListWidget(nullptr)
    , sendRequestButton(nullptr)
    , disconnectRemoteButton(nullptr)
    , targetStatusLabel(nullptr)
    , menuBars(nullptr)
    , aboutAction(nullptr)
    , settingsAction(nullptr)
    , fullScreenAction(nullptr)
    , exitAction(nullptr)
    , statusBars(nullptr)
    , statusLabel(nullptr)
    , fpsLabel(nullptr)
    , statusTimer(nullptr)
    , settings(nullptr)
    , isConnected(false)
    , isFullScreen(false)
    , isRemoteConnected(false)
{
    QIcon appIcon(":/logo/res/Wilson_DST.png");
    if (!appIcon.isNull()) {
        setWindowIcon(appIcon);
        QApplication::setWindowIcon(appIcon);
        qDebug() << "应用程序图标设置成功";
    } else {
        qDebug() << "警告：无法加载应用程序图标：:/logo/res/Wilson_DST.png";
    }

    // 设置窗口属性
    setWindowTitle("WebRTC视频客户端 - 控制面板");
    setMinimumSize(750, 850);
    resize(850, 900);

    // 初始化设置
    settings = new QSettings("WebRTCClient", "Settings", this);

    // 初始化账号列表
    loadAccounts();

    // 设置UI
    setupUI();
    setupMenuBar();
    setupStatusBar();
    setupConnections();
    applyModernStyles();

    // 加载设置
    loadSettings();

    // 创建WebRTC客户端，但不创建VideoWidget
    webRTCRemoteClient = new WebRTCRemoteClient(WebRTCRemoteState::nullRemote);

    webRTCRemoteClient->followRemoteHandle = ([this]() {
        // 使用Qt的事件系统确保在主线程中执行UI更新
        QMetaObject::invokeMethod(this, "onRemoteControlStarted",
                                  Qt::QueuedConnection);
    });

    // 注册被远程端断开连接的回调
    webRTCRemoteClient->disConnectRemoteHandle = ([this]() {
        // 使用Qt的事件系统确保在主线程中执行UI更新
        QMetaObject::invokeMethod(this, "onRemoteDisconnectedByPeer",
                                  Qt::QueuedConnection);
    });

    // 初始化状态
    updateConnectionState(false);

    qDebug() << "MainWindow initialized";
}

MainWindow::~MainWindow()
{
    saveSettings();
    saveAccounts();

    // 关闭视频窗口
    if (videoWidget) {
        videoWidget->close();
        delete videoWidget;
        videoWidget = nullptr;
    }

    if (webRTCRemoteClient) {
        delete webRTCRemoteClient;
        webRTCRemoteClient = nullptr;
    }
}

QString MainWindow::createButtonStyle(const QString& bgColor, const QString& hoverColor, const QString& textColor)
{
    return QString(R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %1, stop:1 %2);
            color: %3;
            border: none;
            border-radius: 8px;
            padding: 4px 8px;
            font-size: 14px;
            font-weight: 600;
            min-height: 36px;
            min-width: 90px;
            margin: 2px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %4, stop:1 %1);
            transform: translateY(-1px);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %2, stop:1 %1);
            padding-top: 5px;
            padding-bottom: 4px;
        }
        QPushButton:disabled {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #F5F7FA, stop:1 #E4E7ED);
            color: #C0C4CC;
        }
    )").arg(bgColor, QString("#%1").arg(QColor(bgColor).darker(110).name().mid(1)), textColor, hoverColor);
}

QString MainWindow::createGroupBoxStyle()
{
    return R"(
        QGroupBox {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #FAFBFC);
            border: 2px solid #E1E6EB;
            border-radius: 12px;
            padding-top: 4px;
            margin-top: 10px;
            margin-bottom: 15px;
            font-size: 15px;
            font-weight: 700;
            color: #2C3E50;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 20px;
            padding: 4px 5px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #667EEA, stop:1 #764BA2);
            color: white;
            border-radius: 6px;
            font-weight: 600;
        }
    )";
}

QString MainWindow::createInputStyle()
{
    return R"(
        QLineEdit, QSpinBox, QComboBox {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F8F9FA);
            border: 2px solid #E1E6EB;
            border-radius: 8px;
            padding: 4px 6px;
            font-size: 14px;
            color: #2C3E50;
            selection-background-color: #667EEA;
            min-height: 18px;
            margin: 2px;
        }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
            border-color: #667EEA;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F0F4FF);
            outline: none;
        }
        QLineEdit:hover, QSpinBox:hover, QComboBox:hover {
            border-color: #B0BEC5;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F5F7FA);
        }
        QComboBox::drop-down {
            border: none;
            width: 25px;
            background: transparent;
        }
        QComboBox::down-arrow {
            image: none;
            border-left: 5px solid transparent;
            border-right: 5px solid transparent;
            border-top: 7px solid #667EEA;
            margin-right: 8px;
        }
        QComboBox QAbstractItemView {
            background-color: white;
            border: 2px solid #E1E6EB;
            border-radius: 8px;
            selection-background-color: #F0F4FF;
            color: #2C3E50;
        }
    )";
}

QString MainWindow::createListWidgetStyle()
{
    return R"(
        QListWidget {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F8F9FA);
            border: 2px solid #E1E6EB;
            border-radius: 10px;
            padding: 4px;
            font-size: 14px;
            color: #2C3E50;
            outline: none;
            margin: 5px 0;
        }
        QListWidget::item {
            padding: 5px 7px;
            border-radius: 8px;
            margin: 2px 0;
            background: transparent;
            border: 1px solid transparent;
        }
        QListWidget::item:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #F0F4FF, stop:1 #E8F0FE);
            border: 1px solid #B3D4FC;
        }
        QListWidget::item:selected {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #667EEA, stop:1 #764BA2);
            color: white;
            border: 1px solid #5A67D8;
        }
        QListWidget:focus {
            border-color: #667EEA;
        }
    )";
}

QString MainWindow::createStatusLabelStyle(const QString& type)
{
    if (type == "success") {
        return R"(
            QLabel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #48BB78, stop:1 #38A169);
                color: white;
                border: none;
                border-radius: 10px;
                padding: 5px 8px;
                font-size: 14px;
                font-weight: 600;
                min-height: 18px;
                margin: 5px 0;
            }
        )";
    } else if (type == "error") {
        return R"(
            QLabel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #F56565, stop:1 #E53E3E);
                color: white;
                border: none;
                border-radius: 10px;
                padding: 5px 8px;
                font-size: 14px;
                font-weight: 600;
                min-height: 18px;
                margin: 5px 0;
            }
        )";
    } else if (type == "warning") {
        return R"(
            QLabel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ED8936, stop:1 #DD6B20);
                color: white;
                border: none;
                border-radius: 10px;
                padding: 5px 8px;
                font-size: 14px;
                font-weight: 600;
                min-height: 18px;
                margin: 5px 0;
            }
        )";
    } else if (type == "info") {
        return R"(
            QLabel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #4299E1, stop:1 #3182CE);
                color: white;
                border: none;
                border-radius: 10px;
                padding: 5px 8px;
                font-size: 14px;
                font-weight: 600;
                min-height: 18px;
                margin: 5px 0;
            }
        )";
    } else { // default/neutral
        return R"(
            QLabel {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #A0AEC0, stop:1 #718096);
                color: white;
                border: none;
                border-radius: 10px;
                padding: 5px 8px;
                font-size: 14px;
                font-weight: 600;
                min-height: 18px;
                margin: 5px 0;
            }
        )";
    }
}

QString MainWindow::createRegularLabelStyle()
{
    return R"(
        QLabel {
            color: #2D3748;
            font-size: 14px;
            font-weight: 600;
            background: transparent;
            padding: 6px 2px;
            min-width: 75px;
        }
    )";
}

void MainWindow::applyModernStyles()
{
    // 设置主窗口背景
    centralWidget->setStyleSheet(R"(
        QWidget {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #F7FAFC, stop:1 #EDF2F7);
            font-family: "Microsoft YaHei UI", "Segoe UI", "Helvetica Neue", Helvetica, Arial, sans-serif;
        }
    )");

    // 应用组框样式
    connectionGroup->setStyleSheet(createGroupBoxStyle());
    targetGroup->setStyleSheet(createGroupBoxStyle());

    // 应用输入框样式
    serverAddressEdit->setStyleSheet(createInputStyle());
    portSpinBox->setStyleSheet(createInputStyle());
    accountComboBox->setStyleSheet(createInputStyle());

    // 应用按钮样式
    connectButton->setStyleSheet(createButtonStyle("#667EEA", "#5A67D8"));
    disconnectButton->setStyleSheet(createButtonStyle("#F56565", "#E53E3E"));
    addAccountButton->setStyleSheet(createButtonStyle("#48BB78", "#38A169"));
    removeAccountButton->setStyleSheet(createButtonStyle("#ED8936", "#DD6B20"));
    sendRequestButton->setStyleSheet(createButtonStyle("#667EEA", "#5A67D8"));
    disconnectRemoteButton->setStyleSheet(createButtonStyle("#F56565", "#E53E3E"));
    showVideoButton->setStyleSheet(createButtonStyle("#4299E1", "#3182CE"));
    hideVideoButton->setStyleSheet(createButtonStyle("#A0AEC0", "#718096"));

    // 应用列表样式
    targetListWidget->setStyleSheet(createListWidgetStyle());

    // 普通标签样式
    accountLabel->setStyleSheet(createRegularLabelStyle());

    // 状态标签初始样式
    targetStatusLabel->setStyleSheet(createStatusLabelStyle("default"));

    // 为组框添加阴影效果
    auto addShadow = [](QWidget* widget) {
        QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect;
        shadow->setBlurRadius(20);
        shadow->setXOffset(0);
        shadow->setYOffset(5);
        shadow->setColor(QColor(0, 0, 0, 40));
        widget->setGraphicsEffect(shadow);
    };

    addShadow(connectionGroup);
    addShadow(targetGroup);
}

void MainWindow::setupUI()
{
    // 创建中央窗口部件
    centralWidget = new QWidget;
    setCentralWidget(centralWidget);

    // 创建主布局 - 适当的间距
    mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(25);  // 组件之间的垂直间距
    mainLayout->setContentsMargins(30, 30, 30, 30);  // 适中的边距

    // 创建连接控制面板
    connectionGroup = new QGroupBox("连接设置");
    connectionLayout = new QGridLayout(connectionGroup);
    connectionLayout->setSpacing(18);  // 网格间距
    connectionLayout->setContentsMargins(25, 35, 25, 25);  // 减少内边距

    // 服务器地址输入 - 创建独立的标签并应用样式
    QLabel* serverLabel = new QLabel("服务器地址:");
    serverLabel->setStyleSheet(createRegularLabelStyle());
    connectionLayout->addWidget(serverLabel, 0, 0);

    serverAddressEdit = new QLineEdit();
    serverAddressEdit->setPlaceholderText("输入服务器IP地址，例如：localhost");
    serverAddressEdit->setText("localhost");
    connectionLayout->addWidget(serverAddressEdit, 0, 1);

    // 端口输入 - 创建独立的标签并应用样式
    QLabel* portLabel = new QLabel("端口:");
    portLabel->setStyleSheet(createRegularLabelStyle());
    connectionLayout->addWidget(portLabel, 0, 2);

    portSpinBox = new QSpinBox();
    portSpinBox->setRange(1, 65535);
    portSpinBox->setValue(8080);
    connectionLayout->addWidget(portSpinBox, 0, 3);

    // 账号选择
    accountLabel = new QLabel("账号:");
    accountLabel->setStyleSheet(createRegularLabelStyle());
    connectionLayout->addWidget(accountLabel, 1, 0);

    accountComboBox = new QComboBox();
    accountComboBox->setEditable(false);
    accountComboBox->addItems(accountList);
    if (accountList.isEmpty()) {
        accountComboBox->addItem("请添加账号");
    }
    connectionLayout->addWidget(accountComboBox, 1, 1);

    // 账号管理按钮
    addAccountButton = new QPushButton("添加账号");
    addAccountButton->setCursor(Qt::PointingHandCursor);
    connectionLayout->addWidget(addAccountButton, 1, 2);

    removeAccountButton = new QPushButton("删除账号");
    removeAccountButton->setCursor(Qt::PointingHandCursor);
    removeAccountButton->setEnabled(accountComboBox->count() > 0 && !accountList.isEmpty());
    connectionLayout->addWidget(removeAccountButton, 1, 3);

    // 添加一行间距
    connectionLayout->setRowMinimumHeight(2, 10);

    // 连接按钮
    connectButton = new QPushButton("连接");
    connectButton->setCursor(Qt::PointingHandCursor);
    connectButton->setEnabled(!accountList.isEmpty());
    connectionLayout->addWidget(connectButton, 3, 0, 1, 2);

    // 断开连接按钮
    disconnectButton = new QPushButton("断开");
    disconnectButton->setCursor(Qt::PointingHandCursor);
    disconnectButton->setEnabled(false);
    connectionLayout->addWidget(disconnectButton, 3, 2, 1, 2);

    // 添加一行间距
    connectionLayout->setRowMinimumHeight(4, 10);

    // 连接状态标签
    connectionStatusLabel = new QLabel("未连接");
    connectionStatusLabel->setAlignment(Qt::AlignCenter);
    connectionLayout->addWidget(connectionStatusLabel, 5, 0, 1, 4);

    // 设置列拉伸
    connectionLayout->setColumnStretch(1, 1);

    // 创建目标账号面板
    targetGroup = new QGroupBox("远程控制");
    targetLayout = new QVBoxLayout(targetGroup);
    targetLayout->setSpacing(18);  // 组件间距
    targetLayout->setContentsMargins(25, 35, 25, 25);  // 减少内边距

    targetListWidget = new QListWidget();
    targetListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    targetListWidget->setMinimumHeight(180);  // 适中的高度
    targetLayout->addWidget(targetListWidget);

    // 创建按钮容器
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(12);  // 按钮间距

    sendRequestButton = new QPushButton("发送请求");
    sendRequestButton->setCursor(Qt::PointingHandCursor);
    sendRequestButton->setEnabled(false);
    buttonLayout->addWidget(sendRequestButton);

    disconnectRemoteButton = new QPushButton("断开远程操控");
    disconnectRemoteButton->setCursor(Qt::PointingHandCursor);
    disconnectRemoteButton->setEnabled(false);
    buttonLayout->addWidget(disconnectRemoteButton);

    targetLayout->addLayout(buttonLayout);

    targetStatusLabel = new QLabel("请选择目标账号");
    targetStatusLabel->setWordWrap(true);
    targetStatusLabel->setAlignment(Qt::AlignCenter);
    targetLayout->addWidget(targetStatusLabel);

    // 添加视频窗口控制按钮
    QGroupBox* videoControlGroup = new QGroupBox("视频窗口控制");
    videoControlGroup->setStyleSheet(createGroupBoxStyle());

    QHBoxLayout* videoControlLayout = new QHBoxLayout(videoControlGroup);
    videoControlLayout->setSpacing(12);  // 按钮间距
    videoControlLayout->setContentsMargins(25, 35, 25, 25);  // 减少内边距

    showVideoButton = new QPushButton("显示视频窗口");
    showVideoButton->setCursor(Qt::PointingHandCursor);
    showVideoButton->setEnabled(false);
    connect(showVideoButton, &QPushButton::clicked, this, &MainWindow::onShowVideoWindow);
    videoControlLayout->addWidget(showVideoButton);

    hideVideoButton = new QPushButton("隐藏视频窗口");
    hideVideoButton->setCursor(Qt::PointingHandCursor);
    hideVideoButton->setEnabled(false);
    connect(hideVideoButton, &QPushButton::clicked, this, &MainWindow::onHideVideoWindow);
    videoControlLayout->addWidget(hideVideoButton);

    // 为视频控制组添加阴影
    QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect;
    shadow->setBlurRadius(20);
    shadow->setXOffset(0);
    shadow->setYOffset(5);
    shadow->setColor(QColor(0, 0, 0, 40));
    videoControlGroup->setGraphicsEffect(shadow);

    // 添加到主布局
    mainLayout->addWidget(connectionGroup);
    mainLayout->addWidget(targetGroup);
    mainLayout->addWidget(videoControlGroup);
    mainLayout->addStretch();  // 添加弹性空间
}

void MainWindow::setupMenuBar()
{
    menuBars = this->menuBar();

    // 文件菜单
    QMenu* fileMenu = menuBars->addMenu("文件(&F)");

    settingsAction = new QAction("设置(&S)", this);
    settingsAction->setShortcut(QKeySequence::Preferences);
    fileMenu->addAction(settingsAction);

    fileMenu->addSeparator();

    exitAction = new QAction("退出(&X)", this);
    exitAction->setShortcut(QKeySequence::Quit);
    fileMenu->addAction(exitAction);

    // 视图菜单
    QMenu* viewMenu = menuBars->addMenu("视图(&V)");

    fullScreenAction = new QAction("视频窗口全屏(&F)", this);
    fullScreenAction->setShortcut(QKeySequence::FullScreen);
    fullScreenAction->setCheckable(true);
    viewMenu->addAction(fullScreenAction);

    // 帮助菜单
    QMenu* helpMenu = menuBars->addMenu("帮助(&H)");

    aboutAction = new QAction("关于(&A)", this);
    helpMenu->addAction(aboutAction);
}

void MainWindow::setupStatusBar()
{
    statusBars = this->statusBar();
    statusBars->setStyleSheet(R"(
        QStatusBar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F7FAFC);
            border-top: 1px solid #E2E8F0;
            font-size: 13px;
            color: #4A5568;
            min-height: 26px;
        }
        QStatusBar QLabel {
            background: transparent;
            padding: 4px 8px;
            font-weight: 500;
        }
    )");

    statusLabel = new QLabel("就绪");
    statusBars->addWidget(statusLabel);

    statusBars->addPermanentWidget(new QFrame());

    fpsLabel = new QLabel("FPS: 0.0");
    fpsLabel->setStyleSheet(R"(
        QLabel {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #667EEA, stop:1 #764BA2);
            color: white;
            border-radius: 8px;
            padding: 4px 10px;
            font-weight: 600;
            font-size: 12px;
        }
    )");
    statusBars->addPermanentWidget(fpsLabel);

    // 状态更新计时器
    statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    statusTimer->start(1000);
}

void MainWindow::setupConnections()
{
    // 连接按钮
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);

    // 账号管理
    connect(accountComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onAccountChanged);
    connect(addAccountButton, &QPushButton::clicked, this, &MainWindow::onAddAccountClicked);
    connect(removeAccountButton, &QPushButton::clicked, this, &MainWindow::onRemoveAccountClicked);

    // 目标账号管理
    connect(targetListWidget, &QListWidget::itemSelectionChanged,
            this, &MainWindow::onTargetSelectionChanged);
    connect(sendRequestButton, &QPushButton::clicked, this, &MainWindow::onSendRequestClicked);

    // 断开远程操控按钮
    connect(disconnectRemoteButton, &QPushButton::clicked, this, &MainWindow::onDisconnectRemoteControl);

    // 回车键连接
    connect(serverAddressEdit, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);

    // 菜单操作
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettings);
    connect(fullScreenAction, &QAction::triggered, this, &MainWindow::onFullScreen);
    connect(exitAction, &QAction::triggered, this, &QMainWindow::close);
}

void MainWindow::loadSettings()
{
    // 恢复窗口几何
    restoreGeometry(settings->value("geometry").toByteArray());
    restoreState(settings->value("windowState").toByteArray());

    // 恢复连接设置
    serverAddressEdit->setText(settings->value("serverAddress", "localhost").toString());
    portSpinBox->setValue(settings->value("port", 8080).toInt());

    // 恢复选中的账号
    QString lastAccount = settings->value("lastAccount", "").toString();
    if (!lastAccount.isEmpty()) {
        int index = accountComboBox->findText(lastAccount);
        if (index >= 0) {
            accountComboBox->setCurrentIndex(index);
        }
    }
}

void MainWindow::saveSettings()
{
    // 保存窗口几何
    settings->setValue("geometry", saveGeometry());
    settings->setValue("windowState", saveState());

    // 保存连接设置
    settings->setValue("serverAddress", serverAddressEdit->text());
    settings->setValue("port", portSpinBox->value());

    // 保存当前账号
    if (accountComboBox->currentIndex() >= 0 && !accountList.isEmpty()) {
        settings->setValue("lastAccount", accountComboBox->currentText());
    }

    // 保存视频窗口设置
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

    // 如果没有账号，添加一个默认的
    if (accountList.isEmpty()) {
        accountList << "913140924@qq.com";
        accountList << "2044580040@qq.com";
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

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();

    // 关闭视频窗口
    if (videoWidget) {
        videoWidget->close();
    }

    event->accept();
}

void MainWindow::onConnectClicked()
{
    if (isConnected || accountList.isEmpty()) {
        return;
    }

    QString serverAddress = serverAddressEdit->text().trimmed();
    int port = portSpinBox->value();
    QString currentAccount = accountComboBox->currentText();

    if (serverAddress.isEmpty()) {
        showErrorMessage("连接错误", "请输入服务器地址");
        return;
    }

    if (currentAccount.isEmpty() || currentAccount == "请添加账号") {
        showErrorMessage("连接错误", "请先添加并选择一个账号");
        return;
    }

    // 设置账号ID
    webRTCRemoteClient->setAccountID(currentAccount.toStdString());

    connectButton->setEnabled(false);
    connectButton->setText("连接中...");
    connectionStatusLabel->setText("正在连接...");
    connectionStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    // 异步连接
    QTimer::singleShot(100, [this, serverAddress, port]() {
        QString url = QString("%1:%2").arg(serverAddress).arg(port);
        this->webRTCRemoteClient->connect(url.toStdString());
        // 模拟连接成功
        QTimer::singleShot(1000, [this]() {
            this->onConnectionStateChanged(true);
        });
    });
}

void MainWindow::onDisconnectClicked()
{
    if (!isConnected) {
        return;
    }

    webRTCRemoteClient->disConnect();
    onConnectionStateChanged(false);

    // 断开连接时关闭视频窗口
    if (videoWidget) {
        videoWidget->close();
        delete videoWidget;
        videoWidget = nullptr;
    }

    // 禁用视频控制按钮
    showVideoButton->setEnabled(false);
    hideVideoButton->setEnabled(false);
    disconnectRemoteButton->setEnabled(false);
    isRemoteConnected = false;
}

void MainWindow::onRemoteControlStarted()
{
    qDebug() << "Remote control started - someone is controlling this machine";

    // 更新远程连接状态
    isRemoteConnected = true;

    // 启用断开按钮，让被操控端可以主动断开
    disconnectRemoteButton->setEnabled(true);
    disconnectRemoteButton->setText("断开被控制");

    // 禁用发送请求按钮（正在被控制时不能发起新的控制请求）
    sendRequestButton->setEnabled(false);

    // 更新状态标签
    targetStatusLabel->setText("正在被远程控制中");
    targetStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    // 如果有视频窗口，可能需要显示一些提示
    if (videoWidget) {
        videoWidget->setWindowTitle("WebRTC远程桌面 - 正在被控制");
    }

    // 更新状态栏
    statusLabel->setText("正在被远程控制");
}

void MainWindow::onRemoteDisconnectedByPeer()
{
    qDebug() << "Remote connection closed by peer";

    // 更新远程连接状态
    isRemoteConnected = false;

    // 恢复按钮状态
    disconnectRemoteButton->setEnabled(false);
    disconnectRemoteButton->setText("断开远程操控");
    sendRequestButton->setEnabled(targetListWidget->currentItem() != nullptr);

    // 更新状态标签
    targetStatusLabel->setText("远程连接已被对方断开");
    targetStatusLabel->setStyleSheet(createStatusLabelStyle("info"));

    // 处理视频窗口
    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
        videoWidget->setWindowTitle("WebRTC远程桌面");
        showVideoButton->setEnabled(false);
        hideVideoButton->setEnabled(false);
    }

    // 更新状态栏
    statusLabel->setText("远程连接已断开");
}

void MainWindow::onDisconnectRemoteControl()
{
    // 根据按钮文本判断当前是控制方还是被控制方
    bool isBeingControlled = (disconnectRemoteButton->text() == "断开被控制");

    if (isBeingControlled) {
        qDebug() << "User (controlled side) initiated disconnect";

        // 被控制方主动断开
        if (webRTCRemoteClient) {
            webRTCRemoteClient->disConnectRemote();
        }

        targetStatusLabel->setText("已主动断开被控制");
        targetStatusLabel->setStyleSheet(createStatusLabelStyle("success"));

        // 恢复按钮文本
        disconnectRemoteButton->setText("断开远程操控");

        // 恢复可以发起控制请求
        sendRequestButton->setEnabled(targetListWidget->currentItem() != nullptr);
    } else {
        qDebug() << "User (controller side) initiated disconnect";

        // 控制方主动断开
        if (webRTCRemoteClient) {
            webRTCRemoteClient->disConnectRemote();
        }

        targetStatusLabel->setText("已主动断开远程操控");
        targetStatusLabel->setStyleSheet(createStatusLabelStyle("info"));
    }

    // 更新UI状态
    isRemoteConnected = false;
    disconnectRemoteButton->setEnabled(false);

    // 处理视频窗口
    if (videoWidget) {
        videoWidget->clearDisplay();
        videoWidget->hide();
        videoWidget->setWindowTitle("WebRTC远程桌面");
        showVideoButton->setEnabled(false);
        hideVideoButton->setEnabled(false);
    }

    // 更新状态栏
    statusLabel->setText("已断开远程连接");
}

void MainWindow::onAccountChanged(int index)
{
    Q_UNUSED(index)
    bool hasValidAccount = !accountList.isEmpty() &&
                           accountComboBox->currentText() != "请添加账号";
    connectButton->setEnabled(hasValidAccount && !isConnected);
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
                if (accountComboBox->count() == 1 &&
                    accountComboBox->itemText(0) == "请添加账号") {
                    accountComboBox->clear();
                }
                accountComboBox->addItem(email);
                accountComboBox->setCurrentText(email);

                removeAccountButton->setEnabled(true);
                connectButton->setEnabled(!isConnected);

                saveAccounts();
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

    QString currentAccount = accountComboBox->currentText();
    if (currentAccount.isEmpty() || currentAccount == "请添加账号") {
        return;
    }

    int ret = QMessageBox::question(this, "确认删除",
                                    QString("确定要删除账号 %1 吗？").arg(currentAccount),
                                    QMessageBox::Yes | QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        accountList.removeOne(currentAccount);
        accountComboBox->removeItem(accountComboBox->currentIndex());

        if (accountList.isEmpty()) {
            accountComboBox->clear();
            accountComboBox->addItem("请添加账号");
            removeAccountButton->setEnabled(false);
            connectButton->setEnabled(false);
        }

        saveAccounts();
    }
}

void MainWindow::onTargetSelectionChanged()
{
    bool hasSelection = targetListWidget->currentItem() != nullptr;
    sendRequestButton->setEnabled(hasSelection);

    if (hasSelection) {
        QString targetId = targetListWidget->currentItem()->text();
        targetStatusLabel->setText(QString("已选择: %1").arg(targetId));
        targetStatusLabel->setStyleSheet(createStatusLabelStyle("info"));
        webRTCRemoteClient->setTargetID(targetId.toStdString());
    } else {
        targetStatusLabel->setText("请选择目标账号");
        targetStatusLabel->setStyleSheet(createStatusLabelStyle("default"));
    }
}

void MainWindow::onSendRequestClicked()
{
    if (!isConnected || !targetListWidget->currentItem()) {
        return;
    }

    QString targetId = targetListWidget->currentItem()->text();
    targetStatusLabel->setText(QString("正在向 %1 发送请求...").arg(targetId));
    targetStatusLabel->setStyleSheet(createStatusLabelStyle("warning"));

    // 调用WebRTC客户端发送请求
    webRTCRemoteClient->sendRequestToTarget();

    // 创建VideoWidget（如果还没有创建）
    if (!videoWidget) {
        videoWidget = new VideoWidget();
        videoWidget->setWindowTitle("WebRTC远程桌面");

        // ===== 明确设置窗口标志，强制普通窗口模式 =====
        videoWidget->setWindowFlags(Qt::Window |
                                    Qt::WindowTitleHint |
                                    Qt::WindowSystemMenuHint |
                                    Qt::WindowMinMaxButtonsHint |
                                    Qt::WindowCloseButtonHint);

        // 确保不是全屏状态
        videoWidget->setWindowState(Qt::WindowNoState);

        videoWidget->setMinimumSize(640, 480);

        // 设置窗口位置（居中显示）
        QScreen* screen = QApplication::primaryScreen();
        if (screen) {
            QRect screenGeometry = screen->geometry();
            int x = (screenGeometry.width() - 1280) / 2;
            int y = (screenGeometry.height() - 720) / 2;
            videoWidget->move(x, y);
        }

        // 设置WebRTC客户端
        videoWidget->webRTCRemoteClient = webRTCRemoteClient;

        // 设置回调
        webRTCRemoteClient->setVideoFrameCallback([this](std::shared_ptr<VideoFrame> frame) {
            if (videoWidget) {
                videoWidget->displayFrame(frame);
            }
        });

        // 恢复视频窗口设置
        if (settings->contains("videoWindowGeometry")) {
            videoWidget->restoreGeometry(settings->value("videoWindowGeometry").toByteArray());
        }

        // 连接VideoWidget的关闭信号
        connect(videoWidget, &QWidget::destroyed, this, [this]() {
            videoWidget = nullptr;
            showVideoButton->setEnabled(false);
            hideVideoButton->setEnabled(false);
            disconnectRemoteButton->setEnabled(false);
        });
    }

    // ===== 显示为普通窗口，绝对不全屏 =====
    videoWidget->showMaximized();  // 明确使用 showNormal() 而不是 show()
    videoWidget->raise();
    videoWidget->activateWindow();

    // 启用视频控制按钮
    showVideoButton->setEnabled(true);
    hideVideoButton->setEnabled(true);
    isRemoteConnected = true;
    disconnectRemoteButton->setEnabled(true);

    // 模拟请求发送完成
    QTimer::singleShot(1000, [this, targetId]() {
        targetStatusLabel->setText(QString("已连接到 %1").arg(targetId));
        targetStatusLabel->setStyleSheet(createStatusLabelStyle("success"));
    });
}

void MainWindow::onConnectionStateChanged(bool connected)
{
    isConnected = connected;
    updateConnectionState(connected);

    if (connected) {
        statusLabel->setText("已连接到服务器");
        updateTargetList();
    } else {
        statusLabel->setText("连接已断开");
        targetListWidget->clear();

        // 断开连接时清理视频窗口
        if (videoWidget) {
            videoWidget->clearDisplay();
        }
    }
}

void MainWindow::onClientError(const QString& error)
{
    lastError = error;
    statusLabel->setText(QString("错误: %1").arg(error));

    if (isConnected) {
        showErrorMessage("连接错误", error);
    }

    updateConnectionState(false);
}

void MainWindow::updateConnectionState(bool connected)
{
    isConnected = connected;

    connectButton->setEnabled(!connected && !accountList.isEmpty() &&
                              accountComboBox->currentText() != "请添加账号");
    connectButton->setText("连接");
    disconnectButton->setEnabled(connected);

    // 连接时禁用账号选择
    accountComboBox->setEnabled(!connected);
    addAccountButton->setEnabled(!connected);
    removeAccountButton->setEnabled(!connected && !accountList.isEmpty());

    if (connected) {
        connectionStatusLabel->setText(QString("已连接 (账号: %1)").arg(accountComboBox->currentText()));
        connectionStatusLabel->setStyleSheet(createStatusLabelStyle("success"));
    } else {
        connectionStatusLabel->setText("未连接");
        connectionStatusLabel->setStyleSheet(createStatusLabelStyle("error"));
    }
}

void MainWindow::updateTargetList()
{
    // 模拟获取在线账号列表
    targetList.clear();
    targetList << "913140924@qq.com"
               << "2044580040@qq.com"
               << "2512647272@qq.com";

    // 排除自己
    QString currentAccount = accountComboBox->currentText();
    targetList.removeOne(currentAccount);

    // 更新列表
    targetListWidget->clear();
    targetListWidget->addItems(targetList);
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "关于",
                       "<h3>WebRTC视频客户端</h3>"
                       "<p>版本 1.0</p>"
                       "<p>基于libdatachannel和Qt开发的WebRTC视频通信客户端</p>"
                       "<p>支持多账号管理和点对点视频传输</p>"
                       "<br>"
                       "<p>使用的技术：</p>"
                       "<ul>"
                       "<li>libdatachannel - WebRTC通信</li>"
                       "<li>OpenH264 - 视频编解码</li>"
                       "<li>Qt6 - 用户界面</li>"
                       "</ul>");
}

void MainWindow::onSettings()
{
    QMessageBox::information(this, "设置", "设置功能开发中...");
}

void MainWindow::onFullScreen()
{
    if (!videoWidget) {
        return;
    }

    if (isFullScreen) {
        // 退出全屏
        isFullScreen = false;
        fullScreenAction->setText("视频窗口全屏(&F)");
        fullScreenAction->setChecked(false);
        videoWidget->showMaximized(); // 显示为普通窗口
        videoWidget->raise();
        videoWidget->activateWindow();
    } else {
        // 进入全屏
        isFullScreen = true;
        fullScreenAction->setText("退出全屏(&F)");
        fullScreenAction->setChecked(true);
        videoWidget->showFullScreen(); // 改为 showFullScreen() 而不是 show()
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

void MainWindow::updateStatus()
{
    if (videoWidget) {
        double fps = videoWidget->getFrameRate();
        fpsLabel->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
    }
}

void MainWindow::showErrorMessage(const QString& title, const QString& message)
{
    QMessageBox::warning(this, title, message);
}
