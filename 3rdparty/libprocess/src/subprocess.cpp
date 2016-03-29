// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif // __linux__
#include <sys/types.h>

#include <condition_variable>
#include <mutex>
#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/os/strerror.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

using std::condition_variable;
using std::map;
using std::mutex;
using std::string;
using std::vector;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;


Subprocess::Hook::Hook(
    const lambda::function<Try<Nothing>(pid_t)>& _parent_callback)
  : parent_callback(_parent_callback) {}

Subprocess::ChildHook::ChildHook(
    const lambda::function<Try<Nothing>()>& _child_setup)
  : child_setup(_child_setup) {}

Subprocess::ChildHook Subprocess::ChildHook::CHDIR(
    const std::string& working_directory)
{
  return Subprocess::ChildHook([&working_directory]() -> Try<Nothing> {
    // Put child into its own process session to prevent slave suicide
    // on child process SIGKILL/SIGTERM.
    if (::chdir(working_directory.c_str()) == -1) {
      return Error("Could not chdir");
    }

    return Nothing();
  });
}

Subprocess::ChildHook Subprocess::ChildHook::SETSID()
{
  return Subprocess::ChildHook([]() -> Try<Nothing> {
    // Put child into its own process session to prevent slave suicide
    // on child process SIGKILL/SIGTERM.
    if (::setsid() == -1) {
      return Error("Could not setdid");
    }

    return Nothing();
  });
}

namespace internal {

// See the comment below as to why subprocess is passed to cleanup.
static void cleanup(
    const Future<Option<int>>& result,
    Promise<Option<int>>* promise,
    const Subprocess& subprocess)
{
  CHECK(!result.isPending());
  CHECK(!result.isDiscarded());

  if (result.isFailed()) {
    promise->fail(result.failure());
  } else {
    promise->set(result.get());
  }

  delete promise;
}


// This function will invoke `os::close` on all specified file
// descriptors that are valid (i.e., not `None` and >= 0).
static void close(
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds)
{
  int fds[6] = {
    stdinfds.read, stdinfds.write.getOrElse(-1),
    stdoutfds.read.getOrElse(-1), stdoutfds.write,
    stderrfds.read.getOrElse(-1), stderrfds.write
  };

  foreach (int fd, fds) {
    if (fd >= 0) {
      os::close(fd);
    }
  }
}

// This function will invoke `os::cloexec` on all specified file
// descriptors that are valid (i.e., not `None` and >= 0).
static Try<Nothing> cloexec(
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds)
{
  int fds[6] = {
    stdinfds.read, stdinfds.write.getOrElse(-1),
    stdoutfds.read.getOrElse(-1), stdoutfds.write,
    stderrfds.read.getOrElse(-1), stderrfds.write
  };

  foreach (int fd, fds) {
    if (fd >= 0) {
      Try<Nothing> cloexec = os::cloexec(fd);
      if (cloexec.isError()) {
        return Error(cloexec.error());
      }
    }
  }

  return Nothing();
}

}  // namespace internal {


Subprocess::IO Subprocess::PIPE()
{
  return Subprocess::IO(
      []() -> Try<InputFileDescriptors> {
        int pipefd[2];
        if (::pipe(pipefd) == -1) {
          return ErrnoError("Failed to create pipe");
        }

        InputFileDescriptors fds;
        fds.read = pipefd[0];
        fds.write = pipefd[1];
        return fds;
      },
      []() -> Try<OutputFileDescriptors> {
        int pipefd[2];
        if (::pipe(pipefd) == -1) {
          return ErrnoError("Failed to create pipe");
        }

        OutputFileDescriptors fds;
        fds.read = pipefd[0];
        fds.write = pipefd[1];
        return fds;
      });
}


