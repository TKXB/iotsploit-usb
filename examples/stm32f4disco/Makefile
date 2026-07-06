PROJECT     := stm32f4disco_usbscpi
OUTPUT_DIR  := _build

# ---- Paths (override with env vars if needed) ----
USBSCPI_ROOT  := $(abspath $(CURDIR)/../..)
TINYUSB_ROOT  ?= $(HOME)/nordic/tinyusb
LIBOPENCM3_DIR ?= $(HOME)/Projects/libopencm3
PROJ_DIR      := $(CURDIR)

# ---- Toolchain ----
CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

# ---- Target CPU (Cortex-M4F) ----
CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

# ---- Optimization & warning flags ----
OPT := -O2 -g3
CFLAGS  := $(CPU_FLAGS) $(OPT)
CFLAGS  += -Wall -Wextra
CFLAGS  += -ffunction-sections -fdata-sections
CFLAGS  += -fno-builtin -fshort-enums
CFLAGS  += -std=c99

# ---- Compile defines ----
# libopencm3 family/device selection (normally provided by genlink)
CFLAGS  += -DSTM32F4 -DSTM32F4CCM -DSTM32F407VG
CFLAGS  += -DCFG_TUSB_MCU=OPT_MCU_STM32F4
CFLAGS  += -DSCPI_USER_CONFIG=1

# ---- Include paths ----
# IMPORTANT: PROJ_DIR must come first so stm32f4xx.h (CMSIS shim) is found
# before any system header.
INC_FOLDERS := \
  $(PROJ_DIR) \
  $(USBSCPI_ROOT)/include \
  $(USBSCPI_ROOT)/glue \
  $(USBSCPI_ROOT)/third_party/libscpi/inc \
  $(USBSCPI_ROOT)/third_party/libscpi/src \
  $(TINYUSB_ROOT)/src \
  $(LIBOPENCM3_DIR)/include

CFLAGS += $(addprefix -I,$(INC_FOLDERS))

# ---- Source files ----
APP_SRCS := \
  $(PROJ_DIR)/main.c \
  $(PROJ_DIR)/usb_descriptors.c

USBSCPI_SRCS := \
  $(USBSCPI_ROOT)/src/usbscpi.c \
  $(wildcard $(USBSCPI_ROOT)/third_party/libscpi/src/*.c) \
  $(USBSCPI_ROOT)/glue/usbscpi_tinyusb.c

TINYUSB_SRCS := \
  $(TINYUSB_ROOT)/src/tusb.c \
  $(TINYUSB_ROOT)/src/common/tusb_fifo.c \
  $(TINYUSB_ROOT)/src/device/usbd.c \
  $(TINYUSB_ROOT)/src/class/usbtmc/usbtmc_device.c \
  $(TINYUSB_ROOT)/src/portable/synopsys/dwc2/dcd_dwc2.c \
  $(TINYUSB_ROOT)/src/portable/synopsys/dwc2/dwc2_common.c

ALL_SRCS := $(APP_SRCS) $(USBSCPI_SRCS) $(TINYUSB_SRCS)

# VPATH so the pattern rule can find sources from multiple directories
VPATH := \
  $(PROJ_DIR) \
  $(USBSCPI_ROOT)/src \
  $(USBSCPI_ROOT)/glue \
  $(USBSCPI_ROOT)/third_party/libscpi/src \
  $(TINYUSB_ROOT)/src \
  $(TINYUSB_ROOT)/src/common \
  $(TINYUSB_ROOT)/src/device \
  $(TINYUSB_ROOT)/src/class/usbtmc \
  $(TINYUSB_ROOT)/src/portable/synopsys/dwc2

# Object files: flatten to _build/<basename>.o (no name collisions in this project)
OBJS := $(foreach src,$(ALL_SRCS),$(OUTPUT_DIR)/$(notdir $(src:.c=.o)))

# ---- Linker flags ----
# NOTE: libraries (LIBS) must come AFTER object files on the link line.
LDFLAGS  := $(CPU_FLAGS) $(OPT)
LDFLAGS  += -T$(PROJ_DIR)/linker.ld
LDFLAGS  += -Wl,--gc-sections
LDFLAGS  += -Wl,-Map=$(OUTPUT_DIR)/$(PROJECT).map
LDFLAGS  += -Wl,--print-memory-usage

LIBS := -L$(LIBOPENCM3_DIR)/lib -lopencm3_stm32f4
LIBS += --specs=nano.specs --specs=nosys.specs

# ---- Build rules ----
.PHONY: all clean flash

all: $(OUTPUT_DIR)/$(PROJECT).elf $(OUTPUT_DIR)/$(PROJECT).bin $(OUTPUT_DIR)/$(PROJECT).hex

# Pattern rule: compile .c -> .o (VPATH resolves source locations)
$(OUTPUT_DIR)/%.o: %.c
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link (object files first, then libraries)
$(OUTPUT_DIR)/$(PROJECT).elf: $(OBJS) $(PROJ_DIR)/linker.ld
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	$(SIZE) $@

# Binary
$(OUTPUT_DIR)/$(PROJECT).bin: $(OUTPUT_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

# Intel HEX
$(OUTPUT_DIR)/$(PROJECT).hex: $(OUTPUT_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O ihex $< $@

# Flash via on-board ST-Link (openocd)
flash: $(OUTPUT_DIR)/$(PROJECT).elf
	openocd -f $(PROJ_DIR)/openocd.cfg \
	  -c "program $(OUTPUT_DIR)/$(PROJECT).elf verify reset exit"

clean:
	rm -rf $(OUTPUT_DIR)
