// SPDX-License-Identifier: MIT
// Fix 43 (tools/source-1.0.2/fixes/43-ui-lang-overlay.patch) and the reconstructed language
// loader (reconstructed/xbox360ps5/i18n_ui.hpp) against the official 1.0.2 executable:
//  - UiLanguageCode against the official loader's table (0x1e63e48: ids 2-19 as id-2, anything
//    else "en"; id 9, the official's source language, reads no file there, pt-BR.txt here since
//    fix 25), SupportedUiLanguage against the settings table (0x1e63e90: 10, 13 and 15 are
//    English) and UiLanguageIsCjk against the CJK font loop's mask (0x20184);
//  - every interface language of the assembled launcher's kLanguages has its file, and every file
//    of tools/source-1.0.2/english/lang (the argument) parses with the reconstructed parser: no
//    malformed line, no line lost, each key once, escapes ("\s") canonical as rekey.py writes them;
//  - fix 43: /app0/lang/<code>.txt is read after /app0/assets/lang/<code>.txt into the same
//    table (0x1c1b4d, 0x1c1bbe), adding keys and replacing text; without that folder or file the
//    table is the package file's alone, as before. Folders are temporary ones (i18n_ui::Folder,
//    OverlayFolder).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/i18n.hpp"

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace ui = xbox360ps5::i18n_ui;
using Map = std::map<std::string, std::string>;

void Write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

fs::path TempRoot() {
  char pattern[] = "/tmp/check-ui-lang-overlay-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  return made;
}

bool Is(const char* got, const char* want) {
  if (got && want && std::strcmp(got, want) == 0) return true;
  if (!got && !want) return true;
  std::fprintf(stderr, "  got %s%s%s, want %s%s%s\n", got ? "\"" : "", got ? got : "nothing", got ? "\"" : "",
               want ? "\"" : "", want ? want : "nothing", want ? "\"" : "");
  return false;
}

const Map& Loaded() { return ui::Current()->text; }

// rekey.py's escape(): what a generated file holds for a key or a text.
std::string Escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case ' ': out += "\\s"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  if (!out.empty() && out[0] == '#') out.insert(0, 1, '\\');
  return out;
}

void Defaults() {
  assert(ui::Folder() == "/app0/assets/lang/");
  assert(ui::OverlayFolder() == "/app0/lang/");
  const std::vector<std::string> polish = {"/app0/assets/lang/pl.txt", "/app0/lang/pl.txt"};
  assert(ui::Files(11) == polish);
  const std::vector<std::string> brazilian = {"/app0/assets/lang/pt-BR.txt", "/app0/lang/pt-BR.txt"};
  assert(ui::Files(9) == brazilian);
  std::printf("  folders: /app0/assets/lang/<code>.txt, then /app0/lang/<code>.txt\n");
}

void OfficialTables() {
  // 0x1c1a50: id 9 skips the files; id-2 in 0..0x11 indexes the table at 0x1e63e48; else "en"
  // (0x1db0422). Entry 7 (id 9) is "en" but never read.
  static const char* const kOfficial[20] = {"en", "en",  "ja", "de", "fr",      "es",      "it",
                                            "ko", "zh-Hant", "en", "en", "pl",      "ru",      "en",
                                            "tr", "en",  "nl", "zh-Hans", "es-419", "pt"};
  for (int id = 0; id < 20; ++id) {
    if (id == 9) continue;
    if (!Is(xbox360ps5::UiLanguageCode(id), kOfficial[id])) {
      std::fprintf(stderr, "FAIL: UiLanguageCode(%d)\n", id);
      std::exit(1);
    }
  }
  // Brazilian Portuguese: the official's source language (no file); a file like the others since
  // fix 25 (the keys are English).
  assert(Is(xbox360ps5::UiLanguageCode(9), "pt-BR"));
  for (const int id : {-1, 20, 21, 100, INT_MIN, INT_MAX}) assert(Is(xbox360ps5::UiLanguageCode(id), "en"));
  // 0x1e63e90: the setting's values 1..19 as interface languages.
  static const int kSupported[19] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 11, 12, 1, 14, 1, 16, 17, 18, 19};
  for (int id = 1; id <= 19; ++id) assert(xbox360ps5::SupportedUiLanguage(id) == kSupported[id - 1]);
  // 0x1c27e6: CJK fonts for ids <= 0x11 in the mask 0x20184 (Japanese, Korean, both Chinese).
  for (int id = 0; id <= 19; ++id)
    assert(xbox360ps5::UiLanguageIsCjk(id) == (id <= 17 && ((0x20184u >> id) & 1u)));
  std::printf("  UiLanguageCode = the official table for ids 0-19 (9: pt-BR.txt, fix 25), others en;"
              " SupportedUiLanguage and UiLanguageIsCjk as the official\n");
}

