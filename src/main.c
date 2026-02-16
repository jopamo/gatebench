/* src/main.c
 * Main entry point and orchestration of the benchmark tool.
 */

#include "../include/gatebench.h"
#include "../include/gatebench_cli.h"
#include "../include/gatebench_util.h"
#include "../include/gatebench_bench.h"
#include "../include/gatebench_selftest.h"
#include "../include/gatebench_proof.h"
#include "../include/gatebench_race.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

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

static void json_print_escaped_string(const char* value) {
    const unsigned char* p;

    putchar('"');
    if (!value) {
        putchar('"');
        return;
    }

    for (p = (const unsigned char*)value; *p != '\0'; p++) {
        unsigned char c = *p;

        switch (c) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\b':
                fputs("\\b", stdout);
                break;
            case '\f':
                fputs("\\f", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (c < 0x20u) {
                    printf("\\u%04x", (unsigned int)c);
                }
                else {
                    putchar((int)c);
                }
                break;
        }
    }

    putchar('"');
}

static void json_print_string_or_null(const char* value) {
    if (value) {
        json_print_escaped_string(value);
    }
    else {
        fputs("null", stdout);
    }
}

static void json_print_double(double value) {
    if (isfinite(value))
        printf("%.9g", value);
    else
        fputs("null", stdout);
}

static void json_print_environment_obj(void) {
    struct utsname uts;
    int uname_ret;
    int cpu;
    int cpu_ret;

    uname_ret = uname(&uts);
    cpu_ret = gb_util_get_cpu(&cpu);

    printf("{\n");
    printf("    \"sysname\": ");
    if (uname_ret == 0)
        json_print_escaped_string(uts.sysname);
    else
        fputs("null", stdout);
    printf(",\n");

    printf("    \"release\": ");
    if (uname_ret == 0)
        json_print_escaped_string(uts.release);
    else
        fputs("null", stdout);
    printf(",\n");

    printf("    \"machine\": ");
    if (uname_ret == 0)
        json_print_escaped_string(uts.machine);
    else
        fputs("null", stdout);
    printf(",\n");

    printf("    \"current_cpu\": ");
    if (cpu_ret == 0)
        printf("%d", cpu);
    else
        fputs("null", stdout);
    printf("\n");
    printf("  }");
}

static void json_print_config_obj(const struct gb_config* cfg) {
    printf("{\n");
    printf("    \"iters\": %" PRIu32 ",\n", cfg->iters);
    printf("    \"warmup\": %" PRIu32 ",\n", cfg->warmup);
    printf("    \"runs\": %" PRIu32 ",\n", cfg->runs);
    printf("    \"entries\": %" PRIu32 ",\n", cfg->entries);
    printf("    \"interval_ns\": %" PRIu64 ",\n", cfg->interval_ns);
    printf("    \"index\": %" PRIu32 ",\n", cfg->index);
    printf("    \"cpu\": %d,\n", cfg->cpu);
    printf("    \"timeout_ms\": %d,\n", cfg->timeout_ms);
    printf("    \"sample_mode\": %s,\n", cfg->sample_mode ? "true" : "false");
    printf("    \"sample_every\": %" PRIu32 ",\n", cfg->sample_every);
    printf("    \"dump_proof\": %s,\n", cfg->dump_proof ? "true" : "false");
    printf("    \"pcap_path\": ");
    json_print_string_or_null(cfg->pcap_path);
    printf(",\n");
    printf("    \"nlmon_iface\": ");
    json_print_string_or_null(cfg->nlmon_iface);
    printf(",\n");
    printf("    \"clockid\": %" PRIu32 ",\n", cfg->clockid);
    printf("    \"base_time\": %" PRIu64 ",\n", cfg->base_time);
    printf("    \"cycle_time\": %" PRIu64 ",\n", cfg->cycle_time);
    printf("    \"cycle_time_ext\": %" PRIu64 ",\n", cfg->cycle_time_ext);
    printf("    \"race_mode\": %s,\n", cfg->race_mode ? "true" : "false");
    printf("    \"race_seconds\": %" PRIu32 "\n", cfg->race_seconds);
    printf("  }");
}

static const char* selftest_status(bool ran, int ret) {
    if (!ran)
        return "skipped";
    if (ret < 0)
        return "hard_fail";
    if (ret > 0)
        return "soft_fail";
    return "ok";
}

