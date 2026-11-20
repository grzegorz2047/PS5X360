// SPDX-License-Identifier: MIT
// The reconstructed game information reader (reconstructed/xbox360ps5/game_info.hpp, GameInfoFor;
// fix 43 lets a check point it at a file, game_info::Path) on the official package's own archive:
// the argument is tools/source-1.0.2/fixtures/gameinfo-excerpt.txt, the header and four sections of
// PPSA50011/assets/gameinfo.txt of the official v1.0.2 package byte for byte (its header says which
// file). BRINK (303407D4) gives its title, genre, developer, publisher, release date and text and
// no rating or compatibility; an id not in the archive gives nothing; a value keeps the '=' after
// the first; the same id asked again is answered without reading the file; a missing file gives
// nothing and is not remembered as indexed; CRLF lines read the same.
// With PS5X360_GAMEINFO naming the packaged assets/gameinfo.txt (3,759,451 bytes), the whole
// archive is read too: 6,508 sections, each key's count, and the excerpt's sections the same there.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/game_info.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace gi = xbox360ps5::game_info;
using Fields = std::map<std::string, std::string>;

const char* const kExcerptIds[] = {"00000000", "303407D4", "435607D7", "58550281"};

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

fs::path TempRoot() {
  char pattern[] = "/tmp/check-game-info-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  return made;
}

// A fresh reader on another file (the console reads one file for the process).
void Use(const fs::path& path) {
  gi::Shared() = gi::Index{};
  gi::Path() = path.string();
}

bool Has(const Fields& fields, const char* key, const char* want) {
  const auto found = fields.find(key);
  if (found != fields.end() && found->second == want) return true;
  std::fprintf(stderr, "  %s: got %s, want \"%s\"\n", key,
               found == fields.end() ? "nothing" : ("\"" + found->second + "\"").c_str(), want);
  return false;
}

void Brink(const Fields& brink) {
  assert(Has(brink, "title", "BRINK"));
  assert(Has(brink, "genre", "Shooter"));
  assert(Has(brink, "developer", "SPLASH DAMAGE"));
  assert(Has(brink, "publisher", "Bethesda Softworks"));
  assert(Has(brink, "released", "2009-12-18"));
  assert(brink.at("about").rfind("Brink is an immersive first-person shooter", 0) == 0);
  assert(brink.at("about").size() > 400 && brink.at("about").back() == '.');
  // Absent optional keys: the game page leaves those rows out.
  assert(!brink.count("rating") && !brink.count("compat") && !brink.count("compat_issue"));
  assert(brink.size() == 6);
}

