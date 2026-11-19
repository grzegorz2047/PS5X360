// SPDX-License-Identifier: MIT
// Fix 26 (tools/source-1.0.2/fixes/26-gdfx-win1252-names.patch): XTLGetLocaleV1, XGetLocale()
// of a title (xam ordinal 0x5DB). Up to 1.0.2 it was not implemented: the undefined import
// logged "undefined extern call to ... XTLGetLocaleV1" and returned without setting r3, so the
// game used whatever r3 held as its XC_LOCALE_*. Handball 16 (Europe, 573607E4) then built its
// localized paths from a garbage index ("pD\xF1\xD0", "", "/Sentences/SENTENCE_297_1.wav")
// and did not react to START after the storage prompt.
//
// Checks xam/xam_locale_v1.h (the locale the export gives, and XamGetLocaleEx's choice, moved
// there unchanged) and, from the assembled tree, that the export is declared for that ordinal.
// Built and run from the tree's root by tools/source-1.0.2/run-fix-checks.sh.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "xenia/kernel/xam/xam_locale_v1.h"

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::cout << "FAIL " << __LINE__ << ": " #cond << std::endl; \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

int main() {
  using namespace xe::kernel::xam;
  // XOnlineCountry ids (xenia/xbox.h) and their XC_LOCALE_*.
  constexpr uint8_t kGermany = 24, kSpain = 31, kFrance = 34, kGreatBritain = 35,
                    kJapan = 53, kPoland = 82, kRussia = 90, kUnitedStates = 103;
  CHECK(XtlLocaleV1(kUnitedStates) == 36);  // the emulator's default country
  CHECK(XtlLocaleV1(kGreatBritain) == 35);
  CHECK(XtlLocaleV1(kGermany) == 13);
  CHECK(XtlLocaleV1(kFrance) == 12);
  CHECK(XtlLocaleV1(kSpain) == 31);
  CHECK(XtlLocaleV1(kPoland) == 26);
  CHECK(XtlLocaleV1(kJapan) == 20);
  // A later locale (Russia, 37) is past the first list: the region's (PAL, Great Britain).
  CHECK(LocaleFromCountry(kRussia) == 37);
  CHECK(GameRegionFromCountry(kRussia) == 0x02FE);
  CHECK(XtlLocaleV1(kRussia) == 35);
  // XamGetLocale's limits (111, 43) keep it.
  CHECK(ChooseLocale(kRussia, GameRegionFromCountry(kRussia), 111, 43) == 37);
  // Never past the first list, for any country byte: the value is safe as a table index.
  for (int country = 0; country < 256; ++country) {
    CHECK(XtlLocaleV1(uint8_t(country)) <= kXtlLocaleV1Max);
  }
  // Unknown country ids fall back by region: 0xFFFF (not 0x01xx/0x02xx) gives the US.
  CHECK(GameRegionFromCountry(200) == 0xFFFF);
  CHECK(XtlLocaleV1(200) == 36);
  // An Asian region falls back to Japan (0x0101) or Korea.
  CHECK(ChooseLocale(255, 0x0101, 111, 36) == 20);
  CHECK(ChooseLocale(255, 0x0102, 111, 36) == 21);

  // The export is declared, for the ordinal the import table names.
  const std::string table = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_table.inc");
  const std::string locale = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_locale.cc");
  CHECK(!table.empty() && !locale.empty());
  CHECK(table.find("0x000005DB, XTLGetLocaleV1,") != std::string::npos);
  CHECK(locale.find("dword_result_t XTLGetLocaleV1_entry()") != std::string::npos);
  CHECK(locale.find("DECLARE_XAM_EXPORT1(XTLGetLocaleV1, kLocale, kImplemented);") !=
        std::string::npos);
  CHECK(locale.find("return locale_id;") != std::string::npos);

  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: XTLGetLocaleV1 gives the console's locale (US 36 by default), never "
               "past XC_LOCALE_UNITED_STATES, and is declared for xam 0x5DB\n";
  return 0;
}
