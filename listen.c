#include "listen.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <signal.h>

#include "config.h"
#include "ringbuf.h"
#include "protocol.h"
#include "sighandler.h"
#include "sockutils.h"

enum connection_state {
  CS_MAGIC_HANDSHAKE,
  CS_READY,
  CS_AWAITING_HEARTBEAT_ACK,
  CS_FORWARDING
};

enum connection_flags {
  FF_EOF_FROM_CC = 1,
  FF_EOF_FROM_OTHER = 2,
  FF_CC_SEND_SHUTDOWN = 4,
  FF_OTHER_SEND_SHUTDOWN = 8
};

typedef struct connection_t {
  enum connection_state state;
  int flags;
  
  int cc_socket;
  int other_socket;
  ringbuf from_cc;
  ringbuf to_cc;
  time_t next_heartbeat;
  struct connection_t *next;
} connection;

typedef struct {
  int ll_listen_socket;
  int other_listen_socket;
  connection* first_connection;
  fd_set readfds;
  fd_set writefds;
  struct timespec clock;
} main_loop_context;

static void myperror(const char* context, FILE* stream)
{
  char buf[128];
  if (!stream) {
    return;
  }
  if (0 == strerror_r(errno, buf, sizeof(buf))) {
    fprintf(stream, "%s: %s\n", context, buf);
  }
}

static connection* create_connection(connection** holder) {
  connection *new_connection = (connection*)malloc(sizeof(connection));
  assert(new_connection);
  new_connection->flags = 0;
  new_connection->cc_socket = new_connection->other_socket = -1;
  ringbuf_init(&new_connection->from_cc);
  ringbuf_init(&new_connection->to_cc);
  new_connection->next = *holder;
  *holder = new_connection;
  return new_connection;
}

static void destroy_connection(connection** holder) {
  connection *nxt = (*holder)->next;
  if ((*holder)->cc_socket >=0 ) {
    close_rst((*holder)->cc_socket);
  }
  if ((*holder)->other_socket >=0 ) {
    close_rst((*holder)->other_socket);
  }
  free(*holder);
  *holder = nxt;
}

static void close_connection(connection* c) {
  if (c->cc_socket >= 0) {
    close(c->cc_socket);
    c->cc_socket = -1;
  }
  if (c->other_socket >= 0) {
    close(c->other_socket);
    c->other_socket = -1;
  }
}


static int make_listening_socket(const struct sockaddr_in *addr,
				 const char* error_context,
				 FILE* error_stream)
{
  int sock;
  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    myperror(error_context, error_stream);
    return 0;
  }

  if (0 != fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK)) {
    myperror(error_context, error_stream);
    return 0;
  }

  if (0 != bind(sock, (const struct sockaddr *) addr, sizeof(*addr))) {
    myperror(error_context, error_stream);
    return 0;
  }

  if (0 != listen(sock, LISTEN_BACKLOG)) {
    myperror(error_context, error_stream);
    return 0;
  }

  return sock;
}

static void schedule_heartbeat(const main_loop_context *context,
			       connection *c)
{
  c->next_heartbeat = context->clock.tv_sec + HEARTBEAT_INTERVAL;
}


