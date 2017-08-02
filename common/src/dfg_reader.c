#include <strings.h>
#include <string.h>
#include "ip_utils.h"
#include "dfg.h"
#include "dfg_reader.h"
#include "jsmn_parser.h"
#include "jsmn.h"

enum object_type {
    ROOT=0, RUNTIMES=1, ROUTES=2, DESTINATIONS=3, MSUS=4, PROFILING=5,
    META_ROUTING=6, SOURCE_TYPES=7, SCHEDULING=8, DEPENDENCIES=9
};

struct dfg_config *dfg_global = NULL;

struct dfg_config *get_dfg(){
    return dfg_global;
}

int load_dfg_from_file(const char *init_filename){
    dfg_global = parse_dfg_json(init_filename);
    if (dfg_global == NULL){
        log_error("Error loading dfg from file %s!", init_filename);
        return -1;
    }
    return 0;
}

static struct key_mapping key_map[];

struct dfg_config *parse_dfg_json(const char *filename){

    struct dfg_config *cfg = calloc(1, sizeof(*cfg));

    int rtn = parse_into_obj(filename, cfg, key_map);

    if (rtn >= 0){
        pthread_mutex_init(&cfg->dfg_mutex, NULL);
        log_debug("Success!");
        return cfg;
    } else {
        log_error("Failed to parse jsmn");
        return NULL;
    }
}
static int set_app_name(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_config *cfg = in->data;
    strcpy(cfg->application_name, tok_to_str(*tok, j));
    return 0;
}

static int set_app_deadline(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_config *cfg = in->data;
    cfg->application_deadline = tok_to_int(*tok, j);
    return 0;
}

static int set_ctl_ip(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_config *cfg = in->data;
    strcpy(cfg->global_ctl_ip, tok_to_str(*tok, j));
    return 0;
}

static int set_ctl_port(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_config *cfg = in->data;
    cfg->global_ctl_port = tok_to_int(*tok, j);
    return 0;
}

static int set_load_mode(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved) {
    char *load_mode = tok_to_str(*tok, j);
    if (strcasecmp(load_mode, "preload")){
        return 0;
    } else { // TODO: What if preload/toload?
        return 0;
    }
}

struct json_state init_runtime(struct json_state *in, int index){
    struct dfg_config *cfg = in->data;

    cfg->runtimes_cnt++;
    cfg->runtimes[index] = calloc(1, sizeof(*cfg->runtimes[index]));

    cfg->runtimes[index]->num_pinned_threads = 0;
    cfg->runtimes[index]->num_threads = 0;
    cfg->runtimes[index]->num_cores = 0;
    cfg->runtimes[index]->ip = 0;
    cfg->runtimes[index]->current_alloc = NULL;
    cfg->runtimes[index]->sock = -1;

    struct json_state rt_obj = {
        .data = cfg->runtimes[index],
        .parent_type = RUNTIMES
    };

    return rt_obj;
}

static int set_runtimes(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    return parse_jsmn_obj_list(tok, j, in, saved, init_runtime);
}

static struct json_state init_dfg_route(struct json_state *in, int index){
    struct dfg_runtime_endpoint *rt = in->data;

    rt->num_routes++;
    rt->routes[index] = calloc(1, sizeof(*rt->routes[index]));

    struct json_state route_obj = {
        .data = rt->routes[index],
        .parent_type = ROUTES
    };

    return route_obj;
}

static int set_routes(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    return parse_jsmn_obj_list(tok, j, in, saved, init_dfg_route);
}

struct json_state init_dfg_msu(struct json_state *in, int index){
    struct dfg_config *cfg = in->data;

    cfg->vertex_cnt++;
    cfg->vertices[index] = calloc(1, sizeof(struct dfg_vertex));

    struct json_state msu_obj = {
        .data = cfg->vertices[index],
        .parent_type = MSUS
    };
    return msu_obj;
}

static int set_msus(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    return parse_jsmn_obj_list(tok, j, in, saved, init_dfg_msu);
}

static int set_msu_vertex_type(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    strcpy(vertex->vertex_type, tok_to_str(*tok, j));
    return 0;
}

static int set_msu_profiling(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    struct json_state profiling = {
        .data = &vertex->profiling,
        .parent_type = PROFILING,
        .tok = *tok
    };
    int n_parsed = parse_jsmn_obj(tok, j, &profiling, saved);
    if (n_parsed < 0){
        return -1;
    }
    return 0;
}

static int set_meta_routing(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    struct json_state meta_routing = {
        .data = &vertex->meta_routing,
        .parent_type = META_ROUTING,
        .tok = (*tok)
    };
    int n_parsed = parse_jsmn_obj(tok, j, &meta_routing, saved);
    if (n_parsed < 0){
        return -1;
    }
    return 0;
}

