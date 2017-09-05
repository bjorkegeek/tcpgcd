#include "config.h"

// Do not access members directly!
typedef struct {
  char* head;
  const char* tail;
  char data[RINGBUF_SIZE];
} ringbuf;

// Initialize ring buffer
void ringbuf_init(ringbuf* rb);

// Get a writable buffer, return its size through num_bytes.
// If num_bytes is 0, the ring buffer is full.
char* ringbuf_write_buf(const ringbuf* rb, int *num_bytes);

// Update the ring buffer data structure after data
// has been written.  num_bytes must be <= to what
// had previously been returned by ringbuf_write_buf
void ringbuf_signal_write(ringbuf* rb, int num_bytes);

// Get a readable buffer, return its size through num_bytes.
// If num_bytes is 0, the ring buffer is empty
const char* ringbuf_read_buf(const ringbuf* rb, int *num_bytes);

// Update the ring buffer data structure after data
// has been read.  num_bytes must be <= to what
// had previously been returned by ringbuf_read_buf
void ringbuf_signal_read(ringbuf* rb, int num_bytes);

static inline int ringbuf_empty(ringbuf* rb)
{
  return !rb->tail;
}

int cap_bytes_to_expected_data(ringbuf* ringbuf,
			       int buf_size,
			       int expected_bytes);
