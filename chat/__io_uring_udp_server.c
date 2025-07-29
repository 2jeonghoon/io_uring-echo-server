#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>

#include "liburing.h"

#define QD 1024
#define BUF_SHIFT 12
#define CQES (QD * 16)
#define BUFFERS CQES
#define MAX_CLIENTS 1024
#define CONTROLLEN 0

struct sendmsg_ctx {
    struct msghdr msg;
    struct iovec iov;
};

struct client_info {
    struct sockaddr_storage addr;
    socklen_t addr_len;
};

struct ctx {
    struct io_uring ring;
    struct io_uring_buf_ring *buf_ring;
    unsigned char *buffer_base;
    struct msghdr msg;
    int buf_shift;
    int af;
    bool verbose;
    struct sendmsg_ctx send[BUFFERS];
    size_t buf_ring_size;
    struct client_info clients[MAX_CLIENTS];
    size_t client_count;
};

static size_t buffer_size(struct ctx *ctx) {
    return 1U << ctx->buf_shift;
}

static unsigned char *get_buffer(struct ctx *ctx, int idx) {
    return ctx->buffer_base + (idx << ctx->buf_shift);
}

static int match_client(struct sockaddr_storage *a, socklen_t len, struct sockaddr_storage *b, socklen_t blen) {
    return (len == blen) && (memcmp(a, b, len) == 0);
}

static void add_client(struct ctx *ctx, struct sockaddr_storage *addr, socklen_t len) {

    char ip[INET6_ADDRSTRLEN];
    void *src = (addr->ss_family == AF_INET)
                  ? (void *)&((struct sockaddr_in *)addr)->sin_addr
                  : (void *)&((struct sockaddr_in6 *)addr)->sin6_addr;
    inet_ntop(addr->ss_family, src, ip, sizeof(ip));
    int port = (addr->ss_family == AF_INET)
                 ? ntohs(((struct sockaddr_in *)addr)->sin_port)
                 : ntohs(((struct sockaddr_in6 *)addr)->sin6_port);

    fprintf(stderr, "[DEBUG] New client [%s]:%d added (count = %zu)\n", ip, port, ctx->client_count + 1);

    for (size_t i = 0; i < ctx->client_count; i++) {
        if (match_client(addr, len, &ctx->clients[i].addr, ctx->clients[i].addr_len)) {
            return;
        }
    }

    if (ctx->client_count < MAX_CLIENTS) {
        ctx->clients[ctx->client_count].addr = *addr;
        ctx->clients[ctx->client_count].addr_len = len;
        ctx->client_count++;
    }
}

static void broadcast_to_all(struct ctx *ctx, int fdidx, const void *data, size_t len) {
    for (size_t i = 0; i < ctx->client_count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            io_uring_submit(&ctx->ring);
            sqe = io_uring_get_sqe(&ctx->ring);
            if (!sqe) continue;
        }

        ctx->send[i].iov = (struct iovec){ .iov_base = (void *)data, .iov_len = len };
        ctx->send[i].msg = (struct msghdr){
            .msg_name = &ctx->clients[i].addr,
            .msg_namelen = ctx->clients[i].addr_len,
            .msg_iov = &ctx->send[i].iov,
            .msg_iovlen = 1
        };

		char ip[INET6_ADDRSTRLEN];
        void *src = (ctx->clients[i].addr.ss_family == AF_INET)
                      ? (void *)&((struct sockaddr_in *)&ctx->clients[i].addr)->sin_addr
                      : (void *)&((struct sockaddr_in6 *)&ctx->clients[i].addr)->sin6_addr;
        inet_ntop(ctx->clients[i].addr.ss_family, src, ip, sizeof(ip));

        int port = (ctx->clients[i].addr.ss_family == AF_INET)
                     ? ntohs(((struct sockaddr_in *)&ctx->clients[i].addr)->sin_port)
                     : ntohs(((struct sockaddr_in6 *)&ctx->clients[i].addr)->sin6_port);

        fprintf(stderr, "Broadcasting to [%s]:%d, %zu bytes\n", ip, port, len);

        io_uring_prep_sendmsg(sqe, fdidx, &ctx->send[i].msg, 0);
        io_uring_sqe_set_data64(sqe, i);
    }
}

