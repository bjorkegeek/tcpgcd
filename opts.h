#ifndef _INCLUDE_OPTS_H
#define _INCLUDE_OPTS_H

#include <netinet/in.h>
#include <stdio.h>

typedef enum {
  LISTEN_LISTEN,
  CONNECT_CONNECT
} run_mode ;

typedef struct {
  run_mode run_mode;
  struct sockaddr_in other_tcpgcd;
  struct sockaddr_in client;
} parsed_options;

// Returns 0 on success, non-zero on failure.
// If errstream is non-null, it will have explanatory
// error messages sent to it.
int parse_options(int argc,
		  char* const argv[],
		  FILE *errstream,
		  parsed_options *dest);

#endif
