/**
 * Copyright (c) 2017-2022 Paul-Louis Ageneau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// The subset of libdatachannel's C API (rtc/rtc.h) that datachannel-wasm implements on top of the browser's
// RTCPeerConnection, so that plain C code written against libdatachannel also builds for WebAssembly. Names, types and
// struct layouts are the same as in libdatachannel. Tracks, media, WebSockets, logging, the pull style receive API
// and manual local descriptions are not available.
//
// Differences in behavior:
// - Negotiation is always automatic, rtcSetLocalDescription() does not exist.
// - rtcConfiguration: only iceServers, iceServersCount and iceTransportPolicy are used, the rest is ignored. TURN
//   credentials are taken from the server URL (turn:user:pass@host:port, percent-encoded) like in libdatachannel and
//   handed to the browser separately.
// - rtcMaxMessageSize() always returns 65536.
// - After rtcClosePeerConnection(), the browser reports no more events (no state change to RTC_CLOSED).
// - A channel received through rtcSetDataChannelCallback() is already live. Its open and closed state is reported by
//   rtcIsOpen() and rtcIsClosed(), and messages received before rtcSetMessageCallback() are delivered when it is set.
//
// Everything runs on the browser's single thread.

#ifndef RTC_C_API
#define RTC_C_API

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define RTC_C_EXPORT
#define RTC_API

typedef enum {
	RTC_NEW = 0,
	RTC_CONNECTING = 1,
	RTC_CONNECTED = 2,
	RTC_DISCONNECTED = 3,
	RTC_FAILED = 4,
	RTC_CLOSED = 5
} rtcState;

typedef enum {
	RTC_GATHERING_NEW = 0,
	RTC_GATHERING_INPROGRESS = 1,
	RTC_GATHERING_COMPLETE = 2
} rtcGatheringState;

typedef enum {
	RTC_CERTIFICATE_DEFAULT = 0, // ECDSA
	RTC_CERTIFICATE_ECDSA = 1,
	RTC_CERTIFICATE_RSA = 2,
} rtcCertificateType;

typedef enum { RTC_TRANSPORT_POLICY_ALL = 0, RTC_TRANSPORT_POLICY_RELAY = 1 } rtcTransportPolicy;

#define RTC_ERR_SUCCESS 0
#define RTC_ERR_INVALID -1   // invalid argument
#define RTC_ERR_FAILURE -2   // runtime error
#define RTC_ERR_NOT_AVAIL -3 // element not available
#define RTC_ERR_TOO_SMALL -4 // buffer too small

typedef void(RTC_API *rtcDescriptionCallbackFunc)(int pc, const char *sdp, const char *type,
                                                  void *ptr);
typedef void(RTC_API *rtcCandidateCallbackFunc)(int pc, const char *cand, const char *mid,
                                                void *ptr);
typedef void(RTC_API *rtcStateChangeCallbackFunc)(int pc, rtcState state, void *ptr);
typedef void(RTC_API *rtcGatheringStateCallbackFunc)(int pc, rtcGatheringState state, void *ptr);
typedef void(RTC_API *rtcDataChannelCallbackFunc)(int pc, int dc, void *ptr);
typedef void(RTC_API *rtcOpenCallbackFunc)(int id, void *ptr);
typedef void(RTC_API *rtcClosedCallbackFunc)(int id, void *ptr);
typedef void(RTC_API *rtcErrorCallbackFunc)(int id, const char *error, void *ptr);
typedef void(RTC_API *rtcMessageCallbackFunc)(int id, const char *message, int size, void *ptr);
typedef void(RTC_API *rtcBufferedAmountLowCallbackFunc)(int id, void *ptr);

// User pointer
RTC_C_EXPORT void rtcSetUserPointer(int id, void *ptr);
RTC_C_EXPORT void *rtcGetUserPointer(int i);

// PeerConnection

typedef struct {
	const char **iceServers;
	int iceServersCount;
	const char *proxyServer;        // ignored
	const char *bindAddress;        // ignored
	rtcCertificateType certificateType; // ignored
	const char *certificatePemFile; // ignored
	const char *keyPemFile;         // ignored
	const char *keyPemPass;         // ignored
	rtcTransportPolicy iceTransportPolicy;
	bool enableIceTcp;              // ignored
	bool enableIceUdpMux;           // ignored
	bool disableAutoNegotiation;    // ignored
	bool forceMediaTransport;       // ignored
	uint16_t portRangeBegin;        // ignored
	uint16_t portRangeEnd;          // ignored
	int mtu;                        // ignored
	int maxMessageSize;             // ignored
	bool disableFingerprintVerification; // ignored
} rtcConfiguration;

RTC_C_EXPORT int rtcCreatePeerConnection(const rtcConfiguration *config); // returns pc id
RTC_C_EXPORT int rtcClosePeerConnection(int pc);
RTC_C_EXPORT int rtcDeletePeerConnection(int pc);

RTC_C_EXPORT int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb);
RTC_C_EXPORT int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb);
RTC_C_EXPORT int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb);
RTC_C_EXPORT int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb);

RTC_C_EXPORT int rtcSetRemoteDescription(int pc, const char *sdp, const char *type);
RTC_C_EXPORT int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid);

// DataChannel, Track, and WebSocket common API (DataChannel only here)

RTC_C_EXPORT int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb);
RTC_C_EXPORT int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb);
RTC_C_EXPORT int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb);
RTC_C_EXPORT int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb);
RTC_C_EXPORT int rtcSendMessage(int id, const char *data, int size); // size < 0 for a string
RTC_C_EXPORT int rtcClose(int id);
RTC_C_EXPORT int rtcDelete(int id);
RTC_C_EXPORT bool rtcIsOpen(int id);
RTC_C_EXPORT bool rtcIsClosed(int id);

RTC_C_EXPORT int rtcMaxMessageSize(int id);
RTC_C_EXPORT int rtcGetBufferedAmount(int id); // total size buffered to send
RTC_C_EXPORT int rtcSetBufferedAmountLowThreshold(int id, int amount);
RTC_C_EXPORT int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb);

// DataChannel

typedef struct {
	bool unordered;
	bool unreliable;
	unsigned int maxPacketLifeTime; // ignored if reliable
	unsigned int maxRetransmits;    // ignored if reliable
} rtcReliability;

typedef struct {
	rtcReliability reliability;
	const char *protocol; // empty string if NULL
	bool negotiated;
	bool manualStream;
	uint16_t stream; // numeric ID 0-65534, ignored if manualStream is false
} rtcDataChannelInit;

RTC_C_EXPORT int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb);
RTC_C_EXPORT int rtcCreateDataChannel(int pc, const char *label); // returns dc id
RTC_C_EXPORT int rtcCreateDataChannelEx(int pc, const char *label,
                                        const rtcDataChannelInit *init); // returns dc id
RTC_C_EXPORT int rtcDeleteDataChannel(int dc);

RTC_C_EXPORT int rtcGetDataChannelLabel(int dc, char *buffer, int size);
RTC_C_EXPORT int rtcGetDataChannelProtocol(int dc, char *buffer, int size);

// Global

// Nothing to clean up in the browser, this only exists for source compatibility.
RTC_C_EXPORT void rtcCleanup(void);

#ifdef __cplusplus
}
#endif

#endif
