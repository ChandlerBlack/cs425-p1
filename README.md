# CS 425 Project 1: C SMTP Client
**Author:** Chandler Black 

## Project Overview
This project implements a fully functional, robust SMTP (Simple Mail Transfer Protocol) client in C. The client is capable of parsing command-line arguments, establishing TCP connections, and speaking the SMTP protocol to deliver email payloads. 

To ensure high reliability and testability, the application is built using a strict three-layer architecture. This separation of concerns allows the core protocol logic to be tested in complete isolation from the physical network socket.

---

## The Three-Layer Design

### 1. Pure Protocol Helpers (Layer 1)
Located in `lab.c`, this layer consists of stateless helper functions that deal exclusively with strings and memory. 
Formatting the message body (e.g., dot-stuffing and bare LF conversion), detecting malicious CRLF injections, and parsing server status codes.
By keeping these functions entirely unaware of sockets or connection states, they can be instantly verified using simple unit tests.

### 2. Swappable Session Layer (Layer 2)
This is the core state machine of the SMTP client, also located in `lab.c`. 
Executing the sequence of SMTP commands (`HELO`, `MAIL FROM`, `RCPT TO`, `DATA`, `QUIT`) and validating the server's responses.
 Instead of hardcoding `send()` and `recv()`, this layer uses dependency injection via the `smtp_session_t` struct. It reads and writes data using `read_fn` and `write_fn` function pointers. This allows the application to swap out the underlying transport mechanism without changing a single line of protocol logic.

### 3. Application & Network Layer (Layer 3)
Located in `main.c`, this layer connects the pure logic to the real world.
Parsing `getopt` CLI arguments, resolving the hostname via `getaddrinfo`, opening the actual TCP socket, and managing standard input for the message payload.
It maps the real socket operations to the function pointers expected by Layer 2 and kicks off the session state machine. 

---

## Testing & Verification
Because of the decoupled three-layer design, the core application was rigorously tested without requiring a live SMTP server.
*   **Mock Transport:** The Unity test suite (`lab-test.c`) injects custom `mock_read` and `mock_write` callbacks. 
