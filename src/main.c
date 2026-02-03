/* src/main.c
 * Main entry point and orchestration of the benchmark tool.
 */

#include "../include/gatebench.h"
#include "../include/gatebench_cli.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_stats.h"
#include "../include/gatebench_util.h"
#include "../include/gatebench_bench.h"
#include "../include/gatebench_selftest.h"
#include "../include/gatebench_proof.h"
#include "../include/gatebench_race.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sched.h>

/* Forward declarations for functions that will be implemented in other files */
extern int gb_bench_run(const struct gb_config* cfg, struct gb_summary* summary);

static void print_environment(void) {
    struct utsname uts;
    int cpu;
    int ret;
    int uname_ret;

    printf("Environment:\n");

    uname_ret = uname(&uts);
    if (uname_ret == 0) {
        printf("  Kernel: %s %s %s\n", uts.sysname, uts.release, uts.machine);
    }
    else {
        int err = errno;

        fprintf(stderr, "Failed to uname: %s (%d)\n", strerror(err), err);
        printf("  Kernel: unknown\n");
    }

    /* Try to get current CPU */
    ret = gb_util_get_cpu(&cpu);
    if (ret == 0) {
        printf("  Current CPU: %d\n", cpu);
    }
    else {
        fprintf(stderr, "Failed to get current CPU: %s\n", strerror(-ret));
    }

    printf("  Clock source: CLOCK_MONOTONIC_RAW\n");
    printf("\n");
}

static void print_json_header(const struct gb_config* cfg) {
    struct utsname uts;
    int cpu;
    int ret;
    int uname_ret;

    printf("{\n");
    printf("  \"version\": \"0.1.0\",\n");

    uname_ret = uname(&uts);
    if (uname_ret < 0) {
        int err = errno;

        fprintf(stderr, "Failed to uname: %s (%d)\n", strerror(err), err);
    }
    printf("  \"environment\": {\n");
    if (uname_ret == 0) {
        printf("    \"sysname\": \"%s\",\n", uts.sysname);
        printf("    \"release\": \"%s\",\n", uts.release);
        printf("    \"machine\": \"%s\"\n", uts.machine);
    }
    else {
        printf("    \"sysname\": null,\n");
        printf("    \"release\": null,\n");
        printf("    \"machine\": null\n");
    }
    printf("  },\n");

    ret = gb_util_get_cpu(&cpu);
    if (ret < 0) {
        fprintf(stderr, "Failed to get current CPU: %s\n", strerror(-ret));
        cpu = -1;
    }
    printf("  \"current_cpu\": %d,\n", cpu);

    printf("  \"config\": {\n");
    printf("    \"iters\": %u,\n", cfg->iters);
    printf("    \"warmup\": %u,\n", cfg->warmup);
    printf("    \"runs\": %u,\n", cfg->runs);
    printf("    \"entries\": %u,\n", cfg->entries);
    printf("    \"interval_ns\": %lu,\n", cfg->interval_ns);
    printf("    \"index\": %u,\n", cfg->index);
    printf("    \"cpu\": %d,\n", cfg->cpu);
    printf("    \"timeout_ms\": %d,\n", cfg->timeout_ms);
    printf("    \"sample_mode\": %s,\n", cfg->sample_mode ? "true" : "false");
    printf("    \"sample_every\": %u,\n", cfg->sample_every);
    printf("    \"dump_proof\": %s,\n", cfg->dump_proof ? "true" : "false");
    if (cfg->pcap_path)
        printf("    \"pcap_path\": \"%s\",\n", cfg->pcap_path);
    else
        printf("    \"pcap_path\": null,\n");
    if (cfg->nlmon_iface)
        printf("    \"nlmon_iface\": \"%s\",\n", cfg->nlmon_iface);
    else
        printf("    \"nlmon_iface\": null,\n");
    printf("    \"clockid\": %u,\n", cfg->clockid);
    printf("    \"base_time\": %lu,\n", cfg->base_time);
    printf("    \"cycle_time\": %lu,\n", cfg->cycle_time);
    printf("    \"race_mode\": %s,\n", cfg->race_mode ? "true" : "false");
    printf("    \"race_seconds\": %u\n", cfg->race_seconds);
    printf("  },\n");
}

