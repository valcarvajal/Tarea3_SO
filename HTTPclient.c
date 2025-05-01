//
// Created by ajrf on 30/04/25.
//

/**
 * @file HTTPclient.c
 * @brief Cliente HTTP CLI en C usando libcurl.
 *
 * Este programa permite interactuar con un servidor HTTP/1.1:
 * - GET, POST, PUT, HEAD, DELETE
 * - Descarga binarios a un fichero con -o <archivo>
 * - Permite enviar datos en POST/PUT con -d "<datos>"
 *
 * Uso:
 *   HTTPclient -h <host:puerto> [ -o <fichero> ]
 *              <METHOD> <recurso> [ -d "<datos>" ]
 *
 * Ejemplos:
 *   HTTPclient -h localhost:8080 GET /index.html
 *   HTTPclient -h localhost:8080 GET /ejecutable.bin -o salida.bin
 *   HTTPclient -h localhost:8080 POST /nuevo.txt -d "Hola mundo"
 *   HTTPclient -h localhost:8080 PUT /upload.txt -d "Contenido"
 *   HTTPclient -h localhost:8080 DELETE /viejo.txt
 */

#include <stddef.h>      // para size_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <curl/curl.h>

/** Estructura para acumular la respuesta en memoria */
struct Memory {
    char *data;
    size_t size;
};

/** Escribir datos en memoria */
static size_t write_memory_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;
    char *ptr_new = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr_new) return 0;
    mem->data = ptr_new;
    memcpy(&(mem->data[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

/** Escribir datos directamente en fichero */
static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

int main(int argc, char **argv) {
    char *host = NULL, *method = NULL, *resource = NULL, *data = NULL, *outfile = NULL;
    FILE *fp = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "h:o:d:")) != -1) {
        switch (opt) {
            case 'h': host    = strdup(optarg); break;
            case 'o': outfile = strdup(optarg); break;
            case 'd': data    = strdup(optarg); break;
            default:
                fprintf(stderr, "Uso: %s -h <host:puerto> [-o <fichero>] <METHOD> <recurso> [-d \"datos\"]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }
    if (!host || optind + 1 >= argc) {
        fprintf(stderr, "Error: se requiere -h <host> y METHOD + recurso\n");
        return EXIT_FAILURE;
    }
    method   = argv[optind++];
    resource = argv[optind++];

    /* Construir URL */
    char url[1024];
    if (strncmp(host, "http://", 7) && strncmp(host, "https://", 8))
        snprintf(url, sizeof(url), "http://%s%s", host, resource);
    else
        snprintf(url, sizeof(url), "%s%s", host, resource);

    CURL *curl = curl_easy_init();
    if (!curl) return EXIT_FAILURE;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    /* Para imprimir cabeceras junto con el cuerpo en la salida por pantalla */
    curl_easy_setopt(curl, CURLOPT_HEADER, 1L);

    /* Preparar cuerpo y longitud si hace falta */
    size_t data_len = data ? strlen(data) : 0;

    /* Selección de método */
    if (strcasecmp(method, "GET") == 0) {
        // por defecto es GET
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

    /* Decidir salida: fichero o memoria */
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

    /* Ejecutar para la rama de fichero */
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

    curl_easy_cleanup(curl);
    if (fp) fclose(fp);
    return (res == CURLE_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
