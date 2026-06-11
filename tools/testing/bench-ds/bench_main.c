// SPDX-License-Identifier: GPL-2.0+
/*
 * bench_main.c — Benchmark de estructuras de datos del kernel en userspace.
 *
 * Uso:
 *   ./bench_ds [n] [seq|random] [output.csv]
 *
 *   n           número de elementos (default: 100000)
 *   seq|random  modo de generación de claves (default: seq)
 *   output.csv  archivo CSV de salida (opcional)
 *
 * Ejemplos:
 *   ./bench_ds
 *   ./bench_ds 500000 random
 *   ./bench_ds 200000 seq resultados.csv
 *   ./bench_ds 200000 random resultados.csv
 */

/* ==============================================================
 * 1. COMPAT LAYER
 * ============================================================== */
#include "bench.h"

#ifndef module_init
#define module_init(x)
#endif
#ifndef module_exit
#define module_exit(x)
#endif
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#ifndef dump_stack
#define dump_stack() assert(0)
#endif

/* ==============================================================
 * 2. FUENTES DEL KERNEL
 * ============================================================== */
#include "../../../lib/maple_tree.c"

/* __rb_change_child_rcu no está en la copia de tools/include/linux/
 * rbtree_augmented.h. En userspace no hay diferencia semántica real. */
#define __rb_change_child_rcu __rb_change_child
#include "../../../lib/rbtree.c"

/* list.h es solo inline functions — no existe lib/list.c */

/* ==============================================================
 * 3. CONFIGURACIÓN Y RESULTADOS
 * ============================================================== */

struct bench_config {
	unsigned long  n;
	bool           random_mode;
	FILE          *out;        /* NULL = sin exportación */
};

struct bench_result {
	char           ds[16];
	char           op[16];
	unsigned long  n_ops;
	unsigned long long ns;
};

#define MAX_RESULTS 32
static struct bench_result g_results[MAX_RESULTS];
static int                 g_nresults;

/* Registra un resultado: lo imprime en stdout y lo guarda para exportar. */
static void record(const char *ds, const char *op,
		   unsigned long n_ops, unsigned long long ns)
{
	printf("  %-14s  %-10s  n=%-8lu  total=%12llu ns  %.2f ns/op\n",
	       ds, op, n_ops, ns, n_ops ? (double)ns / n_ops : 0.0);

	if (g_nresults < MAX_RESULTS) {
		struct bench_result *r = &g_results[g_nresults++];
		strncpy(r->ds,  ds,  sizeof(r->ds)  - 1);
		strncpy(r->op,  op,  sizeof(r->op)  - 1);
		r->n_ops = n_ops;
		r->ns    = ns;
	}
}

/* Escribe todos los resultados acumulados al archivo CSV. */
static void export_csv(const struct bench_config *cfg)
{
	const char *mode;
	int i;

	if (!cfg->out)
		return;

	mode = cfg->random_mode ? "random" : "sequential";

	fprintf(cfg->out, "ds,operation,mode,n,total_ns,ns_per_op\n");
	for (i = 0; i < g_nresults; i++) {
		struct bench_result *r = &g_results[i];
		fprintf(cfg->out, "%s,%s,%s,%lu,%llu,%.2f\n",
			r->ds, r->op, mode, cfg->n, r->ns,
			r->n_ops ? (double)r->ns / r->n_ops : 0.0);
	}
	fclose(cfg->out);
}

/* ==============================================================
 * 4. GENERACIÓN DE CLAVES
 *
 * Genera un array de n claves únicas (0..n-1).
 * En modo random aplica Fisher-Yates con semilla fija para
 * reproducibilidad entre corridas.
 * ============================================================== */
static unsigned long *gen_keys(unsigned long n, bool random_mode)
{
	unsigned long *keys = malloc(n * sizeof(*keys));
	unsigned long  i;

	assert(keys);
	for (i = 0; i < n; i++)
		keys[i] = i;

	if (random_mode) {
		srand(42); /* semilla fija — resultados reproducibles */
		for (i = n - 1; i > 0; i--) {
			unsigned long j   = (unsigned long)rand() % (i + 1);
			unsigned long tmp = keys[i];
			keys[i] = keys[j];
			keys[j] = tmp;
		}
	}
	return keys;
}

