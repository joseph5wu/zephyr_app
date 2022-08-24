#define MY_PORT 4242
#define RECV_BUFFER_SIZE 1280
// How often do we print statistics (in seconds)
#define STATS_TIMER 60
#define STACK_SIZE 4096
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)

struct data {
    const char* proto;

    struct {
        int sock;
        atomic_t bytes_received;
        struct k_work_delayable stats_print;

        struct {
            int sock;
            char recv_buffer[RECV_BUFFER_SIZE];
            uint32_t counter;
        } accepted[CONFIG_NET_NUM_HANDLERS];
    } tcp;
};

struct configs {
    struct data ipv4;
    struct data ipv6;
};

extern struct configs conf;

void quit(void);

void start_tcp(void);
void stop_tcp(void);
