#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <argp.h>

#include "simplekv.h"
#include "helpers.h"

struct ArgState {
    /* Flags */
    int create;
    int xrp;
    int key_set;

    /* Option Params */
    int threads;
    int requests;
    long key;

    /* Required Args */
    char *filename;
    int layers;
};

struct ArgState default_argstate() {
    struct ArgState as = {
        .threads = 1,
        .requests = 500,
    };
    return as;
}

const char *argp_program_version = "SimpleKV 0.1";
const char *argp_program_bug_address = "<etm2131@columbia.edu>";
static char doc[] =
        "SimpleKV Benchmark for Oliver XRP Kernel\v"
        "This utility provides several tools for testing and benchmarking"
        " SimpleKV database files on XRP enabled kernels.";

int get_handler(char *db_path, int flag) {
    int fd = open(db_path, flag | O_DIRECT, 0755);
    if (fd < 0) {
        printf("Fail to open file %s!\n", db_path);
        exit(0);
    }
    return fd;
}

/* TODO (etm): Probably want to delete this... seems to be unused */
int get_log_handler(int flag) {
    int fd = open(LOG_PATH, flag | O_DIRECT, 0755);
    if (fd < 0) {
        printf("Fail to open file %s!\n", LOG_PATH);
        exit(0);
    }
    return fd;
}

/* Open database and logfile; calculate number of nodes per layer of B+tree */
int initialize(size_t layer_num, int mode, char *db_path) {
    int db;
    if (mode == LOAD_MODE) {
        db = get_handler(db_path, O_CREAT|O_TRUNC|O_WRONLY);
    } else {
        db = get_handler(db_path, O_RDONLY);
    }
    
    layer_cap = (size_t *)malloc(layer_num * sizeof(size_t));
    total_node = 1;
    layer_cap[0] = 1;
    for (size_t i = 1; i < layer_num; i++) {
        layer_cap[i] = layer_cap[i - 1] * FANOUT;
        total_node += layer_cap[i];
    }
    max_key = layer_cap[layer_num - 1] * NODE_CAPACITY;
    cache_cap = 0;

    printf("%lu blocks in total, max key is %lu\n", total_node, max_key);

    return db;
}

/* Cache the first [layer_num] layers of the tree */
void build_cache(int db_fd, size_t layer_num) {
    size_t entry_num = 0;
    for (size_t i = 0; i < layer_num; i++) {
        entry_num += layer_cap[i];
    }
    
    if (posix_memalign((void **)&cache, 512, entry_num * sizeof(Node))) {
        perror("posix_memalign failed");
        exit(1);
    }

    size_t head = 0, tail = 1;
    read_node(encode(0), &cache[head], db_fd);

    while (tail < entry_num) {
        for (size_t i = 0; i < NODE_CAPACITY; i++) {
            read_node(cache[head].ptr[i], &cache[tail], db_fd);
            cache[head].ptr[i] = (ptr__t)(&cache[tail]); // in-memory cache entry has in-memory pointer
            tail++;
        }
        head++;
    }

    cache_cap = entry_num; // enable the cache
    printf("Cache built. %lu layers %lu entries in total.\n", layer_num, entry_num);
}

void free_globals(void) {
    free(layer_cap);
    free(cache);
}

int terminate(void) {
    printf("Done!\n");
    free_globals();
    return 0;
}