static int set_working_mode(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    strcpy(vertex->msu_mode, tok_to_str(*tok, j));

    struct runtime_thread *thread = vertex->scheduling.thread;
    if (thread == NULL) {
        // Have to wait for the thread to be instantiated
        return 1;
    }

    if (strcasecmp(vertex->msu_mode, "non-blocking") == 0) {
        if (thread->mode != 1) {
            if (thread->num_msus > 1) {
                log_error("MSUs of mixed types placed on thread %d! Cannot proceed!", thread->id);
                return -1;
            } else {
                log_warn("Changing working mode of thread %d to non-blocking. Count of non-blocking threads may now be innacurate.", thread->id);
                thread->mode = 1;
                return 0;
            }
        }
    } else if (strcasecmp(vertex->msu_mode, "blocking") == 0) {
        if (thread->mode != 2) {
            if (thread->num_msus > 1) {
                log_error("MSUs of mixed types placed on thread %d! Cannot proceed", thread->id);
                return -1;
            } else {
                log_warn("Changing working mode of thread %d to blocking. Count of non-blocking threads may now be innacurate.", thread->id);
                thread->mode = 2;
                return 0;
            }
        }
    } else {
        log_error("Unknown working mode: %s", vertex->msu_mode);
        return -1;
    }

    return 0;
}

static int set_scheduling(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    struct json_state scheduling = {
        .data = &vertex->scheduling,
        .parent_type = SCHEDULING,
        .tok = *tok
    };
    int n_parsed = parse_jsmn_obj(tok, j, &scheduling, saved);
    if (n_parsed < 0){
        return -1;
    }
    return 0;
}

static int set_msu_type(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    vertex->msu_type = tok_to_int(*tok, j);
    return 0;
}

static int set_msu_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    vertex->msu_id = tok_to_int(*tok, j);
    log_debug("Set msu ID %d", vertex->msu_id);
    return 0;
}

static int set_msu_init_data(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_vertex *vertex = in->data;
    strcpy(vertex->init_data, tok_to_str(*tok, j));
    log_debug("Set MSU init data to %s", vertex->init_data);
    return 0;
}

static int set_rt_dram(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->dram = tok_to_int(*tok, j);
    return 0;
}

static int set_rt_num_threads(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved) {
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->num_threads = tok_to_int(*tok, j);

    int i;
    for (i = 0; i < runtime->num_threads; ++i) {
        struct runtime_thread *rt_thread = calloc(1, sizeof(*rt_thread));
        rt_thread->id = i + 1;
        rt_thread->mode = 2;

        runtime->threads[i] = rt_thread;
    }

    return 0;
}

static int set_rt_num_pinned_threads(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved) {
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->num_pinned_threads = tok_to_int(*tok, j);

    if (runtime->num_threads <= 0) {
        // num_threads must be instantiated before num_pinned_threads
        return 1;
    }

    if (runtime->num_threads < runtime->num_pinned_threads) {
        log_error("Cannot instantiate more pinned threads than threads");
        return -1;
    }

    for (int i = 0; i < runtime->num_pinned_threads; ++i) {
        runtime->threads[i]->mode = 1;
    }

    return 0;
}

static int set_rt_ip(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    char *ipstr = tok_to_str(*tok, j);
    string_to_ipv4(ipstr, &runtime->ip);
    return 0;
}

static int set_rt_bw(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->io_network_bw = tok_to_int(*tok, j);
    return 0;
}

static int set_rt_n_cores(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->num_cores = tok_to_int(*tok, j);
    return 0;
}

static int set_rt_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->id = tok_to_int(*tok, j);
    return 0;
}

static int set_rt_port(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_runtime_endpoint *runtime = in->data;
    runtime->port = tok_to_int(*tok, j);
    return 0;
}

static int set_route_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_route *route = in->data;
    route->route_id = tok_to_int(*tok, j);
    return 0;
}

static int set_route_type(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_route *route = in->data;
    route->msu_type = tok_to_int(*tok, j);
    return 0;
}

