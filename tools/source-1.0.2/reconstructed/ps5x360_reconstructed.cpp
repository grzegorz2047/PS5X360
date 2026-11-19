// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): definitions that 1.0.x's
// published sources call but that live in unpublished files. See reconstructed/README.md.
#include <atomic>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

// Defined in an unpublished file of 1.0.x; declared by the core's vulkan_presenter.cc and set by
// presenter_sidecar.hpp from /app0/framegen.conf (the menu's frame generation switch).
DEFINE_bool(ps5_frame_interpolation, false,
            "Frame generation (ps5-framegen); set from framegen.conf.", "GPU");

namespace xbox360ps5 {
// 1.0.x radv_dispatch.cpp chooses the console's OpenGL driver under the Vulkan renderer
// (through glvk). Neither glvk nor the GL driver is published, so the choice is only logged:
// the game keeps drawing with RADV (xbox360ps5::OpenGLDriverSelected stays false).
void SelectOpenGLDriver(bool opengl) {
  if (opengl) XELOGW("OpenGL renderer: not part of this source rebuild; Vulkan is used");
}
}  // namespace xbox360ps5

namespace glvk {
// The Vulkan-on-OpenGL layer (unpublished).
void SetLogSink(void (*)(const char*)) {}
// -1: "nothing but black reached the screen". canary_game_main.cpp then turns the OpenGL
// choice off and restarts with Vulkan, which is what this build can draw with.
int PictureState() { return -1; }
}  // namespace glvk
