#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <ranges>
#include <semaphore>
#include <string>
#include <utility>

#include "src/cli/serve/audio_stream.hpp"
#include "src/cli/serve/audio_tts_api.hpp"
#include "src/cli/serve/tts_service.hpp"
#include "src/models/qwen3_tts/text_stream.hpp"

namespace {

namespace qwen3_tts = gufo::models::qwen3_tts;
using gufo::server::HandleAudioTtsApiRequest;
using gufo::server::HttpRequest;
using gufo::server::HttpResponse;
using gufo::server::TtsService;
using gufo::server::TtsServiceOptions;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "FAIL qwen3_tts_audio_api_test: " << message << '\n';
  std::exit(1);
}

void Check(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

class FakeRunner {
public:
  bool Run(const qwen3_tts::SynthesisRequest& request,
           const qwen3_tts::CancellationCheck&,
           qwen3_tts::SynthesisResult* result, std::string* error) {
    ++calls;
    latest = request;
    latest.on_audio = {};
    result->sample_rate = 24000;
    result->code_groups = 16;
    result->codes.resize(32);
    result->samples = {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    if (request.on_audio) {
      for (const auto sample : result->samples) {
        if (!request.on_audio(std::span<const float>(&sample, 1))) {
          if (error)
            *error = "cancelled";
          return false;
        }
      }
    }
    return true;
  }

  int calls{0};
  qwen3_tts::SynthesisRequest latest;
};

TtsService MakeService(FakeRunner* runner) {
  return TtsService(TtsServiceOptions{
      .model_root = {},
      .validate_model = false,
      .model_id = "qwen3-tts-12hz-1.7b-customvoice",
      .voices = {"vivian", "ryan"},
      .runner =
          [runner](const qwen3_tts::SynthesisRequest& request,
                   const qwen3_tts::CancellationCheck& cancelled,
                   qwen3_tts::SynthesisResult* result, std::string* error) {
            return runner->Run(request, cancelled, result, error);
          },
  });
}

TtsService MakeVoiceDesignService(FakeRunner* runner) {
  return TtsService(TtsServiceOptions{
      .model_root = {},
      .validate_model = false,
      .variant = qwen3_tts::ModelVariant::kVoiceDesign,
      .model_id = {},
      .voices = {},
      .runner =
          [runner](const qwen3_tts::SynthesisRequest& request,
                   const qwen3_tts::CancellationCheck& cancelled,
                   qwen3_tts::SynthesisResult* result, std::string* error) {
            return runner->Run(request, cancelled, result, error);
          },
  });
}

TtsService MakeBaseService(FakeRunner* runner) {
  return TtsService(TtsServiceOptions{
      .model_root = {},
      .validate_model = false,
      .variant = qwen3_tts::ModelVariant::kBase,
      .model_id = {},
      .voices = {},
      .runner =
          [runner](const qwen3_tts::SynthesisRequest& request,
                   const qwen3_tts::CancellationCheck& cancelled,
                   qwen3_tts::SynthesisResult* result, std::string* error) {
            return runner->Run(request, cancelled, result, error);
          },
  });
}

HttpResponse Send(TtsService& service, std::string method, std::string path,
                  std::string body = {}) {
  return HandleAudioTtsApiRequest(
      HttpRequest{
          .method = std::move(method),
          .path = std::move(path),
          .query = {},
          .body = std::move(body),
          .headers = {},
          .is_cancelled = {},
      },
      service);
}

bool HasHeader(const HttpResponse& response, std::string_view name,
               std::string_view value) {
  return std::ranges::any_of(response.headers, [&](const auto& header) {
    return header.first == name && header.second == value;
  });
}
std::string_view BodyOrEmpty(const HttpResponse& response) {
  return response.body;
}

// The served stream has to be readable by any MP3 decoder, so every byte
// group is checked as an MPEG frame instead of trusting the encoder. LAME is
// configured for constant bitrate, mono, 24 kHz and no CRC, which fixes the
// frame header bits and gives 72 * 160000 / 24000 = 480 bytes per frame.
void CheckMp3Stream(const std::string& bytes, const char* what) {
  constexpr int kBitrateKbps = 160;
  constexpr int kSampleRate = 24000;
  std::size_t offset = 0;
  int frames = 0;
  while (offset + 4 <= bytes.size()) {
    const auto at = [&](std::size_t index) {
      return static_cast<unsigned char>(bytes[offset + index]);
    };
    // Eleven sync bits, then version (10 is MPEG-2), layer (01 is Layer III)
    // and the protection bit, which is set when no CRC follows.
    if (at(0) != 0xFF || (at(1) & 0xE0U) != 0xE0U) {
      break;
    }
    Check((at(1) & 0x18U) == 0x10U, "mp3 frames carry the MPEG-2 version");
    Check((at(1) & 0x06U) == 0x02U, "mp3 frames carry Layer III");
    Check((at(1) & 0x01U) == 0x01U, "mp3 frames carry no CRC");
    Check((at(2) >> 4) == 14U, "mp3 frames carry the 160 kbit/s bitrate");
    Check(((at(2) >> 2) & 0x03U) == 1U, "mp3 frames carry 24 kHz");
    Check(((at(3) >> 6) & 0x03U) == 3U, "mp3 frames carry one channel");
    const std::size_t padding = (at(2) >> 1) & 1U;
    offset += 72U * kBitrateKbps * 1000U / kSampleRate + padding;
    ++frames;
  }
  Check(frames > 0, what);
  Check(offset == bytes.size(), "mp3 output ends on a frame boundary");
}

// Concatenates the audio of every SSE delta event, which is how a client has
// to rebuild an encoded stream from base64 events.
std::string SseAudio(const std::string& events) {
  std::string encoded;
  std::size_t position = 0;
  while ((position = events.find("\"audio\":\"", position)) !=
         std::string::npos) {
    position += std::strlen("\"audio\":\"");
    const std::size_t end = events.find('\"', position);
    if (end == std::string::npos) {
      Fail("SSE audio field is not terminated");
    }
    encoded += events.substr(position, end - position);
    position = end;
  }
  std::string decoded;
  if (!gufo::server::DecodeAudioBase64(encoded, 1U << 20U, &decoded)) {
    Fail("SSE audio is not canonical base64");
  }
  return decoded;
}

std::string ConsumeBody(const HttpResponse& response) {
  std::string body;
  if (!response.streaming_body) {
    Fail("response has no streaming body");
  }
  response.streaming_body([&](std::string_view bytes) {
    body.append(bytes);
    return true;
  });
  return body;
}

void TestSpeech() {
  FakeRunner runner;
  TtsService service = MakeService(&runner);
  Check(service.ready(), "injected service is ready");

  const HttpResponse response = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"The boy who lived.","voice":"vivian","language":"english","instructions":"calm","seed":7,"max_new_tokens":12,"greedy":true,"response_format":"wav","speed":1.0})");
  Check(response.status == 200, "valid request succeeds");
  Check(
      response.log_details.find("codec_steps=2") != std::string::npos &&
          response.log_details.find("synthesis_ms=") != std::string::npos &&
          response.log_details.find("The boy who lived.") == std::string::npos,
      "speech diagnostics report work without input text");
  Check(response.body.size() == 54 && response.body.substr(0, 4) == "RIFF" &&
            response.body.substr(8, 8) == "WAVEfmt ",
        "response is a mono PCM16 WAV");
  Check(HasHeader(response, "Content-Type", "audio/wav"),
        "WAV content type is explicit");
  Check(HasHeader(response, "X-Gufo-Codec-Steps", "2"),
        "codec step count is exposed");
  Check(runner.calls == 1 && runner.latest.text == "The boy who lived." &&
            runner.latest.speaker == "vivian" &&
            runner.latest.language == "english" &&
            runner.latest.instruct == "calm" &&
            runner.latest.sampling.seed == 7 &&
            runner.latest.max_new_tokens == 12 &&
            !runner.latest.sampling.sample &&
            !runner.latest.sampling.predictor_sample,
        "API fields reach the model contract without drift");

  const HttpResponse sampled = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"The boy who lived.","voice":"vivian"})");
  Check(sampled.status == 200 && runner.calls == 2 &&
            runner.latest.sampling.sample &&
            runner.latest.sampling.predictor_sample,
        "checkpoint-quality sampling is the API default");
}

