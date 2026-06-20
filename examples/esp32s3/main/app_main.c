#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/usb_phy.h"     /* usb_new_phy */
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "wifi_scan.h"
#include "ble_scan.h"

/* ---------- 静态缓冲(等价 pico2,避免动态分配) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
static uint8_t s_io[256];

/* ---------- ADC oneshot 句柄 ---------- */
static adc_oneshot_unit_handle_t s_adc1;

static void adc_setup(void) {
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&u, &s_adc1);
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    /* 预配置通道0(GPIO1 on esp32s3),DATA:READ? 用它 */
    adc_oneshot_config_channel(s_adc1, ADC_CHANNEL_0, &c);
}

/* ---------- SCPI 发送回调:走 glue 缓冲 IN 路径 ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- DATA:READ? 数据源:连续采 ADC1 ch0 ---------- */
static size_t adc_avail(void *user) { (void)user; return (size_t)-1; }  /* 永远有数据 */

static size_t adc_read_cb(void *user, uint8_t *buf, size_t len) {
    (void)user;
    size_t n = len / 2;               /* 每样本 2 字节(little-endian) */
    for (size_t i = 0; i < n; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc1, ADC_CHANNEL_0, &raw);
        uint16_t v = (uint16_t)raw;
        buf[i*2]   = (uint8_t)(v & 0xFF);
        buf[i*2+1] = (uint8_t)(v >> 8);
    }
    return n * 2;
}

/* ---------- 业务命令(每个 3–5 行,仅 HAL 不同) ---------- */
static scpi_result_t cmd_gpio_set(scpi_t *ctx) {
    uint32_t pin, val;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, val != 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *ctx) {
    uint32_t pin;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    SCPI_ResultUInt32(ctx, (uint32_t)gpio_get_level((gpio_num_t)pin));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_adc_read(scpi_t *ctx) {
    uint32_t ch = 0;
    (void)SCPI_ParamUInt32(ctx, &ch, FALSE);
    int raw = 0;
    adc_oneshot_read(s_adc1, (adc_channel_t)ch, &raw);
    SCPI_ResultUInt32(ctx, (uint32_t)raw);
    return SCPI_RES_OK;
}

/* ---------- WiFi 扫描命令(异步触发 → 轮询 → 逐行取) ---------- */
static scpi_result_t cmd_wlan_scan(scpi_t *ctx) {
    (void)ctx;
    return wifi_scan_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_wlan_done(scpi_t *ctx) {
    SCPI_ResultBool(ctx, wifi_scan_done());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)wifi_scan_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_get(scpi_t *ctx) {
    uint32_t idx = 0;
    char buf[96];
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (wifi_scan_get(idx, buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));   /* 裸 CSV,不要 ResultText(会加引号) */
    return SCPI_RES_OK;
}

/* ---------- BLE 扫描命令 ---------- */
static scpi_result_t cmd_ble_scan(scpi_t *ctx) {
    uint32_t secs = 5;
    (void)SCPI_ParamUInt32(ctx, &secs, FALSE);
    return ble_scan_start(secs) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_done(scpi_t *ctx) {
    SCPI_ResultBool(ctx, ble_scan_done());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)ble_scan_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_get(scpi_t *ctx) {
    uint32_t idx = 0;
    char buf[96];
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (ble_scan_get(idx, buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

static const scpi_command_t demo_commands[] = {
    { "GPIO:SET",  cmd_gpio_set, 0 },
    { "GPIO:GET?", cmd_gpio_get, 0 },
    { "ADC:READ?", cmd_adc_read, 0 },
    { "WLAN:SCAN",        cmd_wlan_scan,  0 },
    { "WLAN:SCAN:DONE?",  cmd_wlan_done,  0 },
    { "WLAN:SCAN:COUNt?", cmd_wlan_count, 0 },
    { "WLAN:SCAN?",       cmd_wlan_get,   0 },
    { "BLE:SCAN",         cmd_ble_scan,   0 },
    { "BLE:SCAN:DONE?",   cmd_ble_done,   0 },
    { "BLE:SCAN:COUNt?",  cmd_ble_count,  0 },
    { "BLE:SCAN?",        cmd_ble_get,    0 },
    SCPI_CMD_LIST_END
    /* *IDN? / SYST:CAP? / SYST:HELP:HEAD? / SYST:ERR? / DATA:READ? 由 core 白送 */
};

/* ---------- TinyUSB USBTMC 必须回调(glue 未提供的补 stub) ---------- */
#if CFG_TUD_USBTMC_ENABLE_488
usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_488_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
        .bmDevCapabilities = {0},
        .bcdUSB488 = 0x0100,
        .bmIntfcCapabilities = {0},
        .bmDevCapabilities488 = {0},
    };
    return &caps;
}
#else
usbtmc_response_capabilities_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
    };
    return &caps;
}
#endif

/* tud_usbtmc_open_cb is provided by the iotsploit-usb TinyUSB glue
 * (it arms the first bulk-OUT read); do not define it here. */
bool tud_usbtmc_msgBulkOut_start_cb(usbtmc_msg_request_dev_dep_out const *msg) { (void)msg; return true; }
bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }

/* ---------- USB Descriptors (merged from usb_descriptors.c to ensure linking) ---------- */
#ifndef TUD_CONFIG_DESC_LEN
#define TUD_CONFIG_DESC_LEN     9
#endif
#ifndef TUD_INTERFACE_DESC_LEN
#define TUD_INTERFACE_DESC_LEN  9
#endif
#ifndef TUD_ENDPOINT_DESC_LEN
#define TUD_ENDPOINT_DESC_LEN   7
#endif
#ifndef TUD_INTERFACE_DESCRIPTOR
#define TUD_INTERFACE_DESCRIPTOR(itf_num, alt, num_ep, bclass, subclass, protocol, str_idx) \
    9, TUSB_DESC_INTERFACE, itf_num, alt, num_ep, bclass, subclass, protocol, str_idx
#endif
#ifndef TUD_ENDPOINT_DESCRIPTOR
#define TUD_ENDPOINT_DESCRIPTOR(ep_addr, ep_attr, ep_size, ep_interval) \
    7, TUSB_DESC_ENDPOINT, ep_addr, ep_attr, U16_TO_U8S_LE(ep_size), ep_interval
#endif

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }

enum { ITF_NUM_USBTMC = 0, ITF_NUM_TOTAL };
#define USBTMC_EP_OUT 0x01
#define USBTMC_EP_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_INTERFACE_DESC_LEN + TUD_ENDPOINT_DESC_LEN * 2)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_INTERFACE_DESCRIPTOR(ITF_NUM_USBTMC, 0, 2, 0xFE, 0x03, 0x01, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_OUT, TUSB_XFER_BULK, 64, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_IN,  TUSB_XFER_BULK, 64, 0),
};
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; return desc_configuration;
}

static uint16_t _desc_str[32];
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    const char *str = NULL; uint8_t n = 0;
    switch (index) {
    case 0: _desc_str[1] = 0x0409; n = 1; break;
    case 1: str = "IoTSploit";       break;
    case 2: str = "ESP32-S3 USBTMC"; break;
    case 3: str = "0001";            break;
    default: return NULL;
    }
    if (str) while (*str && n < 31) _desc_str[1 + n++] = *str++;
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return _desc_str;
}

/* ---------- esp32s3 内置 PHY 起步 ---------- */
static usb_phy_handle_t s_phy;
static void usb_phy_start(void) {
    usb_phy_config_t c = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,    /* esp32s3 内置 PHY */
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    usb_new_phy(&c, &s_phy);
}

/* ---------- USB 泵任务 ---------- */
static void usb_task(void *arg) {
    usbscpi_t *dev = (usbscpi_t *)arg;
    for (;;) {
        tud_task();           /* 驱动 tinyusb,回调链里跑 glue → usbscpi_on_rx/tx */
        usbscpi_task(dev);    /* 处理 core 的延迟工作(无则 no-op) */
    }
}

/* ---------- 无线初始化任务 ----------
 * WiFi + NimBLE 起步占用较多栈,且可能耗时;放到独立任务里,使 USB(USBTMC)
 * 枚举不被其阻塞。即便扫描初始化卡住,USB 仍可响应。 */
static void scan_init_task(void *arg) {
    (void)arg;
    wifi_scan_init();     /* NVS + netif + Wi-Fi STA */
    ble_scan_init();      /* NimBLE controller + host task */
    vTaskDelete(NULL);
}

void app_main(void) {
    adc_setup();
    usb_phy_start();
    tusb_init();

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,ESP32S3,0001,0.1.0",
        .data_avail    = adc_avail,
        .data_read     = adc_read_cb,
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);          /* glue 接管 IN/OUT 路径 */
    usbscpi_register(dev, demo_commands);

    xTaskCreate(usb_task, "usb", 6144, dev, 5, NULL);
    /* USB 起来后再异步初始化无线,避免阻塞枚举 */
    xTaskCreate(scan_init_task, "scan_init", 8192, NULL, 4, NULL);
}
