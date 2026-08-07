#include "VirtualDisplayCapture.h"

#include <chrono>
#include <objbase.h>
#include <cstdint>
#include <cstring>
#include <setupapi.h>
#include <cfgmgr32.h>

#include "../../utils/Utils.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "setupapi.lib")

namespace hope {
    namespace rtc {

        EXTERN_C const GUID DECLSPEC_SELECTANY GUID_DEVINTERFACE_ZAKO_VDD_CONTROL =
        { 0xDA9F8C2B, 0x7E4F, 0x49A1, { 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D } };

        static const wchar_t* kVddMonitorId = L"HPD"; // EDID manufacturer -> DeviceID contains "HPD"

        // Deterministic GUID from a string (FNV-1a 64-bit, spread across the 16 bytes).
        // Used so the same `id` (e.g. a systemService string) maps to the same identity.
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

        VirtualDisplayCapture::VirtualDisplayCapture() = default;

        VirtualDisplayCapture::~VirtualDisplayCapture()
        {
            stopCapture();
            closeFrameChannel();
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
        LUID VirtualDisplayCapture::getAdapterLuid() const { return adapterLuid; }

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

        bool VirtualDisplayCapture::sendCommand(const wchar_t* cmd)
        {
            if (driverDevice == INVALID_HANDLE_VALUE) return false;
            DWORD br = 0;
            return DeviceIoControl(driverDevice, IOCTL_VDD_COMMAND,
                const_cast<wchar_t*>(cmd),
                static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t)),
                nullptr, 0, &br, nullptr) != FALSE;
        }

        // Enable the driver's hardware cursor so the OS renders the pointer through
        // the driver's out-of-band cursor channel (IddCx hardware cursor) instead of
        // compositing it into the frame buffer. Without this, the cursor ends up in
        // the captured frames. The setting is persisted in the registry and applied
        // by the driver reload triggered by the command.
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

            // Applying the setting reloads driver settings and re-enumerates monitors
            // so the next swap chain uses the hardware cursor. Best effort: if it
            // fails the capture still runs, just with the cursor in the frames.
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

        // After enumeration: in clone (duplicate) mode a physical display occupies the
        // same screen rect as the VDD; in extend mode it does not.
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

