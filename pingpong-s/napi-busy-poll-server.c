/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <liburing.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>

#define MAXBUFLEN 1024
#define MAX_CLIENTS 8192
#define RINGSIZE 1024
#define CQE_MULTIPLIER 2
#define MAX_MEASUREMENTS 200000000
#define PORTNOLEN 10
#define ADDRLEN   80

#define printable(ch) (isprint((unsigned char)ch) ? ch : '#')

struct options
{
	__u32 timeout;

	bool listen;
	bool defer_tw;
	bool sq_poll;
	bool busy_loop;
	bool prefer_busy_poll;
	bool ipv6;

	char port[PORTNOLEN];
	char addr[ADDRLEN];
};

static struct options opt;

static struct option longopts[] =
{
	{"address"  , 1, NULL, 'a'},
	{"busy"     , 0, NULL, 'b'},
	{"help"     , 0, NULL, 'h'},
	{"listen"   , 0, NULL, 'l'},
	{"port"     , 1, NULL, 'p'},
	{"prefer"   , 1, NULL, 'u'},
	{"sqpoll"   , 0, NULL, 's'},
	{"timeout"  , 1, NULL, 't'},
	{NULL       , 0, NULL,  0 }
};

enum {
	IOURING_RECV,
	IOURING_SEND,
	IOURING_RECVMSG,
	IOURING_SENDMSG
};

struct client_info {
	struct sockaddr_storage addr;
	socklen_t addr_len;
	bool active;
};

struct ctx {
	struct io_uring ring;
	struct sockaddr_storage saddr;
	struct iovec iov;
	struct msghdr msg;
	int sockfd;
	bool napi_check;
	char buffer[MAXBUFLEN];
	struct client_info clients[MAX_CLIENTS];
};

struct sendmsg_data {
	char type;
	int fd;
	struct msghdr msg;
	struct iovec iov;
	char *buf;
};

int latency_index = 0;
uint64_t latency_array[MAX_MEASUREMENTS];
bool server_exit = false;

static void dump_latency_to_file(const char *base_filename) {
	char filename[256];
	snprintf(filename, sizeof(filename), "%s.txt", base_filename);
	FILE *fp = fopen(filename, "w");
	if (!fp) {
		perror("fopen");
		return;
	}

	printf("%d\n", latency_index);

	for (int i = 0; i < latency_index; i++) {
		fprintf(fp, "%llu\n", (unsigned long long)latency_array[i]);
	}

	fprintf(fp, "fin\n");
	fclose(fp);
}

void signal_handler(int signum) {
	if (signum == SIGINT) {
		dump_latency_to_file("blocking_time");	

		exit(0);
	}
}

static void printUsage(const char *name)
{
	fprintf(stderr,
	"Usage: %s [-l|--listen] [-a|--address ip_address] [-p|--port port-no] [-s|--sqpoll]"
	" [-b|--busy] [-n|--num pings] [-t|--timeout busy-poll-timeout] [-u|--prefer] [-6] [-h|--help]\n"
	" --listen\n"
	"-l        : Server mode\n"
	"--address\n"
	"-a        : remote or local ipv6 address\n"
	"--busy\n"
	"-b        : busy poll io_uring instead of blocking.\n"
	"--num_pings\n"
	"-n        : number of pings\n"
	"--port\n"
	"-p        : port\n"
	"--sqpoll\n"
	"-s        : Configure io_uring to use SQPOLL thread\n"
	"--timeout\n"
	"-t        : Configure NAPI busy poll timeout"
	"--prefer\n"
	"-u        : prefer NAPI busy poll\n"
	"-6        : use IPV6\n"
	"--help\n"
	"-h        : Display this usage message\n\n",
	name);
}

static void printError(const char *msg, int opt)
{
	if (msg && opt)
		fprintf(stderr, "%s (-%c)\n", msg, printable(opt));
}

