// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>
#include <signal.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define MAX_CONNECTIONS 1024
static struct connection *active_connections[MAX_CONNECTIONS] = {NULL};

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

// static io_context_t ctx;

/* global flag to keep the server running */
static volatile sig_atomic_t server_running = 1;

/* signal handler for Ctrl+C */
static void handle_sigint(int signum)
{
	(void)signum; // Suppress unused warning
	server_running = 0;
}

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	dlog(LOG_INFO, "[DEBUG] on_path_cb. buffer %s, req_path: '%s'\n", buf, conn->request_path);

	return 0;
}

// Prepares the connection buffer to send the reply header.
static void connection_prepare_send_reply_header(struct connection *conn)
{
	conn->state = STATE_SENDING_HEADER;
	
	/* build the header to include file size and connection status */
	sprintf(conn->send_buffer, 
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: %ld\r\n"
		"Connection: keep-alive\r\n"
		"\r\n", 
		conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
}

// Prepares the connection buffer to send the reply header.
// static void connection_prepare_send_reply_header(struct connection *conn)
// {
// 	conn->state = STATE_SENDING_HEADER;
// 	char *header = HTTP_FOUND_MSG;

// 	size_t len = strlen(header);

// 	if (len >= sizeof(conn->send_buffer))
// 		len = sizeof(conn->send_buffer) - 1;

// 	memcpy(conn->send_buffer, header, len);
// 	conn->send_buffer[len] = '\0';

// 	conn->send_len = strlen(conn->send_buffer);
// }


// Prepares the connection buffer to send the 404 header.
static void connection_prepare_send_404(struct connection *conn)
{
	strcpy(conn->send_buffer, HTTP_NOT_FOUND_MSG);
	conn->send_len = strlen(HTTP_NOT_FOUND_MSG);
	conn->send_pos = 0;
	conn->state = STATE_SENDING_404;
}


/* Gets resource type depending on request path/filename. Filename should
 * point to the static or dynamic folder.
 */
static enum resource_type connection_get_resource_type(struct connection *conn)
{
	conn->res_type = RESOURCE_TYPE_NONE;
	if (strstr(conn->request_path, "dynamic"))
		conn->res_type = RESOURCE_TYPE_DYNAMIC;

	if (strstr(conn->request_path, "static"))
		conn->res_type = RESOURCE_TYPE_STATIC;

	strcpy(conn->filename, ".");
	strncat(conn->filename + 1, conn->request_path, BUFSIZ - 2);

	dlog(LOG_INFO, "[DEBUG] Determined Type: %d, Filename: '%s'\n", conn->res_type, conn->filename);
	return conn->res_type;
}


// Initialize connection structure on given socket
struct connection *connection_create(int sockfd)
{
	int rc;
	struct connection *conn = calloc(1, sizeof(struct connection));

	// socket data
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;

	memset(conn->recv_buffer, 0, BUFSIZ);
	conn->recv_len = 0;
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;
	conn->send_pos = 0;

	// async init
	conn->ctx = 0;
	rc = io_setup(1, &conn->ctx);
	DIE(rc < 0, "io setup failed");

	conn->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	DIE(conn->eventfd < 0, "eventfd failed");

	w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);

	return conn;
}


/* Starts asynchronous operation (read from file).
 * Uses io_submit(2) & friends for reading data asynchronously.
 */
void connection_start_async_io(struct connection *conn)
{
	int rc;

	memset(&conn->iocb, 0, sizeof(conn->iocb));
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer,
		BUFSIZ, conn->file_pos);

	io_set_eventfd(&conn->iocb, conn->eventfd);
	conn->piocb[0] = &conn->iocb;

	rc = io_submit(conn->ctx, 1, conn->piocb);
	DIE(rc < 0, "io_submit failed");

	conn->state = STATE_ASYNC_ONGOING;
}


// Removes connection handler.
void connection_remove(struct connection *conn)
{
	if (conn->sockfd < MAX_CONNECTIONS) {
		active_connections[conn->sockfd] = NULL;
	}

	shutdown(conn->sockfd, SHUT_RDWR);

	if (conn->sockfd)
		close(conn->sockfd);
	if (conn->ctx)
		io_destroy(conn->ctx);
	if (conn->eventfd > 0)
		close(conn->eventfd);
	if (conn->fd > 0)
		close(conn->fd);

	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}


