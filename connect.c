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
  CS_CONNECTING_TO_LL,
  CS_MAGIC_HANDSHAKE,
  CS_READY,
  CS_CONNECTING_TO_OTHER,
  CS_FORWARDING
};

enum connection_flags {
  FF_EOF_FROM_LL = 1,
  FF_EOF_FROM_OTHER = 2,
  FF_LL_SEND_SHUTDOWN = 4,
  FF_OTHER_SEND_SHUTDOWN = 8
};

typedef struct connection_t {
  enum connection_state state;
  int flags;
  
  int ll_socket;
  int other_socket;
  ringbuf from_ll;
  ringbuf to_ll;
  time_t timeout;
  struct connection_t *next;
} connection;

typedef struct {
  const parsed_options *program_options;
  connection* first_connection;
  fd_set readfds;
  fd_set writefds;
  struct timespec clock;
  time_t inhibit_connections_until;
  FILE* error_stream;
} main_loop_context;

static void myperror_code(const char* context,
			  int error_code,
			  FILE* stream)
{
  char buf[128];
  if (!stream) {
    return;
  }
  if (0 == strerror_r(error_code, buf, sizeof(buf))) {
    fprintf(stream, "%s: %s\n", context, buf);
  }
}

static void myperror(const char* context, FILE* stream)
{
  myperror_code(context, errno, stream);
}

static connection* create_connection(connection** holder) {
  connection *new_connection = (connection*)malloc(sizeof(connection));
  assert(new_connection);
  new_connection->flags = 0;
  new_connection->ll_socket = new_connection->other_socket = -1;
  ringbuf_init(&new_connection->from_ll);
  ringbuf_init(&new_connection->to_ll);
  new_connection->next = *holder;
  *holder = new_connection;
  return new_connection;
}

static void destroy_connection(connection** holder) {
  connection *nxt = (*holder)->next;
  if ((*holder)->ll_socket >=0 ) {
    close_rst((*holder)->ll_socket);
  }
  if ((*holder)->other_socket >=0 ) {
    close_rst((*holder)->other_socket);
  }
  free(*holder);
  *holder = nxt;
}

static void close_connection(connection* c) {
  if (c->ll_socket >= 0) {
    close(c->ll_socket);
    c->ll_socket = -1;
  }
  if (c->other_socket >= 0) {
    close(c->other_socket);
    c->other_socket = -1;
  }
}

static int should_make_new_ll_connection(main_loop_context *context)
{
   connection *c;
   if (context->clock.tv_sec < context->inhibit_connections_until) {
     return 0;
   }
   for (c = context->first_connection;
	c; c = c->next) {
     if (c->state != CS_FORWARDING) {
       return 0;
     }
   }
   return 1;
}

static void inhibit_new_connections(main_loop_context *context)
{
  context->inhibit_connections_until = context->clock.tv_sec +
    RECONNECT_INTERVAL;
}

static int send_heartbeat_ack(const main_loop_context *context,
			      connection** holder)
{
  connection *c = *holder;
  char *buf;
  int num_bytes;
  buf = ringbuf_write_buf(&c->to_ll, &num_bytes);
  if (num_bytes < 1) {
    destroy_connection(holder);
    return 0;
  }
  *buf = HEARTBEAT_ACK;
  ringbuf_signal_write(&c->to_ll, 1);
  return 1;
}


static int connect_to_other(const main_loop_context *context,
			    connection **holder)
{
  int v;
  connection *c = *holder;
  c->other_socket = socket(PF_INET, SOCK_STREAM, 0);
  if (c->other_socket < 0) {
    myperror("Making socket for other connection", context->error_stream);
    destroy_connection(holder);
    return 0;
  }

  if (0 != fcntl(c->other_socket, F_SETFL, fcntl(c->other_socket, F_GETFL, 0) | O_NONBLOCK)) {
    myperror("Going non-blocking", context->error_stream);
    destroy_connection(holder);
    return 0;
  }

  v = connect(c->other_socket,
	      (const struct sockaddr *) &context->program_options->client,
	      sizeof(context->program_options->client));
  if (v < 0 && errno != EINPROGRESS) {
    myperror("Connecting to other server", context->error_stream);
    destroy_connection(holder);
    return 0;
  } else if (v == 0) {
    c->state = CS_FORWARDING;
    set_nodelay(c->other_socket);
  } else {
    c->state = CS_CONNECTING_TO_OTHER;
  }
  return 1;
}


