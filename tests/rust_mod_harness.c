/* Loads a Rust mod (a Linux build of examples/hello-rust) with a fake GridNode API table and checks
 * that the bindings call through the C ABI correctly: entry point, logging, ticks, messages. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "gridnode/api.h"

static int lines = 0, saw_loaded = 0, saw_faith = 0, saw_message = 0;
static gn_tick_fn client_fn, server_fn;
static void *client_user, *server_user;
static gn_message_fn msg_fn;
static void* msg_user;
static gn_transform faith = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {1.5f, 2.5f, 3.5f, 1}};

static void api_log(const char* mod, const char* m) {
    printf("[%s] %s\n", mod, m);
    ++lines;
    if (strstr(m, "loaded into GridNode test")) saw_loaded = 1;
    if (strstr(m, "Faith at (1.50, 2.50, 3.50)")) saw_faith = 1;
    if (strstr(m, "message: ServerPlayerTickMessage (category ServerPlayer)")) saw_message = 1;
}
static int api_on_client_tick(gn_tick_fn fn, void* u) { client_fn = fn; client_user = u; return GN_OK; }
static int api_on_server_tick(gn_tick_fn fn, void* u) { server_fn = fn; server_user = u; return GN_OK; }
static const gn_type* api_type_find(const char* n) { (void)n; return NULL; }
static void* api_local(void) { return &faith; }
static int api_transform(const void* e, gn_transform* out) { *out = *(const gn_transform*)e; return GN_OK; }
static int api_raycast(const float* f, const float* t, float r, gn_ray_hit* out) {
    (void)f; (void)r; memset(out, 0, sizeof(*out)); out->position[1] = t[1] + 50.f; return GN_OK;
}
static int api_on_message(const char* mod, const char* name, gn_message_fn fn, void* u) {
    (void)mod;
    if (strcmp(name, "ServerPlayer") != 0) return GN_ERR_NOT_FOUND;
    msg_fn = fn; msg_user = u; return GN_OK;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    gn_mod_load_fn load = (gn_mod_load_fn)dlsym(h, "gn_mod_load");
    if (!load) { printf("no gn_mod_load\n"); return 1; }
    gn_api a;
    memset(&a, 0, sizeof(a));
    a.version = GN_API_VERSION;
    a.size = sizeof(gn_api);
    a.gridnode_version = "test";
    a.log = api_log;
    a.on_client_tick = api_on_client_tick;
    a.on_server_tick = api_on_server_tick;
    a.type_find = api_type_find;
    a.local_client_character = api_local;
    a.local_server_character = api_local;
    a.entity_transform = api_transform;
    a.raycast = api_raycast;
    a.on_message = api_on_message;
    if (load(&a) != 0) { printf("gn_mod_load failed\n"); return 1; }
    if (!client_fn || !server_fn || !msg_fn) { printf("callbacks not registered\n"); return 1; }
    client_fn(2.5f, client_user);
    server_fn(2.5f, server_user);
    gn_message_info info = {NULL, "ServerPlayerTickMessage", "ServerPlayer", 1, 2, 0xff, 2};
    char msg[0x30] = {0};
    msg_fn(msg, &info, msg_user);
    /* An older host (smaller table) is refused. */
    a.size = 16;
    int refused = load(&a) != 0;
    printf("%d lines; loaded %d, faith %d, message %d, old host refused %d\n", lines, saw_loaded, saw_faith, saw_message, refused);
    return saw_loaded && saw_faith && saw_message && refused ? 0 : 1;
}
