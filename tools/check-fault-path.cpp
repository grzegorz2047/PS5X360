// SPDX-License-Identifier: MIT
// Fix 65 (tools/source-1.0.2/fixes/65-fault-path.patch): every fault the emulator does not take is
// reported, and the report goes to the game log. Ninja Gaiden II on 1.0.4_candidate_22 ended with
// the system's own dialog ("page fault (user write data, protection violation)" at guest memory,
// SIGSEGV) and no line from any handler, and every game's log said "Crash report remains in
// boot.log".
//  (a) the mechanism, on this host: a second fault inside a SIGSEGV handler arrives with the signal
//      blocked and ends the process with no further handler output, unless the handler is
//      installed with SA_NODEFER (each case in a child process);
//  (b) the assembled tree: the console's fault transport is SA_NODEFER, counts its depth per thread
//      (written at a nested fault, a third reported and left to the system), writes a FAULT line
//      with the emulator's page state before forwarding what no handler took, and reports itself
//      when nothing is behind it; the page state is read with the memory lock only tried; the
//      report file takes the game log's own descriptor when dup2 fails, and the game log says why.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

// --- (a) -----------------------------------------------------------------------------------------
int report_pipe = -1;
volatile char* protected_page = nullptr;
volatile int depth = 0;
void Handler(int, siginfo_t*, void*) {
  const char mark = char('0' + ++depth);
  (void)!write(report_pipe, &mark, 1);
  if (depth == 1) {
    protected_page[1] = 1;  // the handler itself writes protected memory: a second fault
    return;
  }
  _exit(3);  // the second fault reached the handler
}
// Returns the handler entries the child wrote ("1": only the first; "12": the nested one too).
std::string Run(bool nodefer) {
  int fds[2];
  if (pipe(fds)) return "?";
  const pid_t child = fork();
  if (child == 0) {
    close(fds[0]);
    report_pipe = fds[1];
    protected_page = static_cast<char*>(mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    struct sigaction action{};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | (nodefer ? SA_NODEFER : 0);
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    protected_page[0] = 1;
    _exit(0);
  }
  close(fds[1]);
  std::string marks;
  char mark;
  while (read(fds[0], &mark, 1) == 1) marks += mark;
  close(fds[0]);
  int status = 0;
  waitpid(child, &status, 0);
  std::printf("   nested fault %s SA_NODEFER: handler entries \"%s\", child %s %d\n", nodefer ? "with" : "without",
              marks.c_str(), WIFSIGNALED(status) ? "ended by signal" : "exited",
              WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
  return marks;
}
void CheckMechanism() {
  Expect(Run(false) == "1", "without SA_NODEFER the nested fault ends the process with no handler output");
  Expect(Run(true) == "12", "with SA_NODEFER the handler sees the nested fault");
}

// --- (b) -----------------------------------------------------------------------------------------
void CheckTree() {
  const std::string transport = Read("platform/ps5/exception_handler.cpp");
  Expect(transport.find("action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;") != std::string::npos,
         "the transport is SA_NODEFER (and on the signal stack)");
  const size_t callback = transport.find("void callback(int number, siginfo_t* info, void* raw_context) {");
  const size_t depth_at = transport.find("const int depth = xbox360ps5::FaultDepth(1);", callback);
  const size_t leave = transport.find("~Leave() { if (known) xbox360ps5::FaultDepth(-1); }", depth_at);
  const size_t nested = transport.find("a fault inside the fault handler, handled like any other", leave);
  const size_t third = transport.find("if (depth > 3) {", leave);
  const size_t layout = transport.find("HostThreadContext host{};", callback);
  Expect(callback != std::string::npos && depth_at != std::string::npos && leave != std::string::npos &&
             nested != std::string::npos && third < nested && nested < layout,
         "the depth is counted first, a nested fault written, a third reported and left to the system");
  const size_t untaken = transport.find("\"an access no handler took\"", layout);
  const size_t describe = transport.rfind("xe::memory::DescribeHostPage(", untaken);
  const size_t forward = transport.find("forward(number, info, raw_context);", untaken);
  Expect(untaken != std::string::npos && describe > layout && describe < untaken && forward != std::string::npos,
         "what no handler took is written with the page state, then forwarded");
  const size_t forward_fn = transport.find("void forward(int number, siginfo_t* info, void* context) {");
  const size_t reports = transport.find("xbox360ps5::ReportFatalSignal(number, info, context);", forward_fn);
  const size_t raise_at = transport.find("raise(number); return;", forward_fn);
  Expect(forward_fn != std::string::npos && reports != std::string::npos && reports < raise_at,
         "with nothing behind the transport it reports before the default action");

  const std::string memory = Read("platform/ps5/memory_ps5.cpp");
  const size_t page = memory.find("bool DescribeHostPage(const void* address, char* out, size_t size) {");
  Expect(page != std::string::npos && memory.find("std::unique_lock lock(guard, std::try_to_lock);", page) != std::string::npos &&
             memory.find("page state unknown (the memory lock is held)", page) != std::string::npos,
         "the page state is read with the memory lock only tried");

  const std::string report = Read("platform/ps5/crash_report.cpp");
  Expect(report.find("std::atomic<int> report_file{-1};") != std::string::npos &&
             report.find("report_file.store(next);") != std::string::npos &&
             report.find("dup2 errno %d") != std::string::npos,
         "when dup2 fails the game log's own descriptor becomes the report file");
  Expect(report.find("faults.fetch_add(delta, std::memory_order_relaxed) + delta;") != std::string::npos &&
             report.find("put(\"[X360] FAULT \"); put(why);") != std::string::npos &&
             report.find("Handle(number, static_cast<siginfo_t*>(info), context);") != std::string::npos,
         "the depth per thread, the FAULT line and the whole report for the transport");
  Expect(Count(report, "Mirror(") >= 6, "every report line also goes to the hang-watch file");

  const std::string main_code = Read("src/canary_game_main.cpp");
  Expect(main_code.find("XELOGW(\"Crash report remains in boot.log: {}\", xbox360ps5::CrashReportNote());") !=
                 std::string::npos &&
             main_code.find("XELOGW(\"Crash report: {}\", xbox360ps5::CrashReportNote());") != std::string::npos,
         "the game log says where its crash report goes, and why");
}

}  // namespace

int main() {
  CheckMechanism();
  CheckTree();
  if (failures) {
    std::printf("check-fault-path: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-fault-path: ok\n");
  return 0;
}
