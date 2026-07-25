#pragma once
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/ref_counted_object.h>

namespace hope {

	namespace rtc {

        class WebrtcManager;
	
        class CreateOfferObserverImpl : public webrtc::CreateSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<CreateOfferObserverImpl> Create(
                WebrtcManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateOfferObserverImpl>(
                    new webrtc::RefCountedObject<CreateOfferObserverImpl>(manager, pc));

            }

            CreateOfferObserverImpl(WebrtcManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : webrtcManager(webrtcManager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateOfferObserverImpl() override = default;

        private:
            WebrtcManager* webrtcManager;


            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

        class CreateAnswerObserverImpl : public webrtc::CreateSessionDescriptionObserver {
        public:
            static webrtc::scoped_refptr<CreateAnswerObserverImpl> Create(
                WebrtcManager* webrtcManager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateAnswerObserverImpl>(
                    new webrtc::RefCountedObject<CreateAnswerObserverImpl>(webrtcManager, pc));

            }

            CreateAnswerObserverImpl(WebrtcManager* webrtcManager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : webrtcManager(webrtcManager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateAnswerObserverImpl() override = default;

        private:

            WebrtcManager* webrtcManager;

            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

	
	}

}