static void reportNapi(struct ctx *ctx)
{
	unsigned int napi_id = 0;
	socklen_t len = sizeof(napi_id);

	getsockopt(ctx->sockfd, SOL_SOCKET, SO_INCOMING_NAPI_ID, &napi_id, &len);
	if (napi_id)
		printf(" napi id: %d\n", napi_id);
	else
		printf(" unassigned napi id\n");

	ctx->napi_check = true;
}

static inline uint64_t get_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t encodeUserData(char type, int fd) {
	return (uint32_t)fd | ((__u64)type << 56);
}

static void decodeUserData(uint64_t data, char *type, int *fd) {
	*type = data >> 56;
	*fd = data & 0xffffffffU;
}

static bool sockaddr_equal(struct sockaddr_storage *a, socklen_t alen,
                           struct sockaddr_storage *b, socklen_t blen) {
    if (a->ss_family != b->ss_family)
        return false;

    if (a->ss_family == AF_INET && alen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sa = (struct sockaddr_in *)a;
        struct sockaddr_in *sb = (struct sockaddr_in *)b;
        return sa->sin_port == sb->sin_port &&
               sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    } else if (a->ss_family == AF_INET6 && alen >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)a;
        struct sockaddr_in6 *sb6 = (struct sockaddr_in6 *)b;
        return sa6->sin6_port == sb6->sin6_port &&
               memcmp(&sa6->sin6_addr, &sb6->sin6_addr, sizeof(struct in6_addr)) == 0;
    }

    return false;
}

static int find_or_add_client(struct ctx *ctx, struct sockaddr_storage *addr, socklen_t len) {
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!ctx->clients[i].active) continue;
		if (sockaddr_equal(&ctx->clients[i].addr, ctx->clients[i].addr_len, addr, len)) 
			return i;
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!ctx->clients[i].active) {
			ctx->clients[i].addr_len = len;
			memcpy(&ctx->clients[i].addr, addr, len);
			ctx->clients[i].active = true;
			return i;
		}
	}
	return -1;
}

static struct io_uring_sqe* io_uring_sqe_overflow(struct io_uring *ring) {
	uint64_t start  = get_ns();
	struct io_uring_sqe *sqe;

	io_uring_submit(ring);

	do {	
		sqe = io_uring_get_sqe(ring);
	} while (!sqe);

	uint64_t end = get_ns();

	if (latency_index < MAX_MEASUREMENTS)
			latency_array[latency_index++] = end - start;

	return sqe;

}

static struct io_uring_sqe* get_sqe(struct io_uring *ring) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe) {
			sqe = io_uring_sqe_overflow(ring);
	}

	return sqe;
}

static void receivePing(struct ctx *ctx) {
	struct io_uring_sqe *sqe = get_sqe(&ctx->ring);
	struct sendmsg_data *recv_data = malloc(sizeof(struct sendmsg_data));
	bzero(&ctx->msg, sizeof(ctx->msg));

	recv_data->type = IOURING_RECVMSG;
	recv_data->fd = ctx->sockfd;
	recv_data->buf = malloc(MAXBUFLEN);
	recv_data->iov.iov_base = recv_data->buf;
	recv_data->iov.iov_len = MAXBUFLEN;

	recv_data->msg.msg_name = malloc(sizeof(struct sockaddr_storage));
	recv_data->msg.msg_namelen = sizeof(struct sockaddr_storage);
	recv_data->msg.msg_iov = &recv_data->iov;
	recv_data->msg.msg_iovlen = 1;

	io_uring_prep_recvmsg(sqe, ctx->sockfd, &recv_data->msg, 0);
	sqe->user_data = (uintptr_t)recv_data;
}

