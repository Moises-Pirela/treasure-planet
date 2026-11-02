// Checks that the port behaves like clockwork-c: every subsystem, through its public API.
#include "../src/ecs/clockwork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

struct Position { float x, y; };
struct Velocity { float x, y; };
struct Health { int hit_points; };

struct schema { cw_component_id position, velocity, health; };

static schema register_schema(cw_world *w) {
    schema s;
    cw_component_id base = cw_comp_block(w, 3);
    s.position = cw_cid(base, 0);
    s.velocity = cw_cid(base, 1);
    s.health = cw_cid(base, 2);
    CHECK(CW_REGISTER(w, s.position, Position));
    CHECK(CW_REGISTER(w, s.velocity, Velocity));
    CHECK(CW_REGISTER(w, s.health, Health));
    return s;
}

static void test_entities_and_components(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    schema s = register_schema(&w);

    cw_entity_id a = cw_world_create_entity(&w);
    cw_entity_id b = cw_world_create_entity(&w);
    CHECK(a == 0 && b == 1);
    CHECK(cw_world_entity_count(&w) == 2);

    Position p = {1, 2};
    CHECK(cw_comp_set(&w, a, s.position, &p, sizeof(p)));
    CHECK(cw_comp_has(&w, a, s.position));
    CHECK(!cw_comp_has(&w, b, s.position));
    CHECK(CW_GET(&w, a, s.position, Position)->y == 2);
    CHECK(cw_comp_set(&w, a, s.position, &p, sizeof(p) - 1) == false);   // size mismatch

    cw_component_id found;
    CHECK(cw_comp_by_name(&w, "Velocity", &found) && found == s.velocity);

    cw_world_mark_destroy(&w, a);
    CHECK(cw_world_is_valid(&w, a));        // still valid until swept
    cw_world_sweep(&w);
    CHECK(!cw_world_is_valid(&w, a));
    CHECK(!cw_comp_has(&w, a, s.position));
    CHECK(cw_world_create_entity(&w) == 2); // ids are not reused by default

    cw_world_destroy(&w);
}

// Component pointers must survive world growth: systems hold them across creation.
static void test_stable_pointers(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    schema s = register_schema(&w);
    cw_entity_id first = cw_world_create_entity(&w);
    Position p = {7, 8};
    cw_comp_set(&w, first, s.position, &p, sizeof(p));
    Position *held = CW_GET(&w, first, s.position, Position);
    for (int i = 0; i < 100000; ++i) cw_world_create_entity(&w);
    CHECK(held == CW_GET(&w, first, s.position, Position));
    CHECK(held->x == 7);
    cw_world_destroy(&w);
}

static void test_queries(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    schema s = register_schema(&w);
    Position p = {0, 0};
    Velocity v = {1, 1};
    Health h = {3};
    cw_entity_id moving[3];
    for (int i = 0; i < 3; ++i) {
        moving[i] = cw_world_create_entity(&w);
        cw_comp_set(&w, moving[i], s.position, &p, sizeof(p));
        cw_comp_set(&w, moving[i], s.velocity, &v, sizeof(v));
    }
    cw_entity_id still = cw_world_create_entity(&w);
    cw_comp_set(&w, still, s.position, &p, sizeof(p));
    cw_comp_set(&w, still, s.health, &h, sizeof(h));

    cw_component_id both[] = {s.position, s.velocity};
    cw_query adhoc = cw_query_iter(&w, cw_mask(both, 2));
    cw_entity_id id, last = -1;
    int seen = 0;
    while (cw_query_next(&adhoc, &id)) {
        CHECK(id > last);       // ascending
        last = id;
        ++seen;
    }
    CHECK(seen == 3);

    cw_cached_query *cached = cw_query_create(&w, cw_filter_all(cw_mask(both, 2)));
    CHECK(cw_query_count(cached) == 3);
    CHECK(cw_query_create(&w, cw_filter_all(cw_mask(both, 2))) == cached);   // shared handle
    CHECK(cw_query_refs(cached) == 2);
    cw_query_destroy(cached);

    // The cache follows membership changes, including mid-iteration.
    cw_query walk = cw_query_begin(cached);
    while (cw_query_next(&walk, &id)) cw_comp_remove(&w, id, s.velocity);
    CHECK(cw_query_is_empty(cached));
    cw_comp_set(&w, still, s.velocity, &v, sizeof(v));
    CHECK(cw_query_single(cached, &id) == CW_SINGLE_ONE && id == still);

    // Filters: position without health.
    cw_component_id none[] = {s.health};
    cw_filter filter = cw_filter_make(cw_mask(&s.position, 1), cw_signature{}, cw_mask(none, 1));
    CHECK(cw_query_count_filter(&w, filter, 100) == 3);
    CHECK(cw_filter_disjoint(filter, cw_filter_all(cw_mask(none, 1))));
    cw_world_destroy(&w);
}

