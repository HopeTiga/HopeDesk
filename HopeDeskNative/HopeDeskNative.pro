QT += core gui widgets gui-private
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets
CONFIG += c++20 no_keywords


CONFIG += release
CONFIG -= debug debug_and_release

RC_ICONS = hope.ico

TCMALLOC_AGGRESSIVE_DECOMMIT = true
TCMALLOC_RELEASE_RATE = 10.0


win32-msvc* {

    QMAKE_CXXFLAGS_RELEASE += /Ox
    QMAKE_CXXFLAGS_RELEASE += /Ob2
    QMAKE_CXXFLAGS_RELEASE += /Oi
    QMAKE_CXXFLAGS_RELEASE += /Ot

    QMAKE_CXXFLAGS_RELEASE += /GL
    QMAKE_CXXFLAGS_RELEASE += /arch:AVX2
    QMAKE_LFLAGS_RELEASE += /OPT:REF
    QMAKE_LFLAGS_RELEASE += /OPT:ICF
    QMAKE_LFLAGS_RELEASE += /LTCG
    QMAKE_CXXFLAGS_RELEASE += /fp:fast
    QMAKE_CXXFLAGS += /MP
    QMAKE_CXXFLAGS_RELEASE += /Zc:alignedNew
    QMAKE_CXXFLAGS += /utf-8
    QMAKE_CXXFLAGS += /arch:AVX2
    QMAKE_CXXFLAGS_RELEASE += /Zi
    QMAKE_LFLAGS_RELEASE += /DEBUG

}


QMAKE_PROJECT_DEPTH = 0

# 源文件
SOURCES += \
    AudioDeviceModuleImpl.cpp \
    CreateDescriptionObserverImpl.cpp \
    DataChannelObserverImpl.cpp \
    De265Decoder.cpp \
    InterceptionHook.cpp \
    MainWindow.cpp \
    PeerConnectionObserverImpl.cpp \
    RTCStatsCollectorHandle.cpp \
    SetDescriptionObserverImpl.cpp \
    Utils.cpp \
    VideoTrackSinkImpl.cpp \
    VideoWidget.cpp \
    WebRTCVideoDecoderFactory.cpp \
    WebRTCVideoEncoderFactory.cpp \
    main.cpp \
    WebRTCManager.cpp \

# 头文件
HEADERS += \
    AsioConcurrentQueue.h \
    AudioDeviceModuleImpl.h \
    ConfigManager.h \
    CreateDescriptionObserverImpl.h \
    CustomDialogs.h \
    DataChannelObserverImpl.h \
    De265Decoder.h \
    InterceptionHook.h \
    MainWindow.h \
    PeerConnectionObserverImpl.h \
    RTCStatsCollectorHandle.h \
    SetDescriptionObserverImpl.h \
    Utils.h \
    VideoTrackSinkImpl.h \
    VideoWidget.h \
    WebRTCVideoDecoderFactory.h \
    WebRTCVideoEncoderFactory.h \
    WindowsServiceManager.h \
    WebRTCManager.h \


win32 {

    INCLUDEPATH += $$PWD/include/boost
    INCLUDEPATH += $$PWD/include/webrtc
    INCLUDEPATH += $$PWD/include/interception
    INCLUDEPATH += $$PWD/include/openssl
    INCLUDEPATH += $$PWD/include/libde265

    LIBS += -L$$PWD/lib/boost/
    LIBS += -L$$PWD/lib/webrtc/
    LIBS += -L$$PWD/lib/openssl/
    LIBS += -L$$PWD/lib/interception/x64/
    LIBS += -L$$PWD/lib/tcmalloc/
    LIBS += -L$$PWD/lib/libde265/

    LIBS += -lwebrtc

    LIBS += -lde265

    LIBS += -linterception

    LIBS += -llibcrypto

    LIBS += -llibssl

    LIBS += -llibtcmalloc_minimal

    LIBS += -llibboost_sam-vc143-mt-x64-1_89


    LIBS += -lws2_32          # Windows Socket 2.0
    LIBS += -lmswsock         # Microsoft Winsock 2.0
    LIBS += -lwtsapi32        # Windows Terminal Services API
    LIBS += -lgdi32

    LIBS += -lwindowscodecs   # Windows Imaging Component
    LIBS += -lgdiplus         # GDI+
    LIBS += -ldwmapi          # Desktop Window Manager API
    LIBS += -lwindowsapp      # Windows Runtime
    LIBS += -lruntimeobject   # Windows Runtime Object

    LIBS += -luserenv         # User Environment
    LIBS += -ladvapi32        # Advanced Windows API
    LIBS += -lwinmm           # Windows Multimedia

    LIBS += -lmsdmo           # Microsoft DirectShow Media Objects
    LIBS += -ldmoguids        # DirectShow Media Object GUIDs
    LIBS += -lwmcodecdspuuid  # Windows Media Codec DSP UUIDs
    LIBS += -lstrmiids        # DirectShow Stream Interface IDs
    LIBS += -lmfuuid          # Media Foundation UUIDs


    QMAKE_LFLAGS += /INCLUDE:_tcmalloc

    DEFINES += WIN32_LEAN_AND_MEAN
    DEFINES += NOMINMAX
    DEFINES += WEBRTC_WIN
    DEFINES += WEBRTC_ARCH_LITTLE_ENDIAN

    TARGET = HopeDesk
}

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# 发布版本优化选项
release {
    DEFINES += QT_NO_DEBUG_OUTPUT
    DEFINES += QT_NO_WARNING_OUTPUT
    DEFINES += NDEBUG
}

# 版本信息
VERSION = 1.0.0
QMAKE_TARGET_DESCRIPTION = "HopeDesk"
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2025"

RESOURCES += \
    res.qrc

DISTFILES += \
    config.ini

FORMS += \
    mainwindow.ui
