#include "audio_helpers.hpp"

#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace example {

bool initializePortAudio() {
    return Pa_Initialize() == paNoError;
}

void terminatePortAudio() {
    Pa_Terminate();
}

bool openInputStream(AudioState &state, const AudioConfig &config) {
    PaStreamParameters inputParameters{};
    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device == paNoDevice) {
        return false;
    }

    const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inputParameters.device);
    if (!deviceInfo) {
        return false;
    }

    inputParameters.channelCount = static_cast<int>(config.channels);
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = nullptr;

    const auto result = Pa_OpenStream(
        &state.stream,
        &inputParameters,
        nullptr,
        static_cast<double>(config.sampleRate),
        static_cast<unsigned long>(config.framesPerBuffer),
        paClipOff,
        nullptr,
        nullptr);

    if (result != paNoError) {
        return false;
    }

    if (Pa_StartStream(state.stream) != paNoError) {
        Pa_CloseStream(state.stream);
        state.stream = nullptr;
        return false;
    }

    state.captureBuffer.assign(config.framesPerBuffer * config.channels, 0);
    state.encodeBuffer.assign(2048, std::byte{0});

    return true;
}

void closeInputStream(AudioState &state) {
    if (!state.stream) {
        return;
    }

    Pa_StopStream(state.stream);
    Pa_CloseStream(state.stream);
    state.stream = nullptr;
}

bool initializeOpusEncoder(AudioState &state, const AudioConfig &config) {
    state.encoder = std::make_unique<mumble::Opus::Encoder>(static_cast<uint8_t>(config.channels));
    const auto code = state.encoder->init(config.sampleRate, mumble::Opus::Encoder::Preset::VoIP);
    if (code != mumble::Code::Success || !*state.encoder) {
        state.encoder.reset();
        return false;
    }

    state.captureBuffer.assign(config.framesPerBuffer * config.channels, 0);
    state.encodeBuffer.assign(2048, std::byte{0});

    return true;
}

bool setupCrypto(AudioState &state, const std::vector<std::byte> &key,
                 const std::vector<std::byte> &clientNonce,
                 const std::vector<std::byte> &serverNonce) {
    if (!state.cryptSend.setKey(key) || !state.cryptRecv.setKey(key)) {
        return false;
    }

    state.sendNonce = clientNonce;
    state.recvNonce = serverNonce;

    if (!state.cryptSend.setNonce(state.sendNonce) || !state.cryptRecv.setNonce(state.recvNonce)) {
        return false;
    }

    state.cryptReady = true;
    state.cv.notify_all();
    return true;
}

bool sendOpusPacket(AudioState &state, const std::vector<int16_t> &frame, mumble::Peer &peer) {
    if (!state.cryptReady || frame.empty() || !state.encoder) {
        return false;
    }

    const auto input = mumble::Opus::Encoder::IntegerViewConst(frame.data(), frame.size());
    const auto encoded = (*state.encoder)(state.encodeBuffer, input);
    if (encoded.empty()) {
        return false;
    }

    mumble::udp::Message::Audio audio;
    audio.direction = mumble::udp::Message::Audio::ClientToServer;
    audio.target = 0;
    const auto frameNumber = state.frameNumber.fetch_add(1, std::memory_order_relaxed);
    audio.frameNumber = frameNumber;
    audio.opusData.assign(encoded.begin(), encoded.end());
    audio.volumeAdjustment = 0.f;
    audio.isTerminator = false;

    for (auto &byte : state.sendNonce) {
        if (++*reinterpret_cast< uint8_t * >(&byte)) {
            break;
        }
    }

    if (!state.cryptSend.setNonce(state.sendNonce)) {
        printf("sendOpusPacket: failed to set send nonce\n");
        return false;
    }

    mumble::udp::Pack pack(audio);
    mumble::Buf encrypted(pack.buf().size() + 4);
    auto encryptedPayload = mumble::BufView(encrypted.data() + 4, encrypted.size() - 4);
    mumble::Buf tag(state.cryptSend.blockSize());
    const auto written = state.cryptSend.encrypt(encryptedPayload, pack.buf(), tag);
    if (!written) {
        printf("sendOpusPacket: encryption failed, payload=%zu\n", pack.buf().size());
        return false;
    }

    encrypted[0] = state.sendNonce[0];
    std::copy_n(tag.cbegin(), 3, encrypted.begin() + 1);

    const auto code = peer.sendUDP(state.serverEndpoint, mumble::BufViewConst(encrypted.data(), written + 4));
    if (code != mumble::Code::Success) {
        printf("sendOpusPacket: UDP send failed (%s)\n", text(code).data());
        return false;
    }

    return true;
}

