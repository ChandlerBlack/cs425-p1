#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "lab.h"

/*
 * Layer 1: Pure Protocol Helpers
*/

bool has_crlf_injection(const char *input) {
    if (!input) return false;
    return (strchr(input, '\r') != NULL || strchr(input, '\n') != NULL);
}

int parse_status_code(const char *line) {
    if (!line || strlen(line) < 3) return -1;
    
    char code_str[4];
    strncpy(code_str, line, 3);
    code_str[3] = '\0';
    
    return atoi(code_str);
}

bool is_final_reply_line(const char *line) {
    size_t len = strlen(line);
    if (len < 3) return false;
    
    if (len >= 4 && line[3] == '-') {
        return false; 
    }
    return true; 
}

char *format_smtp_body(const char *body) {
    if (!body) return NULL;
    
    size_t orig_len = strlen(body);
    size_t extra_chars = 0;
    bool at_bol = true; 
    
    for (size_t i = 0; i < orig_len; i++) {
        if (at_bol && body[i] == '.') extra_chars++;
        if (body[i] == '\n' && (i == 0 || body[i-1] != '\r')) extra_chars++;
        at_bol = (body[i] == '\n');
    }
    
    char *safe_body = malloc(orig_len + extra_chars + 1);
    if (!safe_body) return NULL;
    
    size_t j = 0;
    at_bol = true;
    for (size_t i = 0; i < orig_len; i++) {
        if (at_bol && body[i] == '.') safe_body[j++] = '.';
        if (body[i] == '\n' && (i == 0 || body[i-1] != '\r')) safe_body[j++] = '\r';
        
        safe_body[j++] = body[i];
        at_bol = (body[i] == '\n');
    }
    
    safe_body[j] = '\0';
    return safe_body;
}

/*
 * Layer 2: The Swappable Session Layer
*/

int read_reply_line(smtp_session_t *session, char *line_out, size_t max_len) {
    while (1) {
        ssize_t crlf_pos = -1;
        
        for (size_t i = 0; i + 1 < session->buffer_len; i++) {
            if (session->read_buffer[i] == '\r' && session->read_buffer[i+1] == '\n') {
                crlf_pos = i;
                break;
            }
        }

        if (crlf_pos >= 0) {
            size_t line_len = crlf_pos + 2;
            if (line_len >= max_len) return -1; 

            memcpy(line_out, session->read_buffer, line_len);
            line_out[line_len] = '\0';

            size_t remaining = session->buffer_len - line_len;
            if (remaining > 0) {
                memmove(session->read_buffer, session->read_buffer + line_len, remaining);
            }
            session->buffer_len = remaining;

            return 0; 
        }

        if (session->buffer_len == sizeof(session->read_buffer)) {
            return -1; 
        }

        ssize_t bytes_read = session->read_fn(
            session->ctx, 
            session->read_buffer + session->buffer_len, 
            sizeof(session->read_buffer) - session->buffer_len
        );

        if (bytes_read <= 0) return -1; 
        session->buffer_len += bytes_read;
    }
}

int read_smtp_reply(smtp_session_t *session) {
    char line[1024];
    int code = -1;

    while (1) {
        if (read_reply_line(session, line, sizeof(line)) != 0) {
            return -1; 
        }
        code = parse_status_code(line);
        if (is_final_reply_line(line)) {
            break;
        }
    }
    return code; 
}

// Internal helper to format and send a command securely via the write callback
static int send_command(smtp_session_t *session, const char *fmt, ...) {
    char buf[2048];
    va_list args;
    
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    if (len < 0 || len >= (int)sizeof(buf)) return -1;
    
    size_t total_sent = 0;
    while (total_sent < (size_t)len) {
        ssize_t sent = session->write_fn(session->ctx, buf + total_sent, len - total_sent);
        if (sent <= 0) return -1; 
        total_sent += sent;
    }
    return 0;
}

int run_smtp_session(smtp_session_t *session, const char *helo_host, 
                     const char *from, const char *to, 
                     const char *subject, const char *body) {
    int code;

    // 1. Check initial greeting
    code = read_smtp_reply(session);
    if (code != 220) {
        fprintf(stderr, "Error: Expected 220 greeting, server sent %d\n", code);
        return 2;
    }

    // 2. HELO
    if (send_command(session, "HELO %s\r\n", helo_host) < 0) return 2;
    code = read_smtp_reply(session);
    if (code != 250) {
        fprintf(stderr, "Error: HELO rejected, server sent %d\n", code);
        return 2;
    }

    // 3. MAIL FROM
    if (send_command(session, "MAIL FROM:<%s>\r\n", from) < 0) return 2;
    code = read_smtp_reply(session);
    if (code != 250) {
        fprintf(stderr, "Error: MAIL FROM rejected, server sent %d\n", code);
        return 2;
    }

    // 4. RCPT TO
    if (send_command(session, "RCPT TO:<%s>\r\n", to) < 0) return 2;
    code = read_smtp_reply(session);
    if (code != 250) {
        fprintf(stderr, "Error: RCPT TO rejected, server sent %d\n", code);
        return 2;
    }

    // 5. DATA
    if (send_command(session, "DATA\r\n") < 0) return 2;
    code = read_smtp_reply(session);
    if (code != 354) {
        fprintf(stderr, "Error: DATA command rejected, server sent %d\n", code);
        return 2;
    }

    // 6. Format and send the message body
    char *safe_body = format_smtp_body(body ? body : "");
    if (!safe_body) {
        fprintf(stderr, "Error: Failed to format message body\n");
        return 2;
    }

    if (send_command(session, "From: <%s>\r\n", from) < 0 ||
        send_command(session, "To: <%s>\r\n", to) < 0 ||
        send_command(session, "Subject: %s\r\n\r\n", subject ? subject : "") < 0 ||
        send_command(session, "%s", safe_body) < 0) {
        free(safe_body);
        return 2;
    }

    // Ensure terminal newline before the dot
    if (strlen(safe_body) > 0 && safe_body[strlen(safe_body)-1] != '\n') {
        if (send_command(session, "\r\n") < 0) { free(safe_body); return 2; }
    }
    
    if (send_command(session, ".\r\n") < 0) { free(safe_body); return 2; }
    
    // Free the safely allocated body on the happy path
    free(safe_body);

    code = read_smtp_reply(session);
    if (code != 250) {
        fprintf(stderr, "Error: Message payload rejected, server sent %d\n", code);
        return 2;
    }

    // 7. QUIT
    if (send_command(session, "QUIT\r\n") < 0) return 2;
    code = read_smtp_reply(session);
    if (code != 221) {
        fprintf(stderr, "Error: QUIT command rejected, server sent %d\n", code);
        return 2;
    }

    return 0;
}