void TestSampling() {
  FakeRunner runner;
  TtsService service = MakeService(&runner);
  const auto response = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","temperature":0.5,"top_k":0,"top_p":0.8,"repetition_penalty":1.2,"subtalker_dosample":false,"subtalker_temperature":0.7,"subtalker_top_k":13,"subtalker_top_p":0.9})");
  const auto& sampling = runner.latest.sampling;
  Check(response.status == 200 && sampling.temperature == 0.5F &&
            sampling.top_k == 0 && sampling.top_p == 0.8F &&
            sampling.repetition_penalty == 1.2F && !sampling.predictor_sample &&
            sampling.predictor_temperature == 0.7F &&
            sampling.predictor_top_k == 13 && sampling.predictor_top_p == 0.9F,
        "main and predictor sampling controls reach their own distributions");
  for (const std::string field :
       {R"("top_p":0)", R"("top_p":1.1)", R"("temperature":0)",
        R"("temperature":1e100)", R"("top_k":-1)", R"("top_k":1.5)",
        R"("subtalker_top_p":2)", R"("subtalker_dosample":1)",
        R"("repetition_penalty":0)"}) {
    const auto invalid = Send(
        service, "POST", "/v1/audio/speech",
        R"({"model":"qwen3-tts","input":"x","voice":"vivian",)" + field + "}");
    Check(invalid.status == 400 &&
              invalid.body.find("invalid_sampling") != std::string::npos,
          "invalid sampling rejected: " + field);
  }
  Check(runner.calls == 1, "invalid sampling never invokes inference");

  std::mt19937 random(42);
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::vector<qwen3_tts::TokenScore> logits{{0, 0}, {1, -1}, {2, -2}};
    Check(qwen3_tts::SampleCodec(logits, 0, 0.8F, 0.5F, &random) == 0,
          "temperature is applied before nucleus filtering");
  }
  bool saw_tied = false;
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::vector<qwen3_tts::TokenScore> logits{{0, 0}, {1, 0}, {2, -10}};
    saw_tied |= qwen3_tts::SampleCodec(logits, 1, 1.0F, 1.0F, &random) == 1;
  }
  Check(saw_tied, "top-k preserves all logits tied at the threshold");
  std::vector<qwen3_tts::TokenScore> invalid{
      {0, std::numeric_limits<float>::quiet_NaN()}};
  bool rejected = false;
  try {
    (void)qwen3_tts::SampleCodec(invalid, 1, 1, 1, &random);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  Check(rejected, "nonfinite codec logits fail closed");
}

