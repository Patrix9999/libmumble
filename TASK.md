# ExampleClient audio send tasks

This task list describes the remaining work to make `examples/ExampleClient` capture microphone audio with PortAudio, encode it with Opus, and send it to a connected Mumble server.

## 1. Add PortAudio dependency
- Add PortAudio to `vcpkg.json` and the root `CMakeLists.txt` / `examples/ExampleClient/CMakeLists.txt`.
- Find and link PortAudio in `examples/ExampleClient/CMakeLists.txt`.
- Ensure the example client can include PortAudio headers and link against the PortAudio library.

## 2. Configure audio capture in ExampleClient
- Add configuration options to `examples/ExampleClient/config.toml` for microphone device, sample rate, channel count, frame size, and optionally input latency.
- Initialize PortAudio in `examples/ExampleClient/main.cpp` before connecting (or immediately after initialization).
- Open an input stream for the microphone and choose a callback or blocking read mode.

## 3. Initialize Opus encoder
- Create a `mumble::Opus::Encoder` instance in the client.
- Initialize it for the desired sample rate (typically 48000 Hz) and use `Opus::Encoder::Preset::VoIP`.
- Allocate input and output buffers sized appropriately for capture frames and encoded packets.

## 4. Set up UDP voice transport
- Bind a local UDP endpoint for voice transport using `Peer::bindUDP(...)`.
- Start UDP processing with `Peer::startUDP(...)`, providing feedback callbacks for encrypted UDP packets, pings, and errors.
- Ensure the voice transport is active before sending audio packets.

## 5. Build and send voice packets
- For each captured microphone frame:
  - Convert PCM samples to the format expected by the Opus encoder.
  - Encode the frame with `mumble::Opus::Encoder`.
  - Construct a `mumble::udp::Message::Audio` packet with:
    - `direction = ClientToServer`
    - `target` set for the correct voice target or channel routing
    - `frameNumber` incremented for each packet
    - `opusData` filled with encoded bytes
    - `volumeAdjustment` and `isTerminator` as needed
- Serialize the UDP audio packet into a `udp::Pack` / buffer and send it with `Peer::sendUDP(serverEndpoint, data)`.

## 6. Handle server audio handshake and voice routing
- Verify the TCP authentication message enables Opus support (`auth.opus = true` already done).
- Confirm the server accepts the client's UDP packets and the server sends voice back if needed.
- If required, send a `Message::VoiceTarget` or correct audio target configuration during session setup.

## 7. Add cleanup and error handling
- Shut down PortAudio cleanly when the example exits.
- Stop UDP and TCP peers on shutdown.
- Handle Opus encode failures, PortAudio stream errors, and UDP send errors gracefully.

## 8. Optional debugging and verification
- Log encoded frame sizes and send success/failure.
- Optionally receive and decode incoming UDP audio frames with `mumble::Opus::Decoder` to verify the server path.
- Test against the stock Mumble server and ensure the client appears as a speaking user.
