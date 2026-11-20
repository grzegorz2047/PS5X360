// SPDX-License-Identifier: MIT
// Fix 61 (tools/source-1.0.2/fixes/61-fault-stack.patch): a fault on an exhausted thread stack is
// reported, and Ninja Gaiden II's stack switches and protection changes are written as they
// happen. 1.0.4_candidate_22: the game ended at the same moment on every run (right after the
// same two textures, as a scene loads), healthy until then, with no handler output. Every
// console thread has a 2 MiB stack, guest calls nest on it, and no handler had a stack to run on.
//  (a) the mechanism, on this host: a thread that overflows its stack with a SIGSEGV handler
//      installed ends with no handler output, unless the thread has an alternate signal stack
//      and the handler SA_ONSTACK (each case in a child process);
//  (b) include/xbox360ps5/hang_watch.hpp: the stack switch and protection lines, at most
//      kRareLines of each and then one line saying so, the host stack part, and the deepest host
//      stack sampled in every 1024th kernel call shown in the guest beat;
//  (c) the assembled tree: the console's handlers run on the alternate stack (crash report and
//      Xenia's fault transport), every emulator thread gets one as it starts, the report says
//      where the fault is on the thread's stack, and the two guest calls write their lines
//      (the stack switch before its jump).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/hang_watch.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
  return n;
}
namespace hw = xbox360ps5::hang_watch;