Subprocess::IO Subprocess::PATH(const string& path)
{
  return Subprocess::IO(
      [path]() -> Try<InputFileDescriptors> {
        Try<int> open = os::open(path, O_RDONLY | O_CLOEXEC);
        if (open.isError()) {
          return Error("Failed to open '" + path + "': " + open.error());
        }

        InputFileDescriptors fds;
        fds.read = open.get();
        return fds;
      },
      [path]() -> Try<OutputFileDescriptors> {
        Try<int> open = os::open(
            path,
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

        if (open.isError()) {
          return Error("Failed to open '" + path + "': " + open.error());
        }

        OutputFileDescriptors fds;
        fds.write = open.get();
        return fds;
      });
}


Subprocess::IO Subprocess::FD(int fd, IO::FDType type)
{
  return Subprocess::IO(
      [fd, type]() -> Try<InputFileDescriptors> {
        int prepared_fd = -1;
        switch (type) {
          case IO::DUPLICATED:
            prepared_fd = ::dup(fd);
            break;
          case IO::OWNED:
            prepared_fd = fd;
            break;

          // NOTE: By not setting a default we leverage the compiler
          // errors when the enumeration is augmented to find all
          // the cases we need to provide. Same for below.
        }

        if (prepared_fd == -1) {
          return ErrnoError("Failed to dup");
        }

        InputFileDescriptors fds;
        fds.read = prepared_fd;
        return fds;
      },
      [fd, type]() -> Try<OutputFileDescriptors> {
        int prepared_fd = -1;
        switch (type) {
          case IO::DUPLICATED:
            prepared_fd = ::dup(fd);
            break;
          case IO::OWNED:
            prepared_fd = fd;
            break;
        }

        if (prepared_fd == -1) {
          return ErrnoError("Failed to dup");
        }

        OutputFileDescriptors fds;
        fds.write = prepared_fd;
        return fds;
      });
}


static pid_t defaultClone(const lambda::function<int()>& func)
{
  pid_t pid = ::fork();
  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // Child.
    ::exit(func());
    UNREACHABLE();
  } else {
    // Parent.
    return pid;
  }
}


static void signalHandler(int signal)
{
  // Send SIGKILL to every process in the process group of the
  // calling process.
  kill(0, SIGKILL);
  abort();
}


// Creates a seperate watchdog process to monitor the child process and
// kill it in case the parent process dies.
//
// NOTE: This function needs to be async signal safe. In fact,
// all the library functions we used in this function are async
// signal safe.
static int watchdogProcess()
{
#ifdef __linux__
  // Send SIGTERM to the current process if the parent (i.e., the
  // slave) exits.
  // NOTE:: This function should always succeed because we are passing
  // in a valid signal.
  prctl(PR_SET_PDEATHSIG, SIGTERM);

  // Put the current process into a separate process group so that
  // we can kill it and all its children easily.
  if (setpgid(0, 0) != 0) {
    abort();
  }

  // Install a SIGTERM handler which will kill the current process
  // group. Since we already setup the death signal above, the
  // signal handler will be triggered when the parent (e.g., the
  // slave) exits.
  if (os::signals::install(SIGTERM, &signalHandler) != 0) {
    abort();
  }

  pid_t pid = fork();
  if (pid == -1) {
    abort();
  } else if (pid == 0) {
    // Child. This is the process that is going to exec the
    // process if zero is returned.

    // We setup death signal for the process as well in case
    // someone, though unlikely, accidentally kill the parent of
    // this process (the bookkeeping process).
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    // NOTE: We don't need to clear the signal handler explicitly
    // because the subsequent 'exec' will clear them.
    return 0;
  } else {
    // Parent. This is the bookkeeping process which will wait for
    // the child process to finish.

    // Close the files to prevent interference on the communication
    // between the slave and the child process.
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Block until the child process finishes.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
      abort();
    }

    // Forward the exit status if the child process exits normally.
    if (WIFEXITED(status)) {
      _exit(WEXITSTATUS(status));
    }

    abort();
    UNREACHABLE();
  }
#endif
  return 0;
}


