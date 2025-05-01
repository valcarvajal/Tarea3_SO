/**
 * @file prethread-WebServer.c
 * @brief Pre-threaded HTTP/1.1 WebServer en C con límite de peticiones para DoS.
 *
 * Añade un parámetro -L <límite> de peticiones tras el cual el servidor aborta.
 * Permite servir ficheros estáticos, manejo de HTTP/1.1 y métodos CRUD.
 *
 * Sintaxis:
 *   $ prethread-WebServer -n <num_hilos> -w <http_root> -p <port> [-T <protocolo>] [-L <límite>]
 *
 * Ejemplo:
 *   ./prethread-WebServer -n 4 -w ./var/www -p 8080 -L 100
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
 
 #define BACKLOG 16
 #define BUF_SIZE 8192
 
 /** Configuración del servidor */
 typedef struct {
     int port;
     char *root_dir;
     int thread_count;
     char *protocol;
     int request_limit;    // Límite de peticiones antes de abortar (0=infinito)
 } server_config_t;
 
 static server_config_t config;
 
 /** Contador de peticiones global */
 static int served_requests = 0;
 static pthread_mutex_t served_mutex = PTHREAD_MUTEX_INITIALIZER;
 
 /** Nodo de cola */
 typedef struct sock_node {
     int client_fd;
     struct sock_node *next;
 } sock_node_t;
 
 /** Cola de sockets */
 typedef struct {
     sock_node_t *head, *tail;
     pthread_mutex_t mutex;
     pthread_cond_t cond;
 } socket_queue_t;
 
 static socket_queue_t sock_queue;
 
 void queue_init(socket_queue_t *q) {
     q->head = q->tail = NULL;
     pthread_mutex_init(&q->mutex, NULL);
     pthread_cond_init(&q->cond, NULL);
 }
 
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
 
 void handle_request(int client_fd) {
     // Límite de peticiones
     if (config.request_limit > 0) {
         pthread_mutex_lock(&served_mutex);
         if (++served_requests > config.request_limit) {
             pthread_mutex_unlock(&served_mutex);
             fprintf(stderr, "Límite de %d peticiones alcanzado, abortando.\n", config.request_limit);
             exit(EXIT_FAILURE);
         }
         pthread_mutex_unlock(&served_mutex);
     }
 
     // Protocolos distintos de HTTP: banner y cierre
     if (strcmp(config.protocol, "http") != 0) {
         const char *banner;
         if      (strcmp(config.protocol, "ftp")==0)    banner = "220 Servicio FTP listo\r\n";
         else if (strcmp(config.protocol, "ssh")==0)    banner = "SSH-2.0-OpenSSH_8.0\r\n";
         else if (strcmp(config.protocol, "smtp")==0)   banner = "220 ejemplo.com ESMTP\r\n";
         else if (strcmp(config.protocol, "dns")==0)    banner = "; DNS server up\r\n";
         else if (strcmp(config.protocol, "telnet")==0) banner = "telnet ready\r\n";
         else if (strcmp(config.protocol, "snmp")==0)   banner = "SNMP agent listening\r\n";
         else banner = "200 OK\r\n";
         send(client_fd, banner, strlen(banner), 0);
         close(client_fd);
         return;
     }
 
     char buf[BUF_SIZE];
     ssize_t rec = recv(client_fd, buf, sizeof(buf)-1, 0);
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
 
     // GET/HEAD
     if (strcmp(method, "GET")==0 || strcmp(method, "HEAD")==0) {
         int fd = open(full_path, O_RDONLY);
         if (fd < 0) send_header(client_fd, 404, "Not Found");
         else {
             struct stat st; fstat(fd, &st);
             char hdr[BUF_SIZE];
             int hlen = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", st.st_size);
             send(client_fd, hdr, hlen, 0);
             if (strcmp(method, "GET")==0) {
                 ssize_t r;
                 while ((r = read(fd, buf, sizeof(buf)))>0) send(client_fd, buf, r, 0);
             }
             close(fd);
         }
     }
     // POST
     else if (strcmp(method, "POST")==0) {
         char *cl = strstr(buf, "Content-Length:"); int len=0;
         if (cl) sscanf(cl, "Content-Length: %d", &len);
         if (len<=0) send_header(client_fd, 411, "Length Required");
         else {
             char *body = strstr(buf, "\r\n\r\n");
             if (!body) send_header(client_fd,400,"Bad Request");
             else {
                 body+=4; int recb = rec - (body-buf);
                 int fd = open(full_path, O_WRONLY|O_CREAT|O_EXCL, 0666);
                 if (fd<0) send_header(client_fd,409,"Conflict");
                 else {
                     write(fd, body, recb);
                     int total = recb;
                     while (total < len && (rec = recv(client_fd, buf, BUF_SIZE,0))>0) {
                         write(fd, buf, rec); total+=rec;
                     }
                     close(fd); send_header(client_fd,201,"Created");
                 }
             }
         }
     }
     // PUT
     else if (strcmp(method, "PUT")==0) {
         char *cl = strstr(buf, "Content-Length:"); int len=0;
         if (cl) sscanf(cl, "Content-Length: %d", &len);
         if (len<=0) send_header(client_fd,411,"Length Required");
         else {
             char *body = strstr(buf, "\r\n\r\n");
             if (!body) send_header(client_fd,400,"Bad Request");
             else {
                 body+=4; int recb = rec - (body-buf);
                 int fd = open(full_path, O_WRONLY|O_CREAT|O_TRUNC,0666);
                 write(fd, body, recb);
                 int total=recb;
                 while (total < len && (rec = recv(client_fd, buf, BUF_SIZE,0))>0) {
                     write(fd, buf, rec); total+=rec;
                 }
                 close(fd); send_header(client_fd,200,"OK");
             }
         }
     }
     // DELETE
     else if (strcmp(method, "DELETE")==0) {
         if (access(full_path,F_OK)<0) send_header(client_fd,404,"Not Found");
         else { remove(full_path); send_header(client_fd,200,"OK"); }
     }
     else { send_header(client_fd,405,"Method Not Allowed"); }
 
     close(client_fd);
 }
 
 int setup_server_socket(int port) {
     int sockfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sockfd<0){perror("socket");exit(EXIT_FAILURE);}
     int opt=1; setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
     struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=htons(port)};
     if (bind(sockfd,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind");exit(EXIT_FAILURE);}    
     if (listen(sockfd,BACKLOG)<0){perror("listen");exit(EXIT_FAILURE);}    
     return sockfd;
 }
 
 void parse_args(int argc, char *argv[], server_config_t *cfg) {
     int opt;
     cfg->protocol = strdup("http");
     cfg->request_limit = 0;
     while ((opt = getopt(argc, argv, "n:w:p:T:L:")) != -1) {
         switch (opt) {
             case 'n': cfg->thread_count = atoi(optarg); break;
             case 'w': cfg->root_dir = strdup(optarg); break;
             case 'p': cfg->port = atoi(optarg); break;
             case 'T': free(cfg->protocol); cfg->protocol = strdup(optarg); break;
             case 'L': cfg->request_limit = atoi(optarg); break;
             default:
                 fprintf(stderr, "Uso: %s -n <hilos> -w <root> "
                         "-p <puerto> [-T <protocolo>] [-L <límite>]\n", argv[0]);
                 exit(EXIT_FAILURE);
         }
     }
     if (!cfg->thread_count || !cfg->root_dir || !cfg->port) {
         fprintf(stderr, "Parámetros incompletos.\n"); exit(EXIT_FAILURE);
     }
 }
 
 void *worker_routine(void *arg) {
     (void)arg;
     while (1) {
         int client_fd = queue_pop(&sock_queue);
         handle_request(client_fd);
     }
     return NULL;
 }
 
 int main(int argc, char *argv[]) {
     parse_args(argc, argv, &config);
     printf("Iniciando WebServer (threaded): puerto %d, hilos %d, root %s, proto %s, límite %d\n",
            config.port, config.thread_count, config.root_dir, config.protocol, config.request_limit);
     queue_init(&sock_queue);
     pthread_t *threads = malloc(sizeof(pthread_t) * config.thread_count);
     for (int i = 0; i < config.thread_count; ++i) {
         pthread_create(&threads[i], NULL, worker_routine, NULL);
     }
     int server_fd = setup_server_socket(config.port);
     while (1) {
         int client_fd = accept(server_fd, NULL, NULL);
         if (client_fd < 0) continue;
         queue_push(&sock_queue, client_fd);
     }
     close(server_fd);
     return 0;
 }
 