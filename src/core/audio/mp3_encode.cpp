#include "src/core/audio/mp3_encode.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

extern "C" {
#include <lame/lame.h>
}

namespace gufo::audio {
namespace {

// LAME quality 0 is the slowest and best, 9 the fastest. Mono speech at the
// top of the bitrate range costs no measurable time on the host CPU, so the
// production path takes the better psychoacoustic model.
constexpr int kEncoderQuality = 2;

// Samples handed to LAME at a time. LAME keeps its own residue, so the block
// only bounds the temporary output buffer.
constexpr std::size_t kInputBlockSamples = 65536;

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

std::uint32_t Mp3BitrateKbpsForSampleRate(std::uint32_t sample_rate) noexcept {
  // Each entry is the highest bitrate the MPEG version that carries the rate
  // allows: 320 kbit/s for MPEG-1, 160 kbit/s for MPEG-2 LSF. Measured with
  // libmp3lame 3.100, so the bitrate an encoder actually writes is known.
  // MPEG-2.5 rates stay unsupported because LAME writes a lower bitrate than
  // the table there, which would make this function report a bitrate the
  // output does not carry.
  switch (sample_rate) {
    case 32000:
    case 44100:
    case 48000:
      return 320;  // MPEG-1 Layer III
    case 16000:
    case 22050:
    case 24000:
      return 160;  // MPEG-2 Layer III, low sampling frequency
    default:
      return 0;
  }
}

Mp3StreamEncoder::~Mp3StreamEncoder() {
  if (encoder_ != nullptr) {
    lame_close(static_cast<lame_global_flags*>(encoder_));
  }
}

Mp3StreamEncoder::Mp3StreamEncoder(Mp3StreamEncoder&& other) noexcept
    : encoder_(other.encoder_),
      sample_rate_(other.sample_rate_),
      bitrate_kbps_(other.bitrate_kbps_) {
  other.encoder_ = nullptr;
  other.sample_rate_ = 0;
  other.bitrate_kbps_ = 0;
}

Mp3StreamEncoder& Mp3StreamEncoder::operator=(
    Mp3StreamEncoder&& other) noexcept {
  if (this != &other) {
    if (encoder_ != nullptr) {
      lame_close(static_cast<lame_global_flags*>(encoder_));
    }
    encoder_ = other.encoder_;
    sample_rate_ = other.sample_rate_;
    bitrate_kbps_ = other.bitrate_kbps_;
    other.encoder_ = nullptr;
    other.sample_rate_ = 0;
    other.bitrate_kbps_ = 0;
  }
  return *this;
}

bool Mp3StreamEncoder::Open(std::uint32_t sample_rate, std::string* error) {
  if (encoder_ != nullptr) {
    SetError(error, "MP3 encoder is already open");
    return false;
  }
  const std::uint32_t bitrate = Mp3BitrateKbpsForSampleRate(sample_rate);
  if (bitrate == 0) {
    SetError(error, "MP3 does not support a " + std::to_string(sample_rate) +
                        " Hz sample rate");
    return false;
  }
  lame_global_flags* handle = lame_init();
  if (handle == nullptr) {
    SetError(error, "cannot allocate the MP3 encoder");
    return false;
  }
  const auto release = [&](std::string message) {
    lame_close(handle);
    SetError(error, std::move(message));
    return false;
  };
  // Mono, no error-protection CRC, no LAME tag: every frame is an ordinary
  // constant-bitrate frame, so the stream length and duration follow directly
  // from the bitrate, and a chunked response needs no metadata frame.
  //
  // Measured level, kept here so nobody "fixes" it twice: libmp3lame 3.100
  // writes MPEG-2 LSF frames (the 24 kHz this service synthesizes at) that
  // decode to 0.95 of the input level, a flat -0.44 dB at any bitrate, while
  // MPEG-1 rates decode at 1.00. Feeding the same samples to libmp3lame
  // through FFmpeg gives the same 0.95, so the offset comes from the encoder
  // and not from the settings below. Compensating here would put gufo out of
  // step with every other user of the library.
  //
  // Measured against FFmpeg's own libmp3lame encode and read back with both
  // FFmpeg and minimp3. tests/functional/tts_service.py checks the frame
  // structure of the served stream.
  if (lame_set_num_channels(handle, 1) != 0 ||
      lame_set_mode(handle, MONO) != 0 ||
      lame_set_in_samplerate(handle, static_cast<int>(sample_rate)) != 0 ||
      lame_set_brate(handle, static_cast<int>(bitrate)) != 0 ||
      lame_set_VBR(handle, vbr_off) != 0 ||
      lame_set_quality(handle, kEncoderQuality) != 0 ||
      lame_set_error_protection(handle, 0) != 0 ||
      lame_set_emphasis(handle, 0) != 0 ||
      lame_set_bWriteVbrTag(handle, 0) != 0) {
    return release("cannot configure the MP3 encoder");
  }
  if (lame_init_params(handle) != 0) {
    return release("cannot initialize the MP3 encoder");
  }
  encoder_ = handle;
  sample_rate_ = sample_rate;
  bitrate_kbps_ = bitrate;
  return true;
}

bool Mp3StreamEncoder::Append(std::span<const float> samples,
                              std::string* output) {
  if (output == nullptr || encoder_ == nullptr) {
    return false;
  }
  for (const float sample : samples) {
    if (!std::isfinite(sample)) {
      return false;
    }
  }
  std::vector<unsigned char> buffer;
  for (std::size_t offset = 0; offset < samples.size();) {
    const std::size_t count =
        std::min(kInputBlockSamples, samples.size() - offset);
    const auto block = samples.subspan(offset, count);
    offset += count;
    // lame/lame.h: "mp3buf_size in bytes = 1.25*num_samples + 7200".
    buffer.resize(count + count / 4U + 7200U);
    // num_channels is 1, so LAME reads the left pointer only and never
    // averages. With 2 channels and mono mode it would average L and R into
    // the left buffer and overwrite the input, which would corrupt the
    // caller's span, so that combination stays out.
    const int written = lame_encode_buffer_ieee_float(
        static_cast<lame_global_flags*>(encoder_), block.data(), block.data(),
        static_cast<int>(count), buffer.data(),
        static_cast<int>(buffer.size()));
    if (written < 0) {
      return false;
    }
    output->append(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::size_t>(written));
  }
  return true;
}

bool Mp3StreamEncoder::Flush(std::string* output) {
  if (output == nullptr || encoder_ == nullptr) {
    return false;
  }
  // lame/lame.h: for lame_encode_flush "'mp3buf' should be at least 7200 bytes
  // long to hold all possible emitted data". LAME_MAXMP3BUFFER is marked
  // obsolete there and reserves 128 KB of album-art space this encoder never
  // writes.
  std::vector<unsigned char> buffer(7200);
  const int written =
      lame_encode_flush(static_cast<lame_global_flags*>(encoder_),
                        buffer.data(), static_cast<int>(buffer.size()));
  if (written < 0) {
    return false;
  }
  output->append(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::size_t>(written));
  return true;
}

}  // namespace gufo::audio
