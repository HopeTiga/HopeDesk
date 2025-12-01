#pragma once
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/ref_counted_object.h>

namespace hope {

	namespace rtc {

        class WebRTCManager;
	
        class CreateOfferObserverImpl : public webrtc::CreateSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<CreateOfferObserverImpl> Create(
                WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateOfferObserverImpl>(
                    new webrtc::RefCountedObject<CreateOfferObserverImpl>(manager, pc));

            }

            CreateOfferObserverImpl(WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : manager(manager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateOfferObserverImpl() override = default;

        private:
            WebRTCManager* manager;


            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

        class CreateAnswerObserverImpl : public webrtc::CreateSessionDescriptionObserver {
        public:
            static webrtc::scoped_refptr<CreateAnswerObserverImpl> Create(
                WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateAnswerObserverImpl>(
                    new webrtc::RefCountedObject<CreateAnswerObserverImpl>(manager, pc));

            }

            CreateAnswerObserverImpl(WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : manager(manager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateAnswerObserverImpl() override = default;

        private:

            WebRTCManager* manager;

            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

	
	}

}

