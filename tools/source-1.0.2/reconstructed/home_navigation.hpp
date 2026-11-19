// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): the original file is
// build/ui-kinect-20261020/home_navigation.hpp, not published. Declares what launcher.cpp uses.
#pragma once
#include <cstdint>

namespace kinect_ui {
enum class FaceButton { cross, circle, square, triangle };
// The colour a button hint is drawn in (0xRRGGBB): the controller's symbol colours.
inline uint32_t HintRgb(FaceButton button) {
  switch (button) {
    case FaceButton::cross: return 0x7fa6e8;
    case FaceButton::circle: return 0xe8636b;
    case FaceButton::square: return 0xd88ccf;
    case FaceButton::triangle: return 0x3fbf9b;
  }
  return 0x9aa3b2;
}
// The Kinect home's root navigation state. 1.0.0's kinect_*.inc replaced its card page, so
// launcher.cpp only keeps an instance.
struct HomeNavigation {};
}  // namespace kinect_ui
