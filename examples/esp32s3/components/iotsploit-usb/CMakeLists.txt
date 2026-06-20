set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
    SRCS "${REPO_ROOT}/src/scpi_compat.c"
         "${REPO_ROOT}/src/usbscpi.c"
         "${REPO_ROOT}/helpers/ring_buffer.c"
         "${REPO_ROOT}/glue/usbscpi_tinyusb.c"
    INCLUDE_DIRS "${REPO_ROOT}/include"
                 "${REPO_ROOT}/glue"
    REQUIRES tinyusb main)
