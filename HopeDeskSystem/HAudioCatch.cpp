#include "HAudioCatch.h"
#include <iostream>
#include "Utils.h"

namespace hope {
    namespace rtc {

        static const WAVEFORMATEX kDefaultFmt = {
            WAVE_FORMAT_PCM, 2, 48000, 48000 * 2 * 2, 4, 16, 0
        };

        HAudioCatch::HAudioCatch(const WAVEFORMATEX& fmt)
            : userFmt(fmt),
            immEnum(nullptr),
            immDevice(nullptr),
            iAudioClient(nullptr),
            dummyRenderClient(nullptr),
            iAudioCaptureClient(nullptr),
            pwfx(nullptr)
        {
            if (userFmt.nSamplesPerSec == 0 && userFmt.nChannels == 0)
                userFmt = kDefaultFmt;
        }

        HAudioCatch::~HAudioCatch() { stopEventLoop(); }

        bool HAudioCatch::initlize()
        {
            if (initlized.load()) return true;

            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr)) { LOG_ERROR("CoInitializeEx failed: 0x%08X", hr); return false; }

            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), (void**)&immEnum);
            if (FAILED(hr)) { LOG_ERROR("Create MMDeviceEnumerator failed: 0x%08X", hr); return false; }

            hr = immEnum->GetDefaultAudioEndpoint(eRender, eConsole, &immDevice);
            if (FAILED(hr)) { LOG_ERROR("GetDefaultAudioEndpoint failed: 0x%08X", hr); return false; }

            hr = immDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&iAudioClient);
            if (FAILED(hr)) { LOG_ERROR("Activate IAudioClient failed: 0x%08X", hr); return false; }

            WAVEFORMATEX* sysFmt = nullptr;
            hr = iAudioClient->GetMixFormat(&sysFmt);
            if (FAILED(hr)) { LOG_ERROR("GetMixFormat failed: 0x%08X", hr); return false; }

            bool useUser = userFmt.wFormatTag == sysFmt->wFormatTag &&
                userFmt.nChannels == sysFmt->nChannels &&
                userFmt.nSamplesPerSec == sysFmt->nSamplesPerSec &&
                userFmt.wBitsPerSample == sysFmt->wBitsPerSample;
            pwfx = useUser ? sysFmt : &userFmt;

            hr = immDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&dummyRenderClient);
            if (SUCCEEDED(hr)) {
                hr = dummyRenderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, nullptr);
                if (SUCCEEDED(hr)) {
                    dummyRenderClient->Start();
                    LOG_INFO("Dummy render stream started");
                }
                else {
                    LOG_WARNING("Dummy render Initialize failed: 0x%08X", hr);
                }
            }

            hr = iAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                10000000, 0, pwfx, nullptr);
            if (FAILED(hr)) { LOG_ERROR("Loopback Initialize failed: 0x%08X", hr); return false; }

            hr = iAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&iAudioCaptureClient);
            if (FAILED(hr)) { LOG_ERROR("Get IAudioCaptureClient failed: 0x%08X", hr); return false; }

            hr = iAudioClient->Start();
            if (FAILED(hr)) { LOG_ERROR("Start loopback failed: 0x%08X", hr); return false; }

            LOG_INFO("Loopback capture started: %d ch, %d Hz, %d bit",
                pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);

            initlized.store(true);
            return true;
        }

        bool HAudioCatch::runEventLoop()
        {
            if (eventLoopRunning.load()) return true;
            eventLoopRunning.store(true);

            startTick = GetTickCount();
            LOG_INFO("Event loop thread started");

            // 此时 iAudioClient 已经在 initlize 中 Start() 过了，不要再动它

            // 注意：不要在这里传 &silenceBuffer，要在线程内部创建，防止野指针
            eventLoopThread = std::thread([this] {
                // 在新线程初始化 COM (防止部分系统上调用 WASAPI 接口崩溃)
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                // 1. 计算 10ms 数据量 (WebRTC 推荐 10ms 切片)
                const size_t bytesPerFrame = pwfx->nBlockAlign;
                const size_t samplesPerSec = pwfx->nSamplesPerSec;
                // 10ms 的帧数 = 采样率 / 100
                const size_t framesPer10ms = samplesPerSec / 100;
                const size_t bytesPer10ms = framesPer10ms * bytesPerFrame;

                // 2. 准备全 0 的静音包 (在线程栈上分配，安全)
                std::vector<BYTE> silence10ms(bytesPer10ms, 0);

                while (eventLoopRunning.load()) {
                    bool hasData = false;
                    UINT32 packetSize = 0;
                    HRESULT hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);

                    // 循环读取缓冲区里积压的所有数据
                    while (SUCCEEDED(hr) && packetSize > 0) {
                        hasData = true;
                        BYTE* data;
                        UINT32 frames;
                        DWORD flags;

                        hr = iAudioCaptureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                        if (SUCCEEDED(hr)) {
                            size_t currentBytes = frames * bytesPerFrame;

                            // === 音质修复核心 ===
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                // 既然是静音，发全0数据。
                                // 关键：必须按照当前 frames 的长度发，不能瞎发固定长度
                                if (currentBytes == bytesPer10ms) {
                                    // 优化：如果是标准的 10ms，直接用预分配好的
                                    dataHandle(silence10ms.data(), bytesPer10ms);
                                }
                                else {
                                    // 只有长度不标准时才临时分配，减少性能开销
                                    std::vector<BYTE> tempSilence(currentBytes, 0);
                                    dataHandle(tempSilence.data(), currentBytes);
                                }
                            }
                            else {
                                // 正常有声音的数据
                                dataHandle(data, currentBytes);
                            }

                            iAudioCaptureClient->ReleaseBuffer(frames);
                        }

                        // 检查下一包
                        hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);
                    }

                    if (!hasData) {
                        // === 兜底逻辑 ===
                        // 只有在完全读不到数据（Loopback 没声音或 buffer 空了）时
                        // 适当 sleep 防止 CPU 100%
                        // 因为你有 dummyRenderClient，理论上这里极少会长时间没数据
                        Sleep(3);
                    }
                    else {
                        // 如果读到了数据，不要 sleep，立刻尝试读下一块，直到读空
                        // 这样能降低延迟
                    }
                }

                CoUninitialize();
                LOG_DEBUG("Event loop thread exit");
                });

            return true;
        }

        void HAudioCatch::stopEventLoop()
        {
            if (!eventLoopRunning.load()) return;

            // 1. 先置标志位
            eventLoopRunning.store(false);

            // 2. 【关键】必须等待线程完全退出，否则线程里还在用 captureClient 时你就把它 Release 了
            if (eventLoopThread.joinable()) {
                eventLoopThread.join();
            }

            LOG_INFO("Stopping audio capture resources");

            // 3. 安全停止和释放资源
            if (iAudioClient) {
                iAudioClient->Stop();
            }
            if (dummyRenderClient) {
                dummyRenderClient->Stop();
            }

            if (iAudioCaptureClient) {
                iAudioCaptureClient->Release();
                iAudioCaptureClient = nullptr;
            }
            if (iAudioClient) {
                iAudioClient->Release();
                iAudioClient = nullptr;
            }
            if (dummyRenderClient) {
                dummyRenderClient->Release();
                dummyRenderClient = nullptr;
            }
            if (immDevice) {
                immDevice->Release();
                immDevice = nullptr;
            }
            if (immEnum) {
                immEnum->Release();
                immEnum = nullptr;
            }

            if (pwfx) {
                pwfx = nullptr;
            }

            // initlize 里的 CoInitializeEx 对应这里的 Uninitialize
            // 但你在 initlize 里调了一次，在类析构或 stop 时调一次即可
            CoUninitialize();

            initlized.store(false); // 重置初始化状态，允许再次 init
            LOG_INFO("HAudioCatch released");
        }

        void HAudioCatch::setDataHandle(std::function<void(unsigned char*, size_t)> fn)
        {
            dataHandle = fn;
        }

    }
} // namespace hope::rtc