/**
 * @file prethread-WebServer.c
 * @brief Servidor Web pre–threaded con soporte HTTP/1.1 y envío opcional de banners de otros protocolos.
 *
 * El programa crea un _pool_ de hilos que atiende
 * concurrentemente conexiones entrantes, sirviendo archivos estáticos desde un directorio raíz
 * y gestionando los métodos **GET**, **HEAD**, **POST**, **PUT** y **DELETE**.
 *
 * Además permite simular otros servicios (FTP, SSH, SMTP, DNS, etc.) enviando únicamente el
 * banner correspondiente y cerrando la conexión.
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
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>

/** Número máximo de conexiones en la cola de espera de `accept()`. */
#define BACKLOG 16

/** Tamaño del búfer utilizado para cabeceras y transferencias. */
#define BUF_SIZE 8192

/**
 * @struct server_config_t
 * @brief Estructura que encapsula todos los parámetros de configuración del servidor.
 */
typedef struct {
    int   port;          /**< Puerto TCP donde el servidor escuchará.               */
    char *root_dir;      /**< Directorio raíz de los archivos servidos.            */
    int   thread_count;  /**< Cantidad de hilos en el _pool_.                      */
    char *protocol;      /**< Protocolo a emular (`"http"`, `"ftp"`, etc.).       */
    int   request_limit; /**< Límite global de peticiones; 0 =infinito.            */
} server_config_t;

/** Configuración global, inicializada por @ref parse_args. */
static server_config_t config;

/**
 * Contador global de peticiones servidas. Se protege mediante
 * @ref served_mutex para evitar condiciones de carrera.
 */
static int served_requests = 0;
static pthread_mutex_t served_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @struct sock_node
 * @brief Nodo de la cola de sockets pendientes.
 */
typedef struct sock_node {
    int client_fd;              /**< Descriptor del socket del cliente. */
    struct sock_node *next;     /**< Puntero al siguiente nodo.        */
} sock_node_t;

/**
 * @struct socket_queue_t
 * @brief Cola FIFO protegida por mutex/condición para pasar sockets a los hilos.
 */
typedef struct {
    sock_node_t    *head;   /**< Primer nodo de la cola.   */
    sock_node_t    *tail;   /**< Último nodo de la cola.   */
    pthread_mutex_t mutex;  /**< Mutex que protege la cola.*/
    pthread_cond_t  cond;   /**< Condición para notificar hilos. */
} socket_queue_t;

/** Cola global de sockets utilizada por todos los hilos de trabajo. */
static socket_queue_t sock_queue;

/*==============================  Cola de sockets  ==============================*/

/**
 * @brief Inicializa la cola de sockets.
 * @param[out] q  Cola a inicializar.
 */
