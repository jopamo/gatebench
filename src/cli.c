#include "../include/gatebench.h"
#include "../include/gatebench_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

#define DEFAULT_ITERS 1000
#define DEFAULT_WARMUP 100
#define DEFAULT_RUNS 5
#define DEFAULT_ENTRIES 10
#define DEFAULT_INTERVAL_NS 1000000 /* 1ms */
#define DEFAULT_INDEX 1000
#define DEFAULT_CPU -1 /* No pinning */
#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_CLOCKID CLOCK_TAI
#define DEFAULT_BASE_TIME 0
#define DEFAULT_CYCLE_TIME 0

static const char* usage_str =
    "Usage: gatebench [OPTIONS]\n"
    "\n"
    "Benchmark tc gate (act_gate) control-plane operations over rtnetlink.\n"
    "\n"
    "Required options:\n"
    "  -e, --entries=NUM       Number of gate entries (default: 10)\n"
    "\n"
    "Benchmark options:\n"
    "  -i, --iters=NUM         Iterations per run (default: 1000)\n"
    "  -w, --warmup=NUM        Warmup iterations (default: 100)\n"
    "  -r, --runs=NUM          Number of runs (default: 5)\n"
    "  -I, --interval-ns=NS    Gate interval in nanoseconds (default: 1000000)\n"
    "  -x, --index=NUM         Starting index for gate actions (default: 1000)\n"
    "\n"
    "System options:\n"
    "  -c, --cpu=NUM           CPU to pin to (-1 for no pinning, default: -1)\n"
    "  -t, --timeout-ms=MS     Netlink timeout in milliseconds (default: 1000)\n"
    "\n"
    "Gate shape options:\n"
    "  --clockid=ID            Clock ID (default: CLOCK_TAI)\n"
    "  --base-time=NS          Base time for gate schedule (default: 0)\n"
    "  --cycle-time=NS         Cycle time for gate schedule (default: 0)\n"
    "  --cycle-time-ext=NS     Cycle time extension (default: 0)\n"
    "\n"
    "Mode options:\n"
    "  -s, --selftest          Run selftests before benchmark (default: off)\n"
    "  -j, --json              Output JSON format (default: off)\n"
    "  --sample-every=N        Sample every N iterations (default: 0 = no sampling)\n"
    "\n"
    "Other options:\n"
    "  -h, --help              Show this help message\n"
    "  -v, --version           Show version information\n";

static const struct option long_options[] = {{"entries", required_argument, 0, 'e'},
                                             {"iters", required_argument, 0, 'i'},
                                             {"warmup", required_argument, 0, 'w'},
                                             {"runs", required_argument, 0, 'r'},
                                             {"interval-ns", required_argument, 0, 'I'},
                                             {"index", required_argument, 0, 'x'},
                                             {"cpu", required_argument, 0, 'c'},
                                             {"timeout-ms", required_argument, 0, 't'},
                                             {"clockid", required_argument, 0, 256},
                                             {"base-time", required_argument, 0, 257},
                                             {"cycle-time", required_argument, 0, 258},
                                             {"cycle-time-ext", required_argument, 0, 260},
                                             {"selftest", no_argument, 0, 's'},
                                             {"json", no_argument, 0, 'j'},
                                             {"sample-every", required_argument, 0, 259},
                                             {"help", no_argument, 0, 'h'},
                                             {"version", no_argument, 0, 'v'},
                                             {0, 0, 0, 0}};

static void print_usage(void) {
    fputs(usage_str, stdout);
}

static void print_version(void) {
    printf("gatebench 0.1.0\n");
}

