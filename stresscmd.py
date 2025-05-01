#!/usr/bin/env python3
## @file stresscmd.py
#  @brief **StressCMD** — Herramienta de *Denial-of-Service* (DoS) que dispara
#         un cliente HTTP (escrito en C) en paralelo hasta que el primer fallo
#         es detectado.
#
#  Este script:
#  - Lanza *n* hilos que ejecutan de forma reiterada un comando externo
#    (típicamente un **HTTP client** compilado en C).
#  - Detiene toda la prueba en cuanto un hilo detecta la primera anomalía
#    (código de retorno distinto de 0 o excepción en la invocación).
#  - Resume las estadísticas de peticiones exitosas, fallidas y excepciones.
#
#  Se documenta en formato **Doxygen** para Python, empleando anotaciones
#  `@param`, `@return`, `@note`, etc.  Esto facilita la generación de
#  documentación HTML/LaTeX con `doxygen Doxyfile`.
#
#  @author  (tu nombre aquí)
#  @date    30 abr 2025
#
#  @section requisitos Requisitos
#  - Python ≥ 3.8
#  - Cliente HTTP binario accesible en `$PATH` o ruta absoluta.
#
#  Uso:
#  @code{.bash}
#    $ ./stresscmd.py -n 8 -- ./HTTPclient -h 127.0.0.1:8080 GET /index.html
#  @endcode
#
#  @section lic Licencia
#  MIT License — libre uso con atribución.
"""
StressCMD: herramienta de DoS para tus WebServers usando el cliente HTTP en C,
ahora detiene el ataque en el primer fallo y reporta el error.
"""

import argparse
import threading
import subprocess
import sys
import time

# ======================================================================
# Variables globales de conteo (protegidas por `lock`)
# ======================================================================

total: int = 0           ## @var total  Número total de ejecuciones realizadas
ok:    int = 0           ## @var ok     Ejecuciones con retorno 0 (éxito)
err:   int = 0           ## @var err    Ejecuciones con retorno ≠ 0 (error)
exc:   int = 0           ## @var exc    Ejecuciones abortadas por excepción

# Datos del primer fallo detectado (se utilizan para el informe final)
first_error_code: int | None = None    ## @var first_error_code  Código de retorno del primer error (si aplica)
first_error_type: str | None  = None   ## @var first_error_type  Etiqueta del tipo de fallo (RETURN_CODE_xxx / EXCEPTION_yyy)

# Sincronización entre hilos
lock: threading.Lock      = threading.Lock()      ## @var lock  Exclusión mutua para modificar contadores
stop_event: threading.Event = threading.Event()   ## @var stop_event  Señal global para detener a los hilos cuando se detecta un fallo

# ======================================================================
# Función worker
# ======================================================================

def worker(cmd: list[str], thread_id: int) -> None:
    """Hilo que invoca al cliente HTTP en bucle.

    @param cmd       Lista de strings con el ejecutable y sus argumentos,
                     tal cual será pasada a `subprocess.run`.
    @param thread_id Identificador del hilo (solo para propósitos de log).
    @post
        - Incrementa los contadores globales (`total`, `ok`, `err`, `exc`)
          de forma atómica.
        - Si detecta el primer error o excepción, almacena la información
          en `first_error_code`/`first_error_type` y activa `stop_event`
          para detener el resto de hilos.
    """
    global total, ok, err, exc, first_error_code, first_error_type

    while not stop_event.is_set():
        try:
            # Ejecuta el comando, descartando la salida estándar y de error
            result = subprocess.run(cmd,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)

            with lock:
                total += 1
                if result.returncode == 0:
                    ok += 1
                else:
                    err += 1

                    # Primer fallo detectado: detenemos la prueba
                    if first_error_code is None:
                        first_error_code = result.returncode
                        first_error_type = f"RETURN_CODE_{result.returncode}"
                        stop_event.set()

        except Exception as e:  # pylint: disable=broad-except
            # Captura de excepciones inesperadas (p. ej. fallo al lanzar el binario)
            with lock:
                total += 1
                exc += 1
                if first_error_code is None:
                    first_error_code = None
                    first_error_type = f"EXCEPTION_{type(e).__name__}"
                    stop_event.set()

            # El mensaje se emite **fuera** del bloqueo para no retener el lock
            print(f"[Thread {thread_id}] Excepción al ejecutar cliente: {e}",
                  file=sys.stderr)

# ======================================================================
# Función main
# ======================================================================

def main() -> None:
    """Punto de entrada del programa.

    - Analiza argumentos de línea de comandos.
    - Lanza los hilos *worker*.
    - Espera la señal de parada (`stop_event`) o `Ctrl+C`.
    - Genera un informe resumido al finalizar.

    @retval None  El programa finaliza con código de retorno 0 en cualquier
                  circunstancia; los errores detectados corresponden **al
                  cliente** HTTP, no a StressCMD.
    """
    parser = argparse.ArgumentParser(
        description=("StressCMD: lanza múltiples instancias del cliente "
                     "HTTP para DoS.")
    )
    parser.add_argument(
        "-n", "--threads",
        type=int,
        required=True,
        help="Número de hilos que correrán el ataque"
    )

    # `parse_known_args()` permite pasar parámetros adicionales al ejecutable
    # que invocaremos tras el separador `--`.
    args, remainder = parser.parse_known_args()

    if not remainder or remainder[0] != "--":
        parser.error("Debes usar ' -- ' y luego el ejecutable con sus parámetros")

    cmd: list[str] = remainder[1:]
    if not cmd:
        parser.error("No se indicó ningún ejecutable tras '--'")

    # ------------------------------------------------------------------
    # Lanzamiento de la prueba
    # ------------------------------------------------------------------
    print(f"Iniciando ataque con {args.threads} hilos contra: {' '.join(cmd)}")
    start_time = time.time()

    threads: list[threading.Thread] = []
    for i in range(args.threads):
        t = threading.Thread(target=worker, args=(cmd, i), daemon=True)
        t.start()
        threads.append(t)

    # ------------------------------------------------------------------
    # Bucle principal: espera a que ocurra un fallo o a Ctrl+C
    # ------------------------------------------------------------------
    try:
        while not stop_event.is_set():
            time.sleep(0.1)
    except KeyboardInterrupt:
        # Interrupción manual del usuario
        stop_event.set()

    # ------------------------------------------------------------------
    # Fase de cierre y reporte final
    # ------------------------------------------------------------------
    finally:
        # Nos aseguramos de que todos los hilos terminen correctamente
        for t in threads:
            t.join()

        elapsed: float = time.time() - start_time

        # Informe principal
        if first_error_type:
            print(f"\n[✔] Ataque exitoso: servidor respondió con {first_error_type}")
        else:
            print("\n[!] Ataque detenido por el usuario antes de detectar un fallo.")

        # Resumen de métricas
        print("\n=== Resumen de resultados ===")
        print(f"Total       : {total}")
        print(f"  ✔ OK      : {ok}")
        print(f"  ✖ ERROR   : {err}")
        print(f"  ⚠ EXCEPT. : {exc}")
        print(f"Tiempo total: {elapsed:.2f} s")
        print("=============================")

# ======================================================================
# Ejecución como script
# ======================================================================

if __name__ == "__main__":
    main()