            // Give the driver + PnP a short moment to connect; the frame-channel open
            // already retries until the producer is ready.
            Sleep(1000);
            return true;
        }

        // Activate the virtual display. With a physical monitor present and
        // mirrorPrimary=true, the primary is cloned onto the VDD. On headless hosts the
        // VDD becomes the only / primary display.
        bool VirtualDisplayCapture::applyTopology()
        {
            // Check the CURRENT topology first. If the VDD is already an active display,
            // it was set up by a previous session — re-applying EXTEND/CLONE would
            // rearrange the desktop (screen flicker) for no reason.
            VddMonitorScan scan{};
            EnumDisplayMonitors(nullptr, nullptr, ScanVddMonitor, reinterpret_cast<LPARAM>(&scan));
            FinalizeScan(scan);
            const bool physicalActive = scan.physicalActive;
            const bool vddActive = scan.vddActive;
            const bool isMirrored = scan.isMirrored;

            if (vddActive) {
                // VDD already active. Only change if the mirror setting differs from the
                // current state (want clone but currently extended).
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

            // Headless fallback: if the VDD is still not active (extend needs a primary),
            // build an explicit topology with only the VDD path.
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

        bool VirtualDisplayCapture::openFrameChannel()
        {
            // The producer creates its shared textures lazily on the first frame, so the
            // channel may report NOT_READY for a short while after the monitor connects.
            for (int attempt = 0; attempt < 30; ++attempt) {
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

                // Read the initial metadata to get the render adapter + format.
                ZakoFrameMetadata meta{};
                if (readStableMetadata(meta)) {
                    adapterLuid.LowPart = meta.AdapterLuidLowPart;
                    adapterLuid.HighPart = meta.AdapterLuidHighPart;
                }

                return true;
            }

            return false;
        }

        void VirtualDisplayCapture::closeFrameChannel()
        {
            slotKm.clear();
            slotTex.clear();
            slotHandles.clear();
            if (pMeta) { UnmapViewOfFile(pMeta); pMeta = nullptr; }
            if (metaMapping) { CloseHandle(metaMapping); metaMapping = nullptr; }
            if (frameReadyEvent) { CloseHandle(frameReadyEvent); frameReadyEvent = nullptr; }
            slotCount = 0;
        }

        // 驱动重建共享纹理（ChannelGeneration 变化）后，旧 slot handle 全部失效。
        // 重新打开通道拿新 handle，让下游编码器（其 resourceCache 也已随
        // generation 变化被清空）用新 handle 重新建立同步。
        bool VirtualDisplayCapture::reopenFrameChannel()
        {
            closeFrameChannel();
            if (!openFrameChannel()) {
                LOG_ERROR("VirtualDisplayCapture::reopenFrameChannel openFrameChannel failed");
                return false;
            }
            haveFrame = false;  // 新通道后首个发布视为新帧
            LOG_INFO("VirtualDisplayCapture frame channel reopened");
            return true;
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

        // ---------------------------------------------------------------------------
        // Frame delivery
        // ---------------------------------------------------------------------------

        bool VirtualDisplayCapture::deliverNewFrame(const ZakoFrameMetadata& meta)
        {
            const UINT slot = meta.SlotIndex;
            if (slot >= slotCount || !slotKm[slot]) return false;

            if (config.cpuPath && dataHandle) {
                // CPU path: staging copy + map + deliver + cache for repeat.
                HRESULT hr = slotKm[slot]->AcquireSync(1, 2000);
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

            // GPU path: relay the producer's slot handle directly to the encoder. The
            // capture does NOT touch the keyed mutex — the encoder is the consumer and
            // syncs itself: AcquireSync(1) -> encode -> ReleaseSync(0). This never
            // stalls: if the encoder is not ready (e.g. still initializing), the
            // producer's 3-slot ring keeps republishing and reclaims unread slots, so
            // the latest frame is always relayed once the encoder starts.
            if (!gpuDataHandle) return false;

            gpuDataHandle(slotHandles[slot], (int)meta.Width, (int)meta.Height,
                meta.DxgiFormat, (UINT)(meta.Width * 4), meta.FrameCounter);
            haveFrame = true;
            lastFrameId = meta.FrameCounter;
            return true;
        }

        void VirtualDisplayCapture::deliverRepeatFrame()
        {
            // Repeat is only meaningful on the CPU path, where we cache the last frame.
            // The GPU path hands the producer's slot to the encoder directly (Sunshine
            // borrow); a producer slot cannot be safely re-delivered once the encoder
            // has released it back to key 0.
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
            const DWORD waitMs = 100; // repeat-frame cadence

            // 建立 ChannelGeneration 基线，避免首圈误判通道已重建。
            if (pMeta) {
                ZakoFrameMetadata meta{};
                if (readStableMetadata(meta)) lastChannelGen = static_cast<UINT16>(meta.MetadataSequence >> 16);
            }

            while (capturing.load()) {
                // 主动检测驱动重建共享通道（ChannelGeneration 变化）。只依赖
                // frameReadyEvent 不可靠：静止/熄屏后驱动可能不再 signal 旧事件，
                // 捕获线程会无任何日志地空转定格。这里每圈轮询 metadata 对比基线。
                bool reopenNeeded = false;
                if (pMeta) {
                    ZakoFrameMetadata meta{};
                    if (readStableMetadata(meta)) {
                        reopenNeeded = (static_cast<UINT16>(meta.MetadataSequence >> 16) != lastChannelGen);
                    }
                    else {
                        reopenNeeded = true; // metadata 读不到 = 通道异常
                    }
                }
                else {
                    reopenNeeded = true; // 通道未建立（如上次重开失败），持续重开
                }

                // 下游编码器报告 keyed-mutex 同步丢失 → 同样触发重开（双保险）。
                if (channelSync && channelSync->reopenRequested.exchange(0)) {
                    reopenNeeded = true;
                }

                if (reopenNeeded) {
                    if (reopenFrameChannel()) {
                        // 重开后刷新基线，避免下一圈重复触发。
                        ZakoFrameMetadata meta{};
                        if (readStableMetadata(meta)) lastChannelGen = static_cast<UINT16>(meta.MetadataSequence >> 16);
                        if (channelSync) channelSync->generation.fetch_add(1);
                        LOG_INFO("VirtualDisplayCapture frame channel reopened after channel generation change");
                    }
                    else {
                        // 重开失败：短暂退避，避免热自旋，下一圈再试。
                        LOG_WARN("VirtualDisplayCapture frame channel reopen failed, will retry");
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                }

                DWORD wr = WaitForSingleObject(frameReadyEvent, waitMs);

                if (wr == WAIT_OBJECT_0) {
                    ZakoFrameMetadata meta{};
                    if (!readStableMetadata(meta) || meta.Magic != 0x5A564446u || meta.SlotIndex >= slotCount) {
                        continue;
                    }
                    if (meta.FrameCounter == lastFrameId && haveFrame) {
                        // Same frame id re-signalled; treat as a repeat tick.
                        deliverRepeatFrame();
                        continue;
                    }
                    deliverNewFrame(meta);
                }
                else if (wr == WAIT_TIMEOUT) {
                    deliverRepeatFrame();
                }
                else {

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

            // Keep the OS cursor out of the captured frames (hardware cursor).
            // Best effort: if it fails, capture still runs but may include the cursor.
            if (!enableHardwareCursor()) {

                LOG_WARN("enableHardwareCursor failed; captured frames may include the cursor");

            }

            frameIntervalMs = (config.refreshRate > 0) ? (1000 / config.refreshRate) : 16;
            if (frameIntervalMs < 1) frameIntervalMs = 1;

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

            if (!openFrameChannel()) {

                LOG_ERROR("OpenFrameChannel Failed");

                return false;

            }

            LOG_INFO("VirtualDisplayCapture::initialize Successful");

            return true;
        }

        bool VirtualDisplayCapture::startCapture()
        {
            if (capturing.load()) return true;
            if (driverDevice == INVALID_HANDLE_VALUE || !pMeta) {

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
