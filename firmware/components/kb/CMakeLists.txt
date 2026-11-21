# Wraps the hardware-independent code that lives one level up (and is
# unit-tested on the desktop) so the firmware builds exactly the same files.
set(KB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
idf_component_register(
    SRCS "${KB_ROOT}/engine/keymap_engine.cpp"
         "${KB_ROOT}/core/touch_tracker.cpp"
         "${KB_ROOT}/core/key_layout.cpp"
    INCLUDE_DIRS "${KB_ROOT}/engine" "${KB_ROOT}/core")
