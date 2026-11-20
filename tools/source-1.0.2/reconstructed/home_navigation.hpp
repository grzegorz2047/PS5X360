// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): the original file is
// build/ui-kinect-20261020/home_navigation.hpp, not published. Declares what launcher.cpp uses.
#pragma once
#include <cstdint>

namespace kinect_ui {
enum class FaceButton { cross, circle, square, triangle };
// The colour a button hint is drawn in (0xRRGGBB): the Xbox 360 pad's A, B, X and Y colours, as
// the official 1.0.2 eboot draws them (fix 40: Canvas::Hint at 0x210f86-0x211349 and NxeHint at
// 0x211f82-0x211fb0 load 0x55a329, 0xd84639, 0x287ac3 and 0xe3b52d). The callers pick their own
// colour for a button that is none of the four, so the last return is never reached.
inline uint32_t HintRgb(FaceButton button) {
  switch (button) {
    case FaceButton::cross: return 0x55a329;
    case FaceButton::circle: return 0xd84639;
    case FaceButton::square: return 0x287ac3;
    case FaceButton::triangle: return 0xe3b52d;
  }
  return 0x9aa3b2;
}
// The Kinect home's root navigation state. 1.0.0's kinect_*.inc replaced its card page, so
// launcher.cpp only keeps an instance.
struct HomeNavigation {};
}  // namespace kinect_ui