// --- (a) -----------------------------------------------------------------------------------------
int report_pipe = -1;
void Report(int, siginfo_t*, void*) {
  const char byte = 'H';
  (void)!write(report_pipe, &byte, 1);
  _exit(3);
}
__attribute__((noinline)) int Deep(int n) {
  volatile char block[4096];
  block[0] = char(n);
  block[4095] = char(n);
  return n > 1000000 ? 0 : Deep(n + 1) + block[0] + block[4095];
}
bool use_alternate = false;
void* Overflow(void*) {
  if (use_alternate) {
    stack_t stack{};
    stack.ss_size = 256 * 1024;
    stack.ss_sp = std::malloc(stack.ss_size);
    sigaltstack(&stack, nullptr);
  }
  return reinterpret_cast<void*>(intptr_t(Deep(0)));
}
// Runs an overflowing 256 KiB thread in a child; true when the handler ran.
bool HandlerRan(bool alternate) {
  int fds[2];
  if (pipe(fds)) return false;
  const pid_t child = fork();
  if (child == 0) {
    close(fds[0]);
    report_pipe = fds[1];
    use_alternate = alternate;
    struct sigaction action{};
    action.sa_sigaction = Report;
    action.sa_flags = SA_SIGINFO | (alternate ? SA_ONSTACK : 0);
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t thread;
    pthread_create(&thread, &attr, Overflow, nullptr);
    pthread_join(thread, nullptr);
    _exit(0);
  }
  close(fds[1]);
  char byte = 0;
  const bool ran = read(fds[0], &byte, 1) == 1 && byte == 'H';
  close(fds[0]);
  int status = 0;
  waitpid(child, &status, 0);
  std::printf("   overflow %s an alternate stack: handler %s, child %s %d\n", alternate ? "with" : "without",
              ran ? "ran" : "did not run", WIFSIGNALED(status) ? "ended by signal" : "exited",
              WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
  return ran;
}
void CheckMechanism() {
  Expect(!HandlerRan(false), "an overflow without an alternate stack ends the process with no handler output");
  Expect(HandlerRan(true), "with an alternate stack and SA_ONSTACK the handler reports it");
}

// --- (b) -----------------------------------------------------------------------------------------
std::vector<std::string> raw_lines;
void Sink(const char* text, size_t size) { raw_lines.emplace_back(text, size); }
size_t fake_used = 0;
bool FakeQuery(size_t* used, size_t* size) {
  *used = fake_used;
  *size = 2048 * 1024;
  return true;
}
void CheckLines() {
  Expect(hw::HostStackPart() == "host stack unknown", "no query yet");
  const std::string sw = hw::StackSwitchLine(0xF8000008, 0x70012340, 0x70020000, 0x70010000, 0x40001000, true,
                                             0x838191F0, "host stack 12 KiB of 2048 KiB");
  std::printf("   %s\n", sw.c_str());
  Expect(sw == "HANG stack switch F8000008: guest stack 70012340 in 70010000-70020000 (64 KiB), fiber 40001000, "
               "re-entered at 838191F0; host stack 12 KiB of 2048 KiB",
         "the stack switch line");
  Expect(hw::StackSwitchLine(1, 2, 3, 4, 0, false, 5, "x").find("no fiber, returns") != std::string::npos,
         "a switch without a fiber");
  const std::string protect = hw::ProtectLine(0xF8000008, 0x40000000, 0x1000, 0x1, "applied", "host stack 1 KiB of 2048 KiB");
  std::printf("   %s\n", protect.c_str());
  Expect(protect == "HANG protect F8000008: 40000000-40000FFF (4096 bytes) to 1, applied; host stack 1 KiB of 2048 KiB",
         "the protection line");

  hw::raw_sink.store(&Sink);
  std::atomic<uint32_t> lines{0};
  uint32_t taken = 0;
  for (uint32_t n = 0; n < hw::kRareLines + 50; ++n) taken += hw::TakeRareLine(lines, "protect");
  Expect(taken == hw::kRareLines, "at most kRareLines of a kind");
  Expect(raw_lines.size() == 1 && raw_lines[0] == "HANG protect: 400 lines written, no more this run",
         "then one line saying so");
  hw::raw_sink.store(nullptr);

  hw::host_stack_query.store(&FakeQuery);
  fake_used = 300 * 1024;
  Expect(hw::HostStackPart() == "host stack 300 KiB of 2048 KiB", "the host stack part");
  const uint64_t before = hw::guest_calls.load();
  for (int n = 0; n < 4096; ++n) {
    fake_used = n == 2048 ? 1900 * 1024 : 100 * 1024;
    hw::NoteGuestCall("KeSetCurrentStackPointers");
  }
  (void)before;
  const std::string guest = hw::GuestLine(1.0, 0, 1.0);
  std::printf("   %s\n", guest.c_str());
  Expect(guest.find("; deepest host stack 1900 KiB of 2048 KiB") != std::string::npos,
         "the guest beat says the deepest host stack sampled");
  hw::host_stack_query.store(nullptr);
}

// --- (c) -----------------------------------------------------------------------------------------
void CheckTree() {
  const std::string report = Read("platform/ps5/crash_report.cpp");
  Expect(report.find("action.sa_flags = SA_SIGINFO | SA_ONSTACK;  // fix 61") != std::string::npos &&
             report.find("ending.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;  // fix 61") != std::string::npos,
         "the crash report's handlers run on the alternate stack");
  Expect(report.find("if (sigaltstack(&stack, nullptr))") != std::string::npos &&
             report.find("pthread_key_create(&signal_stack_key, ReleaseSignalStack)") != std::string::npos &&
             report.find("hang_watch::host_stack_query.store(&HostStackUse") != std::string::npos,
         "a signal stack per thread, freed when it ends; the host stack query set");
  Expect(report.find("a STACK OVERFLOW (the fault is at the end of this thread's stack)") != std::string::npos &&
             report.find("[X360] CRASH thread stack: starts near 0x") != std::string::npos,
         "the report says where the fault is on the thread's stack");
  const std::string transport = Read("platform/ps5/exception_handler.cpp");
  Expect(transport.find("action.sa_flags = SA_SIGINFO | SA_ONSTACK;") != std::string::npos,
         "Xenia's fault transport runs on it too (it gets the faults first)");
  const std::string threads = Read(".deps/xenia-canary/src/xenia/base/threading_posix.cc");
  const size_t start = threads.find("void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {");
  const size_t use = threads.find("xbox360ps5::UseSignalStack();", start);
  const size_t routine = threads.find("start_routine();", start);
  Expect(start != std::string::npos && use != std::string::npos && use < routine,
         "every emulator thread gets one before it runs");
  const std::string threading = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc");
  const size_t pointers = threading.find("void KeSetCurrentStackPointers_entry(");
  const size_t line = threading.find("xbox360ps5::hang_watch::StackSwitchLine(", pointers);
  const size_t jump = threading.find("current_thread->Reenter(", pointers);
  Expect(pointers != std::string::npos && line != std::string::npos && line < jump,
         "a stack switch is written before the jump that does not come back");
  const std::string memory = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  const size_t protect = memory.find("void MmSetAddressProtect_entry(");
  Expect(protect != std::string::npos && Count(memory, "note(\"") == 4 &&
             memory.find("note(\"about to apply\");", protect) <
                 memory.find("heap->Protect(base_address.guest_address(), region_size, protect);", protect),
         "each protection change is written, before and after it is applied");
}

}  // namespace

int main() {
  CheckMechanism();
  CheckLines();
  CheckTree();
  if (failures) {
    std::printf("check-fault-stack: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-fault-stack: ok\n");
  return 0;
}
