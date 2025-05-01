/**
 * @file prefork-WebServer.c
 * @brief Pre-forked HTTP/1.1 WebServer en C con protocolo configurable
 *
 * Crea previamente N procesos que atienden conexiones HTTP/1.1 o envían banners de
 * otros protocolos (FTP, SSH, SMTP, DNS, TELNET, SNMP) según parámetro -T.
 *
 * Sintaxis:
 *   $ prefork-WebServer -n <num_procesos> -w <http_root> -p <port> [-T <protocolo>]
 *
 * Ejemplos:
 *   ./prefork-WebServer -n 4 -w ./var/www -p 8080
 *   ./prefork-WebServer -n 4 -w ./var/www -p 21 -T ftp
 *
 * Requisitos:
 *   - HTTP/1.1: GET, HEAD, POST, PUT, DELETE.
 *   - Protocolo configurable: http (por defecto), ftp, ssh, smtp, dns, telnet, snmp.
 */

 #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>

#define BACKLOG 16
#define BUF_SIZE 8192

/** Configuración del servidor */
typedef struct {
    int port;
    char *root_dir;
    int process_count;
    char *protocol;
} server_config_t;

static server_config_t config;
static pid_t *child_pids = NULL;
static int listen_fd = -1;
static sem_t proc_sem;

/* Prototipos */
void handle_request(int client_fd);
int setup_server_socket(int port);
void sigint_handler(int signo);
void parse_args(int argc, char *argv[], server_config_t *cfg);
void worker_loop();
void send_header(int fd, int status, const char *reason);
void send_banner(int client_fd);

/**
 * @brief Señal SIGINT: termina hijos y sale.
 */
void sigint_handler(int signo) {
    (void)signo;
    for (int i = 0; i < config.process_count; ++i) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    for (int i = 0; i < config.process_count; ++i) {
        waitpid(child_pids[i], NULL, 0);
    }
    if (listen_fd >= 0) close(listen_fd);
    sem_destroy(&proc_sem);
    printf("Servidor finalizado.\n");
    exit(EXIT_SUCCESS);
}

/**
 * @brief Rutina del proceso trabajador: atiende peticiones.
 */
void worker_loop() {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        /* Límite de clientes concurrentes */
        if (sem_trywait(&proc_sem) < 0) {
            /* No hay procesos libres */
            send_header(client_fd, 503, "Service Unavailable: Too many clients");
            close(client_fd);
            continue;
        }
        /* Atender petición */
        handle_request(client_fd);
        sem_post(&proc_sem);
    }
}

/**
 * @brief Envía cabecera HTTP simple.
 */
void send_header(int fd, int status, const char *reason) {
    char buf[BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       status, reason);
    send(fd, buf, len, 0);
}

/**
 * @brief Envía banner de protocolo alternativo.
 */
void send_banner(int client_fd) {
    const char *banner;
    if      (strcmp(config.protocol, "ftp")    == 0) banner = "220 Servicio FTP listo\r\n";
    else if (strcmp(config.protocol, "ssh")    == 0) banner = "SSH-2.0-OpenSSH_8.0\r\n";
    else if (strcmp(config.protocol, "smtp")   == 0) banner = "220 ejemplo.com ESMTP\r\n";
    else if (strcmp(config.protocol, "dns")    == 0) banner = "; DNS server up\r\n";
    else if (strcmp(config.protocol, "telnet") == 0) banner = "telnet ready\r\n";
    else if (strcmp(config.protocol, "snmp")   == 0) banner = "SNMP agent listening\r\n";
    else banner = "200 OK\r\n";
    send(client_fd, banner, strlen(banner), 0);
    close(client_fd);
}

/**
 * @brief Maneja peticiones HTTP o banners.
 */
