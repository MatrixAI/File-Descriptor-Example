#include <stdlib.h>     // EXIT_FAILURE, exit(), mkdtemp(), getenv(), atexit()
#include <stdio.h>      // printf(), snprintf(), remove()
#include <stddef.h>     // NULL
#include <stdbool.h>    // bool, true, false
#include <string.h>     // size_t, strcmp(), strcpy()
#include <unistd.h>     // read(), write(), fork(), exec(), getpid(), getppid()
#include <errno.h>      // perror()
#include <sysexits.h>   // EX_USAGE, EX_CANTCREAT, EX_UNAVAILABLE
#include <ftw.h>        // nftw()
#include <libgen.h>     // basename()
#include <assert.h>     // assert()
#include <fcntl.h>      // fcntl()
#include <sys/socket.h> // PF_UNIX, SOCK_STREAM, socklen_t, struct ucred, socket(), bind(), listen(), accept(), getsockopt
#include <sys/un.h>     // UNIX_PATH_MAX, struct sockaddr_un
#include <sys/types.h>  // pid_t
#include <sys/wait.h>   // waitpid()
#include <sys/select.h> // pselect()
#include <sys/prctl.h>  // PR_SET_PDEATHSIG, prctl()
#include <signal.h>     // SIGCHLD, SIGTERM, sigset_t, struct sigaction, sigemptyset(), sigaddset(), sigprocmask(), sigaction()

#include "argparse.h"

#define STR(s) #s
#define XSTR(s) STR(s)

#if !defined(PKEXEC_PATH) || !defined(MECHANISM_PATH)
    #error "PKEXEC_PATH and MECHANISM_PATH must be defined."
#endif

static char unix_sock_dir[UNIX_PATH_MAX] = {0};

static int ntfw_callback(const char * path, const struct stat * sb, int type_flag, struct FTW * ftw_buf) {

    int status = remove(path);

    if (status != 0) {
        char error_string[8 + UNIX_PATH_MAX];
        snprintf(error_string, sizeof(error_string), "remove(%s)", path);
        perror(error_string);
    }

    return status;

}

static void cleanup_and_exit() {

    if (unix_sock_dir && *unix_sock_dir) {

        ntfw(unix_sock_dir, ntfw_callback, 64, FTW_DEPTH | FTW_PHYS);

    }

}

static void handle_mechanism_process (int signal, siginfo_t * signal_info, void * context) {

    // the signal_info properties...
    /* signal_info->si_pid, */
    /* signal_info->si_uid, */
    /* signal_info->si_status, */
    /* signal_info->si_utime, */
    /* signal_info->si_stime */

    // the only signal we should be handling here is a SIGCHLD
    assert(signal == SIGCHLD);

    switch (signal_info->si_code) {
        case CLD_EXITED:
            switch (signal_info->si_status) {
                case EX_NOPERM:
                    // so basically this means we retry with pkexec
                    // how should we communicate back to the main thread?
                    // a volatile variable?
                break;
            }
        break;
        case CLD_KILLED:
        case CLD_DUMPED:
            // child was killed, this is abnormal, should exit parent process somehow
            // can we exit the main thread here, or is it better to propagate to the parent some how?
            // exit is not a async signal safe function, which means it's not atomic
            // while running the exit, another signal may be received, and handling that will interrupt the exit call
            // the exit call will call the atexit functions, while _exit which is a signal safe function won't it will just close immediately
            // an extra signal being emitted here could mean that the same signal is received, like another SIGCHLD, this causes the same signal handler to run again
            // when dealing with non-atomic actions, the behaviour of the program becomes undefined
            // so how to solve this?
            // the only way to make this manageable is to queue up the signals, and handle them with a event loop/state machine
            // on linux, we could use signalfd
            // to be cross-platform, we could use libuv
            // since we are only using polkit, then signalfd could be way forward
            // the other thing is that you could check whether you're handling a signal or not for every signal handler, but thist would require applying it to all signals
            // also our exit handler should be used for normal exits, and for when a signal is received
            // ultimately this all still requires an event-driven mindset
            // currently all the signal events are asynchronous, causing problems for programming, instead we need to leave signals as asynchronous, but the handling of such signals to be synchronous
            // note that from the kernel's perspective all software interrupts from the userspace (syscalls) are synchronous, because while handling a syscall, no other process could be executing a syscall (single-threaded that is)
            // but the kernel needs to deal with hardware interrupts in the same way
            // ok so i guess the best solution to read up on signalfd here because this program only cares about Linux...
            // remember to close signal fd (unmask it) before calling child process, because only the parent process should be handling the signal fd
            // the caller can decide how to send the signal to the child process
            // but in this case, I don't want to rely on the caller, or process groups
            // simply that if the parent dies, the child must also die
            // so there is a way to propagate the death signals to the child as well
          exit(EX_UNAVAILABLE);
        break;
    }

}