static void completion(struct ctx *ctx, struct io_uring_cqe *cqe) {

	struct sendmsg_data *data = (void *)(uintptr_t)cqe->user_data;
	if (cqe->res < 0) {
		int err = -cqe->res;

		if (data) {
			if (data->type == IOURING_SENDMSG) {
				fprintf(stderr, "sendmsg 실패: %s\n", strerror(err));
				free(data->msg.msg_name);
				free(data->buf);
				free(data);
			} else if (data->type == IOURING_RECVMSG) {
				fprintf(stderr, "recvmsg 실패: %s\n", strerror(err));
				free(data);
			} else {
				fprintf(stderr, "Unknown data type in error: %d (%s)\n", data->type, strerror(err));
				free(data);
			}
		} else {
			fprintf(stderr, "cqe->res < 0, but user_data is NULL? (%s)\n", strerror(err));
		}

		return;
	}


	if (data->type == IOURING_RECVMSG) {
		int len = cqe->res;
		ctx->iov.iov_len = len;

		int sender_idx = find_or_add_client(ctx, (struct sockaddr_storage *)data->msg.msg_name, data->msg.msg_namelen);
		if (sender_idx < 0) return;

		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (!ctx->clients[i].active) continue;

			struct sendmsg_data *send_data = malloc(sizeof(struct sendmsg_data));
			send_data->type = IOURING_SENDMSG;
			send_data->fd = ctx->sockfd;

			send_data->buf = malloc(len);
			memcpy(send_data->buf, data->buf, len);

			send_data->iov.iov_base = send_data->buf;
			send_data->iov.iov_len = len;

			memset(&send_data->msg, 0, sizeof(struct msghdr));
			send_data->msg.msg_name = &ctx->clients[i].addr;
			send_data->msg.msg_namelen = ctx->clients[i].addr_len;
			send_data->msg.msg_iov = &send_data->iov;
			send_data->msg.msg_iovlen = 1;

			struct io_uring_sqe *sqe = get_sqe(&ctx->ring);
			io_uring_prep_sendmsg(sqe, send_data->fd, &send_data->msg, 0);
			sqe->user_data = (uintptr_t)send_data;
		}


		free(data->msg.msg_name);
		free(data->buf);
		free(data);
		receivePing(ctx);

		if (!ctx->napi_check)
			reportNapi(ctx);
	}
	else if (data->type == IOURING_SENDMSG) {
		free(data->buf);
		free(data);
	}
	else {
		fprintf(stderr, "Unknown CQE type: %d\n", data->type);
	}
}

static void setProcessScheduler(void)
{
	struct sched_param param;

	param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &param) < 0)
		fprintf(stderr, "sched_setscheduler() failed: (%d) %s\n",
			errno, strerror(errno));
}

