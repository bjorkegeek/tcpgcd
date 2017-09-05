#include "sockutils.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

void close_rst(int socket)
{
  struct linger so_linger = { l_onoff: 1, l_linger: 0 };
  setsockopt(socket, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
  close(socket);
}

int set_nodelay(int socket)
{
  int flag = 1;
  int result = setsockopt(socket,
			  IPPROTO_TCP,
			  TCP_NODELAY,
			  &flag,
			  sizeof(int));
  return result;
}
