#ifndef SCPI_SCPI_H
#define SCPI_SCPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCPI_RES_OK 0
#define SCPI_RES_ERR -1
#define SCPI_CMD_LIST_END { NULL, NULL, 0 }

typedef struct scpi_context scpi_t;
typedef int scpi_result_t;
typedef scpi_result_t (*scpi_command_callback_t)(scpi_t *ctx);
typedef int (*scpi_write_t)(void *user, const char *data, size_t len);

typedef struct {
    const char *pattern;
    scpi_command_callback_t callback;
    int tag;
} scpi_command_t;

typedef struct {
    int code;
    char message[48];
} scpi_error_t;

struct scpi_context {
    scpi_write_t write;
    void *user;

    const scpi_command_t *tables[8];
    size_t table_count;

    char input[128];
    size_t input_len;

    scpi_error_t errors[8];
    size_t error_head;
    size_t error_count;

    void *user_context;

    const char *args;
};

void SCPI_Init(scpi_t *ctx, scpi_write_t write, void *user);
int SCPI_RegisterCommands(scpi_t *ctx, const scpi_command_t *commands);
int SCPI_Input(scpi_t *ctx, const char *data, size_t len);

int SCPI_ResultText(scpi_t *ctx, const char *value);
int SCPI_ResultUInt32(scpi_t *ctx, uint32_t value);
int SCPI_ResultInt32(scpi_t *ctx, int32_t value);
int SCPI_ResultBool(scpi_t *ctx, int value);
int SCPI_ResultArbitraryBlock(scpi_t *ctx, const uint8_t *data, size_t len);
int SCPI_ParamUInt32(scpi_t *ctx, uint32_t *val, int mandatory);
void SCPI_ErrorPush(scpi_t *ctx, int code, const char *message);
int SCPI_ErrorPop(scpi_t *ctx, scpi_error_t *out);
void SCPI_ErrorClear(scpi_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