// Handles a new connection request on the server socket
void handle_new_connection(void)
{
	int socketfd;
	socklen_t addresslen = sizeof(struct sockaddr_in);
	struct sockaddr_in address;
	struct connection *conn;
	int rc;

	// accept new connection
	socketfd = accept(listenfd, (SSA *) &address, &addresslen);
	DIE(socketfd < 0, "accpet failed");

	// set socket to be non-blocking (taken from stack overflow)
	rc = fcntl(socketfd, F_SETFL, fcntl(socketfd, F_GETFL, 0) | O_NONBLOCK);
	DIE(rc < 0, "set socket to non-blocking failed");

	// instantiate new connection handler.
	conn = connection_create(socketfd);

	// add socket to epoll
	w_epoll_add_ptr_in(epollfd, socketfd, (void *)conn);

	if (socketfd < MAX_CONNECTIONS) {
		active_connections[socketfd] = conn;
	}

	// initialize HTTP_REQUEST parser
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}


/* Receives message on socket.
 * Stores message in recv_buffer in struct connection.
 */
void receive_data(struct connection *conn)
{
	int rc;
	char buffer[BUFSIZ];

	memset(buffer, 0, BUFSIZ);

	// read into intermediary buffer
	rc = recv(conn->sockfd, buffer, BUFSIZ, 0);

	// error cheks
	// way too much data was given. could have a buffer overflow if not careful
	// (since i just use memcpy not memncpy)
	if (rc + conn->recv_len > BUFSIZ) {
		ERR("recv failed, payload too large");
		return;
	}

	// compare revc return value
	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		ERR("recv failed");
		return;
	}

	if (rc == 0) {
		connection_remove(conn);
		return;
	}

	dlog(LOG_INFO, "[Debug] recv read %d bytes. Content start: '%.20s'\n", rc, buffer);
	dlog(LOG_INFO, "[Debug] recv_len before copy: %zu\n", conn->recv_len);

	int recv_size = rc;
	// rc > 0
	conn->state = STATE_RECEIVING_DATA;
	if (strstr(buffer, "\r\n\r\n")) {
		conn->state = STATE_REQUEST_RECEIVED;

		// tell epoll to write
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_inout failed");
	}

	dlog(LOG_INFO, "[Debug] Copying to recv_buffer at index %zu this many bytes %d\n", conn->recv_len, recv_size);
	memcpy(conn->recv_buffer + conn->recv_len, buffer, recv_size);
	conn->recv_len += recv_size;

	conn->recv_buffer[conn->recv_len] = '\0';

	dlog(LOG_INFO, "[Debug] New recv_len: %zu. Buffer start: '%.10s'\n", conn->recv_len, conn->recv_buffer);
}


// Opens file and updates connection fields.
int connection_open_file(struct connection *conn)
{
	if (conn == NULL || conn->filename[0] == '\0') {
		dlog(LOG_INFO, "invalid file for open_file\n");
		return -1;
	}


	conn->fd = open(conn->filename, O_RDONLY | O_NONBLOCK);
	if (conn->fd < 0) {
		ERR("failed to open conn file");
		return -1;
	}

	struct stat stat_buffer;

	if (fstat(conn->fd, &stat_buffer) < 0) {
		ERR("fstat open file failed");
		close(conn->fd);
		return -1;
	}
	conn->file_size = stat_buffer.st_size;

	return 0;
}


/* Complete asynchronous operation; operation returns successfully.
 * Prepares socket for sending.
 */
void connection_complete_async_io(struct connection *conn)
{
	int rc;
	struct io_event event;

	rc = io_getevents(conn->ctx, 1, 1, &event, NULL);
	DIE(rc < 0, "io_getevents failed");
	DIE(event.res < 0, "invalid read size");

	// change cursors
	conn->send_len = event.res;
	conn->send_pos = 0;
	conn->file_pos += event.res;

	conn->state = STATE_SENDING_DATA;
}


/* Parses the HTTP header and extracts the file path.
 * Uses mostly null settings except for on_path callback.
 */
