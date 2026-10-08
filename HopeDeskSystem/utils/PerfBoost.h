#pragma once
#ifndef PERFBOOST_H
#define PERFBOOST_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include "Utils.h"

#pragma comment(lib, "winmm.lib")

typedef long NTSTATUS;

namespace hope {
    namespace perf {

        typedef enum _D3DKMT_SCHEDULINGPRIORITYCLASS {
            D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE,
            D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL,
            D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL,
            D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL,
            D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH,
            D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME
        } D3DKMT_SCHEDULINGPRIORITYCLASS;

        typedef UINT D3DKMT_HANDLE;

        typedef struct _D3DKMT_OPENADAPTERFROMLUID {
            LUID AdapterLuid;
            D3DKMT_HANDLE hAdapter;
        } D3DKMT_OPENADAPTERFROMLUID;

        typedef struct _D3DKMT_WDDM_2_7_CAPS {
            union {
                struct {
                    UINT HwSchSupported : 1;
                    UINT HwSchEnabled : 1;
                    UINT HwSchEnabledByDefault : 1;
                    UINT IndependentVidPnVSyncControl : 1;
                    UINT Reserved : 28;
                };
                UINT Value;
            };
        } D3DKMT_WDDM_2_7_CAPS;

        typedef struct _D3DKMT_QUERYADAPTERINFO {
            D3DKMT_HANDLE hAdapter;
            UINT Type;
            VOID* pPrivateDriverData;
            UINT PrivateDriverDataSize;
        } D3DKMT_QUERYADAPTERINFO;

        const UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;

        typedef struct _D3DKMT_CLOSEADAPTER {
            D3DKMT_HANDLE hAdapter;
        } D3DKMT_CLOSEADAPTER;

        typedef NTSTATUS(WINAPI* PD3DKMTSetProcessSchedulingPriorityClass)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS);
        typedef NTSTATUS(WINAPI* PD3DKMTOpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID*);
        typedef NTSTATUS(WINAPI* PD3DKMTQueryAdapterInfo)(D3DKMT_QUERYADAPTERINFO*);
        typedef NTSTATUS(WINAPI* PD3DKMTCloseAdapter)(D3DKMT_CLOSEADAPTER*);

        // HAGS 开着时是否仍用 REALTIME。语义抄 Sunshine 的 nvenc_realtime_hags（默认 true）。
        // 驱动有一条未修的 bug：REALTIME + HAGS + DX12 + 显存接近打满 会导致编码卡死甚至驱动崩溃
        // （Sunshine display_base.cpp:800-805 的注释）。中招就把这里改成 false 退到 HIGH。
        // 2026-09-30 实测：日志确认 REALTIME 已生效（HAGS=Enabled）但串流仍锁在 71fps，
        // 按上面那条规避退到 HIGH，验证是不是踩在这个已知 bug 上。
        constexpr bool kRealtimeWithHags = false;

        inline std::atomic<bool>& boostedFlag() {
            static std::atomic<bool> boosted = false;
            return boosted;
        }

        inline void boostStreamingPriority() {
            if (boostedFlag()) return;

            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

            timeBeginPeriod(1);

            boostedFlag() = true;
        }

        inline void restoreStreamingPriority() {
            if (!boostedFlag()) return;

            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
            timeEndPeriod(1);

            boostedFlag() = false;
        }

        inline void applyGpuDeviceLatency(ID3D11Device* device) {
            if (!device) return;

            static std::once_flag gpuPriorityOnceFlag;
            std::call_once(gpuPriorityOnceFlag, [&]() {
                // D3DKMTSetProcessSchedulingPriorityClass 需要 SeIncreaseBasePriorityPrivilege，
                // 不显式启用会失败，而返回值以前被丢掉了 —— 失败也看不出来。
                // 照 Sunshine display_base.cpp:751-765。
                HANDLE processToken = nullptr;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &processToken)) {
                    LUID privilegeLuid = {};
                    if (LookupPrivilegeValue(NULL, SE_INC_BASE_PRIORITY_NAME, &privilegeLuid)) {
                        TOKEN_PRIVILEGES privileges = {};
                        privileges.PrivilegeCount = 1;
                        privileges.Privileges[0].Luid = privilegeLuid;
                        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                        AdjustTokenPrivileges(processToken, FALSE, &privileges, sizeof(privileges), NULL, NULL);
                        // AdjustTokenPrivileges 只能启用令牌里已有的特权；令牌里没有时它仍返回 TRUE，
                        // 只把 GetLastError 设成 ERROR_NOT_ALL_ASSIGNED —— 原代码没查这一层，看不到。
                        if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                            LOG_WARN("SeIncreaseBasePriorityPrivilege Not Held By Token (Not Elevated?); Gpu Scheduling Priority Will Likely Fail");
                        }
                    }
                    CloseHandle(processToken);
                }

