QT += core gui widgets gui-private
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets
CONFIG += c++20 no_keywords

# 编译优化配置
CONFIG += release
CONFIG -= debug debug_and_release

# MSVC编译器优化标志 - 稳定版本
win32-msvc* {
    # 基础优化
    QMAKE_CXXFLAGS_RELEASE += /O2        # 最大化速度优化
    QMAKE_CXXFLAGS_RELEASE += /Ob2       # 内联函数展开
    QMAKE_CXXFLAGS_RELEASE += /Oi        # 启用内联函数
    QMAKE_CXXFLAGS_RELEASE += /Ot        # 偏向速度优化
    # 移除可能导致问题的优化选项
     QMAKE_CXXFLAGS_RELEASE += /GL      # 全程序优化 - 暂时禁用
     QMAKE_CXXFLAGS_RELEASE += /arch:AVX2 # AVX2指令集 - 暂时禁用
    # 链接器基础优化
    QMAKE_LFLAGS_RELEASE += /OPT:REF     # 移除未引用的函数和数据
    QMAKE_LFLAGS_RELEASE += /OPT:ICF     # 相同函数折叠
    QMAKE_LFLAGS_RELEASE += /LTCG      # 链接时代码生成 - 暂时禁用
    QMAKE_CXXFLAGS_RELEASE += /fp:fast
    # 多核编译
    QMAKE_CXXFLAGS += /MP                # 启用多处理器编译
    # 内存对齐 (提升SIMD效率)
    QMAKE_CXXFLAGS_RELEASE += /Zc:alignedNew
    # UTF-8支持
    QMAKE_CXXFLAGS += /utf-8
    QMAKE_CXXFLAGS += /arch:AVX2
}

# 禁用过时API警告(可选，取消注释启用)
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000
QMAKE_PROJECT_DEPTH = 0

# 源文件
SOURCES += \
    Logger.cpp \
    Utils.cpp \
    main.cpp \
    mainwindow.cpp \
    videowidget.cpp \
    webrtcremoteclient.cpp \
    windowshook.cpp

# 头文件
HEADERS += \
    Logger.h \
    Utils.h \
    WindowsServiceManager.h \
    mainwindow.h \
    videowidget.h \
    webrtcremoteclient.h \
    windowshook.h

# Windows特定配置
win32 {
    # 包含路径
    INCLUDEPATH += $$PWD/include/boost
    INCLUDEPATH += $$PWD/include/webrtc

    # 库路径
    LIBS += -L$$PWD/lib/boost/
    LIBS += -L$$PWD/lib/webrtc/

    # WebRTC相关库
    LIBS += -lwebrtc

    # Windows系统库
    LIBS += -lws2_32          # Windows Socket 2.0
    LIBS += -lmswsock         # Microsoft Winsock 2.0
    LIBS += -lwtsapi32        # Windows Terminal Services API
    LIBS += -lgdi32
    # Windows图形和多媒体库
    LIBS += -lwindowscodecs   # Windows Imaging Component
    LIBS += -lgdiplus         # GDI+
    LIBS += -ldwmapi          # Desktop Window Manager API
    LIBS += -lwindowsapp      # Windows Runtime
    LIBS += -lruntimeobject   # Windows Runtime Object

    # Windows系统服务库
    LIBS += -luserenv         # User Environment
    LIBS += -ladvapi32        # Advanced Windows API
    LIBS += -lwinmm           # Windows Multimedia

    # 媒体相关库
    LIBS += -lmsdmo           # Microsoft DirectShow Media Objects
    LIBS += -ldmoguids        # DirectShow Media Object GUIDs
    LIBS += -lwmcodecdspuuid  # Windows Media Codec DSP UUIDs
    LIBS += -lstrmiids        # DirectShow Stream Interface IDs
    LIBS += -lmfuuid          # Media Foundation UUIDs

    # Windows版本定义
    DEFINES += WIN32_LEAN_AND_MEAN
    DEFINES += NOMINMAX

    # 明确指定目标名称
    TARGET = WebRTCRemoteControl
}

# 部署规则
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# 发布版本优化选项
release {
    DEFINES += QT_NO_DEBUG_OUTPUT    # 移除调试输出
    DEFINES += QT_NO_WARNING_OUTPUT  # 移除警告输出
    DEFINES += NDEBUG                # 标准发布版本定义
}

# 版本信息
VERSION = 1.0.0
QMAKE_TARGET_DESCRIPTION = "WebRTC Video Application"
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2025"

RESOURCES += \
    res.qrc

DISTFILES += \
    config.ini