void TestStreaming() {
  FakeRunner runner;
  TtsService service = MakeService(&runner);
  auto response = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"pcm"})");
  Check(response.status == 200 && response.streaming_body && runner.calls == 0,
        "PCM synthesis starts when the response is consumed");
  std::string pcm;
  response.streaming_body([&](std::string_view bytes) {
    pcm.append(bytes);
    return true;
  });
  Check(
      pcm == gufo::server::EncodePcm16(std::vector<float>{-1, -.5F, 0, .5F, 1}),
      "PCM chunks retain buffered sample encoding");
  auto sse = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","stream_format":"sse"})");
  std::string events;
  sse.streaming_body([&](std::string_view bytes) {
    events.append(bytes);
    return true;
  });
  Check(events.find("speech.audio.delta") != std::string::npos &&
            events.find("\"audio\":") != std::string::npos &&
            events.find("speech.audio.done") != std::string::npos,
        "SSE uses the OpenAI audio field and terminal event");
  std::size_t writes = 0;
  response.streaming_body([&](std::string_view) {
    ++writes;
    return false;
  });
  Check(writes == 1 && response.stream_log->error_code == "cancelled",
        "output disconnect cancels native synthesis immediately");
  for (std::string value : {R"("bogus")", "true"}) {
    Check(
        Send(
            service, "POST", "/v1/audio/speech",
            R"({"model":"qwen3-tts","input":"x","voice":"vivian","stream_format":)" +
                value + "}")
                .status == 400,
        "unsupported stream formats fail closed");
  }
  std::string decoded;
  Check(gufo::server::DecodeAudioBase64(gufo::server::EncodeAudioBase64(pcm),
                                        pcm.size(), &decoded) &&
            decoded == pcm,
        "audio base64 round trip");
  Check(!gufo::server::DecodeAudioBase64("AB==", 16, &decoded) &&
            !gufo::server::DecodeAudioBase64("YQ==AAAA", 16, &decoded) &&
            !gufo::server::DecodeAudioBase64("AAAA", 2, &decoded),
        "base64 rejects noncanonical padding and oversized audio");
  const std::string text = "Value 3.14 is fine. Next phrase! Then 中文。";
  std::string pending = text;
  const auto expected =
      qwen3_tts::TakeSpeechSegments(&pending, "sentence", true);
  for (std::size_t split = 0; split <= text.size(); ++split) {
    pending = text.substr(0, split);
    auto actual = qwen3_tts::TakeSpeechSegments(&pending, "sentence", false);
    pending += text.substr(split);
    auto tail = qwen3_tts::TakeSpeechSegments(&pending, "sentence", true);
    actual.insert(actual.end(), tail.begin(), tail.end());
    Check(actual == expected,
          "text splitting is independent of packet boundaries");
  }
}

