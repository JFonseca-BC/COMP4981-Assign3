/*
 * include/handler.h
 * Interface for the dynamic HTTP request handler.
 */

#ifndef HANDLER_H
#define HANDLER_H

/* * Processes the HTTP request from the given client socket file descriptor.
 * Returns 0 on success, or a negative value on failure.
 */
int handle_request(int client_fd);

#endif /* HANDLER_H */
