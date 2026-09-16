#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/*
 * Layer 1: Pure Protocol Helpers
 * These functions perform no I/O. They manipulate and inspect strings,
 * making them fully testable without any mock transport state.
*/

/**
 * Checks if the input contains a bare CR or LF to prevent header injection.
 * Returns true if an injection attempt is found, false otherwise.
 */
bool has_crlf_injection(const char *input);

/**
 * Parses the 3-digit status code from a reply line.
 * Returns the integer code (e.g., 250), or -1 on parsing failure.
 */
int parse_status_code(const char *line);

/**
 * Checks if the line has a space at index 3 (e.g., "250 Ok"),
 * indicating it is the final line in a multi-line reply.
 */
bool is_final_reply_line(const char *line);

/**
 * Replaces leading periods with double periods (dot-stuffing) and 
 * ensures all lines end with \r\n. 
 * Returns a dynamically allocated string. Caller must free().
 */
char *format_smtp_body(const char *body);


/*
 * Layer 2: The Swappable Session Layer
 * I/O is abstracted through callbacks. This state machine can be driven 
 * by physical sockets in production or a mock buffer in Unity tests.
*/

// Function pointer signatures for I/O abstraction
typedef ssize_t (*read_fn_t)(void *ctx, char *buf, size_t len);
typedef ssize_t (*write_fn_t)(void *ctx, const char *buf, size_t len);

// Stateful session structure
typedef struct {
    read_fn_t read_fn;        // Callback for reading data
    write_fn_t write_fn;      // Callback for writing data
    void *ctx;                // Context: int* for sockets, test struct for Unity

    char read_buffer[4096];   // Persistent buffer for handling partial reads
    size_t buffer_len;        // Current number of valid bytes in read_buffer
} smtp_session_t;

/**
 * Reads from the session buffer/callback until it extracts a single \r\n 
 * terminated line. Safely shifts remaining bytes in the buffer.
 * Returns 0 on success, -1 on error.
 */
int read_reply_line(smtp_session_t *session, char *line_out, size_t max_len);

/**
 * Loops read_reply_line() to handle multi-line SMTP replies.
 * Returns the parsed 3-digit status code, or -1 on network error.
 */
int read_smtp_reply(smtp_session_t *session);

/**
 * Drives the complete SMTP transaction (HELO through QUIT).
 * Returns 0 on success, 2 if the protocol sequence or connection fails.
 */
int run_smtp_session(smtp_session_t *session, const char *helo_host, 
                     const char *from, const char *to, 
                     const char *subject, const char *body);

#endif // LAB_H