int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	if (conn == NULL) {
		ERR("parse header null conn");
		return -1;
	}

	dlog(LOG_INFO, "[DEBUG] Parsing Buffer (len=%zu):\n'%s'\n", strlen(conn->recv_buffer), conn->recv_buffer);

	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
	http_parser_execute(&conn->request_parser, &settings_on_path,
		conn->recv_buffer, strlen(conn->recv_buffer));
	return 0;
}


// Sends static data using sendfile(2).
enum connection_state connection_send_static(struct connection *conn)
{
	conn->state = STATE_SENDING_DATA;

	int rc;

	rc = sendfile(conn->sockfd, conn->fd, (off_t *)&conn->file_pos, conn->file_size - conn->file_pos);

	// err check
	if (rc < 0) {
		// check if socket can be used. try again later
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return STATE_SENDING_DATA;
		dlog(LOG_INFO, "sendfile failed\n");
		conn->state = STATE_CONNECTION_CLOSED;
		return STATE_CONNECTION_CLOSED;
	}

	if (conn->file_pos == conn->file_size) {
		// reset connecton once file is done sending
		rc = close(conn->fd);
		DIE(rc < 0, "cannot close fd conn_send_static");

		/* reset the cursors */
		conn->state = STATE_INITIAL;
		conn->recv_len = 0;
		conn->send_pos = 0;
		conn->send_len = 0;
		conn->file_pos = 0;
		
		/* Clear the buffers*/
		memset(conn->recv_buffer, 0, BUFSIZ);
		memset(conn->send_buffer, 0, BUFSIZ);

		/* Tell epoll to switch back to listening for a new HTTP request on this same socket */
		w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
		
		return STATE_INITIAL;
	}

	return conn->state;
}


int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	return -1;
}


/* Reads data asynchronously.
 * Returns 0 on success and -1 on error.
 */
int connection_send_dynamic(struct connection *conn)
{
	int rc;

	// program has already handled all of the data
	if (conn->send_pos == conn->send_len) {
		// no more new data to handle
		if (conn->file_pos == conn->file_size) {
			conn->state = STATE_DATA_SENT;
			connection_remove(conn);
			return 0;
		}

		// need to read more
		connection_start_async_io(conn);
	}

	// there's data to send
	if (conn->send_pos < conn->send_len) {
		rc = send(conn->sockfd, conn->send_buffer + conn->send_pos,
						  conn->send_len - conn->send_pos, 0);

		if (rc < 0) {
			// check if socket can't currently be used
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			// actual error
			return -1;
		}

		conn->send_pos += rc;
	}

	return 0;
}


/* Handles input information: may be a new message or notification of
 * completion of an asynchronous I/O operation.
 */
void handle_input(struct connection *conn)
{
	dlog(LOG_INFO, "input state %d\n", conn->state);
	switch (conn->state) {
	case STATE_INITIAL: // state 0
		receive_data(conn);
		break;
	case STATE_RECEIVING_DATA: // state 1
		receive_data(conn);
		break;
	case STATE_CONNECTION_CLOSED: // state 10
		connection_remove(conn);
		break;
	// async op done, can read more into buffers
	case STATE_ASYNC_ONGOING: // state 6
		handle_output(conn);
		break;
	default:
		printf("shouldn't get here %d\n", conn->state);
		exit(1);
	}
}


/* Handles output information: may be a new valid requests or notification of
 * completion of an asynchronous I/O operation or invalid requests.
 */