// The main entry of the child process.
//
// NOTE: This function has to be async signal safe.
static int childMain(
    const string& path,
    char** argv,
    char** envp,
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds,
    bool blocking,
    int pipes[2],
    const std::vector<Subprocess::ChildHook>& child_hooks,
    const Watchdog watchdog)
{
  // Close parent's end of the pipes.
  if (stdinfds.write.isSome()) {
    ::close(stdinfds.write.get());
  }
  if (stdoutfds.read.isSome()) {
    ::close(stdoutfds.read.get());
  }
  if (stderrfds.read.isSome()) {
    ::close(stderrfds.read.get());
  }

  // Currently we will block the child's execution of the new process
  // until all the parent hooks (if any) have executed.
  if (blocking) {
    ::close(pipes[1]);
  }

  // Redirect I/O for stdin/stdout/stderr.
  while (::dup2(stdinfds.read, STDIN_FILENO) == -1 && errno == EINTR);
  while (::dup2(stdoutfds.write, STDOUT_FILENO) == -1 && errno == EINTR);
  while (::dup2(stderrfds.write, STDERR_FILENO) == -1 && errno == EINTR);

  // Close the copies. We need to make sure that we do not close the
  // file descriptor assigned to stdin/stdout/stderr in case the
  // parent has closed stdin/stdout/stderr when calling this
  // function (in that case, a dup'ed file descriptor may have the
  // same file descriptor number as stdin/stdout/stderr).
  if (stdinfds.read != STDIN_FILENO &&
      stdinfds.read != STDOUT_FILENO &&
      stdinfds.read != STDERR_FILENO) {
    ::close(stdinfds.read);
  }
  if (stdoutfds.write != STDIN_FILENO &&
      stdoutfds.write != STDOUT_FILENO &&
      stdoutfds.write != STDERR_FILENO) {
    ::close(stdoutfds.write);
  }
  if (stderrfds.write != STDIN_FILENO &&
      stderrfds.write != STDOUT_FILENO &&
      stderrfds.write != STDERR_FILENO) {
    ::close(stderrfds.write);
  }

  if (blocking) {
    // Do a blocking read on the pipe until the parent signals us to
    // continue.
    char dummy;
    ssize_t length;
    while ((length = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
          errno == EINTR);

    if (length != sizeof(dummy)) {
      ABORT("Failed to synchronize with parent");
    }

    // Now close the pipe as we don't need it anymore.
    ::close(pipes[0]);
  }

  // Run the child hooks.
  foreach (const Subprocess::ChildHook& hook, child_hooks) {
    Try<Nothing> callback = hook();

    // If the callback failed, we should abort execution.
    if (callback.isError()) {
      LOG(WARNING)
        << "Failed to execute Subprocess::ChildHook: " << callback.error();

      ABORT("Failed to execute Subprocess::ChildHook: " + callback.error());
    }
  }

  // If the child process should die together with its parent we spawn a
  // separate watchdog process which kills the child when the parent dies.
  //
  // NOTE: The watchdog process sets the process group id in order for it and
  // its child processes to be killed together. We should not (re)set the sid
  // after this.
  if (watchdog == MONITOR) {
    watchdogProcess();
  }

  os::execvpe(path.c_str(), argv, envp);

  ABORT("Failed to os::execvpe on path '" + path + "': " + os::strerror(errno));
}


struct CloneConfig
{
  char** argv;
  char** environment;
  string path;
  InputFileDescriptors stdinfds;
  OutputFileDescriptors stdoutfds;
  OutputFileDescriptors stderrfds;
  mutex* mut;
  bool* ready;
  condition_variable* notifier;
  Watchdog watchdog;
  std::vector<Subprocess::ChildHook> child_hooks;

  ~CloneConfig() {
    std::cout << "CloneConfig Destruction";
    // As we are waiting on the condition_variable we know the parent has
    // finished using those variables at this point.
    delete mut;
    delete ready;
    delete notifier;

    // Need to delete 'envp' if we had environment variables passed to
    // us and we needed to allocate the space.
    if (environment != os::raw::environment()) {
      // We ignore the last 'envp' entry since it is NULL.
      for (size_t index = 0; environment[index] != NULL; ++index) {
        delete[] environment[index];
      }

      delete[] environment;
    }
  }
};


// Creates a seperate watchdog process to monitor the child process and
// kill it in case the parent process dies.
// Note: This function needs to be async signal safe. In fact,
// all the library functions we used in this function are async
// signal safe.
static int watchdogSharedProcess()
{
#ifdef __linux__
  // Send SIGTERM to the current process if the parent (i.e., the
  // slave) exits.
  // Note: This function should always succeed because we are passing
  // in a valid signal.
  prctl(PR_SET_PDEATHSIG, SIGTERM);

  // Put the current process into a separate process group so that
  // we can kill it and all its children easily.
  if (setpgid(0, 0) != 0) {
    LOG(ERROR) <<"Could not install Sigterm Handler in watchdogProcess";
    // Exit without cleanup which could effect the parent process.
    _exit(EXIT_FAILURE);
  }

  // Install a SIGTERM handler which will kill the current process
  // group. Since we already setup the death signal above, the
  // signal handler will be triggered when the parent (e.g., the
  // slave) exits.
  if (os::signals::install(SIGTERM, &signalHandler) != 0) {
    LOG(ERROR) <<"Could not install Sigterm Handler in watchdogProcess";
    // Exit without cleanup which could effect the parent process.
    _exit(EXIT_FAILURE);
  }

  // TODO(joerg84): Use the optimized clone function here.
  pid_t pid = fork();
  if (pid == -1) {
    // Exit without cleanup which could effect the parent process.
    LOG(ERROR) <<"Failed to fork watchdogProcess";
    _exit(EXIT_FAILURE);
  } else if (pid == 0) {
    // Child. This is the process that is going to exec the
    // process if zero is returned.

    // We setup death signal for the process as well in case
    // someone, though unlikely, accidentally kill the parent of
    // this process (the bookkeeping process).
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    // NOTE: We don't need to clear the signal handler explicitly
    // because the subsequent 'exec' will clear them.
    return 0;
  } else {
    // Parent. This is the bookkeeping process which will wait for
    // the child process to finish.

    // Close the files to prevent interference on the communication
    // between the slave and the child process.
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Block until the child process finishes.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
      LOG(ERROR) <<"Failed to wait for child in watchdogProcess";
      // Exit without cleanup which could effect the parent process.
      _exit(EXIT_FAILURE);
    }

    // Forward the exit status if the child process exits normally.
    if (WIFEXITED(status)) {
      _exit(WEXITSTATUS(status));
    }

    abort();
    UNREACHABLE();
  }
#endif
  // WatchDogProcess shared process should only be called on linux.
  return 0;
}


