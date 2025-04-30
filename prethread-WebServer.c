/**
 * @file prethread-WebServer.c
 * @brief Pre-threaded HTTP/1.1 WebServer en C con manejo completo de métodos y errores
 *
 * Permite servir ficheros estáticos desde un directorio raíz, manejar conexiones
 * concurrentes con un pool de hilos (pthreads) y soporta métodos HTTP: GET, HEAD,
 * POST, PUT y DELETE, con gestión de errores.
 *
 * Sintaxis:
 *   $ prethread-WebServer -n <num_hilos> -w <http_root> -p <port>
 *
 * Requerimientos:
 *   - HTTP/1.1: Content-Length, Connection: close.
 *   - Manejo completo de POST, PUT, DELETE.
 *   - Tratamiento de errores y códigos HTTP apropiados.
 *
 * Uso:
 *   gcc prethread-WebServer.c -o prethread-WebServer -lpthread
 *   ./prethread-WebServer -n 4 -w ./var/www -p 8080
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <getopt.h>
 #include <errno.h>
 #include <fcntl.h>
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <pthread.h>
 #include <sys/stat.h>
 
 #define BACKLOG 16       /**< Tamaño de la cola de conexiones entrantes */
 #define BUF_SIZE 8192    /**< Tamaño del buffer de lectura/escritura */
 
 /** Configuración del servidor */
 typedef struct {
     int port;
     char *root_dir;
     int thread_count;
 } server_config_t;
 
 /** Nodo para la cola de sockets */
 typedef struct sock_node {
     int client_fd;
     struct sock_node *next;
 } sock_node_t;
 
 /** Cola de sockets con sincronización */
 typedef struct {
     sock_node_t *head, *tail;
     pthread_mutex_t mutex;
     pthread_cond_t cond;
 } socket_queue_t;
 
 static socket_queue_t sock_queue;
 static server_config_t config;
 
 /** Inicializa la cola de sockets */
 void queue_init(socket_queue_t *q) {
     q->head = q->tail = NULL;
     pthread_mutex_init(&q->mutex, NULL);
     pthread_cond_init(&q->cond, NULL);
 }
 
 /** Encola un descriptor de cliente */
 void queue_push(socket_queue_t *q, int client_fd) {
     sock_node_t *node = malloc(sizeof(*node));
     if (!node) return;
     node->client_fd = client_fd;
     node->next = NULL;
     pthread_mutex_lock(&q->mutex);
     if (!q->tail) q->head = node;
     else q->tail->next = node;
     q->tail = node;
     pthread_cond_signal(&q->cond);
     pthread_mutex_unlock(&q->mutex);
 }
 
 /** Desencola un descriptor de cliente (bloqueante) */
 int queue_pop(socket_queue_t *q) {
     pthread_mutex_lock(&q->mutex);
     while (!q->head) pthread_cond_wait(&q->cond, &q->mutex);
     sock_node_t *node = q->head;
     q->head = node->next;
     if (!q->head) q->tail = NULL;
     pthread_mutex_unlock(&q->mutex);
     int fd = node->client_fd;
     free(node);
     return fd;
 }
 
 /** Envía una respuesta HTTP con solo cabecera */
 void send_header(int client_fd, int status, const char *reason) {
     char buf[BUF_SIZE];
     int len = snprintf(buf, sizeof(buf),
         "HTTP/1.1 %d %s\r\n"
         "Content-Length: 0\r\n"
         "Connection: close\r\n"
         "\r\n",
         status, reason);
     if (send(client_fd, buf, len, 0) < 0) perror("send header");
 }
 
 /** Envía una respuesta de error simplificada */
 void send_error(int client_fd, int status, const char *reason) {
     send_header(client_fd, status, reason);
 }
 
 /** Maneja las solicitudes HTTP de un cliente */
 void handle_request(int client_fd) {
     char buf[BUF_SIZE];
     ssize_t received = recv(client_fd, buf, sizeof(buf) - 1, 0);
     if (received <= 0) { close(client_fd); return; }
     buf[received] = '\0';
 
     char method[16], path[256], version[16];
     if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3) {
         send_error(client_fd, 400, "Bad Request");
         close(client_fd);
         return;
     }
 
     if (strcmp(version, "HTTP/1.1") != 0) {
         send_error(client_fd, 505, "HTTP Version Not Supported");
         close(client_fd);
         return;
     }
 
     char full_path[512];
     snprintf(full_path, sizeof(full_path), "%s%s", config.root_dir, path);
 
     /** GET y HEAD */
     if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
         int fd = open(full_path, O_RDONLY);
         if (fd < 0) {
             send_error(client_fd, 404, "Not Found");
         } else {
             struct stat st;
             fstat(fd, &st);
             char header[BUF_SIZE];
             int hlen = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Length: %ld\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 st.st_size);
             send(client_fd, header, hlen, 0);
             if (strcmp(method, "GET") == 0) {
                 ssize_t r;
                 while ((r = read(fd, buf, sizeof(buf))) > 0) {
                     if (send(client_fd, buf, r, 0) < 0) perror("send body");
                 }
             }
             close(fd);
         }
     }
     /** POST y PUT */
     else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
         int content_length = 0;
         char *cl = strstr(buf, "Content-Length:");
         if (cl) sscanf(cl, "Content-Length: %d", &content_length);
         if (content_length <= 0) {
             send_error(client_fd, 411, "Length Required");
             close(client_fd);
             return;
         }
         char *body = strstr(buf, "\r\n\r\n");
         if (!body) {
             send_error(client_fd, 400, "Bad Request");
             close(client_fd);
             return;
         }
         body += 4;
         int header_len = body - buf;
         int received_body = received - header_len;
         char *data = malloc(content_length);
         if (!data) { send_error(client_fd, 500, "Internal Server Error"); close(client_fd); return; }
         memcpy(data, body, received_body);
         int total = received_body;
         while (total < content_length) {
             ssize_t r = recv(client_fd, data + total, content_length - total, 0);
             if (r <= 0) break;
             total += r;
         }
 
         int flags = O_WRONLY | O_CREAT | (strcmp(method, "POST") == 0 ? O_EXCL : O_TRUNC);
         mode_t mode = 0666;
         int fd = open(full_path, flags, mode);
         if (fd < 0) {
             if (errno == EEXIST && strcmp(method, "POST") == 0)
                 send_error(client_fd, 409, "Conflict");
             else
                 send_error(client_fd, 500, "Internal Server Error");
         } else {
             ssize_t w = write(fd, data, total);
             if (w < total) perror("write body");
             close(fd);
             send_header(client_fd,
                         strcmp(method, "POST") == 0 ? 201 : 200,
                         strcmp(method, "POST") == 0 ? "Created" : "OK");
         }
         free(data);
     }
     /** DELETE */
     else if (strcmp(method, "DELETE") == 0) {
         if (access(full_path, F_OK) < 0) {
             send_error(client_fd, 404, "Not Found");
         } else if (remove(full_path) < 0) {
             send_error(client_fd, 500, "Internal Server Error");
         } else {
             send_header(client_fd, 200, "OK");
         }
     }
     /** Método no permitido */
     else {
         send_error(client_fd, 405, "Method Not Allowed");
     }
 
     close(client_fd);
 }
 
 /** Rutina de hilo trabajador */
 void *worker_routine(void *arg) {
     (void)arg;
     while (1) {
         int client_fd = queue_pop(&sock_queue);
         handle_request(client_fd);
     }
     return NULL;
 }
 
 /** Configura y devuelve un socket servidor */
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
 
 /** Procesa argumentos de línea de comando */
 void parse_args(int argc, char *argv[], server_config_t *cfg) {
     int opt;
     while ((opt = getopt(argc, argv, "n:w:p:")) != -1) {
         switch (opt) {
             case 'n': cfg->thread_count = atoi(optarg); break;
             case 'w': cfg->root_dir = strdup(optarg); break;
             case 'p': cfg->port = atoi(optarg); break;
             default:
                 fprintf(stderr, "Uso: %s -n <hilos> -w <root> -p <puerto>\n", argv[0]);
                 exit(EXIT_FAILURE);
         }
     }
     if (!cfg->thread_count || !cfg->root_dir || !cfg->port) {
         fprintf(stderr, "Parámetros incompletos.\n"); exit(EXIT_FAILURE);
     }
 }
 
 /** Función principal */
 int main(int argc, char *argv[]) {
     parse_args(argc, argv, &config);
     printf("Iniciando servidor: puerto %d, hilos %d, root %s\n",
            config.port, config.thread_count, config.root_dir);
     queue_init(&sock_queue);
     pthread_t *threads = malloc(sizeof(pthread_t) * config.thread_count);
     for (int i = 0; i < config.thread_count; ++i) {
         if (pthread_create(&threads[i], NULL, worker_routine, NULL) != 0)
             perror("pthread_create");
     }
     int server_fd = setup_server_socket(config.port);
     while (1) {
         int client_fd = accept(server_fd, NULL, NULL);
         if (client_fd < 0) { perror("accept"); continue; }
         queue_push(&sock_queue, client_fd);
     }
     close(server_fd);
     return 0;
 }
 