void handle_output(struct connection *conn)
{
	int rc;
	uint64_t read_buffer;

	dlog(LOG_INFO, "entering output swtich with state %d\n", conn->state);
	switch (conn->state) {
	case STATE_REQUEST_RECEIVED: // state 2
		parse_header(conn);
		conn->res_type = connection_get_resource_type(conn);

		rc = connection_open_file(conn);

		// check if file found
		if (rc < 0) {
			dlog(LOG_INFO, "File not found: %s\n", conn->filename);
			connection_prepare_send_404(conn);
		}
		else
			connection_prepare_send_reply_header(conn);

		break;

	case STATE_SENDING_DATA: // state 3
	switch (conn->res_type) {
	case RESOURCE_TYPE_STATIC:
		connection_send_static(conn);
		break;

	case RESOURCE_TYPE_DYNAMIC:
		connection_send_dynamic(conn);
		break;

	default:
		ERR("unknown resource type");
		connection_remove(conn);
		break;
	}
	break;

	case STATE_SENDING_HEADER: // state 4
		rc = send(conn->sockfd, conn->send_buffer + conn->send_pos,
			conn->send_len - conn->send_pos, 0);

		// err checks
		if (rc < 0) {
			// check if socket can be used. try again later
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;

			// socket isn't full, there was an actual error
			ERR("ok header send failed");
			connection_remove(conn);
			break;
		}

		conn->send_pos += rc;
		if (conn->send_pos == conn->send_len) {
			// move on to sending data
			conn->state = STATE_SENDING_DATA;
		}
		break;

	case STATE_SENDING_404: // state 5
		rc = send(conn->sockfd, conn->send_buffer + conn->send_pos,
			conn->send_len - conn->send_pos, 0);

		// err checks
		if (rc < 0) {
			// check if socket can be used. try again later
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;

			// socket isn't full, there was an actual error
			ERR("404 header send failed");
			connection_remove(conn);
			break;
		}

		conn->send_pos += rc;
		if (conn->send_pos == conn->send_len) {
			conn->state = STATE_404_SENT;
			connection_remove(conn);
		}
		break;

	case STATE_ASYNC_ONGOING: // state 6
		rc = read(conn->eventfd, &read_buffer, sizeof(read_buffer));
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		DIE(rc < 0, "eventfd read failed");

		connection_complete_async_io(conn);
		connection_send_dynamic(conn);
		break;

	default:
		ERR("Unexpected state handle output\n");
		connection_remove(conn);
		break;
	}
}


/* Handle new client. There can be input and output connections.
 * Take care of what happened at the end of a connection.
 */
void handle_client(uint32_t event, struct connection *conn)
{
	if (event & EPOLLHUP)
		connection_remove(conn);
	else if (event & EPOLLIN)
		handle_input(conn);
	else if (event & EPOLLOUT)
		handle_output(conn);
	else
		printf("this shouldn't happen %d\n", event);
		// ERR("this shouldn't happen\n");
}


int main(void)
{
	int rc;

	/* register the signal handler */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigint;
	sigaction(SIGINT, &sa, NULL);

	// the w_epoll_create function from the provided w_epoll.h uses the deprecated
	// epoll_create function:
	// "Since Linux 2.6.8, the size argument (of epoll_create) is ignored" - the man page
	// thus i'll just call epoll_create1 to initialize multiplexing
	epollfd = epoll_create1(0);
	DIE(epollfd < 0, "epoll create failed");

	// create server socket. it will listen on 8888
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener failed");

	// add server socket to epoll
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "epoll_add failed");

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (server_running) {
		struct epoll_event rev;

		// wait for events
		rc = epoll_wait(epollfd, &rev, 1, EPOLL_TIMEOUT_INFINITE);

		/* Check if epoll_wait was interrupted by Ctrl+C signal */
		if (rc < 0) {
			if (errno == EINTR) {
				dlog(LOG_INFO, "Server shutting down\n");
				break; 
			}
			DIE(rc < 0, "epoll_wait failed");
		}

		dlog(LOG_INFO, "Server has a new event of type");
		// we can either have a request for a new connection
		// or a data request from the client
		// new connection request
		if (rev.events == EPOLLIN && rev.data.fd == listenfd) {
			dlog(LOG_INFO, "new connection\n");
			handle_new_connection();
		} else {
			// socket communication
			dlog(LOG_INFO, "socket communication\n");
			handle_client(rev.events, rev.data.ptr);
		}
	}

	// cleanup
	rc = close(epollfd);
	DIE(rc < 0, "epoll close failed");

	rc = close(listenfd);
	DIE(rc < 0, "listenfd close failed");

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (active_connections[i] != NULL) {
			dlog(LOG_INFO, "Cleaning up dangling connection on fd %d\n", i);
			connection_remove(active_connections[i]);
		}
	}

	return 0;
}
