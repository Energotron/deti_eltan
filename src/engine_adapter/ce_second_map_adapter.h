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
    CE_ADAPTER_ABI_VERSION = 8,
    CE_CAP_BIND_GALAXY_POINTER = 1u << 0,
    CE_CAP_NATIVE_MULTI_GALAXY = 1u << 1,
    CE_CAP_NATIVE_SWITCH = 1u << 2,
    CE_CAP_NATIVE_SAVE_EXTENSION = 1u << 3,
    CE_CAP_SMOKE_MARKER = 1u << 4,
    CE_CAP_READONLY_GALAXY_FINGERPRINT = 1u << 5,
    CE_CAP_READONLY_GALAXY_LAYOUT_SAMPLE = 1u << 6,
    CE_CAP_READONLY_GALAXY_LAYOUT_LATEST = 1u << 7,
    CE_CAP_POINTER_NORMALIZED_LAYOUT_HASH = 1u << 8,
    CE_CAP_EXPERIMENTAL_ENGINE_GALAXY = 1u << 9
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
CE_EXPORT uint32_t CE_CALL CEAdapterSampleGalaxyLayout(uint32_t galaxy_ptr, uint32_t sample_tag);
CE_EXPORT uint32_t CE_CALL CEAdapterObserveGalaxyLayout(
    uint32_t galaxy_ptr, uint32_t sample_tag, uint32_t observation_tag
);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutObservationCount(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutBlockHash(uint32_t block_index);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutNormalizedBlockHash(uint32_t block_index);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutZeroMaskLow(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutZeroMaskHigh(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskLow(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskHigh(void);
CE_EXPORT uint32_t CE_CALL CEAdapterGetLayoutSampleBytes(void);
CE_EXPORT uint32_t CE_CALL CEAdapterSupportsNativeMultiGalaxy(void);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeEngineGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCreateAndEnterSecondGalaxy(
    uint32_t galaxy_ptr, uint32_t second_seed, uint32_t player_race
);
CE_EXPORT uint32_t CE_CALL CEAdapterSecondGalaxyStatus(void);
CE_EXPORT uint32_t CE_CALL CEAdapterEnterReadySecondGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterReturnToOldGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterActiveArm(void);

#ifdef __cplusplus
}
#endif

#endif
