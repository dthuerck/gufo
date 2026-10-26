#ifndef GUFO_CORE_AUDIO_MP3_ENCODE_HPP_
#define GUFO_CORE_AUDIO_MP3_ENCODE_HPP_

#include <cstdint>
#include <span>
#include <string>

namespace gufo::audio {

/// Bitrate for a sample rate: the highest value the MPEG version that carries
/// that rate allows, so the encoder gives up nothing beyond its own loss.
/// Returns 0 for a rate MP3 cannot carry.
[[nodiscard]] std::uint32_t Mp3BitrateKbpsForSampleRate(
    std::uint32_t sample_rate) noexcept;

/// Incremental MPEG Layer III encoder for the mono float waveforms the audio
/// APIs already hold, backed by libmp3lame. MP3 frames concatenate without a
/// container, so every Append and Flush result simply follows the previous
/// ones and the whole sequence plays. Out-of-range finite samples saturate,
/// as the WAV and PCM paths do; a non-finite sample is refused.
class Mp3StreamEncoder {
public:
  Mp3StreamEncoder() = default;
  ~Mp3StreamEncoder();

  Mp3StreamEncoder(Mp3StreamEncoder&& other) noexcept;
  Mp3StreamEncoder& operator=(Mp3StreamEncoder&& other) noexcept;
  Mp3StreamEncoder(const Mp3StreamEncoder&) = delete;
  Mp3StreamEncoder& operator=(const Mp3StreamEncoder&) = delete;

  /// Prepares one constant-bitrate mono stream at `sample_rate`.
  [[nodiscard]] bool Open(std::uint32_t sample_rate,
                          std::string* error = nullptr);
  /// Appends the frames that `samples` completed; LAME keeps the remainder.
  [[nodiscard]] bool Append(std::span<const float> samples,
                            std::string* output);
  /// Emits the frames holding the tail. The encoder is finished afterwards.
  [[nodiscard]] bool Flush(std::string* output);

  [[nodiscard]] std::uint32_t sample_rate() const noexcept {
    return sample_rate_;
  }
  [[nodiscard]] std::uint32_t bitrate_kbps() const noexcept {
    return bitrate_kbps_;
  }

private:
  /// lame_global_flags*, kept opaque so lame.h stays out of this header.
  void* encoder_{nullptr};
  std::uint32_t sample_rate_{0};
  std::uint32_t bitrate_kbps_{0};
};

}  // namespace gufo::audio

#endif  // GUFO_CORE_AUDIO_MP3_ENCODE_HPP_