static int setup_buffer_pool(struct ctx *ctx) {
    void *mapped;
    struct io_uring_buf_reg reg;

    ctx->buf_ring_size = (sizeof(struct io_uring_buf) + buffer_size(ctx)) * BUFFERS;
    mapped = mmap(NULL, ctx->buf_ring_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (mapped == MAP_FAILED) return -1;

    ctx->buf_ring = (struct io_uring_buf_ring *)mapped;
    ctx->buffer_base = (unsigned char *)ctx->buf_ring + sizeof(struct io_uring_buf) * BUFFERS;

    io_uring_buf_ring_init(ctx->buf_ring);
    reg = (struct io_uring_buf_reg){
        .ring_addr = (unsigned long)ctx->buf_ring,
        .ring_entries = BUFFERS,
        .bgid = 0
    };

    if (io_uring_register_buf_ring(&ctx->ring, &reg, 0)) return -1;

    for (int i = 0; i < BUFFERS; i++) {
        io_uring_buf_ring_add(ctx->buf_ring, get_buffer(ctx, i), buffer_size(ctx), i,
                              io_uring_buf_ring_mask(BUFFERS), i);
    }
    io_uring_buf_ring_advance(ctx->buf_ring, BUFFERS);
    return 0;
}

static int add_recv(struct ctx *ctx, int sockfd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) return -1;

    io_uring_prep_recvmsg_multishot(sqe, sockfd, &ctx->msg, MSG_TRUNC);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    io_uring_sqe_set_data64(sqe, BUFFERS + 1);
    return 0;
}

static void recycle_buffer(struct ctx *ctx, int idx) {
    io_uring_buf_ring_add(ctx->buf_ring, get_buffer(ctx, idx), buffer_size(ctx), idx,
                          io_uring_buf_ring_mask(BUFFERS), 0);
    io_uring_buf_ring_advance(ctx->buf_ring, 1);
}

static int process_cqe(struct ctx *ctx, struct io_uring_cqe *cqe, int sockfd) {
	printf("process_cqe\n");
    uint64_t user_data = cqe->user_data;

    if (user_data < BUFFERS) {
        int idx = user_data;
        if (cqe->res < 0)
            fprintf(stderr, "send error: %s\n", strerror(-cqe->res));
        recycle_buffer(ctx, idx);
        return 0;
    }

    int idx = cqe->flags >> 16;
    if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res <= 0) {
        fprintf(stderr, "recv error: %d\n", cqe->res);
        return -1;
    }

    struct io_uring_recvmsg_out *o = io_uring_recvmsg_validate(get_buffer(ctx, idx), cqe->res, &ctx->msg);
    if (!o) return -1;

    add_client(ctx, io_uring_recvmsg_name(o), o->namelen);
    broadcast_to_all(ctx, sockfd, io_uring_recvmsg_payload(o, &ctx->msg), io_uring_recvmsg_payload_length(o, cqe->res, &ctx->msg));
    recycle_buffer(ctx, idx);
    return 0;
}

int main(int argc, char *argv[]) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8050), .sin_addr.s_addr = INADDR_ANY };
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    struct ctx ctx = { .af = AF_INET, .buf_shift = BUF_SHIFT, .client_count = 0 };
    if (io_uring_queue_init(QD, &ctx.ring, 0) < 0) { perror("io_uring_queue_init"); return 1; }
    if (setup_buffer_pool(&ctx) < 0) { perror("setup_buffer_pool"); return 1; }

    static struct sockaddr_storage peer_addr;

	ctx.msg.msg_name = &peer_addr;
	ctx.msg.msg_namelen = sizeof(struct sockaddr_storage);
    ctx.msg.msg_controllen = CONTROLLEN;

    if (io_uring_register_files(&ctx.ring, &sockfd, 1) < 0) { perror("register_files"); return 1; }
    if (add_recv(&ctx, 0) < 0) { perror("add_recv"); return 1; }

    while (1) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ctx.ring, &cqe);
        if (ret == 0) {
            process_cqe(&ctx, cqe, 0);
            io_uring_cqe_seen(&ctx.ring, cqe);
        }
    }
}