/* ==============================================================
 * 5. MAPLE TREE BENCHMARK
 *
 * Modo secuencial: claves 0..n-1 en orden → el árbol puede usar
 * "dense nodes" (rangos contiguos en un solo nodo) → muy eficiente.
 *
 * Modo random: claves en orden aleatorio → nodos más dispersos,
 * rendimiento más cercano al rbtree.
 * ============================================================== */
static void bench_maple(unsigned long *keys, const struct bench_config *cfg)
{
	struct maple_tree  mt;
	unsigned long long t;
	unsigned long      i;
	unsigned long      n = cfg->n;
	void              *entry;

	mt_init(&mt);

	/* Insert */
	t = bench_now();
	for (i = 0; i < n; i++)
		mtree_insert(&mt, keys[i], xa_mk_value(keys[i]), GFP_KERNEL);
	record("maple_tree", "insert", n, bench_now() - t);

	/* Search: busca en el mismo orden en que se insertó */
	t = bench_now();
	for (i = 0; i < n; i++) {
		entry = mtree_load(&mt, keys[i]);
		(void)entry;
	}
	record("maple_tree", "search", n, bench_now() - t);

	/* Iterate */
	{
		MA_STATE(mas, &mt, 0, 0);
		unsigned long count = 0;

		t = bench_now();
		mas_for_each(&mas, entry, ULONG_MAX)
			count++;
		record("maple_tree", "iterate", count, bench_now() - t);
	}

	/* Delete */
	t = bench_now();
	for (i = 0; i < n; i++)
		mtree_erase(&mt, keys[i]);
	record("maple_tree", "delete", n, bench_now() - t);

	mtree_destroy(&mt);
}

/* ==============================================================
 * 6. RED-BLACK TREE BENCHMARK
 *
 * El rbtree se rebalancea solo → el orden de inserción no cambia
 * la complejidad O(log n), pero sí la estructura interna del árbol
 * y el patrón de acceso a cache durante las rotaciones.
 * ============================================================== */
struct rbt_node {
	struct rb_node rb;
	unsigned long  key;
};

static struct rb_root rbt_root;

static void rbt_insert_node(struct rbt_node *new)
{
	struct rb_node **link   = &rbt_root.rb_node;
	struct rb_node  *parent = NULL;

	while (*link) {
		struct rbt_node *cur = rb_entry(*link, struct rbt_node, rb);

		parent = *link;
		if (new->key < cur->key)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&new->rb, parent, link);
	rb_insert_color(&new->rb, &rbt_root);
}

static struct rbt_node *rbt_search_node(unsigned long key)
{
	struct rb_node *n = rbt_root.rb_node;

	while (n) {
		struct rbt_node *cur = rb_entry(n, struct rbt_node, rb);

		if      (key < cur->key) n = n->rb_left;
		else if (key > cur->key) n = n->rb_right;
		else                     return cur;
	}
	return NULL;
}

static void bench_rbtree(unsigned long *keys, const struct bench_config *cfg)
{
	struct rbt_node   *nodes;
	unsigned long long t;
	unsigned long      i;
	unsigned long      n = cfg->n;

	nodes = malloc(n * sizeof(*nodes));
	assert(nodes);
	rbt_root = RB_ROOT;

	/* Insert */
	t = bench_now();
	for (i = 0; i < n; i++) {
		nodes[i].key = keys[i];
		rbt_insert_node(&nodes[i]);
	}
	record("rbtree", "insert", n, bench_now() - t);

	/* Search */
	t = bench_now();
	for (i = 0; i < n; i++) {
		struct rbt_node *found = rbt_search_node(keys[i]);
		(void)found;
	}
	record("rbtree", "search", n, bench_now() - t);

	/* Iterate in-order */
	{
		struct rb_node *node;
		unsigned long   count = 0;

		t = bench_now();
		for (node = rb_first(&rbt_root); node; node = rb_next(node))
			count++;
		record("rbtree", "iterate", count, bench_now() - t);
	}

	/* Delete */
	t = bench_now();
	for (i = 0; i < n; i++)
		rb_erase(&nodes[i].rb, &rbt_root);
	record("rbtree", "delete", n, bench_now() - t);

	free(nodes);
}

