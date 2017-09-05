#include "opts.h"
#include <getopt.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

const int MUST_HAVE_HOST = 1;

const int INCORRECT_COMMAND_LINE_USAGE = 2;

static int string_to_sockaddr(const char* st,
			      struct sockaddr_in *dest,
			      int flags)
{
  const char *psep = strchr(st, ':');
  char host[128];
  int psep_len;
  int port;
  dest->sin_family = AF_INET;
  if (psep) {  // a.b.c.d:x style
    psep_len = psep - st;
    if (psep_len > 127) {
      return 0;
    }
    memcpy(host, st, psep_len);
    host[psep_len] = '\0';
    if (!inet_pton(AF_INET, host, &dest->sin_addr)) {
      return 0;
    }
    port = atoi(psep + 1);
  } else {  // Just x style
    if (flags & MUST_HAVE_HOST) {
      return 0;
    }
    dest->sin_addr.s_addr = htonl(INADDR_ANY);
    port = atoi(st);
  }
  if (port == 0) {
    return 0;
  }
  dest->sin_port = htons(port);
  memset(dest->sin_zero, 0, sizeof(dest->sin_zero));
  return 1;
}

static void print_usage(FILE* d, const char* program)
{
  fprintf(d, "Usage: %s { -l | -c } [ip:]port -o [ip:]port\n", program);
  fprintf(d, "Options are:\n\n");
  
  fprintf(d, " -l,--listen [ip:]port  Listen-listen mode.\n");
  fprintf(d, "                        Provide port number and optional interface.\n");
  fprintf(d, " -c,--connect host:port Connect-connect mode.\n");
  fprintf(d, "                        Provide host and port of listen-listen server.\n");
  fprintf(d, " -o,--other [ip]:port   For listen-listen, provide port number and\n");
  fprintf(d, "                        optional interface for clients to connect.\n");
  fprintf(d, "                        For connect-connect, provide host and port\n");
  fprintf(d, "                        for the local service to connect to.\n");
}

int parse_options(int argc,
		  char *const argv[],
		  FILE *errstream,
		  parsed_options *dest)
{
  const char *short_options = "l:c:o:";
  const struct option long_options[] = {
		{"listen",	1, NULL, 'l'},
		{"connect",	1, NULL, 'c'},
		{"other",	1, NULL, 'o'},
		{NULL,          0, NULL, 0}
  };
  int next_option;
  extern char *optarg;	/* getopt */
  extern int optind; /* getopt */
  int got_run_mode = 0;
  int got_other = 0;
  
  do {
    next_option = getopt_long(argc, argv, short_options, long_options, NULL);
    switch (next_option) {
    case 'l':
      dest->run_mode = LISTEN_LISTEN;
      got_run_mode = 1;
      if (!string_to_sockaddr(optarg,
			      &dest->other_tcpgcd,
			      0)) {
	if (errstream) {
	  fprintf(errstream, "Syntax error in listening port: %s\n", optarg);
	}
	return INCORRECT_COMMAND_LINE_USAGE;
      }
      break;
    case 'c':
      dest->run_mode = CONNECT_CONNECT;
      got_run_mode = 1;
      if (!string_to_sockaddr(optarg,
			      &dest->other_tcpgcd,
			      MUST_HAVE_HOST)) {
	if (errstream) {
	  fprintf(errstream, "Syntax error in bind string %s\n", optarg);
	}
	return INCORRECT_COMMAND_LINE_USAGE;
      }
      break;
    case 'o':
      if (!string_to_sockaddr(optarg,
			      &dest->client,
			      got_run_mode && dest->run_mode == LISTEN_LISTEN
			      ? 0 : MUST_HAVE_HOST)) {
	if (errstream) {
	  fprintf(errstream, "Syntax error in bind string %s\n", optarg);
	}
	return INCORRECT_COMMAND_LINE_USAGE;
      }
      got_other = 1;
      break;
    case -1:
      ;
    }
  } while (next_option != -1);
  if (!got_run_mode || ! got_other) {
    if (errstream) {
      print_usage(errstream, argv[0]);
    }
    return INCORRECT_COMMAND_LINE_USAGE;
  }
  return 0;
}
  