bool captureAndSendLoop(AudioState &state, const AudioConfig &config) {
    if (!state.stream) {
        return false;
    }

    std::unique_lock<std::mutex> lock(state.mutex);
    while (state.running.load(std::memory_order_acquire)) {
        state.cv.wait(lock, [&state]() { return !state.running.load(std::memory_order_acquire) || state.cryptReady; });
        if (!state.running.load(std::memory_order_acquire)) {
            break;
        }

        lock.unlock();

        std::vector<int16_t> pendingFrame(static_cast<std::size_t>(config.framesPerBuffer) * config.channels);
        const auto firstResult = Pa_ReadStream(state.stream, pendingFrame.data(), static_cast<unsigned long>(config.framesPerBuffer));
        if (firstResult != paNoError) {
            lock.lock();
            break;
        }

        const auto packetDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(config.framesPerBuffer) / static_cast<double>(config.sampleRate)));
        const size_t sendBufferFrames = 12;
        std::deque<std::vector<int16_t>> sendQueue;
        sendQueue.emplace_back(std::move(pendingFrame));

        while (state.running.load(std::memory_order_acquire) && sendQueue.size() < sendBufferFrames) {
            std::vector<int16_t> nextFrame(static_cast<std::size_t>(config.framesPerBuffer) * config.channels);
            const auto nextResult = Pa_ReadStream(state.stream, nextFrame.data(), static_cast<unsigned long>(config.framesPerBuffer));
            if (nextResult != paNoError) {
                lock.lock();
                return false;
            }
            sendQueue.emplace_back(std::move(nextFrame));
        }

        auto nextSendTime = std::chrono::steady_clock::now() + packetDuration;
        auto lastSendTime = nextSendTime - packetDuration;

        while (state.running.load(std::memory_order_acquire)) {
            auto nextFrame = std::vector<int16_t>(static_cast<std::size_t>(config.framesPerBuffer) * config.channels);
            const auto nextResult = Pa_ReadStream(state.stream, nextFrame.data(), static_cast<unsigned long>(config.framesPerBuffer));
            if (nextResult != paNoError) {
                lock.lock();
                break;
            }

            sendQueue.emplace_back(std::move(nextFrame));

            const auto now = std::chrono::steady_clock::now();
            if (now < nextSendTime) {
                std::this_thread::sleep_until(nextSendTime);
            } else {
                const auto drift = std::chrono::duration_cast<std::chrono::milliseconds>(now - nextSendTime).count();
                if (drift > static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(packetDuration).count())) {
                    printf("audio send drifted=%lldms, resyncing\n", static_cast<long long>(drift));
                    nextSendTime = now;
                }
            }

            if (!sendOpusPacket(state, sendQueue.front(), *state.peer)) {
                lock.lock();
                break;
            }

            const auto sendTime = std::chrono::steady_clock::now();
            const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(sendTime - lastSendTime).count();
            printf("audio send interval=%lldms\n", static_cast<long long>(delay));
            lastSendTime = sendTime;

            sendQueue.pop_front();
            nextSendTime += packetDuration;
        }

        lock.lock();
    }

    return true;
}

bool decryptUdpPacket(AudioState &state, const mumble::BufViewConst encryptedPacket, mumble::Buf &outPlaintext) {
    if (!state.cryptReady || encryptedPacket.size() < 4) {
        return false;
    }

    outPlaintext.assign(encryptedPacket.size() - 4, std::byte{0});
    const auto tag = encryptedPacket.subspan(1, 3);
    const auto encryptedData = encryptedPacket.subspan(4);
    const auto written = state.cryptRecv.decrypt(outPlaintext, encryptedData, tag);
    if (!written) {
        return false;
    }

    outPlaintext.resize(written);
    return true;
}

} // namespace example
