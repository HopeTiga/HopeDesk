#include "VirtualDisplayCapture.h"

#include <chrono>
#include <objbase.h>
#include <cstdint>
#include <cstring>
#include <setupapi.h>
#include <cfgmgr32.h>

#include "../../utils/Utils.h"
#include "../../utils/PerfBoost.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "setupapi.lib")

namespace hope {
    namespace rtc {

        EXTERN_C const GUID DECLSPEC_SELECTANY GUID_DEVINTERFACE_ZAKO_VDD_CONTROL =
        { 0xDA9F8C2B, 0x7E4F, 0x49A1, { 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D } };

        static const wchar_t* kVddMonitorId = L"HPD"; // EDID manufacturer -> DeviceID contains "HPD"

        constexpr int kVddReopenMaxAttempts = 2;   // reopen：2×500ms ≈ 1s
        constexpr int kVddStartupAttempts = 6;     // 启动：6×500ms ≈ 3s，快速失败交给采集线程后台重试
        constexpr int kVddBackoffStartMs = 300;    // 指数退避 0.3s→0.6s→1s 封顶
        constexpr int kVddBackoffMaxMs = 1000;
        constexpr UINT32 kVddProbeIdleTicks = 8;        // 静止 8 圈（~0.8s）后开始探测
        constexpr UINT32 kVddProbeMaxIdleTicks = 50;    // 探测间隔封顶 5s
        constexpr int kVddNotReadyToleranceMs = 15000;  // NOT_READY 持续超时才判驱动僵死
        constexpr UINT32 kVddMaxDimension = 16384;

        constexpr DWORD kVddCaptureAcquireMs = 2;  // keyed-mutex 等待预算，超时丢帧不阻塞

        static void DeriveGuidFromString(const char* s, GUID& g)
        {
            uint64_t h = 1469598103934665603ULL;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
                h ^= *p;
                h *= 1099511628211ULL;
            }
            uint64_t h2 = h ^ 0x9E3779B97F4A7C15ULL;
            h2 *= 1099511628211ULL;

            g.Data1 = static_cast<uint32_t>(h);
            g.Data2 = static_cast<uint16_t>(h >> 32);
            g.Data3 = static_cast<uint16_t>(h >> 48);
            uint64_t combo = h2 ^ (h * 0xD1B54A32D192ED03ULL);
            std::memcpy(g.Data4, &combo, 8);
        }

        // 生产者格式白名单，返回每像素字节数；0 = 不支持（与 vdd_capture_test 的判别一致）。
        static UINT dxgiFormatBytesPerPixel(UINT32 format)
        {
            switch (format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            default:
                return 0;
            }
        }

        VirtualDisplayCapture::VirtualDisplayCapture() = default;

        VirtualDisplayCapture::~VirtualDisplayCapture()
        {
            stopCapture();
            closeFrameChannel();
            if (weCreated && config.removeOnDestroy && driverDevice != INVALID_HANDLE_VALUE) {
                sendCommand(L"DESTROYMONITOR");
            }
            if (driverDevice != INVALID_HANDLE_VALUE) {
                CloseHandle(driverDevice);
                driverDevice = INVALID_HANDLE_VALUE;
            }
        }

        void VirtualDisplayCapture::setConfig(Config c) { config = c; }
        void VirtualDisplayCapture::setGpuDataHandle(GpuDataHandle h) { gpuDataHandle = h; }
        void VirtualDisplayCapture::setDataHandle(DataHandle h) { dataHandle = h; }
        void VirtualDisplayCapture::setChannelSync(std::shared_ptr<VddChannelSync> s) { channelSync = std::move(s); }
        GUID VirtualDisplayCapture::getMonitorGuid() const { return monitorGuid; }

        // ---------------------------------------------------------------------------
        // Driver / monitor management
        // ---------------------------------------------------------------------------

