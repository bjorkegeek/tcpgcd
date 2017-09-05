#include <stdio.h>
#include "opts.h"
#include "listen.h"
#include "connect.h"

static int run_from_options(const parsed_options* program_options,
			    FILE* error_stream)
{
  if (program_options->run_mode == LISTEN_LISTEN) {
    return run_listen_listen(program_options, error_stream);
  } else if (program_options->run_mode == CONNECT_CONNECT) {
    return run_connect_connect(program_options, error_stream);
  }
}

int main(int argc, char *argv[]) {
  parsed_options program_options;
  int ret = parse_options(argc, argv,
			  stderr,
			  &program_options);
  if (ret != 0) {
    return ret;
  }
  return run_from_options(&program_options, stderr);
}