int
launch_mechanism (char const * process_path, char const * process_arguments[], int exec_pipe[2]) {

  pid_t parent_pid = getpid();
  pid_t mechanism_pid = fork();

  if (mechanism_pid == -1) {

    perror("fork()");

  } else if (mechanism_pid == 0) {

    // close the read side in the child
    close(exec_pipe[0]);

    // if the parent dies, we want the child to commit suicide
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
      // if the parent had died, this would result in a SIGPIPE
      // which close the parent process as well
      write(exec_pipe[1], &errno, sizeof(errno));
      exit(EX_OSERR);
    }

    // what if the parent already died!? If so we must die
    // since there's no parent, there's no point writing to the exec pipe
    if (getppid() != parent_pid) {
      exit(EX_UNAVAILABLE);
    }

    // set the write side to close if the subsequent exec works
    // this doesn't guarantee that the mechanism process succeeds
    if ((fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC)) == -1) {
      write(exec_pipe[1], &errno, sizeof(errno));
      exit(EX_OSERR);
    }

    // execute the process with the arguments
    // the child process can access file descriptors in the parent
    // however we need to pass file descriptors from the child to the parent
    // so we'll be using unix domain sockets for communication
    execv(process_path, process_arguments);
    // exec failed, we must write the errno into the pipe
    write(exec_pipe[1], &errno, sizeof(errno));
    // exit the child process
    exit(EX_UNAVAILABLE);

  }

  return mechanism_pid;

}