static int set_route_destination(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_route *route = in->data;
    struct dfg_config *cfg =  get_jsmn_obj();
    int n_dests = (*tok)->size;
    if (cfg->vertex_cnt == 0){
        (*tok) += (n_dests * 2);
        return 1;
    }
    ASSERT_JSMN_TYPE(*tok, JSMN_OBJECT, j);
    bzero(route->destination_keys, sizeof(int) * MAX_DESTINATIONS);
    for (int i=0; i<n_dests; i++){
        ++(*tok);
        ASSERT_JSMN_TYPE(*tok, JSMN_STRING, j);
        log_debug("looking for msu %s", tok_to_str(*tok, j));
        int msu_id = tok_to_int(*tok, j);
        int found = 0;
        for (int msu_i=0; msu_i<cfg->vertex_cnt; msu_i++){
            if (cfg->vertices[msu_i]->msu_id == msu_id){
                found = 1;
                ++(*tok);
                ASSERT_JSMN_TYPE(*tok, JSMN_STRING, j);
                int key = tok_to_int(*tok, j);
                struct dfg_vertex *v = cfg->vertices[msu_i];
                route->destinations[route->num_destinations] = v;
                route->destination_keys[route->num_destinations] = key;
                ++route->num_destinations;
                break;
            }
        }
        if (!found){
            log_error("Could not find msu %d for route %d", msu_id, route->route_id);
            return -1;
        }
    }
    return 0;
}

static int set_prof_dram(jsmntok_t  **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_profiling *profiling = in->data;
    profiling->dram = tok_to_long(*tok, j);
    return 0;
}

static int set_tx_node_local(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_profiling *profiling = in->data;
    profiling->tx_node_local = tok_to_int(*tok, j);
    return 0;
}

static int set_wcet(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_profiling *profiling = in->data;
    profiling->wcet = tok_to_int(*tok, j);
    return 0;
}

static int set_tx_node_remote(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_profiling *profiling = in->data;
    profiling->tx_node_remote = tok_to_int(*tok, j);
    return 0;
}

static int set_tx_core_local(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_profiling *profiling = in->data;
    profiling->tx_core_local = tok_to_int(*tok, j);
    return 0;
}

static int set_source_types(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_meta_routing *meta = in->data;
    int n_srcs = (*tok)->size;
    for (int i=0; i<n_srcs; i++){
        ++(*tok);
        int type = tok_to_int(*tok, j);
        meta->src_types[meta->num_src_types] = type;
        ++meta->num_src_types;
    }
    return 0;
}

static int set_dst_types(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_meta_routing *meta = in->data;
    int n_dests = (*tok)->size;
    for (int i=0; i<n_dests; i++){
        ++(*tok);
        int type = tok_to_int(*tok, j);
        meta->dst_types[meta->num_dst_types] = type;
        ++meta->num_dst_types;
    }
    return 0;
}

static int set_thread_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_scheduling *sched = in->data;
    int thread_id = tok_to_int(*tok, j);

    if (sched->runtime == NULL) {
        return 1;
    }

    struct dfg_config *cfg = get_jsmn_obj();
    if (cfg->runtimes_cnt == 0){
        return 1;
    }

    // Bit of a convoluted process to find the current MSU...
    // Go through the vertices and find the msu with the scheduling pointer
    // that is equal to this MSUs
    struct dfg_vertex *this_msu = NULL;
    for (int i = 0; i < cfg->vertex_cnt; i++) {
        if (&cfg->vertices[i]->scheduling == sched) {
            this_msu = cfg->vertices[i];
            break;
        }
    }

    if (this_msu == NULL)
        return 1;

    struct dfg_runtime_endpoint *rt = sched->runtime;
    for (int i = 0; i < rt->num_threads; i++) {
        struct runtime_thread *thread = rt->threads[i];
        if (thread != NULL && thread->id == thread_id) {
            thread->msus[thread->num_msus] = this_msu;
            thread->num_msus++;
            sched->thread = thread;
            sched->thread_id = thread_id;
            log_info("Setting thread of msu %d to %d", this_msu->msu_id, thread_id);
            return 0;
        }
    }

    // Thread object was not found
    return 1;
}

static int set_deadline(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_scheduling *sched = in->data;
    sched->deadline = tok_to_int(*tok, j);
    return 0;
}

static int set_msu_rt_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dfg_config *cfg = get_jsmn_obj();
    if (cfg->runtimes_cnt == 0){
        return 1;
    }

    struct msu_scheduling *sched = in->data;
    ASSERT_JSMN_TYPE(*tok, JSMN_STRING, j);
    int id = tok_to_int(*tok, j);

    for (int i=0; i<cfg->runtimes_cnt; i++){
        struct dfg_runtime_endpoint *rt = cfg->runtimes[i];
        if (rt->id == id){
            sched->runtime = rt;
            return 0;
        }
    }
    log_error("Could not find runtime with id %d (%d present runtimes)", id, cfg->runtimes_cnt);
    return -1;
}