static void json_print_selftests_obj(bool ran, int ret) {
    printf("{\n");
    printf("    \"ran\": %s,\n", ran ? "true" : "false");
    printf("    \"status\": ");
    json_print_escaped_string(selftest_status(ran, ret));
    printf(",\n");
    printf("    \"result_code\": ");
    if (ran)
        printf("%d", ret);
    else
        fputs("null", stdout);
    printf("\n");
    printf("  }");
}

static void json_print_benchmark_obj(const struct gb_summary* summary) {
    if (!summary || !summary->runs || summary->run_count == 0) {
        fputs("null", stdout);
        return;
    }

    printf("{\n");
    printf("    \"aggregate\": {\n");
    printf("      \"run_count\": %" PRIu32 ",\n", summary->run_count);
    printf("      \"median_ops_per_sec\": ");
    json_print_double(summary->median_ops_per_sec);
    printf(",\n");
    printf("      \"min_ops_per_sec\": ");
    json_print_double(summary->min_ops_per_sec);
    printf(",\n");
    printf("      \"max_ops_per_sec\": ");
    json_print_double(summary->max_ops_per_sec);
    printf(",\n");
    printf("      \"stddev_ops_per_sec\": ");
    json_print_double(summary->stddev_ops_per_sec);
    printf(",\n");
    printf("      \"median_p50_ns\": %" PRIu64 ",\n", summary->median_p50_ns);
    printf("      \"median_p95_ns\": %" PRIu64 ",\n", summary->median_p95_ns);
    printf("      \"median_p99_ns\": %" PRIu64 ",\n", summary->median_p99_ns);
    printf("      \"median_p999_ns\": %" PRIu64 "\n", summary->median_p999_ns);
    printf("    },\n");

    printf("    \"runs\": [\n");
    for (uint32_t i = 0; i < summary->run_count; i++) {
        const struct gb_run_result* run = &summary->runs[i];

        printf("      {\n");
        printf("        \"run\": %" PRIu32 ",\n", i + 1u);
        printf("        \"secs\": ");
        json_print_double(run->secs);
        printf(",\n");
        printf("        \"ops_per_sec\": ");
        json_print_double(run->ops_per_sec);
        printf(",\n");

        printf("        \"latency_ns\": {\n");
        printf("          \"min\": %" PRIu64 ",\n", run->min_ns);
        printf("          \"max\": %" PRIu64 ",\n", run->max_ns);
        printf("          \"mean\": ");
        json_print_double(run->mean_ns);
        printf(",\n");
        printf("          \"stddev\": ");
        json_print_double(run->stddev_ns);
        printf(",\n");
        printf("          \"p50\": %" PRIu64 ",\n", run->p50_ns);
        printf("          \"p95\": %" PRIu64 ",\n", run->p95_ns);
        printf("          \"p99\": %" PRIu64 ",\n", run->p99_ns);
        printf("          \"p999\": %" PRIu64 "\n", run->p999_ns);
        printf("        },\n");

        printf("        \"message_len_bytes\": {\n");
        printf("          \"create\": %" PRIu32 ",\n", run->create_len);
        printf("          \"replace\": %" PRIu32 ",\n", run->replace_len);
        printf("          \"delete\": %" PRIu32 "\n", run->del_len);
        printf("        },\n");

        printf("        \"sample_count\": %" PRIu32 "\n", run->sample_count);
        printf("      }%s\n", (i + 1u < summary->run_count) ? "," : "");
    }
    printf("    ]\n");
    printf("  }");
}

static void json_print_dump_proof_obj(const struct gb_dump_summary* summary) {
    if (!summary) {
        fputs("null", stdout);
        return;
    }

    printf("{\n");
    printf("    \"reply_msgs\": %" PRIu32 ",\n", summary->reply_msgs);
    printf("    \"payload_bytes\": %" PRIu64 ",\n", summary->payload_bytes);
    printf("    \"saw_done\": %s,\n", summary->saw_done ? "true" : "false");
    printf("    \"saw_error\": %s,\n", summary->saw_error ? "true" : "false");
    printf("    \"error_code\": %d,\n", summary->error_code);
    printf("    \"pcap_enabled\": %s,\n", summary->pcap_enabled ? "true" : "false");
    printf("    \"pcap_error\": %d\n", summary->pcap_error);
    printf("  }");
}

