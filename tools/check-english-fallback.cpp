// SPDX-License-Identifier: MIT
// Fixes 23 and 25 (tools/source-1.0.2/fixes): the interface text lookup (xbox360ps5::Tr in
// i18n.hpp, with the reconstructed i18n_ui.hpp's language files) is keyed by English (fix 25).
// A text is looked up in the chosen language's file; what that file lacks comes, for Portugal's
// Portuguese, from pt-BR.txt and, for Latin America's Spanish, from es.txt, then in English
// (en.txt, else the key itself without its "||context"), never in Portuguese (fix 23). Brazilian
// Portuguese is a language file like the others. A text that is no key is shown as it is.
// Language files are written to a temporary folder (i18n_ui::Folder). With an argument (the
// folder of tools/source-1.0.2/english/lang), the generated files are checked too, in a second
// run of this program (the fallback files are read once per process). Built against the
// assembled 1.0.2 tree.
#undef NDEBUG
#include "xbox360ps5/i18n.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using xbox360ps5::Tr;

void Write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

bool Is(const char* got, const char* want) {
  if (std::strcmp(got, want) == 0) return true;
  std::fprintf(stderr, "  got \"%s\", want \"%s\"\n", got, want);
  return false;
}

void Use(int language) {
  xbox360ps5::ui_language.store(language);
  xbox360ps5::LoadUiTranslations(language);
}

// The files of tools/source-1.0.2/english/lang (rekey.py), as the package carries them.
int CheckGeneratedFiles(const char* folder) {
  xbox360ps5::i18n_ui::Folder() = std::string(folder) + "/";
  Use(1);
  assert(Is(Tr("Settings"), "Settings"));
  assert(Is(Tr("On||feminine"), "On"));
  assert(Is(Tr("Network"), "Network"));
  Use(9);
  assert(Is(Tr("Settings"), "Configurações"));
  assert(Is(Tr("On||feminine"), "Ligada"));
  assert(Is(Tr("On"), "Ligado"));
  assert(Is(Tr("Network"), "Rede"));
  assert(Is(Tr("Automatic (console)"), "Automático (console)"));
  Use(19);
  assert(Is(Tr("Settings"), "Definições"));  // pt.txt: Portugal's own word
  assert(Is(Tr("Network"), "Rede"));
  Use(3);
  assert(Is(Tr("Settings"), "Einstellungen"));
  assert(Is(Tr("Network"), "Netzwerk"));
  assert(Is(Tr("On||feminine"), "Ein"));
  // In no official file: CoreUiText's English, and its Spanish for both Spanish.
  assert(Is(Tr("The game asks where to keep its saved data."), "The game asks where to keep its saved data."));
  Use(4);
  assert(Is(Tr("On||feminine"), "Activée"));
  for (int spanish : {5, 18}) {
    Use(spanish);
    assert(Is(Tr("The game asks where to keep its saved data."), "El juego pregunta dónde guardar el progreso."));
  }
  std::printf("  generated files: English, Brazilian and Portugal's Portuguese, German, French, Spanish\n");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--files") == 0) return CheckGeneratedFiles(argv[2]);
  const fs::path folder = fs::temp_directory_path() / "check-english-fallback";
  fs::create_directories(folder);
  // English keys. Polish has A and a context key; English has A, B and E (a CRLF line);
  // Spain's Spanish A and C; Latin America's A; Brazil's A, C and the context key; Portugal's A.
  Write(folder / "pl.txt", "# test\nText\\sA = Polski A\nOn||feminine = Włączona\n");
  Write(folder / "en.txt", "Text\\sA = English A\nText\\sB = English B\r\nText\\sE = English E\n");
  Write(folder / "es.txt", "Text\\sA = Español A\nText\\sC = Español C\n");
  Write(folder / "es-419.txt", "Text\\sA = Latino A\n");
  Write(folder / "pt-BR.txt", "Text\\sA = Brasil A\nText\\sC = Brasil C\nOn||feminine = Ligada\n");
  Write(folder / "pt.txt", "Text\\sA = Portugal A\n");
  xbox360ps5::i18n_ui::Folder() = folder.string() + "/";

  // Polish: its file, then English (en.txt, else the key without its context).
  Use(11);
  assert(Is(Tr("Text A"), "Polski A"));
  assert(Is(Tr("Text B"), "English B"));
  assert(Is(Tr("Text D"), "Text D"));
  assert(Is(Tr("On||feminine"), "Włączona"));
  assert(Is(Tr("Off||feminine"), "Off"));
  std::printf("  Polish: selected -> en.txt -> the English key\n");

  // English: en.txt is the chosen file; a context key without an entry shows its English.
  Use(1);
  assert(Is(Tr("Text A"), "English A"));
  assert(Is(Tr("Text E"), "English E"));
  assert(Is(Tr("Text D"), "Text D"));
  assert(Is(Tr("On||feminine"), "On"));
  const char* off = Tr("Off||plural");
  assert(Is(off, "Off") && Tr("Off||plural") == off);  // one stored text per context key
  const char* game = "Sonic the Hedgehog";
  assert(Tr(game) == game);  // no key: shown as it is
  std::printf("  English: en.txt -> the key, \"||context\" removed\n");

  // Spanish: Spain's file, then English; Latin America's file, then Spain's, then English.
  Use(5);
  assert(Is(Tr("Text A"), "Español A"));
  assert(Is(Tr("Text C"), "Español C"));
  assert(Is(Tr("Text B"), "English B"));
  Use(18);
  assert(Is(Tr("Text A"), "Latino A"));
  assert(Is(Tr("Text C"), "Español C"));
  assert(Is(Tr("Text B"), "English B"));
  assert(Is(Tr("Text D"), "Text D"));
  std::printf("  Spanish: es.txt -> English; Latin America: es-419.txt -> es.txt -> English\n");

  // Portuguese: Brazil's is a file like the others; Portugal's file, then Brazil's, then English.
  Use(9);
  const char* brazilian = Tr("Text A");
  assert(Is(brazilian, "Brasil A"));
  assert(Is(Tr("Text B"), "English B"));
  assert(Is(Tr("On||feminine"), "Ligada"));
  Use(19);
  assert(Is(Tr("Text A"), "Portugal A"));
  assert(Is(Tr("Text C"), "Brasil C"));
  assert(Is(Tr("On||feminine"), "Ligada"));
  assert(Is(Tr("Text B"), "English B"));
  assert(Is(brazilian, "Brasil A"));  // a text from an earlier language stays valid
  std::printf("  Portuguese: pt-BR.txt -> English; Portugal: pt.txt -> pt-BR.txt -> English\n");

  // A language without its file: English throughout.
  Use(3);
  assert(Is(Tr("Text A"), "English A"));
  assert(Is(Tr("On||feminine"), "On"));
  std::printf("  German without a file: English\n");

  // The default interface language: the console's, English when it is not supported.
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(16)) == 11);  // Polish console
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(1)) == 1);
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(99)) == 1);
  assert(xbox360ps5::SupportedUiLanguage(13) == 1);  // an id with no interface language
  std::printf("  default language: the console's, else English\n");
  fs::remove_all(folder);

  if (argc == 2) {
    // The generated files, in a process of their own.
    const std::string command = "\"" + std::string(argv[0]) + "\" --files \"" + argv[1] + "\"";
    if (std::system(command.c_str()) != 0) {
      std::fprintf(stderr, "FAIL: the generated language files (%s)\n", argv[1]);
      return 1;
    }
  }
  std::puts("PASS: the interface is keyed by English; missing text falls back to English (fixes 23, 25)");
}
