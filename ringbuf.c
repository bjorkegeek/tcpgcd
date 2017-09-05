#include "ringbuf.h"
#include <stddef.h>
//#include <stdio.h>

void ringbuf_init(ringbuf* rb)
{
  rb->head = rb->data;
  rb->tail = NULL;
}

char* ringbuf_write_buf(const ringbuf* rb, int *num_bytes)
{
  if (!rb->tail) {
    *num_bytes = RINGBUF_SIZE;
  } else if (rb->head == rb->tail) {
    *num_bytes = 0;
  } else if (rb->head > rb->tail) {
    *num_bytes = RINGBUF_SIZE - (rb->head - rb->data);
  } else /* if (rb->head < rb->tail) */ {
    *num_bytes = rb->tail - rb->head;
  }
  return rb->head;
}

void ringbuf_signal_write(ringbuf *rb, int num_bytes) {
  if (num_bytes) {
    if (!rb->tail) {
      rb->tail = rb->head;
    }
    rb->head += num_bytes;
    // printf("ringbuf: %i bytes written\n", num_bytes);
    if ((rb->head - rb->data) == RINGBUF_SIZE) {
      // printf("ringbuf: Write wrap\n");
      rb->head = rb->data;
    }
  }
}

const char* ringbuf_read_buf(const ringbuf* rb, int *num_bytes)
{
  if (!rb->tail) {
    *num_bytes = 0;
  } else if (rb->head <= rb->tail) {
    *num_bytes = RINGBUF_SIZE - (rb->tail - rb->data);
  } else if (rb->head > rb->tail) {
    *num_bytes = rb->head - rb->tail;
  }
  return rb->tail;
}

void ringbuf_signal_read(ringbuf *rb, int num_bytes) {
  if (num_bytes) {
    rb->tail += num_bytes;
    if ((rb->tail - rb->data) == RINGBUF_SIZE) {
      // printf("ringbuf: Read wrap\n");
      rb->tail = rb->data;
    }
    if (rb->tail == rb->head) {
      // All data has been read.
      // Reset ringbuf to pristine state to
      // maximize contiguous buffer
      // printf("ringbuf: All data read\n");
      ringbuf_init(rb);
    } else {
      // printf("ringbuf: %i bytes read\n", num_bytes);
    }
  }
}

int cap_bytes_to_expected_data(ringbuf* ringbuf,
			       int buf_size,
			       int expected_bytes)
{
  int num_bytes;
  ringbuf_read_buf(ringbuf, &num_bytes);
  num_bytes = expected_bytes - num_bytes;
  if (num_bytes < buf_size && num_bytes >= 0) {
    return num_bytes;
  } else {
    return buf_size;
  }
}