static int new_data_from_ll(const main_loop_context *context,
			    connection **holder) {
  connection *c = *holder;
  int num_bytes;
  const char* buf;
  
  switch (c->state) {
  case CS_MAGIC_HANDSHAKE:
    buf = ringbuf_read_buf(&c->from_ll, &num_bytes);
    if (num_bytes > NUM_MAGIC_BYTES) {
      num_bytes = NUM_MAGIC_BYTES;
    }
    if (0 != memcmp(buf, magic_bytes_listen, num_bytes)) {
      destroy_connection(holder);
      return 0;
    }
    if (num_bytes == NUM_MAGIC_BYTES) {
      c->state = CS_READY;
      ringbuf_signal_read(&c->from_ll, num_bytes);
      c->timeout = context->clock.tv_sec + 2 * HEARTBEAT_INTERVAL;
    }
    break;
  case CS_READY:
    buf = ringbuf_read_buf(&c->from_ll, &num_bytes);
    if (num_bytes > 1) {
      num_bytes = 1;
    }
    if (num_bytes) {
      switch (*buf) {
      case HEARTBEAT:
	c->timeout = context->clock.tv_sec + 2 * HEARTBEAT_INTERVAL;
	if (!send_heartbeat_ack(context, holder)) {
	  return 0;
	}
	break;
      case GO_FORWARD:
	if (!connect_to_other(context, holder)) {
	  return 0;
	}
	break;
      default:
	destroy_connection(holder);
	return 0;
      }
      ringbuf_signal_read(&c->from_ll, num_bytes);
    }
  }
  return 1;
}

static int enter_state_cs_magic_handshake(const main_loop_context *context,
					  connection **holder)
{
  char* buf;
  int num_bytes;
  connection *c = *holder;

  set_nodelay(c->ll_socket);
  c->state = CS_MAGIC_HANDSHAKE;
  c->timeout = context->clock.tv_sec + HEARTBEAT_INTERVAL;
  buf = ringbuf_write_buf(&c->to_ll, &num_bytes);
  assert(num_bytes > NUM_MAGIC_BYTES);
  memcpy(buf, magic_bytes_connect, NUM_MAGIC_BYTES);
  ringbuf_signal_write(&c->to_ll, NUM_MAGIC_BYTES);

  return new_data_from_ll(context, holder);
}

static void make_new_ll_connection(main_loop_context *context)
{
  int v;
  connection *c = create_connection(&context->first_connection);
  c->ll_socket = socket(PF_INET, SOCK_STREAM, 0);
  if (c->ll_socket < 0) {
    myperror("Making socket for listen-listen connection", context->error_stream);
    inhibit_new_connections(context);
    destroy_connection(&context->first_connection);
    return;
  }

  if (0 != fcntl(c->ll_socket, F_SETFL, fcntl(c->ll_socket, F_GETFL, 0) | O_NONBLOCK)) {
    myperror("Going non-blocking", context->error_stream);
    inhibit_new_connections(context);
    destroy_connection(&context->first_connection);
    return;
  }

  v = connect(c->ll_socket,
	      (const struct sockaddr *) &context->program_options->other_tcpgcd,
	      sizeof(context->program_options->other_tcpgcd));
  if (v < 0 && errno != EINPROGRESS) {
    myperror("Connecting to listen-listen server", context->error_stream);
    inhibit_new_connections(context);
    destroy_connection(&context->first_connection);
    return;
  } else if (v == 0) {
    enter_state_cs_magic_handshake(context, &context->first_connection);
  } else {
    c->state = CS_CONNECTING_TO_LL;
  }
}

