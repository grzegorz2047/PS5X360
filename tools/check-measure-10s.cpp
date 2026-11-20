// SPDX-License-Identifier: MIT
// Fix 54 (tools/source-1.0.2/fixes/54-measure-10s.patch): the guide's "Measure performance"
// samples the game's threads for 10 s instead of 5 s, and says so.
//  (a) the assembled canary_game_main.cpp: kMeasureSeconds is 10, MeasurePerformance's sampling
//      loop runs for it (no 5 s left in the function), the guide's row is
//      Tr("Measure performance (10 s)") and the 5 s text is gone;
//  (b) every language file (argument: tools/source-1.0.2/english/lang) has the 10 s key with a
//      translation that names 10 and not 5, and no 5 s key (english/rekey.py RENAMES, fix 25's
//      rules: the key fix 25 made is renamed in each language's text).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
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

void CheckMain() {
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(!main_text.empty(), "read src/canary_game_main.cpp");
  Expect(main_text.find("constexpr int kMeasureSeconds = 10;") != std::string::npos, "the measurement lasts 10 s");
  const size_t start = main_text.find("void MeasurePerformance(xe::Emulator& emulator, float fps) {");
  const size_t end = main_text.find("\n}", start);
  Expect(start != std::string::npos && end != std::string::npos, "MeasurePerformance");
  if (start == std::string::npos || end == std::string::npos) return;
  const std::string body = main_text.substr(start, end - start);
  Expect(body.find("while (std::chrono::steady_clock::now() - started < std::chrono::seconds(kMeasureSeconds)) {") !=
             std::string::npos,
         "the sampling loop runs for kMeasureSeconds");
  Expect(body.find("seconds(5)") == std::string::npos, "no 5 s left in the measurement");
  Expect(main_text.find("Tr(\"Measure performance (10 s)\")") != std::string::npos, "the guide's row says 10 s");
  Expect(main_text.find("Measure performance (5 s)") == std::string::npos, "the 5 s text is gone");
}

void CheckLanguages(const std::string& folder) {
  namespace fs = std::filesystem;
  int files = 0;
  const std::string key = "\nMeasure\\sperformance\\s(10\\ss) = ";
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string name = entry.path().filename().string();
    const std::string text = Read(entry.path().string());
    Expect(text.find("Measure\\sperformance\\s(5\\ss)") == std::string::npos, name + ": no 5 s key");
    const size_t at = text.find(key);
    Expect(at != std::string::npos, name + ": the 10 s key");
    if (at == std::string::npos) continue;
    const size_t from = at + key.size();
    const std::string value = text.substr(from, text.find('\n', from) - from);
    Expect(value.find("10") != std::string::npos && value.find('5') == std::string::npos, name + ": 10, not 5, in " + value);
  }
  Expect(files == 16, "16 language files (" + std::to_string(files) + ")");
}

}  // namespace

int main(int argc, char** argv) {
  CheckMain();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "the language folder is given");
  if (failures) {
    std::printf("check-measure-10s: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-measure-10s: ok\n");
  return 0;
}