        bool VirtualDisplayCapture::openDriver()
        {
            HDEVINFO hDevInfo = SetupDiGetClassDevsW(
                &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL, nullptr, nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (hDevInfo == INVALID_HANDLE_VALUE) {

                return false;
            }

            SP_DEVICE_INTERFACE_DATA ifData = {};
            ifData.cbSize = sizeof(ifData);
            std::wstring path;
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL, i, &ifData); ++i) {
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &needed, nullptr);
                if (needed == 0) continue;
                std::vector<BYTE> buf(needed);
                auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, needed, nullptr, nullptr)) {
                    path = detail->DevicePath;
                    break;
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);

            if (path.empty()) {

                return false;
            }

            driverDevice = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (driverDevice == INVALID_HANDLE_VALUE) {

                return false;
            }

            return true;
        }

        bool VirtualDisplayCapture::reopenDriver()
        {
            if (driverDevice != INVALID_HANDLE_VALUE) {
                CloseHandle(driverDevice);
                driverDevice = INVALID_HANDLE_VALUE;
            }
            return openDriver();
        }

        bool VirtualDisplayCapture::sendCommand(const wchar_t* cmd)
        {
            if (driverDevice == INVALID_HANDLE_VALUE) return false;
            DWORD br = 0;
            return DeviceIoControl(driverDevice, IOCTL_VDD_COMMAND,
                const_cast<wchar_t*>(cmd),
                static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t)),
                nullptr, 0, &br, nullptr) != FALSE;
        }

        bool VirtualDisplayCapture::enableHardwareCursor()
        {
            if (driverDevice == INVALID_HANDLE_VALUE) return false;

            HKEY hKey = nullptr;
            DWORD keyDisposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter",
                    0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, &keyDisposition) != ERROR_SUCCESS)
            {
                return false;
            }
            DWORD one = 1;
            RegSetValueExW(hKey, L"HARDWARECURSOR", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(hKey);

            return sendCommand(L"HARDWARECURSOR true");
        }

        // Scan result shared by the monitor enumeration callback.
        struct VddMonitorScan {
            bool found = false;
            bool vddActive = false;
            bool physicalActive = false;
            RECT vddRect{};
            bool hasMode = false;
            DEVMODEW mode{};
            std::vector<RECT> physicalRects;
            bool isMirrored = false;   // a physical display shares the VDD's screen rect (clone)
        };

        // MONITORENUMPROC needs a plain __stdcall function; state is passed via LPARAM.
        static BOOL CALLBACK ScanVddMonitor(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
        {
            auto* scan = reinterpret_cast<VddMonitorScan*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

            DISPLAY_DEVICEW dd = { sizeof(dd) };
            if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) {
                if (wcsstr(dd.DeviceID, kVddMonitorId) != nullptr) {
                    scan->vddActive = true;
                    scan->found = true;
                    scan->vddRect = mi.rcMonitor;
                    DEVMODEW dm{};
                    dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                        scan->mode = dm;
                        scan->hasMode = true;
                    }
                }
                else {
                    scan->physicalActive = true;
                    scan->physicalRects.push_back(mi.rcMonitor);
                }
            }
            return TRUE;
        }

        static void FinalizeScan(VddMonitorScan& scan)
        {
            if (scan.vddActive) {
                for (const auto& pr : scan.physicalRects) {
                    if (EqualRect(&pr, &scan.vddRect)) {
                        scan.isMirrored = true;
                        break;
                    }
                }
            }
        }

        // Find the HopeDesk virtual display and read its current mode.
        static bool FindVddMonitor(RECT& rc, DEVMODEW& mode, bool& hasMode)
        {
            VddMonitorScan scan{};
            EnumDisplayMonitors(nullptr, nullptr, ScanVddMonitor, reinterpret_cast<LPARAM>(&scan));
            FinalizeScan(scan);
            if (scan.found) {
                rc = scan.vddRect;
                mode = scan.mode;
                hasMode = scan.hasMode;
            }
            return scan.found;
        }

        bool VirtualDisplayCapture::ensureDisplay()
        {
            GUID targetGuid = config.monitorGuid;
            if (targetGuid == GUID{}) {
                if (config.id && config.id[0]) DeriveGuidFromString(config.id, targetGuid);
                else CoCreateGuid(&targetGuid);
            }
            monitorGuid = targetGuid;

            RECT rc{};
            DEVMODEW curMode{};
            bool hasMode = false;
            const bool exists = FindVddMonitor(rc, curMode, hasMode);

            const int wantW = config.width, wantH = config.height, wantR = config.refreshRate;
            bool modeMatches = exists && hasMode &&
                (int)curMode.dmPelsWidth == wantW &&
                (int)curMode.dmPelsHeight == wantH &&
                (int)curMode.dmDisplayFrequency == wantR;

            if (exists && modeMatches) {

                // Re-issue CREATEMONITOR so the driver's in-memory mode list matches.
                wchar_t cmd[128];
                swprintf_s(cmd, L"CREATEMONITOR %d %d %d", wantW, wantH, wantR);
                sendCommand(cmd);
                return true;
            }

            if (exists && !modeMatches) {

                sendCommand(L"DESTROYMONITOR");
                Sleep(1000);
            }
            else if (!exists) {

            }

            wchar_t cmd[128];
            swprintf_s(cmd, L"CREATEMONITOR %d %d %d", wantW, wantH, wantR);
            if (!sendCommand(cmd)) {

                return false;
            }
            weCreated = true;

            Sleep(1000);
            return true;
        }

        bool VirtualDisplayCapture::applyTopology()
        {
            VddMonitorScan scan{};
            EnumDisplayMonitors(nullptr, nullptr, ScanVddMonitor, reinterpret_cast<LPARAM>(&scan));
            FinalizeScan(scan);
            const bool physicalActive = scan.physicalActive;
            const bool vddActive = scan.vddActive;
            const bool isMirrored = scan.isMirrored;

            if (vddActive) {
                if (config.mirrorPrimary && physicalActive && !isMirrored) {
                    LONG cloneErr = ERROR_INVALID_PARAMETER;
                    for (int a = 0; a < 10 && cloneErr != ERROR_SUCCESS; ++a) {
                        cloneErr = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_CLONE | SDC_APPLY);
                        if (cloneErr != ERROR_SUCCESS) Sleep(500);
                    }
                    Sleep(600);
                }
                return true;
            }

            // VDD not active yet — activate it (single EXTEND).
            LONG scErr = ERROR_INVALID_PARAMETER;
            for (int a = 0; a < 10 && scErr != ERROR_SUCCESS; ++a) {
                scErr = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
                if (scErr != ERROR_SUCCESS) Sleep(500);
            }
            Sleep(800);

            // Re-scan after activation.
            VddMonitorScan scan2{};
            EnumDisplayMonitors(nullptr, nullptr, ScanVddMonitor, reinterpret_cast<LPARAM>(&scan2));
            FinalizeScan(scan2);
            const bool vddActive2 = scan2.vddActive;
            const bool physicalActive2 = scan2.physicalActive;

            if (config.mirrorPrimary && physicalActive2 && vddActive2) {
                LONG cloneErr = ERROR_INVALID_PARAMETER;
                for (int a = 0; a < 10 && cloneErr != ERROR_SUCCESS; ++a) {
                    cloneErr = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_CLONE | SDC_APPLY);
                    if (cloneErr != ERROR_SUCCESS) Sleep(500);
                }
                Sleep(600);
            }

            if (!vddActive2) {

                UINT32 np = 0, nm = 0;
                if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) {

                    return false;
                }
                std::vector<DISPLAYCONFIG_PATH_INFO> paths(np + 8);
                std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm + 8);
                UINT32 np2 = np, nm2 = nm;
                if (QueryDisplayConfig(QDC_ALL_PATHS, &np2, paths.data(), &nm2, modes.data(), nullptr) != ERROR_SUCCESS) {

                    return false;
                }

                for (UINT32 i = 0; i < np2; ++i) {
                    DISPLAYCONFIG_TARGET_DEVICE_NAME tdn = {};
                    tdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tdn.header.size = sizeof(tdn);
                    tdn.header.adapterId = paths[i].targetInfo.adapterId;
                    tdn.header.id = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tdn.header) != ERROR_SUCCESS) continue;
                    bool isVdd = (wcsstr(tdn.monitorFriendlyDeviceName, L"HopeDesk") != nullptr) ||
                        (wcsstr(tdn.monitorFriendlyDeviceName, kVddMonitorId) != nullptr);
                    if (!isVdd) continue;

                    DISPLAYCONFIG_PATH_INFO vddPath = paths[i];
                    vddPath.flags = DISPLAYCONFIG_PATH_ACTIVE;
                    vddPath.sourceInfo.modeInfoIdx = 0;
                    vddPath.targetInfo.modeInfoIdx = 1;

                    DISPLAYCONFIG_MODE_INFO src = {};
                    src.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
                    src.adapterId = vddPath.sourceInfo.adapterId;
                    src.id = vddPath.sourceInfo.id;
                    src.sourceMode.width = (UINT32)config.width;
                    src.sourceMode.height = (UINT32)config.height;
                    src.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
                    src.sourceMode.position.x = 0;
                    src.sourceMode.position.y = 0;

                    DISPLAYCONFIG_MODE_INFO tgt = {};
                    tgt.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
                    tgt.adapterId = vddPath.targetInfo.adapterId;
                    tgt.id = vddPath.targetInfo.id;
                    DISPLAYCONFIG_VIDEO_SIGNAL_INFO& vsi = tgt.targetMode.targetVideoSignalInfo;
                    vsi.totalSize.cx = vsi.activeSize.cx = (UINT32)config.width;
                    vsi.totalSize.cy = vsi.activeSize.cy = (UINT32)config.height;
                    vsi.vSyncFreq.Numerator = (UINT32)config.refreshRate;
                    vsi.vSyncFreq.Denominator = 1;
                    vsi.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

                    DISPLAYCONFIG_PATH_INFO newPaths[1] = { vddPath };
                    DISPLAYCONFIG_MODE_INFO newModes[2] = { src, tgt };
                    LONG hr = SetDisplayConfig(1, newPaths, 2, newModes,
                        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE);

                    if (hr != ERROR_SUCCESS) return false;
                    return true;
                }

                return false;
            }
            return true;
        }

        // ---------------------------------------------------------------------------
        // D3D11 device + frame channel
        // ---------------------------------------------------------------------------

        bool VirtualDisplayCapture::initLocalDevice()
        {
            if (d3dDevice) return true;

            HRESULT hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                &d3dDevice, nullptr, &d3dContext);
            if (FAILED(hr)) {

                return false;
            }
            d3dDevice.As(&d3dDevice1);

            // 降低设备级延迟:GPU 线程优先级 + 最大帧延迟,并设置进程 GPU 调度优先级(只做一次)
            hope::perf::applyGpuDeviceLatency(d3dDevice.Get());

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
            if (SUCCEEDED(d3dDevice.As(&dxgiDev))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        adapterLuid = desc.AdapterLuid;
                    }
                }
            }

            return true;
        }

        bool VirtualDisplayCapture::openFrameChannel(int maxAttempts)
        {
            for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                VDD_FRAME_CHANNEL_CAPS caps = {};
                caps.Size = sizeof(caps);
                DWORD br = 0;
                BOOL ok = DeviceIoControl(driverDevice, IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS,
                    nullptr, 0, &caps, sizeof(caps), &br, nullptr);
                if (!ok || caps.Version != VDD_FRAME_CHANNEL_CAPS_VERSION) {

                    return false;
                }

                VDD_FRAME_CHANNEL_OPEN_REQUEST req = {};
                req.Size = sizeof(req);
                req.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
                req.MonitorIndex = 0;
                req.RequiredFlags = 0;
                req.TargetProcessId = GetCurrentProcessId();
                req.DesiredSlots = 0;
                req.AdapterLuidLowPart = adapterLuid.LowPart;
                req.AdapterLuidHighPart = adapterLuid.HighPart;

                VDD_FRAME_CHANNEL_OPEN_RESPONSE resp = {};
                ok = DeviceIoControl(driverDevice, IOCTL_VDD_OPEN_FRAME_CHANNEL,
                    &req, sizeof(req), &resp, sizeof(resp), &br, nullptr);
                if (!ok) {
                    DWORD err = GetLastError();
                    if (err == ERROR_NOT_READY) {
                        Sleep(500);
                        continue; // producer not ready yet
                    }

                    return false;
                }
                if (resp.SlotCount == 0 || resp.SlotCount > VDD_FRAME_CHANNEL_MAX_SLOTS) {

                    return false;
                }

                slotCount = resp.SlotCount;
                frameReadyEvent = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.FrameReadyEventHandle));
                HANDLE hMeta = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.MetadataHandle));
                metaMapping = hMeta;
                pMeta = static_cast<ZakoFrameMetadata*>(MapViewOfFile(hMeta, FILE_MAP_READ, 0, 0, sizeof(ZakoFrameMetadata)));
                if (!pMeta) {

                    return false;
                }

                slotHandles.resize(slotCount);
                slotTex.assign(slotCount, {});
                slotKm.assign(slotCount, {});
                for (UINT32 s = 0; s < slotCount; ++s) {
                    slotHandles[s] = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.Slots[s].TextureHandle));
                    HRESULT hr = d3dDevice1->OpenSharedResource1(slotHandles[s], __uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(slotTex[s].GetAddressOf()));
                    if (FAILED(hr) || !slotTex[s]) {

                        return false;
                    }
                    slotTex[s].As(&slotKm[s]);
                    if (!slotKm[s]) {

                        return false;
                    }
                }

                return true;
            }

            return false;
        }

        void VirtualDisplayCapture::closeFrameChannel()
        {
            slotKm.clear();
            slotTex.clear();
            for (HANDLE h : slotHandles) { if (h) CloseHandle(h); }
            slotHandles.clear();
            if (pMeta) { UnmapViewOfFile(pMeta); pMeta = nullptr; }
            if (metaMapping) { CloseHandle(metaMapping); metaMapping = nullptr; }
            if (frameReadyEvent) { CloseHandle(frameReadyEvent); frameReadyEvent = nullptr; }
            slotCount = 0;
        }

        bool VirtualDisplayCapture::reopenFrameChannel()
        {
            closeFrameChannel();
            if (!openFrameChannel(kVddReopenMaxAttempts)) {
                return false;
            }
            haveFrame = false;  // 新通道后首个发布视为新帧
            return true;
        }

        VirtualDisplayCapture::ProbeResult VirtualDisplayCapture::probeChannelGeneration(UINT16& outGen)
        {
            VDD_FRAME_CHANNEL_OPEN_REQUEST req = {};
            req.Size = sizeof(req);
            req.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
            req.MonitorIndex = 0;
            req.RequiredFlags = 0;
            req.TargetProcessId = GetCurrentProcessId();
            req.DesiredSlots = 0;
            req.AdapterLuidLowPart = adapterLuid.LowPart;
            req.AdapterLuidHighPart = adapterLuid.HighPart;

            VDD_FRAME_CHANNEL_OPEN_RESPONSE resp = {};
            DWORD br = 0;
            BOOL ok = DeviceIoControl(driverDevice, IOCTL_VDD_OPEN_FRAME_CHANNEL,
                &req, sizeof(req), &resp, sizeof(resp), &br, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_NOT_READY) return ProbeResult::NotReady;
                lastProbeError = err;
                return ProbeResult::Rejected;
            }
            if (resp.SlotCount == 0 || resp.MetadataHandle == 0) return ProbeResult::NotReady;

            HANDLE hEvent = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.FrameReadyEventHandle));
            HANDLE hMeta = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.MetadataHandle));
            if (hEvent) CloseHandle(hEvent);

            auto* view = static_cast<const ZakoFrameMetadata*>(
                MapViewOfFile(hMeta, FILE_MAP_READ, 0, 0, sizeof(ZakoFrameMetadata)));
            if (!view) { CloseHandle(hMeta); return ProbeResult::NotReady; }

            // seqlock 读高 16 位 generation（与 readStableMetadata 同一模式）。
            bool got = false;
            UINT32 gen = 0;
            for (int s = 0; s < 8; ++s) {
                UINT32 s1 = view->MetadataSequence;
                if (s1 & 1u) { std::this_thread::yield(); continue; } // producer mid-write
                UINT32 g = s1 >> 16;
                UINT32 s2 = view->MetadataSequence;
                if (s1 == s2 && !(s2 & 1u)) { gen = g; got = true; break; }
                std::this_thread::yield();
            }

            UnmapViewOfFile(view);
            CloseHandle(hMeta);
            // 响应里重复出来的槽句柄本探测用不到，必须关掉避免句柄泄漏。
            for (UINT32 s = 0; s < resp.SlotCount; ++s) {
                HANDLE hSlot = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(resp.Slots[s].TextureHandle));
                if (hSlot) CloseHandle(hSlot);
            }

            if (!got) return ProbeResult::NotReady;
            outGen = static_cast<UINT16>(gen);
            return ProbeResult::Ready;
        }

        bool VirtualDisplayCapture::readStableMetadata(ZakoFrameMetadata& out)
        {
            if (!pMeta) return false;
            for (int attempt = 0; attempt < 8; ++attempt) {
                UINT32 s1 = pMeta->MetadataSequence;
                if (s1 & 1u) { std::this_thread::yield(); continue; } // producer mid-write
                std::memcpy(&out, pMeta, sizeof(out));
                UINT32 s2 = pMeta->MetadataSequence;
                if (s1 == s2 && !(s2 & 1u)) return true;
                std::this_thread::yield();
            }
            return false;
        }

        bool VirtualDisplayCapture::isMetadataValid(const ZakoFrameMetadata& meta) const
        {
            if (meta.Magic != 0x5A564446u) return false;
            if (meta.Version != 1u) return false;
            if (meta.MetadataSize != sizeof(ZakoFrameMetadata)) return false;
            if (meta.SlotCount != slotCount) return false;
            if (meta.SlotIndex >= slotCount) return false;
            if (meta.Width == 0 || meta.Height == 0) return false;
            if (meta.Width > kVddMaxDimension || meta.Height > kVddMaxDimension) return false;
            return dxgiFormatBytesPerPixel(meta.DxgiFormat) != 0;
        }

        bool VirtualDisplayCapture::deliverNewFrame(const ZakoFrameMetadata& meta)
        {
            const UINT slot = meta.SlotIndex;
            if (slot >= slotCount || !slotKm[slot]) return false;

            if (config.cpuPath && dataHandle) {
                // CPU path: staging copy + map + deliver + cache for repeat.
                HRESULT hr = slotKm[slot]->AcquireSync(1, kVddCaptureAcquireMs);
                if (hr != S_OK) return false;

                D3D11_TEXTURE2D_DESC stagingDesc = {};
                stagingDesc.Width = meta.Width;
                stagingDesc.Height = meta.Height;
                stagingDesc.MipLevels = 1;
                stagingDesc.ArraySize = 1;
                stagingDesc.Format = (DXGI_FORMAT)meta.DxgiFormat;
                stagingDesc.SampleDesc.Count = 1;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                if (FAILED(d3dDevice->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf()))) {
                    slotKm[slot]->ReleaseSync(0);
                    return false;
                }
                d3dContext->CopyResource(staging.Get(), slotTex[slot].Get());
                d3dContext->Flush();

                D3D11_MAPPED_SUBRESOURCE m = {};
                hr = d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
                if (SUCCEEDED(hr)) {
                    const int rowBytes = (int)m.RowPitch;
                    const size_t need = (size_t)rowBytes * meta.Height;
                    if (cpuCache.size() < need) cpuCache.resize(need);
                    const uint8_t* src = reinterpret_cast<const uint8_t*>(m.pData);
                    for (UINT y = 0; y < meta.Height; ++y)
                        std::memcpy(cpuCache.data() + (size_t)y * rowBytes, src + (size_t)y * rowBytes, rowBytes);
                    cpuCacheW = (int)meta.Width;
                    cpuCacheH = (int)meta.Height;
                    cpuCachePitch = rowBytes;
                    d3dContext->Unmap(staging.Get(), 0);

                    dataHandle(cpuCache.data(), (int)meta.Width, (int)meta.Height, rowBytes, meta.FrameCounter);
                    haveFrame = true;
                    lastFrameId = meta.FrameCounter;
                }
                slotKm[slot]->ReleaseSync(0);
                return SUCCEEDED(hr);
            }

            if (!gpuDataHandle) return false;

            gpuDataHandle(slotHandles[slot], (int)meta.Width, (int)meta.Height,
                meta.DxgiFormat, (UINT)(meta.Width * dxgiFormatBytesPerPixel(meta.DxgiFormat)),
                meta.FrameCounter);
            haveFrame = true;
            lastFrameId = meta.FrameCounter;
            return true;
        }

        void VirtualDisplayCapture::deliverRepeatFrame()
        {
            if (!haveFrame) return;
            if (config.cpuPath && dataHandle && !cpuCache.empty()) {
                dataHandle(cpuCache.data(), cpuCacheW, cpuCacheH, cpuCachePitch, lastFrameId);
            }
        }

        // ---------------------------------------------------------------------------
        // Capture thread
        // ---------------------------------------------------------------------------

        void VirtualDisplayCapture::captureThreadFunc()
        {
            // 帧通道采集线程对帧率/延迟最敏感,提到 TIME_CRITICAL
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            const DWORD waitMs = 100;       // 单圈等待粒度（repeat-frame cadence）
            UINT32 idleTicks = 0;
            UINT32 probeIdleTicks = kVddProbeIdleTicks;
            bool notReadyPending = false;
            bool rejectedLogged = false;
            std::chrono::steady_clock::time_point notReadySince{};

            // 建立 ChannelGeneration + 分辨率/格式基线，避免首圈误判通道已重建。
            if (pMeta) {
                ZakoFrameMetadata meta{};
                if (readStableMetadata(meta)) {
                    lastChannelGen = static_cast<UINT16>(meta.MetadataSequence >> 16);
                    lastWidth = (int)meta.Width;
                    lastHeight = (int)meta.Height;
                    lastFormat = meta.DxgiFormat;
                }
            }

            auto reopenChannel = [this]() -> bool {
                if (reopenFrameChannel()) {
                    ZakoFrameMetadata meta{};
                    if (readStableMetadata(meta)) {
                        lastChannelGen = static_cast<UINT16>(meta.MetadataSequence >> 16);
                        lastWidth = (int)meta.Width;
                        lastHeight = (int)meta.Height;
                        lastFormat = meta.DxgiFormat;
                    }
                    if (channelSync) {
                        channelSync->generation.fetch_add(1, std::memory_order_release);
                    }
                    if (channelDown) {
                        channelDown = false;
                        openBackoffMs = 0;
                        downLogCounter = 0;
                        LOG_INFO("VirtualDisplayCapture Frame Channel Recovered");
                    }
                    return true;
                }
                channelDown = true;
                if (openBackoffMs == 0) openBackoffMs = kVddBackoffStartMs;
                nextOpenRetryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(openBackoffMs);
                if (++downLogCounter == 1 || downLogCounter % 30 == 0) {
                    LOG_WARN("VirtualDisplayCapture Frame Channel Down, Retry In {} Ms", openBackoffMs);
                }
                openBackoffMs *= 2;
                if (openBackoffMs > kVddBackoffMaxMs) openBackoffMs = kVddBackoffMaxMs;
                return false;
            };

            while (capturing.load()) {

                // 通道未建立或已 down：退避期外才尝试 open，退避期内休眠。
                if (channelDown || !pMeta) {
                    if (channelDown && std::chrono::steady_clock::now() < nextOpenRetryAt) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        continue;
                    }
                    reopenChannel();
                    continue;
                }

                // 编码器报告 keyed-mutex 同步丢失 → 重开（明确信号）。
                if (channelSync && channelSync->reopenRequested.exchange(0)) {
                    reopenChannel();
                    continue;
                }

                DWORD wr;
                if (frameReadyEvent) {
                    wr = WaitForSingleObject(frameReadyEvent, waitMs);
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                    wr = WAIT_TIMEOUT;
                }

                if (wr == WAIT_OBJECT_0) {
                    // 与 Sunshine 一致：所有校验都在帧事件后做。
                    ZakoFrameMetadata meta{};
                    if (!readStableMetadata(meta) || !isMetadataValid(meta)) {
                        reopenChannel();  // 元数据不可读 = 通道异常（Sunshine: → reinit）
                        continue;
                    }
                    if (static_cast<UINT16>(meta.MetadataSequence >> 16) != lastChannelGen) {
                        // 生产端重建纹理/通道（Sunshine: generation changed → reinit）。
                        reopenChannel();
                        continue;
                    }
                    if ((int)meta.Width != lastWidth || (int)meta.Height != lastHeight ||
                        meta.DxgiFormat != lastFormat) {
                        // 生产端换了分辨率/格式（Sunshine: resolution/format changed → reinit）。
                        reopenChannel();
                        continue;
                    }
                    if (meta.FrameCounter == lastFrameId && haveFrame) {
                        deliverRepeatFrame();  // 同一帧重发，落入下方静止探测
                    }
                    else {
                        deliverNewFrame(meta);
                        idleTicks = 0;                // 有新帧，静止计数清零
                        probeIdleTicks = kVddProbeIdleTicks;
                        notReadyPending = false;
                        rejectedLogged = false;
                        continue;
                    }
                }
                else {
                    deliverRepeatFrame();
                }

                if (pMeta && ++idleTicks >= probeIdleTicks) {
                    idleTicks = 0;
                    UINT16 probeGen = 0;
                    switch (probeChannelGeneration(probeGen)) {
                    case ProbeResult::Ready:
                        notReadyPending = false;
                        if (probeGen != lastChannelGen) {
                            reopenChannel();
                        }
                        else if (probeIdleTicks < kVddProbeMaxIdleTicks) {
                            probeIdleTicks *= 2;
                            if (probeIdleTicks > kVddProbeMaxIdleTicks) probeIdleTicks = kVddProbeMaxIdleTicks;
                        }
                        break;
                    case ProbeResult::NotReady:
                        // 驱动在模式切换窗口内合法拒绝开通道，按墙上时钟容忍；只有持续
                        // 超过容忍窗口才是真僵死（设备下到 D3 后 swap chain 一直没回来）。
                        if (!notReadyPending) {
                            notReadyPending = true;
                            notReadySince = std::chrono::steady_clock::now();
                            probeIdleTicks = kVddProbeIdleTicks;
                        }
                        else if (std::chrono::steady_clock::now() - notReadySince >=
                                 std::chrono::milliseconds(kVddNotReadyToleranceMs)) {
                            notReadyPending = false;
                            LOG_WARN("VirtualDisplayCapture Driver Stalled, Reopening Device To Wake D0");
                            reopenDriver();
                            closeFrameChannel();        // 旧帧通道失效，pMeta 置空
                            channelDown = true;
                            openBackoffMs = kVddBackoffStartMs;
                            nextOpenRetryAt = std::chrono::steady_clock::now();
                            continue;
                        }
                        break;
                    case ProbeResult::Rejected:
                        // 请求被驱动拒绝（LUID 不符 / 参数非法），重载设备修不了，报一次即可。
                        if (!rejectedLogged) {
                            rejectedLogged = true;
                            LOG_WARN("VirtualDisplayCapture Frame Channel Probe Rejected, Error {}",
                                lastProbeError);
                        }
                        probeIdleTicks = kVddProbeMaxIdleTicks;
                        break;
                    }
                }

            }

        }

        // ---------------------------------------------------------------------------
        // Public lifecycle
        // ---------------------------------------------------------------------------

        bool VirtualDisplayCapture::initialize()
        {
            if (!openDriver()) {

                LOG_ERROR("OpenDriver Failed");

                return false;

            }

            if (!enableHardwareCursor()) {

                LOG_WARN("EnableHardwareCursor Failed; Captured Frames May Include The Cursor");

            }

            if (!ensureDisplay()) {

                LOG_ERROR("EnsureDisplay Failed");

                return false;

            }

            if (!applyTopology()) {

                LOG_ERROR("ApplyTopology Failed");

                return false;

            }

            if (!initLocalDevice()) {

                LOG_ERROR("InitLocalDevice Failed");

                return false;

            }

            if (!openFrameChannel(kVddStartupAttempts)) {
                LOG_WARN("OpenFrameChannel Not Ready At Startup, Deferring To Capture Thread");
                closeFrameChannel();
                reopenDriver();
                channelDown = true;
                openBackoffMs = kVddBackoffStartMs;
                nextOpenRetryAt = std::chrono::steady_clock::now();
            }

            LOG_INFO("VirtualDisplayCapture::Initialize Successful");

            return true;
        }

        bool VirtualDisplayCapture::startCapture()
        {
            if (capturing.load()) return true;
            if (driverDevice == INVALID_HANDLE_VALUE) {
                return false;
            }
            capturing = true;
            captureThread = std::thread([this]() { captureThreadFunc(); });
            return true;
        }

        void VirtualDisplayCapture::stopCapture()
        {
            if (!capturing.load()) return;
            capturing = false;
            if (captureThread.joinable()) captureThread.join();
        }

    } // namespace rtc
} // namespace hope
