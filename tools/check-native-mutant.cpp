// Fix 64: a mutant made for a guest KMUTANT is bound to it (XMutant::InitializeNative calls
// SetNativePointer, as XEvent and XSemaphore do).
//
//   check-native-mutant        run from an assembled tree's root
//
// The decisions of fix 20 (native_object_resolve.h, the real header) with a kernel model:
// a mutant whose guest object stays 0 is taken for another object's at its next use, so
// every Ke* call made a new one (Dragon Ball Z for Kinect on 1.0.4_candidate_21: about 30 a
// second, each with a host mutex, until pthread_mutex_lock failed with ENOMEM); bound, the
// first call makes it and every later call finds it. Then the tree: all three types Create()
// makes bind themselves.
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/kernel/native_object_resolve.h"

namespace no = xbox360ps5::native_object;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// The kernel as fix 20 sees it: an object table and one guest dispatch header.
struct Kernel {
  struct Object {
    uint32_t handle = 0, guest = 0;
    int references = 1;  // the table's
  };
  bool bind_on_create = false;  // what XMutant::InitializeNative does with the guest pointer
  uint32_t header_address = 0x40224B10;
  bool stamped = false;
  uint32_t recorded = 0;
  uint32_t next_handle = 0xF80000C8;
  std::map<uint32_t, std::unique_ptr<Object>> table;
  int created = 0;
};

struct Backend {
  using Object = Kernel::Object;
  Kernel& k;
  bool Stamped() const { return k.stamped; }
  uint32_t RecordedHandle() const { return k.recorded; }
  Object* Lookup(uint32_t handle) {
    auto it = k.table.find(handle);
    if (it == k.table.end()) return nullptr;
    ++it->second->references;
    return it->second.get();
  }
  bool BelongsToHeader(Object* object) const { return object->guest == k.header_address; }
  void NoteStaleOwner(Object*) {}
  void Release(Object* object) { --object->references; }
  void SyncFromGuest(Object*) {}
  Object* Create() {
    auto object = std::make_unique<Object>();
    object->handle = k.next_handle;
    k.next_handle += 4;
    // SetNativePointer: stamps the header and records the guest object.
    if (k.bind_on_create) {
      object->guest = k.header_address;
      k.stamped = true;
      k.recorded = object->handle;
    }
    Object* raw = object.get();
    ++raw->references;  // the caller's
    k.table[raw->handle] = std::move(object);
    ++k.created;
    return raw;
  }
  bool IsThreadType() const { return false; }
  Object* FindThread() { return nullptr; }
  Object* CreateExitedThread() { return nullptr; }
  void Stamp(Object* object) {  // GetNativeObject's Stamp: StashHandle
    k.stamped = true;
    k.recorded = object->handle;
  }
  void NoteUnsupported(bool) {}
};

// KeInitializeMutant once, then `calls` KeWaitForSingleObject / KeReleaseMutant on it.
Kernel Run(bool bind, int calls, int& stale, std::vector<uint32_t>& handles) {
  Kernel k;
  k.bind_on_create = bind;
  Backend backend{k};
  stale = 0;
  for (int n = 0; n <= calls; ++n) {
    no::Report report;
    Kernel::Object* object = no::Resolve(backend, report);
    if (report.stale) ++stale;
    handles.push_back(object ? object->handle : 0);
    if (object) backend.Release(object);  // the object_ref the kernel call held
  }
  return k;
}

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // 1.0.4_candidate_21: not bound. Every call after the first finds the recorded handle on an
  // object whose guest object is 0, calls the header stale and makes another mutant.
  std::vector<uint32_t> handles;
  int stale = 0;
  Kernel before = Run(false, 3600, stale, handles);  // two minutes at 30 a second
  CHECK(before.created == 3601);
  CHECK(stale == 3600);
  CHECK(handles[1] != handles[2]);
  int leaked = 0;
  for (const auto& entry : before.table) leaked += entry.second->references > 0;
  CHECK(leaked == 3601);  // each one kept by the table, none ever freed
  // Fix 64: bound. One mutant, found by every later call.
  handles.clear();
  Kernel after = Run(true, 3600, stale, handles);
  CHECK(after.created == 1);
  CHECK(stale == 0);
  bool same = true;
  for (uint32_t handle : handles) same = same && handle == handles[0];
  CHECK(same);
  CHECK(after.table.size() == 1 && after.table.begin()->second->references == 1);

  const std::string mutant = ReadText(".deps/xenia-canary/src/xenia/kernel/xmutant.cc");
  if (mutant.empty()) {
    std::printf("no assembled tree here; xmutant.cc not checked\n");
  } else {
    const size_t init = mutant.find("void XMutant::InitializeNative(");
    const size_t end = mutant.find("\n}", init);
    CHECK(init != std::string::npos && end != std::string::npos);
    const std::string body = mutant.substr(init, end - init);
    CHECK(body.find("SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);") != std::string::npos);
    // The other two types GetNativeObject's Create() makes already bind themselves.
    CHECK(ReadText(".deps/xenia-canary/src/xenia/kernel/xevent.cc").find("SetNativePointer(") != std::string::npos);
    CHECK(ReadText(".deps/xenia-canary/src/xenia/kernel/xsemaphore.cc").find("SetNativePointer(") != std::string::npos);
    std::printf("tree: XMutant, XEvent and XSemaphore bind to their guest objects\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-native-mutant: all passed\n");
  return 0;
}
