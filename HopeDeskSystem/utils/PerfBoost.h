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
                Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
                if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                        DXGI_ADAPTER_DESC desc{};
                        if (SUCCEEDED(adapter->GetDesc(&desc))) {
                            bool hagsEnabled = false;
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

                            if (desc.VendorId == 0x10DE && hagsEnabled) {
                                priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
                            }
                        }
                    }
                }

                d3dkmtSetProcessPriority(GetCurrentProcess(), priority);
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
