#include "protocol.h"

const char magic_bytes_listen[NUM_MAGIC_BYTES] = {
  't','c','p','g','c', 'p', '0', 'l'
};
const char magic_bytes_connect[NUM_MAGIC_BYTES] = {
  't','c','p','g','c', 'p', '0', 'c'
};
