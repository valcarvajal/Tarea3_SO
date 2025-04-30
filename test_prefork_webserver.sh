#!/usr/bin/env bash

# Script de pruebas para prefork-WebServer (HTTP/1.1)
# Asegúrate de tener prefork-WebServer corriendo en otro terminal:
# ./prefork-WebServer -n 4 -w ~/var/www -p 8080

BASE="http://localhost:8080"
ROOT="$HOME/var/www"

# 1. Verificar servidor vivo
echo "Comprobando servidor..."
if ! curl -4 -s -o /dev/null -w "%{http_code}" "$BASE/index.html" | grep -q '^200$'; then
  echo "ERROR: el servidor no está respondiendo. Arráncalo primero en otro terminal."
  exit 1
fi
echo "Servidor OK ✅"
echo

# 2. GET /index.html
echo "=== GET /index.html ==="
curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "$BASE/index.html"
echo

# 3. HEAD /index.html
echo "=== HEAD /index.html ==="
curl -4 -s -I -w "HTTP_CODE: %{http_code}\n" "$BASE/index.html"
echo

# 4. POST /prueba.txt
echo "=== POST /prueba.txt (limpiando fichero previo) ==="
rm -f "$ROOT/prueba.txt"
curl -4 -s -X POST "$BASE/prueba.txt" \
     -H "Content-Type: text/plain" \
     --data-binary "Hola desde POST" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Contenido en disco tras POST:"
cat "$ROOT/prueba.txt" 2>/dev/null || echo "(no existe)"
echo

# 5. PUT /prueba.txt
echo "=== PUT /prueba.txt ==="
curl -4 -s -X PUT "$BASE/prueba.txt" \
     -H "Content-Type: text/plain" \
     --data-binary "Hola PUT!!!" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Contenido en disco tras PUT:"
cat "$ROOT/prueba.txt" 2>/dev/null || echo "(no existe)"
echo

# 6. DELETE /prueba.txt
echo "=== DELETE /prueba.txt ==="
curl -4 -s -X DELETE "$BASE/prueba.txt" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Comprobación de borrado:"
ls "$ROOT/prueba.txt" && echo "¡Sigue existiendo!" || echo "El archivo fue eliminado correctamente"
echo

# 7. 404 Not Found
echo "=== 404 Not Found ==="
curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "$BASE/no_existe.html"
echo

# 8. 405 Method Not Allowed (OPTIONS)
echo "=== 405 Method Not Allowed (OPTIONS) ==="
curl -4 -s -X OPTIONS -w "\nHTTP_CODE: %{http_code}\n" "$BASE/index.html"
echo