void Excerpt(const fs::path& root, const fs::path& fixture) {
  assert(gi::Path() == "/app0/assets/gameinfo.txt" && gi::Path() == gi::kPath);
  const fs::path file = root / "gameinfo.txt";
  fs::copy_file(fixture, file);
  Use(file);
  const Fields brink = xbox360ps5::GameInfoFor("303407D4");
  Brink(brink);
  assert(gi::Shared().loaded && gi::Shared().sections.size() == 4);
  assert(xbox360ps5::GameInfoFor("303407d4") == brink);  // ids in any case
  std::printf("  303407D4: BRINK / Shooter / SPLASH DAMAGE, no rating or compatibility\n");

  // Not in the archive.
  assert(xbox360ps5::GameInfoFor("4D5308D6").empty());
  assert(xbox360ps5::GameInfoFor("").empty());
  // The first section (no text) and two others.
  const Fields unknown = xbox360ps5::GameInfoFor("00000000");
  assert(Has(unknown, "title", "Unknown Game") && Has(unknown, "compat", "Unknown") && Has(unknown, "compat_issue", "780"));
  assert(!unknown.count("about") && unknown.size() == 8);
  assert(std::strcmp(xbox360ps5::CompatStateText(unknown.at("compat")), "Not rated") == 0);
  const Fields pinball = xbox360ps5::GameInfoFor("435607D7");
  assert(Has(pinball, "title", "Pinball Hall of Fame") && Has(pinball, "genre", "Other, Strategy & Simulation"));
  assert(Has(pinball, "rating", "4.75") && Has(pinball, "compat", "Gameplay") && Has(pinball, "compat_issue", "912"));
  assert(Has(pinball, "about", "Welcome to Pinball Hall of Fame!") && !pinball.count("released"));
  assert(std::strcmp(xbox360ps5::CompatStateText(pinball.at("compat")), "Playable, with issues") == 0);
  // The last section, ended by the file; its text has '=' in it.
  const Fields reef = xbox360ps5::GameInfoFor("58550281");
  assert(Has(reef, "about", "This game was classified by the community with the following category scores - "
                            "Violence=0/3, Sex=0/3, Mature Content=0/3. See the Rave."));
  assert(Has(reef, "rating", "2.50") && reef.size() == 6);
  std::printf("  an id not in the archive: nothing; the first and last sections; '=' inside a text kept\n");

  // The same id again: the kept answer, the file not read (here: moved away).
  assert(xbox360ps5::GameInfoFor("303407D4") == brink);
  fs::rename(file, root / "moved.txt");
  assert(xbox360ps5::GameInfoFor("303407D4") == brink);
  assert(xbox360ps5::GameInfoFor("303407d4") == brink);
  assert(xbox360ps5::GameInfoFor("435607D7").empty());  // another id needs the file
  assert(xbox360ps5::GameInfoFor("303407D4") == brink);  // still the kept answer
  fs::rename(root / "moved.txt", file);
  assert(xbox360ps5::GameInfoFor("435607D7") == pinball);  // the index made at the first lookup
  assert(gi::Shared().sections.size() == 4);
  std::printf("  the same id again: answered without the file; the index is made once\n");

  // No file: nothing, and the next lookup with the file indexes it.
  Use(root / "none.txt");
  assert(xbox360ps5::GameInfoFor("303407D4").empty());
  assert(!gi::Shared().loaded);
  gi::Path() = file.string();
  Brink(xbox360ps5::GameInfoFor("303407D4"));
  // CRLF lines (the file has none; an edited copy might).
  std::string crlf;
  for (const char c : Read(file)) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  Write(root / "crlf.txt", crlf);
  Use(root / "crlf.txt");
  for (const char* id : kExcerptIds) {
    Use(file);
    const Fields lf = xbox360ps5::GameInfoFor(id);
    Use(root / "crlf.txt");
    assert(xbox360ps5::GameInfoFor(id) == lf);
  }
  std::printf("  no file: nothing, not indexed; CRLF lines read the same\n");
}

// The whole packaged archive (PS5X360_GAMEINFO), against the excerpt.
void Packaged(const fs::path& packaged, const fs::path& fixture) {
  assert(fs::file_size(packaged) == 3759451);
  std::map<std::string, Fields> excerpt;
  Use(fixture);
  for (const char* id : kExcerptIds) excerpt[id] = xbox360ps5::GameInfoFor(id);
  Use(packaged);
  Brink(xbox360ps5::GameInfoFor("303407D4"));
  assert(gi::Shared().sections.size() == 6508);
  std::vector<std::string> ids;
  for (const auto& [id, offset] : gi::Shared().sections) ids.push_back(id);
  std::map<std::string, int> keys;
  for (const std::string& id : ids) {
    const Fields fields = xbox360ps5::GameInfoFor(id);
    assert(!fields.empty());
    for (const auto& [key, value] : fields) ++keys[key];
  }
  const std::map<std::string, int> want = {{"title", 6508},     {"about", 6507},    {"publisher", 6499},
                                           {"genre", 6494},     {"developer", 6493}, {"rating", 6003},
                                           {"released", 2853},  {"compat", 1070},    {"compat_issue", 1070}};
  for (const auto& [key, count] : keys) std::printf("    %s %d\n", key.c_str(), count);
  assert(keys == want);
  for (const auto& [id, fields] : excerpt) assert(xbox360ps5::GameInfoFor(id) == fields);
  std::printf("  the packaged archive: 6,508 sections, keys as counted, the excerpt's sections the same\n");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <tools/source-1.0.2/fixtures/gameinfo-excerpt.txt>\n", argv[0]);
    return 2;
  }
  const fs::path fixture = argv[1];
  assert(fs::is_regular_file(fixture));
  const fs::path root = TempRoot();
  Excerpt(root, fixture);
  if (const char* packaged = std::getenv("PS5X360_GAMEINFO"); packaged && *packaged) {
    Packaged(packaged, fixture);
  } else {
    std::printf("  (PS5X360_GAMEINFO not set: the packaged archive itself not read)\n");
  }
  fs::remove_all(root);
  std::puts("PASS: the game information reader on the official package's archive (fix 43)");
}
