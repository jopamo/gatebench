/* include/gatebench_cli.h
 * Public API for CLI parsing.
 */
#ifndef GATEBENCH_CLI_H
#define GATEBENCH_CLI_H

#include "gatebench.h"

/* Parse command line arguments */
int gb_cli_parse(int argc, char* argv[], struct gb_config* cfg);

/* Print configuration */
void gb_config_print(const struct gb_config* cfg);

#endif /* GATEBENCH_CLI_H */