// SPDX-License-Identifier: MIT
// Fix 20 (tools/source-1.0.2/fixes/20-stale-dispatch-header.patch): drives
// xbox360ps5::native_object::Resolve, the decisions of XObject::GetNativeObject, with a fake
// kernel: an object table that counts references, guest dispatch headers, and the kinds of
// object the kernel can make. Built against the assembled 1.0.2 tree (core include root
// .deps/xenia-canary/src).
#undef NDEBUG
#include "xenia/kernel/native_object_resolve.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

namespace {
using xbox360ps5::native_object::Outcome;
using xbox360ps5::native_object::Report;
using xbox360ps5::native_object::Resolve;

constexpr uint32_t kSignature = 0x58454E00;  // 'XEN\0', kXObjSignature
// X_OBJECT_TYPES values the kernel side switches on.
constexpr uint8_t kEventNotification = 0, kEventSynchronization = 1, kMutant = 2, kSemaphore = 5,
                  kThread = 6, kTimerNotification = 8;

struct Header {  // the fields of X_DISPATCH_HEADER GetNativeObject reads and writes
  uint8_t type = 0;
  uint32_t signal_state = 0;
  uint32_t flink = 0, blink = 0;
};

struct Object {
  enum Kind { kEvent, kMutantObject, kSemaphoreObject, kThreadObject, kExitedThreadStandIn, kOther };
  Kind kind = kOther;
  uint32_t handle = 0;
  uint32_t guest = 0;   // guest address of its dispatch header
  int references = 0;
  int syncs = 0;
};

// The object table: handles to objects, a reference per handle plus the lookups'.
struct Kernel {
  std::map<uint32_t, Object*> table;
  std::vector<std::unique_ptr<Object>> objects;
  std::vector<uint32_t> running_threads;  // guest addresses of running threads' KTHREADs
  uint32_t next_handle = 0xF8000100;
  int lookups = 0, releases = 0, unsupported = 0;

  Object* Add(Object::Kind kind, uint32_t guest) {
    objects.push_back(std::make_unique<Object>());
    Object* object = objects.back().get();
    object->kind = kind;
    object->guest = guest;
    object->handle = next_handle;
    next_handle += 4;
    object->references = 1;  // the handle's
    table[object->handle] = object;
    return object;
  }
  // The object leaves the table (its last handle closed); the handle may be given again.
  void Close(Object* object) {
    table.erase(object->handle);
    --object->references;
  }
  // The table gives a closed object's handle to a new object.
  Object* AddWithHandle(Object::Kind kind, uint32_t guest, uint32_t handle) {
    const uint32_t saved = next_handle;
    next_handle = handle;
    Object* object = Add(kind, guest);
    next_handle = saved;
    return object;
  }
};

// GetNativeObject's kernel side, over the fake kernel and one header.
struct Backend {
  using Object = ::Object;
  Kernel& kernel;
  Header& header;
  uint32_t guest;
  Object* stale_owner = nullptr;

