#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sched.h>
#include "monitor_ioctl.h"

#define MAX_CONTAINERS  16
#define MAX_NAME_LEN    64
#define MAX_PATH_LEN   256
#define LOG_DIR        "/tmp/container_logs"
#define SOCK_PATH      "/tmp/engine.sock"
#define LOG_BUF_SIZE   4096
#define LOG_ENTRY_SIZE  512
#define SOFT_LIMIT_DEFAULT  (8UL  * 1024 * 1024)
#define HARD_LIMIT_DEFAULT  (16UL * 1024 * 1024)

typedef enum {
    STATE_STARTING = 0, STATE_RUNNING, STATE_STOPPED, STATE_KILLED, STATE_EXITED
} ContainerState;

static const char *state_names[] = {
    "starting", "running", "stopped", "killed", "exited"
};

typedef struct {
    int           used;
    char          name[MAX_NAME_LEN];
    pid_t         host_pid;
    time_t        start_time;
    ContainerState state;
    unsigned long soft_limit;
    unsigned long hard_limit;
    char          log_path[MAX_PATH_LEN];
    int           exit_status;
    int           term_signal;
    int           log_pipe[2];
} Container;

static Container g_containers[MAX_CONTAINERS];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char data[LOG_ENTRY_SIZE];
    int  len;
    char log_path[MAX_PATH_LEN];
} LogEntry;

static LogEntry  g_log_buf[LOG_BUF_SIZE];
static int       g_log_head  = 0;
static int       g_log_tail  = 0;
static int       g_log_count = 0;
static pthread_mutex_t g_log_lock      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_log_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_log_not_full  = PTHREAD_COND_INITIALIZER;
static volatile int    g_log_running   = 1;
static volatile int    g_running       = 1;

typedef struct {
    int  fd;
    char log_path[MAX_PATH_LEN];
} LogReaderArg;

static void supervisor_loop(const char *rootfs);
static void handle_sigchld(int sig);
static void handle_sigterm(int sig);
static void *log_consumer_thread(void *arg);
static void *log_reader_thread(void *arg);
static int  find_container_by_name(const char *name);
static int  find_free_slot(void);
static void log_push(const char *log_path, const char *data, int len);
static void register_with_monitor(pid_t pid, unsigned long soft, unsigned long hard);

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo ./engine supervisor <rootfs>\n"
            "  sudo ./engine start <name> <rootfs> <cmd>\n"
            "  sudo ./engine run   <name> <rootfs> <cmd>\n"
            "  sudo ./engine ps\n"
            "  sudo ./engine logs  <name>\n"
            "  sudo ./engine stop  <name>\n");
        return 1;
    }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "supervisor needs <rootfs>\n"); return 1; }
        supervisor_loop(argv[2]);
        return 0;
    }
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor. Is it running?\n");
        return 1;
    }
    char msg[1024] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(msg, " ");
        strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
    }
    strcat(msg, "\n");
    write(sock, msg, strlen(msg));
    char buf[4096];
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(sock);
    return 0;
}

