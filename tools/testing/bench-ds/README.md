# Benchmark de Estructuras de Datos del Kernel Linux en Espacio de Usuario

Mide y compara el rendimiento de **maple tree**, **red-black tree** y **lista
doblemente enlazada** usando exactamente las implementaciones que viven en
`lib/` del kernel, compiladas y ejecutadas en userspace sin modificar ni una
línea de su código fuente.

---

## ¿Qué problema resolvemos?

El código de `lib/maple_tree.c` o `lib/rbtree.c` llama a funciones que solo
existen en el kernel: `kmem_cache_alloc`, `spin_lock`, `call_rcu`, `BUG_ON`,
`EXPORT_SYMBOL`, etc.  En userspace esas funciones no existen, por lo que el
código no compilaría directamente.

La solución es proveer **implementaciones alternativas** (mocks) de cada una
de esas dependencias usando primitivas estándar de C y POSIX.  El compilador
encuentra los mocks *antes* que los headers reales del kernel gracias al orden
de los flags `-I`.  El código del kernel nunca se entera del cambio.

---

## Mapa completo de dependencias y sus mocks

| Dependencia del kernel | ¿Qué hace en el kernel? | Mock en userspace | Archivo |
|---|---|---|---|
| `kmem_cache` / `kmem_cache_alloc` | Slab allocator, cache de objetos | `malloc` / `posix_memalign` + `pthread_mutex_t` | `tools/testing/shared/linux.c` |
| `kmem_cache_alloc_bulk` | Asignación en bulk | Loop de `malloc` con mutex | `tools/testing/shared/linux.c` |
| `spinlock_t` / `spin_lock` | Spinlock de bajo nivel | `pthread_mutex_t` / `pthread_mutex_lock` | `tools/include/linux/spinlock.h` |
| `rcu_dereference` / `call_rcu` | Read-Copy-Update | `liburcu` (implementación userspace de RCU) | `tools/testing/shared/linux/rcupdate.h` |
| `synchronize_rcu` | Barrera de gracia RCU | `#define → noop` | `tools/include/linux/kernel.h` |
| `BUG_ON(x)` | Panic si condición es true | `assert(!(x))` | `tools/include/linux/kernel.h` |
| `WARN_ON(x)` | Imprime warning y sigue | `fprintf` + continúa | varios |
| `dump_stack()` | Imprime stack trace | `assert(0)` | `tools/testing/shared/shared.h` |
| `gfp_t` / `GFP_KERNEL` / `GFP_NOWAIT` | Flags de asignación de memoria | `typedef unsigned int`, flags ignorados | `tools/include/linux/gfp.h` |
| `WRITE_ONCE(x, v)` / `READ_ONCE(x)` | Accesos atómicos sin carrera | Asignación/lectura directa | `tools/include/linux/compiler.h` |
| `smp_wmb()` / `smp_rmb()` | Barreras de memoria SMP | Barreras del compilador o noop | `tools/include/asm/barrier.h` |
| `EXPORT_SYMBOL(sym)` | Exporta símbolo al módulo | `#define → vacío` | `tools/include/linux/export.h` |
| `module_init` / `MODULE_AUTHOR` | Metadatos de módulo | `#define → vacío` | `bench.h` |
| `atomic_t` / `atomic_inc` | Enteros atómicos | `int32_t` + `uatomic_inc` (liburcu) | `tools/testing/shared/linux/maple_tree.h` |
| `pr_info` / `pr_debug` | Logging del kernel | `printf` | `tools/include/linux/kernel.h` |
| `kmalloc` / `kfree` | Asignador genérico | `malloc` / `free` | `tools/include/linux/slab.h` |

---

## Cómo se resuelven los headers: el truco de los `-I`

El compilador busca headers **en orden** según los flags `-I`.  Los mocks
están en directorios que aparecen **antes** que `include/` del kernel:

```
Orden de búsqueda para #include <linux/foo.h>
──────────────────────────────────────────────────────────────────────────
 1. -I../shared          →  tools/testing/shared/linux/
                              maple_tree.h  (mock: define atomic_t, incluye el real)
                              rcupdate.h    (mock: usa liburcu)
                              xarray.h      (mock)

 2. -I../../include      →  tools/include/linux/
                              rbtree.h      (copia para tools, compatible)
                              rbtree_augmented.h (ídem)
                              spinlock.h    (mock: pthread_mutex)
                              gfp.h         (mock: typedef)
                              slab.h        (mock: kmalloc→malloc)
                              export.h      (mock: EXPORT_SYMBOL vacío)
                              kernel.h      (mock: BUG_ON→assert, etc.)

 3. -I../../../include   →  include/linux/   ← headers reales del kernel
                              (se usan como fallback para lo que no tiene mock)
──────────────────────────────────────────────────────────────────────────
```

Cuando `lib/maple_tree.c` hace `#include <linux/rcupdate.h>`, el compilador
encuentra **primero** `tools/testing/shared/linux/rcupdate.h` (el mock con
liburcu) y **nunca llega** al `include/linux/rcupdate.h` real del kernel.

### El caso especial de maple_tree.h

`tools/testing/shared/linux/maple_tree.h` no reimplementa nada: define los
tres macros que necesita (`atomic_t`, `atomic_inc`, `atomic_read`) y luego
incluye el **header real** del kernel con una ruta relativa:

```c
/* tools/testing/shared/linux/maple_tree.h */
#define atomic_t int32_t
#define atomic_inc(x) uatomic_inc(x)
#define atomic_read(x) uatomic_read(x)
#include "../../../../include/linux/maple_tree.h"   /* ← header real */
```

### ¿Por qué incluir el `.c` directamente en vez de compilarlo aparte?

```c
/* bench_main.c */
#include "../../../lib/maple_tree.c"
#include "../../../lib/rbtree.c"
```

Incluir el `.c` en lugar de compilarlo como unidad separada sirve para que
**todas las funciones `static` del kernel compilen con los mocks ya activos**.
Si compiláramos `lib/maple_tree.c` por separado sin los flags `-I` correctos,
el compilador encontraría los headers reales del kernel y fallaría.

Al incluirlo dentro de nuestra unidad de traducción, hereda exactamente el
mismo entorno de compilación: mismos `-I`, mismos `#define`, mismos mocks.

---

## Arquitectura del proyecto

```
tools/testing/bench-ds/
├── README.md          ← Este archivo
├── Makefile           ← Build system (reutiliza tools/testing/shared/)
├── bench.h            ← Punto de entrada del compat layer
└── bench_main.c       ← Benchmarks + include directo de los .c del kernel

tools/testing/shared/          ← Infraestructura compartida (NO modificar)
├── shared.mk                  ← Flags de compilación y reglas comunes
├── linux.c                    ← kmem_cache, slab allocator en userspace
├── linux/
│   ├── maple_tree.h           ← Mock: define atomic_t, incluye el real
│   ├── rcupdate.h             ← Mock: #include <urcu.h>
│   └── xarray.h               ← Mock
└── ...

tools/include/linux/           ← Headers del kernel adaptados para userspace
├── rbtree.h                   ← Copia compatible con tools
├── rbtree_augmented.h         ← Copia compatible con tools
├── list.h                     ← Copia compatible con tools
├── spinlock.h                 ← Mock: pthread_mutex_t
├── slab.h                     ← Mock: kmalloc → malloc
├── gfp.h                      ← Mock: GFP flags como unsigned int
└── ...

lib/                           ← Código fuente del kernel (NO modificar)
├── maple_tree.c               ← Incluido directamente en bench_main.c
└── rbtree.c                   ← Incluido directamente en bench_main.c

include/linux/                 ← Headers reales del kernel
├── maple_tree.h               ← Incluido por el mock de shared/linux/
├── list.h                     ← Fallback si tools/include no lo tiene
└── ...
```

---

## Qué mide el benchmark

Se corren **cuatro operaciones** sobre cada estructura con `n` elementos:

| Operación | Maple Tree | RB-Tree | Lista |
|---|---|---|---|
| **insert** | `mtree_insert` — O(log n) amortizado | `rb_insert_color` — O(log n) | `list_add_tail` — O(1) |
| **search** | `mtree_load` — O(log n) | Traversal manual — O(log n) | `list_for_each_entry` — O(n) |
| **iterate** | `mas_for_each` — O(n) | `rb_first`/`rb_next` — O(n) | `list_for_each_entry` — O(n) |
| **delete** | `mtree_erase` — O(log n) | `rb_erase` — O(log n) | `list_del` — O(1) |

> **Nota sobre list search**: buscar en una lista es O(n) por operación.
> Hacer `n` búsquedas sería O(n²), por eso se limita a 5000 operaciones y
> se muestra el tiempo por operación para que sea comparable.

### ¿Cuándo usar cada estructura?

- **Lista**: inserciones/eliminaciones O(1) con puntero directo. Búsqueda lenta. Ideal para colas, listas de tareas, LRU.
- **RB-Tree**: búsqueda e inserción O(log n). El kernel lo usa en el scheduler (`task_struct`), VMA pre-maple-tree, timers.
- **Maple Tree**: sucesor del RB-Tree para VMAs. Optimizado para rangos contiguos (ej: `0..999` se puede guardar en un solo nodo). Mejor cache locality que el rbtree para acceso secuencial.

---

## Compilar y ejecutar

### Dependencias

```bash
# Ubuntu/Debian
sudo apt-get install -y liburcu-dev

# Fedora/RHEL
sudo dnf install -y userspace-rcu-devel
```

### Build

```bash
cd tools/testing/bench-ds
make
```

### Ejecutar

```bash
# Con n = 100000 elementos (default)
./bench_ds

# Con n personalizado
./bench_ds 500000

# Con n pequeño para debugging (lista search no se limita)
./bench_ds 1000
```

### Limpiar

```bash
make clean
```

---

## Salida esperada

```
================================================================
  Kernel DS Benchmark — userspace   (n = 100000 elementos)
================================================================

[Maple Tree]
  maple_tree      insert     n=100000    total=    45123456 ns  451.23 ns/op
  maple_tree      search     n=100000    total=    23456789 ns  234.57 ns/op
  maple_tree      iterate    n=100000    total=     5678901 ns   56.79 ns/op
  maple_tree      delete     n=100000    total=    34567890 ns  345.68 ns/op

[Red-Black Tree]
  rbtree          insert     n=100000    total=    56789012 ns  567.89 ns/op
  rbtree          search     n=100000    total=    45678901 ns  456.79 ns/op
  rbtree          iterate    n=100000    total=     4567890 ns   45.68 ns/op
  rbtree          delete     n=100000    total=    23456789 ns  234.57 ns/op

[Linked List]
  list            insert     n=100000    total=     2345678 ns   23.46 ns/op
  list            search*    n=5000      total=   234567890 ns 46913.58 ns/op
  list            iterate    n=100000    total=     1234567 ns   12.35 ns/op
  list            delete     n=100000    total=     1234567 ns   12.35 ns/op

* list search limitado a 5000 ops (O(n) por op — O(n²) total)
```

---

## Generar las gráficas

```bash
# Ejecutar todos los escenarios (1k, 10k, 32k, 100k, 500k) en seq y random
./run_benchmarks.sh

# Generar todas las gráficas (requiere uv)
uv run plot_results.py

# O la versión simple (tiempo vs N, una página)
uv run plot_simple.py
```

Las imágenes se guardan en `results/plots/`.

---

## Resultados y análisis de las gráficas

Todos los resultados que se citan a continuación son medidos en la misma
máquina con la misma semilla aleatoria (`seed=42`).  El eje Y siempre está en
**ns por operación** (no tiempo total), para que los puntos sean comparables
entre tamaños de N distintos.

---

### `01_comparacion_ops.png` — Comparación de operaciones (n = 100 000)

**Qué prueba.** Las cinco operaciones (insert, search, search\*, iterate, delete)
de las tres estructuras a escala media, con el mismo N en ambos modos lado a
lado.  La escala del eje Y es logarítmica porque los valores abarcan tres
órdenes de magnitud (de ~2 ns a ~10 000 ns).