                HMODULE gdi32 = GetModuleHandleA("GDI32");
                if (!gdi32) return;

                auto d3dkmtSetProcessPriority =
                    (PD3DKMTSetProcessSchedulingPriorityClass)GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass");
                auto d3dkmtOpenAdapter = (PD3DKMTOpenAdapterFromLuid)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
                auto d3dkmtQueryAdapterInfo = (PD3DKMTQueryAdapterInfo)GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
                auto d3dkmtCloseAdapter = (PD3DKMTCloseAdapter)GetProcAddress(gdi32, "D3DKMTCloseAdapter");
                if (!d3dkmtSetProcessPriority) return;

                auto priority = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;

                // 取当前适配器的 VendorId + HAGS 状态,判断是否需要避开 REALTIME
                bool hagsEnabled = false;
                Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
                if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                        DXGI_ADAPTER_DESC desc{};
                        if (SUCCEEDED(adapter->GetDesc(&desc))) {
                            if (desc.VendorId == 0x10DE && d3dkmtOpenAdapter && d3dkmtQueryAdapterInfo && d3dkmtCloseAdapter) {
                                D3DKMT_OPENADAPTERFROMLUID openAdapter{ desc.AdapterLuid };
                                if (SUCCEEDED(d3dkmtOpenAdapter(&openAdapter))) {
                                    D3DKMT_WDDM_2_7_CAPS caps{};
                                    D3DKMT_QUERYADAPTERINFO queryInfo{};
                                    queryInfo.hAdapter = openAdapter.hAdapter;
                                    queryInfo.Type = KMTQAITYPE_WDDM_2_7_CAPS;
                                    queryInfo.pPrivateDriverData = &caps;
                                    queryInfo.PrivateDriverDataSize = sizeof(caps);
                                    if (SUCCEEDED(d3dkmtQueryAdapterInfo(&queryInfo))) {
                                        hagsEnabled = caps.HwSchEnabled != 0;
                                    }
                                    D3DKMT_CLOSEADAPTER closeAdapter{ openAdapter.hAdapter };
                                    d3dkmtCloseAdapter(&closeAdapter);
                                }
                            }

                            // 照 Sunshine display_base.cpp:803：默认 REALTIME，只有显式关掉
                            // kRealtimeWithHags 才退 HIGH。原来这里是「NVIDIA + HAGS 就无条件降 HIGH」，
                            // 等于把 Sunshine 的默认档位砍掉了。
                            if (desc.VendorId == 0x10DE && hagsEnabled && !kRealtimeWithHags) {
                                priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
                            }
                        }
                    }
                }

                const bool realtimePriority = priority == D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
                const NTSTATUS priorityStatus = d3dkmtSetProcessPriority(GetCurrentProcess(), priority);
                if (FAILED(priorityStatus)) {
                    LOG_WARN("Gpu Scheduling Priority Set Failed: Status=0x{:X} HAGS={} Priority={} (Needs SeIncreaseBasePriorityPrivilege)",
                        static_cast<unsigned int>(priorityStatus),
                        hagsEnabled ? "Enabled" : "Disabled",
                        realtimePriority ? "Realtime" : "High");
                }
                else {
                    LOG_INFO("Gpu Scheduling Priority Applied: HAGS={} Priority={}",
                        hagsEnabled ? "Enabled" : "Disabled",
                        realtimePriority ? "Realtime" : "High");
                }
            });

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                dxgiDevice->SetGPUThreadPriority(7);
            }

            Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice1))) {
                dxgiDevice1->SetMaximumFrameLatency(1);
            }
        }

    }
}

#endif  // _WIN32
#endif  // PERFBOOST_H