static void supervisor_loop(const char *rootfs)
{
    (void)rootfs;
    mkdir(LOG_DIR, 0755);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, log_consumer_thread, NULL);

    unlink(SOCK_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(server_fd, 8) < 0) { perror("listen"); exit(1); }
    fcntl(server_fd, F_SETFL, O_NONBLOCK);
    printf("[supervisor] Started. Listening on %s\n", SOCK_PATH);
    fflush(stdout);

    while (g_running) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(50000); continue; }
            if (errno == EINTR) continue;
            break;
        }
        char buf[1024] = {0};
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        if (n <= 0) { close(client); continue; }
        buf[n] = '\0';
        buf[strcspn(buf, "\n")] = '\0';

        char *tokens[32];
        int ntok = 0;
        char *tok = strtok(buf, " ");
        while (tok && ntok < 31) { tokens[ntok++] = tok; tok = strtok(NULL, " "); }
        tokens[ntok] = NULL;

        char response[4096] = {0};

        if (ntok == 0) {
            snprintf(response, sizeof(response), "Empty command\n");
        } else if (strcmp(tokens[0], "start") == 0 || strcmp(tokens[0], "run") == 0) {
            int foreground = (strcmp(tokens[0], "run") == 0);
            if (ntok < 4) { snprintf(response, sizeof(response), "Usage: start <name> <rootfs> <cmd>\n"); goto send_response; }
            const char *name   = tokens[1];
            const char *rfs    = tokens[2];
            const char *cmd_in = tokens[3];

            pthread_mutex_lock(&g_table_lock);
            int slot = find_free_slot();
            if (slot < 0) {
                pthread_mutex_unlock(&g_table_lock);
                snprintf(response, sizeof(response), "Error: too many containers\n");
                goto send_response;
            }
            Container *c = &g_containers[slot];
            memset(c, 0, sizeof(*c));
            c->used = 1; c->state = STATE_STARTING;
            c->soft_limit = SOFT_LIMIT_DEFAULT; c->hard_limit = HARD_LIMIT_DEFAULT;
            c->start_time = time(NULL);
            strncpy(c->name, name, MAX_NAME_LEN - 1);
            snprintf(c->log_path, MAX_PATH_LEN, "%s/%s.log", LOG_DIR, name);
            if (pipe(c->log_pipe) < 0) {
                c->used = 0; pthread_mutex_unlock(&g_table_lock);
                snprintf(response, sizeof(response), "Error: pipe failed\n");
                goto send_response;
            }
            pthread_mutex_unlock(&g_table_lock);

            pid_t pid = fork();
            if (pid < 0) { snprintf(response, sizeof(response), "Error: fork failed\n"); goto send_response; }

            if (pid == 0) {
                close(c->log_pipe[0]);
                dup2(c->log_pipe[1], STDOUT_FILENO);
                dup2(c->log_pipe[1], STDERR_FILENO);
                close(c->log_pipe[1]);
                if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) < 0) { _exit(1); }
                sethostname(name, strlen(name));
                char proc_path[MAX_PATH_LEN];
                snprintf(proc_path, sizeof(proc_path), "%s/proc", rfs);
                mkdir(proc_path, 0755);
                mount(rfs, rfs, NULL, MS_BIND | MS_REC, NULL);
                if (chroot(rfs) < 0) { _exit(1); }
                chdir("/");
                mount("proc", "/proc", "proc", 0, NULL);
                char *exec_argv[32];
                exec_argv[0] = (char *)cmd_in;
                for (int i = 4; i < ntok && (i-4+1) < 31; i++) exec_argv[i-3] = tokens[i];
                exec_argv[ntok-3] = NULL;
                execvp(cmd_in, exec_argv);
                _exit(1);
            }

            pthread_mutex_lock(&g_table_lock);
            c->host_pid = pid; c->state = STATE_RUNNING;
            pthread_mutex_unlock(&g_table_lock);
            close(c->log_pipe[1]);

            LogReaderArg *lra = malloc(sizeof(LogReaderArg));
            lra->fd = c->log_pipe[0];
            strncpy(lra->log_path, c->log_path, MAX_PATH_LEN - 1);
            pthread_t reader_tid;
            pthread_create(&reader_tid, NULL, log_reader_thread, lra);
            pthread_detach(reader_tid);

            register_with_monitor(pid, c->soft_limit, c->hard_limit);

            if (foreground) {
                int wstatus; waitpid(pid, &wstatus, 0);
                pthread_mutex_lock(&g_table_lock);
                c->state = STATE_EXITED;
                if (WIFEXITED(wstatus)) c->exit_status = WEXITSTATUS(wstatus);
                if (WIFSIGNALED(wstatus)) c->term_signal = WTERMSIG(wstatus);
                pthread_mutex_unlock(&g_table_lock);
                snprintf(response, sizeof(response), "Container '%s' exited\n", name);
            } else {
                snprintf(response, sizeof(response), "Container '%s' started (PID %d)\n", name, pid);
            }
        } else if (strcmp(tokens[0], "ps") == 0) {
            snprintf(response, sizeof(response), "%-16s %-8s %-10s %-20s\n", "NAME", "PID", "STATE", "STARTED");
            strcat(response, "------------------------------------------------------------\n");
            pthread_mutex_lock(&g_table_lock);
            for (int i = 0; i < MAX_CONTAINERS; i++) {
                if (!g_containers[i].used) continue;
                Container *c = &g_containers[i];
                char tstr[32]; struct tm *tm_info = localtime(&c->start_time);
                strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm_info);
                char line[256];
                snprintf(line, sizeof(line), "%-16s %-8d %-10s %-20s\n",
                    c->name, c->host_pid, state_names[c->state], tstr);
                strncat(response, line, sizeof(response) - strlen(response) - 1);
            }
            pthread_mutex_unlock(&g_table_lock);
        } else if (strcmp(tokens[0], "logs") == 0) {
            if (ntok < 2) { snprintf(response, sizeof(response), "Usage: logs <name>\n"); goto send_response; }
            pthread_mutex_lock(&g_table_lock);
            int idx = find_container_by_name(tokens[1]);
            char log_path[MAX_PATH_LEN] = {0};
            if (idx >= 0) strncpy(log_path, g_containers[idx].log_path, MAX_PATH_LEN - 1);
            pthread_mutex_unlock(&g_table_lock);
            if (idx < 0) {
                snprintf(response, sizeof(response), "Container '%s' not found\n", tokens[1]);
            } else {
                FILE *f = fopen(log_path, "r");
                if (!f) { snprintf(response, sizeof(response), "Log file not found\n"); }
                else { size_t r = fread(response, 1, sizeof(response)-1, f); response[r] = '\0'; fclose(f); }
            }
        } else if (strcmp(tokens[0], "stop") == 0) {
            if (ntok < 2) { snprintf(response, sizeof(response), "Usage: stop <name>\n"); goto send_response; }
            pthread_mutex_lock(&g_table_lock);
            int idx = find_container_by_name(tokens[1]);
            if (idx < 0) {
                pthread_mutex_unlock(&g_table_lock);
                snprintf(response, sizeof(response), "Container '%s' not found\n", tokens[1]);
            } else {
                pid_t pid = g_containers[idx].host_pid;
                g_containers[idx].state = STATE_STOPPED;
                pthread_mutex_unlock(&g_table_lock);
                kill(pid, SIGTERM);
                snprintf(response, sizeof(response), "Sent SIGTERM to '%s' (PID %d)\n", tokens[1], pid);
            }
        } else {
            snprintf(response, sizeof(response), "Unknown command: %s\n", tokens[0]);
        }

