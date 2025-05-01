/**
 * @file prefork-WebServer.c
 * @brief Servidor Web **pre‑forked** con soporte HTTP/1.1 y envío opcional de banners de otros protocolos.
 *
 * El programa lanza un conjunto fijo de procesos hijo (_pool_ de procesos) que comparten el
 * mismo *socket* de escucha. Cada proceso maneja conexiones entrantes de forma concurrente,
 * sirviendo archivos estáticos desde un directorio raíz y gestionando los métodos
 * **GET**, **HEAD**, **POST**, **PUT** y **DELETE**.
 *
 * Adicionalmente permite emular servicios como FTP, SSH, SMTP, DNS, Telnet y SNMP
 * enviando únicamente su *banner* de bienvenida y cerrando la conexión, útil para
 * pruebas de *banner‑grabbing*.
 *
 * El servidor incorpora un contador global de peticiones (compartido entre procesos
 * mediante *mmap*) que, al superar un límite configurable, responde **503 Service Unavailable**
 * sin abortar la ejecución. También emplea un semáforo para controlar el número máximo
 * de clientes atendidos simultáneamente por los procesos.
 *
 * @author  Valery Carvajal y Anthony Rojas
 * @date    30 abr 2025
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
#include <pthread.h>
#include <sys/mman.h>

/** Número máximo de conexiones en la cola de espera de `accept()`. */
#define BACKLOG 16

/** Tamaño del búfer para cabeceras y transferencia de datos. */
#define BUF_SIZE 8192

/**
 * @struct server_config_t
 * @brief Estructura con los parámetros de configuración del servidor.
 */
typedef struct {
    int   port;          /**< Puerto TCP donde el servidor escuchará.               */
    char *root_dir;      /**< Directorio raíz de los archivos servidos.            */
    int   process_count; /**< Número de procesos hijo en el *pool*.               */
    char *protocol;      /**< Protocolo a emular (`"http"`, `"ftp"`, etc.).      */
    int   request_limit; /**< Límite global de peticiones; 0 =infinito.            */
} server_config_t;

/*─────────────────────────── Variables globales ───────────────────────────*/

static server_config_t config;         /**< Configuración global. */
static pid_t          *child_pids = NULL; /**< PIDs de los procesos hijo. */
static int             listen_fd  = -1;   /**< *Socket* de escucha común. */

/* Contador y mutex compartidos entre procesos (en memoria compartida). */
static int             *served_requests;  /**< Peticiones atendidas en total. */
static pthread_mutex_t *served_mutex;     /**< Mutex para proteger el contador. */

static sem_t proc_sem;                   /**< Semáforo de procesos activos. */

/*─────────────────────────── Prototipos de funciones ──────────────────────*/
void handle_request(int client_fd);
int  setup_server_socket(int port);
void sigint_handler(int signo);
void parse_args(int argc, char *argv[], server_config_t *cfg);
void send_header(int fd, int status, const char *reason);
void send_banner(int client_fd);

/*====================  Manejador de señal SIGINT/SIGTERM  ==================*/

/**
 * @brief Manejador de **SIGINT**. Finaliza todos los procesos hijo y libera recursos.
 * @param signo Número de señal recibida (no utilizado).
 */
void sigint_handler(int signo) {
    (void)signo;

    /* Terminar procesos hijo. */
    for (int i = 0; i < config.process_count; ++i) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    for (int i = 0; i < config.process_count; ++i) {
        waitpid(child_pids[i], NULL, 0);
    }

    /* Liberar recursos del proceso padre. */
    if (listen_fd >= 0) close(listen_fd);
    sem_destroy(&proc_sem);
    pthread_mutex_destroy(served_mutex);
    munmap(served_requests, sizeof(int));
    munmap(served_mutex, sizeof(pthread_mutex_t));

    printf("Servidor finalizado.\n");
    exit(EXIT_SUCCESS);
}

/*========================  Utilidades de respuesta HTTP  ==================*/

