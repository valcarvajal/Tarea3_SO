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
Pruebas con archivo grande:  
         ``` curl -4 -O http://localhost:8080/bigfile.bin ```   
         ``` ls -lh bigfile.bin ```   