int main(int argc, char *argv[])
{
	int flag;
	struct ctx       ctx;
	struct __kernel_timespec *tsPtr;
	struct __kernel_timespec ts;
	struct io_uring_params params;
	struct io_uring_napi napi;
	int ret, af;

	signal(SIGINT, signal_handler);

	memset(&opt, 0, sizeof(struct options));

	// Process flags.
	while ((flag = getopt_long(argc, argv, ":lhs:bua:n:p:t:6d:", longopts, NULL)) != -1) {
		switch (flag) {
		case 'a':
			strcpy(opt.addr, optarg);
			break;
		case 'b':
			opt.busy_loop = true;
			break;
		case 'h':
			printUsage(argv[0]);
			exit(0);
			break;
		case 'l':
			opt.listen = true;
			break;
		case 'p':
			strcpy(opt.port, optarg);
			break;
		case 's':
			opt.sq_poll = !!atoi(optarg);
			break;
		case 't':
			opt.timeout = atoi(optarg);
			break;
		case 'u':
			opt.prefer_busy_poll = true;
			break;
		case '6':
			opt.ipv6 = true;
			break;
		case 'd':
			opt.defer_tw = !!atoi(optarg);
			break;
		case ':':
			printError("Missing argument", optopt);
			printUsage(argv[0]);
			exit(-1);
			break;
		case '?':
			printError("Unrecognized option", optopt);
			printUsage(argv[0]);
			exit(-1);
			break;

		default:
			fprintf(stderr, "Fatal: Unexpected case in CmdLineProcessor switch()\n");
			exit(-1);
			break;
		}
	}

	if (strlen(opt.addr) == 0) {
		fprintf(stderr, "address option is mandatory\n");
		printUsage(argv[0]);
		exit(1);
	}

	struct sockaddr_in *addr4 = (struct sockaddr_in *)&ctx.saddr;
	addr4->sin_family = AF_INET;
	af = AF_INET;
	addr4->sin_port = htons(atoi(opt.port));
	
	ret = inet_pton(AF_INET, opt.addr, &addr4->sin_addr);

	if (ret <= 0) {
		fprintf(stderr, "inet_pton error for %s\n", optarg);
		printUsage(argv[0]);
		exit(1);
	}

	// Connect to server.
	fprintf(stdout, "Listening %s : %s...\n", opt.addr, opt.port);

	if ((ctx.sockfd = socket(af, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "socket() failed: (%d) %s\n", errno, strerror(errno));
		exit(1);
	}
	
	ret = bind(ctx.sockfd, (struct sockaddr *)&ctx.saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		fprintf(stderr, "bind() failed: (%d) %s\n", errno, strerror(errno));
		exit(1);
	}

	// Setup ring.
	memset(&params, 0, sizeof(params));
	memset(&ts, 0, sizeof(ts));
	memset(&napi, 0, sizeof(napi));

	params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE | IORING_SETUP_SQ_AFF;
	params.cq_entries = RINGSIZE * CQE_MULTIPLIER;
	params.sq_thread_cpu = 0;
	if (opt.defer_tw) {
		params.flags |= IORING_SETUP_DEFER_TASKRUN;
	} else if (opt.sq_poll) {
		params.flags = IORING_SETUP_SQPOLL;
		params.sq_thread_idle = 50;
	} else {
		params.flags |= IORING_SETUP_COOP_TASKRUN;
	}

	ret = io_uring_queue_init_params(RINGSIZE, &ctx.ring, &params);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init_params() failed: (%d) %s\n",
			ret, strerror(-ret));
		exit(1);
	}

    if (params.features & IORING_FEAT_NODROP) {
        printf("✅ IORING_FEAT_NODROP is supported!\n");
    } else {
        printf("❌ IORING_FEAT_NODROP is NOT supported on this kernel.\n");
    }

	if (opt.timeout || opt.prefer_busy_poll) {
		napi.prefer_busy_poll = opt.prefer_busy_poll;
		napi.busy_poll_to = opt.timeout;

		ret = io_uring_register_napi(&ctx.ring, &napi);
		if (ret) {
			fprintf(stderr, "io_uring_register_napi: %d\n", ret);
			exit(1);
		}
	}

	if (opt.busy_loop)
		tsPtr = &ts;
	else
		tsPtr = NULL;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(1, &cpuset);

	pid_t pid = getpid();
	if (sched_setaffinity(pid, sizeof(cpuset), &cpuset) != 0) {
		perror("sched_setaffinity 실패");
		return 1;
	}

	// Use realtime scheduler.
	setProcessScheduler();

	// Setup context.
	ctx.napi_check = false;

	// Receive initial message to get napi id.
	receivePing(&ctx);

	while (!server_exit) {
		int res;
		unsigned int num_completed = 0;
		unsigned int head;
		struct io_uring_cqe *cqe;

		do {
			res = io_uring_submit_and_wait_timeout(&ctx.ring, &cqe, 1, tsPtr, NULL);
			if (res >= 0)
				break;
			else if (res == -ETIME)
				continue;
			fprintf(stderr, "submit_and_wait: %d\n", res);
			exit(1);
		} while (1);

		io_uring_for_each_cqe(&ctx.ring, head, cqe) {
			++num_completed;
			completion(&ctx, cqe);
		}

		if (num_completed)
			io_uring_cq_advance(&ctx.ring, num_completed);
	}

	// Clean up.
	if (opt.timeout || opt.prefer_busy_poll) {
		ret = io_uring_unregister_napi(&ctx.ring, &napi);
		if (ret)
			fprintf(stderr, "io_uring_unregister_napi: %d\n", ret);
	}

	dump_latency_to_file("blocking_time");

	io_uring_queue_exit(&ctx.ring);
	close(ctx.sockfd);
	return 0;
}

