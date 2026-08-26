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

# 源文件(按包分目录,目录名与命名空间一致)
SOURCES += \
    main.cpp \
    rtc/MainWindow.cpp \
    rtc/WebrtcManager.cpp \
    rtc/audio/AudioDeviceModuleImpl.cpp \
    rtc/codec/D3D11Av1VideoDecoder.cpp \
    rtc/codec/D3D11Nv12Renderer.cpp \
    rtc/codec/De265Decoder.cpp \
    rtc/codec/NvdecDecoder.cpp \
    rtc/codec/WebrtcD3D11TextureBuffer.cpp \
    rtc/factory/WebrtcVideoDecoderFactory.cpp \
    rtc/factory/WebrtcVideoEncoderFactory.cpp \
    rtc/impl/CreateDescriptionObserverImpl.cpp \
    rtc/impl/DataChannelObserverImpl.cpp \
    rtc/impl/PeerConnectionObserverImpl.cpp \
    rtc/impl/RTCStatsCollectorHandle.cpp \
    rtc/impl/SetDescriptionObserverImpl.cpp \
    rtc/impl/VideoTrackSinkImpl.cpp \
    rtc/widget/VideoWidget.cpp \
    net/TcpSocket.cpp \
    net/TcpAcceptor.cpp \
    net/WebSocket.cpp \
    system/InterceptionHook.cpp \
    utils/Utils.cpp \
    thirdParty/chromiumMedia/Av1Decoder.cc \
    thirdParty/chromiumMedia/Av1Picture.cc \

# 头文件(按包分目录,目录名与命名空间一致)
HEADERS += \
    net/AsioConcurrentQueue.h \
    net/TcpSocket.h \
    net/TcpAcceptor.h \
    net/WebSocket.h \
    net/Socket.h \
    rtc/MainWindow.h \
    rtc/WebrtcManager.h \
    rtc/audio/AudioDeviceModuleImpl.h \
    rtc/codec/D3D11Av1VideoDecoder.h \
    rtc/codec/D3D11Nv12Renderer.h \
    rtc/codec/D3D11VideoFrameData.h \
    rtc/codec/De265Decoder.h \
    rtc/codec/Nvdec.h \
    rtc/codec/NvdecDecoder.h \
    rtc/codec/WebrtcD3D11TextureBuffer.h \
    rtc/factory/WebrtcVideoDecoderFactory.h \
    rtc/factory/WebrtcVideoEncoderFactory.h \
    rtc/impl/CreateDescriptionObserverImpl.h \
    rtc/impl/DataChannelObserverImpl.h \
    rtc/impl/PeerConnectionObserverImpl.h \
    rtc/impl/RTCStatsCollectorHandle.h \
    rtc/impl/SetDescriptionObserverImpl.h \
    rtc/impl/VideoTrackSinkImpl.h \
    rtc/widget/CustomDialogs.h \
    rtc/widget/VideoWidget.h \
    system/InterceptionHook.h \
    system/WindowsServiceManager.h \
    utils/ConfigManager.h \
    utils/Options.h \
    utils/Utils.h \
    thirdParty/chromiumMedia/Av1Decoder.h \
    thirdParty/chromiumMedia/Av1Picture.h \
    thirdParty/chromiumShims/AbslCleanup.h \
    thirdParty/chromiumShims/BaseHelpers.h \
    thirdParty/chromiumShims/BaseMisc.h \
    thirdParty/chromiumShims/BaseSpan.h \
    thirdParty/chromiumShims/GfxGeometry.h \
    thirdParty/chromiumShims/GfxHdrMetadata.h \
    thirdParty/chromiumShims/MediaAcceleratedVideoDecoder.h \
    thirdParty/chromiumShims/MediaCodecPicture.h \
    thirdParty/chromiumShims/MediaDecoderBuffer.h \
    thirdParty/chromiumShims/MediaGpuExport.h \
    thirdParty/chromiumShims/MediaLimits.h \
    thirdParty/chromiumShims/MediaSvcGenericMetadata.h \
    thirdParty/chromiumShims/MediaSwitches.h \
    thirdParty/chromiumShims/MediaUtils.h \
    thirdParty/chromiumShims/MediaVideoTypes.h


win32 {

    INCLUDEPATH += $$PWD/include/boost
    INCLUDEPATH += $$PWD/include/webrtc
    INCLUDEPATH += $$PWD/include/interception
    INCLUDEPATH += $$PWD/include/openssl
    INCLUDEPATH += $$PWD/include/libde265
    INCLUDEPATH += $$PWD/include/nvidia   # NVDEC/NVENC 头(用户稍后恢复 NVDEC 用)
    INCLUDEPATH += $$PWD/include/libgav1
    INCLUDEPATH += $$PWD/include/ylt
    INCLUDEPATH += $$PWD/thirdParty

    # 包目录(目录名与命名空间一致,include 文件名保持原样靠 INCLUDEPATH 解析)
    INCLUDEPATH += $$PWD/rtc
    INCLUDEPATH += $$PWD/rtc/impl
    INCLUDEPATH += $$PWD/rtc/factory
    INCLUDEPATH += $$PWD/rtc/widget
    INCLUDEPATH += $$PWD/rtc/audio
    INCLUDEPATH += $$PWD/rtc/codec
    INCLUDEPATH += $$PWD/net
    INCLUDEPATH += $$PWD/system
    INCLUDEPATH += $$PWD/utils

    LIBS += -L$$PWD/lib/boost/
    LIBS += -L$$PWD/lib/webrtc/
    LIBS += -L$$PWD/lib/openssl/
    LIBS += -L$$PWD/lib/interception/x64/
    LIBS += -L$$PWD/lib/tcmalloc/
    LIBS += -L$$PWD/lib/libde265/
    LIBS += -L$$PWD/lib/libgav1/   # libgav1_static(cmake 编译产物)

    LIBS += -lwebrtc

    LIBS += -llibgav1   # AV1 码流解析(ObuParser),cmake 编译

    LIBS += -lde265

    LIBS += -linterception

    LIBS += -llibcrypto

    LIBS += -llibssl

    LIBS += -llibtcmalloc_minimal

    LIBS += -ld3d11           # D3D11(零拷贝共享纹理/DXVA)
    LIBS += -ldxgi            # DXGI
    LIBS += -ld3dcompiler     # D3DCompile(裸 D3D11 NV12 shader 编译)
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
    DEFINES += BOOST_SAM_HEADER_ONLY

    # libgav1 编译配置(与 cmake 一致,保证头文件一致性)
    DEFINES += LIBGAV1_CMAKE=1
    DEFINES += LIBGAV1_MAX_BITDEPTH=12
    DEFINES += LIBGAV1_ENABLE_AVX2=1
    DEFINES += LIBGAV1_ENABLE_SSE4_1=1
    DEFINES += LIBGAV1_ENABLE_NEON=0
    DEFINES += LIBGAV1_THREADPOOL_USE_STD_MUTEX=1

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