/**
 * @brief Envía una cabecera HTTP vacía y cierra la conexión.
 *
 * @param fd      Descriptor de *socket* destino.
 * @param status  Código de estado HTTP.
 * @param reason  Frase asociada al estado.
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
 * @brief Envía el *banner* de un protocolo simulado y cierra la conexión.
 * @param client_fd Descriptor del cliente.
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

/*===========================  Manejo de petición  =========================*/

/**
 * @brief Atiende una petición HTTP (o banner) en el proceso hijo actual.
 *
 * Controla el límite global de peticiones, gestiona la concurrencia mediante
 * `proc_sem` y ejecuta la lógica de cada método soportado.
 *
 * @param client_fd Descriptor del *socket* del cliente.
 */
void handle_request(int client_fd) {
    /* ---------- Límite global de peticiones ------------------------------ */
    if (config.request_limit > 0) {
        pthread_mutex_lock(served_mutex);
        (*served_requests)++;
        if (*served_requests > config.request_limit) {
            pthread_mutex_unlock(served_mutex);
            send_header(client_fd, 503, "Service Unavailable");
            close(client_fd);
            return;
        }
        pthread_mutex_unlock(served_mutex);
    }

    /* ---------- Control de concurrencia entre procesos ------------------- */
    if (sem_trywait(&proc_sem) < 0) {
        send_header(client_fd, 503, "Service Unavailable: Too many clients");
        close(client_fd);
        return;
    }

    /* ---------- Protocolos no‑HTTP --------------------------------------- */
    if (strcmp(config.protocol, "http") != 0) {
        send_banner(client_fd);
        sem_post(&proc_sem);
        return;
    }

    /* ---------- Recepción y parsing de la petición HTTP ------------------ */
    char buf[BUF_SIZE];
    ssize_t r = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (r <= 0) { close(client_fd); sem_post(&proc_sem); return; }
    buf[r] = '\0';

    char method[16], path[256], version[16];
    if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3) {
        send_header(client_fd, 400, "Bad Request");
        close(client_fd); sem_post(&proc_sem); return;
    }
    if (strcmp(version, "HTTP/1.1") != 0) {
        send_header(client_fd, 505, "HTTP Version Not Supported");
        close(client_fd); sem_post(&proc_sem); return;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", config.root_dir, path);

    /* ===================  Métodos soportados  ============================ */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        /* ---------------------- GET / HEAD ------------------------------- */
        int fd = open(full_path, O_RDONLY);
        if (fd < 0) {
            send_header(client_fd, 404, "Not Found");
        } else {
            struct stat st; fstat(fd, &st);
            char hdr[BUF_SIZE];
            int hlen = snprintf(hdr, sizeof(hdr),
                               "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                               st.st_size);
            send(client_fd, hdr, hlen, 0);
            if (strcmp(method, "GET") == 0) {
                while ((r = read(fd, buf, sizeof(buf))) > 0)
                    send(client_fd, buf, r, 0);
            }
            close(fd);
        }
    } else if (strcmp(method, "POST") == 0) {
        /* ---------------------- POST ------------------------------------- */
        char *cl = strstr(buf, "Content-Length:");
        int len = 0;
        if (cl) sscanf(cl, "Content-Length: %d", &len);
        if (len <= 0) {
            send_header(client_fd, 411, "Length Required");
        } else {
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) send_header(client_fd, 400, "Bad Request");
            else {
                body += 4;
                int recb = r - (body - buf);
                int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (fd < 0) send_header(client_fd, 409, "Conflict");
                else {
                    write(fd, body, recb);
                    int total = recb;
                    while (total < len && (r = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
                        write(fd, buf, r);
                        total += r;
                    }
                    close(fd);
                    send_header(client_fd, 201, "Created");
                }
            }
        }
    } else if (strcmp(method, "PUT") == 0) {
        /* ---------------------- PUT -------------------------------------- */
        char *cl = strstr(buf, "Content-Length:");
        int len = 0;
        if (cl) sscanf(cl, "Content-Length: %d", &len);
        if (len <= 0) {
            send_header(client_fd, 411, "Length Required");
        } else {
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) send_header(client_fd, 400, "Bad Request");
            else {
                body += 4;
                int recb = r - (body - buf);
                int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                write(fd, body, recb);
                int total = recb;
                while (total < len && (r = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
                    write(fd, buf, r);
                    total += r;
                }
                close(fd);
                send_header(client_fd, 200, "OK");
            }
        }
    } else if (strcmp(method, "DELETE") == 0) {
        /* ---------------------- DELETE ----------------------------------- */
        if (access(full_path, F_OK) < 0) send_header(client_fd, 404, "Not Found");
        else {
            remove(full_path);
            send_header(client_fd, 200, "OK");
        }
    } else {
        /* ---------------------- No permitido ------------------------------ */
        send_header(client_fd, 405, "Method Not Allowed");
    }

    close(client_fd);
    sem_post(&proc_sem);
}

