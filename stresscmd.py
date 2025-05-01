#!/usr/bin/env python3
"""
StressCMD: herramienta de Denegación de Servicio para WebServer.

Crea múltiples hilos que invocan un ejecutable (p.ej. HTTPclient) con los parámetros
que especifiques, para simular un ataque DoS.

Uso:
  python3 StressCMD.py -n <num_hilos> -- <ruta_ejecutable> [args_del_ejecutable]

Ejemplo:
  ./StressCMD.py -n 50 -- ./HTTPclient -h localhost:8080 GET /index.html
"""

import argparse
import subprocess
import threading
import time

# Estructura para contar resultados
results = {
    "OK": 0,
    "ERROR": 0,
    "EXCEPTION": 0
}
results_lock = threading.Lock()

def worker(cmd, idx):
    """
    Ejecuta el comando dado y registra si tuvo éxito (exit code 0)
    o falló.
    """
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        status = "OK" if res.returncode == 0 else "ERROR"
    except Exception:
        status = "EXCEPTION"

    with results_lock:
        results[status] += 1

    print(f"[Hilo {idx:03d}] {status} (exit_code={getattr(res, 'returncode', 'N/A')})")

def main():
    parser = argparse.ArgumentParser(description="StressCMD: stress test para HTTPclient")
    parser.add_argument("-n", "--threads", type=int, required=True,
                        help="Número de hilos a lanzar")
    parser.add_argument("executable", help="Ruta al ejecutable (p.ej. ./HTTPclient)")
    parser.add_argument("exec_args", nargs=argparse.REMAINDER,
                        help="Argumentos a pasar al ejecutable")
    args = parser.parse_args()

    if not args.exec_args:
        parser.error("Debes especificar al menos un argumento para el ejecutable")

    # Construir la línea de comando base (lista)
    base_cmd = [args.executable] + args.exec_args

    print(f"Lanzando {args.threads} hilos:")
    start = time.time()

    threads = []
    for i in range(args.threads):
        t = threading.Thread(target=worker, args=(base_cmd, i+1), daemon=True)
        threads.append(t)
        t.start()

    # Esperar a que terminen todos
    for t in threads:
        t.join()

    elapsed = time.time() - start
    total = args.threads
    ok = results["OK"]
    err = results["ERROR"]
    exc = results["EXCEPTION"]

    print("\n=== Resumen de resultados ===")
    print(f"Total hilos : {total}")
    print(f"  ✔ OK      : {ok}")
    print(f"  ✖ ERROR   : {err}")
    print(f"  ⚠ EXCEPT. : {exc}")
    print(f"Tiempo total: {elapsed:.2f} s")
    print("=============================")

if __name__ == "__main__":
    main()