static void make_fdsets(main_loop_context *context)
{
  connection *c;
  int num_bytes;
  FD_ZERO(&context->readfds);
  FD_ZERO(&context->writefds);
  FD_SET(context->ll_listen_socket, &context->readfds);
  for (c=context->first_connection; c; c = c->next) {
    switch (c->state) {
    case CS_MAGIC_HANDSHAKE:
      ringbuf_read_buf(&c->from_cc, &num_bytes);
      if (num_bytes < NUM_MAGIC_BYTES) {
	FD_SET(c->cc_socket, &context->readfds);
      }
      ringbuf_read_buf(&c->to_cc, &num_bytes);
      if (num_bytes) {
	FD_SET(c->cc_socket, &context->writefds);
      }
      break;
    case CS_READY:
      FD_SET(context->other_listen_socket, &context->readfds);
      break;
    case CS_AWAITING_HEARTBEAT_ACK:
      ringbuf_read_buf(&c->from_cc, &num_bytes);
      if (num_bytes < 1) {
	FD_SET(c->cc_socket, &context->readfds);
      }
      ringbuf_read_buf(&c->to_cc, &num_bytes);
      if (num_bytes) {
	FD_SET(c->cc_socket, &context->writefds);
      }
      break;
    case CS_FORWARDING:
      if (!(c->flags & FF_EOF_FROM_OTHER)) {
	ringbuf_write_buf(&c->to_cc, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->other_socket, &context->readfds);
	}
      }

      if (!(c->flags & FF_CC_SEND_SHUTDOWN)) {
	ringbuf_read_buf(&c->to_cc, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->cc_socket, &context->writefds);
	}
      }
      if (!(c->flags & FF_EOF_FROM_CC)) {
	ringbuf_write_buf(&c->from_cc, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->cc_socket, &context->readfds);
	}
      }
      if (!(c->flags & FF_OTHER_SEND_SHUTDOWN)) {
	ringbuf_read_buf(&c->from_cc, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->other_socket, &context->writefds);
	}
      }
    }
  }
}

static int accept_new_ll(main_loop_context *context)
{
  connection *c = create_connection(&context->first_connection);
  char *buf;
  int num_bytes;
  if (0 > (c->cc_socket = accept(context->ll_listen_socket, NULL, NULL))) {
    destroy_connection(&context->first_connection);
    return EXIT_FAILURE;
  }
  fcntl(c->cc_socket, F_SETFL, fcntl(c->cc_socket, F_GETFL, 0) | O_NONBLOCK);

  set_nodelay(c->cc_socket);
  
  c->state = CS_MAGIC_HANDSHAKE;
  schedule_heartbeat(context, c);
  
  buf = ringbuf_write_buf(&c->to_cc, &num_bytes);
  assert(num_bytes > NUM_MAGIC_BYTES);
  memcpy(buf, magic_bytes_listen, NUM_MAGIC_BYTES);
  ringbuf_signal_write(&c->to_cc, NUM_MAGIC_BYTES);
  return 0;
}

static void accept_new_other(main_loop_context *context,
			    connection* c)
{
  char *buf;
  int num_bytes;
  buf = ringbuf_write_buf(&c->to_cc, &num_bytes);
  if (!num_bytes) {
    return;
  }
  if (0 > (c->other_socket = accept(context->other_listen_socket, NULL, NULL))) {
    return;
  }
  fcntl(c->other_socket, F_SETFL, fcntl(c->other_socket, F_GETFL, 0) | O_NONBLOCK);

  set_nodelay(c->other_socket);

  *buf = GO_FORWARD;
  ringbuf_signal_write(&c->to_cc, 1);

  c->state = CS_FORWARDING;
}

static void shutdown_cc_if_done(connection *c)
{
  if ((c->flags & FF_EOF_FROM_OTHER) &&
      ringbuf_empty(&c->to_cc)) {
    shutdown(c->cc_socket, SHUT_WR);
    c->flags |= FF_CC_SEND_SHUTDOWN;
  }
}

static void shutdown_other_if_done(connection *c)
{
  if ((c->flags & FF_EOF_FROM_CC) &&
      ringbuf_empty(&c->from_cc) && c->other_socket >= 0) {
    shutdown(c->other_socket, SHUT_WR);
    c->flags |= FF_OTHER_SEND_SHUTDOWN;
  }
}