/* Create a new database on disk at [db_path] with [layer_num] layers */
int load(size_t layer_num, char *db_path) {
    printf("Load the database of %lu layers\n", layer_num);
    int db = initialize(layer_num, LOAD_MODE, db_path);

    // 1. Load the index
    Node *node;
    if (posix_memalign((void **)&node, 512, sizeof(Node))) {
        perror("posix_memalign failed");
        close(db);
        free_globals();
        exit(1);
    }
    /* 
    Disk layout:
    B+ tree nodes, each with 31 keys and 31 associated block offsets to other nodes
    Nodes are written by level in order, so, the root is first, followed by all nodes on the second level.
    Since each node has pointers to 31 other nodes, fanout is 31
    | 0  - 1  2  3  4 ... 31 - .... | ### LOG DATA ### |

    Leaf nodes have pointers into the log data, which is appended as a "heap" in the same file
    at the end of the B+tree. Once we reach a leaf node, we scan through its keys and if one matches
    the key we need, we read the offset into the heap and can retrieve the value.
    */
    ptr__t next_pos = 1;
    for (size_t i = 0; i < layer_num; i++) {
        size_t extent = max_key / layer_cap[i], start_key = 0;
        printf("layer %lu extent %lu\n", i, extent);
        for (size_t j = 0; j < layer_cap[i]; j++) {
            node->type = (i == layer_num - 1) ? LEAF : INTERNAL;
            size_t sub_extent = extent / NODE_CAPACITY;
            for (size_t k = 0; k < NODE_CAPACITY; k++) {
                node->key[k] = start_key + k * sub_extent;
                node->ptr[k] = node->type == INTERNAL ? 
                              encode(next_pos   * BLK_SIZE) :
                              encode(total_node * BLK_SIZE + (next_pos - total_node) * VAL_SIZE);
                next_pos++;
            }
            write(db, node, sizeof(Node));
            start_key += extent;
        }
    }

    // 2. Load the value log
    Log *log;
    if (posix_memalign((void **)&log, 512, sizeof(Log))) {
        perror("posix_memalign failed");
        close(db);
        free(node);
        free_globals();
        exit(1);
    }
    for (size_t i = 0; i < max_key; i += LOG_CAPACITY) {
        for (size_t j = 0; j < LOG_CAPACITY; j++) {
            sprintf((char *) log->val[j], "%63lu", i + j);
        }
        write(db, log, sizeof(Log));
    }

    free(log);
    free(node);
    close(db);
    return terminate();
}

void initialize_workers(WorkerArg *args, size_t total_op_count, char *db_path, int use_xrp) {
    for (size_t i = 0; i < worker_num; i++) {
        args[i].index = i;
        args[i].op_count = (total_op_count / worker_num) + (i < total_op_count % worker_num);
        args[i].db_handler = get_handler(db_path, O_RDONLY);
        args[i].log_handler = get_log_handler(O_RDONLY);
        args[i].timer = 0;
        args[i].use_xrp = use_xrp;
    }
}

void start_workers(pthread_t *tids, WorkerArg *args) {
    for (size_t i = 0; i < worker_num; i++) {
        pthread_create(&tids[i], NULL, subtask, (void*)&args[i]);
    }
}

void terminate_workers(pthread_t *tids, WorkerArg *args) {
    for (size_t i = 0; i < worker_num; i++) {
        pthread_join(tids[i], NULL);
        close(args[i].db_handler);
    }
}

int run(char *db_path, size_t layer_num, size_t request_num, size_t thread_num, int use_xrp) {
    if (!use_xrp) {
        fprintf(stderr, "Running without XRP is currently unimplemented\n");
        exit(0);
    }

    printf("Running benchmark with %ld layers, %ld requests, and %ld thread(s)\n",
                layer_num, request_num, thread_num);
    int db_fd = initialize(layer_num, RUN_MODE, db_path);
    /* Cache up to 3 layers of the B+tree */
    build_cache(db_fd, layer_num > 3 ? 3 : layer_num);

    worker_num = thread_num;
    struct timespec start, end;
    pthread_t tids[worker_num];
    WorkerArg args[worker_num];

    initialize_workers(args, request_num, db_path, use_xrp);

    clock_gettime(CLOCK_REALTIME, &start);
    start_workers(tids, args);
    terminate_workers(tids, args);
    clock_gettime(CLOCK_REALTIME, &end);

    long total_latency = 0;
    for (size_t i = 0; i < worker_num; i++) total_latency += args[i].timer;
    long run_time = 1000000000 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec);

    printf("Average throughput: %f op/s latency: %f usec\n", 
            (double)request_num / run_time * 1000000000, (double)total_latency / request_num / 1000);

    return terminate();
}

