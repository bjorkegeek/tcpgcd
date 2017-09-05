#include "sighandler.h"
#include <signal.h>
#include <stdlib.h>

static const int SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };

enum {
  NUM_SIGNALS = sizeof(SIGNALS)/sizeof(SIGNALS[0])
};

typedef struct s_signal_stack_entry {
  struct sigaction saved[NUM_SIGNALS];
  struct s_signal_stack_entry *next;
} signal_stack_entry;

static signal_stack_entry *signal_stack_top = NULL;

static int num_triggers = 0;

static void signal_handler(int signo)
{
  num_triggers++;
}

static void signal_stack_push() {
  signal_stack_entry *new_entry =
    (signal_stack_entry *)malloc(sizeof(signal_stack_entry));
  new_entry->next = signal_stack_top;
  signal_stack_top = new_entry;
}

static void signal_stack_pop() {
  signal_stack_entry *old_top = signal_stack_top;
  signal_stack_top = old_top->next;
  free(old_top);  
}

static void setup_sigaction(struct sigaction *action)
{
  action->sa_handler = &signal_handler;
  sigemptyset(&action->sa_mask);
  action->sa_flags = 0;
}

void activate_signal_handlers() {
  struct sigaction *save_i;
  const int *signal_i;
  struct sigaction action;

  setup_sigaction(&action);
  
  signal_stack_push();
  
  for (signal_i = SIGNALS, save_i = signal_stack_top->saved;
       signal_i < (SIGNALS + NUM_SIGNALS);
       signal_i++, save_i++) {
    sigaction(*signal_i, &action, save_i);
  }
}

void restore_signal_handlers() {
  const struct sigaction *save_i;
  const int *signal_i;
  for (signal_i = SIGNALS, save_i = signal_stack_top->saved;
       signal_i < (SIGNALS + NUM_SIGNALS);
       signal_i++, save_i++) {
    sigaction(*signal_i, save_i, NULL);
  }
  signal_stack_pop();
}

int get_signal_trigger_count() {
  return num_triggers;
}