void TestQueuedCancellation() {
  std::binary_semaphore entered{0}, release{0};
  std::atomic<int> calls{0};
  TtsService service(
      TtsServiceOptions{.validate_model = false,
                        .runner = [&](const auto&, const auto&, auto*, auto*) {
                          ++calls;
                          entered.release();
                          release.acquire();
                          return true;
                        }});
  auto active = std::async(std::launch::async, [&] {
    qwen3_tts::SynthesisResult output;
    std::string error;
    return service.Synthesize({}, {}, &output, &error);
  });
  entered.acquire();
  std::atomic<bool> cancel{false};
  auto queued = std::async(std::launch::async, [&] {
    qwen3_tts::SynthesisResult output;
    output.samples = {1};
    std::string error;
    Check(!service.Synthesize(
              {}, [&] { return cancel.load(); }, &output, &error) &&
              output.samples.empty(),
          "queued cancellation clears the result");
  });
  cancel = true;
  Check(queued.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
        "queued cancellation does not wait for active inference");
  queued.get();
  Check(calls == 1, "cancelled waiter never enters the mutable runtime");
  release.release();
  Check(active.get(),
        "queued cancellation does not invalidate the active request");
}

void TestValidationAndVoices() {
  FakeRunner runner;
  TtsService service = MakeService(&runner);

  const HttpResponse voices = Send(service, "GET", "/v1/audio/voices");
  Check(voices.status == 200 &&
            voices.body.find("\"id\":\"vivian\"") != std::string::npos &&
            voices.body.find("\"id\":\"ryan\"") != std::string::npos,
        "voice discovery lists configured CustomVoice speakers");

  Check(Send(service, "POST", "/v1/audio/speech",
             R"({"model":"qwen3-tts","input":"x","voice":"unknown"})")
                .status == 400,
        "unknown voice fails closed");
  const auto rejected = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"flac"})");
  Check(rejected.status == 400 &&
            BodyOrEmpty(rejected).find("'wav', 'pcm' or 'mp3'") !=
                std::string::npos,
        "unsupported format fails closed and names the supported ones");
  Check(Send(service, "POST", "/v1/audio/speech",
             R"({"model":"qwen3-tts","input":"x","voice":"vivian","seed":1.5})")
                .status == 400,
        "fractional seed fails closed");
  Check(
      Send(service, "POST", "/v1/audio/speech",
           R"({"model":"qwen3-tts","input":"x","voice":"vivian","typo":true})")
              .status == 400,
      "unknown fields fail closed");
  Check(runner.calls == 0, "invalid requests never invoke inference");
}