static int new_data_from_cc(const main_loop_context *context,
			    connection **holder) {
  connection *c = *holder;
  int num_bytes;
  const char* buf;
  
  switch (c->state) {
  case CS_MAGIC_HANDSHAKE:
    buf = ringbuf_read_buf(&c->from_cc, &num_bytes);
    if (num_bytes > NUM_MAGIC_BYTES) {
      num_bytes = NUM_MAGIC_BYTES;
    }
    if (0 != memcmp(buf, magic_bytes_connect, num_bytes)) {
      destroy_connection(holder);
      return 0;
    }
    if (num_bytes == NUM_MAGIC_BYTES) {
      c->state = CS_READY;
      schedule_heartbeat(context, c);
      ringbuf_signal_read(&c->from_cc, num_bytes);
    }
    break;
  case CS_AWAITING_HEARTBEAT_ACK:
    buf = ringbuf_read_buf(&c->from_cc, &num_bytes);
    if (num_bytes > 1) {
      num_bytes = 1;
    }
    if (num_bytes) {
      if (*buf != HEARTBEAT_ACK) {
	destroy_connection(holder);
	return 0;
      }
      c->state = CS_READY;
      ringbuf_signal_read(&c->from_cc, num_bytes);
    }
  }
  return 1;
}

static int read_from_cc_socket(const main_loop_context *context,
			       connection **holder)
{
  char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_write_buf(&c->from_cc, &num_bytes);
  if (c->state == CS_MAGIC_HANDSHAKE) {
    num_bytes = cap_bytes_to_expected_data(&c->from_cc,
					   num_bytes,
					   NUM_MAGIC_BYTES);
  } else if (c->state == CS_AWAITING_HEARTBEAT_ACK) {
    num_bytes = cap_bytes_to_expected_data(&c->from_cc,
					   num_bytes,
					   1);
  }
  if (num_bytes) {
    num_bytes = read(c->cc_socket, buf, num_bytes);
    if (num_bytes > 0) {
      ringbuf_signal_write(&c->from_cc, num_bytes);
      if (!new_data_from_cc(context, holder)) {
	return 0;
      }
    } else if (num_bytes == 0) {
      if (c->state != CS_FORWARDING) {
	destroy_connection(holder);
	return 0;
      }
      shutdown(c->cc_socket, SHUT_RD);
      c->flags |= FF_EOF_FROM_CC;
      shutdown_other_if_done(c);
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
	destroy_connection(holder);
	return 0;
      }
    }
  }
  return 1;
}

static int write_to_cc_socket(connection **holder)
{
  const char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_read_buf(&c->to_cc, &num_bytes);
  if (num_bytes) {
    num_bytes = write(c->cc_socket, buf, num_bytes);
    if (num_bytes >= 0) {
      ringbuf_signal_read(&c->to_cc, num_bytes);
      shutdown_cc_if_done(c);
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
	destroy_connection(holder);
	return 0;
      }
    }
  }
  return 1;
}

static int read_from_other_socket(connection **holder)
{
  char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_write_buf(&c->to_cc, &num_bytes);
  if (num_bytes) {
    num_bytes = read(c->other_socket, buf, num_bytes);
    if (num_bytes > 0) {
      ringbuf_signal_write(&c->to_cc, num_bytes);
    } else if (num_bytes == 0) {
      shutdown(c->other_socket, SHUT_RD);
      c->flags |= FF_EOF_FROM_OTHER;
      shutdown_cc_if_done(c);
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
	destroy_connection(holder);
	return 0;
      }
    }
  }
  return 1;
}

static int write_to_other_socket(connection **holder)
{
  const char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_read_buf(&c->from_cc, &num_bytes);
  if (num_bytes) {
    num_bytes = write(c->other_socket, buf, num_bytes);
    if (num_bytes >= 0) {
      ringbuf_signal_read(&c->from_cc, num_bytes);
      shutdown_other_if_done(c);
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
	destroy_connection(holder);
	return 0;
      }
    }
  }
  return 1;
}

static int send_heartbeat(main_loop_context *context,
			  connection** holder)
{
  connection *c = *holder;
  char *buf;
  int num_bytes;
  buf = ringbuf_write_buf(&c->to_cc, &num_bytes);
  if (num_bytes < 1) {
    destroy_connection(holder);
    return 0;
  }
  *buf = HEARTBEAT;
  ringbuf_signal_write(&c->to_cc, 1);
  c->state = CS_AWAITING_HEARTBEAT_ACK;
  schedule_heartbeat(context, c);
  return 1;
}