static void json_print_race_obj(const struct gb_race_summary* summary) {
    uint64_t total_ops;
    uint64_t total_errors;

    if (!summary) {
        fputs("null", stdout);
        return;
    }

    total_ops = summary->replace.ops + summary->dump.ops + summary->get.ops + summary->traffic.ops +
                summary->traffic_sync.ops + summary->basetime.ops + summary->delete_worker.ops + summary->invalid.ops;
    total_errors = summary->replace.errors + summary->dump.errors + summary->get.errors + summary->traffic.errors +
                   summary->basetime.errors + summary->delete_worker.errors + summary->invalid.errors;

    printf("{\n");
    printf("    \"duration_seconds\": %" PRIu32 ",\n", summary->duration_seconds);
    printf("    \"completed\": %s,\n", summary->completed ? "true" : "false");
    printf("    \"cpu_count\": %d,\n", summary->cpu_count);
    printf("    \"total_ops\": %" PRIu64 ",\n", total_ops);
    printf("    \"total_errors\": %" PRIu64 ",\n", total_errors);

    printf("    \"threads\": {\n");
    printf("      \"replace\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n", summary->replace.cpu,
           summary->replace.ops, summary->replace.errors);
    printf("      \"dump\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n", summary->dump.cpu,
           summary->dump.ops, summary->dump.errors);
    printf("      \"get\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n", summary->get.cpu,
           summary->get.ops, summary->get.errors);
    printf("      \"traffic\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n", summary->traffic.cpu,
           summary->traffic.ops, summary->traffic.errors);
    printf("      \"traffic_sync\": {\"cpu\": %d, \"ops\": %" PRIu64 "},\n", summary->traffic_sync.cpu,
           summary->traffic_sync.ops);
    printf("      \"basetime\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n", summary->basetime.cpu,
           summary->basetime.ops, summary->basetime.errors);
    printf("      \"delete\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "},\n",
           summary->delete_worker.cpu, summary->delete_worker.ops, summary->delete_worker.errors);
    printf("      \"invalid\": {\"cpu\": %d, \"ops\": %" PRIu64 ", \"errors\": %" PRIu64 "}\n", summary->invalid.cpu,
           summary->invalid.ops, summary->invalid.errors);
    printf("    }\n");
    printf("  }");
}

static void json_print_error_obj(const char* phase, int error_code) {
    int errnum;

    if (!phase || error_code == 0) {
        fputs("null", stdout);
        return;
    }

    errnum = error_code < 0 ? -error_code : error_code;
    printf("{\n");
    printf("    \"phase\": ");
    json_print_escaped_string(phase);
    printf(",\n");
    printf("    \"code\": %d,\n", error_code);
    printf("    \"errno\": %d,\n", errnum);
    printf("    \"message\": ");
    json_print_escaped_string(strerror(errnum));
    printf("\n");
    printf("  }");
}

static void json_print_report(const struct gb_config* cfg,
                              const char* mode,
                              bool ok,
                              bool selftests_ran,
                              int selftests_result,
                              const struct gb_summary* benchmark,
                              const struct gb_dump_summary* dump_summary,
                              const struct gb_race_summary* race_summary,
                              const char* error_phase,
                              int error_code) {
    printf("{\n");
    printf("  \"version\": \"0.1.0\",\n");
    printf("  \"mode\": ");
    json_print_escaped_string(mode ? mode : "benchmark");
    printf(",\n");
    printf("  \"ok\": %s,\n", ok ? "true" : "false");

    printf("  \"error\": ");
    json_print_error_obj(error_phase, error_code);
    printf(",\n");

    printf("  \"environment\": ");
    json_print_environment_obj();
    printf(",\n");

    printf("  \"config\": ");
    json_print_config_obj(cfg);
    printf(",\n");

    printf("  \"selftests\": ");
    json_print_selftests_obj(selftests_ran, selftests_result);
    printf(",\n");

    printf("  \"benchmark\": ");
    json_print_benchmark_obj(benchmark);
    printf(",\n");

    printf("  \"dump_proof\": ");
    json_print_dump_proof_obj(dump_summary);
    printf(",\n");

    printf("  \"race\": ");
    json_print_race_obj(race_summary);
    printf("\n");

    printf("}\n");
}

