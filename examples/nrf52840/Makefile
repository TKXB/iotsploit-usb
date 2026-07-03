PROJECT_NAME     := nrf52840_ble_usbtmc
TARGETS          := nrf52840_xxaa
OUTPUT_DIRECTORY := _build

# ---- Paths ----
SDK_ROOT ?= $(HOME)/nordic/nRF5_SDK_17.1.0_ddde560
USBSCPI_ROOT := $(abspath $(CURDIR)/../..)
TINYUSB_ROOT ?= $(HOME)/nordic/tinyusb

PROJ_DIR := $(CURDIR)

# ---- Toolchain template path (for Makefile.posix) ----
TEMPLATE_PATH := $(SDK_ROOT)/components/toolchain/gcc

# ---- Linker script ----
$(OUTPUT_DIRECTORY)/nrf52840_xxaa.out: \
  LINKER_SCRIPT  := linker.ld

# ---- Optimization & flags ----
OPT = -O3 -g3
CFLAGS += $(OPT)
CFLAGS += -DBOARD_PCA10056
CFLAGS += -DCONFIG_GPIO_AS_PINRESET
CFLAGS += -DFLOAT_ABI_HARD
CFLAGS += -DNRF52840_XXAA
CFLAGS += -DNRF_DFU_SETTINGS_VERSION=1
CFLAGS += -DNRF_DFU_SVCI_VERSION=1
CFLAGS += -DNRF_SDH_BLE_ENABLED=1
CFLAGS += -DNRF_SDH_ENABLED=1
CFLAGS += -DNRF_SDH_SOC_ENABLED=1
CFLAGS += -DSCPI_USER_CONFIG=1
CFLAGS += -DS140
CFLAGS += -DSOFTDEVICE_PRESENT
CFLAGS += -mcpu=cortex-m4
CFLAGS += -mthumb -mabi=aapcs
CFLAGS += -Wall
CFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS += -ffunction-sections -fdata-sections -fno-strict-aliasing
CFLAGS += -fno-builtin -fshort-enums

# ---- Include paths ----
INC_FOLDERS += \
  $(PROJ_DIR) \
  $(USBSCPI_ROOT)/include \
  $(USBSCPI_ROOT)/glue \
  $(USBSCPI_ROOT)/third_party/libscpi/inc \
  $(USBSCPI_ROOT)/third_party/libscpi/src \
  $(SDK_ROOT)/components \
  $(SDK_ROOT)/components/ble/nrf_ble_scan \
  $(SDK_ROOT)/components/ble/common \
  $(SDK_ROOT)/components/libraries/atomic \
  $(SDK_ROOT)/components/libraries/balloc \
  $(SDK_ROOT)/components/libraries/util \
  $(SDK_ROOT)/components/libraries/strerror \
  $(SDK_ROOT)/components/libraries/memobj \
  $(SDK_ROOT)/components/libraries/ringbuf \
  $(SDK_ROOT)/components/libraries/log \
  $(SDK_ROOT)/components/libraries/log/src \
  $(SDK_ROOT)/components/libraries/experimental_section_vars \
  $(SDK_ROOT)/components/boards \
  $(SDK_ROOT)/modules/nrfx/mdk \
  $(SDK_ROOT)/components/softdevice/s140/headers \
  $(SDK_ROOT)/components/softdevice/s140/headers/nrf52 \
  $(SDK_ROOT)/components/softdevice/common \
  $(SDK_ROOT)/components/toolchain \
  $(SDK_ROOT)/components/toolchain/cmsis/include \
  $(SDK_ROOT)/components/toolchain/gcc \
  $(SDK_ROOT)/external/fprintf \
  $(TINYUSB_ROOT)/src \
  $(SDK_ROOT)/modules/nrfx \
  $(SDK_ROOT)/modules/nrfx/drivers/include \
  $(SDK_ROOT)/modules/nrfx/hal \
  $(SDK_ROOT)/modules/nrfx/mdk \
  $(SDK_ROOT)/integration/nrfx \
  $(SDK_ROOT)/integration/nrfx/legacy \
  $(SDK_ROOT)/integration/nrfx

CFLAGS += $(addprefix -I,$(INC_FOLDERS))

# ---- Source files ----
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52840.S \
  $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52840.c \
  $(SDK_ROOT)/components/libraries/util/app_error.c \
  $(SDK_ROOT)/components/libraries/util/app_error_handler_gcc.c \
  $(SDK_ROOT)/components/libraries/util/app_error_weak.c \
  $(SDK_ROOT)/components/libraries/util/app_util_platform.c \
  $(SDK_ROOT)/components/libraries/util/nrf_assert.c \
  $(SDK_ROOT)/components/libraries/atomic/nrf_atomic.c \
  $(SDK_ROOT)/components/libraries/balloc/nrf_balloc.c \
  $(SDK_ROOT)/components/libraries/strerror/nrf_strerror.c \
  $(SDK_ROOT)/components/boards/boards.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_frontend.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_str_formatter.c \
  $(SDK_ROOT)/components/libraries/memobj/nrf_memobj.c \
  $(SDK_ROOT)/components/libraries/experimental_section_vars/nrf_section_iter.c \
  $(SDK_ROOT)/components/libraries/ringbuf/nrf_ringbuf.c \
  $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_clock.c \
  $(SDK_ROOT)/modules/nrfx/soc/nrfx_atomic.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_clock.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_power.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_gpiote.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_systick.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_rtc.c \
  $(SDK_ROOT)/components/ble/nrf_ble_scan/nrf_ble_scan.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh_ble.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh_soc.c \
  $(PROJ_DIR)/main.c \
  $(PROJ_DIR)/usb_descriptors.c \
  $(PROJ_DIR)/ble_scan_handler.c \
  $(PROJ_DIR)/ble_conn_handler.c \
  $(USBSCPI_ROOT)/src/usbscpi.c \
  $(wildcard $(USBSCPI_ROOT)/third_party/libscpi/src/*.c) \
  $(USBSCPI_ROOT)/glue/usbscpi_tinyusb.c \
  $(USBSCPI_ROOT)/helpers/ring_buffer.c \
  $(TINYUSB_ROOT)/src/tusb.c \
  $(TINYUSB_ROOT)/src/common/tusb_fifo.c \
  $(TINYUSB_ROOT)/src/portable/nordic/nrf5x/dcd_nrf5x.c \
  $(TINYUSB_ROOT)/src/class/usbtmc/usbtmc_device.c \
  $(TINYUSB_ROOT)/src/device/usbd.c

# TinyUSB needs CFG_TUSB_MCU and config
CFLAGS += -DCFG_TUSB_MCU=OPT_MCU_NRF5X

# Assembly flags (must match CFLAGS target options for ARM/Thumb interlinking)
ASMFLAGS += -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16

# ---- Linker flags ----
LDFLAGS += $(OPT)
LDFLAGS += -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
LDFLAGS += -L $(SDK_ROOT)/modules/nrfx/mdk -T$(LINKER_SCRIPT)
LDFLAGS += --specs=nano.specs --specs=nosys.specs
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(@:.out=.map)

# ---- Stack/heap sizes ----
nrf52840_xxaa: CFLAGS += -D__HEAP_SIZE=8192
nrf52840_xxaa: CFLAGS += -D__STACK_SIZE=8192

# ---- Define build targets (must come before Makefile.common include) ----

# ---- Standard SDK build boilerplate ----
include $(SDK_ROOT)/components/toolchain/gcc/Makefile.common

# ---- Instantiate targets ----
$(foreach target,$(TARGETS), $(call define_target, $(target)))