static void post_select(main_loop_context *context)
{
  connection **holder;
  connection *c;
  int accepted = 0;
  if (FD_ISSET(context->ll_listen_socket, &context->readfds)) {
    accept_new_ll(context);
  }
  for (holder=&context->first_connection; *holder;) {
    c = *holder;
    if (FD_ISSET(c->cc_socket, &context->readfds)) {
      if (!read_from_cc_socket(context, holder)) {
	continue;
      }
    }
    if (FD_ISSET(c->cc_socket, &context->writefds)) {
      if (!write_to_cc_socket(holder)) {
	continue;
      }
    }
    if (c->state == CS_FORWARDING) {
      if (FD_ISSET(c->other_socket, &context->readfds)) {
	if (!read_from_other_socket(holder)) {
	  continue;
	}
      }
      if (FD_ISSET(c->other_socket, &context->writefds)) {
	if (!write_to_other_socket(holder)) {
	  continue;
	}
      }
      if ((c->flags & (FF_CC_SEND_SHUTDOWN | FF_OTHER_SEND_SHUTDOWN)) ==
	  (FF_CC_SEND_SHUTDOWN | FF_OTHER_SEND_SHUTDOWN)) {
	close_connection(c);
	destroy_connection(holder);
	continue;
      }
    } else if (c->flags & FF_EOF_FROM_CC) {
      destroy_connection(holder);
      continue;
    }
    if ((c->state == CS_AWAITING_HEARTBEAT_ACK ||
	 c->state == CS_MAGIC_HANDSHAKE) &&
	context->clock.tv_sec >= c->next_heartbeat) {
      // Heartbeat timeout
      destroy_connection(holder);
      continue;
    }
    if (c->state == CS_READY) {
      if (context->clock.tv_sec >= c->next_heartbeat) {
	if (!send_heartbeat(context, holder)) {
	  continue;
	}
      } else if (!accepted &&
	  FD_ISSET(context->other_listen_socket, &context->readfds)) {
	accept_new_other(context, c);
	accepted = 1;
      }
    }
    
    holder = &(*holder)->next;
  }
}

static void destroy_all_connections(main_loop_context *context)
{
  while (context->first_connection) {
    destroy_connection(&context->first_connection);
  }
}

static int main_loop(int ll_listen_socket,
		     int other_listen_socket,
		     FILE* error_stream)
{
  
  main_loop_context context;
  context.ll_listen_socket = ll_listen_socket;
  context.other_listen_socket = other_listen_socket;
  context.first_connection = NULL;
  struct timeval timeout;
  int num_triggers_base = get_signal_trigger_count();
  
  if (ll_listen_socket >= FD_SETSIZE || other_listen_socket >= FD_SETSIZE) {
    if (error_stream) {
      fprintf(error_stream, "Too many fds for select\n");
      return EXIT_FAILURE;
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &context.clock);
  
  activate_signal_handlers();
  
  while (get_signal_trigger_count() <= num_triggers_base) {
    make_fdsets(&context);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    select(FD_SETSIZE, &context.readfds, &context.writefds, NULL, &timeout);
    clock_gettime(CLOCK_MONOTONIC, &context.clock);

    post_select(&context);
  }  
  restore_signal_handlers();
  destroy_all_connections(&context);
}


int run_listen_listen(const parsed_options *program_options,
		      FILE* error_stream)
{
  int ll_listen_socket;
  int other_listen_socket;
  ll_listen_socket = make_listening_socket(&program_options->other_tcpgcd,
					   "Creating listen-listen socket",
					   error_stream);
  if (!ll_listen_socket) {
    return EXIT_FAILURE;
  }
  other_listen_socket = make_listening_socket(&program_options->client,
					      "Creating client listen socket",
					      error_stream);
  if (!other_listen_socket) {
    return EXIT_FAILURE;
  }

  return main_loop(ll_listen_socket,
		   other_listen_socket,
		   error_stream);
}