static bool argv_requests_json(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
            return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    struct gb_config cfg;
    struct gb_summary summary;
    struct gb_dump_summary dump_summary;
    struct gb_race_summary race_summary;
    const struct gb_summary* benchmark_out = NULL;
    const struct gb_dump_summary* dump_out = NULL;
    const struct gb_race_summary* race_out = NULL;
    const char* mode = "benchmark";
    const char* error_phase = NULL;
    int error_code = 0;
    bool selftests_ran = false;
    bool json_requested;
    int selftests_result = 0;
    int ret;
    int exit_code = EXIT_SUCCESS;

    json_requested = argv_requests_json(argc, argv);
    gb_config_init(&cfg);
    memset(&summary, 0, sizeof(summary));
    memset(&dump_summary, 0, sizeof(dump_summary));
    memset(&race_summary, 0, sizeof(race_summary));

    ret = gb_cli_parse(argc, argv, &cfg);
    if (ret < 0) {
        if (json_requested) {
            json_print_report(&cfg, mode, false, false, 0, NULL, NULL, NULL, "cli_parse", ret);
        }
        return EXIT_FAILURE;
    }

    if (cfg.race_mode)
        mode = "race";
    else if (cfg.dump_proof)
        mode = "dump_proof";

    if (!cfg.json) {
        if (cfg.verbose) {
            gb_config_print(&cfg);
            print_environment();
        }
    }

    if (cfg.cpu >= 0) {
        ret = gb_util_pin_cpu(cfg.cpu);
        if (ret < 0) {
            fprintf(stderr, "Failed to pin CPU: %s\n", strerror(-ret));
            error_phase = "pin_cpu";
            error_code = ret;
            exit_code = EXIT_FAILURE;
            goto out;
        }
        if (!cfg.json)
            printf("Pinned to CPU %d\n\n", cfg.cpu);
    }

    if (cfg.race_mode) {
        if (!cfg.json)
            printf("Running race mode for %" PRIu32 " seconds...\n", cfg.race_seconds);

        ret = cfg.json ? gb_race_run_with_summary(&cfg, &race_summary) : gb_race_run(&cfg);
        if (ret < 0) {
            fprintf(stderr, "Race mode failed: %s (%d)\n", strerror(-ret), ret);
            error_phase = "race";
            error_code = ret;
            exit_code = EXIT_FAILURE;
        }

        if (cfg.json)
            race_out = &race_summary;
        goto out;
    }

    if (!cfg.json) {
        if (cfg.verbose)
            printf("Running selftests...\n");
        else
            printf("Selftests:\n");
    }

    selftests_ran = true;
    ret = gb_selftest_run(&cfg);
    selftests_result = ret;
    if (ret < 0) {
        fprintf(stderr, "Selftests failed: %s (%d)\n", strerror(-ret), ret);
        error_phase = "selftests";
        error_code = ret;
        exit_code = EXIT_FAILURE;
        goto out;
    }

    if (!cfg.json) {
        if (ret == 0)
            printf("Selftests: OK\n\n");
        else
            printf("Selftests: WARN (soft-failures)\n\n");
    }

    if (cfg.dump_proof) {
        if (!cfg.json)
            printf("Running dump proof harness...\n");

        ret = gb_proof_run(&cfg, &dump_summary);
        dump_out = &dump_summary;
        if (ret < 0) {
            fprintf(stderr, "Dump proof failed: %s (%d)\n", strerror(-ret), ret);
            error_phase = "dump_proof";
            error_code = ret;
            exit_code = EXIT_FAILURE;
        }

        if (!cfg.json) {
            gb_proof_print_summary(&dump_summary, &cfg);
            printf("\n");
        }

        goto out;
    }

    if (!cfg.json)
        printf("Running benchmark...\n");

    ret = gb_bench_run(&cfg, &summary);
    if (ret < 0) {
        fprintf(stderr, "Benchmark run failed: %s (%d)\n", strerror(-ret), ret);
        error_phase = "benchmark";
        error_code = ret;
        exit_code = EXIT_FAILURE;
        goto out;
    }

    benchmark_out = &summary;

    if (!cfg.json)
        printf("Benchmark completed successfully\n");

out:
    if (cfg.json) {
        json_print_report(&cfg, mode, exit_code == EXIT_SUCCESS, selftests_ran, selftests_result, benchmark_out,
                          dump_out, race_out, error_phase, error_code);
    }

    gb_summary_free(&summary);
    return exit_code;
}

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
