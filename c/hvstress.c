/*
 * A simple Hyper-V sockets stress test.
 *
 * This program uses a configurable number of client threads which all
 * open a connection to a server and then transfer a random amount of
 * data to the server which echos the data back.
 *
 * The send()/recv() calls optionally alternate between RXTX_BUF_LEN
 * and RXTX_SMALL_LEN worth of data to add more variability in the
 * interaction.
 */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3049197C-FACB-11E6-BD58-64006A7986D3 */
DEFINE_GUID(BM_GUID,
    0x3049197c, 0xfacb, 0x11e6, 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3);

/* Maximum amount of data for a single send()/recv() call.
 * Note: On Windows the maximum length seems to be 8KB and if larger
 * buffers are passed to send() the connection will be close. We could
 * use getsockopt(SO_MAX_MSG_SIZE) */
#define RXTX_BUF_LEN (4 * 1024)
/* Small send()/recv() lengths */
#define RXTX_SMALL_LEN 4
/* Maximum amount of data transferred over a single connection */
#define MAX_DATA_LEN (20 * 1024 * 1024)
/* Default number of connections made by the client */
#define DEFAULT_CLIENT_CONN 100

/* Global flag to alternate between short and long send()/recv() buffers */
static int opt_alternate;

static int verbose;
#define INFO(...)                                                       \
    do {                                                                \
        if (verbose) {                                                  \
            printf(__VA_ARGS__);                                        \
            fflush(stdout);                                             \
        }                                                               \
    } while (0)
#define DBG(...)                                                        \
    do {                                                                \
        if (verbose > 1) {                                              \
            printf(__VA_ARGS__);                                        \
            fflush(stdout);                                             \
        }                                                               \
    } while (0)
#define TRC(...)                                                        \
    do {                                                                \
        if (verbose > 2) {                                              \
            printf(__VA_ARGS__);                                        \
            fflush(stdout);                                             \
        }                                                               \
    } while (0)

#ifdef _MSC_VER
static WSADATA wsaData;
#endif


/* Server code
 *
 * The server accepts a new connection and spins of a new thread to
 * handle it. The thread simply echos back the data. We use a thread
 * per connection because it's simpler code, but ideally we should be
 * using a pool of worker threads.
 */

/* Arguments to the server thread */
struct svr_args {
    SOCKET fd;
    int conn;
};

/* Thread entry point for a server */
static void *handle(void *a)
{
    struct svr_args *args = a;
    uint64_t start, end, diff;
    char recvbuf[RXTX_BUF_LEN];
    int total_bytes = 0;
    int rxlen = RXTX_SMALL_LEN;
    int received;
    int sent;
    int res;

    TRC("[%05d] server thread: Handle fd=%d\n", args->conn, (int)args->fd);

    start = time_ns();

    for (;;) {
        if (opt_alternate)
            rxlen = (rxlen == RXTX_SMALL_LEN) ? RXTX_BUF_LEN : RXTX_SMALL_LEN;
        else
            rxlen = RXTX_BUF_LEN;
        received = recv(args->fd, recvbuf, rxlen, 0);
        if (received == 0) {
            DBG("[%05d] Peer closed\n", args->conn);
            break;
        } else if (received == SOCKET_ERROR) {
            sockerr("recv()");
            goto out;
        }

        sent = 0;
        while (sent < received) {
            res = send(args->fd, recvbuf + sent, received - sent, 0);
            if (res == SOCKET_ERROR) {
                sockerr("send()");
                goto out;
            }
            sent += res;
        }
        total_bytes += sent;
    }

    end = time_ns();

out:
    diff = end - start;
    diff /= 1000 * 1000;
    INFO("[%05d] ECHOED: %9d Bytes in %5"PRIu64"ms\n",
         args->conn, total_bytes, diff);
    TRC("close(%d)\n", (int)args->fd);
    closesocket(args->fd);
    free(args);
    return NULL;
}


