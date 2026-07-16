#pragma once

/*
 * RISC-V 64 syscall register ABI:
 *   a7      syscall number
 *   a0-a5   operation arguments
 *   a0      sign-extended myos_status_t on return
 *   a1      operation-specific value on return
 */
