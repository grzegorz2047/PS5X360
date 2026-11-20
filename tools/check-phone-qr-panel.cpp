// Fix 63: the motion panel over a running game shows the camera QR code while no phone sends
// frames (include/xbox360ps5/phone_qr_panel.hpp).
//
//   check-phone-qr-panel <english/lang folder>     run from an assembled tree's root
//
// Checks when the code or the skeleton is shown, the link (the library's), that the code fits
// in the panel's unchanged box and is readable at 1080p and 4K, the texts in every language
// file, and the tree's overlay.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/phone_qr_panel.hpp"
#include "xbox360ps5/qr_code.hpp"

namespace m = xbox360ps5::motion;
using clock_type = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void CheckView() {
  const std::string url = "http://192.168.1.170:8360/camera?k=6e1456";
  const auto now = clock_type::now();
  using std::chrono::milliseconds;
  // No frame since the setting was turned on: the code.
  CHECK(m::ChoosePanelView(clock_type::time_point{}, now, url) == m::PanelView::kPhoneQr);
  // Frames arriving (tracked or not: a phone without a body in view is connected): the skeleton.
  CHECK(m::ChoosePanelView(now - milliseconds(30), now, url) == m::PanelView::kSkeleton);
  CHECK(m::ChoosePanelView(now - milliseconds(2900), now, url) == m::PanelView::kSkeleton);
  CHECK(m::ChoosePanelView(now - m::kPhoneQuiet, now, url) == m::PanelView::kSkeleton);
  // The phone stopped: the code comes back.
  CHECK(m::ChoosePanelView(now - m::kPhoneQuiet - milliseconds(1), now, url) == m::PanelView::kPhoneQr);
  CHECK(m::ChoosePanelView(now - std::chrono::minutes(5), now, url) == m::PanelView::kPhoneQr);
  // No address or key (the settings server is off): nothing to scan, the old panel.
  CHECK(m::ChoosePanelView(clock_type::time_point{}, now, "") == m::PanelView::kSkeleton);
  CHECK(m::ChoosePanelView(now - std::chrono::minutes(5), now, "") == m::PanelView::kSkeleton);
}

void CheckUrlAndLayout() {
  CHECK(m::CameraPageUrl("http://192.168.1.170:8360/", "6e1456") == "http://192.168.1.170:8360/camera?k=6e1456");
  CHECK(m::CameraPageUrl("", "6e1456").empty());
  CHECK(m::CameraPageUrl("http://192.168.1.170:8360/", "").empty());
  // The longest address a console gets (four three-digit octets, a five-digit port, a long key).
  const auto qr = xbox360ps5::MakeQrCode(m::CameraPageUrl("http://192.168.100.200:65535/", "0123456789ab"));
  CHECK(qr.size > 0);
  const auto& at = m::kQrPanel;
  // The panel keeps its box; everything is inside it.
  CHECK(at.left == 1550 && at.top == 100 && at.right == 1900 && at.bottom == 640);
  CHECK(at.card_x >= at.left && at.card_x + at.card <= at.right);
  CHECK(at.card_y >= at.title_y + at.title_size && at.card_y + at.card <= at.code_y);
  CHECK(at.code_y + at.code_size <= at.text_y);
  CHECK(at.text_x + at.text_width <= at.right);
  CHECK(at.text_y + 6 * at.text_size * 1.2f <= at.bottom);  // up to six wrapped lines
  // Readable modules: at least 4 pixels at 1080p, twice that at 4K (2160 / 1080).
  for (int size : {21, 25, 29, 33, 37}) {
    const float m1080 = m::QrModulePixels(size, at.card, 1.0f), m4k = m::QrModulePixels(size, at.card, 2.0f);
    CHECK(m1080 >= 4 && m4k >= 2 * m1080);
    CHECK(m1080 * size <= at.card - 16 && m4k * size <= (at.card - 16) * 2);
  }
  CHECK(m::QrModulePixels(qr.size, at.card, 1.0f) >= 4);
  CHECK(m::QrModulePixels(0, at.card, 1.0f) == 0);
  CHECK(m::QrModulePixels(400, at.card, 1.0f) == 0);
}

// A key as the language files write it: spaces as "\s".
std::string FileKey(const std::string& text) {
  std::string out;
  for (char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
  return out;
}

void CheckLanguages(const std::string& folder) {
  const char* kTexts[] = {"Motion: no phone connected", "Scan with your phone to use it as Kinect.",
                          "Same Wi-Fi, stand 2-3 m back, full body visible.", "Code: "};
  const char* kLanguages[] = {"de", "en", "es", "es-419", "fr", "it", "ja", "ko", "nl", "pl",
                              "pt", "pt-BR", "ru", "tr", "zh-Hans", "zh-Hant"};
  for (const char* language : kLanguages) {
    const std::string text = ReadText(folder + "/" + language + ".txt");
    CHECK(!text.empty());
    for (const char* key : kTexts) {
      const bool found = text.find("\n" + FileKey(key) + " = ") != std::string::npos ||
                         text.rfind(FileKey(key) + " = ", 0) == 0;
      if (!found) std::printf("missing in %s.txt: %s\n", language, key);
      CHECK(found);
    }
  }
}

void CheckTree() {
  const std::string game = ReadText("src/canary_game_main.cpp"), launcher = ReadText("src/launcher.cpp");
  if (game.empty()) {
    std::printf("no assembled tree here; the overlay not checked\n");
    return;
  }
  // The overlay asks phone_qr_panel.hpp, with the library's generator and link.
  CHECK(game.find("#include \"xbox360ps5/phone_qr_panel.hpp\"") != std::string::npos);
  CHECK(game.find("xbox360ps5::motion::ChoosePanelView(pose.received, now, phone_url_)") != std::string::npos);
  CHECK(game.find("xbox360ps5::MakeQrCode(url)") != std::string::npos);
  CHECK(game.find("xbox360ps5::motion::CameraPageUrl(xbox360ps5::WebSettingsPlainAddress(), phone_key_)") !=
        std::string::npos);
  CHECK(launcher.find("plain + \"camera?k=\" + key") != std::string::npos);  // the library's link, same form
  // The panel's box is the one it always had.
  CHECK(game.find("draw->AddRectFilled(ImVec2(1550*scale,100*scale),ImVec2(1900*scale,640*scale)") != std::string::npos);
  // Its texts go through Tr (English keys, fix 25).
  for (const char* key : {"Tr(\"Motion: no phone connected\")", "Tr(\"Scan with your phone to use it as Kinect.\")",
                          "Tr(\"Same Wi-Fi, stand 2-3 m back, full body visible.\")", "Tr(\"Code: \")"})
    CHECK(game.find(key) != std::string::npos);
  std::printf("tree: the motion panel shows the camera QR code while no phone sends frames\n");
}

int main(int argc, char** argv) {
  CheckView();
  CheckUrlAndLayout();
  if (argc > 1) CheckLanguages(argv[1]);
  else std::printf("no language folder given; texts not checked\n");
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-phone-qr-panel: all passed\n");
  return 0;
}
