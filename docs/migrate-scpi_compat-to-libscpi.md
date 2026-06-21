# 迁移方案:`scpi_compat` → `libscpi`(j123b567/scpi-parser）

> 目标:把仓库自研的精简 SCPI 解析器 `src/scpi_compat.c` 替换为成熟的
> [libscpi](https://github.com/j123b567/scpi-parser),在**不改变 USBTMC 传输层、不改变
> host 交互方式**的前提下,获得完整 IEEE 488.2 语义、丰富参数解析、数字后缀/通道列表等能力。

---

## 1. 目标与范围

### 要做
- 用 libscpi 替换 `scpi_compat.c` 作为 SCPI 命令解析/分发/响应格式化层。
- 保持对外行为:`/dev/usbtmc`、pyvisa `USB0::0x1209::0x0001::INSTR` 照常工作。
- 业务命令(`GPIO/ADC/WLAN/BLE/DATA`)语义不变,host 脚本(`scan_test.py`)无需改动或仅微调。

### 不做(保持不变)
- USBTMC 封帧与 TinyUSB glue 的 IN/OUT 路径(`glue/usbscpi_tinyusb.c` 的 512B `s_tx_buf` 缓冲、`tud_usbtmc_*` 回调)。
- 异步扫描架构(触发 → 轮询 `:DONE?` → 分行 `? <i>` 取)。
- 静态分配原则(MCU 无 malloc)。

### 衡量成功
1. `*IDN?` / `SYST:ERR?` / `WLAN:SCAN*` / `BLE:SCAN*` / `DATA:READ?` 在硬件上行为与现状一致。
2. 单元测试(`tests/`)全绿。
3. esp32s3 与 pico2 两个 example 均能编译、运行。

---

## 2. 现状分析:耦合点在哪里

迁移的难点**不在命令回调**(那部分 API 是照 libscpi 抄的,几乎能原样移植),而在
`usbscpi` core 直接“伸手进” `scpi_compat` 的结构体内部。逐一列出:

| # | 耦合点 | 位置 | libscpi 下如何处理 |
|---|---|---|---|
| C1 | `scpi_owner()` 读 `scpi->user_context` | `src/usbscpi.c:34` | ✅ libscpi 的 `scpi_t` 也有 `user_context`,直接用 |
| C2 | `cmd_syst_help_head` 遍历 `scpi->tables[]` / `table_count` | `src/usbscpi.c:88` | ❗ libscpi 是**单一 `cmdlist`**,需改用 `context->cmdlist` 遍历 |
| C3 | `cmd_syst_err_count` 读 `scpi->error_count` | `src/usbscpi.c:185` | ➡️ 改用 libscpi 内建 `SCPI_SystemErrorCountQ` |
| C4 | **多命令表**注册(`tables[8]` + `SCPI_RegisterCommands` 追加) | `src/usbscpi.c:296,300` | ❗ libscpi `SCPI_Init` 只接收**一个** `commands[]`;需合并 core+user 命令表 |
| C5 | 自己的 **RX 字节状态机** + `DATA:WRITE #NN` 块流式路径 | `src/usbscpi.c:307-412` | ⚠️ 保留状态机,仅把“文本行”交给 `SCPI_Input`;块路径继续走 `on_block_data` |
| C6 | `SCPI_Init(write, user)` 二参签名 | `src/usbscpi.c:294` | ❗ libscpi `SCPI_Init` 是 12 参(commands/interface/units/idn×4/input buf/error queue) |
| C7 | `SCPI_ErrorPush(ctx, code, "msg")` 带自定义消息 | 各处 | ➡️ 改 `SCPI_ErrorPush(ctx, code)`(标准码自带消息),自定义消息用 `SCPI_ErrorPushEx` |
| C8 | `SCPI_ResultText` 每次追加 `\n`(多结果各占一行) | `src/scpi_compat.c:199` | ⚠️ libscpi 多结果**自动逗号分隔**、整体末尾换行——影响 `SYST:HELP:HEAD?` |

> 结论:**core(`usbscpi.c`)是主战场**;业务回调虽不碰内部结构体,但**每个返回结果的回调都要做
> 两处机械改写**(返回值语义 + `ResultText` 引号,见 §5 ⚠️ 框),不是"零改动";glue 几乎不动。

---

## 3. libscpi 选型与集成

### 3.1 版本与获取
- 采用 libscpi **v2.x**,且**必须 pin 到具体 tag/commit**(如 `v2.3` 或某 commit SHA),
  不要写 `master`——本方案依赖具体 API 行为(返回值语义、`ResultText` 引号、config 机制),
  浮动 `master` 会导致 vendoring 不可复现。把选定的 SHA 记录在本文件与 vendoring 目录的 `VERSION` 文件里。
- 集成方式三选一(推荐 B):
  - **A. git submodule**:`third_party/scpi-parser`,可追溯版本,但克隆需 `--recursive`。
  - **B. 复制源码(vendor)** ✅ 推荐:把 `libscpi/src/*.c` 与 `libscpi/inc/scpi/*.h` 拷进
    `third_party/libscpi/`,锁定版本、离线可编译,和现仓库“单文件 vendoring”风格一致。
  - **C. ESP-IDF managed component**:仅 esp32s3 方便,pico 端仍需自行 vendoring,故不统一。

### 3.2 需要纳入编译的源文件(libscpi/src)
```
error.c  fifo.c  ieee488.c  minimal.c  parser.c
units.c  utils.c  expression.c  lexer.c
```
头文件目录 `libscpi/inc/`(提供 `scpi/scpi.h` 等)。

### 3.3 编译期配置(用 libscpi 官方机制,**不能**随便命名一个头就生效)
libscpi 的 `inc/scpi/config.h` 只在**定义了宏 `SCPI_USER_CONFIG` 时**才 `#include "scpi_user_config.h"`。
因此正确做法是**两步**:
1. 新增头文件,文件名必须是 **`scpi_user_config.h`**(放到 include path 上,如 `third_party/libscpi/inc/`);
2. 全局编译选项加 **`-DSCPI_USER_CONFIG=1`**(CMake 里对 libscpi 目标 `target_compile_definitions`)。

`scpi_user_config.h` 内容(为 MCU 全静态、无 malloc):
```c
#define USE_MEMORY_ALLOCATION_FREE  1   /* 全静态,无 malloc */
#define USE_FULL_ERROR_LIST         1   /* 提供 -222 等标准码消息文本 */
#define USE_COMMAND_TAGS            1
/* ❌ 不要开 USE_DEVICE_DEPENDENT_ERROR_INFORMATION:
 *    它在 MEMORY_ALLOCATION_FREE 下仍走 strndup/free,违反"无 malloc"。
 *    => 放弃自定义错误消息文本,只用标准错误码(见 §5 / §7.2)。 */
/* 若不用单位/表达式,可不纳入 units.c/expression.c 以省 flash。 */
```
> 验证:编译后确认 `config.h` 里读到的就是本文件的宏(可临时 `#error` 探针);并用 `nm` 检查最终
> 固件**不含 `malloc/free/strndup`** 符号,坐实"无动态分配"。

---

## 4. 架构改造设计

### 4.1 新的 `struct usbscpi`(`src/usbscpi.c`)
```c
struct usbscpi {
    usbscpi_config_t cfg;
    scpi_t           scpi;                 /* libscpi 上下文 */
    scpi_interface_t itf;                  /* write/error/control/reset/flush */
    char             scpi_input[SCPI_INPUT_BUFFER_LEN];   /* 给 libscpi 的输入缓冲 */
    scpi_error_t     err_queue[SCPI_ERR_QUEUE_LEN];        /* 静态错误队列 */
    scpi_command_t   cmd_merged[USBSCPI_MAX_CMDS];         /* core+user 合并后的命令表(C4) */
    size_t           cmd_count;
    /* —— 原有 RX 块状态机字段保留(C5)—— */
    rx_mode_t mode; size_t line_len, block_len, block_received;
    unsigned ndigits, got_digits; int block_started;
};
```

### 4.2 interface 回调(把 libscpi 接到现有路径)
```c
static size_t scpi_write_cb(scpi_t *ctx, const char *data, size_t len) {
    usbscpi_t *u = ctx->user_context;
    return u->cfg.usb_tx(u->cfg.user, (const uint8_t*)data, len, true) == 0 ? len : 0;
}
static scpi_result_t scpi_flush_cb(scpi_t *ctx){ (void)ctx; return SCPI_RES_OK; }
static int scpi_error_cb(scpi_t *ctx, int_fast16_t err){ (void)ctx;(void)err; return 0; }
static scpi_result_t scpi_control_cb(scpi_t *ctx, scpi_ctrl_name_t name, scpi_reg_val_t val){
    if (name == SCPI_CTRL_SRQ) usbscpi_tinyusb_set_srq();   /* 复用 glue 的 SRQ(可选) */
    return SCPI_RES_OK;
}
static scpi_result_t scpi_reset_cb(scpi_t *ctx){
    usbscpi_t *u = ctx->user_context;
    if (u->cfg.on_reset) u->cfg.on_reset(u->cfg.user);
    return SCPI_RES_OK;
}
```

### 4.3 init 改造(C6)
```c
usbscpi_t *usbscpi_init(void *storage, size_t len, const usbscpi_config_t *cfg) {
    usbscpi_t *u = storage; memset(u, 0, sizeof *u); u->cfg = *cfg;
    u->itf = (scpi_interface_t){ .write=scpi_write_cb, .error=scpi_error_cb,
                                 .control=scpi_control_cb, .flush=scpi_flush_cb,
                                 .reset=scpi_reset_cb };
    /* 先放入 core 自定义命令(SYST:CAP?/HELP/DATA:*),user 命令在 register 时追加 */
    usbscpi_merge_core_commands(u);
    /* IDN 用 NULL 占位:不依赖 libscpi 内建 SCPI_CoreIdnQ 的 4 字段拆分,
     * 改保留自定义 *IDN? 回调,直接吐 cfg.idn 整串(见下) */
    SCPI_Init(&u->scpi, u->cmd_merged, &u->itf, scpi_units_def,
              NULL, NULL, NULL, NULL,
              u->scpi_input, sizeof u->scpi_input,
              u->err_queue, SCPI_ERR_QUEUE_LEN);
    u->scpi.user_context = u;
    return u;
}
```

> **`*IDN?` 处理(C6 / Medium 反馈)**:当前公共 API 是**单串** `cfg.idn`(tests / minimal_host / pico2
> 各传各的),而 libscpi 内建 `SCPI_CoreIdnQ` 要 4 个独立字段。为避免拆串、保持各 example 配置不变,
> **保留自定义 `*IDN?` 回调**,原样回 `cfg.idn`:
> ```c
> static scpi_result_t cmd_idn(scpi_t *ctx) {
>     usbscpi_t *u = ctx->user_context;
>     const char *s = u->cfg.idn ? u->cfg.idn : "usbscpi,component,0,0.1.0";
>     SCPI_ResultCharacters(ctx, s, strlen(s));   /* 裸串,不加引号 */
>     return SCPI_RES_OK;
> }
> ```
> 把它放进 `cmd_merged[]`,**不**注册 `SCPI_CoreIdnQ`。

### 4.4 命令表合并(C4)
libscpi 只接收一个数组,因此 `usbscpi_register()` 不能再“追加一张表”,而是**把 user 命令
拷进 `cmd_merged[]` 末尾、重新 `SCPI_Init` 或就地扩展**。推荐做法:
- `usbscpi_init` 先填入 core 命令 + 一个占位 `SCPI_CMD_LIST_END`。
- `usbscpi_register(u, user_cmds)`:把 `user_cmds` 复制到 `cmd_merged` 末尾、更新结尾哨兵,
  再调用 `SCPI_Init`(libscpi 允许重新 init)或仅更新 `u->scpi.cmdlist` 指针。

> core 命令里:`*CLS / *RST / *OPC? / SYST:ERR? / SYST:ERR:COUNT?` **改用 libscpi 内建**
> (`SCPI_CoreCls` / `SCPI_CoreRst` / `SCPI_CoreOpcQ` / `SCPI_SystemErrorNextQ` / `SCPI_SystemErrorCountQ`);
> `*IDN?`(回 `cfg.idn`,见 §4.3)、`SYST:CAP? / SYST:HELP:HEAD? / DATA:FREE? / DATA:COUNT? / DATA:READ?`
> **保留自研回调**(都改成 `SCPI_ResultCharacters/...; return SCPI_RES_OK;` 形式)。

### 4.5 RX 路径(C5)——状态机保留,只换“喂给谁”
`usbscpi_on_rx` 的字节状态机**原样保留**(它负责 `DATA:WRITE #NN<payload>` 的块流式拦截)。
唯一改动:`MODE_TEXT` 下凑齐一行后,把
```c
SCPI_Input(&ctx->scpi, line_buf, len);     /* scpi_compat */
```
换成 libscpi 的
```c
SCPI_Input(&ctx->scpi, line_buf, len);     /* libscpi:同名,签名兼容,自己管 \n/; 终止 */
```
> 注意:libscpi 的 `SCPI_Input` 也能识别 `\n`/`;`,可一次喂整段;但为了保留 `DATA:WRITE` 块
> 拦截,继续按行喂即可。块 payload 仍走 `cfg.on_block_data`,**不进 libscpi**,避免大块被
> 输入缓冲吃掉。

### 4.6 `SYST:HELP:HEAD?` 重写(C2 + C8)
- 遍历改为 libscpi 的单链:`for (const scpi_command_t *c = ctx->cmdlist; c->pattern; c++)`。
- **不要用 `SCPI_ResultText`(会加引号)也不要靠多结果逗号分隔**:每条 `pattern` + `\n` 用
  `SCPI_ResultCharacters(ctx, c->pattern, strlen(c->pattern))` + `SCPI_ResultCharacters(ctx,"\n",1)`,
  或直接走 interface `write`,保持现有"每命令一行"格式;末尾 `return SCPI_RES_OK;`。
- 仍受 512B 限制,继续支持 `offset/count` 分页。

---

## 5. API 映射表(命令回调迁移参考)

> ### ⚠️ 两个**必须全局处理**的 libscpi 行为差异(每个返回结果的回调都受影响)
>
> **(1) `SCPI_Result*` 返回的是"写入字节数(`size_t`)",不是 `SCPI_RES_OK`。**
> scpi_compat 里 `SCPI_ResultUInt32` 返回 `SCPI_RES_OK(=0)`,所以 `return SCPI_ResultUInt32(...)` 能用;
> 在 libscpi 里它返回写入的字节数(如 3),`!= SCPI_RES_OK(=1)` → **parser 会判定回调失败并压入 execution error**。
> 因此**所有** `return SCPI_Result...(...)` 必须改写成两行:
> ```c
> SCPI_ResultUInt32(ctx, v);   /* 先输出 */
> return SCPI_RES_OK;          /* 再单独返回 OK */
> ```
> 涉及 `usbscpi.c`(cmd_idn/opc/syst_err/syst_cap/data_*…)、`app_main.c`(gpio_get/adc/wlan_*/ble_*)、
> `pico2/main.c`、`tests/` 中**几乎每个查询回调**。这是改动量最大的一项,**否定了之前"回调零改动"的说法**。
>
> **(2) `SCPI_ResultText` 会输出带引号的 SCPI string(`"..."`,内部引号转义)。**
> 现有 host 期望**裸 CSV 行**(`"My Hotspot",-16,1,...`)。若沿用 `SCPI_ResultText(ctx, buf)`,
> 整行会被再包一层引号变成 `""My Hotspot",-16,..."` → 格式错。
> 凡是要输出**原样文本/CSV** 的地方,改用 **`SCPI_ResultCharacters(ctx, s, strlen(s))`**(不加引号)或直接走 `write`。
> 影响:`WLAN:SCAN? i`、`BLE:SCAN? i`、`SYST:CAP?`、`SYST:HELP:HEAD?`、自定义 `*IDN?`。
> (注意:CSV 行里 SSID 两侧的引号是我们**自己 snprintf 进去的**,要保留;不要让库再包一层。)

| scpi_compat | libscpi | 改动 |
|---|---|---|
| `scpi_command_t{pattern,callback,tag}` | 同名同序 | 无 |
| 回调 `scpi_result_t f(scpi_t*)` | 同 | 无 |
| `SCPI_ParamUInt32(ctx,&v,1)` | `SCPI_ParamUInt32(ctx,&v,TRUE)` | `1/0`→`TRUE/FALSE`(值兼容,建议改) |
| `return SCPI_ResultUInt32/Int32/Bool(...)` | `SCPI_Result...(...); return SCPI_RES_OK;` | ❗ **返回值语义不同(见上 (1)),每处都要改** |
| `SCPI_ResultText(ctx,s)`(输出裸文本/CSV) | `SCPI_ResultCharacters(ctx,s,strlen(s))` | ❗ **`ResultText` 会加引号(见上 (2)),改用 `ResultCharacters`** |
| `SCPI_ResultArbitraryBlock(ctx,p,n)` | 同名 | 无(`DATA:READ?` 块格式一致),仍需 `; return SCPI_RES_OK;` |
| `SCPI_ErrorPush(ctx,-222,"...")` | `SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE)` | 去掉消息参数(C7) |
| `SCPI_ErrorPop/Clear` | `SCPI_ErrorPop`(签名/语义略不同)/ `SCPI_ErrorClear` | 错误队列由 libscpi 管 |

> ~~`SCPI_ErrorPush` 自定义消息 → `SCPI_ErrorPushEx`~~ **不推荐**:`SCPI_ErrorPushEx` 在
> `USE_MEMORY_ALLOCATION_FREE` 下仍走 `strndup/free`,违反"MCU 无 malloc"。本仓库错误码都是标准码
> (-222/-109/-113/-200/-161/-223),直接 `SCPI_ErrorPush(code)` 用标准消息即可,**放弃自定义消息文本**。

> 例:`cmd_wlan_get` 迁移后(注意两处改动)——
> ```c
> static scpi_result_t cmd_wlan_get(scpi_t *ctx) {
>     uint32_t idx; char buf[96];
>     if (SCPI_ParamUInt32(ctx, &idx, TRUE) != SCPI_RES_OK) return SCPI_RES_ERR;
>     if (wifi_scan_get(idx, buf, sizeof buf) != 0) {
>         SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
>         return SCPI_RES_ERR;
>     }
>     SCPI_ResultCharacters(ctx, buf, strlen(buf));  /* 裸 CSV,不要 ResultText */
>     return SCPI_RES_OK;                            /* 不能 return SCPI_Result*() */
> }
> ```

---

## 6. 分阶段实施步骤(逐文件、可单独验证)

### 阶段 0 — 准备(0.5d)
- [ ] vendoring libscpi 到 `third_party/libscpi/`(**pin 到具体 SHA**,写 `VERSION` 文件)。
- [ ] 新增 `scpi_user_config.h` + 全局 `-DSCPI_USER_CONFIG=1`(§3.3),**先不接线**,确认 libscpi
      能独立编过(host gcc 编个 demo),并 `nm` 确认无 `malloc/free/strndup`。

### 阶段 1 — core 改造(`src/usbscpi.c` + `include/scpi/scpi.h`)(2–3d)
- [ ] `include/scpi/scpi.h`:**直接删除本仓库这个 shim**(不能改成 `#include "scpi/scpi.h"`——
      该文件自身就在 `scpi/scpi.h` 路径上,会**递归包含自己**)。删除后让 libscpi 的 `inc/` 提供
      `scpi/scpi.h`;确认本仓库 include 路径里**不再有** `include/scpi/` 抢先匹配(必要时调整
      `target_include_directories` 顺序,使 libscpi 的 `inc` 在前)。`include/usbscpi/usbscpi.h`
      里的 `#include "scpi/scpi.h"` 届时解析到 libscpi 头。
- [ ] 重写 `struct usbscpi`、interface 回调、`usbscpi_init`(§4.1–4.3)。
- [ ] 命令表合并 `usbscpi_register`(§4.4)。
- [ ] core 命令:内建替换 + 保留自研(§4.4),重写 `SYST:HELP:HEAD?`(§4.6)。
- [ ] RX 状态机仅替换 `SCPI_Input` 目标(§4.5);块路径保持。
- [ ] 删除 `src/scpi_compat.c`。

### 阶段 2 — 业务回调移植(1d)
- [ ] `examples/esp32s3/main/app_main.c`:按 §5 表过一遍(主要是 `ErrorPush` 去消息、`TRUE/FALSE`)。
- [ ] `examples/pico2/main.c`:同上。

### 阶段 3 — glue(0.5d)
- [ ] `glue/usbscpi_tinyusb.c`:基本不动;若启用 SRQ,确认 `scpi_control_cb`→`usbscpi_tinyusb_set_srq` 链路。

### 阶段 4 — 构建系统(1d)
- [ ] 根 `CMakeLists.txt`、`cmake/usbscpi-pico.cmake`、`cmake/iotsploit-usb-pico.cmake`:
      用 libscpi 源替换 `src/scpi_compat.c`,加 `third_party/libscpi/inc` 到 include。
- [ ] `examples/esp32s3/components/iotsploit-usb/CMakeLists.txt`:同步 SRCS/INCLUDE_DIRS。
- [ ] `idf_component.yml`:如走 managed component 方式则在此声明依赖(否则 vendoring 即可)。

### 阶段 5 — 测试与验收(1d)
- [ ] `tests/test_usbscpi.c` / `tests/test_glue_tinyusb.c`:更新对内部字段的断言(改走公共 API)。
- [ ] host 烟测:`sudo python3 examples/esp32s3/host/scan_test.py` 对比迁移前后输出。
- [ ] 硬件:flash 后 `*IDN?`、WLAN/BLE 扫描、`DATA:READ?` 二进制块逐项核对。

> **总工作量估计:约 7–9 人日**(回调返回值/引号改写覆盖面比初版预估大)。
> 风险集中在:阶段 1 core 重写;**每个查询回调的返回值/引号改写**(漏改即 execution error 或多包引号);
> host 端**线缆格式逐字节回归**(引号、逗号、`\r\n`);二进制块与错误队列行为。

---

## 6.5 代码改动清单(逐文件,基于实际代码扫描)

> 扫描结论:**对内部结构体 `scpi->...` 的直接访问只出现在 `src/usbscpi.c`(3 处)与
> `src/scpi_compat.c`(将删除)**;app / pico / tests / glue 全部只用公共 API。
> 因此真正要写代码的核心只有 `src/usbscpi.c` 一个文件。

### 🔴 Tier 1 — 删除 / 新增
| 文件 | 操作 |
|---|---|
| `src/scpi_compat.c` | **删除**(被 libscpi 取代) |
| `include/scpi/scpi.h` | **删除或改 shim**,让 `#include "scpi/scpi.h"` 解析到 libscpi 头 |
| `third_party/libscpi/` | **新增** vendoring:`src/*.c` + `inc/scpi/*.h` |
| `third_party/libscpi/inc/scpi_user_config.h` | **新增**(文件名固定)+ 全局 `-DSCPI_USER_CONFIG=1` 才生效(见 §3.3) |

### 🟠 Tier 2 — 重写(唯一硬骨头):`src/usbscpi.c`
| 现状(行号) | 改成 |
|---|---|
| `scpi->user_context`(L35) | ✅ 不变,libscpi 也有 |
| `scpi->tables[t]` / `table_count`(L100,101,118,119) | ❗ 改遍历 libscpi 单一 `context->cmdlist` |
| `scpi->error_count`(L186) | ➡️ 改用内建 `SCPI_SystemErrorCountQ` |
| `SCPI_Init(write,user)` 二参(L294) | ❗ 改 libscpi 12 参 |
| `scpi_write_adapter` 返回 `int`(L26) | ➡️ 改 interface `write` 回调,返回 `size_t` |
| 多命令表 `tables[8]` + 追加(L296,300) | ❗ 合并 core+user 到单个 `cmd_merged[]` |
| `default_commands[]` 的 `cmd_idn/rst/cls/opc/syst_err/syst_err_count` | ➡️ 换 libscpi 内建 `SCPI_CoreIdnQ/Cls/Rst/OpcQ/SystemErrorNextQ/SystemErrorCountQ` |
| `cmd_syst_cap / data_free / data_count / data_read` | ✅ 保留,API 兼容 |
| RX 状态机 + `DATA:WRITE` 块路径(L307-412) | ⚠️ 保留,仅把行内 `SCPI_Input` 指向 libscpi |
| `SCPI_ErrorPush(scpi,code,"...")` ×多处(L112,177,241,255,260,268,340,350,364,388) | ➡️ 改 `SCPI_ErrorPush(scpi, <标准码常量>)` |

### 🟡 Tier 3 — 命令回调机械改写(范围比初版大,逐个回调都要过)
> 注意:**不是"零改动"**。除了 ErrorPush,还有两项对**每个返回结果的回调**都生效(见 §5 ⚠️ 框):
> ① `return SCPI_Result*(...)` → `SCPI_Result*(...); return SCPI_RES_OK;`;
> ② 输出裸文本/CSV 的 `SCPI_ResultText` → `SCPI_ResultCharacters`。
- `examples/esp32s3/main/app_main.c`:
  - L94、L120 `SCPI_ErrorPush(ctx,-222,"...")` → `SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE)`;
  - `cmd_gpio_get / adc_read / wlan_done / wlan_count / ble_done / ble_count`:`return SCPI_ResultUInt32/Bool(...)` 拆两行;
  - `cmd_wlan_get / ble_get`:`SCPI_ResultText` → `SCPI_ResultCharacters` 且拆两行;
  - `SCPI_ParamUInt32(...,1)`→`TRUE`(可选)。
- `examples/pico2/main.c`:`cmd_gpio_get / adc_read` 的 `return SCPI_ResultUInt32(...)` 同样要拆两行;`cmd_gpio_set` 仅 `1→TRUE`(可选)。

### 🟢 Tier 4 — 测试
- `tests/test_usbscpi.c`:只用公共 API,但**断言精确输出串**(`*IDN?`/`SYST:HELP:HEAD?`/`SYST:ERR?`/`DATA:READ?`),
  受 §7.1 逗号、§7.2 错误消息、§7.3 `\r\n` 影响,需**更新期望值**。
- `tests/test_glue_tinyusb.c`:仅 `usbscpi_init`,基本不动。
- `tests/stub/tusb.h`:不动。

### 🔵 Tier 5 — 构建系统(scpi_compat.c → libscpi 源)
`CMakeLists.txt`、`cmake/usbscpi-pico.cmake`、`cmake/iotsploit-usb-pico.cmake`、
`examples/esp32s3/components/iotsploit-usb/CMakeLists.txt`:移除 `src/scpi_compat.c`,
加 `third_party/libscpi/src/*.c` 与 include `third_party/libscpi/inc`。
`idf_component.yml`:仅 managed-component 方式才改。

### ⚪ 不用动
`glue/usbscpi_tinyusb.c/.h`(USBTMC 路径不变;SRQ 经 control 回调挂接,可选)、
`include/usbscpi/usbscpi.h`(**公共 API 签名不变**)、
`examples/*/tusb_config.h`、`usb_descriptors.c`、`examples/minimal_host.c`、
`helpers/ring_buffer.c`、`sdkconfig.defaults`。

> **一句话**:重写集中在 `src/usbscpi.c`;`app_main.c`/`pico2/main.c` 的**每个查询回调**要做两处机械
> 改写(返回值拆行 + `ResultText`→`ResultCharacters`,见 §5);删 `scpi_compat.c`、引入 libscpi(pin 版本);
> 其余是 CMake 接线与测试期望值更新。公共 API(`usbscpi_*` 签名)与 USBTMC glue 不变——
> **host 端线缆格式需逐字节回归**(引号/逗号/`\r\n`),但交互方式不变。

---

## 7. 行为差异与坑(务必逐条确认)

### 7.1 回调返回值 + `ResultText` 引号(最容易踩,详见 §5 ⚠️ 框)
- **返回值**:libscpi `SCPI_Result*` 返回字节数,`return SCPI_Result*(...)` 会被判定失败 → 每处拆成
  `SCPI_Result*(...); return SCPI_RES_OK;`。
- **引号**:`SCPI_ResultText` 输出 `"..."` 引号串;裸文本/CSV 改用 `SCPI_ResultCharacters`。

### 7.1b 多结果分隔:换行 → 逗号
`scpi_compat` 每个 `SCPI_ResultText` 自带 `\n`;libscpi 在**同一命令内**多次 `SCPI_Result*`
会以逗号分隔、命令末尾统一换行。
- 影响:`SYST:HELP:HEAD?`(逐命令多次输出)。→ 解决:§4.6 用 `SCPI_ResultCharacters` 自己拼整段、带 `\n`。
- 不影响分隔逻辑:`WLAN:SCAN? i` / `BLE:SCAN? i`(本就一次性输出整行 CSV),但仍受上面的引号/返回值影响。

### 7.2 错误消息:放弃自定义文本,只用标准码
自定义消息(如 `"Block too large"`)在 libscpi 需 `SCPI_ErrorPushEx`,而它在
`USE_MEMORY_ALLOCATION_FREE` 下仍走 **`strndup/free`**,违反"MCU 无 malloc"(§1 原则)。
- 决策:**不开** `USE_DEVICE_DEPENDENT_ERROR_INFORMATION`,所有 `SCPI_ErrorPush(ctx, <标准码>)`。
- 本仓库用的码都是标准码(-222/-109/-113/-200/-161/-223),消息文本由 libscpi 标准表提供
  (文本与现状不完全一致 → §7 测试期望值需更新)。

### 7.3 行尾符
libscpi 默认输出 `\r\n`(可配)。USBTMC 不依赖行尾,host `strip()` 已兼容;但严格比对
测试时注意 `\r`。如需纯 `\n`,在 write 回调或配置里处理。

### 7.4 512B 响应上限不变
libscpi 增量写入仍进 glue 的 `s_tx_buf[512]`。单条响应超限仍会被 `queue_response` 丢弃。
**分页策略(`COUNT?`+`? i`)必须保留**,libscpi 不解决这个约束。

### 7.5 footprint 上升
libscpi 比单文件 `scpi_compat` 大(约十个 .c)。flash/RAM 会增加;`units.c`/`expression.c`
若用不到可裁剪。迁移后用 `idf.py size` 对比,确认仍在分区预算内(当前 8MB flash、单 APP 大分区表充裕)。

### 7.6 输入缓冲长度
libscpi 用你提供的 `scpi_input[]` 暂存一条命令。需 ≥ 最长命令行(当前 ≤ 128 足够,
但块写场景下注意:块 payload 不进该缓冲,走 `on_block_data`)。

---

## 8. 回滚方案
- 迁移在**独立分支**(如 `scpi-libscpi`)进行,`scpi_compat.c` 在合并前**不从历史删除**。
- core 改造用编译开关隔离:`#ifdef USBSCPI_USE_LIBSCPI` 包裹新实现,保留旧路径一段时间,
  灰度验证通过后再删 `scpi_compat.c` 与开关。
- 任一阶段硬件回归失败 → `git revert` 该阶段提交即可回到 scpi_compat。

---

## 9. 检查清单(合并前 Gate)
- [ ] libscpi **已 pin 到具体 SHA**,`scpi_user_config.h` 生效(`-DSCPI_USER_CONFIG=1`)。
- [ ] 固件 `nm` **无 `malloc/free/strndup`** 符号(坐实无动态分配)。
- [ ] esp32s3 + pico2 均编译通过,`idf.py size` 在预算内。
- [ ] **全仓库无 `return SCPI_Result*(...)` 残留**(`grep -rn 'return SCPI_Result'`);**无对裸文本误用 `SCPI_ResultText`**。
- [ ] `tests/` 全绿(期望值已按引号/逗号/`\r\n` 更新)。
- [ ] `*IDN?` 返回 `cfg.idn` 原串 `IoTSploit,ESP32S3,0001,0.1.0`(自定义回调,**不带引号**)。
- [ ] `WLAN:SCAN` 流程:`DONE?/COUNT?/? i` 输出**裸 CSV**(SSID 引号是我们自己加的,非库包裹)。
- [ ] `BLE:SCAN` 流程同上。
- [ ] `DATA:READ? 64` 二进制定长块 `#NN...` 格式与长度正确。
- [ ] `SYST:ERR?` 错误码正确;越界取行返回 `-222`(消息文本改用 libscpi 标准表)。
- [ ] `SYST:HELP:HEAD?` 仍每命令一行、分页可用(用 `ResultCharacters`,无引号/逗号)。

---

## 10. 附:为什么值得 / 何时别做
- **值得**:需要完整 IEEE 488.2 寄存器/事件、带工程单位的参数、数字后缀 `WLAN:SCAN3?`、
  通道列表 `(@0:8)` 批量取、或与严格 SCPI 工具的一致性。
- **先别做**:若近期只是再加几个像 WLAN/BLE 这样的“整数参数 + 文本/块响应”命令,
  `scpi_compat` 已够用,迁移的 6–8 人日 ROI 不高。建议在确有上述高级需求时再启动本方案。
