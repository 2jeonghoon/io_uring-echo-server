#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdint.h>

#define MAX_CONNECTIONS 4096
#define BACKLOG 1024
#define QUEUE_SIZE 256
#define CQE_MULTIPLIER 16
#define MAX_MESSAGE_LEN 2048
//#define IORING_FEAT_FAST_POLL (1U << 5)
#define MAX_MEASUREMENTS 100000000

void add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags);
void add_socket_read(struct io_uring* ring, int fd, size_t size, unsigned flags);
void add_socket_write(struct io_uring* ring, int fd, size_t size, unsigned flags);

static inline uint64_t get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // or CLOCK_MONOTONIC_RAW
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
enum {
    ACCEPT,
    POLL_LISTEN,
    POLL_NEW_CONNECTION,
    READ,
    WRITE,
};

typedef struct conn_info
{
    unsigned fd;
    unsigned type;
} conn_info;

conn_info conns[MAX_CONNECTIONS];
char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN];
uint64_t latency_array[MAX_MEASUREMENTS];
int latency_index = 0;
int submission = 0;
int completion = 0;
int completion_max = 0;
int cur_state = 0;

void dump_latency_to_file(const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen");
        return;
    }

    for (int i = 0; i < latency_index; i++) {
        fprintf(fp, "%llu\n", (unsigned long long)latency_array[i]);
    }

    fclose(fp);
}

void signal_handler(int signum) {
	if (signum == SIGINT) {
		dump_latency_to_file("latency_dump.txt");
		printf("submission:%d, completion:%d _max:%d\n", submission, completion, completion_max);
		printf("cur state:%d\n", cur_state);
		exit(0);
	}
}


int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Please give a port number: ./io_uring_echo_server [port]\n");
        exit(0);
    }

	signal(SIGINT, signal_handler);

    // some variables we need
    int portno = strtol(argv[1], NULL, 10);
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // setup socket
    int sock_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	//inet_pton(AF_INET, "192.168.1.101", &serv_addr.sin_addr);

    // bind and listen
    if (bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Error binding socket..\n");
        exit(1);
    }
    if (listen(sock_listen_fd, BACKLOG) < 0)
    {
        perror("Error listening..\n");
        exit(1);
    }
    printf("io_uring echo server listening for connections on port: %d\n", portno);

    // initialize io_uring
    struct io_uring_params params;
    struct io_uring ring;
    memset(&params, 0, sizeof(params));

	params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_CQSIZE;
	params.sq_thread_idle = 999999;
	params.cq_entries = QUEUE_SIZE * CQE_MULTIPLIER;
	
	printf("%d ", params.cq_entries);

    if (io_uring_queue_init_params(QUEUE_SIZE, &ring, &params) < 0)
    {
        perror("io_uring_init_failed...\n");
        exit(1);
    }

	printf("initial queue size:%d\n", ring.sq.ring_entries);
	printf("initial queue size:%d\n", ring.cq.ring_entries);
    unsigned *sq_head = ring.sq.khead;
    unsigned *sq_tail = ring.sq.ktail;
    unsigned *cq_head = ring.cq.khead;
    unsigned *cq_tail = ring.cq.ktail;

    printf("SQ head: %u\n", *sq_head);
    printf("SQ tail: %u\n", *sq_tail);
    printf("CQ head: %u\n", *cq_head);
    printf("CQ tail: %u\n", *cq_tail);
    if (!(params.features & IORING_FEAT_FAST_POLL))
    {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        exit(0);
    }

    // add first accept sqe to monitor for new incoming connections
    add_accept(&ring, sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);

    // start event loop
    while (1)
    {
        struct io_uring_cqe *cqe;
        int ret;

        // tell kernel we have put a sqe on the submission ring
        io_uring_submit(&ring);

        // wait for new cqe to become available
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret != 0)
        {
            perror("Error io_uring_wait_cqe\n");
            exit(1);
        }

        // check how many cqe's are on the cqe ring at this moment
        struct io_uring_cqe *cqes[QUEUE_SIZE*CQE_MULTIPLIER];
        int cqe_count = io_uring_peek_batch_cqe(&ring, cqes, sizeof(cqes) / sizeof(cqes[0]));
		completion += cqe_count;
		
		cur_state = 0;

		if (cqe_count > completion_max) {
			completion_max = cqe_count;
		}

        // go through all the cqe's
        for (int i = 0; i < cqe_count; ++i)
        {
            struct io_uring_cqe *cqe = cqes[i];
			if (!cqe) {
				printf("cqe null\n");
			}
            struct conn_info *user_data = (struct conn_info *)io_uring_cqe_get_data(cqe);
			if (!user_data) {
				printf("user_data null. CQE details: res=%d, flags=0x%x\n", cqe->res, cqe->flags);
			}
            int type = user_data->type;

			if (cqe->res < 0) {
				printf("Error in CQE: type = %d, fd = %d, res = %d(%s), cqe_flags = %u\n", type, user_data->fd, cqe->res, strerror(-cqe->res), cqe->flags);
			}

            if (type == ACCEPT)
            {
                int sock_conn_fd = cqe->res;
                io_uring_cqe_seen(&ring, cqe);

                // new connected client; read data from socket and re-add accept to monitor for new connections
                add_socket_read(&ring, sock_conn_fd, MAX_MESSAGE_LEN, 0);
                add_accept(&ring, sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);
            }
            else if (type == READ)
            {
                int bytes_read = cqe->res;
                if (bytes_read <= 0)
                {
                    // no bytes available on socket, client must be disconnected
                    io_uring_cqe_seen(&ring, cqe);
                    shutdown(user_data->fd, SHUT_RDWR);
                }
                else
                {
                    // bytes have been read into bufs, now add write to socket sqe
                    io_uring_cqe_seen(&ring, cqe);
                    add_socket_write(&ring, user_data->fd, bytes_read, 0);
                }
            }
            else if (type == WRITE)
            {
                // write to socket completed, re-add socket read
                io_uring_cqe_seen(&ring, cqe);
                add_socket_read(&ring, user_data->fd, MAX_MESSAGE_LEN, 0);
            }
        }
    }
}

