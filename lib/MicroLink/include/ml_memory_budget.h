#pragma once

/* Noise transport buffers used by the constrained ESP32-C3 path. Keep the
 * registration response separate from the larger peer-map record budget. */
#define ML_NOISE_AUTH_TAG_SIZE 16u
#define ML_TARGET_MAP_PLAINTEXT_CAPACITY ((16u * 1024u) + 16u)
#define ML_TARGET_MAP_FRAME_STORAGE_SIZE (ML_TARGET_MAP_PLAINTEXT_CAPACITY + ML_NOISE_AUTH_TAG_SIZE)

/* FreeRTOS task stacks are bytes on ESP-IDF. Targeted sessions start only the
 * coordinator while the control workspace is reserved; the data-plane stacks
 * are allocated after the workspace is released.
 *
 * Sized for the ESP32-C3 (CrossPoint fork), where task stacks are the largest
 * heap consumer in a session: the previous 8/14/12/8 KB (42 KB) left ~16 KB
 * free at the DERP TLS handshake, which failed with MEMORY_E. QEMU high-water
 * marks for a full session (microlink_log_stack_watermarks): coord 3.6 KB,
 * derp_tx 2.8 KB (mbedTLS; the device's wolfSSL + WOLFSSL_SMALL_STACK keeps
 * handshake temporaries on the heap), net_io 3.4 KB, wg_mgr 3.1 KB. Sizes
 * below keep 2-4x margin; the reader logs the device's marks at teardown. */
#define ML_TASK_NET_IO_STACK_SIZE  (4u * 1024u)
#define ML_TASK_DERP_TX_STACK_SIZE (4u * 1024u)
#define ML_TASK_COORD_STACK_SIZE   (6u * 1024u)
#define ML_TASK_WG_MGR_STACK_SIZE  (4u * 1024u)
#define ML_DATA_PLANE_STACK_BYTES \
    (ML_TASK_NET_IO_STACK_SIZE + ML_TASK_DERP_TX_STACK_SIZE + ML_TASK_WG_MGR_STACK_SIZE)
#define ML_TARGET_BOOTSTRAP_RESERVED_BYTES \
    (ML_TASK_COORD_STACK_SIZE + ML_TARGET_MAP_FRAME_STORAGE_SIZE)
