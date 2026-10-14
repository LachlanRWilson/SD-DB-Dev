/*
 * Benchmark operations for one index implementation. Included once per
 * implementation by name_index_bench.c with these macros defined, so every
 * timed call is a direct call (no function pointers):
 *   OPS(x)                      name of the generated function for op x
 *   IMPL_INIT()                 IMPL_INSERT(name, s)    IMPL_INSERT_KEY(key, s)
 *   IMPL_REMOVE(name, s)        IMPL_FIND(name)         IMPL_SET_MSG(name, s, m)
 *   IMPL_REBUILD_BEGIN()        IMPL_REBUILD_ADD(k, s)  IMPL_REBUILD_FINISH()
 *   ITER_T                      IMPL_BEGIN(it)          IMPL_NEXT(it, e)
 *   IMPL_SEEK_RANK(it, k)       IMPL_SEEK(it, name)
 *   IMPL_FIRST_SECTOR()         IMPL_LAST_SECTOR()
 */

/**
 * @brief Untimed: (re)build the index holding sectors 0..g_n-1 in g_order.
 */
static void OPS(build)(void)
{
    IMPL_INIT();
    for (uint16_t i = 0; i < g_n; i++)
    {
        IMPL_INSERT_KEY(nb_keys[g_order[i]], g_order[i]);
    }
}

/**
 * @brief Bulk build by n sequential inserts in g_order.
 */
static void OPS(bulk_insert)(void)
{
    begin_op(OP_BULK_INSERT, bulk_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        IMPL_INIT();
        bench_prepare();
        TIMED(for (uint16_t i = 0; i < g_n; i++) { IMPL_INSERT_KEY(nb_keys[g_order[i]], g_order[i]); });
    }
    end_op("bulk_insert", 1);
}

/**
 * @brief Bulk rebuild: append everything unsorted, then one sort (AVL: and link).
 */
static void OPS(bulk_rebuild)(void)
{
    begin_op(OP_BULK_REBUILD, bulk_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        IMPL_INIT();
        bench_prepare();
        TIMED(IMPL_REBUILD_BEGIN(); for (uint16_t i = 0; i < g_n; i++) { IMPL_REBUILD_ADD(nb_keys[g_order[i]], g_order[i]); } IMPL_REBUILD_FINISH(););
    }
    end_op("bulk_rebuild", 1);
    OPS(build)();
}

/**
 * @brief Insert one new name into the index holding g_n entries, then remove
 *        it again outside the timed region.
 */
static void OPS(insert)(int where, int op_id, const char *op)
{
    begin_op(op_id, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        NiName nm;
        char buf[NB_NAME_MAX];
        pick_insert_name(where, &nm);
        nb_set(g_n, nm);
        nb_compose(nm, buf);
        bench_prepare();
        bool ok = false;
        TIMED(ok = IMPL_INSERT(buf, g_n));
        g_sink += ok;
        IMPL_REMOVE(buf, g_n);
    }
    end_op(op, 1);
}

/**
 * @brief Remove one existing entry, then reinsert it outside the timed region.
 */
static void OPS(remove)(int where, int op_id, const char *op)
{
    begin_op(op_id, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        uint16_t s;
        char buf[NB_NAME_MAX];
        if (where == WHERE_FRONT) { s = IMPL_FIRST_SECTOR(); }
        else if (where == WHERE_END) { s = IMPL_LAST_SECTOR(); }
        else { s = (uint16_t)nb_rand_below(g_n); }
        nb_compose(nb_names[s], buf);
        bench_prepare();
        bool ok = false;
        TIMED(ok = IMPL_REMOVE(buf, s));
        g_sink += ok;
        IMPL_INSERT_KEY(nb_keys[s], s);
    }
    end_op(op, 1);
}

/**
 * @brief Look up a random existing name (hit) or a name nobody has (miss).
 */
