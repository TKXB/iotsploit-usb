# ESP-IDF component shim exposing the repo's C core + BSD-socket SCPI glue to
# this example. HID-only + TCP profile: SCPI rides TCP (lwip), not USBTMC, so
# the TinyUSB glue is not compiled here.
set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB LIBSCPI_SRCS "${REPO_ROOT}/third_party/libscpi/src/*.c")

idf_component_register(
    SRCS "${REPO_ROOT}/src/usbscpi.c"
         "${REPO_ROOT}/helpers/ring_buffer.c"
         "${REPO_ROOT}/glue/usbscpi_socket.c"
         ${LIBSCPI_SRCS}
    INCLUDE_DIRS "${REPO_ROOT}/include"
                 "${REPO_ROOT}/glue"
                 "${REPO_ROOT}/third_party/libscpi/inc"
    PRIV_INCLUDE_DIRS "${REPO_ROOT}/third_party/libscpi/src"
    REQUIRES main lwip)

# Activate scpi_user_config.h (no-malloc layout) for every TU, including 'main'.
target_compile_definitions(${COMPONENT_LIB} PUBLIC SCPI_USER_CONFIG=1)