void TestVoiceDesignContract() {
  FakeRunner runner;
  TtsService service = MakeVoiceDesignService(&runner);
  Check(service.ready() &&
            service.model_id() == "qwen3-tts-12hz-1.7b-voice-design",
        "VoiceDesign service derives its model identity");

  const HttpResponse response = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"A lantern in the rain.","language":"english","instructions":"A calm, warm adult voice."})");
  Check(response.status == 200 && runner.calls == 1 &&
            runner.latest.speaker == "voice-design" &&
            runner.latest.instruct == "A calm, warm adult voice.",
        "VoiceDesign uses instructions without CustomVoice speaker semantics");

  Check(Send(service, "POST", "/v1/audio/speech",
             R"({"model":"qwen3-tts","input":"x"})")
                .status == 400,
        "VoiceDesign rejects a missing instruction");
  Check(
      Send(
          service, "POST", "/v1/audio/speech",
          R"({"model":"qwen3-tts","input":"x","voice":"vivian","instructions":"calm"})")
              .status == 400,
      "VoiceDesign rejects CustomVoice speaker names");
}

void TestBaseContract() {
  constexpr std::string_view kReferenceWav =
      "UklGRiwAAABXQVZFZm10IBAAAAABAAEAwF0AAIC7AAACABAAZGF0YQgAAAAAAOgD"
      "GPwAAA==";
  FakeRunner runner;
  TtsService service = MakeBaseService(&runner);
  Check(service.ready() && service.model_id() == "qwen3-tts-12hz-1.7b-base",
        "Base service derives its model identity");

  const HttpResponse response = Send(
      service, "POST", "/v1/audio/speech",
      std::string(
          R"({"model":"qwen3-tts","input":"Clone me.","reference_audio":")") +
          std::string(kReferenceWav) +
          R"(","reference_text":"Reference phrase.","language":"auto"})");
  Check(response.status == 200 && runner.calls == 1 &&
            runner.latest.speaker == "voice-clone" &&
            runner.latest.reference_audio.sample_rate == 24000 &&
            runner.latest.reference_audio.channels == 1 &&
            runner.latest.reference_audio.samples.size() == 4 &&
            runner.latest.reference_text == "Reference phrase." &&
            !runner.latest.speaker_embedding_only,
        "Base ICL reference audio reaches the synthesis contract");

  Check(Send(service, "POST", "/v1/audio/speech",
             R"({"model":"qwen3-tts","input":"x"})")
                .status == 400,
        "Base rejects a missing reference WAV");
  Check(Send(service, "POST", "/v1/audio/speech",
             std::string(
                 R"({"model":"qwen3-tts","input":"x","reference_audio":")") +
                 std::string(kReferenceWav) + R"("})")
                .status == 400,
        "Base ICL rejects missing reference text");
  Check(Send(service, "POST", "/v1/audio/speech",
             std::string(
                 R"({"model":"qwen3-tts","input":"x","reference_audio":")") +
                 std::string(kReferenceWav) +
                 R"(","voice_clone_mode":"speaker_embedding_only"})")
                    .status == 200 &&
            runner.latest.speaker_embedding_only,
        "Base speaker-embedding-only mode does not require reference text");
}

// MP3 is a streaming format: the frames concatenate, so the reply never needs
// the total length, and the encoder runs inside the response body.
void TestMp3() {
  FakeRunner runner;
  TtsService service = MakeService(&runner);
  const HttpResponse response = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"mp3"})");
  Check(response.status == 200 && response.streaming_body && runner.calls == 0,
        "MP3 synthesis starts when the response is consumed");
  Check(HasHeader(response, "Content-Type", "audio/mpeg"),
        "MP3 answers with the MPEG media type");
  const std::string mp3 = ConsumeBody(response);
  CheckMp3Stream(mp3, "an MP3 reply is a stream of MPEG frames");
  Check(response.stream_log->error_code.empty(),
        "a served MP3 stream reports no error");

  const auto sse = Send(
      service, "POST", "/v1/audio/speech",
      R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"mp3","stream_format":"sse"})");
  Check(sse.status == 200 && sse.streaming_body,
        "SSE accepts an MP3 response format");
  const std::string events = ConsumeBody(sse);
  Check(events.find("speech.audio.done") != std::string::npos,
        "an SSE MP3 stream ends with the terminal event");
  CheckMp3Stream(SseAudio(events),
                 "SSE base64 MP3 deltas rebuild the same MPEG stream");

  Check(
      Send(
          service, "POST", "/v1/audio/speech",
          R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"wav","stream_format":"sse"})")
              .status == 400,
      "SSE still refuses WAV, whose header needs the total length");
}

