#include "../include/gatebench.h"
#include "../include/gatebench_cli.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERS 1000u
#define DEFAULT_WARMUP 100u
#define DEFAULT_RUNS 5u
#define DEFAULT_ENTRIES 10u
#define DEFAULT_INTERVAL_NS 1000000ull /* 1ms */
#define DEFAULT_INDEX 1000u
#define DEFAULT_CPU -1
#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_CLOCKID CLOCK_TAI
#define DEFAULT_BASE_TIME 0ull
#define DEFAULT_CYCLE_TIME 0ull
#define DEFAULT_NLMON_IFACE "nlmon0"

static const char* usage_str =
    "Usage: gatebench [OPTIONS]\n"
    "\n"
    "Benchmark tc gate (act_gate) control-plane operations over rtnetlink.\n"
    "\n"
    "Benchmark options:\n"
    "  -i, --iters=NUM         Iterations per run (default: 1000)\n"
    "  -w, --warmup=NUM        Warmup iterations (default: 100)\n"
    "  -r, --runs=NUM          Number of runs (default: 5)\n"
    "  -e, --entries=NUM       Number of gate entries (default: 10)\n"
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
    "  --sample-every=N        Sample every N iterations (default: 0 = off)\n"
    "  --dump-proof            Run RTM_GETACTION dump proof harness (default: off)\n"
    "  --pcap=PATH             Write nlmon capture to PATH (default: off)\n"
    "  --nlmon-iface=NAME      nlmon interface for capture (default: nlmon0)\n"
    "\n"
    "Other options:\n"
    "  -h, --help              Show this help message\n"
    "  -v, --version           Show version information\n";