// The main entry of the optimized child process running in the same address
// space.
static int childMainShared(void* conf) {
  Owned<CloneConfig> config =
    Owned<CloneConfig>(static_cast<CloneConfig*>(conf));
    // TODO(joerg84): Add consistency checks.

  // Close parent's end of the pipes.
  if (config->stdinfds.write.isSome()) {
    ::close(config->stdinfds.write.get());
  }
  if (config->stdoutfds.read.isSome()) {
    ::close(config->stdoutfds.read.get());
  }
  if (config->stderrfds.read.isSome()) {
    ::close(config->stderrfds.read.get());
  }

  // Redirect I/O for stdin/stdout/stderr.
  while (::dup2(config->stdinfds.read, STDIN_FILENO) == -1 &&
      errno == EINTR);
  while (::dup2(config->stdoutfds.write, STDOUT_FILENO) == -1 &&
      errno == EINTR);
  while (::dup2(config->stderrfds.write, STDERR_FILENO) == -1 &&
     errno == EINTR);

  // Wait for parent process;
  CHECK_NOTNULL(config->mut);
  CHECK_NOTNULL(config->notifier);
  CHECK_NOTNULL(config->ready);
  std::unique_lock<mutex> lk(*(config->mut));
  config->notifier->wait(lk, [&config]{return *(config->ready);});

  // Run the child hooks.
  foreach (const Subprocess::ChildHook& hook, config->child_hooks) {
    Try<Nothing> callback = hook();

    // If the callback failed, we should abort execution.
    if (callback.isError()) {
      LOG(ERROR)
        << "Failed to execute Subprocess::ChildHook: " << callback.error();

      // Exit without cleanup which could effect the parent process.
      _exit(EXIT_FAILURE);
    }
  }

  // If the process should die together with its parent we spawn an seperate
  // watchdog process which kill the child in such case.
  if (config->watchdog == MONITOR) {
    watchdogSharedProcess();
  }

  os::execvpe(config->path.c_str(),
              config->argv,
              config->environment);

  LOG(ERROR) <<"Failed to os::execvpe on path : " << os::strerror(errno);

  // Exit without cleanup which could effect the parent process.
  _exit(EXIT_FAILURE);
}


