#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <functional>

namespace hope {
    namespace rtc {

        class HAudioCatch
        {
        public:

            explicit HAudioCatch(const WAVEFORMATEX& fmt = {});

            ~HAudioCatch();

            bool initlize();

            bool asyncEvent();

            void closeEvent();

            void setDataHandle(std::function<void(unsigned char*, size_t)>);

        private:

            std::thread asyncEventThread;

            std::atomic<bool> asyncEvents{ false };

            std::atomic<bool> initlized{ false };

            std::function<void(unsigned char*, size_t)> dataHandle;

            IMMDeviceEnumerator* immEnum;

            IMMDevice* immDevice;

            IAudioClient* iAudioClient;

            IAudioClient* dummyRenderClient;

            IAudioCaptureClient* iAudioCaptureClient;

            WAVEFORMATEX  userFmt;  

            WAVEFORMATEX* pwfx ;

            DWORD startTick = 0;
        };

    }
} // namespace hope::rtc