static void OPS(find)(bool hit, int op_id, const char *op)
{
    begin_op(op_id, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        NiName nm;
        char buf[NB_NAME_MAX];
        if (hit) { nm = nb_names[nb_rand_below(g_n)]; } else { nb_draw_miss(&nm); }
        nb_compose(nm, buf);
        nb_set_probe(nm);
        bench_prepare();
        uint16_t got = 0;
        TIMED(got = IMPL_FIND(buf));
        g_sink += got;
    }
    end_op(op, 1);
}

/**
 * @brief Update message_index of a random existing entry in place.
 */
static void OPS(update_message)(void)
{
    begin_op(OP_UPDATE_MSG, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        uint16_t s = (uint16_t)nb_rand_below(g_n);
        char buf[NB_NAME_MAX];
        nb_compose(nb_names[s], buf);
        bench_prepare();
        bool ok = false;
        TIMED(ok = IMPL_SET_MSG(buf, s, (uint16_t)r));
        g_sink += ok;
    }
    end_op("update_message", 1);
}

/**
 * @brief Iterate all g_n entries once in order.
 */
static void OPS(traverse)(void)
{
    begin_op(OP_TRAVERSE, traverse_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        bench_prepare();
        uint32_t acc = 0;
        TIMED(ITER_T it; SortedEntry e; IMPL_BEGIN(&it); while (IMPL_NEXT(&it, &e)) { acc += e.contact_index; });
        g_sink += acc;
    }
    end_op("traverse_all", 1);
    end_op("traverse_per_entry", g_n);
}

/**
 * @brief Page fetch by rank: jump to rank k, read 20 consecutive entries.
 */
static void OPS(page_rank)(void)
{
    begin_op(OP_PAGE_RANK, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        uint16_t k = (uint16_t)nb_rand_below((uint32_t)(g_n - PAGE_LEN + 1));
        bench_prepare();
        uint32_t acc = 0;
        TIMED(ITER_T it; SortedEntry e; IMPL_SEEK_RANK(&it, k); for (int j = 0; j < PAGE_LEN && IMPL_NEXT(&it, &e); j++) { acc += e.contact_index; });
        g_sink += acc;
    }
    end_op("page_rank20", 1);
}

/**
 * @brief Page fetch by name: seek to the first entry >= name, read 20.
 */
static void OPS(page_name)(void)
{
    begin_op(OP_PAGE_NAME, single_reps());
    for (uint32_t r = 0; r < g_total; r++)
    {
        NiName nm = nb_names[nb_rand_below(g_n)];
        char buf[NB_NAME_MAX];
        nb_compose(nm, buf);
        nb_set_probe(nm);
        bench_prepare();
        uint32_t acc = 0;
        TIMED(ITER_T it; SortedEntry e; IMPL_SEEK(&it, buf); for (int j = 0; j < PAGE_LEN && IMPL_NEXT(&it, &e); j++) { acc += e.contact_index; });
        g_sink += acc;
    }
    end_op("page_name20", 1);
}

/**
 * @brief Run every operation for this implementation at the current g_n.
 *        update_only limits the run to the operations that move entries.
 */
static void OPS(run_all)(bool update_only)
{
    OPS(build)();
    OPS(bulk_insert)();
    if (!update_only) { OPS(bulk_rebuild)(); }
    OPS(build)();
    OPS(insert)(WHERE_RANDOM, OP_INSERT_RANDOM, "insert_random");
    OPS(insert)(WHERE_FRONT, OP_INSERT_FRONT, "insert_front");
    OPS(insert)(WHERE_END, OP_INSERT_END, "insert_end");
    OPS(remove)(WHERE_RANDOM, OP_REMOVE_RANDOM, "remove_random");
    OPS(remove)(WHERE_FRONT, OP_REMOVE_FIRST, "remove_first");
    OPS(remove)(WHERE_END, OP_REMOVE_LAST, "remove_last");
    if (update_only) { return; }
    OPS(find)(true, OP_FIND_HIT, "find_hit");
    OPS(find)(false, OP_FIND_MISS, "find_miss");
    OPS(update_message)();
    OPS(traverse)();
    OPS(page_rank)();
    OPS(page_name)();
}