static int parse_uint32(const char* str, uint32_t* val, const char* name) {
    char* endptr;
    unsigned long tmp = strtoul(str, &endptr, 10);

    if (*endptr != '\0' || tmp > UINT32_MAX) {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *val = (uint32_t)tmp;
    return 0;
}

static int parse_uint64(const char* str, uint64_t* val, const char* name) {
    char* endptr;
    unsigned long long tmp = strtoull(str, &endptr, 10);

    if (*endptr != '\0' || tmp > UINT64_MAX) {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *val = (uint64_t)tmp;
    return 0;
}

static int parse_int(const char* str, int* val, const char* name) {
    char* endptr;
    long tmp = strtol(str, &endptr, 10);

    if (*endptr != '\0' || tmp < INT_MIN || tmp > INT_MAX) {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *val = (int)tmp;
    return 0;
}

void gb_config_init(struct gb_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->iters = DEFAULT_ITERS;
    cfg->warmup = DEFAULT_WARMUP;
    cfg->runs = DEFAULT_RUNS;
    cfg->entries = DEFAULT_ENTRIES;
    cfg->interval_ns = DEFAULT_INTERVAL_NS;
    cfg->index = DEFAULT_INDEX;
    cfg->cpu = DEFAULT_CPU;
    cfg->timeout_ms = DEFAULT_TIMEOUT_MS;
    cfg->selftest = false;
    cfg->json = false;
    cfg->sample_mode = false;
    cfg->sample_every = 0;
    cfg->clockid = DEFAULT_CLOCKID;
    cfg->base_time = DEFAULT_BASE_TIME;
    cfg->cycle_time = DEFAULT_CYCLE_TIME;
    cfg->cycle_time_ext = 0;
}

void gb_config_print(const struct gb_config* cfg) {
    printf("Configuration:\n");
    printf("  Iterations per run: %u\n", cfg->iters);
    printf("  Warmup iterations:  %u\n", cfg->warmup);
    printf("  Runs:               %u\n", cfg->runs);
    printf("  Gate entries:       %u\n", cfg->entries);
    printf("  Gate interval:      %lu ns\n", cfg->interval_ns);
    printf("  Starting index:     %u\n", cfg->index);
    printf("  CPU pinning:        %s\n", cfg->cpu >= 0 ? "yes" : "no");
    if (cfg->cpu >= 0) {
        printf("  CPU:                %d\n", cfg->cpu);
    }
    printf("  Netlink timeout:    %d ms\n", cfg->timeout_ms);
    printf("  Selftest:           %s\n", cfg->selftest ? "yes" : "no");
    printf("  JSON output:        %s\n", cfg->json ? "yes" : "no");
    printf("  Sampling:           %s\n", cfg->sample_mode ? "yes" : "no");
    if (cfg->sample_mode) {
        printf("  Sample every:       %u iterations\n", cfg->sample_every);
    }
    printf("  Clock ID:           %u\n", cfg->clockid);
    printf("  Base time:          %lu ns\n", cfg->base_time);
    printf("  Cycle time:         %lu ns\n", cfg->cycle_time);
    printf("  Cycle time ext:     %lu ns\n", cfg->cycle_time_ext);
    printf("\n");
}

int gb_cli_parse(int argc, char* argv[], struct gb_config* cfg) {
    int opt;
    int option_index = 0;

    gb_config_init(cfg);

    while ((opt = getopt_long(argc, argv, "e:i:w:r:I:x:c:t:sjhv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'e':
                if (parse_uint32(optarg, &cfg->entries, "entries") < 0)
                    return -EINVAL;
                break;

            case 'i':
                if (parse_uint32(optarg, &cfg->iters, "iters") < 0)
                    return -EINVAL;
                break;

            case 'w':
                if (parse_uint32(optarg, &cfg->warmup, "warmup") < 0)
                    return -EINVAL;
                break;

            case 'r':
                if (parse_uint32(optarg, &cfg->runs, "runs") < 0)
                    return -EINVAL;
                break;

            case 'I':
                if (parse_uint64(optarg, &cfg->interval_ns, "interval-ns") < 0)
                    return -EINVAL;
                break;

            case 'x':
                if (parse_uint32(optarg, &cfg->index, "index") < 0)
                    return -EINVAL;
                break;

            case 'c':
                if (parse_int(optarg, &cfg->cpu, "cpu") < 0)
                    return -EINVAL;
                break;

            case 't':
                if (parse_int(optarg, &cfg->timeout_ms, "timeout-ms") < 0)
                    return -EINVAL;
                if (cfg->timeout_ms <= 0) {
                    fprintf(stderr, "Error: timeout must be positive\n");
                    return -EINVAL;
                }
                break;

            case 256: /* clockid */
                if (parse_uint32(optarg, &cfg->clockid, "clockid") < 0)
                    return -EINVAL;
                break;

            case 257: /* base-time */
                if (parse_uint64(optarg, &cfg->base_time, "base-time") < 0)
                    return -EINVAL;
                break;

            case 258: /* cycle-time */
                if (parse_uint64(optarg, &cfg->cycle_time, "cycle-time") < 0)
                    return -EINVAL;
                break;

            case 260: /* cycle-time-ext */
                if (parse_uint64(optarg, &cfg->cycle_time_ext, "cycle-time-ext") < 0)
                    return -EINVAL;
                break;

            case 's':
                cfg->selftest = true;
                break;

            case 'j':
                cfg->json = true;
                break;

            case 259: /* sample-every */
                if (parse_uint32(optarg, &cfg->sample_every, "sample-every") < 0)
                    return -EINVAL;
                cfg->sample_mode = cfg->sample_every > 0;
                break;

            case 'h':
                print_usage();
                exit(0);

            case 'v':
                print_version();
                exit(0);

            case '?':
                /* getopt already printed an error */
                return -EINVAL;

            default:
                fprintf(stderr, "Error: Unknown option\n");
                return -EINVAL;
        }
    }

    /* Validate configuration */
    if (cfg->iters == 0) {
        fprintf(stderr, "Error: iterations must be positive\n");
        return -EINVAL;
    }

    if (cfg->entries == 0) {
        fprintf(stderr, "Error: entries must be positive\n");
        return -EINVAL;
    }

    if (cfg->interval_ns == 0) {
        fprintf(stderr, "Error: interval must be positive\n");
        return -EINVAL;
    }

    if (cfg->sample_mode && cfg->sample_every == 0) {
        fprintf(stderr, "Error: sample-every must be positive when sampling\n");
        return -EINVAL;
    }

    if (cfg->sample_mode && cfg->sample_every > cfg->iters) {
        fprintf(stderr, "Error: sample-every cannot exceed iterations\n");
        return -EINVAL;
    }

    return 0;
}