void handle_request(int client_fd) {
    /* Protocolo no-HTTP */
    if (strcmp(config.protocol, "http") != 0) {
        send_banner(client_fd);
        return;
    }
    /* Leer petición HTTP */
    char buf[BUF_SIZE];
    ssize_t r = recv(client_fd, buf, sizeof(buf)-1, 0);
    if (r <= 0) { close(client_fd); return; }
    buf[r] = '\0';
    /* Parsear línea */
    char method[16], path[256], version[16];
    if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3) {
        send_header(client_fd, 400, "Bad Request"); close(client_fd); return;
    }
    if (strcmp(version, "HTTP/1.1") != 0) {
        send_header(client_fd, 505, "HTTP Version Not Supported"); close(client_fd); return;
    }
    /* Construir ruta */
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", config.root_dir, path);
    /* Métodos HTTP */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        int fd = open(full_path, O_RDONLY);
        if (fd < 0) send_header(client_fd, 404, "Not Found");
        else {
            struct stat st; fstat(fd, &st);
            char header[BUF_SIZE];
            int hlen = snprintf(header, sizeof(header),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Length: %ld\r\n"
                                "Connection: close\r\n"
                                "\r\n", st.st_size);
            send(client_fd, header, hlen, 0);
            if (strcmp(method, "GET") == 0) {
                while ((r = read(fd, buf, sizeof(buf))) > 0) send(client_fd, buf, r, 0);
            }
            close(fd);
        }
    } else if (strcmp(method, "POST") == 0) {
        char *cl = strstr(buf, "Content-Length:"); int len=0;
        if (cl) sscanf(cl, "Content-Length: %d", &len);
        if (len <= 0) send_header(client_fd, 411, "Length Required");
        else {
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) send_header(client_fd, 400, "Bad Request");
            else {
                body += 4; int rec = r - (body - buf);
                int fd = open(full_path, O_WRONLY|O_CREAT|O_EXCL, 0666);
                if (fd < 0) send_header(client_fd, 409, "Conflict");
                else {
                    write(fd, body, rec);
                    int total = rec;
                    while (total < len && (r = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
                        write(fd, buf, r); total += r;
                    }
                    close(fd); send_header(client_fd, 201, "Created");
                }
            }
        }
    } else if (strcmp(method, "PUT") == 0) {
        char *cl = strstr(buf, "Content-Length:"); int len=0;
        if (cl) sscanf(cl, "Content-Length: %d", &len);
        if (len <= 0) send_header(client_fd, 411, "Length Required");
        else {
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) send_header(client_fd, 400, "Bad Request");
            else {
                body += 4; int rec = r - (body - buf);
                int fd = open(full_path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
                write(fd, body, rec);
                int total = rec;
                while (total < len && (r = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
                    write(fd, buf, r); total += r;
                }
                close(fd); send_header(client_fd, 200, "OK");
            }
        }
    } else if (strcmp(method, "DELETE") == 0) {
        if (access(full_path, F_OK) < 0) send_header(client_fd, 404, "Not Found");
        else { remove(full_path); send_header(client_fd, 200, "OK"); }
    } else {
        send_header(client_fd, 405, "Method Not Allowed");
    }
    close(client_fd);
}

/**
 * @brief Crea socket y lo deja escuchando.
 */
int setup_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                 .sin_addr.s_addr = INADDR_ANY,
                                 .sin_port = htons(port) };
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(fd, BACKLOG) < 0) { perror("listen"); exit(EXIT_FAILURE); }
    return fd;
}

/**
 * @brief Lee args: -n, -w, -p, -T.
 */
void parse_args(int argc, char *argv[], server_config_t *cfg) {
    int opt;
    cfg->protocol = strdup("http");
    while ((opt = getopt(argc, argv, "n:w:p:T:")) != -1) {
        switch (opt) {
            case 'n': cfg->process_count = atoi(optarg); break;
            case 'w': cfg->root_dir     = strdup(optarg); break;
            case 'p': cfg->port         = atoi(optarg); break;
            case 'T': free(cfg->protocol); cfg->protocol = strdup(optarg); break;
            default:
                fprintf(stderr, "Uso: %s -n <procesos> -w <root> -p <puerto> [-T <protocolo>]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (!cfg->process_count || !cfg->root_dir || !cfg->port) {
        fprintf(stderr, "Parámetros incompletos.\n"); exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    parse_args(argc, argv, &config);
    printf("Iniciando pre-forked WebServer: puerto %d, procesos %d, root %s, protocolo %s\n",
           config.port, config.process_count, config.root_dir, config.protocol);
    signal(SIGINT, sigint_handler);
    /* Inicializar semáforo compartido entre procesos */
    if (sem_init(&proc_sem, 1, config.process_count) != 0) {
        perror("sem_init"); exit(EXIT_FAILURE);
    }
    listen_fd = setup_server_socket(config.port);
    child_pids = calloc(config.process_count, sizeof(pid_t));
    for (int i = 0; i < config.process_count; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) {
            worker_loop();
            exit(EXIT_SUCCESS);
        } else {
            child_pids[i] = pid;
        }
    }
    /* El padre no atiende conexiones */
    for (;;) pause();
    return 0;
}