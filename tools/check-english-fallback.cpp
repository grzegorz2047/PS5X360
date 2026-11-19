// SPDX-License-Identifier: MIT
// Fix 23 (tools/source-1.0.2/fixes/23-english-fallback.patch): the interface text lookup
// (xbox360ps5::Tr in i18n.hpp, with the reconstructed i18n_ui.hpp's language files) shows a
// text the chosen language's file lacks in English (assets/lang/en.txt, then the built-in
// English table) and the Portuguese source only when neither has it. The language files are
// written to a temporary folder (i18n_ui::Folder). Built against the assembled 1.0.2 tree.
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

// Texts of the built-in (0.5.9) table: Portuguese, English, Spanish.
constexpr const char* kBuiltInPt = "Renderizador";
constexpr const char* kBuiltInEn = "Renderer";
constexpr const char* kBuiltInEs = "Renderizador";
constexpr const char* kBuiltIn2Pt = "OpenGL: apenas filtro bilinear, sem suavizar degradês.";
constexpr const char* kBuiltIn2En = "OpenGL: bilinear filtering only, without dithering.";

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
}  // namespace

int main() {
  const fs::path folder = fs::temp_directory_path() / "check-english-fallback";
  fs::create_directories(folder);
  // Polish has text A; English has A, B and an override of a built-in text; Spanish has A.
  Write(folder / "pl.txt", "# test\nTexto\\sA = Polski A\n");
  Write(folder / "en.txt", "Texto\\sA = English A\nTexto\\sB = English B\r\n" +
                               std::string(kBuiltIn2Pt) + " = English from en.txt\n");
  Write(folder / "es.txt", "Texto\\sA = Español A\n");
  Write(folder / "pt.txt", "Texto\\sA = Português A\n");
  xbox360ps5::i18n_ui::Folder() = folder.string() + "/";

  // Polish (an interface language without a built-in table).
  Use(11);
  assert(Is(Tr("Texto A"), "Polski A"));              // the chosen language
  assert(Is(Tr("Texto B"), "English B"));             // missing there: en.txt
  assert(Is(Tr(kBuiltIn2Pt), "English from en.txt"));  // en.txt before the built-in English
  assert(Is(Tr(kBuiltInPt), kBuiltInEn));             // in neither file: the built-in English
  assert(Is(Tr("Texto C"), "Texto C"));               // nowhere: the Portuguese source
  std::printf("  Polish: selected -> en.txt -> built-in English -> Portuguese\n");

  // English: en.txt is the chosen file.
  Use(1);
  assert(Is(Tr("Texto B"), "English B"));
  assert(Is(Tr(kBuiltInPt), kBuiltInEn));
  assert(Is(Tr("Texto C"), "Texto C"));
  std::printf("  English: en.txt -> built-in English -> Portuguese\n");

  // Spanish (Spain and Latin America): the built-in Spanish before English.
  for (int spanish : {5, 18}) {
    Use(spanish);
    if (spanish == 5) assert(Is(Tr("Texto A"), "Español A"));
    assert(Is(Tr(kBuiltInPt), kBuiltInEs));
    assert(Is(Tr("Texto B"), "English B"));
    assert(Is(Tr("Texto C"), "Texto C"));
  }
  std::printf("  Spanish: selected -> built-in Spanish -> en.txt -> Portuguese\n");

  // Portuguese (Portugal): its file, then the Brazilian source; Brazilian Portuguese: the source.
  Use(19);
  assert(Is(Tr("Texto A"), "Português A"));
  assert(Is(Tr("Texto B"), "Texto B"));
  assert(Is(Tr(kBuiltInPt), kBuiltInPt));
  Use(9);
  assert(Is(Tr("Texto A"), "Texto A"));
  std::printf("  Portuguese: the source, never English\n");

  // A language without its file: English throughout.
  Use(3);
  assert(Is(Tr("Texto A"), "English A"));
  assert(Is(Tr(kBuiltInPt), kBuiltInEn));
  std::printf("  German without a file: English\n");

  // The default interface language: the console's, English when it is not supported.
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(16)) == 11);  // Polish console
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(1)) == 1);
  assert(xbox360ps5::SupportedUiLanguage(xbox360ps5::ConsoleUiLanguage(99)) == 1);
  assert(xbox360ps5::SupportedUiLanguage(13) == 1);  // an id with no interface language
  std::printf("  default language: the console's, else English\n");

  fs::remove_all(folder);
  std::puts("PASS: untranslated interface text falls back to English (fix 23)");
}