static void test_observers(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    schema s = register_schema(&w);
    cw_observer *watch = cw_observer_create(&w, s.health);
    cw_entity_id e = cw_world_create_entity(&w);
    Health h = {5};
    cw_comp_set(&w, e, s.health, &h, sizeof(h));
    cw_observer_observe(watch);
    CHECK(cw_observer_has(watch, CW_CHANGE_ADDED, e));
    CHECK(cw_observer_count(watch, CW_CHANGE_REMOVED) == 0);

    CW_WRITE(&w, e, s.health, Health)->hit_points = 4;
    cw_observer_observe(watch);
    CHECK(!cw_observer_has(watch, CW_CHANGE_ADDED, e));
    CHECK(cw_observer_has(watch, CW_CHANGE_MODIFIED, e));

    cw_comp_remove(&w, e, s.health);
    cw_observer_observe(watch);
    CHECK(cw_observer_has(watch, CW_CHANGE_REMOVED, e));
    cw_world_destroy(&w);
}

static void add_damage(cw_world *, cw_entity_id, cw_component_id, void *component, const void *data, size_t) {
    ((Health *)component)->hit_points -= *(const int *)data;
}

static void test_commands(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    schema s = register_schema(&w);
    cw_commands *c = cw_world_commands(&w);

    cw_entity_id pending = cw_commands_create(c);
    CHECK(cw_entity_is_pending(pending));
    Health h = {10};
    CHECK(CW_COMMANDS_SET(c, pending, s.health, &h));
    CHECK(cw_world_entity_count(&w) == 0);      // recording touches nothing
    int damage = 3;
    CHECK(cw_commands_update(c, pending, s.health, add_damage, &damage, sizeof(damage)));
    CHECK(cw_commands_update(c, pending, s.health, add_damage, &damage, sizeof(damage)));

    cw_flush_result result = cw_commands_flush(c, &w);
    CHECK(result.failed == 0 && result.applied == 4);
    cw_id_view created = cw_commands_created(c);
    CHECK(created.count == 1);
    CHECK(CW_GET(&w, created.data[0], s.health, Health)->hit_points == 4);
    cw_world_destroy(&w);
}

static int ticks_seen[3];
static void system_a(cw_world *w, cw_phase, float) { ticks_seen[0] = (int)w->tick + 1; CHECK(ticks_seen[1] == (int)w->tick); }
static void system_b(cw_world *w, cw_phase, float) { ticks_seen[1] = (int)w->tick + 1; CHECK(ticks_seen[0] == (int)w->tick + 1); }
static void system_host(cw_world *w, cw_phase, float) { ticks_seen[2] = (int)w->tick + 1; }

