/*
 * src/server.c
 * Pre-forked HTTP 1.0 Server with Dynamic Library Loading
 * Usage: ./server -p <port> -w <workers>
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIB_PATH "./include/handler.so"

/* --- Constants --- */
enum ServerConstants {
  MAX_PORT = 65535,
  LISTEN_BACKLOG = 128,
  TIME_STR_SIZE = 64,
  BASE_TEN = 10
};

/* NOLINT below is required as sig_atomic_t MUST be mutated globally by signal
 * handlers */
/* NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) */
static volatile sig_atomic_t keep_running = 1;

/* --- Function Prototypes --- */
static void handle_sigint(int sig);
static void log_message(const char *level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void worker_loop(int server_fd) __attribute__((noreturn));

/* --- Signal Handlers --- */
static void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

/* --- Logging Function --- */
static void log_message(const char *level, const char *format, ...) {
  time_t now = time(NULL);
  struct tm t_buf;
  char time_str[TIME_STR_SIZE];
  va_list args;

  (void)localtime_r(&now, &t_buf);
  (void)strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &t_buf);

  (void)fprintf(stderr, "[%s] [PID: %d] [%s] ", time_str, getpid(), level);

  va_start(args, format);
  (void)vfprintf(stderr, format, args);
  va_end(args);

  (void)fprintf(stderr, "\n");
}

/* --- Worker Process Loop --- */
static void worker_loop(int server_fd) {
  void *lib_handle = NULL;
  int (*handle_req)(int) = NULL;
  time_t lib_mtime = 0;

  log_message("INFO", "Worker started and waiting for connections.");

  while (keep_running != 0) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    struct stat lib_stat;

    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_message("ERROR", "Accept failed");
      continue;
    }

    /* Check for library updates via stat */
    if (stat(LIB_PATH, &lib_stat) == 0) {
      if (lib_stat.st_mtime > lib_mtime || lib_handle == NULL) {
        if (lib_handle != NULL) {
          (void)dlclose(lib_handle);
          log_message("INFO", "Reloading updated shared library.");
        }

        lib_handle = dlopen(LIB_PATH, RTLD_NOW);
        if (lib_handle != NULL) {
          /* Clear any existing errors */
          (void)dlerror();
          /* Disable strict pedantic warning for dlsym cast */
          *(void **)(&handle_req) = dlsym(lib_handle, "handle_request");

          if (handle_req != NULL) {
            lib_mtime = lib_stat.st_mtime;
          } else {
            log_message("ERROR", "dlsym failed: %s", dlerror());
          }
        } else {
          log_message("ERROR", "dlopen failed: %s", dlerror());
        }
      }
    }

    /* Process request if library is loaded successfully */
    if (handle_req != NULL) {
      (void)handle_req(client_fd);
    } else {
      const char *err_resp = "HTTP/1.0 500 Internal Server Error\r\n\r\nServer "
                             "configuration error.\r\n";
      (void)send(client_fd, err_resp, strlen(err_resp), 0);
      log_message("ERROR", "Cannot handle request, library not loaded.");
    }

    (void)close(client_fd);
  }

  if (lib_handle != NULL) {
    (void)dlclose(lib_handle);
  }
  exit(EXIT_SUCCESS);
}

/* --- Main Event Loop --- */
int main(int argc, char *argv[]) {
  int opt;
  long port_in = 0;
  long num_workers = 0;
  int server_fd = -1;
  struct sigaction sa;
  int reuse = 1;
  struct sockaddr_in server_addr = {0};
  int i;

  while ((opt = getopt(argc, argv, "p:w:")) != -1) {
    switch (opt) {
    case 'p':
      port_in = strtol(optarg, NULL, BASE_TEN);
      break;
    case 'w':
      num_workers = strtol(optarg, NULL, BASE_TEN);
      break;
    default:
      (void)fprintf(stderr, "Usage: %s -p <port> -w <workers>\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  /* If arguments are missing or invalid, print usage and fail */
  if (port_in <= 0 || port_in > MAX_PORT || num_workers <= 0) {
    (void)fprintf(stderr, "Usage: %s -p <port> -w <workers>\n", argv[0]);
    return EXIT_FAILURE;
  }

  /* Setup Graceful Shutdown */
  
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_handler = handle_sigint;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  (void)sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGINT, &sa, NULL);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return EXIT_FAILURE;
  }

  (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons((uint16_t)port_in);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("Bind failed");
    (void)close(server_fd);
    return EXIT_FAILURE;
  }

  if (listen(server_fd, LISTEN_BACKLOG) < 0) {
    perror("Listen failed");
    (void)close(server_fd);
    return EXIT_FAILURE;
  }

  log_message("INFO", "Server listening on port %ld with %ld workers.", port_in,
              num_workers);

  /* Pre-fork Workers */
  for (i = 0; i < num_workers; i++) {
    if (fork() == 0) {
      worker_loop(server_fd);
    }
  }

  /* Parent Process Monitor */
  while (keep_running != 0) {
    int status;
    pid_t dead_pid = waitpid(-1, &status, 0);

    if (dead_pid > 0 && keep_running != 0) {
      log_message("WARN", "Worker %d terminated unexpectedly. Restarting...",
                  dead_pid);
      if (fork() == 0) {
        worker_loop(server_fd);
      }
    }
  }

  log_message("INFO", "Shutting down parent process. Terminating workers...");
  (void)kill(0, SIGINT); /* Forward signal to process group */
  while (waitpid(-1, NULL, 0) > 0) {
    /* Wait for all children to die */
  }

  (void)close(server_fd);
  return EXIT_SUCCESS;
}
