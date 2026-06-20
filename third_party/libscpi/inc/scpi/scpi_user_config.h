#ifndef SCPI_USER_CONFIG_H
#define SCPI_USER_CONFIG_H

/*
 * libscpi build-time configuration for iotsploit-usb.
 * Activated by compiling with -DSCPI_USER_CONFIG=1 (config.h then includes us).
 *
 * Goals: fully static (no malloc), LF line endings to match the previous
 * scpi_compat behaviour, standard error-code message table.
 */

#define USE_MEMORY_ALLOCATION_FREE              1   /* no malloc/free anywhere   */
#define USE_DEVICE_DEPENDENT_ERROR_INFORMATION  0   /* drop custom error strings */
#define USE_FULL_ERROR_LIST                     1   /* standard "-222,..." text  */
#define USE_COMMAND_TAGS                        1

/* Match the legacy parser: terminate responses with a bare '\n', not CRLF. */
#define SCPI_LINE_ENDING                        LINE_ENDING_LF

#endif /* SCPI_USER_CONFIG_H */