  bool Stamped() const { return header.flink == kSignature; }
  uint32_t RecordedHandle() const { return header.blink; }
  Object* Lookup(uint32_t handle) {
    auto found = kernel.table.find(handle);
    if (found == kernel.table.end()) return nullptr;
    ++kernel.lookups;
    ++found->second->references;
    return found->second;
  }
  bool BelongsToHeader(Object* object) const { return object->guest == guest; }
  void NoteStaleOwner(Object* object) { stale_owner = object; }
  void Release(Object* object) {
    ++kernel.releases;
    --object->references;
  }
  void SyncFromGuest(Object* object) { ++object->syncs; }
  // As XObject's constructor: one reference for the creator (handed to the caller) and one
  // for the table's handle.
  Object* Made(Object::Kind kind) {
    Object* object = kernel.Add(kind, guest);
    ++object->references;
    return object;
  }
  Object* Create() {
    switch (header.type) {
      case kEventNotification:
      case kEventSynchronization: return Made(Object::kEvent);
      case kMutant: return Made(Object::kMutantObject);
      case kSemaphore: return Made(Object::kSemaphoreObject);
      default: return nullptr;
    }
  }
  bool IsThreadType() const { return header.type == kThread; }
  bool ThreadRunning() {
    for (uint32_t running : kernel.running_threads)
      if (running == guest) return true;
    return false;
  }
  Object* CreateExitedThread() {
    header.signal_state = 1;
    return Made(Object::kExitedThreadStandIn);
  }
  void Stamp(Object* object) {
    header.flink = kSignature;
    header.blink = object->handle;
  }
  void NoteUnsupported(bool) { ++kernel.unsupported; }
};

// GetNativeObject's caller holds the returned reference (object_ref) and gives it back.
Object* Get(Kernel& kernel, Header& header, uint32_t guest, Report& report, Object** stale_owner = nullptr) {
  Backend backend{kernel, header, guest};
  Object* object = Resolve(backend, report);
  if (stale_owner) *stale_owner = backend.stale_owner;
  return object;
}
void Put(Kernel& kernel, Object* object) {
  if (object) --object->references;
}

// A header stamped for an object that was closed and whose handle now names another object.
struct StaleSetup {
  Header header;
  Object* other = nullptr;
  uint32_t old_handle = 0;
};
StaleSetup MakeStale(Kernel& kernel, uint8_t type, uint32_t guest, Object::Kind kind) {
  StaleSetup setup;
  setup.header.type = type;
  Object* first = kernel.Add(kind, guest);
  setup.header.flink = kSignature;
  setup.header.blink = first->handle;
  setup.old_handle = first->handle;
  kernel.Close(first);
  setup.other = kernel.AddWithHandle(Object::kOther, 0x43808480, first->handle);
  return setup;
}

void StaleObjectIsMadeAgain(uint8_t type, Object::Kind kind, const char* name) {
  Kernel kernel;
  const uint32_t guest = 0x30083010;
  StaleSetup setup = MakeStale(kernel, type, guest, kind);
  const int other_references = setup.other->references;

  Report first;
  Object* stale_owner = nullptr;
  Object* made = Get(kernel, setup.header, guest, first, &stale_owner);
  assert(first.stale && first.stale_handle == setup.old_handle);
  assert(first.outcome == Outcome::kCreated && !first.running_thread);
  assert(made && made->kind == kind && made->guest == guest);
  assert(stale_owner == setup.other);
  // The other object is left as it was: the lookup's reference was given back, once.
  assert(setup.other->references == other_references && setup.other->syncs == 0);
  assert(kernel.lookups == 1 && kernel.releases == 1);
  // Re-stamped: the header now names the new object.
  assert(setup.header.flink == kSignature && setup.header.blink == made->handle);
  assert(made->handle != setup.old_handle);
  // The handle's reference and the caller's.
  assert(made->references == 2);
  Put(kernel, made);
  assert(made->references == 1);

  // A second lookup finds the same new object, not stale (no warning).
  Report second;
  Object* again = Get(kernel, setup.header, guest, second);
  assert(again == made && !second.stale && second.outcome == Outcome::kRecorded);
  assert(again->syncs == 1);
  assert(made->references == 2);
  Put(kernel, again);
  assert(made->references == 1 && setup.other->references == other_references);
  assert(kernel.lookups == 2 && kernel.releases == 1 && kernel.unsupported == 0);
  std::printf("  stale %s header: re-created, re-stamped, second lookup the same object\n", name);
}

void ExitedThreadGetsStandIn() {
  Kernel kernel;
  const uint32_t guest = 0x30083010;
  StaleSetup setup = MakeStale(kernel, kThread, guest, Object::kThreadObject);
  setup.header.signal_state = 0;
  Report report;
  Object* made = Get(kernel, setup.header, guest, report);
  assert(report.stale && !report.running_thread && report.outcome == Outcome::kExitedThread);
  assert(made && made->kind == Object::kExitedThreadStandIn && made->guest == guest);
  assert(setup.header.signal_state == 1);  // a terminated thread is signalled
  assert(setup.header.blink == made->handle);
  assert(kernel.lookups == 1 && kernel.releases == 1 && setup.other->references == 1);
  Put(kernel, made);
  Report again;
  assert(Get(kernel, setup.header, guest, again) == made && again.outcome == Outcome::kRecorded);
  Put(kernel, made);
  assert(made->references == 1 && kernel.unsupported == 0);
  std::printf("  stale header of an exited thread: signalled stand-in\n");
}

void RunningThreadIsLeftAlone() {
  Kernel kernel;
  const uint32_t guest = 0x30083010;
  StaleSetup setup = MakeStale(kernel, kThread, guest, Object::kThreadObject);
  kernel.running_threads.push_back(guest);  // the thread runs; only its handle was lost
  const Header before = setup.header;
  Report report;
  Object* made = Get(kernel, setup.header, guest, report);
  assert(!made && report.stale && report.running_thread && report.outcome == Outcome::kNone);
  assert(setup.header.flink == before.flink && setup.header.blink == before.blink);
  assert(setup.header.signal_state == before.signal_state);
  assert(kernel.lookups == 1 && kernel.releases == 1 && setup.other->references == 1);
  assert(kernel.objects.size() == 2 && kernel.unsupported == 1);
  std::printf("  stale header of a running thread: left alone\n");
}

void UnsupportedTypesGiveNull() {
  {
    Kernel kernel;
    StaleSetup setup = MakeStale(kernel, kTimerNotification, 0x30090000, Object::kOther);
    Report report;
    assert(!Get(kernel, setup.header, 0x30090000, report));
    assert(report.stale && report.outcome == Outcome::kNone && kernel.unsupported == 1);
    assert(kernel.releases == 1 && setup.other->references == 1);
  }
  {
    // Never stamped (first use) of a type with no object; and of a thread.
    for (uint8_t type : {kTimerNotification, kThread}) {
      Kernel kernel;
      Header header;
      header.type = type;
      Report report;
      assert(!Get(kernel, header, 0x300A0000, report));
      assert(!report.stale && report.outcome == Outcome::kNone && kernel.unsupported == 1);
      assert(header.flink == 0 && kernel.objects.empty());
    }
  }
  std::printf("  unsupported types: no object\n");
}

void UnchangedPaths() {
  {
    // First use of an event: created and stamped, as before the fix.
    Kernel kernel;
    Header header;
    header.type = kEventSynchronization;
    Report report;
    Object* made = Get(kernel, header, 0x300B0000, report);
    assert(made && made->kind == Object::kEvent && report.outcome == Outcome::kCreated && !report.stale);
    assert(header.blink == made->handle && kernel.lookups == 0);
    Put(kernel, made);
    assert(made->references == 1);
  }
  {
    // A valid handle: the recorded object, synchronised, one reference for the caller.
    Kernel kernel;
    Header header;
    header.type = kSemaphore;
    Object* object = kernel.Add(Object::kSemaphoreObject, 0x300C0000);
    header.flink = kSignature;
    header.blink = object->handle;
    Report report;
    assert(Get(kernel, header, 0x300C0000, report) == object);
    assert(report.outcome == Outcome::kRecorded && object->references == 2 && object->syncs == 1);
    Put(kernel, object);
    assert(kernel.objects.size() == 1 && kernel.releases == 0);
  }
  {
    // A recorded handle no longer in the table: no object, nothing made (as before).
    Kernel kernel;
    Header header;
    header.type = kEventNotification;
    header.flink = kSignature;
    header.blink = 0xF8000999;
    Report report;
    assert(!Get(kernel, header, 0x300D0000, report));
    assert(report.outcome == Outcome::kNone && !report.stale && kernel.objects.empty());
    assert(kernel.unsupported == 0);
  }
  std::printf("  first use, valid handle and closed handle: as before\n");
}
}  // namespace

int main() {
  StaleObjectIsMadeAgain(kEventNotification, Object::kEvent, "event (notification)");
  StaleObjectIsMadeAgain(kEventSynchronization, Object::kEvent, "event (synchronization)");
  StaleObjectIsMadeAgain(kMutant, Object::kMutantObject, "mutant");
  StaleObjectIsMadeAgain(kSemaphore, Object::kSemaphoreObject, "semaphore");
  ExitedThreadGetsStandIn();
  RunningThreadIsLeftAlone();
  UnsupportedTypesGiveNull();
  UnchangedPaths();
  std::puts("PASS: stale dispatch headers (fix 20)");
}
