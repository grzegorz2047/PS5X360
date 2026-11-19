// SPDX-License-Identifier: MIT
// Fix 41 (tools/source-1.0.2/fixes/41-cover-box-art-fallback.patch): the home's cover download
// (covers.cpp) falls back to Microsoft's box art when XboxUnity has no cover, as the official
// 1.0.2 eboot does (cover routine 0xd1000-0xd2c00, shared HTTP GET 0xd3220).
// Drives xbox360ps5::cover_fallback (port/cover_fallback.hpp) with a fake HTTP GET: the URL (title
// ID in lower case), the picture check (at least 4 bytes, FF D8 or 89 'P'), which server is asked
// and in what order, the source the log names, what counts as fetched, without a cover or failed,
// when a server is no longer asked and when the run stops, and the fixed address only for
// xboxunity.net. Then the assembled covers.cpp and game_art.cpp use it, and every English-keyed
// language file (the folder given as the argument) has the new summary text with its leading space.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/cover_fallback.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace cf = xbox360ps5::cover_fallback;

// The texts as an English interface gives them (Tr of the English keys).
const cf::Texts kTexts{"invalid image", "server returned ", "no connection to ", "socket unavailable"};

const std::vector<uint8_t> kJpeg{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F'};
const std::vector<uint8_t> kPng{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
const std::vector<uint8_t> kHtml{'<', 'h', 't', 'm', 'l', '>'};

std::vector<uint8_t> Bytes(const std::string& text) { return {text.begin(), text.end()}; }

// The answer of a fake server for one URL.
struct Answer {
  bool ok = false;
  std::vector<uint8_t> body;
  std::string error;
};
Answer Ok(std::vector<uint8_t> body) { return {true, std::move(body), ""}; }
Answer Fail(std::string error) { return {false, {}, std::move(error)}; }

// HTTP GETs answered from a table by "http://<host><path>"; an URL not in it is a 404. Each
// request is recorded.
struct FakeHttp {
  std::map<std::string, Answer> answers;
  std::vector<std::string> asked;
  bool operator()(const std::string& host, const std::string& path, std::vector<uint8_t>& body,
                  std::string& error) {
    const std::string url = "http://" + host + path;
    asked.push_back(url);
    const auto found = answers.find(url);
    const Answer answer = found != answers.end() ? found->second : Fail("server returned 404");
    if (!answer.ok) {
      error = answer.error;
      return false;
    }
    body = answer.body;
    return true;
  }
};

// CoverInfo.php's answer: the cover ID after "CoverID" (covers.cpp's ChooseCover, simplified).
std::string Choose(const std::string& json) {
  const auto at = json.find("\"CoverID\":\"");
  if (at == std::string::npos) return "";
  const auto start = at + 11;
  return json.substr(start, json.find('"', start) - start);
}

std::string InfoUrl(const std::string& id) {
  return "http://xboxunity.net/Resources/Lib/CoverInfo.php?titleid=" + id;
}
std::string CoverUrl(const std::string& cid) {
  return "http://xboxunity.net/Resources/Lib/Cover.php?size=large&cid=" + cid;
}
std::string BoxArtUrl(const std::string& lower_id) {
  return "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802" + lower_id +
         "/1033/boxartlg.jpg";
}
std::vector<uint8_t> Info(const std::string& cid) {
  return Bytes("[{\"CoverID\":\"" + cid + "\",\"Official\":\"1\",\"Rating\":\"5\"}]");
}

cf::Result Fetch(FakeHttp& http, const std::string& id, cf::RunState& state) {
  return cf::Fetch(id, state, kTexts, http, Choose);
}

void Helpers() {
  // The image server's URL: the title ID in lower case, English box art.
  assert(cf::LowerTitleId("4D5307E6") == "4d5307e6");
  assert(cf::LowerTitleId("415607F7") == "415607f7");
  assert(std::string("http://") + cf::kMarketplaceHost + cf::BoxArtPath("4D5307E6") ==
         "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d8024d5307e6/1033/boxartlg.jpg");
  // game_art.cpp's background and tile, as it built them before.
  assert(cf::MarketplaceImagePath("5841128F", "background.jpg") ==
         "/content/images/66acd000-77fe-1000-9115-d8025841128f/1033/background.jpg");
  assert(cf::MarketplaceImagePath("5841128F", "tile.png") ==
         "/content/images/66acd000-77fe-1000-9115-d8025841128f/1033/tile.png");
  // The picture check: 4 bytes or more, JPEG or PNG.
  assert(cf::IsCoverImage(kJpeg) && cf::IsCoverImage(kPng));
  assert(cf::IsCoverImage({0xFF, 0xD8, 0xFF, 0xDB}));
  assert(!cf::IsCoverImage({0xFF, 0xD8, 0xFF}));  // 3 bytes
  assert(!cf::IsCoverImage({0x89, 'P'}));
  assert(!cf::IsCoverImage({}));
  assert(!cf::IsCoverImage(kHtml));
  assert(!cf::IsCoverImage({0xFF, 0x00, 0x00, 0x00}));
  assert(!cf::IsCoverImage({0x89, 'p', 'n', 'g'}));
  // The log line's source.
  assert(cf::XboxUnitySource("8211") == "XboxUnity cover 8211");
  assert(std::string(cf::kMarketplaceSource) == "Xbox marketplace box art");
  // The fixed address only for xboxunity.net (1.0.2's strcmp at 0xd3b1d).
  assert(cf::FixedAddress("xboxunity.net") && std::string(cf::FixedAddress("xboxunity.net")) == "149.56.47.64");
  assert(cf::FixedAddress(cf::kXboxUnityHost) == cf::kXboxUnityAddress);
  assert(!cf::FixedAddress("download.xbox.com"));
  assert(!cf::FixedAddress(cf::kMarketplaceHost));
  assert(!cf::FixedAddress("www.xboxunity.net"));
  assert(!cf::FixedAddress(""));
  // A failure the next title would hit as well.
  assert(cf::ConnectionLost("no connection to xboxunity.net (is the PS5 online?)", kTexts));
  assert(cf::ConnectionLost("no connection to download.xbox.com (is the PS5 online?)", kTexts));
  assert(cf::ConnectionLost("socket unavailable", kTexts));
  assert(!cf::ConnectionLost("server returned 404", kTexts));
  assert(!cf::ConnectionLost("incomplete response", kTexts));
  assert(!cf::ConnectionLost("", kTexts));
  assert(!cf::ConnectionLost("", cf::Texts{}));  // empty texts match nothing
  std::printf("  URL in lower case, picture check, log sources, fixed address for xboxunity.net only\n");
}

void XboxUnityFirst() {
  FakeHttp http;
  http.answers[InfoUrl("4D5307E6")] = Ok(Info("8211"));
  http.answers[CoverUrl("8211")] = Ok(kJpeg);
  http.answers[BoxArtUrl("4d5307e6")] = Ok(kPng);
  cf::RunState state;
  const auto result = Fetch(http, "4D5307E6", state);
  assert(result.source == "XboxUnity cover 8211");
  assert(result.image == kJpeg && result.error.empty());
  assert(cf::Classify(result, kTexts) == cf::Outcome::kFetched);
  // Microsoft is not asked when XboxUnity had the cover.
  assert((http.asked == std::vector<std::string>{InfoUrl("4D5307E6"), CoverUrl("8211")}));
  assert(!state.xboxunity_down && !state.marketplace_down && !cf::Stop(state));
  std::printf("  XboxUnity's cover: kept, Microsoft not asked\n");
}

void MarketplaceWhenNoCover() {
  // XboxUnity knows the title but has no cover: the box art.
  FakeHttp http;
  http.answers[InfoUrl("415607F7")] = Ok(Bytes("[]"));
  http.answers[BoxArtUrl("415607f7")] = Ok(kJpeg);
  cf::RunState state;
  auto result = Fetch(http, "415607F7", state);
  assert(result.source == "Xbox marketplace box art" && result.image == kJpeg && result.error.empty());
  assert(cf::Classify(result, kTexts) == cf::Outcome::kFetched);
  assert((http.asked == std::vector<std::string>{InfoUrl("415607F7"), BoxArtUrl("415607f7")}));
  // XboxUnity's cover is no picture (an HTML page): the box art.
  http = {};
  http.answers[InfoUrl("4D5307E6")] = Ok(Info("77"));
  http.answers[CoverUrl("77")] = Ok(kHtml);
  http.answers[BoxArtUrl("4d5307e6")] = Ok(kPng);
  result = Fetch(http, "4D5307E6", state);
  assert(result.source == "Xbox marketplace box art" && result.image == kPng && result.error.empty());
  assert(http.asked.size() == 3 && http.asked[2] == BoxArtUrl("4d5307e6"));
  // XboxUnity's cover download fails: the box art.
  http = {};
  http.answers[InfoUrl("4D5307E6")] = Ok(Info("77"));
  http.answers[CoverUrl("77")] = Fail("incomplete response");
  http.answers[BoxArtUrl("4d5307e6")] = Ok(kJpeg);
  result = Fetch(http, "4D5307E6", state);
  assert(result.source == "Xbox marketplace box art" && result.image == kJpeg);
  // XboxUnity answers an HTTP error for the title: the box art.
  http = {};
  http.answers[InfoUrl("4D5307E6")] = Fail("server returned 500");
  http.answers[BoxArtUrl("4d5307e6")] = Ok(kJpeg);
  result = Fetch(http, "4D5307E6", state);
  assert(result.source == "Xbox marketplace box art");
  assert(!state.xboxunity_down && !state.marketplace_down);
  std::printf("  no cover, no picture, a failed download or an HTTP error on XboxUnity: Microsoft's box art\n");
}

void Misses() {
  cf::RunState state;
  // Neither has it; the marketplace's 404 is a miss, not a failure.
  FakeHttp http;
  http.answers[InfoUrl("12345678")] = Ok(Bytes("[]"));
  auto result = Fetch(http, "12345678", state);
  assert(result.source.empty() && result.image.empty() && result.error == "server returned 404");
  assert(cf::Classify(result, kTexts) == cf::Outcome::kMissing);
  // The marketplace answers 200 with no picture: a miss.
  http = {};
  http.answers[InfoUrl("12345678")] = Ok(Bytes("[]"));
  http.answers[BoxArtUrl("12345678")] = Ok(kHtml);
  result = Fetch(http, "12345678", state);
  assert(result.source.empty() && result.image.empty() && result.error.empty());
  assert(cf::Classify(result, kTexts) == cf::Outcome::kMissing);
  // XboxUnity's HTTP error is the reason given (it comes first), still a miss.
  http = {};
  http.answers[InfoUrl("12345678")] = Fail("server returned 503");
  result = Fetch(http, "12345678", state);
  assert(result.error == "server returned 503");
  assert(cf::Classify(result, kTexts) == cf::Outcome::kMissing);
  // XboxUnity's cover is no picture and the marketplace has none: a failure, "invalid image".
  http = {};
  http.answers[InfoUrl("12345678")] = Ok(Info("9"));
  http.answers[CoverUrl("9")] = Ok({0xFF, 0xD8});
  result = Fetch(http, "12345678", state);
  assert(result.source.empty() && result.image.empty() && result.error == "invalid image");
  assert(cf::Classify(result, kTexts) == cf::Outcome::kFailed);
  // A reason from the marketplace when XboxUnity gave none.
  http = {};
  http.answers[InfoUrl("12345678")] = Ok(Bytes("[]"));
  http.answers[BoxArtUrl("12345678")] = Fail("incomplete response");
  result = Fetch(http, "12345678", state);
  assert(result.error == "incomplete response");
  assert(cf::Classify(result, kTexts) == cf::Outcome::kFailed);
  assert(!state.xboxunity_down && !state.marketplace_down && !cf::Stop(state));
  // Translated texts: the comparison follows them (Polish interface).
  const cf::Texts polish{"nieprawidłowy obraz", "serwer odpowiedział ", "brak połączenia z ", "brak gniazda"};
  cf::Result translated;
  translated.error = "serwer odpowiedział 404";
  assert(cf::Classify(translated, polish) == cf::Outcome::kMissing);
  assert(cf::Classify(translated, kTexts) == cf::Outcome::kFailed);
  std::printf("  neither server has it or an HTTP error: without a cover; other reasons: failed\n");
}

void ServersDown() {
  const std::string unity_down = "no connection to xboxunity.net (is the PS5 online?)";
  const std::string xbox_down = "no connection to download.xbox.com (is the PS5 online?)";
  // XboxUnity cannot be reached: Microsoft still asked, XboxUnity not asked again.
  FakeHttp http;
  http.answers[InfoUrl("4D5307E6")] = Fail(unity_down);
  http.answers[InfoUrl("415607F7")] = Fail(unity_down);
  http.answers[BoxArtUrl("4d5307e6")] = Ok(kJpeg);
  http.answers[BoxArtUrl("415607f7")] = Ok(kPng);
  cf::RunState state;
  auto result = Fetch(http, "4D5307E6", state);
  assert(result.source == "Xbox marketplace box art" && state.xboxunity_down && !cf::Stop(state));
  result = Fetch(http, "415607F7", state);
  assert(result.source == "Xbox marketplace box art" && result.image == kPng);
  assert((http.asked == std::vector<std::string>{InfoUrl("4D5307E6"), BoxArtUrl("4d5307e6"),
                                                 BoxArtUrl("415607f7")}));
  // download.xbox.com cannot be reached: XboxUnity still asked, Microsoft not asked again.
  http = {};
  http.answers[InfoUrl("12345678")] = Ok(Bytes("[]"));
  http.answers[BoxArtUrl("12345678")] = Fail(xbox_down);
  http.answers[InfoUrl("4D5307E6")] = Ok(Info("8211"));
  http.answers[CoverUrl("8211")] = Ok(kJpeg);
  http.answers[InfoUrl("87654321")] = Ok(Bytes("[]"));
  state = {};
  result = Fetch(http, "12345678", state);
  assert(state.marketplace_down && !state.xboxunity_down && !cf::Stop(state));
  assert(result.error == xbox_down && cf::Classify(result, kTexts) == cf::Outcome::kFailed);
  result = Fetch(http, "4D5307E6", state);
  assert(result.source == "XboxUnity cover 8211");
  result = Fetch(http, "87654321", state);
  assert(result.source.empty() && result.error.empty());
  assert(cf::Classify(result, kTexts) == cf::Outcome::kMissing);
  assert(http.asked.size() == 5);  // no second box art request
  // A socket failure counts as down too.
  http = {};
  http.answers[InfoUrl("12345678")] = Fail("socket unavailable");
  http.answers[BoxArtUrl("12345678")] = Fail("socket unavailable");
  state = {};
  result = Fetch(http, "12345678", state);
  assert(state.xboxunity_down && state.marketplace_down && cf::Stop(state));
  assert(result.error == "socket unavailable" && cf::Classify(result, kTexts) == cf::Outcome::kFailed);
  std::printf("  one server down: only it is skipped for the rest of the run\n");
}

// covers.cpp's loop: no connection at all stops after the first title (1.0.2 stopped there too
// with XboxUnity alone; the official asks both servers once).
void RunStopsOffline() {
  FakeHttp http;
  const std::vector<std::string> ids{"4D5307E6", "415607F7", "12345678"};
  for (const auto& id : ids) {
    http.answers[InfoUrl(id)] = Fail("no connection to xboxunity.net (is the PS5 online?)");
    http.answers[BoxArtUrl(cf::LowerTitleId(id))] = Fail("no connection to download.xbox.com (is the PS5 online?)");
  }
  cf::RunState state;
  int fetched = 0, missing = 0, failed = 0, titles = 0;
  std::string problem;
  for (const auto& id : ids) {
    ++titles;
    const auto result = Fetch(http, id, state);
    switch (cf::Classify(result, kTexts)) {
      case cf::Outcome::kFetched: ++fetched; break;
      case cf::Outcome::kMissing: ++missing; break;
      case cf::Outcome::kFailed: ++failed; problem = result.error; break;
    }
    if (cf::Stop(state)) break;
  }
  assert(titles == 1 && fetched == 0 && missing == 0 && failed == 1);
  assert(problem == "no connection to xboxunity.net (is the PS5 online?)");
  assert(http.asked.size() == 2);
  std::printf("  offline: one title, both servers asked once, the run stops with XboxUnity's reason\n");
}

std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TreeUsesIt() {
  const std::string covers = ReadTree("src/covers.cpp");
  assert(Has(covers, "#include \"cover_fallback.hpp\""));
  // One HTTP GET with a host; the fixed address only through FixedAddress.
  assert(Has(covers, "bool HttpGet(const std::string& host, const std::string& path, std::vector<uint8_t>& body, std::string& error)"));
  assert(Has(covers, "Candidates(host)"));
  assert(Has(covers, " HTTP/1.1\\r\\nHost: \" + host +"));
  assert(Has(covers, "if (const char* fixed = cover_fallback::FixedAddress(host)) add(inet_addr(fixed));"));
  assert(!Has(covers, "149.56.47.64"));
  assert(!Has(covers, "kHost"));
  // A status line shorter than "HTTP/1.1 200" no longer throws from substr.
  const auto returned = covers.find("error = Tr(\"server returned \") + head.substr(9, 3);");
  const auto guard = covers.find("if (head.size() < 12) { error = Tr(\"incomplete response\"); return false; }");
  assert(returned != std::string::npos && guard != std::string::npos && guard < returned);
  // The loop: XboxUnity, then Microsoft, the outcome, the stop.
  const auto run = covers.find("void CoverDownloader::Run(");
  assert(run != std::string::npos);
  const std::string body = covers.substr(run);
  assert(Has(body, "cover_fallback::Fetch(id, state, texts, get, ChooseCover)"));
  assert(Has(body, "cover_fallback::Classify(result, texts)"));
  assert(Has(body, "if (cover_fallback::Stop(state)) break;"));
  assert(Has(body, "Tr(\"invalid image\"), Tr(\"server returned \"), Tr(\"no connection to \")"));
  assert(Has(body, "XELOGI(\"Cover {} downloaded ({} bytes, {})\", id, result.image.size(), result.source);"));
  assert(Has(body, "Tr(\" without a cover on XboxUnity or Microsoft\")"));
  assert(!Has(body, "XboxUnity cover {}"));
  assert(!Has(body, "Tr(\" without a cover on XboxUnity\")"));
  // game_art.cpp builds its paths and checks its pictures with the same header.
  const std::string art = ReadTree("port/game_art.cpp");
  assert(Has(art, "#include \"cover_fallback.hpp\""));
  assert(Has(art, "Get(xbox360ps5::cover_fallback::MarketplaceImagePath(id, name))"));
  assert(Has(art, "data.size() > 64 && xbox360ps5::cover_fallback::IsCoverImage(data)"));
  assert(!Has(art, "66acd000"));
  // port/ is on the title's include path.
  assert(Has(ReadTree("canary/CMakeLists.txt"), "\"${ROOT}/port\""));
  std::printf("  covers.cpp and game_art.cpp use it; 149.56.47.64 only through FixedAddress\n");
}

// "\s" at the edges of a language file's text is a space.
std::string Unescape(const std::string& text) {
  std::string out;
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '\\' && at + 1 < text.size()) {
      const char next = text[++at];
      out += next == 's' ? ' ' : next == 'n' ? '\n' : next;
    } else {
      out += text[at];
    }
  }
  return out;
}

