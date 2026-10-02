#include "treewidth.hpp"

#include <chrono>
#include <iostream>
#include <fstream>
#include <ostream>
#include <cstdlib>
#include <cassert>
#include <queue>
#include <algorithm>
#include <sstream>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "utils.hpp"

namespace sspp {

namespace decomp {
namespace {
string TmpInstance(int a, int b, int c, string tmp_dir) {
  std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
  return tmp_dir+"/instance"+std::to_string(micros)+"_"+std::to_string(a)+"_"+std::to_string(b)+"_"+std::to_string(c)+".tmp";
}

// Directory of the running executable (symlinks resolved), or "" if unknown.
string SelfDir() {
  char buf[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return "";
  char real[PATH_MAX];
  if (realpath(buf, real) == nullptr) return "";
  string path(real);
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
  if (len <= 0) return "";
  buf[len] = 0;
  string path(buf);
#endif
  size_t slash = path.rfind('/');
  return slash == string::npos ? "" : path.substr(0, slash);
}

// flow_cutter_pace17 is looked for in $SHARPSAT_FLOWCUTTER, then beside the
// sharpSAT executable, then in the working directory (the original behaviour),
// so sharpSAT can be run from anywhere.
string FlowCutterBinary() {
  const char* env = getenv("SHARPSAT_FLOWCUTTER");
  if (env != nullptr && *env != 0) return env;
  string dir = SelfDir();
  if (!dir.empty()) {
    string beside = dir + "/flow_cutter_pace17";
    if (access(beside.c_str(), X_OK) == 0) return beside;
  }
  return "./flow_cutter_pace17";
}

// Seconds flowcutter gets to exit after SIGTERM before it is sent SIGKILL.
const int kKillGrace = 5;

// If sharpSAT itself is told to stop (SIGTERM, as a caller's timeout sends,
// SIGINT or SIGHUP) while the decomposition is being computed, it must not leave
// flowcutter running -- in its own process group, nothing else would stop it
// before its alarm backstop -- nor the two temp files.  The handler does only
// async-signal-safe things: kill, unlink, then re-raise with the default action
// so the exit status still says which signal it was.  The state it reads is set
// up before the handler is installed and is plain data (paths copied into fixed
// buffers), since a handler cannot touch std::string.
volatile sig_atomic_t g_fc_pid = 0;
char g_tmp1[PATH_MAX] = "";
char g_tmp2[PATH_MAX] = "";
const int kGuardedSignals[] = {SIGTERM, SIGINT, SIGHUP};

extern "C" void CleanupAndReraise(int sig) {
  pid_t pid = g_fc_pid;
  if (pid > 0) kill(-pid, SIGKILL);
  if (g_tmp1[0]) unlink(g_tmp1);
  if (g_tmp2[0]) unlink(g_tmp2);
  signal(sig, SIG_DFL);
  raise(sig);
}

// Installs CleanupAndReraise for the lifetime of the temp files, and restores
// whatever was there before when it goes out of scope.  A signal that was
// IGNORED when sharpSAT started is left ignored: that is how nohup works
// (SIGHUP), and how a shell starts background jobs (SIGINT), and catching it
// would make sharpSAT die of a signal its caller asked it to survive.
class TempFileGuard {
 public:
  TempFileGuard(const string& tmp1, const string& tmp2) {
    snprintf(g_tmp1, sizeof(g_tmp1), "%s", tmp1.c_str());
    snprintf(g_tmp2, sizeof(g_tmp2), "%s", tmp2.c_str());
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = CleanupAndReraise;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < kN; i++) {
      sigaction(kGuardedSignals[i], nullptr, &old_[i]);
      installed_[i] = old_[i].sa_handler != SIG_IGN;
      if (installed_[i]) sigaction(kGuardedSignals[i], &sa, nullptr);
    }
  }
  ~TempFileGuard() {
    for (size_t i = 0; i < kN; i++) {
      if (installed_[i]) sigaction(kGuardedSignals[i], &old_[i], nullptr);
    }
    g_fc_pid = 0;
    g_tmp1[0] = 0;
    g_tmp2[0] = 0;
  }
 private:
  static const size_t kN = sizeof(kGuardedSignals) / sizeof(int);
  struct sigaction old_[kN];
  bool installed_[kN];
};

// Waits for pid, retrying on EINTR.
int WaitFor(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      cerr << "c o waitpid failed: " << strerror(errno) << endl;
      exit(1);
    }
  }
  return status;
}

// Fatal error while running flowcutter: remove its temp files and exit.
[[noreturn]] void Fail(const string& in_file, const string& out_file) {
  std::remove(in_file.c_str());
  std::remove(out_file.c_str());
  exit(1);
}

// Runs flowcutter on in_file for `time` seconds, then sends SIGTERM, on which
// it writes its best decomposition to out_file and exits 0. Done in-process
// rather than with timeout(1), which macOS does not have. Being killed by our
// own SIGTERM is not an error: with a very short `time` it can land before
// flowcutter installs its handler, and a wrapper shell dies of it after
// flowcutter has reported. Whether out_file holds a decomposition is for the
// caller to check. Setup failures (missing binary, unwritable tmpdir) are fatal.
void RunFlowCutter(const string& binary, double time, const string& in_file, const string& out_file) {
  // The child reports a setup failure as (step, errno) through this pipe; a
  // successful exec closes it (FD_CLOEXEC), so the parent reads EOF.
  int report[2];
  if (pipe(report) != 0) {
    cerr << "c o pipe failed: " << strerror(errno) << endl;
    Fail(in_file, out_file);
  }
  fcntl(report[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  if (pid < 0) {
    cerr << "c o fork failed: " << strerror(errno) << endl;
    Fail(in_file, out_file);
  }
  if (pid == 0) {
    close(report[0]);
    // Own process group, so the signals below reach anything it spawns.
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    // Backstop if sharpSAT dies before the deadline: the alarm survives exec,
    // and flowcutter does not handle SIGALRM, so it is killed by it.
    alarm((unsigned)time + 2 * kKillGrace + 1);
    int step = 0;
    int in = open(in_file.c_str(), O_RDONLY);
    if (in >= 0) {
      step = 1;
      int out = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (out >= 0) {
        step = 2;
        int err = open("/dev/null", O_WRONLY);
        if (err >= 0) {
          step = 3;
          dup2(in, 0);
          dup2(out, 1);
          dup2(err, 2);
          execl(binary.c_str(), binary.c_str(), (char*)nullptr);
        }
      }
    }
    int msg[2] = {step, errno};
    ssize_t ignored = write(report[1], msg, sizeof(msg));
    (void)ignored;
    _exit(127);
  }
  setpgid(pid, 0);  // also in the parent, so kill(-pid) cannot race the child
  g_fc_pid = pid;   // only now, so the signal handler never kills a group that is not there yet
  close(report[1]);
  int msg[2];
  ssize_t got;
  while ((got = read(report[0], msg, sizeof(msg))) < 0 && errno == EINTR) {}
  close(report[0]);
  if (got == (ssize_t)sizeof(msg)) {
    WaitFor(pid);
    g_fc_pid = 0;
    // msg[0] is the step that failed, in the order the child attempts them.
    const string what[] = {"open " + in_file, "create " + out_file, "open /dev/null", "run " + binary};
    cerr << "c o could not " << what[msg[0]] << ": " << strerror(msg[1]);
    if (msg[0] == 3) cerr << " (set SHARPSAT_FLOWCUTTER or install it beside sharpSAT)";
    cerr << endl;
    Fail(in_file, out_file);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(time);
  int status = 0;
  bool terminated = false;
  while (true) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) {
      cerr << "c o waitpid failed: " << strerror(errno) << endl;
      Fail(in_file, out_file);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(-pid, SIGTERM);
      terminated = true;
      auto kill_at = std::chrono::steady_clock::now() + std::chrono::seconds(kKillGrace);
      while ((r = waitpid(pid, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < kill_at) {
        usleep(10000);
      }
      if (r == 0) {
        cerr << "c o flowcutter ignored SIGTERM for " << kKillGrace << "s; killing it" << endl;
        kill(-pid, SIGKILL);
        status = WaitFor(pid);
      }
      break;
    }
    usleep(10000);
  }
  // Reaped: its pid (and so its group id) may now be reused, so the signal
  // handler must stop aiming at it.
  g_fc_pid = 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return;
  if (terminated && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM) return;
  cerr << "c o " << binary << " failed (status " << status << ")" << endl;
  Fail(in_file, out_file);
}

// One bag holding every vertex: valid, but gives the heuristic nothing.
TreeDecomposition TrivialDecomposition(int n) {
  TreeDecomposition dec(1, n);
  vector<int> all;
  for (int i = 0; i < n; i++) {
    all.push_back(i);
  }
  dec.SetBag(1, all);
  return dec;
}
} // namespace

TreeDecomposition Treedecomp(const Graph& graph, double time, string tmp_dir) {
	int n = graph.n();
	if (n == 0) {
		TreeDecomposition dec(0, 0);
		return dec;
	}
	if (n == 1) {
		TreeDecomposition dec(1, 1);
		dec.SetBag(1, {0});
		return dec;
	}
	if (time < 0.099) {
		return TrivialDecomposition(n);
	}
	assert(n >= 2);
	auto es = graph.Edges();
	int m = es.size();
	string tmp1 = TmpInstance(n, m, 1, tmp_dir);
	string tmp2 = TmpInstance(n, m, 2, tmp_dir);
	// From here until both files are removed, a SIGTERM/SIGINT/SIGHUP cleans up
	// flowcutter and the files before sharpSAT dies of it.
	TempFileGuard guard(tmp1, tmp2);
	std::ofstream out(tmp1);
	out<<"p tw "<<n<<" "<<m<<'\n';
	for (auto e : es) {
		out << e.F+1 << " " << e.S+1 << '\n';
	}
	out << std::flush;
	out.close();
	cout<<"c o Primal edges "<<es.size()<<endl;
	string tw_binary = FlowCutterBinary();
	cout << "c o CMD: " << tw_binary << " <" << tmp1 << " >" << tmp2 << " (SIGTERM after " << time << "s)" << endl;
	RunFlowCutter(tw_binary, time, tmp1, tmp2);
	cout << "c o tw finish ok" << endl;
	TreeDecomposition dec(0, 0);
	bool found = false;
	std::ifstream in(tmp2);
	string tmp;
	int claim_width = 0;
	while (getline(in, tmp)) {
		std::stringstream ss(tmp);
		ss>>tmp;
		if (tmp == "c") continue;
		if (tmp == "s") {
			ss>>tmp;
			assert(tmp == "td");
			int bs,nn;
			ss>>bs>>claim_width>>nn;
			assert(nn == n);
			claim_width--;
			dec = TreeDecomposition(bs, nn);
			found = true;
		} else if (tmp == "b") {
			int bid;
			ss>>bid;
			vector<int> bag;
			int v;
			while (ss>>v) {
				bag.push_back(v-1);
			}
			dec.SetBag(bid, bag);
		} else {
			int a = stoi(tmp);
			int b;
			ss>>b;
			dec.AddEdge(a, b);
		}
	}
	in.close();
	std::remove(tmp1.c_str());
	std::remove(tmp2.c_str());
	// No decomposition: flowcutter was stopped before its first one (it still
	// exits 0, or was killed before installing its handler), or it threw.
	if (!found) {
		cout << "c o flowcutter produced no decomposition; using a single bag" << endl;
		return TrivialDecomposition(n);
	}
	assert(dec.Width() <= claim_width);
	cout << "c o width " << dec.Width() << endl;
	assert(dec.Verify(graph));
	return dec;
}
} // namespace decomp
} // namespace sspp