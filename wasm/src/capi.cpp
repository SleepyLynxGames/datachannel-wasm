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

// Implementation of the C API in rtc/rtc.h on top of rtc::PeerConnection and rtc::DataChannel. See rtc/rtc.h for what
// is supported.
//
// Peers and channels share one id space, like in libdatachannel, so rtcSetUserPointer() can take either. Callbacks are
// registered once on the C++ objects and look the record up by id, because the browser can report something after the
// record is gone. Exceptions are not enabled in the Emscripten build by default, so nothing here throws or catches.

#include "rtc/rtc.h"
#include "rtc/rtc.hpp"

#include <chrono>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxMessageSize = 65536;

struct PeerRec {
	int id = 0;
	std::unique_ptr<rtc::PeerConnection> pc;
	bool closed = false;
	void *user = nullptr;

	rtcDescriptionCallbackFunc onDescription = nullptr;
	rtcCandidateCallbackFunc onCandidate = nullptr;
	rtcStateChangeCallbackFunc onState = nullptr;
	rtcGatheringStateCallbackFunc onGathering = nullptr;
	rtcDataChannelCallbackFunc onDataChannel = nullptr;
};

struct ChannelRec {
	int id = 0;
	std::shared_ptr<rtc::DataChannel> dc;
	bool closed = false;
	void *user = nullptr;

	rtcOpenCallbackFunc onOpen = nullptr;
	rtcClosedCallbackFunc onClosed = nullptr;
	rtcErrorCallbackFunc onError = nullptr;
	rtcMessageCallbackFunc onMessage = nullptr;
	rtcBufferedAmountLowCallbackFunc onBufferedLow = nullptr;

	// Messages that arrived before the message callback was set.
	std::vector<rtc::message_variant> pending;
};

int g_nextId = 1;
std::unordered_map<int, std::unique_ptr<PeerRec>> g_peers;
std::unordered_map<int, std::unique_ptr<ChannelRec>> g_channels;

PeerRec *findPeer(int id) {
	auto it = g_peers.find(id);
	return it == g_peers.end() ? nullptr : it->second.get();
}

ChannelRec *findChannel(int id) {
	auto it = g_channels.find(id);
	return it == g_channels.end() ? nullptr : it->second.get();
}

void deliver(ChannelRec &ch, const rtc::message_variant &message) {
	if (!ch.onMessage)
		return;

	// Strings are reported with a negative size, -(length + 1), like libdatachannel does.
	if (const rtc::string *text = std::get_if<rtc::string>(&message)) {
		ch.onMessage(ch.id, text->c_str(), -static_cast<int>(text->size()) - 1, ch.user);
	} else {
		const rtc::binary &bytes = std::get<rtc::binary>(message);
		ch.onMessage(ch.id, reinterpret_cast<const char *>(bytes.data()),
		             static_cast<int>(bytes.size()), ch.user);
	}
}

int registerChannel(std::shared_ptr<rtc::DataChannel> dc) {
	int id = g_nextId++;
	auto rec = std::make_unique<ChannelRec>();
	rec->id = id;
	rec->dc = dc;

	dc->onOpen([id]() {
		ChannelRec *ch = findChannel(id);
		if (ch && ch->onOpen)
			ch->onOpen(id, ch->user);
	});
	dc->onClosed([id]() {
		ChannelRec *ch = findChannel(id);
		if (!ch)
			return;
		ch->closed = true;
		if (ch->onClosed)
			ch->onClosed(id, ch->user);
	});
	dc->onError([id](std::string error) {
		ChannelRec *ch = findChannel(id);
		if (ch && ch->onError)
			ch->onError(id, error.c_str(), ch->user);
	});
	dc->onMessage([id](rtc::message_variant message) {
		ChannelRec *ch = findChannel(id);
		if (!ch)
			return;
		if (!ch->onMessage) {
			ch->pending.push_back(std::move(message));
			return;
		}
		deliver(*ch, message);
	});
	dc->onBufferedAmountLow([id]() {
		ChannelRec *ch = findChannel(id);
		if (ch && ch->onBufferedLow)
			ch->onBufferedLow(id, ch->user);
	});

	g_channels[id] = std::move(rec);
	return id;
}

int copyString(const std::string &s, char *buffer, int size) {
	int needed = static_cast<int>(s.size()) + 1;
	if (!buffer)
		return needed;
	if (size < needed)
		return RTC_ERR_TOO_SMALL;

	std::memcpy(buffer, s.c_str(), static_cast<size_t>(needed));
	return needed;
}

