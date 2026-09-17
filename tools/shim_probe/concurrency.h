#ifndef AX_PROBE_CONCURRENCY_H
#define AX_PROBE_CONCURRENCY_H
/* Called by the enrolled main thread. native_fd was created before enrollment. */
int probe_concurrent(int (*pump)(void), int native_fd);
#endif