void queue_init(socket_queue_t *q) {
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

/**
 * @brief Inserta un descriptor de cliente al final de la cola.
 *
 * Se reserva memoria para un nuevo nodo y se notifica a los hilos que
 * esperan mediante la condición.
 *
 * @param q         Cola destino.
 * @param client_fd Descriptor del socket aceptado.
 */
void queue_push(socket_queue_t *q, int client_fd) {
    sock_node_t *node = malloc(sizeof(*node));
    node->client_fd = client_fd;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (!q->tail) q->head = node;
    else q->tail->next = node;
    q->tail = node;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/**
 * @brief Extrae y devuelve el primer descriptor disponible en la cola.
 * @param q Cola origen.
 * @return  Descriptor de socket de cliente.
 */
int queue_pop(socket_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    while (!q->head)                     /* Espera activa si la cola está vacía */
        pthread_cond_wait(&q->cond, &q->mutex);

    sock_node_t *node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;        /* Cola vacía tras la extracción */
    pthread_mutex_unlock(&q->mutex);

    int fd = node->client_fd;
    free(node);
    return fd;
}

/*========================  Utilidades para respuestas  =========================*/

/**
 * @brief Envía una cabecera HTTP mínima y cierra la respuesta.
 *
 * @param fd      Descriptor de socket destino.
 * @param status  Código de estado HTTP (p. ej. 200, 404…).
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

/*==========================  Manejo de peticiones  =============================*/

/**
 * @brief Atiende una única petición de cliente.
 *
 * Valida el método, versión HTTP y ruta solicitada.  Delega en funciones
 * auxiliares para operaciones sobre archivos y envía la respuesta apropiada.
 * Se encarga también de:
 *  - Limitar la cantidad global de peticiones con **503 Service Unavailable**.
 *  - Enviar banners de protocolos distintos a HTTP (FTP, SSH, etc.).
 *
 * @param client_fd Descriptor de socket del cliente.
 */
void handle_request(int client_fd) {
    /* ----- Control de límite global de peticiones -------------------------- */
    if (config.request_limit > 0) {
        pthread_mutex_lock(&served_mutex);
        served_requests++;
        if (served_requests > config.request_limit) {
            pthread_mutex_unlock(&served_mutex);
            send_header(client_fd, 503, "Service Unavailable");
            close(client_fd);
            return;                       /* Continúa el servidor, no termina */
        }
        pthread_mutex_unlock(&served_mutex);
    }

    /* ----- Protocolo distinto de HTTP: sólo banner ------------------------- */
    if (strcmp(config.protocol, "http") != 0) {
        const char *banner;
        if      (strcmp(config.protocol, "ftp")  == 0) banner = "220 Servicio FTP listo\r\n";
        else if (strcmp(config.protocol, "ssh")  == 0) banner = "SSH-2.0-OpenSSH_8.0\r\n";
        else if (strcmp(config.protocol, "smtp") == 0) banner = "220 ejemplo.com ESMTP\r\n";
        else if (strcmp(config.protocol, "dns")  == 0) banner = "; DNS server up\r\n";
        else if (strcmp(config.protocol, "telnet") == 0) banner = "telnet ready\r\n";
        else if (strcmp(config.protocol, "snmp") == 0) banner = "SNMP agent listening\r\n";
        else banner = "200 OK\r\n";   /* Fallback */

        send(client_fd, banner, strlen(banner), 0);
        close(client_fd);
        return;
    }

    /* ----- Recepción y parsing de la petición HTTP ------------------------- */
    char buf[BUF_SIZE];
    ssize_t rec = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (rec <= 0) { close(client_fd); return; }
    buf[rec] = '\0';

    char method[16], path[256], version[16];
    if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3) {
        send_header(client_fd, 400, "Bad Request"); close(client_fd); return;
    }
    if (strcmp(version, "HTTP/1.1") != 0) {
        send_header(client_fd, 505, "HTTP Version Not Supported"); close(client_fd); return;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", config.root_dir, path);

    /* ===================  Métodos soportados  =================== */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        /* ----------- GET / HEAD ------------------------------------------- */
        int fd = open(full_path, O_RDONLY);
        if (fd < 0) {
            send_header(client_fd, 404, "Not Found");
        } else {
            struct stat st; fstat(fd, &st);
            char hdr[BUF_SIZE];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", st.st_size);
            send(client_fd, hdr, hlen, 0);
            if (strcmp(method, "GET") == 0) {
                ssize_t r;                /* Enviar cuerpo sólo para GET */
                while ((r = read(fd, buf, sizeof(buf))) > 0)
                    send(client_fd, buf, r, 0);
            }
            close(fd);
        }
    } else if (strcmp(method, "POST") == 0) {
        /* ----------- POST -------------------------------------------------- */
        char *cl = strstr(buf, "Content-Length:");
        int len = 0;
        if (cl) sscanf(cl, "Content-Length: %d", &len);
        if (len <= 0) {
            send_header(client_fd, 411, "Length Required");
        } else {
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) send_header(client_fd, 400, "Bad Request");
            else {
                body += 4;                        /* Avanza tras cabeceras */
                int recb = rec - (body - buf);
                int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (fd < 0) {
                    send_header(client_fd, 409, "Conflict");
                } else {
                    write(fd, body, recb);
                    int total = recb;
                    while (total < len && (rec = recv(client_fd, buf, BUF_SIZE, 0)) > 0) {
                        write(fd, buf, rec);
                        total += rec;
                    }
                    close(fd);
                    send_header(client_fd, 201, "Created");
                }
            }
        }
    } else if (strcmp(method, "PUT") == 0) {
        /* ----------- PUT --------------------------------------------------- */
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
                int recb = rec - (body - buf);
                int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                write(fd, body, recb);
                int total = recb;
                while (total < len && (rec = recv(client_fd, buf, BUF_SIZE, 0)) > 0) {
                    write(fd, buf, rec);
                    total += rec;
                }
                close(fd);
                send_header(client_fd, 200, "OK");
            }
        }
    } else if (strcmp(method, "DELETE") == 0) {
        /* ----------- DELETE ----------------------------------------------- */
        if (access(full_path, F_OK) < 0) {
            send_header(client_fd, 404, "Not Found");
        } else {
            remove(full_path);
            send_header(client_fd, 200, "OK");
        }
    } else {
        /* ----------- Método no permitido ----------------------------------- */
        send_header(client_fd, 405, "Method Not Allowed");
    }

    close(client_fd);
}

