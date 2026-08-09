#ifndef EXAMPLECLIENT_AUDIO_HELPERS_HPP
#define EXAMPLECLIENT_AUDIO_HELPERS_HPP

#include "mumble/Opus.hpp"
#include "mumble/Peer.hpp"
#include "mumble/Message.hpp"
#include "mumble/Pack.hpp"
#include "mumble/Types.hpp"
#include "mumble/CryptOCB2.hpp"

#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace example {

struct AudioConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 1;
    std::uint32_t framesPerBuffer = 960;
    bool opusEnabled = true;
};

struct AudioState {
    PaStream *stream = nullptr;
    std::unique_ptr<mumble::Opus::Encoder> encoder;
    mumble::CryptOCB2 cryptSend;
    mumble::CryptOCB2 cryptRecv;
    bool cryptReady = false;
    std::vector<std::byte> sendNonce;
    std::vector<std::byte> recvNonce;
    mumble::Peer::FeedbackUDP udpFeedback;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> frameNumber{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::byte> encodeBuffer;
    std::vector<int16_t> captureBuffer;
    mumble::Endpoint serverEndpoint;
    mumble::Peer *peer = nullptr;
};

bool initializePortAudio();
void terminatePortAudio();

bool openInputStream(AudioState &state, const AudioConfig &config);
void closeInputStream(AudioState &state);

bool initializeOpusEncoder(AudioState &state, const AudioConfig &config);

bool setupCrypto(AudioState &state, const std::vector<std::byte> &key,
                 const std::vector<std::byte> &clientNonce,
                 const std::vector<std::byte> &serverNonce);

bool captureAndSendLoop(AudioState &state, const AudioConfig &config);

} // namespace example

#endif // EXAMPLECLIENT_AUDIO_HELPERS_HPP