static const struct option long_options[] = {
    {"entries", required_argument, NULL, 'e'},
    {"iters", required_argument, NULL, 'i'},
    {"warmup", required_argument, NULL, 'w'},
    {"runs", required_argument, NULL, 'r'},
    {"interval-ns", required_argument, NULL, 'I'},
    {"index", required_argument, NULL, 'x'},
    {"cpu", required_argument, NULL, 'c'},
    {"timeout-ms", required_argument, NULL, 't'},
    {"clockid", required_argument, NULL, 256},
    {"base-time", required_argument, NULL, 257},
    {"cycle-time", required_argument, NULL, 258},
    {"sample-every", required_argument, NULL, 259},
    {"cycle-time-ext", required_argument, NULL, 260},
    {"dump-proof", no_argument, NULL, 261},
    {"pcap", required_argument, NULL, 262},
    {"nlmon-iface", required_argument, NULL, 263},
    {"selftest", no_argument, NULL, 's'},
    {"json", no_argument, NULL, 'j'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {NULL, 0, NULL, 0},
};

static void print_usage(void) {
    fputs(usage_str, stdout);
}

static void print_version(void) {
    puts("gatebench 0.1.0");
}

static int parse_u32(const char* str, uint32_t* out, const char* name) {
    char* end = NULL;
    unsigned long v;

    if (!str || !out)
        return -EINVAL;

    errno = 0;
    v = strtoul(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0' || v > UINT32_MAX) {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *out = (uint32_t)v;
    return 0;
}

static int parse_u64(const char* str, uint64_t* out, const char* name) {
    char* end = NULL;
    unsigned long long v;

    if (!str || !out)
        return -EINVAL;

    errno = 0;
    v = strtoull(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0') {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *out = (uint64_t)v;
    return 0;
}

static int parse_int(const char* str, int* out, const char* name) {
    char* end = NULL;
    long v;

    if (!str || !out)
        return -EINVAL;

    errno = 0;
    v = strtol(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        fprintf(stderr, "Error: Invalid value for %s: %s\n", name, str);
        return -EINVAL;
    }

    *out = (int)v;
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
    cfg->dump_proof = false;
    cfg->pcap_path = NULL;
    cfg->nlmon_iface = DEFAULT_NLMON_IFACE;
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
    printf("  Gate interval:      %llu ns\n", (unsigned long long)cfg->interval_ns);
    printf("  Starting index:     %u\n", cfg->index);
    printf("  CPU pinning:        %s\n", cfg->cpu >= 0 ? "yes" : "no");
    if (cfg->cpu >= 0)
        printf("  CPU:                %d\n", cfg->cpu);
    printf("  Netlink timeout:    %d ms\n", cfg->timeout_ms);
    printf("  Selftest:           %s\n", cfg->selftest ? "yes" : "no");
    printf("  JSON output:        %s\n", cfg->json ? "yes" : "no");
    printf("  Sampling:           %s\n", cfg->sample_mode ? "yes" : "no");
    if (cfg->sample_mode)
        printf("  Sample every:       %u iterations\n", cfg->sample_every);
    printf("  Dump proof:         %s\n", cfg->dump_proof ? "yes" : "no");
    if (cfg->dump_proof) {
        printf("  nlmon iface:        %s\n", cfg->nlmon_iface ? cfg->nlmon_iface : "(none)");
        printf("  pcap output:        %s\n", cfg->pcap_path ? cfg->pcap_path : "(disabled)");
    }
    printf("  Clock ID:           %u\n", cfg->clockid);
    printf("  Base time:          %llu ns\n", (unsigned long long)cfg->base_time);
    printf("  Cycle time:         %llu ns\n", (unsigned long long)cfg->cycle_time);
    printf("  Cycle time ext:     %llu ns\n", (unsigned long long)cfg->cycle_time_ext);
    printf("\n");
}

int gb_cli_parse(int argc, char* argv[], struct gb_config* cfg) {
    int opt;
    int option_index = 0;

    gb_config_init(cfg);

    while ((opt = getopt_long(argc, argv, "e:i:w:r:I:x:c:t:sjhv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'e':
                if (parse_u32(optarg, &cfg->entries, "entries") < 0)
                    return -EINVAL;
                break;
            case 'i':
                if (parse_u32(optarg, &cfg->iters, "iters") < 0)
                    return -EINVAL;
                break;
            case 'w':
                if (parse_u32(optarg, &cfg->warmup, "warmup") < 0)
                    return -EINVAL;
                break;
            case 'r':
                if (parse_u32(optarg, &cfg->runs, "runs") < 0)
                    return -EINVAL;
                break;
            case 'I':
                if (parse_u64(optarg, &cfg->interval_ns, "interval-ns") < 0)
                    return -EINVAL;
                break;
            case 'x':
                if (parse_u32(optarg, &cfg->index, "index") < 0)
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
            case 256:
                if (parse_u32(optarg, &cfg->clockid, "clockid") < 0)
                    return -EINVAL;
                break;
            case 257:
                if (parse_u64(optarg, &cfg->base_time, "base-time") < 0)
                    return -EINVAL;
                break;
            case 258:
                if (parse_u64(optarg, &cfg->cycle_time, "cycle-time") < 0)
                    return -EINVAL;
                break;
            case 260:
                if (parse_u64(optarg, &cfg->cycle_time_ext, "cycle-time-ext") < 0)
                    return -EINVAL;
                break;
            case 's':
                cfg->selftest = true;
                break;
            case 'j':
                cfg->json = true;
                break;
            case 259:
                if (parse_u32(optarg, &cfg->sample_every, "sample-every") < 0)
                    return -EINVAL;
                cfg->sample_mode = cfg->sample_every > 0;
                break;
            case 261:
                cfg->dump_proof = true;
                break;
            case 262:
                cfg->pcap_path = optarg;
                break;
            case 263:
                cfg->nlmon_iface = optarg;
                break;
            case 'h':
                print_usage();
                exit(0);
            case 'v':
                print_version();
                exit(0);
            case '?':
                return -EINVAL;
            default:
                fprintf(stderr, "Error: Unknown option\n");
                return -EINVAL;
        }
    }

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

    if (cfg->pcap_path && !cfg->dump_proof)
        cfg->dump_proof = true;

    return 0;
}
