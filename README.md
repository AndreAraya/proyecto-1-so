# Proyecto 1: compresor Huffman

Este es el paquete único de trabajo. Contiene las fuentes C, el Makefile, el
script para obtener el corpus y la plantilla LaTeX en `informe/`.

**Estado:** las variantes serial, fork y pthread compilaron y pasaron pruebas
en Debian 13. La interfaz GTK 4 compiló y ejecutó las tres variantes sobre el
corpus con resultado «Correcto». Se agregó después la columna de salud MD5;
esa modificación requiere otra comprobación visual. El PDF final, el enlace
público al código fuente y la prueba de instalación integral siguen pendientes.

## Estructura

- `src/serial/serial.c`, `src/fork/fork.c`, `src/pthread/pthread.c`: programas.
- `src/gui/gui.c`: interfaz GTK 4. Selecciona los archivos originales, ejecuta
  compresión y descompresión con cada variante y coteja las salidas byte a byte.
- `scripts/descargar_corpus.sh`: descarga la clasificación vigente al ejecutarlo.
- `informe/`: fuentes LaTeX y figuras de la plantilla.
- `evidencia/resultados-observados.md`: pruebas previas, conservadas como notas.
- `Makefile`: compila los cuatro ejecutables con `make`. Necesita GTK 4 de
  desarrollo (`libgtk-4-dev`), OpenSSL de desarrollo y `pkg-config`.

La interfaz se inicia con `./bin/gui` desde una sesión GNOME de usuario normal.
La carpeta de resultados `comparacion-XXXXXX` se crea dentro del proyecto.
Las columnas «Mejora C / D» muestran `100 × (1 - tiempo_variante / tiempo_serial)`
para compresión y descompresión. «Razón original/HUF» es el cociente de bytes
originales entre bytes del contenedor. «Salud MD5» muestra los archivos
restaurados después de superar la verificación MD5 interna entre todos los
archivos originales. Cada medición corresponde a una sola
ejecución y también incluye el tiempo de iniciar el proceso correspondiente.

El corpus original de 100 archivos, sus sumas SHA-256, el HTML consultado y la
lista de IDs están en la máquina de desarrollo y aún no están incluidos en el
paquete. Una nueva ejecución del script puede descargar otra selección de
libros. Las carpetas `corpus/` y `resultados/` se generan al ejecutar.
