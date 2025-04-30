#!/usr/bin/env bash

BASE="http://localhost:8080"
ROOT="$HOME/var/www"

echo "Comprobando servidor..."
if ! curl -4 -s -o /dev/null -w "%{http_code}" "$BASE/index.html" | grep -q '^200$'; then
  echo "ERROR: servidor no responde. Arráncalo en otro terminal."
  exit 1
fi
echo "Servidor OK ✅"
echo

echo "=== GET /index.html ==="
curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "$BASE/index.html"
echo

echo "=== HEAD /index.html ==="
curl -4 -s -I -w "HTTP_CODE: %{http_code}\n" "$BASE/index.html"
echo

echo "=== POST /prueba.txt (limpiando fichero previo) ==="
rm -f "$ROOT/prueba.txt"
curl -4 -s -X POST "$BASE/prueba.txt" \
     -H "Content-Type: text/plain" \
     --data-binary "Hola desde POST" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Contenido en disco tras POST:"
cat "$ROOT/prueba.txt" 2>/dev/null || echo "(no existe)"
echo

echo "=== PUT /prueba.txt ==="
curl -4 -s -X PUT "$BASE/prueba.txt" \
     -H "Content-Type: text/plain" \
     --data-binary "Hola PUT!!!" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Contenido en disco tras PUT:"
cat "$ROOT/prueba.txt" 2>/dev/null || echo "(no existe)"
echo

echo "=== DELETE /prueba.txt ==="
curl -4 -s -X DELETE "$BASE/prueba.txt" \
     -w "\nHTTP_CODE: %{http_code}\n"
echo "-> Comprobación de borrado:"
ls "$ROOT/prueba.txt" && echo "¡Sigue existiendo!" || echo "El archivo fue eliminado correctamente"
echo

echo "=== 404 Not Found ==="
curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "$BASE/no_existe.html"
echo

echo "=== 405 Method Not Allowed (OPTIONS) ==="
curl -4 -s -X OPTIONS -w "\nHTTP_CODE: %{http_code}\n" "$BASE/index.html"