std::string percentDecode(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
		    std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
			i += 2;
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

// libdatachannel takes TURN credentials inside the URL (turn:user:pass@host:port, percent-encoded), the browser wants
// them apart from it.
rtc::IceServer parseIceServer(const char *url) {
	std::string s(url);
	std::string username, password;

	size_t schemeEnd = s.find(':');
	size_t at = schemeEnd == std::string::npos ? std::string::npos : s.find('@', schemeEnd + 1);
	size_t query = s.find('?');
	if (at != std::string::npos && (query == std::string::npos || at < query)) {
		std::string userinfo = s.substr(schemeEnd + 1, at - schemeEnd - 1);
		size_t colon = userinfo.find(':');
		username = percentDecode(userinfo.substr(0, colon));
		if (colon != std::string::npos)
			password = percentDecode(userinfo.substr(colon + 1));
		s = s.substr(0, schemeEnd + 1) + s.substr(at + 1);
	}

	// The string constructor passes the URL to the browser as it is.
	rtc::IceServer server(s);
	server.username = std::move(username);
	server.password = std::move(password);
	return server;
}

} // namespace

extern "C" {

void rtcSetUserPointer(int id, void *ptr) {
	if (PeerRec *peer = findPeer(id))
		peer->user = ptr;
	else if (ChannelRec *ch = findChannel(id))
		ch->user = ptr;
}

void *rtcGetUserPointer(int id) {
	if (PeerRec *peer = findPeer(id))
		return peer->user;
	if (ChannelRec *ch = findChannel(id))
		return ch->user;
	return nullptr;
}

int rtcCreatePeerConnection(const rtcConfiguration *config) {
	if (!config)
		return RTC_ERR_INVALID;

	rtc::Configuration rtcConfig;
	for (int i = 0; i < config->iceServersCount; ++i)
		rtcConfig.iceServers.push_back(parseIceServer(config->iceServers[i]));
	if (config->iceTransportPolicy == RTC_TRANSPORT_POLICY_RELAY)
		rtcConfig.iceTransportPolicy = rtc::TransportPolicy::Relay;

	int id = g_nextId++;
	auto rec = std::make_unique<PeerRec>();
	rec->id = id;
	rec->pc = std::make_unique<rtc::PeerConnection>(rtcConfig);

	rec->pc->onLocalDescription([id](const rtc::Description &description) {
		PeerRec *peer = findPeer(id);
		if (!peer || !peer->onDescription)
			return;
		std::string sdp = std::string(description);
		std::string type = description.typeString();
		peer->onDescription(id, sdp.c_str(), type.c_str(), peer->user);
	});
	rec->pc->onLocalCandidate([id](const rtc::Candidate &candidate) {
		PeerRec *peer = findPeer(id);
		if (!peer || !peer->onCandidate)
			return;
		std::string text = candidate.candidate();
		std::string mid = candidate.mid();
		peer->onCandidate(id, text.c_str(), mid.c_str(), peer->user);
	});
	rec->pc->onStateChange([id](rtc::PeerConnection::State state) {
		PeerRec *peer = findPeer(id);
		if (peer && peer->onState)
			peer->onState(id, static_cast<rtcState>(static_cast<int>(state)), peer->user);
	});
	rec->pc->onGatheringStateChange([id](rtc::PeerConnection::GatheringState state) {
		PeerRec *peer = findPeer(id);
		if (peer && peer->onGathering)
			peer->onGathering(id, static_cast<rtcGatheringState>(static_cast<int>(state)),
			                  peer->user);
	});
	rec->pc->onDataChannel([id](std::shared_ptr<rtc::DataChannel> dc) {
		PeerRec *peer = findPeer(id);
		if (!peer || !peer->onDataChannel)
			return;
		peer->onDataChannel(id, registerChannel(std::move(dc)), peer->user);
	});

	g_peers[id] = std::move(rec);
	return id;
}

int rtcClosePeerConnection(int pc) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	if (!peer->closed) {
		peer->closed = true;
		peer->pc->close();
	}
	return RTC_ERR_SUCCESS;
}

int rtcDeletePeerConnection(int pc) {
	if (!g_peers.erase(pc))
		return RTC_ERR_INVALID;
	return RTC_ERR_SUCCESS;
}