void *subtask(void *args) {
    WorkerArg *r = (WorkerArg*)args;
    struct timespec tps, tpe;
    srand(r->index);
    printf("thread %ld op_count %ld\n", r->index, r->op_count);
    for (size_t i = 0; i < r->op_count; i++) {
        key__t key = rand() % max_key;

        /* Time and execute the XRP lookup */
        clock_gettime(CLOCK_REALTIME, &tps);

        struct Query query = new_query(key);
        long retval = lookup_bpf(r->db_handler, &query);
        
        clock_gettime(CLOCK_REALTIME, &tpe);
        r->timer += 1000000000 * (tpe.tv_sec - tps.tv_sec) + (tpe.tv_nsec - tps.tv_nsec);

        /* Parse and check value from db */
        char buf[sizeof(val__t) + 1];
        buf[sizeof(val__t)] = '\0';
        memcpy(buf, query.value, sizeof(val__t));
        unsigned long long_val = strtoul(buf, NULL, 10);


        /* Check result, print errors, etc */
        if (retval < 0) {
            fprintf(stderr, "XRP pread failed with code %d\n", errno);
        } else if (query.found == 0) {
            fprintf(stderr, "Value for key %ld not found\n", key);
        } else if (key != long_val) {
            printf("Error! key: %lu val: %s thrd: %ld\n", key, buf, r->index);
        }      
    }
    return NULL;
}

void read_node(ptr__t ptr, Node *node, int db_handler) {
    checked_pread(db_handler, node, sizeof(Node), decode(ptr));
}


/* Function using the same bit fiddling that we use in the BPF function */
static void read_value_the_hard_way(int fd, char *retval, ptr__t ptr) {
    /* Base of the block containing our vale */
    ptr__t base = decode(ptr) & ~(BLK_SIZE - 1);
    char buf[BLK_SIZE];
    checked_pread(fd, buf, BLK_SIZE, (long) base);
    ptr__t offset = decode(ptr) & (BLK_SIZE - 1);
    memcpy(retval, buf + offset, sizeof(val__t));
}

/**
 * Traverses the B+ tree index and retrieves the value associated with
 * [key] from the heap, if [key] is in the database.
 * @param file_name
 * @param key
 * @return null terminated string containing the value on disk, or NULL if key not found
 */
static char *grab_value(char *file_name, unsigned long const key, int use_xrp) {
    char *const retval = malloc(sizeof(val__t) + 1);
    if (retval == NULL) {
        perror("malloc");
        exit(1);
    }
    /* Ensure we have a null at the end of the string */
    retval[sizeof(val__t)] = '\0';

    /* Open the database */
    // TODO (etm): This is never closed
    int flags = O_RDONLY;
    if (use_xrp) {
    flags = flags | O_DIRECT;
    }
    int db_fd = open(file_name, flags);
    if (db_fd < 0) {
        perror("failed to open database");
        exit(1);
    }

    if (use_xrp) {
        struct Query query = new_query(key);
        long ret = lookup_bpf(db_fd, &query);

        if (ret < 0) {
            printf("reached leaf? %ld\n", query.state_flags);
            fprintf(stderr, "read xrp failed with code %d\n", errno);
            fprintf(stderr, "%s\n", strerror(errno));
            exit(errno);
        }
        if (query.found == 0) {
            printf("reached leaf? %ld\n", query.state_flags);
            printf("result not found\n");
            exit(1);
        }
        memcpy(retval, query.value, sizeof(query.value));
    } else {
        /* Traverse b+ tree index in db to find value */
        Node *const node;
        if (posix_memalign((void **) &node, 512, sizeof(Node))) {
            perror("posix_memalign failed");
            exit(1);
        }

        checked_pread(db_fd, node, sizeof(Node), 0);
        ptr__t ptr = nxt_node(key, node);
        while (node->type != LEAF) {
            checked_pread(db_fd, node, sizeof(Node), (long) decode(ptr));
            ptr = nxt_node(key, node);
        }
        /* Verify that the key exists in this leaf node, otherwise missing key */
        if (!key_exists(key, node)) {
            free(node);
            free(retval);
            close(db_fd);
            return NULL;
        }

        /* Now we're at a leaf node, and ptr points to the log entry */
        read_value_the_hard_way(db_fd, retval, ptr);
        free(node);
    }
    close(db_fd);
    return retval;
}