Try<Subprocess> subprocess(
    const string& path,
    vector<string> argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const Option<flags::FlagsBase>& flags,
    const Option<map<string, string>>& environment,
    const vector<Subprocess::Hook>& parent_hooks,
    const std::vector<Subprocess::ChildHook>& child_hooks,
    const Watchdog watchdog,
    const Option<int>& namespaces)
{
  // File descriptors for redirecting stdin/stdout/stderr.
  // These file descriptors are used for different purposes depending
  // on the specified I/O modes.
  // See `Subprocess::PIPE`, `Subprocess::PATH`, and `Subprocess::FD`.
  InputFileDescriptors stdinfds;
  OutputFileDescriptors stdoutfds;
  OutputFileDescriptors stderrfds;

  // Prepare the file descriptor(s) for stdin.
  Try<InputFileDescriptors> input = in.input();
  if (input.isError()) {
    return Error(input.error());
  }

  stdinfds = input.get();

  // Prepare the file descriptor(s) for stdout.
  Try<OutputFileDescriptors> output = out.output();
  if (output.isError()) {
    internal::close(stdinfds, stdoutfds, stderrfds);
    return Error(output.error());
  }

  stdoutfds = output.get();

  // Prepare the file descriptor(s) for stderr.
  output = err.output();
  if (output.isError()) {
    internal::close(stdinfds, stdoutfds, stderrfds);
    return Error(output.error());
  }

  stderrfds = output.get();

  // TODO(jieyu): Consider using O_CLOEXEC for atomic close-on-exec.
  Try<Nothing> cloexec = internal::cloexec(stdinfds, stdoutfds, stderrfds);
  if (cloexec.isError()) {
    internal::close(stdinfds, stdoutfds, stderrfds);
    return Error("Failed to cloexec: " + cloexec.error());
  }

  // Prepare the arguments. If the user specifies the 'flags', we will
  // stringify them and append them to the existing arguments.
  if (flags.isSome()) {
    foreachpair (const string& name, const flags::Flag& flag, flags.get()) {
      Option<string> value = flag.stringify(flags.get());
      if (value.isSome()) {
        argv.push_back("--" + name + "=" + value.get());
      }
    }
  }


#ifdef __linux__
  // TODO(joerg84): Make this the default mode and not just used if namespace
  // flags are used.
  if (namespaces.isSome()) {
    LOG(INFO) << "Subprocess: Using optimized clone function";

    // Setup signaling to child.
    // NOTE: As we are in the same address space we don't have to use pipes for
    // signaling in this case.
    // NOTE: These variables are shared (and cleaned up) with the child.
    bool* ready = new bool;
    mutex* mut = new mutex;
    condition_variable* notifier = new condition_variable;

    // NOTE: The child is responsible for cleaning up all related storage.
    struct CloneConfig* cloneConfig = new CloneConfig;
    cloneConfig->ready = ready;
    cloneConfig->mut = mut;
    cloneConfig->notifier = notifier;

    cloneConfig->stdinfds = stdinfds;
    cloneConfig->stdoutfds = stdoutfds;
    cloneConfig->stderrfds = stderrfds;

    cloneConfig->path = path;

    // The real arguments that will be passed to 'os::execvpe'. We need
    // to construct them here before doing the clone as it might not be
    // async signal safe to perform the memory allocation.
    char** _argv = new char*[argv.size() + 1];
    for (int i = 0; i < argv.size(); i++) {
      _argv[i] = (char*) argv[i].c_str();
    }
    _argv[argv.size()] = NULL;
    cloneConfig->argv = _argv;

    // Like above, we need to construct the environment that we'll pass
    // to 'os::execvpe' as it might not be async-safe to perform the
    // memory allocations.
    char** envp = os::raw::environment();

    if (environment.isSome()) {
      // NOTE: We add 1 to the size for a NULL terminator.
      envp = new char*[environment.get().size() + 1];

      size_t index = 0;
      foreachpair (const string& key, const string& value, environment.get()) {
        string entry = key + "=" + value;
        envp[index] = new char[entry.size() + 1];
        strncpy(envp[index], entry.c_str(), entry.size() + 1);
        ++index;
      }

      envp[index] = NULL;
    }
    cloneConfig->environment = envp;

    int cloneFlags = namespaces.isSome() ? namespaces.get() : 0;
    cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.
    cloneFlags |= CLONE_VM; // Specify CLONE_VM in order to avoid a
                            // copy-on-write view on the address space.

    pid_t pid = os::clone_d(childMainShared, cloneConfig, cloneFlags);

    // Run the parent hooks.
    foreach (const Subprocess::Hook& hook, parent_hooks) {
      Try<Nothing> callback = hook.parent_callback(pid);

      // If the hook callback fails, we shouldn't proceed with the
      // execution and hence the child process should be killed.
      if (callback.isError()) {
        LOG(WARNING)
          << "Failed to execute Subprocess::Hook in parent for child '"
          << pid << "': " << callback.error();

        // Close the child-ends of the file descriptors that are created
        // by this function.
        os::close(stdinfds.read);
        os::close(stdoutfds.write);
        os::close(stderrfds.write);

        // Ensure the child is killed.
        ::kill(pid, SIGKILL);

        return Error(
            "Failed to execute Subprocess::Hook in parent for child '" +
            stringify(pid) + "': " + callback.error());
      }
    }

    // Signal child to continue.
    {
      std::lock_guard<std::mutex> lk(*mut);
      *ready = true;
    }
    notifier->notify_one();

    // Parent.
    Subprocess process;
    process.data->pid = pid;

    // Close the child-ends of the file descriptors that are created
    // by this function.
    os::close(stdinfds.read);
    os::close(stdoutfds.write);
    os::close(stderrfds.write);

    // For any pipes, store the parent side of the pipe so that
    // the user can communicate with the subprocess.
    process.data->in = stdinfds.write;
    process.data->out = stdoutfds.read;
    process.data->err = stderrfds.read;

    // Rather than directly exposing the future from process::reap, we
    // must use an explicit promise so that we can ensure we can receive
    // the termination signal. Otherwise, the caller can discard the
    // reap future, and we will not know when it is safe to close the
    // file descriptors.
    // Promise<Option<int>>* promise = new Promise<Option<int>>();
    // process.data->status = promise->future();

    // We need to bind a copy of this Subprocess into the onAny callback
    // below to ensure that we don't close the file descriptors before
    // the subprocess has terminated (i.e., because the caller doesn't
    // keep a copy of this Subprocess around themselves).
    // process::reap(process.data->pid)
    //  .onAny(lambda::bind(internal::cleanup, lambda::_1, promise, process));

    return process;
  }
#endif // __linux__
  // If we are not on linux will continue using the default clone function
  // and a copy-on-write address space.

  // The real arguments that will be passed to 'os::execvpe'. We need
  // to construct them here before doing the clone as it might not be
  // async signal safe to perform the memory allocation.
  char** _argv = new char*[argv.size() + 1];
  for (int i = 0; i < argv.size(); i++) {
    _argv[i] = (char*) argv[i].c_str();
  }
  _argv[argv.size()] = NULL;

  // Like above, we need to construct the environment that we'll pass
  // to 'os::execvpe' as it might not be async-safe to perform the
  // memory allocations.
  char** envp = os::raw::environment();

  if (environment.isSome()) {
    // NOTE: We add 1 to the size for a NULL terminator.
    envp = new char*[environment.get().size() + 1];

    size_t index = 0;
    foreachpair (const string& key, const string& value, environment.get()) {
      string entry = key + "=" + value;
      envp[index] = new char[entry.size() + 1];
      strncpy(envp[index], entry.c_str(), entry.size() + 1);
      ++index;
    }

    envp[index] = NULL;
  }

  // Currently we will block the child's execution of the new process
  // until all the `parent_hooks` (if any) have executed.
  int pipes[2];
  const bool blocking = !parent_hooks.empty();

  if (blocking) {
    // We assume this should not fail under reasonable conditions so we
    // use CHECK.
    CHECK_EQ(0, ::pipe(pipes));
  }

  // Now, clone the child process.
  pid_t pid = defaultClone(lambda::bind(
      &childMain,
      path,
      _argv,
      envp,
      stdinfds,
      stdoutfds,
      stderrfds,
      blocking,
      pipes,
      child_hooks,
      watchdog));

  delete[] _argv;

  // Need to delete 'envp' if we had environment variables passed to
  // us and we needed to allocate the space.
  if (environment.isSome()) {
    CHECK_NE(os::raw::environment(), envp);

    // We ignore the last 'envp' entry since it is NULL.
    for (size_t index = 0; index < environment->size(); index++) {
      delete[] envp[index];
    }

    delete[] envp;
  }

  if (pid == -1) {
    // Save the errno as 'close' below might overwrite it.
    ErrnoError error("Failed to clone");
    internal::close(stdinfds, stdoutfds, stderrfds);

    if (blocking) {
      os::close(pipes[0]);
      os::close(pipes[1]);
    }

    return error;
  }

  if (blocking) {
    os::close(pipes[0]);

    // Run the parent hooks.
    foreach (const Subprocess::Hook& hook, parent_hooks) {
      Try<Nothing> callback = hook.parent_callback(pid);

      // If the hook callback fails, we shouldn't proceed with the
      // execution and hence the child process should be killed.
      if (callback.isError()) {
        LOG(WARNING)
          << "Failed to execute Subprocess::Hook in parent for child '"
          << pid << "': " << callback.error();

        os::close(pipes[1]);

        // Close the child-ends of the file descriptors that are created
        // by this function.
        os::close(stdinfds.read);
        os::close(stdoutfds.write);
        os::close(stderrfds.write);

        // Ensure the child is killed.
        ::kill(pid, SIGKILL);

        return Error(
            "Failed to execute Subprocess::Hook in parent for child '" +
            stringify(pid) + "': " + callback.error());
      }
    }

    // Now that we've executed the parent hooks, we can signal the child to
    // continue by writing to the pipe.
    char dummy;
    ssize_t length;
    while ((length = ::write(pipes[1], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);

    os::close(pipes[1]);

    if (length != sizeof(dummy)) {
      // Ensure the child is killed.
      ::kill(pid, SIGKILL);

      // Close the child-ends of the file descriptors that are created
      // by this function.
      os::close(stdinfds.read);
      os::close(stdoutfds.write);
      os::close(stderrfds.write);
      return Error("Failed to synchronize child process");
    }
  }


  // Parent.
  Subprocess process;
  process.data->pid = pid;

  // Close the child-ends of the file descriptors that are created
  // by this function.
  os::close(stdinfds.read);
  os::close(stdoutfds.write);
  os::close(stderrfds.write);

  // For any pipes, store the parent side of the pipe so that
  // the user can communicate with the subprocess.
  process.data->in = stdinfds.write;
  process.data->out = stdoutfds.read;
  process.data->err = stderrfds.read;

  // Rather than directly exposing the future from process::reap, we
  // must use an explicit promise so that we can ensure we can receive
  // the termination signal. Otherwise, the caller can discard the
  // reap future, and we will not know when it is safe to close the
  // file descriptors.
  Promise<Option<int>>* promise = new Promise<Option<int>>();
  process.data->status = promise->future();

  // We need to bind a copy of this Subprocess into the onAny callback
  // below to ensure that we don't close the file descriptors before
  // the subprocess has terminated (i.e., because the caller doesn't
  // keep a copy of this Subprocess around themselves).
  process::reap(process.data->pid)
    .onAny(lambda::bind(internal::cleanup, lambda::_1, promise, process));

  return process;
}

}  // namespace process {
