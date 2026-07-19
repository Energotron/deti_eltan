#ifndef CE_SECOND_MAP_ADAPTER_H
#define CE_SECOND_MAP_ADAPTER_H

#include <stdint.h>

#if defined(_WIN32)
#define CE_EXPORT __declspec(dllexport)
#define CE_CALL __cdecl
#else
#define CE_EXPORT
#define CE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CE_ADAPTER_ABI_VERSION = 3,
    CE_CAP_BIND_GALAXY_POINTER = 1u << 0,
    CE_CAP_NATIVE_MULTI_GALAXY = 1u << 1,
    CE_CAP_NATIVE_SWITCH = 1u << 2,
    CE_CAP_NATIVE_SAVE_EXTENSION = 1u << 3,
    CE_CAP_SMOKE_MARKER = 1u << 4,
    CE_CAP_READONLY_GALAXY_FINGERPRINT = 1u << 5
};

CE_EXPORT uint32_t CE_CALL CEAdapterAbiVersion(void);
CE_EXPORT uint32_t CE_CALL CEAdapterCapabilities(void);
CE_EXPORT uint32_t CE_CALL CEAdapterBindGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterGetBoundGalaxy(void);
CE_EXPORT uint32_t CE_CALL CEAdapterEchoDword(uint32_t value);
CE_EXPORT uint32_t CE_CALL CEAdapterRunSmoke(uint32_t galaxy_ptr, uint32_t marker);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeGalaxy(uint32_t galaxy_ptr, uint32_t byte_count);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLastFingerprintHash(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLastFingerprintBytes(void);
CE_EXPORT uint32_t CE_CALL CEAdapterSupportsNativeMultiGalaxy(void);

#ifdef __cplusplus
}
#endif

#endif
