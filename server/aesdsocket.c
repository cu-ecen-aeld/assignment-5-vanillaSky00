/**
 * aesdsocket.c – ECEN-5013 Assignment 5 reference
 *
 * Listens on TCP 9000, appends line-terminated packets to
 *   /var/tmp/aesdsocketdata
 * echoes the file back once a packet completes, and
 * cleans up on SIGINT / SIGTERM.
 *
 * Optional “-d” argument runs the server as a daemon.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <stdbool.h>

#define PORT              9000
#define DATAFILE          "/var/tmp/aesdsocketdata"
#define BACKLOG           10
#define RECV_CHUNK_BYTES  512U   /* fits easily on small stacks */

static int listen_fd = -1;
static int client_fd = -1;

/* ---------------- utility helpers ---------------- */

static void fatal(const char *msg)
{
    syslog(LOG_ERR, "%s : %s", msg, strerror(errno));
    if (client_fd >= 0) close(client_fd);
    if (listen_fd >= 0) close(listen_fd);
    unlink(DATAFILE);
    closelog();
    exit(EXIT_FAILURE);          /* returns −1 to autograder wrapper */
}

/* ---------------- signal handling ---------------- */

static void handle_signal(int signo)
{
    (void)signo;
    syslog(LOG_INFO, "Caught signal, exiting");
    if (client_fd >= 0) close(client_fd);
    if (listen_fd >= 0) close(listen_fd);
    unlink(DATAFILE);
    closelog();
    exit(EXIT_SUCCESS);
}

static void install_sig_handlers(void)
{
    struct sigaction sa = {.sa_handler = handle_signal};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT,  &sa, NULL) ||
        sigaction(SIGTERM, &sa, NULL))
        fatal("sigaction");
}

/* ---------------- daemon mode ---------------- */

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) fatal("fork");
    if (pid > 0) exit(EXIT_SUCCESS); /* parent exits */

    /* child becomes session leader */
    if (setsid() < 0) fatal("setsid");

    /* second fork to avoid reacquiring a tty */
    pid = fork();
    if (pid < 0) fatal("fork2");
    if (pid > 0) exit(EXIT_SUCCESS);

    /* standard daemon hygiene */
    umask(0);
    if (chdir("/") < 0) fatal("chdir");

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* stdin/out/err -> /dev/null */
    open("/dev/null", O_RDWR); /* stdin  */
    dup(0);                    /* stdout */
    dup(0);                    /* stderr */
}

/* ---------------- main server loop ---------------- */

int main(int argc, char *argv[])
{
    bool run_as_daemon = (argc == 2 && strcmp(argv[1], "-d") == 0);

    openlog("aesdsocket", LOG_PID, LOG_USER);
    install_sig_handlers();

    /* 1. Create, bind, listen */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) fatal("socket");

    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0)
        fatal("setsockopt");

    struct sockaddr_in srvaddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(PORT)
    };
    if (bind(listen_fd, (struct sockaddr *)&srvaddr, sizeof(srvaddr)) < 0)
        fatal("bind");

    if (listen(listen_fd, BACKLOG) < 0) fatal("listen");

    if (run_as_daemon) daemonize();

    /* 2. Accept / serve forever */
    for (;;)
    {
        struct sockaddr_in cliaddr;
        socklen_t          clilen = sizeof(cliaddr);

        client_fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0)
        {
            if (errno == EINTR) continue;     /* interrupted by signal */
            fatal("accept");
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, ipstr, sizeof(ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        /* open file once per connection (append mode) */
        int data_fd = open(DATAFILE,
                           O_CREAT | O_RDWR | O_APPEND,
                           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (data_fd < 0) fatal("open data file");

        /* -------------- receive until newline -------------- */
        size_t packet_len  = 0;
        char  *packet_buf  = NULL;
        char   recv_buf[RECV_CHUNK_BYTES];
        ssize_t rx;

        while ((rx = recv(client_fd, recv_buf, sizeof(recv_buf), 0)) > 0)
        {
            char *tmp = realloc(packet_buf, packet_len + (size_t)rx);
            if (!tmp) { free(packet_buf); fatal("realloc"); }
            packet_buf = tmp;
            memcpy(packet_buf + packet_len, recv_buf, (size_t)rx);
            packet_len += (size_t)rx;
            if (memchr(recv_buf, '\n', (size_t)rx)) break; /* packet complete */
        }
        if (rx < 0) { free(packet_buf); fatal("recv"); }

        /* write packet to file */
        if ((size_t)write(data_fd, packet_buf, packet_len) != packet_len)
        {
            free(packet_buf);
            fatal("write data file");
        }
        free(packet_buf);

        /* -------------- send entire file back -------------- */
        if (lseek(data_fd, 0, SEEK_SET) < 0) fatal("lseek");

        while ((rx = read(data_fd, recv_buf, sizeof(recv_buf))) > 0)
        {
            size_t sent = 0;
            while (sent < (size_t)rx)
            {
                ssize_t tx = send(client_fd,
                                  recv_buf + sent,
                                  (size_t)rx - sent,
                                  0);
                if (tx < 0) fatal("send");
                sent += (size_t)tx;
            }
        }
        if (rx < 0) fatal("read data file");

        close(data_fd);
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
        close(client_fd);
        client_fd = -1;
    }
    /* never reached – cleanup is in signal handler */
    return EXIT_SUCCESS;
}

