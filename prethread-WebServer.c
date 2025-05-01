/**
 * @file prethread-WebServer.c
 * @brief Pre-threaded HTTP/1.1 WebServer en C con protocolo configurable
 *
 * Permite servir ficheros estáticos desde un directorio raíz, manejar conexiones
 * concurrentes con un pool de hilos (pthreads) y soporta métodos HTTP: GET, HEAD,
 * POST, PUT y DELETE. Además permite enviar banners de otros protocolos (FTP, SSH,
 * SMTP, DNS, TELNET, SNMP) según el parámetro -T.
 *
 * Sintaxis:
 *   $ prethread-WebServer -n <num_hilos> -w <http_root> -p <port> [-T <protocolo>]
 *
 * Ejemplos:
 *   ./prethread-WebServer -n 4 -w ./var/www -p 8080
 *   ./prethread-WebServer -n 4 -w ./var/www -p 21 -T ftp
 *
 * Requisitos:
 *   - HTTP/1.1: Content-Length, Connection: close.
 *   - Métodos: GET, HEAD, POST, PUT, DELETE.
 *   - Protocolo configurable: http (por defecto), ftp, ssh, smtp, dns, telnet, snmp.
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
#include <semaphore.h>
#include <sys/stat.h>

#define BACKLOG 16       /**< Tamaño de la cola de conexiones entrantes */
#define BUF_SIZE 8192    /**< Tamaño del buffer para petición/respuesta */

/** Configuración del servidor */
typedef struct {
    int  port;          /**< Puerto de escucha */
    char *root_dir;     /**< Directorio raíz de ficheros */
    int  thread_count;  /**< Número de hilos en el pool */
    char *protocol;     /**< Protocolo a reportar: "http","ftp","ssh",... */
} server_config_t;

static server_config_t config;
static sem_t thread_sem;     /**< Semáforo para limitar clientes */

/** Nodo para la cola de sockets */
typedef struct sock_node {
    int client_fd;
    struct sock_node *next;
} sock_node_t;

/** Cola de sockets con sincronización */
typedef struct {
    sock_node_t *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} socket_queue_t;

static socket_queue_t sock_queue;

/* Prototipos */
void queue_init(socket_queue_t *q);
void queue_push(socket_queue_t *q, int client_fd);
int  queue_pop(socket_queue_t *q);
int  setup_server_socket(int port);
void parse_args(int argc, char *argv[], server_config_t *cfg);
void *worker_routine(void *arg);
void handle_request(int client_fd);
void send_header(int fd, int status, const char *reason);
void send_banner(int client_fd);

