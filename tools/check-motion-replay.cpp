// 1.0.5: phone motion recordings (motion_record.hpp) replayed through what the console does with
// them: body scaling (body_scale.hpp) and the estimated hand reach (virtual_hands.hpp), next to
// what the title made of the hands when it was recorded.
//
//   check-motion-replay                    run from an assembled tree's root: a synthetic
//                                          recording (file format, scaling, reach, bent arm)
//                                          and the tree's recorder
//   check-motion-replay <file.xmr> ...     a real recording (saved by opening the guide, in
//                                          the logs folder): a report for tuning; fails only
//                                          on a file that does not read
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/body_scale.hpp"
#include "xbox360ps5/kinect_hand_view.hpp"
#include "xbox360ps5/motion_record.hpp"
#include "xbox360ps5/virtual_hands.hpp"

namespace m = xbox360ps5::motion;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// The elbow's angle in degrees (180: a straight arm); 0 when a joint was not seen.
float ElbowAngle(const m::Frame& pose, bool right) {
  const auto& s = pose.joints[right ? 8 : 4];
  const auto& e = pose.joints[right ? 9 : 5];
  const auto& w = pose.joints[right ? 10 : 6];
  if (s.confidence < .5f || e.confidence < .5f || w.confidence < .5f) return 0;
  const float ax = s.x - e.x, ay = s.y - e.y, az = s.z - e.z, bx = w.x - e.x, by = w.y - e.y, bz = w.z - e.z;
  const float la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
  if (la < 1e-4f || lb < 1e-4f) return 0;
  return std::acos(std::clamp((ax * bx + ay * by + az * bz) / (la * lb), -1.0f, 1.0f)) * 57.29578f;
}

struct Series {
  std::vector<float> v;
  void Add(float x) { if (std::isfinite(x)) v.push_back(x); }
  float At(float q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * float(v.size() - 1) + 0.5f))];
  }
  std::string Text() {
    char t[96];
    if (v.empty()) return "none";
    std::snprintf(t, sizeof(t), "median %.2f (10%% %.2f, 90%% %.2f, n %zu)", At(.5f), At(.1f), At(.9f), v.size());
    return t;
  }
};

// Replays a recording as the console takes it, one normalisation state for the whole of it.
struct Report {
  int frames = 0, tracked = 0;
  Series raw_arm, arm, factor, our_reach, game_reach, ratio, hand_shoulder, hand_shoulder_dz;
  int straight_arms = 0;
};
Report Replay(const std::vector<m::MotionRecord>& records) {
  Report r;
  m::BodyScaleState state;
  for (const auto& record : records) {
    ++r.frames;
    if (!record.pose.tracked) continue;
    ++r.tracked;
    m::ObserveBody(state, record.pose);
    float factor = 1;
    const m::Frame body = m::NormalizeBody(state, record.pose, &factor);
    r.factor.Add(factor);
    const auto hands = m::EstimateHands(body);
    for (int side = 0; side < 2; ++side) {
      const bool right = side == 1;
      // Arm lengths only for straight arms (shoulder-to-hand depends on the bend otherwise).
      if (ElbowAngle(record.pose, right) > 150) {
        ++r.straight_arms;
        r.raw_arm.Add(m::PhoneArmLength(record.pose, right));
        r.arm.Add(m::PhoneArmLength(body, right));
      }
      const auto& ours = right ? hands.right : hands.left;
      const auto& game = right ? record.hands.right : record.hands.left;
      if (ours.reliable) r.our_reach.Add(ours.magnitude);
      if (record.hands.valid && record.hands.context) {
        r.game_reach.Add(game.magnitude);
        if (ours.reliable && ours.magnitude > 0.1f) r.ratio.Add(game.magnitude / ours.magnitude);
      }
      const auto& hand = body.joints[right ? 11 : 7];
      const auto& shoulder = body.joints[right ? 8 : 4];
      if (hand.confidence >= .5f && shoulder.confidence >= .5f) {
        const float d = m::Distance(hand, shoulder);
        if (d < 0.35f) {
          r.hand_shoulder.Add(d);
          r.hand_shoulder_dz.Add(hand.z - shoulder.z);
        }
      }
    }
  }
  return r;
}

// A body like the phone's on 1.0.4_candidate_25 (0.6 of an adult): arms down, the left arm out
// to the side, then the right hand on its shoulder (forearm up, the hand a little in front).
std::vector<m::MotionRecord> Synthetic() {
  std::vector<m::MotionRecord> records;
  const float k = 0.6f;
  for (int n = 0; n < 90; ++n) {
    m::MotionRecord r;
    r.milliseconds = uint32_t(n * 33);
    m::Frame& p = r.pose;
    p.tracked = true;
    p.session = 3;
    p.sequence = uint32_t(n + 1);
    for (auto& j : p.joints) j = {0, 0, 0, 0.9f};
    auto at = [&](int j, float x, float y, float z) { p.joints[size_t(j)] = {x * k, y * k, z * k, 0.9f}; };
    at(0, 0, 0, 0); at(1, 0, 0.26f, 0); at(2, 0, 0.52f, 0); at(3, 0, 0.75f, 0);
    at(4, 0.18f, 0.5f, 0); at(8, -0.18f, 0.5f, 0);
    at(12, 0.1f, 0, 0); at(13, 0.1f, -0.45f, 0); at(14, 0.1f, -0.85f, 0); at(15, 0.1f, -0.9f, 0.1f);
    at(16, -0.1f, 0, 0); at(17, -0.1f, -0.45f, 0); at(18, -0.1f, -0.85f, 0); at(19, -0.1f, -0.9f, 0.1f);
    if (n < 30) {  // arms down
      at(5, 0.2f, 0.2f, 0); at(6, 0.2f, -0.06f, 0); at(7, 0.2f, -0.14f, 0);
      at(9, -0.2f, 0.2f, 0); at(10, -0.2f, -0.06f, 0); at(11, -0.2f, -0.14f, 0);
    } else if (n < 60) {  // the left arm straight out to the side
      at(5, 0.48f, 0.5f, 0); at(6, 0.74f, 0.5f, 0); at(7, 0.82f, 0.5f, 0);
      at(9, -0.2f, 0.2f, 0); at(10, -0.2f, -0.06f, 0); at(11, -0.2f, -0.14f, 0);
    } else {  // the right hand on its shoulder
      at(5, 0.2f, 0.2f, 0); at(6, 0.2f, -0.06f, 0); at(7, 0.2f, -0.14f, 0);
      at(9, -0.22f, 0.2f, 0.02f); at(10, -0.2f, 0.45f, 0.06f); at(11, -0.19f, 0.52f, 0.06f);
    }
    r.hands.valid = n >= 30;
    r.hands.context = n >= 30;
    r.hands.left.magnitude = n >= 30 && n < 60 ? 0.3f : 0.1f;  // what DBZ made of a short arm
    records.push_back(r);
  }
  return records;
}