/* Server entry point */
static int server(int multi_threaded, int max_conn)
{
    SOCKET lsock, csock;
    SOCKADDR_HV sa, sac;
    socklen_t socklen = sizeof(sac);
    struct svr_args *args;
    THREAD_HANDLE st;
    int conn = 0;
    int res;

    lsock = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (lsock == INVALID_SOCKET) {
        sockerr("socket()");
        return 1;
    }

    sa.Family = AF_HYPERV;
    sa.Reserved = 0;
    sa.VmId = HV_GUID_WILDCARD;
    sa.ServiceId = MY_SERVICE_GUID;

    res = bind(lsock, (const struct sockaddr *)&sa, sizeof(sa));
    if (res == SOCKET_ERROR) {
        sockerr("bind()");
        closesocket(lsock);
        return 1;
    }

    res = listen(lsock, SOMAXCONN);
    if (res == SOCKET_ERROR) {
        sockerr("listen()");
        closesocket(lsock);
        return 1;
    }

    while(1) {
        csock = accept(lsock, (struct sockaddr *)&sac, &socklen);
        if (csock == INVALID_SOCKET) {
            sockerr("accept()");
            closesocket(lsock);
            return 1;
        }

        DBG("Connect from: "GUID_FMT":"GUID_FMT" fd=%d\n",
            GUID_ARGS(sac.VmId), GUID_ARGS(sac.ServiceId), (int)csock);

        /* Spin up a new thread per connection. Not the most
         * efficient, but stops us from having to faff about with
         * worker threads and the like. */
        args = malloc(sizeof(*args));
        if (!args) {
            fprintf(stderr, "failed to malloc thread state\n");
            return 1;
        }
        args->fd = csock;
        args->conn = conn++;
        if (multi_threaded) {
            thread_create(&st, &handle, args);
            thread_detach(st);
        } else {
            handle(args);
        }

        /* Note, since we are not waiting for thread to finish, this may
         * cause the last n connections not being handled properly. */
        if (conn >= max_conn)
            break;
    }

    closesocket(lsock);
    return 0;
}


/* Client code
 *
 * The client sends one message of random size and expects the server
 * to echo it back. The sending is done in a separate thread so we can
 * simultaneously drain the server's replies.  Could do this in a
 * single thread with poll()/select() as well, but this keeps the code
 * simpler.
 */

/* Arguments for client threads */
struct client_args {
    THREAD_HANDLE h;
    GUID target;
    int id;
    int conns;
    int rand;

    int res;
};

/* Argument passed to Client send thread */
struct client_tx_args {
    SOCKET fd;
    int tosend;
    int id;
    int conn;
};

static void *client_tx(void *a)
{
    struct client_tx_args *args = a;
    char sendbuf[RXTX_BUF_LEN];
    char tmp[128];
    int tosend, txlen = RXTX_SMALL_LEN;
    int res;

    tosend = args->tosend;
    while (tosend) {
        if (opt_alternate)
            txlen = (txlen == RXTX_SMALL_LEN) ? RXTX_BUF_LEN : RXTX_SMALL_LEN;
        else
            txlen = RXTX_BUF_LEN;
        txlen = (tosend > txlen) ? txlen : tosend;
        
        res = send(args->fd, sendbuf, txlen, 0);
        if (res == SOCKET_ERROR) {
            snprintf(tmp, sizeof(tmp), "[%02d:%05d] send() after %d bytes",
                     args->id, args->conn, args->tosend - tosend);
            sockerr(tmp);
            goto out;
        }
        tosend -= res;
    }
    DBG("[%02d:%05d] TX: %9d bytes sent\n", args->id, args->conn, args->tosend);

out:
    return NULL;
}

/* Client code for a single connection */
static int client_one(GUID target, int id, int conn)
{
    struct client_tx_args args;
    uint64_t start, end, diff;
    THREAD_HANDLE st;
    SOCKADDR_HV sa;
    SOCKET fd;
    char recvbuf[RXTX_BUF_LEN];
    int rxlen = RXTX_SMALL_LEN;
    char tmp[128];
    int tosend, received = 0;
    int res;

    TRC("[%02d:%05d] start\n", id, conn);

    start = time_ns();

    fd = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (fd == INVALID_SOCKET) {
        sockerr("socket()");
        return 1;
    }

    sa.Family = AF_HYPERV;
    sa.Reserved = 0;
    sa.VmId = target;
    sa.ServiceId = MY_SERVICE_GUID;

    TRC("[%02d:%05d] Connect to: "GUID_FMT":"GUID_FMT"\n",
        id, conn, GUID_ARGS(sa.VmId), GUID_ARGS(sa.ServiceId));

    res = connect(fd, (const struct sockaddr *)&sa, sizeof(sa));
    if (res == SOCKET_ERROR) {
        sockerr("connect()");
        goto out;
    }
    DBG("[%02d:%05d] Connected to: "GUID_FMT":"GUID_FMT" fd=%d\n",
        id, conn, GUID_ARGS(sa.VmId), GUID_ARGS(sa.ServiceId), (int)fd);

    if (RAND_MAX < MAX_DATA_LEN)
        tosend = (int)((1ULL * RAND_MAX + 1) * rand() + rand());
    else
        tosend = rand();

    tosend = tosend % (MAX_DATA_LEN - 1) + 1;

    DBG("[%02d:%05d] TOSEND: %d bytes\n", id, conn, tosend);
    args.fd = fd;
    args.tosend = tosend;
    args.id = id;
    args.conn = conn;
    thread_create(&st, &client_tx, &args);

    while (received < tosend) {
        if (opt_alternate)
            rxlen = (rxlen == RXTX_SMALL_LEN) ? RXTX_BUF_LEN : RXTX_SMALL_LEN;
        else
            rxlen = RXTX_BUF_LEN;
        res = recv(fd, recvbuf, rxlen, 0);
        if (res < 0) {
            snprintf(tmp, sizeof(tmp), "[%02d:%05d] recv() after %d bytes",
                     id, conn, received);
            sockerr(tmp);
            goto thout;
        } else if (res == 0) {
            INFO("[%02d:%05d] Connection closed\n", id, conn);
            res = 1;
            goto thout;
        }
        received += res;
    }

    res = 0;

thout:
    thread_join(st);
    end = time_ns();
    diff = end - start;
    diff /= 1000 * 1000;
    INFO("[%02d:%05d] TX/RX: %9d bytes in %5"PRIu64"ms\n",
         id, conn, received, diff);
out:
    TRC("[%02d:%05d] close(%d)\n", id, conn, (int)fd);
    closesocket(fd);
    return res;
}

