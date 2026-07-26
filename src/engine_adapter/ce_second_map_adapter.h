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
    CE_ADAPTER_ABI_VERSION = 15,
    CE_CAP_BIND_GALAXY_POINTER = 1u << 0,
    CE_CAP_NATIVE_MULTI_GALAXY = 1u << 1,
    CE_CAP_NATIVE_SWITCH = 1u << 2,
    CE_CAP_NATIVE_SAVE_EXTENSION = 1u << 3,
    CE_CAP_SMOKE_MARKER = 1u << 4,
    CE_CAP_READONLY_GALAXY_FINGERPRINT = 1u << 5,
    CE_CAP_READONLY_GALAXY_LAYOUT_SAMPLE = 1u << 6,
    CE_CAP_READONLY_GALAXY_LAYOUT_LATEST = 1u << 7,
    CE_CAP_POINTER_NORMALIZED_LAYOUT_HASH = 1u << 8,
    CE_CAP_EXPERIMENTAL_ENGINE_GALAXY = 1u << 9,
    CE_CAP_NATIVE_GALAXY_SNAPSHOT = 1u << 10,
    CE_CAP_SAVE_LIFECYCLE_SNAPSHOT = 1u << 11,
    CE_CAP_LOAD_LIFECYCLE_TRANSFORM = 1u << 12,
    CE_CAP_LIVE_ARM_SWITCH = 1u << 13,
    CE_CAP_MANUAL_PORTAL_TRANSIT = 1u << 14,
    CE_CAP_MAP_VISUAL_FIXES = 1u << 15
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
CE_EXPORT uint32_t CE_CALL CEAdapterDumpGalaxyWords(uint32_t galaxy_ptr, uint32_t tag);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeEngineGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCreateAndEnterSecondGalaxy(
    uint32_t galaxy_ptr, uint32_t second_seed, uint32_t player_race
);
CE_EXPORT uint32_t CE_CALL CEAdapterSecondGalaxyStatus(void);
CE_EXPORT uint32_t CE_CALL CEAdapterCreateBareSecondGalaxyForLoad(uint32_t galaxy_ptr, uint32_t second_seed);
CE_EXPORT uint32_t CE_CALL CEAdapterEnterReadySecondGalaxy(uint32_t galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterReturnToOldGalaxy(uint32_t galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterActiveArm(void);
CE_EXPORT uint32_t CE_CALL CEAdapterCheckForExternalReload(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterAbandonEmptySecondGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterMarkSecondGalaxyEntryDisabled(void);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeSubobjectConstruction(uint32_t old_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeBareConAllocation(uint32_t old_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterDumpConVmt(uint32_t old_galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterCloneRaceRecords(uint32_t old_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCreateSecondDestination(uint32_t old_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeNextDayOnSecondGalaxy(uint32_t old_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterDumpProcessWindows(void);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeRawGalaxyPointer(uint32_t galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterSnapshotGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCaptureFreshSnapshot(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterArmCameraRecenter(void);
CE_EXPORT uint32_t CE_CALL CEAdapterConsumeCameraRecenterPending(void);
CE_EXPORT uint32_t CE_CALL CEAdapterClearCameraRecenterPending(void);
CE_EXPORT uint32_t CE_CALL CEAdapterLogTurnHeartbeat(uint32_t turn, uint32_t ship_in_hole);
CE_EXPORT uint32_t CE_CALL CEAdapterLogArrivalDiagnostics(
    uint32_t native_x, uint32_t native_y,
    uint32_t star_x, uint32_t star_y,
    uint32_t galaxy_stars, uint32_t galaxy_sectors,
    uint32_t arrival_sector, uint32_t opened_count);
CE_EXPORT uint32_t CE_CALL CEAdapterLogABTestEvent(uint32_t turn, uint32_t ship_in_hole, uint32_t gab_status);
CE_EXPORT uint32_t CE_CALL CEAdapterIsABTestOrdered(void);
CE_EXPORT uint32_t CE_CALL CEAdapterArmABTestOrdered(void);
CE_EXPORT uint32_t CE_CALL CEAdapterClearABTestOrdered(void);
CE_EXPORT uint32_t CE_CALL CEAdapterArmPendingArrival(uint32_t entering);
CE_EXPORT uint32_t CE_CALL CEAdapterConsumePendingArrival(void);
CE_EXPORT uint32_t CE_CALL CEAdapterLoadSnapshotIntoSecondGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterRenameSecondGalaxySystems(uint32_t second_galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterSetRememberedShipStar(uint32_t star_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterGetRememberedShipStar(void);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallStarLookupRecovery(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallDayProcessRecovery(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallNextDayLabel3Recovery(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallDayCounterGuard(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallPostNextDayGuard(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterSetSectorCount(uint32_t galaxy_ptr, uint32_t count);
CE_EXPORT uint32_t CE_CALL CEAdapterSetGalaxyTurn(uint32_t galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterBuildFreshSecondGalaxy(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallLoadGameDiagnostics(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterStashPlayerValue(uint32_t slot, uint32_t value);
CE_EXPORT uint32_t CE_CALL CEAdapterFetchPlayerValue(uint32_t slot);
CE_EXPORT uint32_t CE_CALL CEAdapterPlayerStashReady(void);
CE_EXPORT uint32_t CE_CALL CEAdapterClearPlayerStash(void);
CE_EXPORT uint32_t CE_CALL CEAdapterSavePlayerStash(void);
CE_EXPORT uint32_t CE_CALL CEAdapterLoadPlayerStash(void);
CE_EXPORT uint32_t CE_CALL CEAdapterStashItemsBegin(void);
CE_EXPORT uint32_t CE_CALL CEAdapterStashItem(uint32_t type, uint32_t size, uint32_t level, uint32_t owner, uint32_t slot, uint32_t wear, uint32_t special, uint32_t module, uint32_t hull_class);
CE_EXPORT uint32_t CE_CALL CEAdapterStashedItemCount(void);
CE_EXPORT uint32_t CE_CALL CEAdapterStashedItemField(uint32_t index, uint32_t field);
CE_EXPORT uint32_t CE_CALL CEAdapterRequestSectorLabelSpawn(uint32_t second_home);
CE_EXPORT uint32_t CE_CALL CEAdapterArmGalaxySaveSnapshot(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterPollSecondHomeTransform(
    uint32_t galaxy_ptr, uint32_t seed
);
CE_EXPORT uint32_t CE_CALL CEAdapterInstallLiveArmSwitch(
    uint32_t galaxy_ptr, uint32_t seed
);
CE_EXPORT uint32_t CE_CALL CEAdapterSetSystemSector(
    uint32_t galaxy_ptr, uint32_t system_index, uint32_t sector_ptr
);
CE_EXPORT uint32_t CE_CALL CEAdapterDumpNamedObject(uint32_t object_ptr, uint32_t kind);
CE_EXPORT uint32_t CE_CALL CEAdapterDumpShipSnapshot(uint32_t ship_ptr, uint32_t phase);
CE_EXPORT uint32_t CE_CALL CEAdapterWatchShipForChanges(uint32_t ship_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCapturePlanetName(uint32_t galaxy_ptr, uint32_t planet_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCaptureStationName(uint32_t galaxy_ptr, uint32_t station_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCaptureDominatorFlag(uint32_t system_index, uint32_t star_owner_value);
CE_EXPORT uint32_t CE_CALL CEAdapterWasDominator(uint32_t system_index);
CE_EXPORT uint32_t CE_CALL CEAdapterLogDominatorFlagCount(void);
CE_EXPORT uint32_t CE_CALL CEAdapterHideBossShip(uint32_t ship_ptr, uint32_t original_star_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterRestoreBossShip(uint32_t ship_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterHiddenBossCount(void);
CE_EXPORT uint32_t CE_CALL CEAdapterHiddenBossShipAt(uint32_t index);
CE_EXPORT uint32_t CE_CALL CEAdapterLogCheckpoint(uint32_t id);
CE_EXPORT uint32_t CE_CALL CEAdapterLogCheckpointValue(uint32_t id, uint32_t value);
CE_EXPORT uint32_t CE_CALL CEAdapterSetPendingShipSweep(uint32_t second_home);
CE_EXPORT uint32_t CE_CALL CEAdapterConsumePendingShipSweep(void);
CE_EXPORT uint32_t CE_CALL CEAdapterBeginSectorSurvey(void);
CE_EXPORT uint32_t CE_CALL CEAdapterDumpSectorIndexed(uint32_t sector_ptr, uint32_t index);
CE_EXPORT uint32_t CE_CALL CEAdapterSurveyClusterList(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterCompareStarConCounts(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterSetSectorIndex(uint32_t sector_ptr, uint32_t new_index);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeSectorFlagBefore(uint32_t sector_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeSectorFlagAfter(uint32_t sector_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterPortalReady(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterPortalStatus(void);
CE_EXPORT uint32_t CE_CALL CEAdapterAdvanceCheatCode(uint32_t key, uint32_t keymod);
CE_EXPORT uint32_t CE_CALL CEAdapterConsumeCheatTrigger(void);
CE_EXPORT uint32_t CE_CALL CEAdapterRegisterPortal(
    uint32_t galaxy_ptr, uint32_t hole_id
);
CE_EXPORT uint32_t CE_CALL CEAdapterEnterRegisteredPortal(
    uint32_t galaxy_ptr, uint32_t hole_id
);
CE_EXPORT uint32_t CE_CALL CEAdapterCompleteRegisteredPortal(uint32_t galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeSaveFormatVersion(uint32_t old_galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterProbeConClass(uint32_t old_galaxy_ptr, uint32_t turn);
CE_EXPORT uint32_t CE_CALL CEAdapterGetGeneratedStarCount(uint32_t galaxy_ptr);
CE_EXPORT uint32_t CE_CALL CEAdapterGetGeneratedStarByIndex(uint32_t galaxy_ptr, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