void CheckSynthetic() {
  const auto records = Synthetic();
  // The file format round trip, and files that do not read.
  const auto bytes = m::SaveRecording(records);
  CHECK(bytes.size() == 12 + records.size() * m::kRecordBytes);
  const auto back = m::LoadRecording(bytes.data(), bytes.size());
  CHECK(back.size() == records.size());
  CHECK(back[45].pose.joints[7].x == records[45].pose.joints[7].x && back[45].pose.sequence == 46);
  CHECK(back[45].hands.left.magnitude == 0.3f && back[45].milliseconds == 45 * 33);
  CHECK(m::LoadRecording(bytes.data(), bytes.size() - 1).empty());
  auto broken = bytes;
  broken[12 + 4] = 'Y';  // not an XMP1 packet
  CHECK(m::LoadRecording(broken.data(), broken.size()).empty());
  // Replayed: the short arm becomes an adult's, its reach passes DBZ's 0.4.
  Report r = Replay(back);
  CHECK(r.tracked == 90);
  CHECK(r.raw_arm.At(.5f) < 0.42f);
  CHECK(r.arm.At(.5f) > 0.6f && r.arm.At(.5f) < 0.72f);
  CHECK(std::fabs(r.factor.At(.5f) - 1 / 0.6f) < 0.02f);
  CHECK(r.our_reach.At(.9f) >= 0.4f);
  // The bent arm keeps its hand near the shoulder (within a forearm and a hand).
  CHECK(!r.hand_shoulder.v.empty() && r.hand_shoulder.At(.5f) < 0.3f);
  // The console's ring: the last kRecordFrames, times from the first kept one.
  m::MotionRecorder ring;
  const auto t0 = std::chrono::steady_clock::now();
  m::GameHands none;
  for (size_t n = 0; n < m::kRecordFrames + 40; ++n)
    ring.Add(records[n % records.size()].pose, none, t0 + std::chrono::milliseconds(33 * n));
  CHECK(ring.Size() == m::kRecordFrames);
  const auto taken = ring.Take();
  const auto kept = m::LoadRecording(taken.data(), taken.size());
  CHECK(kept.size() == m::kRecordFrames && kept.front().milliseconds == 0);
  CHECK(kept.back().milliseconds == 33 * (m::kRecordFrames - 1));
  m::MotionRecorder empty;
  m::Frame lost;
  empty.Add(lost, none, t0);
  CHECK(empty.Take().empty());  // nothing to save without a body
}

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const std::string game = ReadText("src/canary_game_main.cpp");
  if (cam.empty() || game.empty()) {
    std::printf("no assembled tree here; the recorder's place not checked\n");
    return;
  }
  CHECK(cam.find("xbox360ps5::motion::motion_recorder.Add(xbox360ps5::motion::Snapshot(),") != std::string::npos);
  CHECK(game.find("if (guide.open) SaveMotionRecording(log_root);") != std::string::npos);
  std::printf("tree: the recorder is fed by the skeleton frames and saved when the guide opens\n");
}

int ReportFiles(int argc, char** argv) {
  int bad = 0;
  for (int n = 1; n < argc; ++n) {
    std::ifstream in(argv[n], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto records = m::LoadRecording(bytes.data(), bytes.size());
    if (records.empty()) {
      std::printf("%s: not a motion recording\n", argv[n]);
      ++bad;
      continue;
    }
    Report r = Replay(records);
    std::printf("%s: %d frames over %.1f s, %d with a body\n", argv[n], r.frames,
                records.back().milliseconds / 1000.0, r.tracked);
    std::printf("  straight arms (elbow > 150 deg): %d; phone %s m; to the title %s m\n", r.straight_arms,
                r.raw_arm.Text().c_str(), r.arm.Text().c_str());
    std::printf("  body scale %s\n", r.factor.Text().c_str());
    std::printf("  reach, estimated %s; the title's %s; title/estimated %s\n", r.our_reach.Text().c_str(),
                r.game_reach.Text().c_str(), r.ratio.Text().c_str());
    std::printf("  hands within 0.35 m of their shoulder: %s m, depth part %s m\n", r.hand_shoulder.Text().c_str(),
                r.hand_shoulder_dz.Text().c_str());
  }
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 1) return ReportFiles(argc, argv);
  CheckSynthetic();
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-motion-replay: all passed\n");
  return 0;
}
