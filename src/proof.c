/* src/proof.c
 * Verification and proof-of-correctness routines.
 */
#include "../include/gatebench_proof.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include "bench_internal.h"
#include <errno.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GATEBENCH_HAVE_PCAP
#include <pcap/pcap.h>
#include <pthread.h>
#endif

struct gb_pcap_ctx {
#ifdef GATEBENCH_HAVE_PCAP
    pcap_t* handle;
    pcap_dumper_t* dumper;
    pthread_t thread;
    int running;
#endif
};

#ifdef GATEBENCH_HAVE_PCAP
static void* pcap_thread_main(void* arg) {
    struct gb_pcap_ctx* ctx = arg;

    pcap_loop(ctx->handle, -1, pcap_dump, (u_char*)ctx->dumper);
    return NULL;
}

static int gb_pcap_start(struct gb_pcap_ctx* ctx, const char* iface, const char* path) {
    char errbuf[PCAP_ERRBUF_SIZE];
    int ret;

    if (!ctx || !iface || !path)
        return -EINVAL;

    memset(ctx, 0, sizeof(*ctx));
    memset(errbuf, 0, sizeof(errbuf));

    ctx->handle = pcap_open_live(iface, 65535, 0, 100, errbuf);
    if (!ctx->handle) {
        fprintf(stderr, "pcap_open_live(%s) failed: %s\n", iface, errbuf);
        return -EIO;
    }

    ctx->dumper = pcap_dump_open(ctx->handle, path);
    if (!ctx->dumper) {
        fprintf(stderr, "pcap_dump_open(%s) failed: %s\n", path, pcap_geterr(ctx->handle));
        pcap_close(ctx->handle);
        ctx->handle = NULL;
        return -EIO;
    }

    ret = pthread_create(&ctx->thread, NULL, pcap_thread_main, ctx);
    if (ret != 0) {
        fprintf(stderr, "pcap capture thread create failed: %s\n", strerror(ret));
        pcap_dump_close(ctx->dumper);
        pcap_close(ctx->handle);
        ctx->dumper = NULL;
        ctx->handle = NULL;
        return -ret;
    }

    ctx->running = 1;
    return 0;
}

static void gb_pcap_stop(struct gb_pcap_ctx* ctx) {
    if (!ctx || !ctx->running)
        return;

    pcap_breakloop(ctx->handle);
    pthread_join(ctx->thread, NULL);
    pcap_dump_flush(ctx->dumper);
    pcap_dump_close(ctx->dumper);
    pcap_close(ctx->handle);

    ctx->dumper = NULL;
    ctx->handle = NULL;
    ctx->running = 0;
}
#else
static int gb_pcap_start(struct gb_pcap_ctx* ctx, const char* iface, const char* path) {
    (void)ctx;
    (void)iface;
    (void)path;
    return -ENOTSUP;
}

static void gb_pcap_stop(struct gb_pcap_ctx* ctx) {
    (void)ctx;
}
#endif

