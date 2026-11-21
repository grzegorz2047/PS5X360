// SPDX-License-Identifier: MIT
// Fix 99b (issue 15, part 2): the Kinect ID (NUI Identity) calls Kinect Sports makes as the
// player is picked answer as a Kinect ID with nothing stored, instead of undefined externs (which
// returned without setting r3) and E_FAIL from the identity messages, after which the game waited
// on "Pick main player".
//
//   check-kinect-identity        run from an assembled tree's root
//
// The tree's answers: each export defined once, with its result; the FE/2A005-2A008 messages
// succeed before the identity service's skeleton frames (2B002, still E_FAIL); the calls logged.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The body of `name`'s _entry up to its DECLARE line.
std::string Body(const std::string& file, const std::string& name) {
  const size_t at = file.find("dword_result_t " + name + "_entry(");
  if (at == std::string::npos) return "";
  const size_t end = file.find("DECLARE_XAM_EXPORT1(" + name + ",", at);
  return end == std::string::npos ? "" : file.substr(at, end - at);
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

int main() {
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  const std::string app = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/apps/xam_app.cc");
  if (nui.empty() || app.empty()) {
    std::printf("no assembled tree here; nothing checked\n");
    return 0;
  }
  // Each call with the answer of a Kinect ID that works with nothing stored.
  const struct {
    const char* name;
    const char* result;
  } calls[] = {
      {"XamReadBiometricData", "return X_ERROR_NOT_FOUND;"},
      {"XamWriteBiometricData", "return X_ERROR_SUCCESS;"},
      {"XamUserNuiEnableBiometric", "return X_ERROR_SUCCESS;"},
      {"XamNuiCameraElevationSetAngle", "return X_ERROR_SUCCESS;"},
      {"XamCacheOpenFile", "return X_ERROR_FILE_NOT_FOUND;"},
      {"XamCacheCloseFile", "return X_ERROR_SUCCESS;"},
      {"XamCacheReset", "return X_ERROR_SUCCESS;"},
      {"XamXlfsInitializeUploadQueue", "return X_E_FAIL;"},
      {"XamXlfsUninitializeUploadQueue", "return X_ERROR_SUCCESS;"},
      {"XamXlfsMountUploadQueueInstance", "return X_E_FAIL;"},
      {"XamXlfsUnmountUploadQueueInstance", "return X_ERROR_SUCCESS;"},
  };
  for (const auto& call : calls) {
    const std::string body = Body(nui, call.name);
    if (body.empty()) std::printf("missing: %s\n", call.name);
    CHECK(!body.empty());
    CHECK(body.find(call.result) != std::string::npos);
    CHECK(body.find(std::string("KinectIdentityLog(\"") + call.name + "\"") != std::string::npos);
    CHECK(Count(nui, std::string("DECLARE_XAM_EXPORT1(") + call.name + ",") == 1);
  }
  // No biometric data: the size reads 0.
  CHECK(Body(nui, "XamReadBiometricData").find("if (size_ptr) *size_ptr = 0;") != std::string::npos);
  // The old answer of XamUserNuiEnableBiometric is gone.
  CHECK(Body(nui, "XamUserNuiEnableBiometric").find("return X_E_INVALIDARG;") == std::string::npos);
  CHECK(nui.find("void KinectIdentityLog(const char* call, uint32_t a, uint32_t b, uint32_t result) {") !=
        std::string::npos);
  // The identity messages succeed; the skeleton frames to the service (2B002) still fail.
  const size_t a005 = app.find("case 0x0002A005:");
  const size_t a008 = app.find("case 0x0002A008: {");
  const size_t b002 = app.find("case 0x0002B002: {");
  CHECK(a005 != std::string::npos && a005 < a008 && a008 < b002 && b002 != std::string::npos);
  const std::string messages = a008 != std::string::npos && b002 != std::string::npos && a008 < b002
                                   ? app.substr(a008, b002 - a008)
                                   : std::string();
  CHECK(messages.find("return X_E_SUCCESS;") != std::string::npos);
  CHECK(messages.find("(fix 99b)") != std::string::npos);
  CHECK(app.find("answered E_FAIL (said once)") != std::string::npos);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-identity: all passed\n");
  return 0;
}
