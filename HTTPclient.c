/**
 * @file HTTPclient.c
 * @brief Cliente HTTP/1.1 en línea de comandos usando **libcurl**.
 *
 * Permite realizar peticiones **GET**, **HEAD**, **POST**, **PUT** y **DELETE** a un servidor
 * remoto.  Soporta descarga directa a fichero (`-o <archivo>`) o impresión en pantalla, así
 * como el envío de datos mediante `-d "<datos>"` para los métodos **POST** y **PUT**.
 *
 * ### Uso general
 *
 * HTTPclient -h <host:puerto> [ -o <fichero> ] <METHOD> <recurso> [ -d "<datos>" ]
 *
 * @author  Valery Carvajal y Anthony Rojas
 * @date    30 abr 2025
 */

/*============================  Inclusiones  ============================*/

#include <stddef.h>   /* size_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <curl/curl.h>

/*============================  Estructuras  ============================*/

/**
 * @struct Memory
 * @brief Búfer dinámico para acumular la respuesta HTTP en memoria.
 */
struct Memory {
    char   *data; /**< Puntero al bloque de memoria con la respuesta. */
    size_t  size; /**< Tamaño actual ocupado en @ref data.           */
};

/*==========================  Callbacks libcurl  ========================*/

/**
 * @brief Callback de escritura que acumula la respuesta en memoria.
 *
 * Reservará (con `realloc`) memoria adicional para almacenar los nuevos
 * bytes recibidos y actualizará el tamaño total.
 *
 * @param ptr    Puntero a los datos recibidos.
 * @param size   Tamaño de cada elemento.
 * @param nmemb  Número de elementos.
 * @param userp  Puntero al @ref Memory que se administra.
 * @return       Número de bytes realmente escritos (realsize).
 */
static size_t write_memory_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr_new = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr_new) return 0;              /* Error de memoria, aborta transferencia */

    mem->data = ptr_new;
    memcpy(&(mem->data[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

/**
 * @brief Callback de escritura que envía los datos directamente a un fichero.
 * @param ptr    Puntero al bloque recibido.
 * @param size   Tamaño de cada elemento.
 * @param nmemb  Número de elementos.
 * @param stream Puntero al `FILE` abierto en modo binario.
 * @return       Número de elementos efectivamente escritos.
 */
static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

/*===============================  main  ================================*/

/**
 * @brief Punto de entrada del cliente HTTP.
 *
 * Analiza los argumentos de línea de comandos, configura libcurl según el
 * método solicitado y gestiona la salida (pantalla o fichero).
 *
 * @return `EXIT_SUCCESS` si la operación concluye correctamente; de lo
 * contrario `EXIT_FAILURE`.
 */
int main(int argc, char **argv) {
    char *host = NULL, *method = NULL, *resource = NULL;
    char *data = NULL, *outfile = NULL;
    FILE *fp = NULL;
    int opt;

    /*---------------------  Parseo de argumentos -------------------------*/
    while ((opt = getopt(argc, argv, "h:o:d:")) != -1) {
        switch (opt) {
            case 'h': host    = strdup(optarg); break;
            case 'o': outfile = strdup(optarg); break;
            case 'd': data    = strdup(optarg); break;
            default:
                fprintf(stderr,
                        "Uso: %s -h <host:puerto> [-o <fichero>] <METHOD> <recurso> [-d \"datos\"]\n",
                        argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (!host || optind + 1 >= argc) {
        fprintf(stderr, "Error: se requiere -h <host> y METHOD + recurso\n");
        return EXIT_FAILURE;
    }

    method   = argv[optind++];
    resource = argv[optind++];

    /*---------------------  Construcción de la URL -----------------------*/
    char url[1024];
    if (strncmp(host, "http://", 7) && strncmp(host, "https://", 8))
        snprintf(url, sizeof(url), "http://%s%s", host, resource);
    else
        snprintf(url, sizeof(url), "%s%s", host, resource);

    /*---------------------  Inicialización de libcurl --------------------*/
    CURL *curl = curl_easy_init();
    if (!curl) return EXIT_FAILURE;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_HEADER, 1L);  /* Imprimir cabeceras */

    /*---------------------  Método HTTP ----------------------------------*/
    size_t data_len = data ? strlen(data) : 0;

    if (strcasecmp(method, "GET") == 0) {
        /* GET es el valor por defecto. */
    }
    else if (strcasecmp(method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    else if (strcasecmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (data) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (curl_off_t)data_len);
        }
    }
    else if (strcasecmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (data) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (curl_off_t)data_len);
        }
    }
    else if (strcasecmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    else {
        fprintf(stderr, "Error: método HTTP no soportado: %s\n", method);
        curl_easy_cleanup(curl);
        return EXIT_FAILURE;
    }

    /*---------------------  Selección de la salida -----------------------*/
    if (outfile) {
        fp = fopen(outfile, "wb");
        if (!fp) { perror("fopen"); curl_easy_cleanup(curl); return EXIT_FAILURE; }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    } else {
        struct Memory chunk = { .data = malloc(1), .size = 0 };
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            printf("%s\n", chunk.data);
        } else {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        free(chunk.data);
        curl_easy_cleanup(curl);
        return (res == CURLE_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /*---------------------  Ejecución cuando hay fichero ------------------*/
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

    curl_easy_cleanup(curl);
    if (fp) fclose(fp);
    return (res == CURLE_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
