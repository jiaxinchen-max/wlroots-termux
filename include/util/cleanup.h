typedef void (*wlr_task_cb)(void *data);

struct wlr_task {
	wlr_task_cb task;
	void *data;
};

void wlr_cleanup_queue_init(void);
void wlr_cleanup_queue_finish(void);
void wlr_cleanup_defer(struct wlr_task t);