/** Inicializa la cola de sockets. */
void queue_init(socket_queue_t *q) {
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

/** Encola un descriptor de cliente. */
void queue_push(socket_queue_t *q, int client_fd) {
    sock_node_t *node = malloc(sizeof(*node));
    if (!node) return;
    node->client_fd = client_fd;
    node->next = NULL;
    pthread_mutex_lock(&q->mutex);
      if (!q->tail) q->head = node;
      else          q->tail->next = node;
      q->tail = node;
      pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/** Desencola un descriptor de cliente (bloqueante). */
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

/** Envía cabecera HTTP simple sin cuerpo. */
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

/** Envía banner de protocolo alternativo. */
void send_banner(int client_fd) {
    const char *banner;
    if      (strcmp(config.protocol, "ftp")==0)    banner = "220 Servicio FTP listo\r\n";
    else if (strcmp(config.protocol, "ssh")==0)    banner = "SSH-2.0-OpenSSH_8.0\r\n";
    else if (strcmp(config.protocol, "smtp")==0)   banner = "220 ejemplo.com ESMTP\r\n";
    else if (strcmp(config.protocol, "dns")==0)    banner = "; DNS server up\r\n";
    else if (strcmp(config.protocol, "telnet")==0) banner = "telnet ready\r\n";
    else if (strcmp(config.protocol, "snmp")==0)   banner = "SNMP agent listening\r\n";
    else                                             banner = "200 OK\r\n";
    send(client_fd, banner, strlen(banner), 0);
    close(client_fd);
}

/** Maneja una petición HTTP o envía banner de otro protocolo. */
void handle_request(int client_fd) {
    /* Limitar clientes concurrentes */
    /* El semáforo ya se decremented en accept, se reponen tras manejar */

    /* Protocolo no-HTTP */
    if (strcmp(config.protocol, "http")!=0) {
        send_banner(client_fd);
        sem_post(&thread_sem);
        return;
    }

    /* Leer petición HTTP */
    char buf[BUF_SIZE];
    ssize_t received = recv(client_fd, buf, sizeof(buf)-1, 0);
    if (received<=0) { close(client_fd); sem_post(&thread_sem); return; }
    buf[received] = '\0';

    /* Parsear petición */
    char method[16], path[256], version[16];
    if (sscanf(buf, "%15s %255s %15s", method, path, version)!=3) {
        send_header(client_fd,400,"Bad Request");
        close(client_fd); sem_post(&thread_sem); return;
    }
    if (strcmp(version,"HTTP/1.1")!=0) {
        send_header(client_fd,505,"HTTP Version Not Supported");
        close(client_fd); sem_post(&thread_sem); return;
    }

    /* Construir ruta */
    char full_path[512];
    snprintf(full_path,sizeof(full_path),"%s%s",config.root_dir,path);

    /* Procesar métodos */
    if (strcmp(method,"GET")==0||strcmp(method,"HEAD")==0) {
        int fd=open(full_path,O_RDONLY);
        if(fd<0) send_header(client_fd,404,"Not Found");
        else {
            struct stat st; fstat(fd,&st);
            char hdr[BUF_SIZE];
            int hlen=snprintf(hdr,sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                st.st_size);
            send(client_fd,hdr,hlen,0);
            if(strcmp(method,"GET")==0) {
                ssize_t r;
                while((r=read(fd,buf,sizeof(buf)))>0)
                    send(client_fd,buf,r,0);
            }
            close(fd);
        }
    } else if(strcmp(method,"POST")==0) {
        char *cl=strstr(buf,"Content-Length:"); int len=0;
        if(cl) sscanf(cl,"Content-Length: %d",&len);
        if(len<=0) send_header(client_fd,411,"Length Required");
        else {
            char *body=strstr(buf,"\r\n\r\n");
            if(!body) send_header(client_fd,400,"Bad Request");
            else {
                body+=4; int rec=received-(body-buf);
                int fd=open(full_path,O_WRONLY|O_CREAT|O_EXCL,0666);
                if(fd<0) send_header(client_fd,409,"Conflict");
                else {
                    write(fd,body,rec);
                    int total=rec;
                    while(total<len&&(rec=recv(client_fd,buf,BUF_SIZE,0))>0) {
                        write(fd,buf,rec); total+=rec;
                    }
                    close(fd); send_header(client_fd,201,"Created");
                }
            }
        }
    } else if(strcmp(method,"PUT")==0) {
        char *cl=strstr(buf,"Content-Length:"); int len=0;
        if(cl) sscanf(cl,"Content-Length: %d",&len);
        if(len<=0) send_header(client_fd,411,"Length Required");
        else {
            char *body=strstr(buf,"\r\n\r\n");
            if(!body) send_header(client_fd,400,"Bad Request");
            else {
                body+=4; int rec=received-(body-buf);
                int fd=open(full_path,O_WRONLY|O_CREAT|O_TRUNC,0666);
                write(fd,body,rec);
                int total=rec;
                while(total<len&&(rec=recv(client_fd,buf,BUF_SIZE,0))>0) {
                    write(fd,buf,rec); total+=rec;
                }
                close(fd); send_header(client_fd,200,"OK");
            }
        }
    } else if(strcmp(method,"DELETE")==0) {
        if(access(full_path,F_OK)<0) send_header(client_fd,404,"Not Found");
        else { remove(full_path); send_header(client_fd,200,"OK"); }
    } else {
        send_header(client_fd,405,"Method Not Allowed");
    }

    close(client_fd);
    sem_post(&thread_sem);
}

/** Configura socket y escucha en puerto dado. */
int setup_server_socket(int port) {
    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd<0){perror("socket"); exit(EXIT_FAILURE);}
    int opt=1; setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={.sin_family=AF_INET,
                              .sin_addr.s_addr=INADDR_ANY,
                              .sin_port=htons(port)};
    if(bind(sockfd,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind"); exit(EXIT_FAILURE);}
    if(listen(sockfd,BACKLOG)<0){perror("listen"); exit(EXIT_FAILURE);}
    return sockfd;
}

/** Procesa args: -n, -w, -p, -T. */
void parse_args(int argc, char *argv[], server_config_t *cfg) {
    int opt;
    cfg->protocol=strdup("http");
    while((opt=getopt(argc,argv,"n:w:p:T:"))!=-1) {
        switch(opt) {
            case 'n': cfg->thread_count=atoi(optarg); break;
            case 'w': cfg->root_dir=strdup(optarg); break;
            case 'p': cfg->port=atoi(optarg); break;
            case 'T': free(cfg->protocol); cfg->protocol=strdup(optarg); break;
            default:
                fprintf(stderr,"Uso: %s -n <hilos> -w <root> -p <puerto> [-T <protocolo>]\n",argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if(!cfg->thread_count||!cfg->root_dir||!cfg->port) {
        fprintf(stderr,"Parámetros incompletos.\n"); exit(EXIT_FAILURE);
    }
}

/** Rutina de hilo trabajador. */
void *worker_routine(void *arg) {
    (void)arg;
    while(1) {
        int client_fd=queue_pop(&sock_queue);
        handle_request(client_fd);
    }
    return NULL;
}

int main(int argc,char *argv[]) {
    parse_args(argc,argv,&config);
    printf("Iniciando pre-threaded WebServer: puerto %d, hilos %d, root %s, protocolo %s\n",
           config.port,config.thread_count,config.root_dir,config.protocol);

    /* Inicializar semáforo y cola */
    sem_init(&thread_sem,0,config.thread_count);
    queue_init(&sock_queue);

    /* Crear pool de hilos */
    pthread_t *threads=malloc(sizeof(pthread_t)*config.thread_count);
    for(int i=0;i<config.thread_count;++i) {
        pthread_create(&threads[i],NULL,worker_routine,NULL);
    }

    /* Socket de escucha */
    int server_fd=setup_server_socket(config.port);
    while(1) {
        int client_fd=accept(server_fd,NULL,NULL);
        if(client_fd<0) continue;
        /* Control de capacidad */
        if(sem_trywait(&thread_sem)<0) {
            send_header(client_fd,503,"Service Unavailable: Too many clients");
            close(client_fd);
        } else {
            queue_push(&sock_queue,client_fd);
        }
    }
    close(server_fd);
    return 0;
}