static void test_sim_and_clock(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    cw_sim sim;
    cw_sim_init(&sim, CW_CLIENT);
    cw_system_desc d = {};
    d.phases = CW_PHASE(CW_UPDATE);
    d.name = "b"; d.fn = system_b; d.order = 2;
    CHECK(cw_sim_register(&sim, d));
    d.name = "a"; d.fn = system_a; d.order = 1;
    CHECK(cw_sim_register(&sim, d));
    d.name = "host"; d.fn = system_host; d.authority = CW_HOST_ONLY;
    CHECK(cw_sim_register(&sim, d));
    for (int i = 0; i < 3; ++i) cw_sim_tick(&sim, &w, 1.0f / 60.0f);
    CHECK(w.tick == 3 && ticks_seen[1] == 3);
    CHECK(ticks_seen[2] == 0);      // host-only systems do not run on a client
    cw_sim_destroy(&sim);
    cw_world_destroy(&w);

    cw_clock clock = cw_clock_make(60);
    CHECK(cw_clock_advance(&clock, 1.0f / 30.0f) == 2);
    CHECK(cw_clock_advance(&clock, 10.0f) == 5);     // clamped, then capped
    CHECK(cw_clock_alpha(&clock) == 0.0f);
}

static void test_snapshots(void) {
    cw_world a;
    cw_world_init(&a, NULL);
    schema sa = register_schema(&a);
    Position p = {3, 4};
    Health h = {9};
    cw_entity_id gone = cw_world_create_entity(&a);
    cw_entity_id kept = cw_world_create_entity(&a);
    cw_comp_set(&a, kept, sa.position, &p, sizeof(p));
    cw_comp_set(&a, kept, sa.health, &h, sizeof(h));
    cw_world_mark_destroy(&a, gone);
    cw_world_sweep(&a);
    a.tick = 77;

    cw_snapshot_save_config save = {};
    save.allow_raw = true;
    cw_writer out;
    cw_writer_init(&out);
    cw_snapshot_error error;
    CHECK(cw_snapshot_save(&a, &save, &out, &error));

    // A loader that registered the schema in another order still finds it by name.
    cw_world b;
    cw_world_init(&b, NULL);
    cw_component_id base = cw_comp_block(&b, 3);
    cw_component_id b_health = cw_cid(base, 0), b_position = cw_cid(base, 1), b_velocity = cw_cid(base, 2);
    CW_REGISTER(&b, b_health, Health);
    CW_REGISTER(&b, b_position, Position);
    CW_REGISTER(&b, b_velocity, Velocity);
    cw_snapshot_load_config load = cw_snapshot_load_defaults();
    load.allow_raw = true;
    CHECK(cw_snapshot_load(&b, out.data, out.size, &load, &error));
    CHECK(b.tick == 77);
    CHECK(!cw_world_is_valid(&b, gone));
    CHECK(CW_GET(&b, kept, b_position, Position)->y == 4);
    CHECK(CW_GET(&b, kept, b_health, Health)->hit_points == 9);
    CHECK(cw_world_create_entity(&b) == kept + 1);   // same ids from here on

    // Corrupt input is refused, not trusted.
    cw_world c;
    cw_world_init(&c, NULL);
    CW_REGISTER(&c, cw_cid(cw_comp_block(&c, 1), 0), Health);
    CHECK(!cw_snapshot_load(&c, out.data, out.size / 2, &load, &error));

    cw_writer_destroy(&out);
    cw_world_destroy(&a);
    cw_world_destroy(&b);
    cw_world_destroy(&c);
}

static void test_bytes(void) {
    unsigned char storage[4];
    cw_writer fixed;
    cw_writer_init_fixed(&fixed, storage, sizeof(storage));
    CHECK(cw_writer_u32(&fixed, 0x01020304u));
    CHECK(storage[0] == 4 && storage[3] == 1);     // little-endian
    CHECK(!cw_writer_u8(&fixed, 1));
    CHECK(!cw_writer_ok(&fixed) && cw_writer_size(&fixed) == 5);

    cw_reader r = cw_reader_make(storage, sizeof(storage));
    uint32_t value;
    uint8_t past;
    CHECK(cw_reader_u32(&r, &value) && value == 0x01020304u);
    CHECK(!cw_reader_u8(&r, &past) && past == 0 && !cw_reader_ok(&r));
}

int main(void) {
    test_entities_and_components();
    test_stable_pointers();
    test_queries();
    test_observers();
    test_commands();
    test_sim_and_clock();
    test_snapshots();
    test_bytes();
    if (failures) {
        printf("test_ecs: %d failed\n", failures);
        return 1;
    }
    puts("test_ecs: ok");
    return 0;
}