static void make_fdsets(main_loop_context *context)
{
  connection *c;
  int num_bytes;
  FD_ZERO(&context->readfds);
  FD_ZERO(&context->writefds);
  // printf("--------\n");
  for (c=context->first_connection; c; c = c->next) {
    switch (c->state) {
    case CS_CONNECTING_TO_LL:
      // printf("CS_CONNECTING_TO_LL\n");
      FD_SET(c->ll_socket, &context->writefds);
      break;
    case CS_MAGIC_HANDSHAKE:
      // printf("CS_MAGIC_HANDSHAKE\n");
      ringbuf_read_buf(&c->from_ll, &num_bytes);
      if (num_bytes < NUM_MAGIC_BYTES) {
	FD_SET(c->ll_socket, &context->readfds);
      }
      ringbuf_read_buf(&c->to_ll, &num_bytes);
      if (num_bytes) {
	FD_SET(c->ll_socket, &context->writefds);
      }
      break;
    case CS_READY:
      // printf("CS_READY\n");
      ringbuf_write_buf(&c->from_ll, &num_bytes);
      if (num_bytes) {
	FD_SET(c->ll_socket, &context->readfds);
      }
      ringbuf_read_buf(&c->to_ll, &num_bytes);
      if (num_bytes) {
	FD_SET(c->ll_socket, &context->writefds);
      }
      break;
    case CS_CONNECTING_TO_OTHER:
      // printf("CS_CONNECTING_TO_OTHER\n");
      FD_SET(c->other_socket, &context->writefds);
      break;
    case CS_FORWARDING:
      // printf("CS_FORWARDING\n");
      if (!(c->flags & FF_EOF_FROM_OTHER)) {
	ringbuf_write_buf(&c->to_ll, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->other_socket, &context->readfds);
	}
      }
      if (!(c->flags & FF_LL_SEND_SHUTDOWN)) {
	ringbuf_read_buf(&c->to_ll, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->ll_socket, &context->writefds);
	}
      }
      if (!(c->flags & FF_EOF_FROM_LL)) {
	ringbuf_write_buf(&c->from_ll, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->ll_socket, &context->readfds);
	}
      }
      if (!(c->flags & FF_OTHER_SEND_SHUTDOWN)) {
	ringbuf_read_buf(&c->from_ll, &num_bytes);
	if (num_bytes) {
	  FD_SET(c->other_socket, &context->writefds);
	}
      }
    }
  }
}