// The encoder is opened for the rate this service synthesizes at, so a model
// that answers with another rate has to fail instead of labelling the already
// written frames wrongly.
void TestMp3SampleRateMismatch() {
  TtsService service(TtsServiceOptions{
      .validate_model = false,
      .voices = {"vivian"},
      .runner =
          [](const auto& request, const auto&, auto* result, auto*) {
            result->sample_rate = 48000;
            result->samples = {0.5F, 0.25F};
            return !request.on_audio || request.on_audio(result->samples);
          },
  });
  for (const auto* format : {"audio", "sse"}) {
    auto response = Send(
        service, "POST", "/v1/audio/speech",
        std::string(
            R"({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":"mp3","stream_format":")") +
            format + "\"}");
    const std::string body = ConsumeBody(response);
    Check(response.stream_log->error_code == "invalid_audio",
          "a sample rate the MP3 stream cannot carry reports invalid audio");
    Check(body.find("speech.audio.done") == std::string::npos,
          "a rate mismatch never becomes a successful terminal event");
    Check(format == std::string_view("sse")
              ? body.find("event: error") != std::string::npos
              : body.empty(),
          "a rate mismatch is only reported through the stream in use");
  }
}

void TestInvalidWaveform() {
  for (const float sample : {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()}) {
    TtsService service(TtsServiceOptions{
        .validate_model = false,
        .voices = {"vivian"},
        .runner =
            [sample](const auto& request, const auto&, auto* result, auto*) {
              result->sample_rate = 24000;
              result->samples = {0, sample};
              return !request.on_audio || request.on_audio(result->samples);
            },
    });
    const auto wav =
        Send(service, "POST", "/v1/audio/speech",
             R"({"model":"qwen3-tts","input":"x","voice":"vivian"})");
    Check(wav.status == 500 &&
              wav.body.find("invalid_audio") != std::string::npos,
          "nonfinite WAV samples fail before conversion");
    for (const auto response_format : {"pcm", "mp3"}) {
      for (const auto format : {"audio", "sse"}) {
        const std::string request =
            std::string(
                R"json({"model":"qwen3-tts","input":"x","voice":"vivian","response_format":")json") +
            response_format + R"json(","stream_format":")json" + format + "\"}";
        auto response = Send(service, "POST", "/v1/audio/speech", request);
        std::string body;
        if (response.streaming_body) {
          response.streaming_body([&](std::string_view bytes) {
            body.append(bytes);
            return true;
          });
        }
        Check(
            response.stream_log != nullptr &&
                response.stream_log->error_code == "invalid_audio",
            std::string("nonfinite streaming samples report invalid audio (") +
                response_format + "/" + format + ")");
        Check(body.find("speech.audio.delta") == std::string::npos &&
                  body.find("speech.audio.done") == std::string::npos &&
                  (format == std::string_view("sse")
                       ? body.find("event: error") != std::string::npos
                       : body.empty()),
              "invalid audio never becomes a stream frame or a terminal event");
      }
    }
  }
}

}  // namespace

int main() {
  TestSpeech();
  TestSampling();
  TestStreaming();
  TestQueuedCancellation();
  TestValidationAndVoices();
  TestVoiceDesignContract();
  TestBaseContract();
  TestMp3();
  TestMp3SampleRateMismatch();
  TestInvalidWaveform();
  std::cout << "PASS qwen3_tts_audio_api_test\n";
  return 0;
}
