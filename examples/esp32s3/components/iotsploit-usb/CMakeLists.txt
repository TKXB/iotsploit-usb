set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB LIBSCPI_SRCS "${REPO_ROOT}/third_party/libscpi/src/*.c")

idf_component_register(
    SRCS "${REPO_ROOT}/src/usbscpi.c"
         "${REPO_ROOT}/helpers/ring_buffer.c"
         "${REPO_ROOT}/glue/usbscpi_tinyusb.c"
         "${REPO_ROOT}/glue/usbscpi_socket.c"
         "${REPO_ROOT}/glue/usbscpi_stream.c"
         ${LIBSCPI_SRCS}
    INCLUDE_DIRS "${REPO_ROOT}/include"
                 "${REPO_ROOT}/glue"
                 "${REPO_ROOT}/third_party/libscpi/inc"
    PRIV_INCLUDE_DIRS "${REPO_ROOT}/third_party/libscpi/src"
    REQUIRES tinyusb main lwip)

# libscpi config: activate scpi_user_config.h for every TU that sees scpi/config.h
# (PUBLIC so dependents like 'main' get a matching scpi_t layout / no-malloc build).
target_compile_definitions(${COMPONENT_LIB} PUBLIC SCPI_USER_CONFIG=1)

# The TinyUSB glue buffers a whole device->host message, and the descriptor is
# now 4408 bytes — over the 4096 default, which made every USB descriptor fetch
# fail silently while TCP (which sends inline, unbuffered) kept working. The
# ESP32-S3 has the RAM; small MCUs keep the smaller default.
target_compile_definitions(${COMPONENT_LIB} PUBLIC USBSCPI_TINYUSB_TX_BUF_SIZE=8192u)
