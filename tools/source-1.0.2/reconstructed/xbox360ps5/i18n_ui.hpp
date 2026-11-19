// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): 1.0.x's i18n.hpp (the
// interface in 17 languages, read from /app0/assets/lang/<code>.txt) is not published. This
// file adds what 1.0.0's launcher.cpp uses to 0.5.9's i18n.hpp; assemble.py includes it there
// and makes Tr() look in the loaded file first. File format, as in the official package:
// "<Portuguese text> = <translation>" per line, "\s" for a space, "\n" a new line, "#" comments.
#pragma once
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>

namespace xbox360ps5 {
// Interface language ids (launcher.cpp's kLanguages): Xbox language ids, plus 18 Spanish
// (Latin America) and 19 Portuguese (Portugal).
inline const char* UiLanguageCode(int id) {
  switch (id) {
    case 2: return "ja";
    case 3: return "de";
    case 4: return "fr";
    case 5: return "es";
    case 6: return "it";
    case 7: return "ko";
    case 8: return "zh-Hant";
    case 9: return "pt-BR";
    case 11: return "pl";
    case 12: return "ru";
    case 14: return "tr";
    case 16: return "nl";
    case 17: return "zh-Hans";
    case 18: return "es-419";
    case 19: return "pt";
    default: return "en";
  }
}
inline bool UiLanguageIsCjk(int id) { return id == 2 || id == 7 || id == 8 || id == 17; }
// The PS5's system language (system service parameter 1) as an interface language.
inline int ConsoleUiLanguage(int ps5) {
  switch (ps5) {
    case 0: return 2;
    case 2: case 22: return 4;
    case 3: return 5;
    case 4: return 3;
    case 5: return 6;
    case 6: return 16;
    case 7: return 19;
    case 8: return 12;
    case 9: return 7;
    case 10: return 8;
    case 11: return 17;
    case 16: return 11;
    case 17: return 9;
    case 19: return 14;
    case 20: return 18;
    default: return 1;
  }
}
// The game language (Xbox ids 1..17) that goes with an interface language.
inline int GameLanguageForUi(int id) { return id == 18 ? 5 : id == 19 ? 9 : id; }

namespace i18n_ui {
struct Table {
  int language = -1;
  std::map<std::string, std::string> text;
};
inline std::shared_ptr<const Table>& Current() {
  static std::shared_ptr<const Table> table = std::make_shared<Table>();
  return table;
}
inline std::mutex& Lock() {
  static std::mutex lock;
  return lock;
}
inline std::string Unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\' && i + 1 < in.size()) {
      const char c = in[++i];
      out += c == 's' ? ' ' : c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      out += in[i];
    }
  }
  return out;
}
}  // namespace i18n_ui

// Loads /app0/assets/lang/<code>.txt for the language (Brazilian Portuguese is the source).
inline void LoadUiTranslations(int id) {
  auto table = std::make_shared<i18n_ui::Table>();
  table->language = id;
  if (id != 9) {
    std::ifstream input(std::string("/app0/assets/lang/") + UiLanguageCode(id) + ".txt", std::ios::binary);
    for (std::string line; std::getline(input, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const size_t split = line.find(" = ");
      if (split == std::string::npos) continue;
      table->text[i18n_ui::Unescape(line.substr(0, split))] = i18n_ui::Unescape(line.substr(split + 3));
    }
  }
  std::lock_guard<std::mutex> guard(i18n_ui::Lock());
  i18n_ui::Current() = std::move(table);
}
// The loaded translation of a Portuguese text, or nullptr. Tables stay alive for the
// process, so the returned text remains valid.
inline const char* LoadedUiText(const char* pt) {
  static std::mutex keep_lock;
  static std::vector<std::shared_ptr<const i18n_ui::Table>> keep;
  std::shared_ptr<const i18n_ui::Table> table;
  {
    std::lock_guard<std::mutex> guard(i18n_ui::Lock());
    table = i18n_ui::Current();
  }
  const auto found = table->text.find(pt);
  if (found == table->text.end()) return nullptr;
  {
    std::lock_guard<std::mutex> guard(keep_lock);
    if (keep.empty() || keep.back() != table) keep.push_back(table);
  }
  return found->second.c_str();
}
}  // namespace xbox360ps5