int rtcSetLocalDescriptionCallback(int pc, rtcDescriptionCallbackFunc cb) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	peer->onDescription = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetLocalCandidateCallback(int pc, rtcCandidateCallbackFunc cb) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	peer->onCandidate = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetStateChangeCallback(int pc, rtcStateChangeCallbackFunc cb) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	peer->onState = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetGatheringStateChangeCallback(int pc, rtcGatheringStateCallbackFunc cb) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	peer->onGathering = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetDataChannelCallback(int pc, rtcDataChannelCallbackFunc cb) {
	PeerRec *peer = findPeer(pc);
	if (!peer)
		return RTC_ERR_INVALID;
	peer->onDataChannel = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetRemoteDescription(int pc, const char *sdp, const char *type) {
	PeerRec *peer = findPeer(pc);
	if (!peer || !sdp || !type || peer->closed)
		return RTC_ERR_INVALID;

	peer->pc->setRemoteDescription(rtc::Description(std::string(sdp), std::string(type)));
	return RTC_ERR_SUCCESS;
}

int rtcAddRemoteCandidate(int pc, const char *cand, const char *mid) {
	PeerRec *peer = findPeer(pc);
	if (!peer || !cand || peer->closed)
		return RTC_ERR_INVALID;

	peer->pc->addRemoteCandidate(rtc::Candidate(std::string(cand), std::string(mid ? mid : "")));
	return RTC_ERR_SUCCESS;
}

int rtcCreateDataChannel(int pc, const char *label) {
	return rtcCreateDataChannelEx(pc, label, nullptr);
}

int rtcCreateDataChannelEx(int pc, const char *label, const rtcDataChannelInit *init) {
	PeerRec *peer = findPeer(pc);
	if (!peer || peer->closed)
		return RTC_ERR_INVALID;
	if (init && init->negotiated && !init->manualStream)
		return RTC_ERR_INVALID;

	rtc::DataChannelInit options;
	if (init) {
		if (init->protocol)
			options.protocol = init->protocol;
		if (init->negotiated) {
			options.negotiated = true;
			options.id = init->stream;
		}
		options.reliability.unordered = init->reliability.unordered;
		if (init->reliability.unreliable) {
			// Only one of the two may be set, the lifetime takes precedence like in libdatachannel.
			if (init->reliability.maxPacketLifeTime)
				options.reliability.maxPacketLifeTime =
				    std::chrono::milliseconds(init->reliability.maxPacketLifeTime);
			else
				options.reliability.maxRetransmits = init->reliability.maxRetransmits;
		}
	}

	std::shared_ptr<rtc::DataChannel> dc =
	    peer->pc->createDataChannel(label ? label : "", options);
	if (!dc)
		return RTC_ERR_FAILURE;
	return registerChannel(std::move(dc));
}

int rtcDeleteDataChannel(int dc) {
	if (!g_channels.erase(dc))
		return RTC_ERR_INVALID;
	return RTC_ERR_SUCCESS;
}

int rtcDelete(int id) { return rtcDeleteDataChannel(id); }

int rtcSetOpenCallback(int id, rtcOpenCallbackFunc cb) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	ch->onOpen = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetClosedCallback(int id, rtcClosedCallbackFunc cb) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	ch->onClosed = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetErrorCallback(int id, rtcErrorCallbackFunc cb) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	ch->onError = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSetMessageCallback(int id, rtcMessageCallbackFunc cb) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	ch->onMessage = cb;

	// Deliver what arrived before there was somebody to receive it.
	if (cb && !ch->pending.empty()) {
		std::vector<rtc::message_variant> pending = std::move(ch->pending);
		ch->pending.clear();
		for (const rtc::message_variant &message : pending)
			deliver(*ch, message);
	}
	return RTC_ERR_SUCCESS;
}

int rtcSetBufferedAmountLowCallback(int id, rtcBufferedAmountLowCallbackFunc cb) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	ch->onBufferedLow = cb;
	return RTC_ERR_SUCCESS;
}

int rtcSendMessage(int id, const char *data, int size) {
	ChannelRec *ch = findChannel(id);
	if (!ch || ch->closed || (!data && size))
		return RTC_ERR_INVALID;

	bool sent;
	if (size < 0)
		sent = ch->dc->send(rtc::message_variant(std::string(data)));
	else
		sent = ch->dc->send(reinterpret_cast<const rtc::byte *>(data), static_cast<size_t>(size));
	return sent ? RTC_ERR_SUCCESS : RTC_ERR_FAILURE;
}

int rtcClose(int id) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	if (!ch->closed)
		ch->dc->close();
	return RTC_ERR_SUCCESS;
}

bool rtcIsOpen(int id) {
	ChannelRec *ch = findChannel(id);
	return ch && !ch->closed && ch->dc->isOpen();
}

bool rtcIsClosed(int id) {
	ChannelRec *ch = findChannel(id);
	return !ch || ch->closed;
}

int rtcGetBufferedAmount(int id) {
	ChannelRec *ch = findChannel(id);
	if (!ch)
		return RTC_ERR_INVALID;
	return static_cast<int>(ch->dc->bufferedAmount());
}

int rtcSetBufferedAmountLowThreshold(int id, int amount) {
	ChannelRec *ch = findChannel(id);
	if (!ch || amount < 0)
		return RTC_ERR_INVALID;
	ch->dc->setBufferedAmountLowThreshold(static_cast<size_t>(amount));
	return RTC_ERR_SUCCESS;
}

int rtcMaxMessageSize(int id) {
	if (!findChannel(id))
		return RTC_ERR_INVALID;
	return kMaxMessageSize;
}

int rtcGetDataChannelLabel(int dc, char *buffer, int size) {
	ChannelRec *ch = findChannel(dc);
	if (!ch)
		return RTC_ERR_INVALID;
	return copyString(ch->dc->label(), buffer, size);
}

int rtcGetDataChannelProtocol(int dc, char *buffer, int size) {
	ChannelRec *ch = findChannel(dc);
	if (!ch)
		return RTC_ERR_INVALID;
	return copyString(ch->dc->protocol(), buffer, size);
}

void rtcCleanup(void) {}

} // extern "C"
