#!/usr/bin/env bash

# Script de pruebas completo y automatizado para prefork-WebServer
# en GNU/Linux.
# 1) Prueba HTTP CRUD en puerto 8080
# 2) Prueba de banners FTP, SSH, SMTP, DNS, TELNET y SNMP

set -euo pipefail

ROOT="$HOME/var/www"
BINARY="./prefork-WebServer"

# -------------------------------------------------------------------
# run_test <Nombre> <args...>
#   Lanza $BINARY con los args (por ejemplo "-n 2 -w $ROOT -p 2323 -T telnet"),
#   espera 0.5 s, ejecuta la prueba (HTTP o banner), y mata el servidor.
# -------------------------------------------------------------------
run_test() {
  local name="$1"; shift
  local args=("$@")

  echo -e "\n>>> Iniciando servidor [$name] '$BINARY ${args[*]}'"
  "$BINARY" "${args[@]}" > /tmp/server_${name}.log 2>&1 &
  local pid=$!

  # Espera a que el socket esté listo
  sleep 0.5

  echo ">>> Ejecutando prueba: $name"
  if [[ "$name" == "HTTP" ]]; then
    # GET
    echo "--- GET /index.html ---"
    curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"
    # HEAD
    echo "--- HEAD /index.html ---"
    curl -4 -s -I -w "HTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"
    # POST
    echo "--- POST /prueba.txt ---"
    rm -f "$ROOT/prueba.txt"
    curl -4 -s -X POST "http://localhost:8080/prueba.txt" \
         -H "Content-Type: text/plain" \
         --data-binary "Hola desde POST" \
         -w "\nHTTP_CODE: %{http_code}\n"
    echo "Contenido POST: $(cat "$ROOT/prueba.txt" 2>/dev/null || echo '(no existe)')"
    # PUT
    echo "--- PUT /prueba.txt ---"
    curl -4 -s -X PUT "http://localhost:8080/prueba.txt" \
         -H "Content-Type: text/plain" \
         --data-binary "Hola PUT!!!" \
         -w "\nHTTP_CODE: %{http_code}\n"
    echo "Contenido PUT: $(cat "$ROOT/prueba.txt" 2>/dev/null || echo '(no existe)')"
    # DELETE
    echo "--- DELETE /prueba.txt ---"
    curl -4 -s -X DELETE "http://localhost:8080/prueba.txt" \
         -w "\nHTTP_CODE: %{http_code}\n"
    echo "Existe tras DELETE? $(if [[ -f "$ROOT/prueba.txt" ]]; then echo 'SI'; else echo 'NO'; fi)"
    # 404
    echo "--- GET /no_existe.html ---"
    curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/no_existe.html"
    # 405
    echo "--- OPTIONS /index.html ---"
    curl -4 -s -X OPTIONS -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"

  else
    # Banner de protocolo alternativo
    # Sacamos el puerto de la opción -p
    local port=""
    for ((i=0; i<${#args[@]}; i++)); do
      if [[ "${args[i]}" == "-p" ]]; then
        port="${args[i+1]}"
        break
      fi
    done
    echo "Banner $name en puerto $port:"
    echo | nc -w2 localhost "$port" || true
  fi

  echo ">>> Deteniendo servidor [$name] (PID $pid)"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

# 1) Pruebas HTTP
echo -e "\n========== Pruebas HTTP =========="
run_test HTTP -n 4 -w "$ROOT" -p 8080 -T http

# 2) Pruebas de protocolos alternativos
declare -A PORTS=(
  [ftp]=2121
  [ssh]=2222
  [smtp]=2525
  [dns]=5353
  [telnet]=2323
  [snmp]=9161
)
for proto in "${!PORTS[@]}"; do
  echo -e "\n========== Prueba $proto =========="
  run_test "$proto" -n 2 -w "$ROOT" -p "${PORTS[$proto]}" -T "$proto"
done

echo -e "\nTodas las pruebas completadas."

