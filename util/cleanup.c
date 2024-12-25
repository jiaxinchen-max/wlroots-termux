#include "util/cleanup.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define MAX_TASKS 16
struct work_queue {
	struct wlr_task list[MAX_TASKS];
	int size;
	bool running;

	pthread_mutex_t lock;
	pthread_t thread;
	int efd;
};

static struct work_queue queue;

static void *cleanup_thread(void *unused)
{
	struct wlr_task work[MAX_TASKS];
	eventfd_t v;
	do {
		eventfd_read(queue.efd, &v);

		pthread_mutex_lock(&queue.lock);
		for (int i = 0; i < (int)v; i++) {
			work[i] = queue.list[i];
		}
		for (int i = (int)v; i < queue.size; i++) {
			queue.list[i - v] = queue.list[i];
		}
		queue.size -= (int)v;
		pthread_mutex_unlock(&queue.lock);

		for (int i = 0; i < (int)v; i++) {
			work[i].task(work[i].data);
		}
	} while (queue.running);

	return NULL;
}

extern void wlr_cleanup_defer(struct wlr_task t)
{
	wlr_cleanup_queue_init();

	pthread_mutex_lock(&queue.lock);
	if (queue.size < MAX_TASKS) {
		queue.list[queue.size] = t;
		queue.size++;
	} else {
		// In case too much work is queue, apply backpressure.
		pthread_mutex_unlock(&queue.lock);
		return t.task(t.data);
	}
	pthread_mutex_unlock(&queue.lock);
	eventfd_write(queue.efd, 1);
}

extern void wlr_cleanup_queue_init(void)
{
	if (!queue.running) {
		queue = (struct work_queue){0};
		queue.running = true;
		queue.efd = eventfd(0, 0);
		pthread_mutex_init(&queue.lock, NULL);
		pthread_create(&queue.thread, NULL, cleanup_thread, NULL);
	}
}

static void done(void *unused) { queue.running = false; }

extern void wlr_cleanup_queue_finish(void)
{
	if (queue.running) {
		// Notify the cleanup thread that it's done.
		wlr_cleanup_defer((struct wlr_task){&done, NULL});
		pthread_join(queue.thread, NULL);
		close(queue.efd);
		queue = (struct work_queue){0};
	}
}