std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

// The interface languages the assembled launcher offers (kLanguages: {id, "name"}).
std::vector<int> LauncherLanguages() {
  const std::string launcher = ReadTree("src/launcher.cpp");
  const auto begin = launcher.find("constexpr Language kLanguages[] = {");
  assert(begin != std::string::npos);
  const auto end = launcher.find("};", begin);
  const std::string table = launcher.substr(begin, end - begin);
  std::vector<int> ids;
  const std::regex entry(R"(\{(\d+), ")");
  for (auto it = std::sregex_iterator(table.begin(), table.end(), entry); it != std::sregex_iterator(); ++it)
    ids.push_back(std::stoi((*it)[1].str()));
  // The CJK glyphs come from the language's files, the overlay's too (fix 43).
  assert(launcher.find("for (const std::string& path : i18n_ui::Files(language))") != std::string::npos);
  assert(launcher.find("std::string(\"/app0/assets/lang/\")") == std::string::npos);
  return ids;
}

// One generated file: its content lines checked and parsed independently, then compared with the
// reconstructed parser's table.
size_t CheckFile(const fs::path& path, int id) {
  const std::string text = Read(path);
  assert(!text.empty());
  Map want;
  size_t lines = 0, number = 0, bad = 0;
  std::istringstream input(text);
  for (std::string line; std::getline(input, line);) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    ++lines;
    const size_t split = line.find(" = ");
    if (split == std::string::npos) {
      std::fprintf(stderr, "  %s:%zu: no \" = \": %s\n", path.c_str(), number, line.c_str());
      ++bad;
      continue;
    }
    const std::string raw_key = line.substr(0, split), raw_text = line.substr(split + 3);
    const std::string key = ui::Unescape(raw_key), value = ui::Unescape(raw_text);
    if (key.empty() || value.empty() || Escape(key) != raw_key || Escape(value) != raw_text) {
      std::fprintf(stderr, "  %s:%zu: empty or not escaped as rekey.py writes: %s\n", path.c_str(), number,
                   line.c_str());
      ++bad;
    }
    if (!want.emplace(key, value).second) {
      std::fprintf(stderr, "  %s:%zu: key again: %s\n", path.c_str(), number, raw_key.c_str());
      ++bad;
    }
  }
  if (bad) {
    std::fprintf(stderr, "FAIL: %s: %zu malformed lines\n", path.c_str(), bad);
    std::exit(1);
  }
  // The reconstructed parser keeps every line.
  ui::Table table;
  ui::ReadFile(path.string(), table);
  assert(table.text.size() == lines);
  assert(table.text == want);
  // As loaded for its language (no overlay folder here).
  xbox360ps5::LoadUiTranslations(id);
  assert(Loaded() == want);
  return lines;
}

void GeneratedFiles(const fs::path& folder, const fs::path& no_folder) {
  ui::Folder() = folder.string() + "/";
  ui::OverlayFolder() = no_folder.string() + "/";
  std::set<std::string> codes;
  int languages = 0;
  for (const int id : LauncherLanguages()) {
    if (id == 0) continue;  // "Automatic (console)": ConsoleUiLanguage picks one of the others
    ++languages;
    assert(xbox360ps5::SupportedUiLanguage(id) == id);
    const std::string code = xbox360ps5::UiLanguageCode(id);
    assert(codes.insert(code).second);
    if (!fs::is_regular_file(folder / (code + ".txt"))) {
      std::fprintf(stderr, "FAIL: language %d (%s) has no file in %s\n", id, code.c_str(), folder.c_str());
      std::exit(1);
    }
  }
  assert(languages == 16);
  std::set<std::string> files;
  for (const auto& entry : fs::directory_iterator(folder))
    if (entry.path().extension() == ".txt") files.insert(entry.path().stem().string());
  assert(files == codes);
  size_t total = 0;
  for (int id = 1; id <= 19; ++id) {
    if (xbox360ps5::SupportedUiLanguage(id) != id) continue;
    total += CheckFile(folder / (std::string(xbox360ps5::UiLanguageCode(id)) + ".txt"), id);
  }
  // Escapes: "\s" for every space, at the ends too.
  xbox360ps5::LoadUiTranslations(11);
  assert(Is(xbox360ps5::LoadedUiText("     Size  "), "     Rozmiar  "));
  assert(Is(xbox360ps5::LoadedUiText("Settings"), "Ustawienia"));
  assert(Is(xbox360ps5::LoadedUiText("\\s\\s\\s\\s\\sSize\\s\\s"), nullptr));
  std::printf("  kLanguages: 16 languages, each with its file; %zu files, %zu lines, all parsed and kept\n",
              files.size(), total);
}

