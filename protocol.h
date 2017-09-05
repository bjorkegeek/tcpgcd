// Protocol parameters
enum {
  NUM_MAGIC_BYTES = 8,
  HEARTBEAT_INTERVAL = 30,
  RECONNECT_INTERVAL = 5
};

// Protocol data
enum {
  HEARTBEAT = 'H',
  HEARTBEAT_ACK = 'A',
  GO_FORWARD = 'F'
};

extern const char magic_bytes_listen[NUM_MAGIC_BYTES];
extern const char magic_bytes_connect[NUM_MAGIC_BYTES];
