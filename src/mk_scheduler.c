/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2015 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <monkey/monkey.h>
#include <monkey/mk_vhost.h>
#include <monkey/mk_scheduler.h>
#include <monkey/mk_server.h>
#include <monkey/mk_memory.h>
#include <monkey/mk_cache.h>
#include <monkey/mk_config.h>
#include <monkey/mk_clock.h>
#include <monkey/mk_signals.h>
#include <monkey/mk_plugin.h>
#include <monkey/mk_utils.h>
#include <monkey/mk_macros.h>
#include <monkey/mk_rbtree.h>
#include <monkey/mk_linuxtrace.h>
#include <monkey/mk_server.h>
#include <monkey/mk_plugin_stage.h>

#include <sys/syscall.h>

struct mk_sched_worker *sched_list;
struct mk_sched_handler mk_http_handler;

static pthread_mutex_t mutex_sched_init = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_worker_init = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_worker_exit = PTHREAD_MUTEX_INITIALIZER;

__thread struct rb_root *cs_list;
__thread struct mk_list *cs_incomplete;
__thread struct mk_sched_notif *worker_sched_notif;
__thread struct mk_sched_worker *worker_sched_node;

/*
 * Returns the worker id which should take a new incomming connection,
 * it returns the worker id with less active connections. Just used
 * if config->scheduler_mode is MK_SCHEDULER_FAIR_BALANCING.
 */
static inline int _next_target()
{
    int i;
    int target = 0;
    unsigned long long tmp = 0, cur = 0;

    cur = sched_list[0].accepted_connections - sched_list[0].closed_connections;
    if (cur == 0)
        return 0;

    /* Finds the lowest load worker */
    for (i = 1; i < mk_config->workers; i++) {
        tmp = sched_list[i].accepted_connections - sched_list[i].closed_connections;
        if (tmp < cur) {
            target = i;
            cur = tmp;

            if (cur == 0)
                break;
        }
    }

    /*
     * If sched_list[target] worker is full then the whole server too, because
     * it has the lowest load.
     */
    if (mk_unlikely(cur >= mk_config->server_capacity)) {
        MK_TRACE("Too many clients: %i", mk_config->server_capacity);

        /* Instruct to close the connection anyways, we lie, it will die */
        return -1;
    }

    return target;
}

struct mk_sched_worker *mk_sched_next_target()
{
    int t = _next_target();

    if (mk_likely(t != -1))
        return &sched_list[t];
    else
        return NULL;
}

/*
 * This function is invoked when the core triggers a MK_SCHED_SIGNAL_FREE_ALL
 * event through the signal channels, it means the server will stop working
 * so this is the last call to release all memory resources in use. Of course
 * this takes place in a thread context.
 */
void mk_sched_worker_free()
{
    int i;
    pthread_t tid;
    struct mk_sched_worker *sl = NULL;

    pthread_mutex_lock(&mutex_worker_exit);

    /*
     * Fix Me: needs to implement API to make plugins release
     * their resources first at WORKER LEVEL
     */

    /* External */
    mk_plugin_exit_worker();
    mk_vhost_fdt_worker_exit();
    mk_cache_worker_exit();

    /* Scheduler stuff */
    tid = pthread_self();
    for (i = 0; i < mk_config->workers; i++) {
        if (sched_list[i].tid == tid) {
            sl = &sched_list[i];
        }
    }

    mk_bug(!sl);

    //sl->request_handler;

    /* Free master array (av queue & busy queue) */
    mk_mem_free(cs_list);
    mk_mem_free(cs_incomplete);
    pthread_mutex_unlock(&mutex_worker_exit);
}

struct mk_sched_handler *mk_sched_handler_cap(char cap)
{
    if (cap == MK_CAP_HTTP) {
        return &mk_http_handler;
    }

    return NULL;
}

/*
 * Register a new client connection into the scheduler, this call takes place
 * inside the worker/thread context.
 */
struct mk_sched_conn *mk_sched_add_connection(int remote_fd,
                                              struct mk_server_listen *listener,
                                              struct mk_sched_worker *sched)
{
    int ret;
    int size;
    struct mk_sched_handler *handler;
    struct mk_sched_conn *conn;
    struct mk_event *event;

    /* Before to continue, we need to run plugin stage 10 */
    ret = mk_plugin_stage_run_10(remote_fd);

    /* Close connection, otherwise continue */
    if (ret == MK_PLUGIN_RET_CLOSE_CONX) {
        listener->network->network->close(remote_fd);
        MK_LT_SCHED(remote_fd, "PLUGIN_CLOSE");
        return NULL;
    }

