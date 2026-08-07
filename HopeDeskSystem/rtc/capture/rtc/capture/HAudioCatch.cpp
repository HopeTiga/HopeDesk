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
                    LOG_WARN("Dummy render Initialize failed: 0x%08X", hr);
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

            // ��ʱ iAudioClient �Ѿ��� initlize �� Start() ���ˣ���Ҫ�ٶ���

            // ע�⣺��Ҫ�����ﴫ &silenceBuffer��Ҫ���߳��ڲ���������ֹҰָ��
            eventLoopThread = std::thread([this] {
                // �����̳߳�ʼ�� COM (��ֹ����ϵͳ�ϵ��� WASAPI �ӿڱ���)
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                // 1. ���� 10ms ������ (WebRTC �Ƽ� 10ms ��Ƭ)
                const size_t bytesPerFrame = pwfx->nBlockAlign;
                const size_t samplesPerSec = pwfx->nSamplesPerSec;
                // 10ms ��֡�� = ������ / 100
                const size_t framesPer10ms = samplesPerSec / 100;
                const size_t bytesPer10ms = framesPer10ms * bytesPerFrame;

                // 2. ׼��ȫ 0 �ľ���� (���߳�ջ�Ϸ��䣬��ȫ)
                std::vector<BYTE> silence10ms(bytesPer10ms, 0);

                while (eventLoopRunning.load()) {
                    bool hasData = false;
                    UINT32 packetSize = 0;
                    HRESULT hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);

                    // ѭ����ȡ���������ѹ����������
                    while (SUCCEEDED(hr) && packetSize > 0) {
                        hasData = true;
                        BYTE* data;
                        UINT32 frames;
                        DWORD flags;

                        hr = iAudioCaptureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                        if (SUCCEEDED(hr)) {
                            size_t currentBytes = frames * bytesPerFrame;

                            // === �����޸����� ===
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                // ��Ȼ�Ǿ������ȫ0���ݡ�
                                // �ؼ������밴�յ�ǰ frames �ĳ��ȷ�������Ϲ���̶�����
                                if (currentBytes == bytesPer10ms) {
                                    // �Ż�������Ǳ�׼�� 10ms��ֱ����Ԥ����õ�
                                    dataHandle(silence10ms.data(), bytesPer10ms);
                                }
                                else {
                                    // ֻ�г��Ȳ���׼ʱ����ʱ���䣬�������ܿ���
                                    std::vector<BYTE> tempSilence(currentBytes, 0);
                                    dataHandle(tempSilence.data(), currentBytes);
                                }
                            }
                            else {
                                // ���������������
                                dataHandle(data, currentBytes);
                            }

                            iAudioCaptureClient->ReleaseBuffer(frames);
                        }

                        // �����һ��
                        hr = iAudioCaptureClient->GetNextPacketSize(&packetSize);
                    }

                    if (!hasData) {
                        // === �����߼� ===
                        // ֻ������ȫ���������ݣ�Loopback û����� buffer ���ˣ�ʱ
                        // �ʵ� sleep ��ֹ CPU 100%
                        // ��Ϊ���� dummyRenderClient�����������Ｋ�ٻ᳤ʱ��û����
                        Sleep(3);
                    }
                    else {
                        // ������������ݣ���Ҫ sleep�����̳��Զ���һ�飬ֱ������
                        // �����ܽ����ӳ�
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

            // 1. ���ñ�־λ
            eventLoopRunning.store(false);

            // 2. ���ؼ�������ȴ��߳���ȫ�˳��������߳��ﻹ���� captureClient ʱ��Ͱ��� Release ��
            if (eventLoopThread.joinable()) {
                eventLoopThread.join();
            }

            LOG_INFO("Stopping audio capture resources");

            // 3. ��ȫֹͣ���ͷ���Դ
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

            // initlize ��� CoInitializeEx ��Ӧ����� Uninitialize
            // ������ initlize �����һ�Σ����������� stop ʱ��һ�μ���
            CoUninitialize();

            initlized.store(false); // ���ó�ʼ��״̬�������ٴ� init
            LOG_INFO("HAudioCatch released");
        }

        void HAudioCatch::setDataHandle(std::function<void(unsigned char*, size_t)> fn)
        {
            dataHandle = fn;
        }

    }
} // namespace hope::rtc