static int handle_ll_connect(main_loop_context *context,
			     connection **holder)
{
  int result;
  socklen_t result_len = sizeof(result);
  connection *c = *holder;
  if (getsockopt(c->ll_socket, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
    myperror("Fetching ll connection error code", context->error_stream);
    inhibit_new_connections(context);
    destroy_connection(holder);
    return 0;
  }

  if (result != 0) {
    myperror_code("Connecting to listen-listen server", result, context->error_stream);
    inhibit_new_connections(context);
    destroy_connection(holder);
    return 0;
  }

  return enter_state_cs_magic_handshake(context, holder);
}

static int handle_other_connect(const main_loop_context *context,
				connection **holder)
{
  int result;
  socklen_t result_len = sizeof(result);
  connection *c = *holder;
  if (getsockopt(c->other_socket, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
    myperror("Fetching other connection error code", context->error_stream);
    destroy_connection(holder);
    return 0;
  }

  if (result != 0) {
    myperror_code("Connecting to other server", result, context->error_stream);
    destroy_connection(holder);
    return 0;
  }
  c->state = CS_FORWARDING;
  set_nodelay(c->other_socket);
}

static void shutdown_ll_if_done(connection *c)
{
  if ((c->flags & FF_EOF_FROM_OTHER) &&
      ringbuf_empty(&c->to_ll)) {
    shutdown(c->ll_socket, SHUT_WR);
    c->flags |= FF_LL_SEND_SHUTDOWN;
  }
}

static void shutdown_other_if_done(connection *c)
{
  if ((c->flags & FF_EOF_FROM_LL) &&
      ringbuf_empty(&c->from_ll) && c->other_socket >= 0) {
    shutdown(c->other_socket, SHUT_WR);
    c->flags |= FF_OTHER_SEND_SHUTDOWN;
  }
}

static int read_from_ll_socket(const main_loop_context *context,
			       connection **holder)
{
  char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_write_buf(&c->from_ll, &num_bytes);
  if (c->state == CS_MAGIC_HANDSHAKE) {
    num_bytes = cap_bytes_to_expected_data(&c->from_ll,
					   num_bytes,
					   NUM_MAGIC_BYTES);
  } else if (c->state == CS_READY) {
    num_bytes = cap_bytes_to_expected_data(&c->from_ll,
					   num_bytes,
					   1);
  }
  if (num_bytes) {
    num_bytes = read(c->ll_socket, buf, num_bytes);
    if (num_bytes > 0) {
      ringbuf_signal_write(&c->from_ll, num_bytes);
      if (!new_data_from_ll(context, holder)) {
	return 0;
      }
    } else if (num_bytes == 0) {
      if (c->state != CS_FORWARDING) {
	destroy_connection(holder);
	return 0;
      }
      shutdown(c->ll_socket, SHUT_RD);
      c->flags |= FF_EOF_FROM_LL;
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

static int write_to_ll_socket(connection **holder)
{
  const char *buf;
  int num_bytes;
  connection *c = *holder;
  buf = ringbuf_read_buf(&c->to_ll, &num_bytes);
  if (num_bytes) {
    num_bytes = write(c->ll_socket, buf, num_bytes);
    if (num_bytes >= 0) {
      ringbuf_signal_read(&c->to_ll, num_bytes);
      shutdown_ll_if_done(c);
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
  buf = ringbuf_write_buf(&c->to_ll, &num_bytes);
  if (num_bytes) {
    num_bytes = read(c->other_socket, buf, num_bytes);
    if (num_bytes > 0) {
      ringbuf_signal_write(&c->to_ll, num_bytes);
    } else if (num_bytes == 0) {
      shutdown(c->other_socket, SHUT_RD);
      c->flags |= FF_EOF_FROM_OTHER;
      shutdown_ll_if_done(c);
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
  buf = ringbuf_read_buf(&c->from_ll, &num_bytes);
  if (num_bytes) {
    num_bytes = write(c->other_socket, buf, num_bytes);
    if (num_bytes >= 0) {
      ringbuf_signal_read(&c->from_ll, num_bytes);
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


static void post_select(main_loop_context *context) {
  connection **holder;
  connection *c;
  for (holder=&context->first_connection; *holder;) {
    c = *holder;
    if (FD_ISSET(c->ll_socket, &context->readfds)) {
      if (!read_from_ll_socket(context, holder)) {
	continue;
      }
    }
    if (FD_ISSET(c->ll_socket, &context->writefds)) {
      if (c->state == CS_CONNECTING_TO_LL) {
	if (!handle_ll_connect(context, holder)) {
	  continue;
	}
      } else if (!write_to_ll_socket(holder)) {
	continue;
      }
    }
    if (c->state == CS_CONNECTING_TO_OTHER) {
      if (FD_ISSET(c->other_socket, &context->writefds)) {
	if (!handle_other_connect(context, holder)) {
	  continue;
	}
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
      if ((c->flags & (FF_LL_SEND_SHUTDOWN | FF_OTHER_SEND_SHUTDOWN)) ==
	  (FF_LL_SEND_SHUTDOWN | FF_OTHER_SEND_SHUTDOWN)) {
	close_connection(c);
	destroy_connection(holder);
	continue;
      }
    } else if (c->state != CS_CONNECTING_TO_OTHER &&
	       (c->flags & FF_EOF_FROM_LL)) {
      destroy_connection(holder);
      continue;
    }

    if ((c->state == CS_MAGIC_HANDSHAKE  ||
	 c->state == CS_READY) &&
	c->timeout <= context->clock.tv_sec) {
      destroy_connection(holder);
      continue;
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

static int main_loop(const parsed_options *program_options,
		     FILE* error_stream)
{
  
  main_loop_context context;
  context.error_stream = error_stream;
  context.program_options = program_options;
  context.first_connection = NULL;
  context.inhibit_connections_until = 0;
  struct timeval timeout;
  int num_triggers_base = get_signal_trigger_count();
  
  clock_gettime(CLOCK_MONOTONIC, &context.clock);
  
  activate_signal_handlers();
  
  while (get_signal_trigger_count() <= num_triggers_base) {
    if (should_make_new_ll_connection(&context)) {
      make_new_ll_connection(&context);
    }
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

int run_connect_connect(const parsed_options *program_options,
			FILE* error_stream)
{
  return main_loop(program_options,
		   error_stream);
}