    handler = listener->protocol;
    if (handler->sched_extra_size > 0) {
        void *data;
        size = (sizeof(struct mk_sched_conn) + handler->sched_extra_size);
        data = mk_mem_malloc_z(size);
        conn = (struct mk_sched_conn *) data;
    }
    else {
        conn = mk_mem_malloc_z(sizeof(struct mk_sched_conn));
    }

    if (!conn) {
        return NULL;
    }

    event = &conn->event;
    event->fd         = remote_fd;
    event->type       = MK_EVENT_CONNECTION;
    event->mask       = MK_EVENT_EMPTY;
    conn->status      = MK_SCHEDULER_CONN_PENDING;
    conn->arrive_time = log_current_utime;
    conn->protocol    = handler;
    conn->net         = listener->network->network;

    /* Register the entry in the red-black tree queue for fast lookup */
    struct rb_node **new = &(sched->rb_queue.rb_node);
    struct rb_node *parent = NULL;

    /* Figure out where to put new node */
    while (*new) {
        struct mk_sched_conn *this = container_of(*new, struct mk_sched_conn, _rb_head);

        parent = *new;
        if (conn->event.fd < this->event.fd)
            new = &((*new)->rb_left);
        else if (conn->event.fd > this->event.fd)
            new = &((*new)->rb_right);
        else {
            mk_bug(1);
            break;
        }
    }

    /* Add new node and rebalance tree. */
    rb_link_node(&conn->_rb_head, parent, new);
    rb_insert_color(&conn->_rb_head, &sched->rb_queue);

    /* Linux trace message */
    MK_LT_SCHED(remote_fd, "REGISTERED");

    return conn;
}

static void mk_sched_thread_lists_init()
{
    /* client_session mk_list */
    cs_list = mk_mem_malloc_z(sizeof(struct rb_root));
    cs_incomplete = mk_mem_malloc(sizeof(struct mk_list));
    mk_list_init(cs_incomplete);
}

/* Register thread information. The caller thread is the thread information's owner */
static int mk_sched_register_thread()
{
    struct mk_sched_worker *sl;
    static int wid = 0;

    /*
     * If this thread slept inside this section, some other thread may touch wid.
     * So protect it with a mutex, only one thread may handle wid.
     */
    pthread_mutex_lock(&mutex_sched_init);

    sl = &sched_list[wid];
    sl->idx = wid++;
    sl->tid = pthread_self();


#if defined(__linux__)
    /*
     * Under Linux does not exists the difference between process and
     * threads, everything is a thread in the kernel task struct, and each
     * one has it's own numerical identificator: PID .
     *
     * Here we want to know what's the PID associated to this running
     * task (which is different from parent Monkey PID), it can be
     * retrieved with gettid() but Glibc does not export to userspace
     * the syscall, we need to call it directly through syscall(2).
     */
    sl->pid = syscall(__NR_gettid);
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    sl->pid = tid;
#else
    sl->pid = 0xdeadbeef;
#endif

    pthread_mutex_unlock(&mutex_sched_init);

    /* Initialize lists */
    sl->rb_queue = RB_ROOT;
    mk_list_init(&sl->incoming_queue);
    sl->request_handler = NULL;

    return sl->idx;
}

/* created thread, all these calls are in the thread context */
void *mk_sched_launch_worker_loop(void *thread_conf)
{
    int ret;
    int wid;
    unsigned long len;
    char *thread_name = 0;
    struct mk_sched_worker *sched = NULL;

    /* Avoid SIGPIPE signals on this thread */
    mk_signal_thread_sigpipe_safe();

    /* Init specific thread cache */
    mk_sched_thread_lists_init();
    mk_cache_worker_init();

    /* Register working thread */
    wid = mk_sched_register_thread();

    sched = &sched_list[wid];
    sched->loop = mk_event_loop_create(MK_EVENT_QUEUE_SIZE);
    if (!sched->loop) {
        mk_err("Error creating Scheduler loop");
        exit(EXIT_FAILURE);
    }

    /*
     * Create the notification instance and link it to the worker
     * thread-scope list.
     */
    worker_sched_notif = mk_mem_malloc(sizeof(struct mk_sched_notif));

    /* Register the scheduler channel to signal active workers */
    ret = mk_event_channel_create(sched->loop,
                                  &sched->signal_channel_r,
                                  &sched->signal_channel_w,
                                  worker_sched_notif);
    if (ret < 0) {
        exit(EXIT_FAILURE);
    }

    /*
     * ULONG_MAX BUG test only
     * =======================
     * to test the workaround we can use the following value:
     *
     *  thinfo->closed_connections = 1000;
     */

    //thinfo->ctx = thconf->ctx;

    mk_mem_free(thread_conf);

    /* Rename worker */
    mk_string_build(&thread_name, &len, "monkey: wrk/%i", sched->idx);
    mk_utils_worker_rename(thread_name);
    mk_mem_free(thread_name);

    /* Export known scheduler node to context thread */
    worker_sched_node = sched;
    mk_plugin_core_thread();

    if (mk_config->scheduler_mode == MK_SCHEDULER_REUSEPORT) {
        if (mk_server_listen_init(mk_config) == NULL) {
            mk_err("[sched] Failed to initialize listen sockets.");
            return 0;
        }
    }

    __builtin_prefetch(sched);
    __builtin_prefetch(&worker_sched_node);

    pthread_mutex_lock(&mutex_worker_init);
    sched->initialized = 1;
    pthread_mutex_unlock(&mutex_worker_init);

    /* init server thread loop */
    mk_server_worker_loop();

    return 0;
}

