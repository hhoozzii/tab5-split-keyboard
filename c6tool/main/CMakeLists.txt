idf_component_register(SRCS "c6_updater.c"
                       INCLUDE_DIRS ".")
set(C6_BIN "${CMAKE_CURRENT_LIST_DIR}/../fw/network_adapter_esp32c6.bin")
if(NOT EXISTS "${C6_BIN}")
    message(FATAL_ERROR "C6 firmware image missing. Run get_c6_fw.ps1 first (it saves fw/network_adapter_esp32c6.bin).")
endif()
target_add_binary_data(${COMPONENT_LIB} "${C6_BIN}" BINARY)