/* ==============================================================
 * 7. LINKED LIST BENCHMARK
 *
 * Insert/delete son O(1) con puntero directo.
 * Search es O(n) por operación — se limita a LIST_SEARCH_CAP
 * para evitar que el modo random (donde cada búsqueda es O(n/2))
 * tarde minutos con n grande.
 * ============================================================== */
struct lst_node {
	struct list_head list;
	unsigned long    key;
};

static LIST_HEAD(bench_list);

#define LIST_SEARCH_CAP 5000

static void bench_list_ds(unsigned long *keys, const struct bench_config *cfg)
{
	struct lst_node   *nodes;
	unsigned long long t;
	unsigned long      i;
	unsigned long      n        = cfg->n;
	unsigned long      search_n = (n < LIST_SEARCH_CAP) ? n : LIST_SEARCH_CAP;

	nodes = malloc(n * sizeof(*nodes));
	assert(nodes);
	INIT_LIST_HEAD(&bench_list);

	/* Insert — append tail: O(1) */
	t = bench_now();
	for (i = 0; i < n; i++) {
		nodes[i].key = keys[i];
		list_add_tail(&nodes[i].list, &bench_list);
	}
	record("list", "insert", n, bench_now() - t);

	/*
	 * Search — O(n) por op, limitado a search_n.
	 * En modo random cada búsqueda recorre ~n/2 nodos en promedio.
	 * En modo seq la clave keys[i]=i está en posición i → ~n/4 promedio.
	 */
	t = bench_now();
	for (i = 0; i < search_n; i++) {
		struct lst_node *entry;

		list_for_each_entry(entry, &bench_list, list) {
			if (entry->key == keys[i])
				break;
		}
	}
	record("list", "search*", search_n, bench_now() - t);

	/* Iterate: O(n) */
	{
		struct lst_node *entry;
		unsigned long    count = 0;

		t = bench_now();
		list_for_each_entry(entry, &bench_list, list)
			count++;
		record("list", "iterate", count, bench_now() - t);
	}

	/* Delete — O(1) con puntero directo al nodo */
	t = bench_now();
	for (i = 0; i < n; i++)
		list_del(&nodes[i].list);
	record("list", "delete", n, bench_now() - t);

	free(nodes);
}

/* ==============================================================
 * 8. SCHEDULER SIMULATION — rbtree runqueue
 *
 * Replica el camino crítico de pick_next_task_fair() en CFS:
 *
 *   pick_next  →  acceso al leftmost cacheado           O(1)
 *   dequeue    →  rb_erase + actualizar caché leftmost  O(log n)
 *   enqueue    →  rb_insert + quizá actualizar caché    O(log n)
 *
 * "tick" = pick_next + dequeue + vruntime += slice + enqueue
 *
 * La clave es vruntime (u64 monótonamente creciente), no un
 * entero arbitrario. El patrón de acceso es: siempre se saca el
 * mínimo y se reinserta con vruntime mayor → el árbol rota de
 * forma cíclica, igual que en un sistema real bajo carga uniforme.
 *
 * Se mide con runqueue sizes realistas: 4..128 tareas por CPU.
 * ============================================================== */

#define SCHED_TICK_SLICE_NS  4000000ULL  /* 4 ms por time slice */
#define SCHED_N_TICKS        100000UL

struct sched_task {
	struct rb_node     rb;
	unsigned long long vruntime;
};

static struct rb_root  sched_rq;
static struct rb_node *sched_leftmost;  /* caché del mínimo, como cfs_rq->rb_leftmost */

static void sched_enqueue(struct sched_task *t)
{
	struct rb_node **link     = &sched_rq.rb_node;
	struct rb_node  *parent   = NULL;
	bool             leftmost = true;

	while (*link) {
		struct sched_task *cur = rb_entry(*link, struct sched_task, rb);

		parent = *link;
		if (t->vruntime < cur->vruntime) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&t->rb, parent, link);
	rb_insert_color(&t->rb, &sched_rq);
	if (leftmost)
		sched_leftmost = &t->rb;
}

