#include "HAudioCatch.h"
#include <iostream>
#include "../../utils/Utils.h"

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

        HAudioCatch::~HAudioCatch() { closeEvent(); }

        bool HAudioCatch::initlize()
        {
            if (initlized.load()) return true;

            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr)) { LOG_ERROR("CoInitializeEx Failed: 0x{:08X}", hr); return false; }

            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), (void**)&immEnum);
            if (FAILED(hr)) { LOG_ERROR("Create MMDeviceEnumerator Failed: 0x{:08X}", hr); return false; }

            hr = immEnum->GetDefaultAudioEndpoint(eRender, eConsole, &immDevice);
            if (FAILED(hr)) { LOG_ERROR("GetDefaultAudioEndpoint Failed: 0x{:08X}", hr); return false; }

            hr = immDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&iAudioClient);
            if (FAILED(hr)) { LOG_ERROR("Activate IAudioClient Failed: 0x{:08X}", hr); return false; }

            WAVEFORMATEX* sysFmt = nullptr;
            hr = iAudioClient->GetMixFormat(&sysFmt);
            if (FAILED(hr)) { LOG_ERROR("GetMixFormat Failed: 0x{:08X}", hr); return false; }

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
                    LOG_INFO("Dummy Render Stream Started");
                }
                else {
                    LOG_WARN("Dummy Render Initialize Failed: 0x{:08X}", hr);
                }
            }

            hr = iAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                10000000, 0, pwfx, nullptr);
            if (FAILED(hr)) { LOG_ERROR("Loopback Initialize Failed: 0x{:08X}", hr); return false; }

            hr = iAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&iAudioCaptureClient);
            if (FAILED(hr)) { LOG_ERROR("Get IAudioCaptureClient Failed: 0x{:08X}", hr); return false; }

            hr = iAudioClient->Start();
            if (FAILED(hr)) { LOG_ERROR("Start Loopback Failed: 0x{:08X}", hr); return false; }

            LOG_INFO("Loopback Capture Started: {} Ch, {} Hz, {} Bit",
                pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);

            initlized.store(true);
            return true;
        }

        bool HAudioCatch::asyncEvent()
        {
            if (asyncEvents.exchange(true)) return true;

            startTick = GetTickCount();
            LOG_INFO("Event Loop Thread Started");

            asyncEventThread = std::thread([this] {

                CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                const size_t bytesPerFrame = pwfx->nBlockAlign;
                const size_t samplesPerSec = pwfx->nSamplesPerSec;
                const size_t framesPer10ms = samplesPerSec / 100;
                const size_t bytesPer10ms = framesPer10ms * bytesPerFrame;

                std::vector<BYTE> silence10ms(bytesPer10ms, 0);

                while (asyncEvents.load()) {
                    bool hasData = false;
                    UINT32 packetSize = 0;
                    HRESULT hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);

                    while (SUCCEEDED(hr) && packetSize > 0) {
                        hasData = true;
                        BYTE* data;
                        UINT32 frames;
                        DWORD flags;

                        hr = iAudioCaptureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                        if (SUCCEEDED(hr)) {
                            size_t currentBytes = frames * bytesPerFrame;

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                if (currentBytes == bytesPer10ms) {
                                    dataHandle(silence10ms.data(), bytesPer10ms);
                                }
                                else {
                                    std::vector<BYTE> tempSilence(currentBytes, 0);
                                    dataHandle(tempSilence.data(), currentBytes);
                                }
                            }
                            else {
                                dataHandle(data, currentBytes);
                            }

                            iAudioCaptureClient->ReleaseBuffer(frames);
                        }

                        hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);
                    }

                    if (!hasData) {
                        Sleep(3);
                    }
                    else {

                    }
                }

                CoUninitialize();
                LOG_DEBUG("Event Loop Thread Exit");
                });

            return true;
        }

        void HAudioCatch::closeEvent()
        {
            if (!asyncEvents.exchange(false)) return;

            if (asyncEventThread.joinable()) {
                asyncEventThread.join();
            }

            LOG_INFO("Stopping Audio Capture Resources");

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

            CoUninitialize();

            initlized.store(false);

            LOG_INFO("HAudioCatch Released");
        }

        void HAudioCatch::setDataHandle(std::function<void(unsigned char*, size_t)> fn)
        {
            dataHandle = fn;
        }

    }
} // namespace hope::rtc