/*======================  Creación del socket servidor  ======================*/

/**
 * @brief Configura y devuelve un *socket* TCP listo para `listen()`.
 * @param port Número de puerto TCP.
 * @return     Descriptor de *socket* en modo escucha.
 */
int setup_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port)
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(fd, BACKLOG) < 0)                             { perror("listen"); exit(EXIT_FAILURE); }

    return fd;
}

/*=======================  Análisis de argumentos CLI  =======================*/

/**
 * @brief Parsea los argumentos de línea de comandos y rellena @p cfg.
 *
 * @param argc Conteo de argumentos.
 * @param argv Vector de argumentos.
 * @param cfg  Estructura de configuración a llenar.
 */
void parse_args(int argc, char *argv[], server_config_t *cfg) {
    int opt;
    cfg->protocol      = strdup("http");   /* Valor por defecto */
    cfg->request_limit = 0;

    while ((opt = getopt(argc, argv, "n:w:p:T:L:")) != -1) {
        switch (opt) {
            case 'n': cfg->process_count = atoi(optarg); break;
            case 'w': cfg->root_dir      = strdup(optarg); break;
            case 'p': cfg->port          = atoi(optarg); break;
            case 'T': free(cfg->protocol); cfg->protocol = strdup(optarg); break;
            case 'L': cfg->request_limit = atoi(optarg); break;
            default:
                fprintf(stderr,
                        "Uso: %s -n <procesos> -w <root> -p <puerto> [-T <proto>] [-L <límite>]\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (!cfg->process_count || !cfg->root_dir || !cfg->port) {
        fprintf(stderr, "Parámetros incompletos.\n");
        exit(EXIT_FAILURE);
    }
}

/*=================================== main ==================================*/

/**
 * @brief Punto de entrada del programa.
 */
int main(int argc, char *argv[]) {
    /* ------------------ Leer parámetros ---------------------------------- */
    parse_args(argc, argv, &config);

    printf("Iniciando pre-forked WebServer: puerto %d, procesos %d, root %s, proto %s, límite %d\n",
           config.port, config.process_count, config.root_dir, config.protocol, config.request_limit);

    /* ------------------ Crear memoria compartida ------------------------- */
    served_requests = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (served_requests == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    *served_requests = 0;

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);

    served_mutex = mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (served_mutex == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    pthread_mutex_init(served_mutex, &mattr);

    /* ------------------ Inicializar semáforo y socket -------------------- */
    signal(SIGINT, sigint_handler);

    if (sem_init(&proc_sem, 1, config.process_count) != 0) { perror("sem_init"); exit(EXIT_FAILURE); }
    listen_fd = setup_server_socket(config.port);

    /* ------------------ Crear procesos hijo ----------------------------- */
    child_pids = calloc(config.process_count, sizeof(pid_t));
    for (int i = 0; i < config.process_count; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) {
            /* ------------ Bucle de servicio en proceso hijo ------------- */
            while (1) {
                int client_fd = accept(listen_fd, NULL, NULL);
                if (client_fd < 0) continue; /* Ignorar fallos transitorios */
                handle_request(client_fd);
            }
            exit(EXIT_SUCCESS); /* No debería alcanzarse */
        } else {
            child_pids[i] = pid;
        }
    }

    /* ------------------ Proceso padre espera señales -------------------- */
    for (;;) pause();

    return 0; /* Inalcanzable */
}
