#pragma once
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/ref_counted_object.h>

namespace hope {
	namespace rtc {
	
        class SetLocalDescriptionObserver : public webrtc::SetSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<SetLocalDescriptionObserver> Create() {

                return webrtc::scoped_refptr<SetLocalDescriptionObserver>(
                    new webrtc::RefCountedObject<SetLocalDescriptionObserver>());

            }

            void OnSuccess() override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            SetLocalDescriptionObserver() = default;

            ~SetLocalDescriptionObserver() override = default;

        };

        class SetRemoteDescriptionObserver : public webrtc::SetSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<SetRemoteDescriptionObserver> Create() {

                return webrtc::scoped_refptr<SetRemoteDescriptionObserver>(
                    new webrtc::RefCountedObject<SetRemoteDescriptionObserver>());

            }

            void OnSuccess() override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            SetRemoteDescriptionObserver() = default;

            ~SetRemoteDescriptionObserver() override = default;

        };

	}
   
}