void LanguageFiles(const fs::path& folder) {
  const std::string key = " without a cover on XboxUnity or Microsoft";
  std::map<std::string, std::string> texts;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    for (std::string line; std::getline(file, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto split = line.find(" = ");
      if (split == std::string::npos || Unescape(line.substr(0, split)) != key) continue;
      assert(line.rfind("\\s", 0) == 0);  // the leading space escaped, or the file's parser drops it
      texts[entry.path().stem().string()] = Unescape(line.substr(split + 3));
    }
  }
  for (const char* code : {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it", "ja", "ko", "nl", "pl",
                           "ru", "tr", "zh-Hans", "zh-Hant"}) {
    const auto found = texts.find(code);
    assert(found != texts.end() && "the summary text is in every language file");
    assert(found->second.size() > 1 && found->second[0] == ' ');  // joined after the count
  }
  assert(texts["en"] == key);
  assert(texts["pt-BR"] == " sem capa no XboxUnity nem na Microsoft");  // the official source text
  assert(texts["pl"] == " bez okładki w XboxUnity ani na serwerze Microsoftu");
  std::printf("  the summary text in all %zu language files, with its leading space\n", texts.size());
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2 && "usage: check-cover-fallback <english/lang folder>");
  Helpers();
  XboxUnityFirst();
  MarketplaceWhenNoCover();
  Misses();
  ServersDown();
  RunStopsOffline();
  TreeUsesIt();
  LanguageFiles(argv[1]);
  std::puts("PASS: covers fall back to Microsoft's box art after XboxUnity (fix 41)");
}
