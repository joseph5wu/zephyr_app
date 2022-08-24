#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(my_echo_server, LOG_LEVEL_DBG);

#include <zephyr/zephyr.h>
#include <zephyr/net/socket.h>

#include <errno.h>
#include <string.h>

#include "common.h"


static void process_tcp4(void);
static void process_tcp6(void);

// tcp4 threads resources
static bool tcp4_handler_in_use[CONFIG_NET_NUM_HANDLERS];
K_THREAD_DEFINE(tcp4_thread_id, STACK_SIZE, process_tcp4, NULL, NULL, NULL, THREAD_PRIORITY, 0, -1);
K_THREAD_STACK_ARRAY_DEFINE(tcp4_handler_stack, CONFIG_NET_NUM_HANDLERS, STACK_SIZE);
static struct k_thread tcp4_handler_thread[CONFIG_NET_NUM_HANDLERS];

// tcp6 resources
static bool tcp6_handler_in_use[CONFIG_NET_NUM_HANDLERS];
K_THREAD_DEFINE(tcp6_thread_id, STACK_SIZE, process_tcp6, NULL, NULL, NULL, THREAD_PRIORITY, 0, -1);
K_THREAD_STACK_ARRAY_DEFINE(tcp6_handler_stack, CONFIG_NET_NUM_HANDLERS, STACK_SIZE);
static struct k_thread tcp6_handler_thread[CONFIG_NET_NUM_HANDLERS];

void print_tcp_stats(struct k_work* work) {
    struct k_work_delayable* dwork = k_work_delayable_from_work(work);
    struct data* data = CONTAINER_OF(dwork, struct data, tcp.stats_print);
    int total_received = atomic_get(&data->tcp.bytes_received);

    if (total_received) {
        if ((total_received / STATS_TIMER) < 1024) {
            LOG_INF("%s TCP: Received %d B/sec", data->proto, total_received / STATS_TIMER);
        } else {
            LOG_INF("%s TCP: Received %d KB/sec", data->proto, total_received / STATS_TIMER / 1024);
        }

        atomic_set(&data->tcp.bytes_received, 0);
    }

    k_work_reschedule(&data->tcp.stats_print, K_SECONDS(STATS_TIMER));
}

static int get_free_slot(struct data* data) {
    int i;
    for (i = 0; i < CONFIG_NET_NUM_HANDLERS; ++i) {
        if (data->tcp.accepted[i].sock < 0) {
            return i;
        }
    }
    return -1;
}

static ssize_t sendall(int sock, const void* buf, size_t len) {
    while (len) {
        ssize_t out_len = send(sock, buf, len, 0);
        if (out_len < 0) {
            return out_len;
        }
        buf = (const char*)buf + out_len;
        len -= out_len;
    }
    return 0;
}