send_response:
        write(client, response, strlen(response));
        close(client);
    }

    printf("[supervisor] Shutting down...\n");
    g_log_running = 0;
    pthread_cond_broadcast(&g_log_not_empty);
    pthread_join(consumer_tid, NULL);
    pthread_mutex_lock(&g_table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_containers[i].used && g_containers[i].state == STATE_RUNNING)
            kill(g_containers[i].host_pid, SIGTERM);
    pthread_mutex_unlock(&g_table_lock);
    int status; while (wait(&status) > 0);
    close(server_fd); unlink(SOCK_PATH);
    printf("[supervisor] Done.\n");
}

static void handle_sigchld(int sig)
{
    (void)sig;
    int saved_errno = errno; int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_table_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_containers[i].used && g_containers[i].host_pid == pid) {
                if (g_containers[i].state != STATE_KILLED) g_containers[i].state = STATE_EXITED;
                if (WIFEXITED(status)) g_containers[i].exit_status = WEXITSTATUS(status);
                if (WIFSIGNALED(status)) g_containers[i].term_signal = WTERMSIG(status);
                break;
            }
        }
        pthread_mutex_unlock(&g_table_lock);
    }
    errno = saved_errno;
}

static void handle_sigterm(int sig) { (void)sig; g_running = 0; }

static void *log_reader_thread(void *arg)
{
    LogReaderArg *lra = (LogReaderArg *)arg;
    char tmp[LOG_ENTRY_SIZE]; ssize_t n;
    while ((n = read(lra->fd, tmp, sizeof(tmp) - 1)) > 0) { tmp[n] = '\0'; log_push(lra->log_path, tmp, (int)n); }
    close(lra->fd); free(lra); return NULL;
}

static void *log_consumer_thread(void *arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_log_lock);
        while (g_log_count == 0 && g_log_running) pthread_cond_wait(&g_log_not_empty, &g_log_lock);
        if (g_log_count == 0 && !g_log_running) { pthread_mutex_unlock(&g_log_lock); break; }
        LogEntry entry = g_log_buf[g_log_head];
        g_log_head = (g_log_head + 1) % LOG_BUF_SIZE;
        g_log_count--;
        pthread_cond_signal(&g_log_not_full);
        pthread_mutex_unlock(&g_log_lock);
        FILE *f = fopen(entry.log_path, "a");
        if (f) { fwrite(entry.data, 1, entry.len, f); fclose(f); }
    }
    return NULL;
}

static void log_push(const char *log_path, const char *data, int len)
{
    pthread_mutex_lock(&g_log_lock);
    while (g_log_count == LOG_BUF_SIZE) pthread_cond_wait(&g_log_not_full, &g_log_lock);
    LogEntry *e = &g_log_buf[g_log_tail];
    int copy = (len < LOG_ENTRY_SIZE - 1) ? len : LOG_ENTRY_SIZE - 1;
    memcpy(e->data, data, copy); e->len = copy;
    strncpy(e->log_path, log_path, MAX_PATH_LEN - 1);
    g_log_tail = (g_log_tail + 1) % LOG_BUF_SIZE;
    g_log_count++;
    pthread_cond_signal(&g_log_not_empty);
    pthread_mutex_unlock(&g_log_lock);
}

static int find_container_by_name(const char *name)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_containers[i].used && strcmp(g_containers[i].name, name) == 0) return i;
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++) if (!g_containers[i].used) return i;
    return -1;
}

static void register_with_monitor(pid_t pid, unsigned long soft, unsigned long hard)
{
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;
    struct container_info ci; ci.pid = pid; ci.soft_limit = soft; ci.hard_limit = hard;
    ioctl(fd, MONITOR_REGISTER, &ci);
    close(fd);
}