int gb_proof_run(const struct gb_config* cfg, struct gb_dump_summary* summary) {
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* create_msg = NULL;
    struct gb_nl_msg* del_msg = NULL;
    struct gb_nl_msg* dump_msg = NULL;
    struct gate_shape shape;
    struct gate_entry* entries = NULL;
    struct gb_dump_stats dump_stats;
    struct gb_pcap_ctx pcap_ctx;
    size_t create_cap;
    uint32_t entry_count;
    int ret;

    if (!cfg || !summary)
        return -EINVAL;

    memset(summary, 0, sizeof(*summary));
    memset(&pcap_ctx, 0, sizeof(pcap_ctx));

    ret = gb_nl_open(&sock);
    if (ret < 0)
        return ret;

    entry_count = cfg->entries;
    if (entry_count > GB_MAX_ENTRIES)
        entry_count = GB_MAX_ENTRIES;

    if (entry_count > 0) {
        entries = malloc((size_t)entry_count * sizeof(*entries));
        if (!entries) {
            ret = -ENOMEM;
            goto out;
        }

        ret = gb_fill_entries(entries, entry_count, cfg->interval_ns);
        if (ret < 0)
            goto out;
    }

    memset(&shape, 0, sizeof(shape));
    shape.clockid = cfg->clockid;
    shape.base_time = cfg->base_time;
    shape.cycle_time = cfg->cycle_time;
    shape.cycle_time_ext = cfg->cycle_time_ext;
    shape.interval_ns = cfg->interval_ns;
    shape.entries = entry_count;

    create_cap = gate_msg_capacity(entry_count, 0);
    create_msg = gb_nl_msg_alloc(create_cap);
    del_msg = gb_nl_msg_alloc(1024);
    dump_msg = gb_nl_msg_alloc(1024);
    if (!create_msg || !del_msg || !dump_msg) {
        ret = -ENOMEM;
        goto out;
    }

    gb_nl_msg_reset(del_msg);
    ret = build_gate_delaction(del_msg, cfg->index);
    if (ret < 0)
        goto out;
    ret = gb_nl_send_recv(sock, del_msg, dump_msg, cfg->timeout_ms);
    if (ret < 0 && ret != -ENOENT)
        goto out;

    gb_nl_msg_reset(create_msg);
    ret = build_gate_newaction(create_msg, cfg->index, &shape, entries, entry_count, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0)
        goto out;

    ret = gb_nl_send_recv(sock, create_msg, dump_msg, cfg->timeout_ms);
    if (ret < 0)
        goto out;

    gb_nl_msg_reset(dump_msg);
    ret = build_gate_getaction_ex(dump_msg, cfg->index, NLM_F_DUMP);
    if (ret < 0)
        goto out;

    if (cfg->pcap_path) {
        ret = gb_pcap_start(&pcap_ctx, cfg->nlmon_iface, cfg->pcap_path);
        if (ret < 0) {
            if (ret == -ENOTSUP)
                fprintf(stderr, "pcap support not built; rebuild with -Dpcap=enabled\n");
            summary->pcap_error = ret;
            goto out;
        }
        summary->pcap_enabled = true;
    }

    memset(&dump_stats, 0, sizeof(dump_stats));
    ret = gb_nl_dump_action(sock, dump_msg, &dump_stats, cfg->timeout_ms);
    summary->reply_msgs = dump_stats.reply_msgs;
    summary->payload_bytes = dump_stats.payload_bytes;
    summary->saw_done = dump_stats.saw_done;
    summary->saw_error = dump_stats.saw_error;
    summary->error_code = dump_stats.error_code;

    if (summary->saw_error)
        ret = summary->error_code;

out:
    if (summary->pcap_enabled)
        gb_pcap_stop(&pcap_ctx);

    if (sock && del_msg && dump_msg) {
        gb_nl_msg_reset(del_msg);
        if (build_gate_delaction(del_msg, cfg->index) >= 0)
            gb_nl_send_recv(sock, del_msg, dump_msg, cfg->timeout_ms);
    }

    free(entries);
    if (create_msg)
        gb_nl_msg_free(create_msg);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    if (dump_msg)
        gb_nl_msg_free(dump_msg);
    if (sock)
        gb_nl_close(sock);

    return ret;
}

void gb_proof_print_summary(const struct gb_dump_summary* summary, const struct gb_config* cfg) {
    if (!summary || !cfg)
        return;

    printf("Dump proof summary:\n");
    printf("  Multipart reply messages: %u\n", summary->reply_msgs);
    printf("  NLMSG_DONE seen:          %s\n", summary->saw_done ? "yes" : "no");
    if (summary->saw_error) {
        printf("  NLMSG_ERROR:              yes (%d)\n", summary->error_code);
    }
    else {
        printf("  NLMSG_ERROR:              no\n");
    }
    printf("  Reply payload bytes:      %llu\n", (unsigned long long)summary->payload_bytes);
    if (cfg->pcap_path) {
        if (summary->pcap_error < 0)
            printf("  pcap capture:             failed (%d)\n", summary->pcap_error);
        else
            printf("  pcap capture:             %s (iface %s)\n", cfg->pcap_path,
                   cfg->nlmon_iface ? cfg->nlmon_iface : "(null)");
    }
}
