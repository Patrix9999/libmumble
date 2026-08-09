// This file is part of libmumble.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "MumbleInit.hpp"
#include "audio_helpers.hpp"

#include "mumble/Connection.hpp"
#include "mumble/Lib.hpp"
#include "mumble/Message.hpp"
#include "mumble/Pack.hpp"
#include "mumble/Peer.hpp"
#include "mumble/Types.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <toml/get.hpp>
#include <toml/parser.hpp>
#include <toml/value.hpp>

using namespace mumble;

static Connection::Feedback connectionFeedback(Connection &connection, std::condition_variable &cv,
                                               std::string username, std::string password,
                                               example::AudioState &audioState) {
	using Message = tcp::Message;
	using Pack    = tcp::Pack;
	using Type    = Message::Type;

	Connection::Feedback feedback;

	feedback.opened = [&connection, username = std::move(username), password = std::move(password)]() {
		printf("Connection opened!\n");

		Message::Version ver;
		ver.version = lib::version();
		ver.release = "Custom client";
		connection.write(Pack(ver).buf());

		Message::Authenticate auth;
		auth.username = username;
		auth.password = password;
		auth.opus     = true;
		connection.write(Pack(auth).buf());

	};

	feedback.closed = [&cv]() {
		printf("Connection closed!\n");

		cv.notify_all();
	};

	feedback.failed = [&cv](const Code code) {
		printf("Connection failed with error \"%s\"!\n", text(code).data());

		cv.notify_all();
	};

	feedback.pack = [&audioState](Pack &pack) {
		const auto type = Message::type(pack);
		if (type == Type::CryptSetup) {
			Message::CryptSetup crypt;
			if (pack(crypt)) {
				if (example::setupCrypto(audioState, crypt.key, crypt.clientNonce, crypt.serverNonce)) {
					printf("UDP crypto configured!\n");
				} else {
					printf("Failed to configure UDP crypto!\n");
				}
			}
		}

		if (type != Type::UDPTunnel) {
			printf("%s received!\n", Message::text(type).data());
		}
	};

	return feedback;
}

static Peer::FeedbackTCP peerFeedback() {
	Peer::FeedbackTCP feedback;

	feedback.started = []() { printf("TCP started!\n"); };
	feedback.stopped = []() { printf("TCP stopped!\n"); };

	feedback.failed = [](const Code code) { printf("TCP failed with error \"%s\"!\n", text(code).data()); };

	feedback.timeout = []() { return 10000; };

	return feedback;
}

int32_t main(const int argc, const char **argv) {
	if (argc > 2) {
		printf("Usage: `example_client <config file>`\n");
		return 1;
	}

	std::string confPath = "config.toml";
	if (argc > 1) {
		confPath = argv[1];
	}

	const auto conf = toml::parse(confPath);

	MumbleInit mumbleInit;
	if (!mumbleInit) {
		return 2;
	}

	const auto local = toml::find(conf, "local");
	const auto peer  = toml::find(conf, "peer");
	const auto audio = toml::find(conf, "audio");

	const auto localTcpIP   = toml::find< std::string_view >(local, "tcpIP");
	const auto localTcpPort = toml::find< uint16_t >(local, "tcpPort");
	const auto localUdpIP   = toml::find< std::string_view >(local, "udpIP");
	const auto localUdpPort = toml::find< uint16_t >(local, "udpPort");

	const auto peerTcpIP   = toml::find< std::string_view >(peer, "tcpIP");
	const auto peerTcpPort = toml::find< uint16_t >(peer, "tcpPort");
	const auto peerUdpIP   = toml::find< std::string_view >(peer, "udpIP");
	const auto peerUdpPort = toml::find< uint16_t >(peer, "udpPort");

	const auto auth     = toml::find(conf, "auth");
	const auto username = toml::find< std::string_view >(auth, "username");
	const auto password = toml::find< std::string_view >(auth, "password");

	example::AudioConfig audioConfig;
	audioConfig.sampleRate = toml::find< uint32_t >(audio, "sampleRate");
	audioConfig.channels = toml::find< uint32_t >(audio, "channels");
	audioConfig.framesPerBuffer = toml::find< uint32_t >(audio, "framesPerBuffer");

	example::AudioState audioState;

	const auto ret = Peer::connect({ peerTcpIP, peerTcpPort }, { localTcpIP, localTcpPort });
	if (ret.first != Code::Success) {
		printf("Peer::connect() failed with error \"%s\"!\n", text(ret.first).data());
		return 3;
	}

	std::condition_variable cv;

	auto connection = std::make_shared< Connection >(ret.second, false);
	auto code       = (*connection)(connectionFeedback(*connection, cv, std::string(username), std::string(password), audioState));
	if (code != Code::Success) {
		printf("Connection() failed with error \"%s\"!\n", text(code).data());
		return 4;
	}

	Peer client;
	client.addTCP(connection);

	mumble::Endpoint localUdpEndpoint(mumble::IP(localUdpIP), localUdpPort);
	const auto udpBindCode = client.bindUDP(localUdpEndpoint);
	if (udpBindCode != Code::Success) {
		printf("Peer::bindUDP() failed with error \"%s\"!\n", text(udpBindCode).data());
		return 6;
	}

	Peer::FeedbackUDP udpFeedback;
	udpFeedback.started = []() { printf("UDP started!\n"); };
	udpFeedback.stopped = []() { printf("UDP stopped!\n"); };
	udpFeedback.failed = [](const Code code) { printf("UDP failed with error \"%s\"!\n", text(code).data()); };
	udpFeedback.timeout = []() { return 10000; };

	code = client.startUDP(udpFeedback);
	if (code != Code::Success) {
		printf("Peer::startUDP() failed with error \"%s\"!\n", text(code).data());
		return 7;
	}

	audioState.serverEndpoint = mumble::Endpoint(mumble::IP(peerUdpIP), peerUdpPort);
	audioState.peer = &client;

	if (!example::initializePortAudio()) {
		printf("Failed to initialize PortAudio!\n");
		return 8;
	}

	if (!example::initializeOpusEncoder(audioState, audioConfig)) {
		printf("Failed to initialize Opus encoder!\n");
		return 9;
	}

	if (!example::openInputStream(audioState, audioConfig)) {
		printf("Failed to open PortAudio input stream!\n");
		return 10;
	}

	audioState.running.store(true, std::memory_order_release);
	std::thread captureThread(example::captureAndSendLoop, std::ref(audioState), std::ref(audioConfig));

	code = client.startTCP(peerFeedback());
	if (code != Code::Success) {
		printf("Peer::startTCP() failed with error \"%s\"!\n", text(code).data());
		audioState.running.store(false, std::memory_order_release);
		audioState.cv.notify_all();
		captureThread.join();
		example::closeInputStream(audioState);
		example::terminatePortAudio();
		return 5;
	}

	std::mutex mutex;
	std::unique_lock< std::mutex > lock(mutex);
	cv.wait(lock);

	audioState.running.store(false, std::memory_order_release);
	audioState.cv.notify_all();
	captureThread.join();

	example::closeInputStream(audioState);
	example::terminatePortAudio();

	client.stopUDP();
	client.stopTCP();

	return 0;
}
