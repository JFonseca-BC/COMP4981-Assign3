/*
 * src/handler.c
 * Shared library for HTTP request handling (GET, HEAD, POST).
 * Compile with: gcc -shared -fPIC -o include/handler.so src/handler.c
 */

#include "../include/handler.h"
#include <fcntl.h>
#include <ndbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h> /* For recv(), send() */
#include <sys/stat.h>
#include <time.h> /* For time() */
#include <unistd.h>

/* --- Named Constants --- */
enum HandlerConstants {
  BUFFER_SIZE = 4096,
  METHOD_SIZE = 16,
  PATH_SIZE = 256,
  PROTOCOL_SIZE = 16,
  FILEPATH_SIZE = 512,
  KEY_SIZE = 64,
  DB_FILENAME_SIZE = 64,
  DB_PERMS = 0666,
  CRLF_LEN = 4,
  HTTP_OK = 200,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_INTERNAL_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  MSG_LEN_13 = 13
};

/* --- Internal Helper Prototypes --- */
static const char *get_mime_type(const char *path);
static void send_header(int client_fd, int status, const char *status_text,
                        const char *mime, size_t length);

/* Map file extensions to MIME types */
static const char *get_mime_type(const char *path) {
  if (strstr(path, ".html") != NULL) {
    return "text/html";
  }
  if (strstr(path, ".jpg") != NULL || strstr(path, ".jpeg") != NULL) {
    return "image/jpeg";
  }
  if (strstr(path, ".png") != NULL) {
    return "image/png";
  }
  if (strstr(path, ".gif") != NULL) {
    return "image/gif";
  }
  return "text/plain";
}

/* Send strict RFC-compliant HTTP response headers */
static void send_header(int client_fd, int status, const char *status_text,
                        const char *mime, size_t length) {
  char header[BUFFER_SIZE];
  (void)snprintf(header, sizeof(header),
                 "HTTP/1.0 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 status, status_text, mime, length);
  (void)send(client_fd, header, strlen(header), 0);
}

int handle_request(int client_fd) {
  char buffer[BUFFER_SIZE] = {0};
  ssize_t bytes_read;
  char method[METHOD_SIZE];
  char path[PATH_SIZE];
  char protocol[PROTOCOL_SIZE];

  bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
  if (bytes_read <= 0) {
    return -1;
  }

  if (sscanf(buffer, "%15s %255s %15s", method, path, protocol) < 2) {
    return -1;
  }

  /* Security: Prevent Directory Traversal */
  if (strstr(path, "..") != NULL) {
    send_header(client_fd, HTTP_FORBIDDEN, "Forbidden", "text/plain",
                (size_t)MSG_LEN_13);
    (void)send(client_fd, "403 Forbidden", (size_t)MSG_LEN_13, 0);
    return 0;
  }

  if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
    char filepath[FILEPATH_SIZE];
    struct stat st;
    int file_fd;
    const char *mime;

    /* Safely format the filepath without using insecure strcat */
    if (strcmp(path, "/") == 0) {
      (void)snprintf(filepath, sizeof(filepath), "./index.html");
    } else {
      (void)snprintf(filepath, sizeof(filepath), ".%s", path);
    }

    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode) != 0) {
      send_header(client_fd, HTTP_NOT_FOUND, "Not Found", "text/plain",
                  (size_t)MSG_LEN_13);
      if (strcmp(method, "GET") == 0) {
        (void)send(client_fd, "404 Not Found", (size_t)MSG_LEN_13, 0);
      }
      return 0;
    }

    /* Enforce O_CLOEXEC for security rules */
    file_fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
      send_header(client_fd, HTTP_FORBIDDEN, "Forbidden", "text/plain",
                  (size_t)MSG_LEN_13);
      return 0;
    }

    mime = get_mime_type(filepath);
    send_header(client_fd, HTTP_OK, "OK", mime, (size_t)st.st_size);

    if (strcmp(method, "GET") == 0) {
      char file_buf[BUFFER_SIZE];
      ssize_t n;
      while ((n = read(file_fd, file_buf, sizeof(file_buf))) > 0) {
        (void)send(client_fd, file_buf, (size_t)n, 0);
      }
    }
    (void)close(file_fd);

  } else if (strcmp(method, "POST") == 0) {
    /* Isolate the body for ndbm storage */
    char *body = strstr(buffer, "\r\n\r\n");
    if (body != NULL) {
      DBM *db = NULL;
      char db_filename[DB_FILENAME_SIZE];

      /* Populate at runtime to satisfy both cppcheck and strict GCC constraints
       */
      (void)strncpy(db_filename, "post_database", sizeof(db_filename) - 1);
      db_filename[sizeof(db_filename) - 1] = '\0';

      body += CRLF_LEN; /* Skip CRLFCRLF */

      db = dbm_open(db_filename, O_RDWR | O_CREAT, DB_PERMS);
      if (db != NULL) {
        char key_str[KEY_SIZE];
        datum key;
        datum value;
        const char *msg_ok = "Data stored successfully.";
        size_t msg_ok_len = strlen(msg_ok);

        (void)snprintf(key_str, sizeof(key_str), "%ld", (long)time(NULL));

        key.dptr = key_str;
        key.dsize = (int)strlen(key_str);

        value.dptr = body;
        value.dsize = (int)strlen(body);

        (void)dbm_store(db, key, value, DBM_REPLACE);
        dbm_close(db);

        send_header(client_fd, HTTP_OK, "OK", "text/plain", msg_ok_len);
        (void)send(client_fd, msg_ok, msg_ok_len, 0);
      } else {
        const char *msg_err = "Database Error.";
        size_t msg_err_len = strlen(msg_err);
        send_header(client_fd, HTTP_INTERNAL_ERROR, "Internal Server Error",
                    "text/plain", msg_err_len);
        (void)send(client_fd, msg_err, msg_err_len, 0);
      }
    }
  } else {
    const char *msg_not_impl = "501 Not Implemented";
    size_t msg_not_impl_len = strlen(msg_not_impl);
    send_header(client_fd, HTTP_NOT_IMPLEMENTED, "Not Implemented",
                "text/plain", msg_not_impl_len);
    (void)send(client_fd, msg_not_impl, msg_not_impl_len, 0);
  }

  return 0;
}
