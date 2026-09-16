#include "harness/unity.h"
#include "../src/lab.h"
#include <string.h>
#include <stdlib.h>

/*
  Mock Transport Context and Callbacks
*/

// Replaces the socket file descriptor for tests.
typedef struct {
    const char *server_input;      // Simulated data sent from the server
    size_t input_len;              // Length of simulated data
    size_t input_pos;              // Current read position

    char client_output[4096];      // Captured data sent by our client
    size_t output_len;             // Length of captured data
} test_ctx_t;

// Mock recv() - feeds predefined strings to the client
ssize_t mock_read(void *ctx, char *buf, size_t len) {
    test_ctx_t *tc = (test_ctx_t *)ctx;
    if (tc->input_pos >= tc->input_len) {
        return 0; // EOF (Server disconnected)
    }
    
    size_t available = tc->input_len - tc->input_pos;
    size_t to_copy = (available < len) ? available : len;
    
    memcpy(buf, tc->server_input + tc->input_pos, to_copy);
    tc->input_pos += to_copy;
    
    return to_copy;
}

// Mock send() - captures the client's output for verification
ssize_t mock_write(void *ctx, const char *buf, size_t len) {
    test_ctx_t *tc = (test_ctx_t *)ctx;
    if (tc->output_len + len < sizeof(tc->client_output)) {
        memcpy(tc->client_output + tc->output_len, buf, len);
        tc->output_len += len;
        tc->client_output[tc->output_len] = '\0'; // Null-terminate for easy printing/asserts
        return len;
    }
    return -1; // Buffer overflow in test
}

// Global test variables
test_ctx_t test_ctx;
smtp_session_t session;

void setUp(void) {
    memset(&test_ctx, 0, sizeof(test_ctx));
    memset(&session, 0, sizeof(session));
    
    session.read_fn = mock_read;
    session.write_fn = mock_write;
    session.ctx = &test_ctx;
}

void tearDown(void) {
    // Nothing dynamic to clean up in the test runner itself
}

/* 
  Layer 1 Tests: Pure Protocol Helpers
*/

void test_has_crlf_injection(void) {
    TEST_ASSERT_TRUE(has_crlf_injection("Bad\rHeader"));
    TEST_ASSERT_TRUE(has_crlf_injection("Bad\nHeader"));
    TEST_ASSERT_FALSE(has_crlf_injection("Good Header"));
    TEST_ASSERT_FALSE(has_crlf_injection(NULL));
}

void test_parse_status_code(void) {
    TEST_ASSERT_EQUAL(250, parse_status_code("250 Ok"));
    TEST_ASSERT_EQUAL(354, parse_status_code("354-Start mail input"));
    TEST_ASSERT_EQUAL(-1, parse_status_code("22")); // Too short
    TEST_ASSERT_EQUAL(-1, parse_status_code(NULL));
}

void test_is_final_reply_line(void) {
    TEST_ASSERT_TRUE(is_final_reply_line("250 Ok"));
    TEST_ASSERT_TRUE(is_final_reply_line("220\r\n"));
    TEST_ASSERT_FALSE(is_final_reply_line("250-SIZE 1024"));
    TEST_ASSERT_FALSE(is_final_reply_line("22")); // Too short
}

void test_format_smtp_body_standard(void) {
    char *result = format_smtp_body("Hello\r\nWorld");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("Hello\r\nWorld", result);
    free(result);
}

void test_format_smtp_body_dot_stuffing(void) {
    // Tests dot-stuffing at the very beginning and after a newline
    char *result = format_smtp_body(".Hello\n.World");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("..Hello\r\n..World", result);
    free(result);
}

void test_format_smtp_body_bare_lf(void) {
    // Tests injecting \r before a bare \n
    char *result = format_smtp_body("Line 1\nLine 2");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("Line 1\r\nLine 2", result);
    free(result);
}

/*
  Layer 2 Tests: Swappable Session Layer
 */

void test_read_reply_line(void) {
    test_ctx.server_input = "250 OK\r\n250 Next\r\n";
    test_ctx.input_len = strlen(test_ctx.server_input);
    
    char line[100];
    
    // First read should get "250 OK\r\n"
    int res = read_reply_line(&session, line, sizeof(line));
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL_STRING("250 OK\r\n", line);
    
    // Buffer should now contain the start of the next line.
    // Second read gets the rest.
    res = read_reply_line(&session, line, sizeof(line));
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL_STRING("250 Next\r\n", line);
}

void test_read_smtp_reply_multiline(void) {
    test_ctx.server_input = "250-First line\r\n250-Second line\r\n250 Final line\r\n";
    test_ctx.input_len = strlen(test_ctx.server_input);
    
    int code = read_smtp_reply(&session);
    
    // It should loop until the final line and return 250
    TEST_ASSERT_EQUAL(250, code);
}

/* 
  Session State Machine Tests
 */

void test_run_smtp_session_happy_path(void) {
    // Provide the complete expected server dialogue
    test_ctx.server_input = 
        "220 smtp.example.com ESMTP\r\n"
        "250 Hello\r\n"
        "250 Ok\r\n"
        "250 Ok\r\n"
        "354 End data with .\r\n"
        "250 Ok: queued\r\n"
        "221 Bye\r\n";
    test_ctx.input_len = strlen(test_ctx.server_input);

    int exit_code = run_smtp_session(&session, "localhost", "me@a.com", "you@b.com", "Hi", "Message");
    
    TEST_ASSERT_EQUAL(0, exit_code);
    
    // Verify our client sent the correct commands in order
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "HELO localhost\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "MAIL FROM:<me@a.com>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "RCPT TO:<you@b.com>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "DATA\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "Message\r\n.\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(test_ctx.client_output, "QUIT\r\n"));
}

void test_run_smtp_session_fails_on_bad_greeting(void) {
    test_ctx.server_input = "500 Service unavailable\r\n";
    test_ctx.input_len = strlen(test_ctx.server_input);

    int exit_code = run_smtp_session(&session, "localhost", "me@a.com", "you@b.com", "Hi", "Message");
    
    // Must return 2 on protocol error
    TEST_ASSERT_EQUAL(2, exit_code);
}

void test_run_smtp_session_fails_and_cleans_up_on_data_rejection(void) {
    test_ctx.server_input = 
        "220 smtp.example.com ESMTP\r\n"
        "250 Hello\r\n"
        "250 Ok\r\n"
        "250 Ok\r\n"
        "554 Transaction failed\r\n"; // Reject the DATA command
    test_ctx.input_len = strlen(test_ctx.server_input);

    int exit_code = run_smtp_session(&session, "localhost", "me@a.com", "you@b.com", "Hi", "Message");
    
    TEST_ASSERT_EQUAL(2, exit_code);
}

/*
  Main Test Runner
 */

int main(void) {
    UNITY_BEGIN();
    
    // Layer 1
    RUN_TEST(test_has_crlf_injection);
    RUN_TEST(test_parse_status_code);
    RUN_TEST(test_is_final_reply_line);
    RUN_TEST(test_format_smtp_body_standard);
    RUN_TEST(test_format_smtp_body_dot_stuffing);
    RUN_TEST(test_format_smtp_body_bare_lf);
    
    // Layer 2
    RUN_TEST(test_read_reply_line);
    RUN_TEST(test_read_smtp_reply_multiline);
    
    // State Machine
    RUN_TEST(test_run_smtp_session_happy_path);
    RUN_TEST(test_run_smtp_session_fails_on_bad_greeting);
    RUN_TEST(test_run_smtp_session_fails_and_cleans_up_on_data_rejection);
    
    return UNITY_END();
}