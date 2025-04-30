/**
 * @file prefork-WebServer.c
 * @brief Pre-forked HTTP/1.1 WebServer en C (implementación base)
 *
 * Crea previamente N procesos para manejar peticiones concurrentes.
 * Cada proceso acepta conexiones y gestiona las solicitudes HTTP.
 *
 * Sintaxis:
 *   $ prefork-WebServer -n <num_procesos> -w <http_root> -p <port>
 *
 * Requerimientos:
 *   - Soporta HTTP/1.1: métodos GET, HEAD, POST, PUT, DELETE.
 *   - Manejo de ficheros estáticos en <http_root>.
 *   - Gestión básica de errores y cierre limpio de procesos.
 *
 * Uso:
 *   gcc prefork-WebServer.c -o prefork-WebServer
 *   ./prefork-WebServer -n 4 -w ./var/www -p 8080
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

#define BACKLOG 16    /**< Cola de conexiones pendientes */
#define BUF_SIZE 8192 /**< Buffer para lectura y escritura */

/** Configuración del servidor */
typedef struct {
    int port;
    char *root_dir;
    int process_count;
} server_config_t;

static server_config_t config;
static pid_t *child_pids = NULL;
static int listen_fd = -1;

/**
 * @brief Maneja una única petición HTTP en el descriptor dado.
 *
 * @param client_fd Descriptor del socket cliente.
 */
void handle_request(int client_fd);

/**
 * @brief Rutina de bucle para cada proceso trabajador.
 *
 * @param listen_fd Descriptor del socket de escucha.
 */
void worker_loop(int listen_fd) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_request(client_fd);
    }
    exit(EXIT_SUCCESS);
}

/**
 * @brief Señal SIGINT: termina procesos hijos y sale.
 */
void sigint_handler(int signo) {
    (void)signo;
    // Enviar SIGTERM a todos los hijos
    for (int i = 0; i < config.process_count; ++i) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    // Esperar por hijos
    for (int i = 0; i < config.process_count; ++i) {
        waitpid(child_pids[i], NULL, 0);
    }
    if (listen_fd >= 0) close(listen_fd);
    printf("Servidor finalizado.\n");
    exit(EXIT_SUCCESS);
}

/**
 * @brief Configura el socket servidor y comienza a escuchar.
 *
 * @param port Puerto TCP.
 * @return Descriptor del socket.
 */
int setup_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(sockfd, BACKLOG) < 0) { perror("listen"); exit(EXIT_FAILURE); }
    return sockfd;
}

/**
 * @brief Lee argumentos de línea de comandos.
 */
void parse_args(int argc, char *argv[], server_config_t *cfg) {
    int opt;
    while ((opt = getopt(argc, argv, "n:w:p:")) != -1) {
        switch (opt) {
            case 'n': cfg->process_count = atoi(optarg); break;
            case 'w': cfg->root_dir = strdup(optarg); break;
            case 'p': cfg->port = atoi(optarg); break;
            default:
                fprintf(stderr, "Uso: %s -n <procesos> -w <root> -p <puerto>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (!cfg->process_count || !cfg->root_dir || !cfg->port) {
        fprintf(stderr, "Parámetros incompletos.\n");
        exit(EXIT_FAILURE);
    }
}

/*******************************************************************************
 *                            IMPLEMENTACIÓN DE handle_request               *
 ******************************************************************************/
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

void send_error(int fd, int status, const char *reason) {
    send_header(fd, status, reason);
}

void handle_request(int client_fd) {
    char buf[BUF_SIZE];
    ssize_t r = recv(client_fd, buf, sizeof(buf)-1, 0);
    if (r <= 0) { close(client_fd); return; }
    buf[r] = '\0';
    char method[16], path[256], version[16];
    if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3) {
        send_error(client_fd, 400, "Bad Request"); close(client_fd); return;
    }
    if (strcmp(version, "HTTP/1.1") != 0) {
        send_error(client_fd, 505, "HTTP Version Not Supported"); close(client_fd); return;
    }

    // Construcción de ruta
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", config.root_dir, path);

    // GET y HEAD
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        int fd = open(full_path, O_RDONLY);
        if (fd < 0) { send_error(client_fd, 404, "Not Found"); }
        else {
            struct stat st; fstat(fd, &st);
            char header[BUF_SIZE];
            int hlen = snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Length: %ld\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               st.st_size);
            send(client_fd, header, hlen, 0);
            if (strcmp(method, "GET") == 0) {
                while ((r = read(fd, buf, sizeof(buf))) > 0) send(client_fd, buf, r, 0);
            }
            close(fd);
        }
    }
    // POST
    else if (strcmp(method, "POST") == 0) {
        // Similar a prethread: crear solo si no existe
        char *cl = strstr(buf, "Content-Length:"); int content_length=0;
        if (cl) sscanf(cl, "Content-Length: %d", &content_length);
        if (content_length <= 0) { send_error(client_fd, 411, "Length Required"); }
        else {
            char *body = strstr(buf, "\r\n\r\n"); if (!body) { send_error(client_fd,400,"Bad Request"); }
            else {
                body+=4;
                int header_len = body - buf;
                int rec = r - header_len;
                int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (fd < 0) { send_error(client_fd, 409, "Conflict"); }
                else {
                    write(fd, body, rec);
                    // Leer resto si body incompleto
                    int total = rec;
                    while (total < content_length && (r=recv(client_fd, buf, BUF_SIZE,0))>0) {
                        write(fd, buf, r); total+=r;
                    }
                    close(fd);
                    send_header(client_fd, 201, "Created");
                }
            }
        }
    }
    // PUT
    else if (strcmp(method, "PUT") == 0) {
        char *cl = strstr(buf, "Content-Length:"); int content_length=0;
        if (cl) sscanf(cl, "Content-Length: %d", &content_length);
        if (content_length <= 0) { send_error(client_fd, 411, "Length Required"); }
        else {
            char *body = strstr(buf, "\r\n\r\n"); if (!body) { send_error(client_fd,400,"Bad Request"); }
            else {
                body+=4; int header_len = body - buf; int rec = r - header_len;
                int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                write(fd, body, rec);
                int total = rec;
                while (total < content_length && (r=recv(client_fd, buf, BUF_SIZE,0))>0) {
                    write(fd, buf, r); total+=r;
                }
                close(fd);
                send_header(client_fd, 200, "OK");
            }
        }
    }
    // DELETE
    else if (strcmp(method, "DELETE") == 0) {
        if (access(full_path, F_OK)<0) send_error(client_fd,404,"Not Found");
        else {
            if (remove(full_path)<0) send_error(client_fd,500,"Internal Server Error");
            else send_header(client_fd,200,"OK");
        }
    }
    else {
        send_error(client_fd, 405, "Method Not Allowed");
    }
    close(client_fd);
}

/*******************************************************************************
 *                                  Función principal                         *
 ******************************************************************************/
int main(int argc, char *argv[]) {
    parse_args(argc, argv, &config);
    printf("Iniciando pre-forked WebServer: puerto %d, procesos %d, root %s\n",
           config.port, config.process_count, config.root_dir);

    // Señal de interrupción para cierre limpio
    signal(SIGINT, sigint_handler);

    // Crear socket de escucha
    listen_fd = setup_server_socket(config.port);

    // Fork de procesos trabajadores
    child_pids = calloc(config.process_count, sizeof(pid_t));
    for (int i = 0; i < config.process_count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            // Hijo
            worker_loop(listen_fd);
        } else {
            // Padre guarda pid
            child_pids[i] = pid;
        }
    }

    // Padre espera señal o muerte de hijos
    for (;;) pause();

    return 0;
}