/* Create thread which will be listening for incomings requests */
int mk_sched_launch_thread(int max_events, pthread_t *tout)
{
    pthread_t tid;
    pthread_attr_t attr;
    sched_thread_conf *thconf;
    (void) max_events;

    /* Thread data */
    thconf = mk_mem_malloc_z(sizeof(sched_thread_conf));

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&tid, &attr, mk_sched_launch_worker_loop,
                       (void *) thconf) != 0) {
        mk_libc_error("pthread_create");
        return -1;
    }

    *tout = tid;

    return 0;
}

/*
 * The scheduler nodes are an array of struct mk_sched_worker type,
 * each worker thread belongs to a scheduler node, on this function we
 * allocate a scheduler node per number of workers defined.
 */
void mk_sched_init()
{
    int size;

    size = sizeof(struct mk_sched_worker) * mk_config->workers;
    sched_list = mk_mem_malloc_z(size);
}

void mk_sched_set_request_list(struct rb_root *list)
{
    cs_list = list;
}

int mk_sched_remove_client(struct mk_sched_worker *sched, int remote_fd)
{
    struct mk_sched_conn *conn;

    /*
     * Close socket and change status: we must invoke mk_epoll_del()
     * because when the socket is closed is cleaned from the queue by
     * the Kernel at its leisure, and we may get false events if we rely
     * on that.
     */
    mk_event_del(sched->loop, remote_fd);

    conn = mk_sched_get_connection(sched, remote_fd);
    if (conn) {
        MK_TRACE("[FD %i] Scheduler remove", remote_fd);

        /* Invoke plugins in stage 50 */
        mk_plugin_stage_run_50(remote_fd);

        sched->closed_connections++;

        /* Unlink from the red-black tree */
        rb_erase(&conn->_rb_head, &sched->rb_queue);
        mk_mem_free(conn);

        /* Only close if this was our connection.
         *
         * This has to happen _after_ the busy list removal,
         * otherwise we could get a new client accept()ed with
         * the same FD before we do the removal from the busy list,
         * causing ghosts.
         */
        conn->net->close(remote_fd);
        MK_LT_SCHED(remote_fd, "DELETE_CLIENT");
        return 0;
    }
    else {
        MK_TRACE("[FD %i] Not found", remote_fd);
        MK_LT_SCHED(remote_fd, "DELETE_NOT_FOUND");
    }
    return -1;
}

struct mk_sched_conn *mk_sched_get_connection(struct mk_sched_worker *sched,
                                                 int remote_fd)
{
    struct rb_node *node;
    struct mk_sched_conn *this;

    /*
     * In some cases the sched node can be NULL when is a premature close,
     * an example of this situation is when the function mk_sched_add_client()
     * close an incoming connection when invoking the MK_PLUGIN_STAGE_10 stage plugin,
     * so no thread context exists.
     */
    if (!sched) {
        MK_TRACE("[FD %i] No scheduler information", remote_fd);
        close(remote_fd);
        return NULL;
    }

  	node = sched->rb_queue.rb_node;
  	while (node) {
        this = container_of(node, struct mk_sched_conn, _rb_head);
		if (remote_fd < this->event.fd)
  			node = node->rb_left;
		else if (remote_fd > this->event.fd)
  			node = node->rb_right;
		else {
            MK_LT_SCHED(remote_fd, "GET_CONNECTION");
  			return this;
        }
	}

    MK_TRACE("[FD %i] not found in scheduler list", remote_fd);
    MK_LT_SCHED(remote_fd, "GET_FAILED");
    return NULL;
}

