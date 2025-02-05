
if(BUILD_LOCAL_EXAMPLES)
# scip_example on localhost
add_executable(scip_example_local
                examples_localhost/c/scip.c)
set_target_properties(scip_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
target_compile_options(scip_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
target_link_libraries(scip_example_local rasta)


# scils_example on localhost
add_executable(scils_example_local
                examples_localhost/c/scils.c)
set_target_properties(scils_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
target_compile_options(scils_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
target_link_libraries(scils_example_local rasta)

# rasta_example_new on localhost
add_executable(rasta_example_local
                examples_localhost/c/rasta.c)
set_target_properties(rasta_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
target_compile_options(rasta_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
target_link_libraries(rasta_example_local rasta)


add_executable(event_system_example_local
                examples_localhost/c/event_test.c)
set_target_properties(event_system_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
target_compile_options(event_system_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
target_link_libraries(event_system_example_local rasta)

if(ENABLE_RASTA_TLS)
add_executable(dtls_example_local
        examples_localhost/c/dtls.c examples_localhost/c/wolfssl_certificate_helper.c examples_localhost/c/wolfssl_certificate_helper.h)
set_target_properties(dtls_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
target_compile_options(dtls_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
target_link_libraries(dtls_example_local rasta wolfssl)
endif()

if(ENABLE_RASTA_OPAQUE)
        add_executable(kex_example_local examples_localhost/c/kex.c)
        set_target_properties(kex_example_local PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
        target_compile_options(kex_example_local PRIVATE ${DEFAULT_COMPILE_OPTIONS})
        target_link_libraries(kex_example_local rasta sodium)

        add_executable(record_generator examples_localhost/c/record_generator.c)
        set_target_properties(record_generator PROPERTIES ${DEFAULT_PROJECT_OPTIONS})
        target_compile_options(record_generator PRIVATE ${DEFAULT_COMPILE_OPTIONS})
        target_link_libraries(record_generator rasta sodium)
endif()

# Copy RaSTA configs to build directory
configure_file(config/rasta_server_local.cfg rasta_server_local.cfg COPYONLY)
configure_file(config/rasta_client1_local.cfg rasta_client1_local.cfg COPYONLY)
configure_file(config/rasta_client2_local.cfg rasta_client2_local.cfg COPYONLY)

configure_file(config/rasta_server_local_tls.cfg rasta_server_local_tls.cfg COPYONLY)
configure_file(config/rasta_client1_local_tls.cfg rasta_client1_local_tls.cfg COPYONLY)
configure_file(config/rasta_client2_local_tls.cfg rasta_client2_local_tls.cfg COPYONLY)

configure_file(config/rasta_server_local_kex.cfg rasta_server_local_kex.cfg COPYONLY)
configure_file(config/rasta_client1_local_kex.cfg rasta_client1_local_kex.cfg COPYONLY)
configure_file(config/rasta_client2_local_kex.cfg rasta_client2_local_kex.cfg COPYONLY)
endif(BUILD_LOCAL_EXAMPLES)


if(BUILD_REMOTE_EXAMPLES)
add_subdirectory(scils_example)
add_subdirectory(scip_example)
add_subdirectory(rasta_example_new)

# Copy RaSTA configs to build directory
configure_file(config/rasta_server.cfg rasta_server.cfg COPYONLY)
configure_file(config/rasta_client1.cfg rasta_client1.cfg COPYONLY)
configure_file(config/rasta_client2.cfg rasta_client2.cfg COPYONLY)

if (EXAMPLE_IP_OVERRIDE)
        add_compile_definitions(EXAMPLE_IP_OVERRIDE)
endif(EXAMPLE_IP_OVERRIDE)

endif(BUILD_REMOTE_EXAMPLES)

if(BUILD_RASTA_GRPC_BRIDGE)
add_subdirectory(rasta_grpc_bridge)
endif(BUILD_RASTA_GRPC_BRIDGE)
