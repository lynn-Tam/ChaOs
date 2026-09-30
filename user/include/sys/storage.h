#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int32_t device_id(uint8_t id[20]);
int32_t volume_id(uint8_t id[16]);
// Format the volume authorized by the process's storage-admin grant.
// size is 0 for no id, or 16 bytes. Open files make the operation busy.
int32_t format_volume(const uint8_t* id, uint32_t size);
#ifdef __cplusplus
}
#endif
