// SPDX-License-Identifier: MIT
// Fix 56 (tools/source-1.0.2/fixes/56-kinect-pairing.patch): pairing the phone as the Kinect sensor.
// The phone camera page (the official package's assets/kinect/index.html, served by the console)
// told a phone scanned from the library "Turn on Camera motion in the Kinect game's settings on
// the PS5 first." with the setting on (the game had not started), and the pairing key was only in
// small text at the bottom of the library.
//  (a) include/xbox360ps5/motion_input.hpp: the status JSON keeps its fields and adds
//      game_running and waiting_for: "game" before a game runs, "setting" in a running game with
//      the camera off, "" once it is on;
//  (b) include/xbox360ps5/kinect_page_patch.hpp on the page's lines (argument: an excerpt of the
//      official page, tools/source-1.0.2/fixtures/kinect-index-excerpt.html): every edit made
//      once; connect() no longer stops on enabled:false but names the reason and sets 'waiting';
//      the two-second retry includes 'waiting'; a quiet retry keeps the reason; English,
//      Portuguese and Spanish get both texts; a page without the expected lines, or with one of
//      them twice, is served unchanged;
//  (c) the assembled tree: the HTTPS server patches the page it serves; both servers compare the
//      key in lower case; the title says a game runs once it has launched; the library shows the
//      key large beside both Kinect QR codes.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/kinect_page_patch.hpp"
#include "xbox360ps5/motion_input.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

namespace motion = xbox360ps5::motion;

void CheckStatus() {
  motion::SetEnabled(false);
  motion::game_running = false;
  std::string status = motion::Status();
  std::printf("   %s\n", status.c_str());
  Expect(status.rfind("{\"enabled\":false,\"game_running\":false,\"waiting_for\":\"game\",\"tracked\":false,", 0) == 0,
         "before a game: waiting for the game");
  Expect(Has(status, "\"guest_camera_open\":false") && Has(status, "\"guest_skeleton_bodies\":0}"),
         "the earlier fields are still there");
  motion::game_running = true;
  Expect(Has(motion::Status(), "\"game_running\":true,\"waiting_for\":\"setting\""), "a game with the camera off: the setting");
  motion::SetEnabled(true);
  Expect(Has(motion::Status(), "{\"enabled\":true,\"game_running\":true,\"waiting_for\":\"\""), "camera on: nothing to wait for");
  motion::SetEnabled(false);
  motion::game_running = false;
}

void CheckPage(const std::string& excerpt_path) {
  namespace page = xbox360ps5::kinect_page;
  const std::string excerpt = Read(excerpt_path);
  Expect(!excerpt.empty(), "read " + excerpt_path);
  const auto patched = page::Patch(excerpt);
  Expect(patched.applied == page::Edits().size() && page::Edits().size() == 6, "every edit made");
  const std::string& p = patched.page;
  Expect(!Has(p, "if (!result.enabled) throw Error(T('enable_motion'));"), "connect() no longer stops there");
  Expect(Has(p, "paired = false; pairing = 'waiting';") &&
             Has(p, "result.waiting_for === 'game' ? T('wait_game') :") &&
             Has(p, "result.waiting_for === 'setting' ? T('wait_setting') : T('enable_motion');"),
         "connect() names the reason and waits");
  Expect(Has(p, "(pairing === 'lost' || pairing === 'expired' || pairing === 'waiting')"), "the retry also while waiting");
  Expect(Has(p, "if (!quiet || (pairing !== 'lost' && pairing !== 'waiting')) relay.textContent = e.message;"),
         "a quiet retry keeps the reason");
  size_t game_texts = 0, setting_texts = 0;
  for (size_t at = p.find("wait_game:'"); at != std::string::npos; at = p.find("wait_game:'", at + 1)) ++game_texts;
  for (size_t at = p.find("wait_setting:'"); at != std::string::npos; at = p.find("wait_setting:'", at + 1)) ++setting_texts;
  Expect(game_texts == 3 && setting_texts == 3, "English, Portuguese and Spanish texts");
  Expect(Has(p, "Start the Kinect game on the PS5: this page connects by itself when it runs.") &&
             Has(p, "Inicie o jogo de Kinect no PS5") && Has(p, "Inicia el juego de Kinect en la PS5"),
         "the texts name the game");
  Expect(Has(p, "Turn it on in the guide or the game\\'s settings"), "the quote is escaped for the page's string");
  // Another page: served as it is.
  const auto other = page::Patch("<html>another page</html>");
  Expect(other.applied == 0 && other.page == "<html>another page</html>", "another page unchanged");
  const auto twice = page::Patch(excerpt + excerpt);
  Expect(twice.applied == 0 && twice.page == excerpt + excerpt, "a line found twice: unchanged");
  std::string partial = excerpt;
  partial.erase(partial.find("motion_off:'Camera motion is off"), 5);
  const auto missing = page::Patch(partial);
  Expect(missing.applied == 0 && missing.page == partial, "a line missing: unchanged (all or none)");
}

void CheckTree() {
  const std::string tls = Read("port/phone_camera_tls.cpp");
  const size_t meta = tls.find("content=\\\"ps5\\\">\");");
  const size_t patch = tls.find("page = kinect_page::Patch(std::move(page)).page;", meta);
  Expect(meta != std::string::npos && patch != std::string::npos, "the HTTPS server patches the page it serves");
  Expect(Has(tls, "std::string given = Header(head, \"X-Key\");") &&
             Has(tls, "for (char& c : given) c = char(std::tolower(static_cast<unsigned char>(c)));") &&
             Has(tls, "if (given != key) return Response(\"403 Forbidden\""),
         "the HTTPS server compares the key in lower case");
  const std::string settings = Read("platform/ps5/web_settings.cpp");
  const size_t parameter = settings.find("if (given.empty()) given = Parameter(query, \"k\");");
  const size_t lower = settings.find("for (char& c : given) c = char(std::tolower(static_cast<unsigned char>(c)));", parameter);
  const size_t accept = settings.find("web_key::Accept(given, key, official_key", parameter);
  Expect(parameter != std::string::npos && lower < accept && accept != std::string::npos,
         "the settings server lowers the key before comparing it");
  const std::string main_text = Read("src/canary_game_main.cpp");
  const size_t launched = main_text.find("XELOGI(\"GAME LAUNCH {:08X}\", status);");
  const size_t running = main_text.find("xbox360ps5::motion::game_running = status == 0;", launched);
  Expect(launched != std::string::npos && running != std::string::npos && running - launched < 200,
         "the title says a game runs once it has launched");
  const std::string launcher = Read("src/launcher.cpp");
  size_t keys = 0;
  for (size_t at = launcher.find("DrawQrCard(c, camera_qr,"); at != std::string::npos;
       at = launcher.find("DrawQrCard(c, camera_qr,", at + 1)) {
    const std::string after = launcher.substr(at, 300);
    if (Has(after, "c.Text(std::string(Tr(\"Code: \")) + web_key_,")) ++keys;
  }
  Expect(keys == 2, "the key beside both Kinect QR codes (" + std::to_string(keys) + ")");
}

}  // namespace

int main(int argc, char** argv) {
  CheckStatus();
  if (argc > 1) CheckPage(argv[1]);
  else Expect(false, "the page excerpt is given");
  CheckTree();
  if (failures) {
    std::printf("check-kinect-pairing: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-pairing: ok\n");
  return 0;
}
