
// Starts listening to SIGTERM, SIGINT, SIGHUP
void activate_signal_handlers();

// Restore previous handlers for above signals
void restore_signal_handlers();

// Returns number of times any signal has been triggered
int get_signal_trigger_count();
