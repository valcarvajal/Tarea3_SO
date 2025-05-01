#!/usr/bin/env bash

# Script de pruebas completo y automatizado para prethread-WebServer
# en GNU/Linux.
# 1) Prueba HTTP CRUD en puerto 8080
# 2) Prueba de banners FTP, SSH, SMTP, DNS, TELNET y SNMP

set -euo pipefail

ROOT="$HOME/var/www"
BINARY="./prethread-WebServer"

# Función para lanzar servidor, esperar readiness, ejecutar prueba y detener
run_test() {
  local name="$1"; shift
  local args=("$@")
  echo -e "\n>>> Iniciando servidor [$name] '$BINARY ${args[*]}'"
  "$BINARY" "${args[@]}" > /tmp/server_${name}.log 2>&1 &
  local pid=$!
  sleep 0.2  # tiempo para bind

  echo ">>> Ejecutando prueba: $name"
  case "$name" in
    HTTP)
      echo "--- GET /index.html ---"
      curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"
      echo "--- HEAD /index.html ---"
      curl -4 -s -I -w "HTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"
      echo "--- POST /prueba.txt ---"
      rm -f "$ROOT/prueba.txt"
      curl -4 -s -X POST "http://localhost:8080/prueba.txt" \
           -H "Content-Type: text/plain" \
           --data-binary "Hola desde POST" \
           -w "\nHTTP_CODE: %{http_code}\n"
      echo "Contenido POST: $(cat "$ROOT/prueba.txt" 2>/dev/null || echo '(no existe)')"
      echo "--- PUT /prueba.txt ---"
      curl -4 -s -X PUT "http://localhost:8080/prueba.txt" \
           -H "Content-Type: text/plain" \
           --data-binary "Hola PUT!!!" \
           -w "\nHTTP_CODE: %{http_code}\n"
      echo "Contenido PUT: $(cat "$ROOT/prueba.txt" 2>/dev/null || echo '(no existe)')"
      echo "--- DELETE /prueba.txt ---"
      curl -4 -s -X DELETE "http://localhost:8080/prueba.txt" \
           -w "\nHTTP_CODE: %{http_code}\n"
      echo "Existe tras DELETE? $(if [[ -f "$ROOT/prueba.txt" ]]; then echo 'SI'; else echo 'NO'; fi)"
      echo "--- GET /no_existe.html ---"
      curl -4 -s -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/no_existe.html"
      echo "--- OPTIONS /index.html ---"
      curl -4 -s -X OPTIONS -w "\nHTTP_CODE: %{http_code}\n" "http://localhost:8080/index.html"
      ;;
    *)
      # Extraer el puerto tras '-p'
      local port=""
      for ((i=0; i<${#args[@]}; i++)); do
        if [[ "${args[i]}" == "-p" ]]; then
          port="${args[i+1]}"; break
        fi
      done
      echo "Banner $name en puerto $port:"
      echo | nc -w2 localhost "$port" || true
      ;;
  esac

  echo ">>> Deteniendo servidor [$name] (PID $pid)"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

# Ejecutar pruebas HTTP
echo -e "\n========== Pruebas HTTP =========="
run_test HTTP -n 4 -w "$ROOT" -p 8080 -T http

# Ejecutar pruebas de protocolos alternativos
declare -A PORTS=( [ftp]=2121 [ssh]=2222 [smtp]=2525 [dns]=5353 [telnet]=2323 [snmp]=9161 )
for proto in "${!PORTS[@]}"; do
  echo -e "\n========== Prueba $proto =========="
  run_test "$proto" -n 2 -w "$ROOT" -p ${PORTS[$proto]} -T $proto
done

echo -e "\nTodas las pruebas completadas."

