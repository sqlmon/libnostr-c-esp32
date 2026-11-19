# The library comes in through EXTRA_COMPONENT_DIRS ../../, and ESP-IDF names a
# component after its folder, so take the name from the checkout rather than
# requiring the repository to be cloned as "libnostr-c".
get_filename_component(nostr_dir "${COMPONENT_DIR}/../../.." ABSOLUTE)
get_filename_component(nostr_component "${nostr_dir}" NAME)

idf_component_register(
    SRCS "test_main.c"
    INCLUDE_DIRS "."
    REQUIRES ${nostr_component}
)
