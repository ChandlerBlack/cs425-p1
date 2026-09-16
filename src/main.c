#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "lab.h"

/*
 * Layer 3: Physical Socket Wrappers
*/

ssize_t socket_read(void *ctx, char *buf, size_t len) {
    int sockfd = *(int *)ctx;
    return recv(sockfd, buf, len, 0);
}

ssize_t socket_write(void *ctx, const char *buf, size_t len) {
    int sockfd = *(int *)ctx;
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t sent = send(sockfd, buf + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            return -1; 
        }
        total_sent += sent;
    }
    return total_sent;
}

int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     
    hints.ai_socktype = SOCK_STREAM; 

    if ((status = getaddrinfo(host, port, &hints, &res)) != 0) {
        fprintf(stderr, "Error: getaddrinfo failed: %s\n", gai_strerror(status));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue; 

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            break; 
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stderr, "Error: Could not connect to %s:%s\n", host, port);
    }
    return sockfd;
}

/*
 * Application Layer
*/

void print_usage() {
    printf("Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
           "          [-H helo-host] <server>\n\n"
           "  -f <from>       envelope sender, for example you@example.com\n"
           "  -t <to>         envelope recipient\n"
           "  -s <subject>    subject line (default: empty)\n"
           "  -b <body>       message body (default: read from stdin)\n"
           "  -p <port>       port or service name (default: 25)\n"
           "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
           "  <server>        host name or address of the mail server\n");
}

char *read_stdin(void) {
    size_t capacity = 1024;
    size_t size = 0;
    char *buf = malloc(capacity);
    if (!buf) return NULL;
    
    int c;
    while ((c = getchar()) != EOF) {
        if (size + 1 >= capacity) {
            capacity *= 2;
            char *new_buf = realloc(buf, capacity);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        buf[size++] = c;
    }
    buf[size] = '\0';
    return buf;
}

int main(int argc, char *argv[]) {
    // Exit 0 when run with no arguments as required by `make leak`
    if (argc == 1) {
        print_usage();
        return 0;
    }

    char *from = NULL;
    char *to = NULL;
    char *subject = "";
    char *body_arg = NULL;
    char *port = "25";
    char *helo_host = "localhost";
    
    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
        switch (opt) {
            case 'f': from = optarg; break;
            case 't': to = optarg; break;
            case 's': subject = optarg; break;
            case 'b': body_arg = optarg; break;
            case 'p': port = optarg; break;
            case 'H': helo_host = optarg; break;
            default:
                print_usage();
                return 1;
        }
    }

    // Validate required arguments and positional server argument
    if (optind >= argc || !from || !to) {
        print_usage();
        return 1;
    }

    char *server = argv[optind];
    char *stdin_body = NULL;
    char *body = body_arg;
    
    // Read from stdin if -b was not provided
    if (!body) {
        stdin_body = read_stdin();
        body = stdin_body ? stdin_body : "";
    }

    // Connect to the SMTP server
    int sockfd = connect_to_server(server, port);
    if (sockfd < 0) {
        if (stdin_body) free(stdin_body);
        return 2;
    }

    // Initialize the Layer 2 session state machine
    smtp_session_t session;
    memset(&session, 0, sizeof(session));
    session.read_fn = socket_read;
    session.write_fn = socket_write;
    session.ctx = &sockfd;

    // Run the transaction
    int exit_code = run_smtp_session(&session, helo_host, from, to, subject, body);

    // Clean up
    close(sockfd);
    if (stdin_body) free(stdin_body);

    return exit_code;
}