void Overlay(const fs::path& root) {
  const fs::path base = root / "assets-lang", overlay = root / "lang", missing = root / "missing";
  fs::create_directories(base);
  fs::create_directories(overlay);
  Write(base / "pl.txt", "# package\nText\\sA = Polski A\nText\\sB = Polski B\nSize\\s = Rozmiar\\s\n");
  Write(base / "en.txt", "Text\\sE = English E\n");
  Write(base / "pt-BR.txt", "Text\\sA = Brasil A\nText\\sB = Brasil B\n");
  const Map package = {{"Text A", "Polski A"}, {"Text B", "Polski B"}, {"Size ", "Rozmiar "}};
  ui::Folder() = base.string() + "/";

  // No overlay folder: the package file alone (as before fix 43), with the default folder too.
  ui::OverlayFolder() = missing.string() + "/";
  xbox360ps5::LoadUiTranslations(11);
  assert(Loaded() == package);
  assert(Is(xbox360ps5::LoadedUiText("Text C"), nullptr));
  if (!fs::exists("/app0/lang")) {
    ui::OverlayFolder() = "/app0/lang/";
    xbox360ps5::LoadUiTranslations(11);
    assert(Loaded() == package);
  }
  // The folder without this language's file.
  Write(overlay / "de.txt", "Text\\sA = Deutsch A\n");
  ui::OverlayFolder() = overlay.string() + "/";
  xbox360ps5::LoadUiTranslations(11);
  assert(Loaded() == package);
  std::printf("  no /app0/lang/ (or no file there): the package file's table, unchanged\n");

  // The language's file there: read second into the same table, adding and replacing (CRLF lines,
  // comments and a malformed line as in the package's files).
  Write(overlay / "pl.txt", "# overlay\r\nText\\sB = Polski B (lang)\r\nText\\sC = Polski C\r\nmalformed\r\n\r\n");
  ui::OverlayFolder() = missing.string() + "/";
  xbox360ps5::LoadUiTranslations(11);
  ui::OverlayFolder() = overlay.string() + "/";
  xbox360ps5::LoadUiTranslations(11);  // the overlay folder changed: read again
  const Map both = {{"Text A", "Polski A"}, {"Text B", "Polski B (lang)"}, {"Text C", "Polski C"}, {"Size ", "Rozmiar "}};
  assert(Loaded() == both);
  assert(Is(xbox360ps5::LoadedUiText("Text B"), "Polski B (lang)"));
  std::printf("  /app0/lang/pl.txt after the package's: a key added, a text replaced\n");

  // Kept while the language and folders stay (the official returns early for the same language).
  const char* kept = xbox360ps5::LoadedUiText("Text B");
  Write(overlay / "pl.txt", "Text\\sB = Polski B (changed)\n");
  xbox360ps5::LoadUiTranslations(11);
  assert(Loaded() == both);
  xbox360ps5::LoadUiTranslations(3);
  assert((Loaded() == Map{{"Text A", "Deutsch A"}}));  // no package file, the overlay's alone
  xbox360ps5::LoadUiTranslations(11);
  assert(Is(xbox360ps5::LoadedUiText("Text B"), "Polski B (changed)"));
  assert(Is(kept, "Polski B (lang)"));  // an earlier table's text stays valid
  std::printf("  the same language again: kept; another language and back: both files read again\n");

  // Brazilian Portuguese (9) reads pt-BR.txt in both folders.
  Write(overlay / "pt-BR.txt", "Text\\sB = Brasil B (lang)\n");
  xbox360ps5::LoadUiTranslations(9);
  assert((Loaded() == Map{{"Text A", "Brasil A"}, {"Text B", "Brasil B (lang)"}}));

  // The fallback files (fix 23) are read the same way: en.txt, then the overlay's en.txt.
  Write(overlay / "en.txt", "Text\\sE = English E (lang)\nText\\sF = English F\n");
  xbox360ps5::ui_language.store(11);
  xbox360ps5::LoadUiTranslations(11);
  assert(Is(xbox360ps5::Tr("Text E"), "English E (lang)"));
  assert(Is(xbox360ps5::Tr("Text F"), "English F"));
  assert(Is(xbox360ps5::Tr("Text A"), "Polski A"));
  assert(Is(xbox360ps5::Tr("Text B"), "Polski B (changed)"));
  assert(Is(xbox360ps5::Tr("Text C"), "Text C"));  // no longer in the overlay's pl.txt, in no en.txt
  std::printf("  pt-BR.txt and the English fallback: the overlay's file over the package's\n");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <tools/source-1.0.2/english/lang>\n", argv[0]);
    return 2;
  }
  const fs::path root = TempRoot();
  Defaults();
  OfficialTables();
  GeneratedFiles(argv[1], root / "no-overlay");
  Overlay(root);
  fs::remove_all(root);
  std::puts("PASS: interface languages as the official loader, every language file parsed, /app0/lang/ read"
            " over the package's file (fix 43)");
}