/*
 * For a given connection number, remove all resources associated: it can be
 * used on any context such as: timeout, I/O errors, request finished,
 * exceptions, etc.
 */
int mk_sched_drop_connection(int socket)
{
    struct mk_sched_conn *conn;
    struct mk_sched_worker *sched;

    sched = mk_sched_get_thread_conf();
    conn = mk_sched_get_connection(sched, socket);
    if (conn) {
        mk_sched_remove_client(sched, socket);
    }

    return 0;
}

int mk_sched_check_timeouts(struct mk_sched_worker *sched)
{
    int client_timeout;
    struct mk_http_session *cs_node;
    struct mk_sched_conn *sched_conn;
    struct mk_list *head;
    struct mk_list *temp;

    /* PENDING CONN TIMEOUT */
    mk_list_foreach_safe(head, temp, &sched->incoming_queue) {
        sched_conn = mk_list_entry(head, struct mk_sched_conn, status_queue);
        client_timeout = sched_conn->arrive_time + mk_config->timeout;

        /* Check timeout */
        if (client_timeout <= log_current_utime) {
            MK_TRACE("Scheduler, closing fd %i due TIMEOUT (incoming queue)",
                     sched_conn->event.fd);
            MK_LT_SCHED(entry_conn->event.fd, "TIMEOUT_CONN_PENDING");
            mk_sched_drop_connection(sched_conn->event.fd);
        }
    }

    mk_list_foreach_safe(head, temp, cs_incomplete) {
        cs_node = mk_list_entry(head, struct mk_http_session, request_incomplete);
        if (cs_node->counter_connections == 0) {
            client_timeout = cs_node->init_time + mk_config->timeout;
        }
        else {
            client_timeout = cs_node->init_time + mk_config->keep_alive_timeout;
        }

        /* Check timeout */
        if (client_timeout <= log_current_utime) {
            MK_TRACE("[FD %i] Scheduler, closing due to timeout (incomplete)",
                     cs_node->socket);
            MK_LT_SCHED(cs_node->socket, "TIMEOUT_REQ_INCOMPLETE");
            mk_sched_drop_connection(cs_node->socket);
        }
    }

    return 0;
}

/*
 * Scheduler events handler: lookup for event handler and invoke
 * proper callbacks.
 */
int mk_sched_event_read(struct mk_sched_conn *conn,
                        struct mk_sched_worker *sched)
{
#ifdef TRACE
    int socket = conn->event.fd;
    MK_TRACE("[FD %i] Connection Handler / read", socket);
#endif

    /*
     * When the event loop notify that there is some readable information
     * from the socket, we need to invoke the protocol handler associated
     * to this connection and also pass as a reference the 'read()' function
     * that handle 'read I/O' operations, e.g:
     *
     *  - plain sockets through liana will use just read(2)
     *  - ssl though mbedtls should use mk_mbedtls_read(..)
     */
    return conn->protocol->cb_read(conn, sched);
}

int mk_sched_event_write(struct mk_sched_conn *conn,
                         struct mk_sched_worker *sched)
{
    int ret = -1;
    int socket = conn->event.fd;

    MK_TRACE("[FD %i] Connection Handler / write", socket);

    ret = conn->protocol->cb_write(conn, sched);

    /*
     * if ret < 0, means that some error happened in the writer call,
     * in the other hand, 0 means a successful request processed, if
     * ret > 0 means that some data still need to be send.
     */
    if (ret == MK_CHANNEL_ERROR) {
        mk_sched_drop_connection(socket);
        return -1;
    }
    else if (ret == MK_CHANNEL_DONE) {
        MK_TRACE("[FD %i] Request End", socket);
        return mk_http_request_end(conn, sched);
    }
    else if (ret == MK_CHANNEL_FLUSH) {
        return 0;
    }

    /* avoid to make gcc cry :_( */
    return -1;
}

int mk_sched_event_close(struct mk_sched_conn *conn,
                         struct mk_sched_worker *worker,
                         int type)
{
    int socket = conn->event.fd;

    MK_TRACE("[FD %i] Connection Handler, closed", socket);

    conn->protocol->cb_close(conn, worker, type);

    /*
     * Remove the socket from the scheduler and make sure
     * to disable all notifications.
     */
    mk_sched_drop_connection(socket);
    return 0;
}