void add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe) {
		uint64_t start = get_ns();

        io_uring_submit(ring);
		while (!sqe) 
			sqe = io_uring_get_sqe(ring);
		
		uint64_t end = get_ns();

		if (latency_index < MAX_MEASUREMENTS)
			latency_array[latency_index++] = end - start;
	}

    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = ACCEPT;

    io_uring_sqe_set_data(sqe, conn_i);
	submission++;
}

void add_socket_read(struct io_uring *ring, int fd, size_t size, unsigned flags)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe) {
		uint64_t start = get_ns();
        io_uring_submit(ring);
		cur_state = 1;
		while (!sqe) {
			sqe = io_uring_get_sqe(ring);
		}
		cur_state = 0;
		uint64_t end = get_ns();

		if (latency_index < MAX_MEASUREMENTS) {
			latency_array[latency_index++] = end - start;
		}
	}

	io_uring_prep_recv(sqe, fd, &bufs[fd], size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = READ;

    io_uring_sqe_set_data(sqe, conn_i);
	submission++;
}

void add_socket_write(struct io_uring *ring, int fd, size_t size, unsigned flags)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe) {
		uint64_t start = get_ns();
	
		io_uring_submit(ring);

		cur_state = 1;
		while (!sqe) {
			sqe = io_uring_get_sqe(ring);
		}
		cur_state = 0;
		
		uint64_t end = get_ns();

		if (latency_index < MAX_MEASUREMENTS) {
			latency_array[latency_index++] = end - start;
		}
	}

    io_uring_prep_send(sqe, fd, &bufs[fd], size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WRITE;

    io_uring_sqe_set_data(sqe, conn_i);
	submission++;
}
