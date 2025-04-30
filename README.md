# Tarea3_SO

Comandos:
Pre-threaded
Compilar: gcc prethread-WebServer.c -o prethread-WebServer -lpthread
Iniciar Servidor: ./prethread-WebServer -n 4 -w ~/var/www -p 8080
Pruebas:
        chmod +x ~/test_webserver.sh
        ~/test_webserver.sh

Pre-forked
Compilar: gcc prefork-WebServer.c -o prefork-WebServer
Iniciar Servidor: ./prefork-WebServer -n 4 -w ~/var/www -p 8080
Pruebas:
        chmod +x test_prefork_webserver.sh
        ./test_prefork_webserver.sh

