#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_echo_server, LOG_LEVEL_DBG);

#include <zephyr/zephyr.h>
#include <zephyr/net/net_config.h>
#include <zephyr/net/net_conn_mgr.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>

#include "common.h"

#define APP_BANNER "Run Echo Server"
#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

// echo_server app conf
static struct k_sem quit_lock;
static bool connected;
K_SEM_DEFINE(run_app, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static bool want_to_quit;

struct configs conf = {
    .ipv4 = {
        .proto = "IPv4",
    },
    .ipv6 = {
        .proto = "IPv6",
    },
};

void quit(void) {
    k_sem_give(&quit_lock);
}

static void event_handler(struct net_mgmt_event_callback* cb, uint32_t mgmt_event, struct net_if* iface) {
    if ((mgmt_event & EVENT_MASK) != mgmt_event) {
        return;
    }

    if (want_to_quit) {
        want_to_quit = false;
        k_sem_give(&run_app);
    }

    // TODO: Tunneling is handled seperately, so ignore it here

    if (mgmt_event == NET_EVENT_L4_CONNECTED) {
        LOG_INF("Network connected");

        connected = true;
        k_sem_give(&run_app);

        return;
    } else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
        if (connected == false) {
            LOG_INF("Waiting network to be connected");
        } else {
            LOG_INF("Network disconnected");
            connected = false;
        }

        k_sem_reset(&run_app);

        return;
    }
}

static void init_app(void) {
    LOG_INF(APP_BANNER);

    k_sem_init(&quit_lock, 0, K_SEM_MAX_LIMIT);

    if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
        net_mgmt_init_event_callback(&mgmt_cb, event_handler, EVENT_MASK);
        net_mgmt_add_event_callback(&mgmt_cb);

        net_conn_mgr_resend_status();
    }
}

void main(void) {
    init_app();

    /* Wait for the connection. */
	k_sem_take(&run_app, K_FOREVER);

    start_tcp();

    k_sem_take(&quit_lock, K_FOREVER);

    if (connected) {
        stop_tcp();
    }
}