/*============================  Rutina de hilo  ===============================*/

/**
 * @brief Función que ejecuta cada hilo trabajador del _pool_.
 * @param arg Argumento no utilizado.
 * @return    Siempre `NULL`.
 */
void *worker_routine(void *arg) {
    (void)arg;
    while (1) {
        int client_fd = queue_pop(&sock_queue);
        handle_request(client_fd);
    }
    return NULL;
}

/*=========================  Creación de socket servidor  =====================*/

/**
 * @brief Configura y devuelve un socket TCP en modo escucha.
 *
 * Aplica `SO_REUSEADDR`, enlaza al puerto indicado y llama a `listen()`.
 * El programa aborta si alguna de las operaciones falla.
 *
 * @param port Número de puerto TCP.
 * @return     Descriptor de socket en modo escucha.
 */
int setup_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port)
    };

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(sockfd, BACKLOG) < 0)                                { perror("listen"); exit(EXIT_FAILURE); }

    return sockfd;
}

/*============================  Parámetros CLI  ===============================*/

/**
 * @brief Analiza los argumentos de línea de comandos y llena @p cfg.
 *
 * Finaliza el programa con **EXIT_FAILURE** si faltan parámetros obligatorios
 * o si ocurre un error de parsing.
 *
 * @param argc  Conteo de argumentos.
 * @param argv  Vector de argumentos.
 * @param cfg   Estructura destino.
 */
void parse_args(int argc, char *argv[], server_config_t *cfg) {
    int opt;
    cfg->protocol = strdup("http");   /* Valor por defecto */
    cfg->request_limit = 0;

    while ((opt = getopt(argc, argv, "n:w:p:T:L:")) != -1) {
        switch (opt) {
            case 'n': cfg->thread_count = atoi(optarg); break;
            case 'w': cfg->root_dir     = strdup(optarg); break;
            case 'p': cfg->port         = atoi(optarg); break;
            case 'T': free(cfg->protocol); cfg->protocol = strdup(optarg); break;
            case 'L': cfg->request_limit = atoi(optarg); break;
            default:
                fprintf(stderr, "Uso: %s -n <hilos> -w <root> -p <puerto> [-T <proto>] [-L <límite>]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (!cfg->thread_count || !cfg->root_dir || !cfg->port) {
        fprintf(stderr, "Parámetros incompletos.\n");
        exit(EXIT_FAILURE);
    }
}

/*=================================== main ====================================*/

/**
 * @brief Punto de entrada del programa.
 *
 * Inicializa la configuración, crea el _pool_ de hilos, configura el socket
 * servidor y entra en un bucle infinito aceptando conexiones.
 */
int main(int argc, char *argv[]) {
    parse_args(argc, argv, &config);

    printf("Iniciando WebServer (threaded): puerto %d, hilos %d, root %s, proto %s, límite %d\n",
           config.port, config.thread_count, config.root_dir, config.protocol, config.request_limit);

    queue_init(&sock_queue);

    pthread_t *threads = malloc(sizeof(pthread_t) * config.thread_count);
    for (int i = 0; i < config.thread_count; ++i) {
        if (pthread_create(&threads[i], NULL, worker_routine, NULL) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    int server_fd = setup_server_socket(config.port);
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;          /* Ignora fallos transitorios */
        queue_push(&sock_queue, client_fd);
    }

    /* No se alcanza, pero por completitud */
    close(server_fd);
    return 0;
}
