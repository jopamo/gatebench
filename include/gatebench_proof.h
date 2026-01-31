#ifndef GATEBENCH_PROOF_H
#define GATEBENCH_PROOF_H

#include "gatebench.h"
#include <stdbool.h>
#include <stdint.h>

struct gb_dump_summary {
    uint32_t reply_msgs;
    uint64_t payload_bytes;
    bool saw_done;
    bool saw_error;
    int error_code;
    bool pcap_enabled;
    int pcap_error;
};

int gb_proof_run(const struct gb_config* cfg, struct gb_dump_summary* summary);
void gb_proof_print_summary(const struct gb_dump_summary* summary, const struct gb_config* cfg);

#endif /* GATEBENCH_PROOF_H */