static void print_json_footer(void) {
    printf("}\n");
}

int main(int argc, char* argv[]) {
    struct gb_config cfg;
    struct gb_nl_sock* sock = NULL;
    struct gb_summary summary;
    int ret = 0;

    /* Parse command line arguments */
    ret = gb_cli_parse(argc, argv, &cfg);
    if (ret < 0) {
        return EXIT_FAILURE;
    }

    /* Print configuration */
    if (!cfg.json) {
        gb_config_print(&cfg);
        print_environment();
    }
    else {
        print_json_header(&cfg);
    }

    /* Pin CPU if requested */
    if (cfg.cpu >= 0) {
        ret = gb_util_pin_cpu(cfg.cpu);
        if (ret < 0) {
            fprintf(stderr, "Failed to pin CPU: %s\n", strerror(-ret));
            return EXIT_FAILURE;
        }
        if (!cfg.json) {
            printf("Pinned to CPU %d\n\n", cfg.cpu);
        }
    }

    if (cfg.race_mode) {
        if (!cfg.json) {
            printf("Running race mode for %u seconds...\n", cfg.race_seconds);
        }

        ret = gb_race_run(&cfg);
        if (ret < 0) {
            fprintf(stderr, "Race mode failed: %s (%d)\n", strerror(-ret), ret);
        }

        if (cfg.json) {
            print_json_footer();
        }

        return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    /* Open netlink socket */
    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return EXIT_FAILURE;
    }

    /* Run selftests before benchmark */
    if (!cfg.json) {
        printf("Running selftests...\n");
    }

    ret = gb_selftest_run(&cfg);
    if (ret < 0) {
        fprintf(stderr, "Selftests failed: %s (%d)\n", strerror(-ret), ret);
        gb_nl_close(sock);
        return EXIT_FAILURE;
    }

    if (!cfg.json) {
        if (ret == 0)
            printf("Selftests passed\n\n");
        else
            printf("Selftests completed with warnings\n\n");
    }

    if (cfg.dump_proof) {
        struct gb_dump_summary dump_summary;

        if (!cfg.json) {
            printf("Running dump proof harness...\n");
        }

        ret = gb_proof_run(&cfg, &dump_summary);
        if (ret < 0) {
            fprintf(stderr, "Dump proof failed: %s (%d)\n", strerror(-ret), ret);
        }
        if (!cfg.json) {
            gb_proof_print_summary(&dump_summary, &cfg);
            printf("\n");
        }
        else {
            print_json_footer();
        }

        gb_nl_close(sock);
        return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    /* Run benchmark */
    if (!cfg.json) {
        printf("Running benchmark...\n");
    }

    memset(&summary, 0, sizeof(summary));
    ret = gb_bench_run(&cfg, &summary);
    if (ret < 0) {
        fprintf(stderr, "Benchmark run failed: %s (%d)\n", strerror(-ret), ret);
        gb_nl_close(sock);
        gb_summary_free(&summary);
        return EXIT_FAILURE;
    }

    /* Print results */
    if (!cfg.json) {
        /* TODO: Implement human-readable output */
        printf("Benchmark completed successfully\n");
    }
    else {
        /* TODO: Implement JSON output */
        print_json_footer();
    }

    /* Cleanup */
    gb_summary_free(&summary);
    gb_nl_close(sock);

    return EXIT_SUCCESS;
}

/* Implement stub functions that will be replaced by actual implementations */
void gb_run_result_free(struct gb_run_result* result) {
    if (result && result->samples) {
        free(result->samples);
        result->samples = NULL;
    }
}

void gb_summary_free(struct gb_summary* summary) {
    if (summary) {
        if (summary->runs) {
            for (uint32_t i = 0; i < summary->run_count; i++) {
                gb_run_result_free(&summary->runs[i]);
            }
            free(summary->runs);
            summary->runs = NULL;
        }
        summary->run_count = 0;
    }
}
