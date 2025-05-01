#!/usr/bin/env bash
#
# test_httpclient.sh
# Script de pruebas para HTTPclient: GET, HEAD, POST, PUT, DELETE y descarga binaria.
#

set -euo pipefail

HTTPCLIENT="./HTTPclient"
HOST="localhost:8080"

echo "### 1. Prueba GET de un recurso de texto"
$HTTPCLIENT -h $HOST GET /index.html

echo -e "\n### 2. Prueba HEAD (solo cabeceras)"
$HTTPCLIENT -h $HOST HEAD /index.html

echo -e "\n### 3. Prueba descarga binaria (GET con -o)"
OUT_BIN="downloaded.bin"
echo "Descargando /imagen.png a ./$OUT_BIN"
$HTTPCLIENT -h $HOST GET /imagen.png -o $OUT_BIN
echo "Tamaño de $OUT_BIN: $(stat -c%s "$OUT_BIN") bytes"

echo -e "\n### 4. Prueba POST (crea un nuevo archivo)"
echo "Haciendo POST a /test-post.txt"
$HTTPCLIENT -h $HOST POST /client_test.txt -d "Contenido POST de prueba"
echo "Verificando creación con GET..."
$HTTPCLIENT -h $HOST GET /client_test.txt

echo -e "\n### 5. Prueba PUT (sube/reemplaza un archivo)"
echo "Haciendo PUT a /test-put.txt"
$HTTPCLIENT -h $HOST PUT /client_test.txt -d "Contenido PUT de prueba"
echo "Verificando con GET..."
$HTTPCLIENT -h $HOST GET /client_test.txt

echo -e "\n### 6. Prueba DELETE (borra un archivo existente)"
echo "Borrando /test-put.txt"
$HTTPCLIENT -h $HOST DELETE /client_test.txt
echo "Verificando borrado con GET (debe dar 404)"
set +e
$HTTPCLIENT -h $HOST GET /test-put.txt
EXIT_CODE=$?
set -e
if [ $EXIT_CODE -ne 404 ]; then
  echo "✅ DELETE funcionó correctamente (exit code $EXIT_CODE)"
else
  echo "❌ DELETE falló"
  exit 1
fi

echo -e "\n### 7. Prueba DELETE en recurso no existente (debe 404)"
set +e
$HTTPCLIENT -h $HOST DELETE /no-existe.txt
DEL_CODE=$?
set -e
if [ $DEL_CODE -ne 404 ]; then
  echo "✅ DELETE dio error esperado en recurso inexistente (exit code $DEL_CODE)"
else
  echo "❌ DELETE no reportó error en inexistente"
  exit 1
fi

echo -e "\nTodas las pruebas completadas con éxito."