static int lookup_single_key(char *filename, long key, int use_xrp) {
    /* Lookup Single Key */
    char *value = grab_value(filename, key, use_xrp);
    printf("Key: %ld\n", key);
    if (value == NULL) {
        printf("Value not found\n");
        return 1;
    }
    char *nospace = value;
    while (*nospace != '\0' && isspace((int) *nospace)) {
        ++nospace;
    }
    printf("Value %s\n", nospace);
    free(value);
    return 0;
}

static int parse_opt(int key, char *arg, struct argp_state *state) {
    struct ArgState *st = state->input;
    switch (key) {
        case 'c':
            st->create = 1;
            break;
        
        case 'x':
            st -> xrp = 1;
            break;
        
        case 'r': {
            char *endptr = NULL;
            st->requests = strtol(arg, &endptr, 10);
            if ((endptr != NULL && *endptr != '\0') || st->requests < 0) {
                argp_failure(state, 1, 0, "invalid number of requests");
            }
        }

        case 't': {
            char *endptr = NULL;
            st->threads = strtol(arg, &endptr, 10);
            if ((endptr != NULL && *endptr != '\0') || st->threads < 0) {
                argp_failure(state, 1, 0, "invalid number of threads");
            }
        }

        case 'k': {
            char *endptr = NULL;
            st->key = strtol(arg, &endptr, 10);
            st->key_set = 1;
            if (endptr != NULL && *endptr != '\0') {
                argp_failure(state, 1, 0, "invalid key");
            }
        }

        case ARGP_KEY_ARG:
        switch (state->arg_num) {
            case 0:
                st->filename = arg;
                break;

            case 1: {
                char *endptr = NULL;
                st->layers = strtol(arg, &endptr, 10);
                if ((endptr != NULL && *endptr != '\0') || st->layers < 0) {
                    argp_failure(state, 1, 0, "invalid number of layers");
                }
                break;
                }
            
            default:
                argp_failure(state, 1, 0, "too many arguments");
        }
        break;

        case ARGP_KEY_END:
            if (state->arg_num != 2) {
                argp_error(state, "too few arguments");
            }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    struct argp_option options[] = {
        { "create", 'c', 0, 0, "Create a new database with n layers." },
        { "key", 'k', "KEY", 0, "Single key to retrieve from the database."},
        { "use-xrp", 'x', 0, 0, "Use the (previously) loaded XRP BPF function to query the DB."
          " Ignored if --create is specified"
        },
        { "requests", 'r', "REQ", 0, "Number of requests to submit per thread. Ignored if -k is set." },
        { "threads" , 't', "N_THREADS", 0, "Number of concurrent threads to run. Ignored if -k is set." },
        { 0 }
    };
    struct ArgState arg_state = default_argstate();
    struct argp argp = { options, parse_opt, "DB_NAME N_LAYERS", doc };
    argp_parse(&argp, argc, argv, 0, 0, &arg_state);

    /* Create a new database */
    if (arg_state.create) {
        return load(arg_state.layers, arg_state.filename);
    }

    /* Lookup Single Key */
    if (arg_state.key_set) {
        return lookup_single_key(arg_state.filename, arg_state.key, arg_state.xrp);
    }

    /* Default: Run the SimpleKV benchmark */
    return run(arg_state.filename, arg_state.layers, arg_state.requests, arg_state.threads, arg_state.xrp);
}