static void sched_dequeue(struct sched_task *t)
{
	/* Actualizar caché antes de borrar, como __dequeue_entity en CFS */
	if (sched_leftmost == &t->rb)
		sched_leftmost = rb_next(&t->rb);
	rb_erase(&t->rb, &sched_rq);
}

static void bench_sched_rq(unsigned long n_tasks)
{
	struct sched_task  *tasks;
	unsigned long long  t;
	unsigned long       i;
	char                op[16];

	tasks = malloc(n_tasks * sizeof(*tasks));
	assert(tasks);

	/* Inicializar runqueue con n_tasks tareas equidistantes en vruntime */
	sched_rq       = RB_ROOT;
	sched_leftmost = NULL;
	for (i = 0; i < n_tasks; i++) {
		tasks[i].vruntime = i * SCHED_TICK_SLICE_NS;
		sched_enqueue(&tasks[i]);
	}

	t = bench_now();
	for (i = 0; i < SCHED_N_TICKS; i++) {
		/* pick_next: O(1) via caché leftmost */
		struct sched_task *next = rb_entry(sched_leftmost,
						   struct sched_task, rb);
		/* dequeue + vruntime += slice + enqueue: el tick real */
		sched_dequeue(next);
		next->vruntime += SCHED_TICK_SLICE_NS;
		sched_enqueue(next);
	}
	snprintf(op, sizeof(op), "tick n=%lu", n_tasks);
	record("sched/rbt", op, SCHED_N_TICKS, bench_now() - t);

	free(tasks);
}

static void bench_sched(void)
{
	static const unsigned long sizes[] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
	unsigned int i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		bench_sched_rq(sizes[i]);
}

/* ==============================================================
 * 9. MAIN
 * ============================================================== */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Uso: %s [n] [seq|random] [output.csv]\n\n"
		"  n           número de elementos (default: 100000)\n"
		"  seq|random  orden de claves (default: seq)\n"
		"  output.csv  archivo de exportación CSV (opcional)\n\n"
		"Ejemplos:\n"
		"  %s\n"
		"  %s 500000 random\n"
		"  %s 200000 seq resultados.csv\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	struct bench_config cfg = {
		.n           = 100000,
		.random_mode = false,
		.out         = NULL,
	};
	unsigned long *keys;

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
		cfg.n = strtoul(argv[1], NULL, 10);
		if (cfg.n == 0) {
			fprintf(stderr, "Error: n debe ser > 0\n");
			usage(argv[0]);
			return 1;
		}
	}

	if (argc > 2) {
		if (strcmp(argv[2], "random") == 0) {
			cfg.random_mode = true;
		} else if (strcmp(argv[2], "seq") != 0) {
			fprintf(stderr, "Error: modo debe ser 'seq' o 'random'\n");
			usage(argv[0]);
			return 1;
		}
	}

	if (argc > 3) {
		cfg.out = fopen(argv[3], "w");
		if (!cfg.out) {
			perror(argv[3]);
			return 1;
		}
	}

	/* El maple tree necesita inicializar su kmem_cache global */
	maple_tree_init();

	keys = gen_keys(cfg.n, cfg.random_mode);

	printf("================================================================\n");
	printf("  Kernel DS Benchmark — userspace\n");
	printf("  n = %lu   modo = %s%s\n",
	       cfg.n,
	       cfg.random_mode ? "random (seed=42)" : "sequential",
	       cfg.out ? "   [exportando CSV]" : "");
	printf("================================================================\n\n");

	printf("[Maple Tree]\n");
	bench_maple(keys, &cfg);

	printf("\n[Red-Black Tree]\n");
	bench_rbtree(keys, &cfg);

	printf("\n[Linked List]\n");
	bench_list_ds(keys, &cfg);

	printf("\n* list search limitado a %d ops (O(n) por op — O(n²) total)\n",
	       LIST_SEARCH_CAP);

	printf("\n[Scheduler Simulation — rbtree runqueue]\n");
	bench_sched();
	printf("  (tick = pick_next + dequeue + vruntime+=4ms + enqueue)\n");

	export_csv(&cfg);

	if (cfg.out)
		printf("Resultados exportados a: %s\n", argv[3]);

	free(keys);
	return 0;
}