int
main (int argc, char * * argv) {

    atexit(cleanup_and_exit);

    static const char * const command_usage[] = {
        "privilege-elevation [options] [--] <serial-port-path>",
        NULL,
    };

    unsigned int selected_baud;

    struct argparse_option command_options[] = {
        OPT_HELP(),
        OPT_INTEGER('b', "baud", &selected_baud, "select standard baud rate, the default is 9600"),
        OPT_END(),
    };

    struct argparse argparse;

    argparse_init(&argparse, command_options, command_usage, 0);
    argparse_describe(&argparse, "\nThis demonstrates lazy privilege elevation via opening a secured serial port resource.");

    // make sure the original argc and argv is preserved
    const char * * argv_ = malloc(sizeof(char *) * argc);
    memcpy(argv_, argv, sizeof(char *) * argc);

    int argc_ = argparse_parse(&argparse, argc, argv_);

    if (argc_ < 1) {
        argparse_usage(&argparse);
        exit(EX_USAGE);
    }

    // both the selected_baud and serial_port will be passed to open-serial-device
    const char * serial_port = argv_[0];

    // unix domain socket name
    const char socket_name[] = "socket.sock";

    // default temporary folder
    const char * temporary_folder = getenv("TMPDIR");
    if (!temporary_folder) {
        temporary_folder = "/tmp";
    }

    // template for generating the temporary directory
    char template[UNIX_PATH_MAX - sizeof(socket_name) + 1]; // +1 for \0 byte
    if (snprintf(
            template,
            sizeof(template),
            "%s/%s",
            temporary_folder,
            "polkit_demo.XXXXXX"
        ) >= sizeof(template)
    ) {
        fprintf(stderr, "$TMPDIR path for saving the socket is too long.\n");
        exit(EX_USAGE);
    }

    // creating a temporary directory
    char * directory = mkdtemp(template);
    if (directory == NULL) {
        perror("mkdtemp()");.
        exit(EX_CANTCREAT);
    }

    // copy temporary directory to static variable, so that it can be cleaned up on exit
    strcpy(unix_sock_dir, directory);

    // the unix_sock_path = unix_sock_dir + socket_name
    char unix_sock_path[UNIX_PATH_MAX];
    snprintf(unix_sock_path, sizeof(unix_sock_path), "%s/%s", unix_sock_dir, socket_name);

    // our unix socket address
    struct sockaddr_un unix_sock_addr = {
        .sun_family = AF_UNIX,
        .sun_path = unix_sock_path
    };

    // initialise a new socket
    int unix_sock_fd = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (unix_sock_fd < 0) {
        perror("socket()");
        exit(EX_OSERR);
    }

    // set the listening socket to non-blocking
    // because: http://stackoverflow.com/a/3444832/582917
    if (fcntl(unix_sock_fd, F_SETFL, (fcntl(unix_sock_fd, F_GETFL, 0) | O_NONBLOCK)) == -1) {
        perror("fcntl()");
        exit(EX_OSERR);
    }

    // bind the socket to the socket address
    if (bind(unix_sock_fd, (struct sockaddr *) &unix_sock_addr, sizeof(unix_sock_addr)) != 0) {
        perror("bind()");
        exit(EX_OSERR);
    }

    // listen to the socket address
    // we only expect 1 client, so a backlog of 1 is fine
    if (listen(unix_sock_fd, 1) != 0) {
        perror("listen()");
        exit(EX_OSERR);
    }

    // setup the execution paths
    // basename mutates its parameter, so we need to duplciate it
    char pkexec_path[] = XSTR(PKEXEC_PATH);
    char pkexec_name[] = XSTR(PKEXEC_PATH);
    pkexec_name = basename(pkexec_name);
    char mechanism_path[] = XSTR(MECHANISM_PATH);
    char mechanism_name[] = XSTR(MECHANISM_PATH);
    mechanism_name = basename(mechanism_name);

    // block SIGCHLD in addition to any existing signals being blocked
    sigset_t signal_sigchld_mask;
    sigset_t signal_current_mask;
    sigemptyset(&signal_sigchld_mask);
    sigaddset(&signal_sigchld_mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &signal_sigchld_mask, &signal_current_mask) != -1) {
        perror("sigprocmask()");
        exit(EX_OSERR);
    }

    // setup the signal handler for SIGCHLD prior to masking SIGCHLD and spawning the child process
    // SIGCHLD carries the information about the child process, to make use of it we need to use SA_SIGINFO flag
    // also we don't care if the child process is suspended and continued, so we also add SA_NOCLDSTOP flag
    struct sigaction signal_action = {0};
    signal_action.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
    signal_action.sa_sigaction = handle_mechanism_process;
    if (sigaction(SIGCHLD, &signal_action, NULL) == -1) {
        perror('sigaction()');
        exit(EX_OSERR);
    }


    // setup a pipe for between parent and forked child process
    // to communicate errors during the fork prior to the exec
    int exec_pipe[2];
    if (pipe(exec_pipe) != 0) {
      perror("pipe()");
      exit(EX_OSERR);
    }

    // attempt unprivileged open-serial-device
    // if we already have the permissions, this is enough to open the serial device
    // if we lack the permissions, then we will attempt privilege elevation with pkexec
    char * mechanism_args[] = {
      mechanisms_name,
      serial_port,
      unix_sock_path,
      (char *) NULL
    };

    int mechanism_pid = launch_mechanism(mechanism_path, mechanism_args, exec_pipe);

    if (mechanism_pid == -1) {
      perror("launch_mechanism()");
      exit(EX_OSERR);
    }

    // close the write end on the parent
    close(exec_pipe[1]);

    // this blocks until we either receive a close or an actual write
    // on close, the size of the read will be 0
    // on write, the size of the read will be > 0
    // we use close to mean successful exec
    // we use write to mean there was an error
    int exec_errno;
    if (read(exec_pipe[0], &exec_errno, sizeof(exec_errno)) > 0) {
        fprintf(stderr, "Could not execute the mechanism.\n");
        fprintf(stderr, "execl(): %s\n", strerror(exec_errno));
        exit(EX_UNAVAILABLE);
    }

    // exec succeeded, close the read end
    close(exec_pipe[0]);

    // now we have 2 ways of receiving information about the mechanism
    // if the mechanism is broken, we'll receive information via SIGCHLD
    // if the mechanism works, we'll receive information via the socket
    // in both cases, there are errors to handle
    // we may receive information from both at the same time
    // at one point receiving info via the socket, and then receive a SIGCHLD signal
    // the only success signal we assume from SIGCHLD is a normal exit
    // anything else indicates an error, and that we should exit the program
    // the mechanism will communicate either a successful FD
    // or it communicates a permission error
    // if it is a permission error, we restart the child process with elevation
    // in this way, the socket is always used for normal operations
    // while the SIGCHLD handler is used for exceptions
    // cleanup will not occur under an async safe exit for the SIGCHLD
    // the only way to resolve this is via signalfd or libuv

    // use pselect to resolve the race condition between SIGCHLD and the socket
    // pselect will block until the unix domain socket has pending data
    // or until it interrupted by a signal such as SIGCHLD
    // it works because we jump into kernel-space avoiding the race
    // between sigprocmask and select

    // it appears to be an alternative to the self-pipe trick
    // however pselect is not as portable as the self-pipe trick
    // this article explains the main problem: https://lwn.net/Articles/176911/
    // this is the socket we have to listen on...
    // accept(unix_sock_fd (struct sockaddr *) &unix_peer_addr, &unix_peer_addr_size) != -1


    // listen on unix_sock_fd for pending connections
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(unix_sock_fd, &readfds);

    while (1) {
      // unmask SIGCHLD, allowing it to be handled during the pselect call
      // remember that the signal_current_mask didn't have SIGCHLD set on it
      // we don't need a timeout on this, as we can receive SIGCHLD to tell us to exit
      int status = pselect(1, &readfds, NULL, NULL, NULL, &signal_current_mask);
      if (status == -1) {
        if (errno != EINTR) {
          perror('pselect()');
          exit(EX_OSERR);
        }
      } else if (status > 0) {
        // break if we have pending connection on the unix sock fd
        if (FD_ISSET(unix_sock_fd, &readfds)) {
          break;
        }
      }
    }

    // permanently unmask the SIGCHLD, allowing it to interrupt us
    // it always represents an error except for a successful exit
    if (sigprocmask(SIG_SET, &signal_current_mask, NULL) != -1) {
        perror("sigprocmask()");
        exit(EX_OSERR);
    }

    // mechanism's unix socket address (an accepted connection to the mechanism)
    struct sockaddr_un unix_peer_addr = {0};
    socklen_t unix_peer_addr_size = sizeof(unix_peer_addr);

    // accept the connection from the child process
    // we only expect 1 connection, so there's no forking the connection handler
    // since it is non-blocking, this would fail as soon as there's no pending connections
    // but our pselect call tells us there's a connection
    // if that connection request is dropped while reaching here
    // then that's an exception!
    int unix_peer_fd = accept(unix_sock_fd, (struct sockaddr *) &unix_peer_addr, &unix_peer_addr_size);

    if (unix_peer_fd == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        perror("accept()");
        exit(EX_UNAVAILABLE);
      } else {
        perror("accept()");
        exit(EX_OSERR);
      }
    }

    // once we have accepted a connection, we don't need to listen anything else
    close(unix_sock_fd);
    // and unlink the socket file

    // check if the peer is the PID that we intended to launch
    struct ucred peer_credentials = {0};
    int peer_credentials_size = sizeof(peer_credentials);

    if (getsockopt(unix_peer_fd, SOL_SOCKET, SO_PEERCRED, &peer_credentials, &peer_credentials_size) != 0) {
      perror("getsockopt()");
      exit(EX_OSERR);
    }

    if (peer_credentials.pid != mechanism_pid) {
      printf("The connecting peer's PID did not match the launched mechanism PID\n");
      exit(EX_PROTOCOL);
    }

    // i need to read on 2 kinds of messages
    // one is a permission error message
    // another is the file descriptor message
    // now that we are using the unix domain datagram socket
    // this means our messages is atomic, so the writer can't possibly write half a message
    // however we now need a message protocol
    // we can do this by doing
    // { enum { PERMERR, PRIVFD }, int data }
    // the above struct type should be kept in a header
    // both the server and the client includes this header as a shared protocol
    // the server now performs a pselect
    // on the connected socket, and then uses recvmsg to acquire the message
    // and it needs to switch on the 2 different messages
    // apparently the client program also in order to connect to our unix domain socket
    // it must create its own unix domain socket
    // in the same temporary directory
    // it will then bind to it (creating the file), and then connect it to the server socket
    // the client program should be able to unlink the socket file as soon as it has binded to it
    // the server program should be able to unlink the its socket file as soon as it received a connection
    // ... however, the problem is that we may expecting a privileged invocation
    // this means we may need to reuse the socket, so we cannot delete or even close the socket
    // until we are sure that we have the file descriptor, or we are exiting out of the program completely
    //http://www.techdeviancy.com/uds.html




    // now receive the file descriptor from the mechanism
    // or an error message indicating permission error
    // if we receive a permission error, we must retry with elevated permissions

}