**Qué se ve.**

| Operación | Ganador | Razón |
|-----------|---------|-------|
| insert | Lista (~18 ns) | `list_add_tail` es O(1), sin rebalanceo |
| search | RB-Tree (~89 ns) | O(log n) con árbol compacto y cache-friendly |
| search\* | Lista: peor (~10 000 ns) | O(n) por operación — recorre media lista en promedio |
| iterate | Lista (~3 ns) | Acceso lineal en memoria, prefetcher del CPU aprovecha la localidad |
| delete | Lista (~25 ns) | `list_del` es O(1) con puntero directo al nodo |

**Por qué el Maple Tree pierde en insert y delete.** Con claves secuenciales
el Maple Tree intenta comprimir rangos contiguos en un solo nodo B-tree ("dense
nodes").  Esa compresión tiene coste: ~1500–2000 ns/op, frente a los ~290 ns
del RB-Tree.  El beneficio aparece en search e iterate, donde la mayor
localidad de caché de los nodos B reduce los saltos de puntero.

**Por qué los dos paneles (seq / random) se parecen tanto a n=100 000.** A
esta escala el Maple Tree ya no obtiene ventaja adicional del orden secuencial
porque los nodos empiezan a superar la caché L2.  Las diferencias grandes entre
modos se ven mejor en las gráficas de escalabilidad y en el peor caso.

---

### `02_escalabilidad_sequential.png` y `02_escalabilidad_random.png` — ns/op vs N

**Qué prueba.** Cómo evoluciona el coste por operación al crecer N desde 1 000
hasta 500 000, para cada combinación de estructura y operación.  Permite
distinguir O(1), O(log n) y O(n) visualmente: una línea plana es O(1), una
línea que sube lentamente es O(log n), y una línea que sube en diagonal es O(n).

**Observaciones clave — modo secuencial.**

- **List search** es la única línea que sube con pendiente pronunciada: pasa de
  ~1700 ns/op en n=1000 a ~9400 ns/op en n=500 000.  Es O(n) puro — cada
  búsqueda recorre de media la mitad de la lista.
- **Maple Tree insert** *baja* al crecer N (~2500 ns en n=1000 → ~1600 ns en
  n=500 000).  Contra-intuitivo: con más claves secuenciales el árbol puede
  empaquetar más rangos en cada nodo, amortizando el coste de inserción.
- **RB-Tree** en todas las operaciones es casi plano — O(log n) con constante
  pequeña y comportamiento muy predecible.
- **Iterate** en modo secuencial: la Lista es la más rápida (~2–3 ns/op) porque
  es un puntero tras otro en memoria contigua.  El RB-Tree cuesta ~8–12 ns
  (un salto de puntero por nodo pero con árbol balanceado y caliente en caché).

**Observaciones clave — modo random.**

- **RB-Tree iterate** pasa de ~14 ns/op en n=1000 a ~100 ns/op en n=500 000.
  Es el crecimiento más llamativo: el árbol fue construido con claves
  aleatorias, así que la iteración in-order (`rb_first`/`rb_next`) salta entre
  nodos dispersos en memoria → muchos cache misses.  En modo secuencial ese
  mismo iterate cuesta solo ~8 ns/op porque el árbol quedó más compacto.
- **Maple Tree insert** es plano en ~4000 ns/op independientemente de N.  Sin
  orden en las claves no puede comprimir rangos, así que cada inserción cuesta
  lo mismo: el overhead fijo de navegar y rebalancear el B-tree.
- **RB-Tree search** en random crece de ~86 ns a ~437 ns al pasar de n=1000 a
  n=500 000, evidenciando O(log n) con constante mayor que en seq por los cache
  misses en punteros aleatorios.

---

### `03_impacto_modo.png` — Seq vs random por estructura (n = 100 000)

**Qué prueba.** Aísla el efecto del orden de acceso sobre cada estructura,
comparando secuencial (azul) y random (rojo) en el mismo panel.  Responde a:
¿cuánto le importa a cada estructura el orden de las claves?

**Resultados.**

- **Maple Tree — insert:** el random es ~2.7× más caro (4122 vs 1507 ns/op).
  Es la penalización más grande del benchmark.  Sin claves ordenadas el Maple
  Tree no puede usar dense nodes y cada inserción requiere más splits de nodos.
- **Maple Tree — delete:** ~1.8× más caro en random (3595 vs 2021 ns/op).  El
  borrado también necesita recolapsar rangos, y eso es más costoso cuando los
  rangos no son contiguos.
- **RB-Tree — iterate:** ~3.3× más caro en random (27 vs 8 ns/op).  El árbol
  construido con claves desordenadas tiene sus nodos dispersos en el heap; la
  iteración in-order fuerza accesos no lineales a memoria.
- **RB-Tree — search:** ~2.2× más caro en random (192 vs 89 ns/op).  El camino
  de búsqueda sigue siendo O(log n) pasos, pero cada paso salta a un puntero
  distante → más cache misses.
- **Lista:** prácticamente indiferente al modo (las barras azul y roja son casi
  iguales en todas las operaciones).  La lista no indexa por clave, así que el
  orden de inserción no cambia su estructura interna.

---

### `04_sched_simulation.png` — Simulación del scheduler CFS

**Qué prueba.** El camino crítico de `pick_next_task_fair()` del CFS usando un
RB-Tree con caché del nodo mínimo (`rb_leftmost`), exactamente como lo hace el
kernel.  Se miden 100 000 ticks para cada tamaño de runqueue (4 a 1024 tareas),
y el coste se reporta en **ns por tick**.  Un tick equivale a:
`pick_next [O(1)] + dequeue [O(log n)] + vruntime += 4 ms + enqueue [O(log n)]`.

**Qué se ve.**

- El coste por tick crece **logarítmicamente** con el tamaño del runqueue:
  ~140–150 ns con 4 tareas, ~260–290 ns con 1024 tareas.  Esto confirma que
  el O(log n) del enqueue/dequeue domina y que pick_next es efectivamente O(1).
- Las curvas de los distintos valores de N (1000, 10000, ..., 500000) se
  solapan casi perfectamente: el número de ticks que se miden no afecta al
  coste por tick, lo que descarta efectos de calentamiento.
- A **runqueue size = 32** (línea punteada gris, tamaño típico de tareas activas
  por CPU en un servidor cargado) el tick cuesta ~200–220 ns, que a 4 ms de
  slice equivale a menos del 0.006 % del tiempo de CPU gastado en decisiones
  de scheduling.
- El modo seq y el modo random producen curvas casi idénticas porque
  `bench_sched_rq` no usa el array de claves aleatorias — las claves son
  siempre `vruntime` monótonamente creciente, independientemente del modo
  global del benchmark.

**Por qué el patrón de acceso es secuencial en la práctica.** La tarea que
acaba de ejecutarse recibe `vruntime += slice` y se reinserta siempre hacia la
derecha del árbol.  El árbol nunca recibe una clave "sorpresa": todas las
inserciones van a posiciones predecibles cerca del extremo derecho, y todas las
extracciones vienen del extremo izquierdo cacheado.  Esto mantiene los dos o
tres nodos más accedidos calientes en caché L1 independientemente del tamaño
del runqueue.

---

### `05_peor_caso.png` — Stress test a n = 500 000

**Qué prueba.** El comportamiento de cada estructura en el tamaño máximo
disponible.  A 500 000 elementos los árboles ya no caben en la caché L2/L3 de
la mayoría de las CPUs, lo que amplifica todos los efectos de localidad de
caché.

**Resultados destacados.**

- **Maple Tree random insert: ~4090 ns/op** frente a ~1620 ns/op secuencial
  (2.5×).  La mayor penalización del benchmark en términos absolutos.
- **RB-Tree random iterate: ~100 ns/op** frente a ~8.7 ns/op secuencial
  (**11×**).  Es la mayor relación seq/random de todo el benchmark.  A 500 000
  nodos dispersos en el heap, la iteración in-order genera un cache miss
  prácticamente en cada puntero `rb_next`.
- **RB-Tree random search: ~437 ns/op** frente a ~124 ns/op secuencial (3.5×).
  O(log n) = ~19 pasos de búsqueda, cada uno con alta probabilidad de cache
  miss a este tamaño.
- **Lista insert y delete** permanecen por debajo de 25 ns/op en ambos modos y
  a cualquier N, confirmando que son operaciones O(1) que no dependen del
  tamaño de la estructura.

---

### `06_pid_max_scenario.png` — Escenario tabla de procesos (n = 32 768)

**Qué prueba.** El valor de `pid_max` por defecto en Linux es 32 768 —
el máximo de procesos concurrentes con la configuración estándar del kernel.
Esta gráfica usa ese N como escenario concreto: simular una tabla de procesos
completamente llena para comparar qué estructura sería más eficiente para
gestionarla.

**Acceso ordenado (panel izquierdo)** representa operaciones del kernel que
iteran la tabla de procesos en orden: `for_each_process`, recorrido de `/proc`,
volcado de estado.

**Acceso aleatorio (panel derecho)** representa búsquedas por PID arbitrario:
`find_task_by_vpid()`, señales, ptrace, etc.

**Conclusiones.**

- Para **búsqueda por PID aleatorio** (el caso más frecuente en un sistema
  real), el RB-Tree gana: ~78 ns/op (seq) y ~148 ns/op (random) frente a
  ~296 ns/op del Maple Tree en ambos modos.  El Maple Tree tiene más overhead
  de navegación en B-tree para N moderado.
- Para **iteración ordenada** (recorrido completo de la tabla), el RB-Tree
  también gana en tiempo absoluto: ~7 ns/op vs ~76 ns/op del Maple Tree.
- El **insert** del Maple Tree (~1867 ns secuencial, ~3713 ns random) es de
  5–13× más caro que el del RB-Tree (~258 ns, ~282 ns), lo que justifica por
  qué el kernel siguió usando RB-Tree para la tabla de procesos (`pid` namespace)
  incluso después de migrar las VMAs al Maple Tree: para PIDs, las claves no
  son rangos contiguos y el acceso es mayoritariamente random.

---

### `simple_time_vs_n.png` — Vista general: tiempo vs N

**Qué prueba.** Una vista consolidada de todas las operaciones y los dos modos
en una sola imagen.  Cada panel es una combinación (operación × modo), con N
en el eje X (escala log) y ns/op en el eje Y.  Es el punto de entrada más
rápido para ver el comportamiento global.

**Lectura rápida.**

- **Líneas planas** → coste constante respecto a N: Lista insert/delete, y los
  árboles en insert (el overhead es fijo por operación, no crece con N visible
  a esta escala).
- **Línea verde que sube** en `search`: la Lista disparándose en O(n), el único
  caso donde crecer N encarece la operación de forma dramática y visible.
- **Maple Tree más alto siempre en insert/delete** y más bajo que el RB-Tree en
  iterate: el trade-off de diseño del Maple Tree — paga más en modificaciones
  para ganar en recorrido y localidad de caché.
- **Modo random eleva todas las líneas** respecto al secuencial, pero el efecto
  no es uniforme: afecta mucho más al Maple Tree (insert/delete) y al RB-Tree
  (search/iterate) que a la Lista.

---

## Agregar un nuevo benchmark

1. Agregar la función `bench_mi_estructura(unsigned long n)` en `bench_main.c`.
2. Si la estructura necesita un `.c` del kernel, incluirlo con `#include "../../../lib/mi_estructura.c"`.
3. Llamar la función desde `main()`.
4. Si la estructura tiene dependencias nuevas (headers o `.c` de lib/), agregarlas al `Makefile`.

Para verificar que el include path es correcto:

```bash
# Ver qué header encuentra el compilador para un include dado
echo '#include <linux/mi_header.h>' | \
    gcc -I../shared -I../../include -I../../../include \
        -D_LGPL_SOURCE -M -MF /dev/null -x c -
```
