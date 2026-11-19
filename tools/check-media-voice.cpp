// SPDX-License-Identifier: MIT
// Fix 32 (tools/source-1.0.2/fixes/32-media-voice-rates.patch): XMP media voices (playlist music)
// through PacedDriver (platform/ps5/canary_audio.cpp). Drives xbox360ps5::media_voice
// (port/media_voice.hpp) with frames shaped as apu/audio_media_player.cc submits them (1536
// interleaved native floats, any channel count, any rate) and checks:
//  * samples per channel for mono, stereo and 5.1 (1.0.2: 768 for every voice that was not 5.1);
//  * the stereo mix reads nothing past the frame (1.0.2's mono mix read in[1] and in[2] past it);
//  * 44.1, 22.05, 32 and 96 kHz come out at 48 kHz with the right length and pitch, and with no
//    step at frame boundaries; 48 kHz passes unchanged;
//  * the grains the port is given over a long run match the input's duration;
//  * the assembled canary_audio.cpp uses it and opens the port for every rate.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/media_voice.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace mv = xbox360ps5::media_voice;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kGrain = 256;  // PacedDriver's samples per port output

// A frame of `channels` interleaved channels inside a larger buffer whose tail is NaN, so that
// a read past the frame shows up in the output.
struct GuardedFrame {
  std::vector<float> storage;
  const float* data() const { return storage.data(); }
  GuardedFrame(const std::vector<float>& frame)
      : storage(frame.size() + 64, std::numeric_limits<float>::quiet_NaN()) {
    std::copy(frame.begin(), frame.end(), storage.begin());
  }
};

// 1.0.2's ToStereo for a voice without conversion, with its channel_samples_.
std::vector<float> OldToStereo(const float* frame, uint32_t channels) {
  const uint32_t samples = channels == 6 ? 256 : 768;
  std::vector<float> stereo(size_t(samples) * 2);
  if (channels == 2) {
    std::copy(frame, frame + 1536, stereo.begin());
  } else {
    for (uint32_t n = 0; n < samples; ++n) {
      const float* in = &frame[size_t(n) * channels];
      stereo[n * 2] = in[0] + in[2] * 0.7071f;
      stereo[n * 2 + 1] = in[1] + in[2] * 0.7071f;
    }
  }
  return stereo;
}

bool AllFinite(const std::vector<float>& v) {
  for (float f : v)
    if (!std::isfinite(f)) return false;
  return true;
}

void CheckShape() {
  assert(mv::ChannelSamples(1) == 1536);
  assert(mv::ChannelSamples(2) == 768);
  assert(mv::ChannelSamples(6) == 256);
  assert(mv::ChannelSamples(0) == 0);

  // Mono: 1536 samples, each to both sides.
  std::vector<float> mono(1536);
  for (size_t n = 0; n < mono.size(); ++n) mono[n] = float(n) / 1536.0f;
  GuardedFrame guarded(mono);
  std::vector<float> stereo;
  mv::InterleavedToStereo(guarded.data(), 1, mv::ChannelSamples(1), stereo);
  assert(stereo.size() == 1536 * 2);
  assert(AllFinite(stereo));
  for (size_t n = 0; n < 1536; ++n) assert(stereo[n * 2] == mono[n] && stereo[n * 2 + 1] == mono[n]);
  // 1.0.2: SubmitFrame kept channels x 768 = 768 floats of the 1536, ToStereo mixed them as 5.1
  // (left = in[0] + 0.7071 in[2]) and its last sample read in[768] and in[769], past the copy.
  GuardedFrame copied(std::vector<float>(mono.begin(), mono.begin() + 768));
  const auto old = OldToStereo(copied.data(), 1);
  assert(old.size() == 768 * 2);  // half of the frame: the music at twice the speed
  assert(old[0] != mono[0]);      // mixed as 5.1
  assert(!AllFinite(old));        // the last sample read the NaN past the frame

  // Stereo: unchanged, as before.
  std::vector<float> two(1536);
  for (size_t n = 0; n < two.size(); ++n) two[n] = (n & 1) ? -float(n) : float(n);
  mv::InterleavedToStereo(two.data(), 2, mv::ChannelSamples(2), stereo);
  assert(stereo == two);
  assert(OldToStereo(two.data(), 2) == two);

  // Interleaved 5.1: front left/right with the centre at -3 dB in both, as before.
  std::vector<float> six(1536);
  for (size_t n = 0; n < six.size(); ++n) six[n] = float(n % 6) + 1.0f;
  GuardedFrame guarded_six(six);
  mv::InterleavedToStereo(guarded_six.data(), 6, mv::ChannelSamples(6), stereo);
  assert(stereo.size() == 512 && AllFinite(stereo));
  assert(std::fabs(stereo[0] - (1.0f + 3.0f * 0.7071f)) < 1e-6f);
  assert(std::fabs(stereo[1] - (2.0f + 3.0f * 0.7071f)) < 1e-6f);
  assert(stereo == OldToStereo(six.data(), 6));
  std::puts("frame shape and stereo mix: ok");
}

// A sine of `tone` Hz at `rate`, `seconds` long, in mono frames of 1536, through the resampler.
std::vector<float> ResampleSine(uint32_t rate, double tone, double seconds, uint32_t channels) {
  mv::Resampler resampler(rate);
  assert(resampler.rate() == rate);
  const uint32_t samples = mv::ChannelSamples(channels);
  const size_t total = size_t(rate * seconds);
  std::vector<float> out, stereo, frame(size_t(samples) * channels);
  for (size_t at = 0; at + samples <= total; at += samples) {
    for (uint32_t n = 0; n < samples; ++n) {
      const float v = float(std::sin(2 * kPi * tone * double(at + n) / rate));
      for (uint32_t c = 0; c < channels; ++c) frame[size_t(n) * channels + c] = c == 2 ? 0.0f : v;
    }
    mv::InterleavedToStereo(frame.data(), channels, samples, stereo);
    resampler.Process(stereo.data(), samples, out);
  }
  return out;
}