static int set_msu_routing(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    int n_routes = (*tok)->size;
    struct dfg_config *cfg = get_jsmn_obj();
    struct msu_scheduling *sched = in->data;
    if (cfg->runtimes_cnt == 0 ||
            sched->runtime == NULL){
        (*tok) += n_routes;
        return 1;
    }
    ASSERT_JSMN_TYPE(*tok, JSMN_ARRAY, j);
    for (int i=0; i<n_routes; i++){
        ++(*tok);
        int route_id = tok_to_int(*tok, j);
        int found = 0;
        for (int route_i=0; route_i < sched->runtime->num_routes; route_i ++){
            if (sched->runtime->routes[route_i]->route_id == route_id){
                found = 1;
                struct dfg_route *route = sched->runtime->routes[route_i];
                sched->routes[sched->num_routes] = route;
                ++sched->num_routes;
                break;
            }
        }
        if (!found){
            log_error("Could not find required route %d", route_id);
            return -1;
        }
    }
    return 0;
}

static int set_msu_cloneable(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_scheduling *sched = in->data;
    sched->cloneable = tok_to_int(*tok, j);
    return 0;
}

static int set_msu_colocation(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct msu_scheduling *sched = in->data;
    sched->colocation_group = tok_to_int(*tok, j);
    return 0;
}

struct json_state init_dependency(struct json_state *in, int index){
    struct dfg_vertex *vertex = in->data;

    vertex->num_dependencies++;
    struct json_state rt_obj = {
        .data = &vertex->dependencies[index],
        .parent_type = DEPENDENCIES
    };

    return rt_obj;
}

static int set_dependencies(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    return parse_jsmn_obj_list(tok, j, in, saved, init_dependency);
}

static int set_dependency_id(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dependent_type *dep = in->data;

    dep->msu_type = tok_to_int(*tok, j);
    return 0;
}

static int set_dependency_locality(jsmntok_t **tok, char *j, struct json_state *in, struct json_state **saved){
    struct dependent_type *dep = in->data;

    dep->locality = tok_to_int(*tok, j);
    return 0;
}

static struct key_mapping key_map[] = {
    { "application_name", ROOT, set_app_name },
    { "application_deadline", ROOT, set_app_deadline },
    { "global_ctl_ip", ROOT, set_ctl_ip },
    { "global_ctl_port", ROOT, set_ctl_port },
    { "load_mode", ROOT, set_load_mode },

    { "runtimes", ROOT, set_runtimes },
    { "MSUs", ROOT, set_msus },

    { "vertex_type", MSUS, set_msu_vertex_type },
    { "profiling", MSUS,  set_msu_profiling },
    { "meta_routing", MSUS,  set_meta_routing },
    { "working_mode", MSUS,  set_working_mode },
    { "scheduling", MSUS,  set_scheduling },
    { "type", MSUS,  set_msu_type },
    { "id", MSUS,  set_msu_id },
    { "dependencies", MSUS,  set_dependencies },

    { "init_data", MSUS, set_msu_init_data },
    { "name", MSUS, jsmn_ignore },
    { "statistics", MSUS, jsmn_ignore },

    { "dram", RUNTIMES, set_rt_dram },
    { "ip", RUNTIMES,  set_rt_ip },
    { "io_network_bw", RUNTIMES, set_rt_bw },
    { "num_cores", RUNTIMES, set_rt_n_cores },
    { "id", RUNTIMES, set_rt_id },
    { "port", RUNTIMES, set_rt_port },
    { "routes", RUNTIMES, set_routes },
    { "num_threads", RUNTIMES, set_rt_num_threads },
    { "num_pinned_threads", RUNTIMES, set_rt_num_pinned_threads },

    { "id", ROUTES, set_route_id },
    { "destinations", ROUTES, set_route_destination },
    { "type", ROUTES, set_route_type },

    { "dram", PROFILING, set_prof_dram },
    { "tx_node_local", PROFILING,  set_tx_node_local },
    { "wcet", PROFILING,  set_wcet },
    { "tx_node_remote", PROFILING,  set_tx_node_remote },
    { "tx_core_local", PROFILING,  set_tx_core_local },

    { "source_types", META_ROUTING,  set_source_types },
    { "dst_types", META_ROUTING, set_dst_types },

    { "msu_type", DEPENDENCIES, set_dependency_id },
    { "locality", DEPENDENCIES, set_dependency_locality },

    { "thread_id", SCHEDULING,  set_thread_id },
    { "deadline", SCHEDULING,  set_deadline },
    { "runtime_id", SCHEDULING,  set_msu_rt_id },
    { "routing", SCHEDULING, set_msu_routing },
    { "cloneable", SCHEDULING, set_msu_cloneable },
    { "colocation_group", SCHEDULING, set_msu_colocation },

    { NULL, 0, NULL }
};