static void *client_thd(void *a)
{
    struct client_args *args = a;
    int res, i;

    if (args->rand)
        srand(time(NULL) + args->id);

    for (i = 0; i < args->conns; i++) {
        res = client_one(args->target, args->id, i);
        if (res)
            break;
    }

    args->res = res;
    return args;
}

void usage(char *name)
{
    printf("%s: -s|-c <carg> [-i <conns>]\n", name);
    printf(" -s         Server mode\n");
    printf(" -1         "
           "Use a single thread (handle one connection at a time)\n");
    printf("\n");
    printf(" -c <carg>  Client mode. <carg>:\n");
    printf("   'loopback': Connect in loopback mode\n");
    printf("   'parent':   Connect to the parent partition\n");
    printf("   <guid>:     Connect to VM with GUID\n");
    printf(" -p <num>   Run 'num' connections in parallel (default 1)\n");
    printf(" -r         Initialise random number generator with the time\n");
    printf("\n");
    printf("Common options\n");
    printf(" -i <conns> Number connections the client makes (default %d)\n",
           DEFAULT_CLIENT_CONN);
    printf(" -a         Alternate using short/long send()/recv() buffers\n");
    printf(" -v         Verbose output (use multiple times)\n");
}

int __cdecl main(int argc, char **argv)
{
    struct client_args *args;
    int opt_conns = DEFAULT_CLIENT_CONN;
    int opt_multi_thds = 1;
    int opt_server = 0;
    int opt_rand = 0;
    int opt_par = 1;
    GUID target;
    int res = 0;
    int i;

#ifdef _MSC_VER
    /* Initialize Winsock */
    res = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (res != 0) {
        fprintf(stderr, "WSAStartup() failed with error: %d\n", res);
        return 1;
    }
#endif

    /* No getopt on windows. Do some manual parsing */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            opt_server = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            opt_server = 0;
            if (i + 1 >= argc) {
                fprintf(stderr, "-c requires an argument\n");
                usage(argv[0]);
                goto out;
            }
            if (strcmp(argv[i + 1], "loopback") == 0) {
                target = HV_GUID_LOOPBACK;
            } else if (strcmp(argv[i + 1], "parent") == 0) {
                target = HV_GUID_PARENT;
            } else {
                res = parseguid(argv[i + 1], &target);
                if (res != 0) {
                    fprintf(stderr, "failed to scan: %s\n", argv[i + 1]);
                    goto out;
                }
            }
            i++;

        } else if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-i requires an argument\n");
                usage(argv[0]);
                goto out;
            }
            opt_conns = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-p requires an argument\n");
                usage(argv[0]);
                goto out;
            }
            opt_par = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0) {
            opt_rand = 1;
        } else if (strcmp(argv[i], "-1") == 0) {
            opt_multi_thds = 0;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_alternate = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose++;
        } else {
            usage(argv[0]);
            goto out;
        }
    }

    if (opt_server) {
        server(opt_multi_thds, opt_conns);
    } else {
        args = calloc(opt_par, sizeof(*args));
        if (!args) {
            fprintf(stderr, "failed to malloc");
            res = -1;
            goto out;
        }

        /* Create threads */
        for (i = 0; i < opt_par; i++) {
            args[i].target = target;
            args[i].id = i;
            args[i].conns = opt_conns / opt_par;
            args[i].rand = opt_rand;
            thread_create(&args[i].h, &client_thd, &args[i]);
        }

        /* Wait for threads to finish and collect return codes */
        res = 0;
        for (i = 0; i < opt_par; i++) {
            thread_join(args[i].h);
            if (args[i].res)
                fprintf(stderr, "THREAD[%d] failed with %d\n", i, args[i].res);
            res |= args[i].res;
        }
    }

out:
#ifdef _MSC_VER
    WSACleanup();
#endif
    return res;
}