void CheckRate(uint32_t rate, uint32_t channels) {
  const double tone = 1000.0, seconds = 4.0;
  const uint32_t samples = mv::ChannelSamples(channels);
  const size_t frames_in = size_t(size_t(rate * seconds) / samples) * samples;
  const auto out = ResampleSine(rate, tone, seconds, channels);
  assert(out.size() % 2 == 0 && AllFinite(out));
  const double pairs = double(out.size() / 2);
  const double expected = double(frames_in) * mv::kPortRate / rate;
  // Length: the input's duration at 48 kHz, within one input sample.
  const double tolerance = double(mv::kPortRate) / rate + 1.0;
  if (std::fabs(pairs - expected) > tolerance) {
    std::fprintf(stderr, "rate %u: %.0f pairs, expected %.1f\n", rate, pairs, expected);
    assert(false);
  }
  // Pitch: 1 kHz stays 1 kHz at 48 kHz (rising zero crossings of the left side).
  size_t crossings = 0;
  for (size_t n = 1; n < out.size() / 2; ++n)
    if (out[(n - 1) * 2] < 0 && out[n * 2] >= 0) ++crossings;
  const double measured = crossings / (pairs / mv::kPortRate);
  if (std::fabs(measured - tone) > 2.0) {
    std::fprintf(stderr, "rate %u: %.1f Hz measured\n", rate, measured);
    assert(false);
  }
  // No step anywhere (frame boundaries included): a 1 kHz sine moves at most 2*pi*1000/48000
  // per output sample, plus the interpolation error.
  const double most = 2 * kPi * tone / mv::kPortRate * 1.05;
  for (size_t n = 1; n < out.size() / 2; ++n) {
    assert(std::fabs(out[n * 2] - out[(n - 1) * 2]) <= most);
    assert(out[n * 2] == out[n * 2 + 1]);  // mono and the 5.1 fronts reach both sides
  }
  // The port's grains over the run: as PacedDriver keeps the rest for the next frame.
  size_t grains = 0, kept = 0;
  {
    mv::Resampler resampler(rate);
    std::vector<float> at48, frame(size_t(samples) * channels, 0.25f), stereo;
    for (size_t at = 0; at < frames_in; at += samples) {
      mv::InterleavedToStereo(frame.data(), channels, samples, stereo);
      resampler.Process(stereo.data(), samples, at48);
      size_t used = 0;
      for (; used + kGrain * 2 <= at48.size(); used += kGrain * 2) ++grains;
      at48.erase(at48.begin(), at48.begin() + ptrdiff_t(used));
    }
    kept = at48.size() / 2;
    for (float f : at48) assert(f == 0.25f || f == 0.0f || (f > 0.0f && f < 0.25f));
  }
  assert(kept < kGrain);
  assert(std::fabs(double(grains * kGrain + kept) - expected) <= tolerance);
  std::printf("%u Hz, %u channel(s): %.0f pairs at 48 kHz (%.1f expected), %.1f Hz, %zu grains\n",
              rate, channels, pairs, expected, measured, grains);
}

void CheckPassThrough() {
  mv::Resampler resampler(48000);
  std::vector<float> in(1536), out;
  for (size_t n = 0; n < in.size(); ++n) in[n] = float(n) * 0.001f;
  resampler.Process(in.data(), 768, out);
  resampler.Process(in.data(), 768, out);
  assert(out.size() == 3072);
  assert(std::equal(in.begin(), in.end(), out.begin()) &&
         std::equal(in.begin(), in.end(), out.begin() + 1536));
  // A rate of 0 (never given by the player, which refuses it) is taken as 48 kHz.
  assert(mv::Resampler(0).rate() == 48000);
  // A frame boundary continues the line: two frames of a ramp at 24 kHz give a ramp.
  mv::Resampler half(24000);
  std::vector<float> ramp_out;
  std::vector<float> a = {0, 0, 1, 1, 2, 2, 3, 3}, b = {4, 4, 5, 5, 6, 6, 7, 7};
  half.Process(a.data(), 4, ramp_out);
  half.Process(b.data(), 4, ramp_out);
  for (size_t n = 0; n < ramp_out.size() / 2; ++n) assert(std::fabs(ramp_out[n * 2] - 0.5f * n) < 1e-6f);
  assert(ramp_out.size() / 2 == 14);  // 0, 0.5, ... 6.5; 7 needs the next frame
  std::puts("48 kHz pass-through and frame boundaries: ok");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTree() {
  const std::string audio = Read("platform/ps5/canary_audio.cpp");
  assert(!audio.empty());
  assert(audio.find("#include \"media_voice.hpp\"") != std::string::npos);
  assert(audio.find("media_voice::ChannelSamples(channels)") != std::string::npos);
  assert(audio.find("media_voice::InterleavedToStereo(") != std::string::npos);
  assert(audio.find("resampler_.Process(") != std::string::npos);
  assert(audio.find("if (frequency_ == 48000)") == std::string::npos);
  assert(audio.find("channels == 6 ? 256 : 768") == std::string::npos);
  std::puts("assembled canary_audio.cpp: ok");
}
}  // namespace

int main() {
  CheckShape();
  CheckPassThrough();
  for (uint32_t rate : {44100u, 22050u, 32000u, 48000u, 96000u}) CheckRate(rate, 1);
  CheckRate(44100, 2);
  CheckRate(44100, 6);
  CheckTree();
  std::puts("check-media-voice: all passed");
  return 0;
}