static int start_tcp_proto(struct data* data, struct sockaddr* bind_addr, socklen_t bind_addrlen) {
    int ret;
    data->tcp.sock = socket(bind_addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (data->tcp.sock < 0) {
        LOG_ERR("Failed to creat TCP socket (%s): %d", data->proto, errno);
        return -errno;
    }
    LOG_INF("Created TCP socket (%s): %d", data->proto, data->tcp.sock);
    // Can set TLS socket option
    ret = bind(data->tcp.sock, bind_addr, bind_addrlen);
    if (ret < 0) {
        LOG_ERR("Failed to bind TCP socket (%s): %d", data->proto, errno);
        return -errno;
    }
    ret = listen(data->tcp.sock, CONFIG_NET_NUM_HANDLERS);
    if (ret < 0) {
        LOG_ERR("Failed to listen on TCP socket (%s): %d", data->proto, errno);
        return -errno;
    }
    return ret;
}

static void handle_data(void* slot_ptr, void* data_ptr, void* handler_in_use) {
    int slot = POINTER_TO_INT(slot_ptr);
    struct data* data = data_ptr;
    bool* in_use = handler_in_use;
    int offset = 0;
    int received;
    int client;
    int ret;

    client = data->tcp.accepted[slot].sock;
    do {
        received = recv(
            client,
            data->tcp.accepted[slot].recv_buffer + offset,
            sizeof(data->tcp.accepted[slot].recv_buffer) - offset,
            0
        );
        if (received == 0) {
            /* Connection closed */
            LOG_INF("TCP (%s): Connection closed", data->proto);
            break;
        } else if (received < 0) {
            /* Socket error */
            LOG_ERR("TCP (%s): Connection error %d", data->proto, errno);
            break;
        } else {
            atomic_add(&data->tcp.bytes_received, received);
        }
        offset += received;

        // To prevent fragmentation of the response, reply only if buffer is full or
        // there is no more data to read
        if (offset == sizeof(data->tcp.accepted[slot].recv_buffer) ||
            (recv(client,
              data->tcp.accepted[slot].recv_buffer + offset,
              sizeof(data->tcp.accepted[slot].recv_buffer) - offset,
              MSG_PEEK | MSG_DONTWAIT) < 0 &&
             (errno == EAGAIN || errno == EWOULDBLOCK))) {
            ret = sendall(client, data->tcp.accepted[slot].recv_buffer, offset);
            if (ret < 0) {
                LOG_ERR("TCP (%s): Failed to send, closing socket", data->proto);
                break;
            }

            LOG_DBG("TCP (%s): Received and replied with %d bytes", data->proto, offset);

            if (++data->tcp.accepted[slot].counter % 1000 == 0U) {
                LOG_INF("%s TCP: Sent %u packets", data->proto, data->tcp.accepted[slot].counter);
            }

            offset = 0;
        }
    } while (true);

    // Release resources
    *in_use = false;
    (void)close(client);
    data->tcp.accepted[slot].sock = -1;
}

static int process_tcp(struct data* data) {
    int client;
    int slot;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    LOG_INF("Waiting for TCP connection on port %d (%s)....",
        MY_PORT, data->proto);

    client = accept(data->tcp.sock, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client < 0) {
        LOG_ERR("%s accept error (%d)", data->proto, -errno);
        return 0;
    }

    slot = get_free_slot(data);
    if (slot < 0) {
        LOG_ERR("Cannot accept more connections");
        close(client);
        return 0;
    }

 #define MAX_NAME_LEN sizeof("tcp6[0]")

    data->tcp.accepted[slot].sock = client;
    LOG_INF("TCP (%s): Accepted connection", data->proto);

    if (client_addr.sin_family == AF_INET6) {
        tcp6_handler_in_use[slot] = true;

        k_thread_create(
            &tcp6_handler_thread[slot],
            tcp6_handler_stack[slot],
            K_THREAD_STACK_SIZEOF(tcp6_handler_stack[slot]),
            (k_thread_entry_t)handle_data, INT_TO_POINTER(slot), data, &tcp6_handler_in_use[slot],
            THREAD_PRIORITY,
            0,
            K_NO_WAIT
        );

        char name[MAX_NAME_LEN];
        snprintk(name, sizeof(name), "tcp6[%d]", slot);
        k_thread_name_set(&tcp4_handler_thread[slot], name);
    } else if (client_addr.sin_family == AF_INET) {
        tcp4_handler_in_use[slot] = true;

        k_thread_create(
            &tcp4_handler_thread[slot],
            tcp4_handler_stack[slot],
            K_THREAD_STACK_SIZEOF(tcp4_handler_stack[slot]),
            (k_thread_entry_t)handle_data, INT_TO_POINTER(slot), data, &tcp4_handler_in_use[slot],
            THREAD_PRIORITY,
            0,
            K_NO_WAIT
        );

        char name[MAX_NAME_LEN];
        snprintk(name, sizeof(name), "tcp4[%d]", slot);
        k_thread_name_set(&tcp4_handler_thread[slot], name);
    }

    return 0;
}

static void process_tcp4(void) {
    int ret;
    struct sockaddr_in addr4;

    (void)memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(MY_PORT);

    ret = start_tcp_proto(&conf.ipv4, (struct sockaddr*)&addr4, sizeof(addr4));
    if (ret < 0) {
        quit();
        return;
    }

    k_work_reschedule(&conf.ipv4.tcp.stats_print, K_SECONDS(STATS_TIMER));

    while (ret == 0) {
        ret = process_tcp(&conf.ipv4);
        if (ret < 0) {
            break;
        }
    }

    quit();
}

static void process_tcp6(void) {
    int ret;
    struct sockaddr_in6 addr6;

    (void)memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(MY_PORT);

    ret = start_tcp_proto(&conf.ipv6, (struct sockaddr*)&addr6, sizeof(addr6));
    if (ret < 0) {
        quit();
        return;
    }

    k_work_reschedule(&conf.ipv6.tcp.stats_print, K_SECONDS(STATS_TIMER));

    while(ret == 0) {
        ret = process_tcp(&conf.ipv6);
        if (ret != 0) {
            break;
        }
    }

    quit();
}


void start_tcp(void) {
    LOG_INF("Starting TCP...");

    int i;
    for (i = 0; i < CONFIG_NET_NUM_HANDLERS; ++i) {
        conf.ipv4.tcp.accepted[i].sock = -1;
        tcp4_handler_in_use[i] = false;

        conf.ipv6.tcp.accepted[i].sock = -1;
        tcp6_handler_in_use[i] = false;
    }

    // user space settings?
    k_work_init_delayable(&conf.ipv6.tcp.stats_print, print_tcp_stats);
    k_thread_start(tcp6_thread_id);

    k_work_init_delayable(&conf.ipv4.tcp.stats_print, print_tcp_stats);
    k_thread_start(tcp4_thread_id);
}

void stop_tcp(void) {
    int i;

    // Not very graceful way to close a thread.
    // But as we may be blocked in accept or recv, call it seems to be necessary
    k_thread_abort(tcp6_thread_id);
    if (conf.ipv6.tcp.sock >= 0) {
        (void)close(conf.ipv6.tcp.sock);
    }
    for (i = 0; i < CONFIG_NET_NUM_HANDLERS; ++i) {
        if (tcp6_handler_in_use[i] == true) {
            k_thread_abort(&tcp6_handler_thread[i]);
        }
        if (conf.ipv6.tcp.accepted[i].sock >= 0) {
            (void)close(conf.ipv6.tcp.accepted[i].sock);
        }
    }

    k_thread_abort(tcp4_thread_id);
    if (conf.ipv4.tcp.sock >= 0) {
        (void)close(conf.ipv4.tcp.sock);
    }
    for (i = 0; i < CONFIG_NET_NUM_HANDLERS; ++i) {
        if (tcp4_handler_in_use[i] == true) {
            k_thread_abort(&tcp4_handler_thread[i]);
        }
        if (conf.ipv4.tcp.accepted[i].sock >= 0) {
            (void)close(conf.ipv4.tcp.accepted[i].sock);
        }
    }
}
