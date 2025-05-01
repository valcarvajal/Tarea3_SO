# Tarea3_SO

## Comandos:

### Pre-threaded

Compilar:   
``` gcc prethread-WebServer.c -o prethread-WebServer -lpthread ```   

Crear carpeta www:   
``` mkdir -p ~/var/www ```   
``` echo "<h1>Servidor Pre-threaded</h1>" > ~/var/www/index.html ```   

Iniciar Servidor:   
``` ./prethread-WebServer -n 4 -w ~/var/www -p 8080 ```   
                  
Pruebas:   
``` chmod +x test_webserver.sh ```   
``` ./test_webserver.sh ```   
         
Pruebas protocolos:   
``` chmod +x test_all_protocols.sh ```   
``` ./test_all_protocols.sh ```   
  
### Pre-forked

Compilar:   
``` gcc prefork-WebServer.c -o prefork-WebServer ```   

Crear carpeta www:  
``` mkdir -p ~/var/www ```   
``` echo "<h1>Servidor Pre-forked</h1>" > ~/var/www/index.html ```   

Iniciar Servidor:   
``` ./prefork-WebServer -n 4 -w ~/var/www -p 8080 ```   

Pruebas:  
``` chmod +x test_prefork_webserver.sh ```   
``` ./test_prefork_webserver.sh ```   

Pruebas protocolos:  
``` chmod +x test_all_protocols_prefork.sh ```   
``` ./test_all_protocols_prefork.sh ```   

### Para ambos Web Servers

Pruebas con archivos grandes:   

Crea el archivo:   
```dd if=/dev/urandom of=bigfile.bin bs=1M count=100```   

Lo agrega al root:   
``` mv bigfile.bin ~/var/www/ ```   

Se arranca el server y luego:   
``` curl -4 -O http://localhost:8080/bigfile.bin ```   
``` ls -lh bigfile.bin ```   

### HTTPclient

Instalaciones (para usar curl en C según lo solicita el enunciado de la tarea):   
``` sudo apt install libcurl4-openssl-dev ```   

Compilar:   
``` gcc HTTPclient.c -o HTTPclient -lcurl -Wall -Wextra -pedantic ```   

Ejecutar:   
``` ./HTTPclient -h localhost:8080 GET /index.html ```   
``` ./HTTPclient -h localhost:8080 POST /client_test.txt -d "Nuevo POST" ```   
``` ./HTTPclient -h localhost:8080 PUT/client_test.txt -d "Nuevo PUT" ```   
``` ./HTTPclient -h localhost:8080 DELETE /client_test.txt ```   

Mover imagen.png a carpeta www:   
``` cp imagen.png ~/var/www ```   

Pruebas:   
``` chmod +x ./test_httpclient.sh ```   
``` ./test_httpclient.sh ```   



