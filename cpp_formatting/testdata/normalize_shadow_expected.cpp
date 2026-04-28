int global_count = 0;

void increment() { ++global_count; }

// Parameter named globalCount shadows the global.
// Only the global is renamed with --scope=global; the parameter is a local.
void reset(int globalCount) { (void)globalCount; }

int get() { return global_count; }
