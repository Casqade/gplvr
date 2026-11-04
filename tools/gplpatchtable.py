#!/usr/bin/env python3
"""Generate docs/patch-table.md: every patch site the 144 fps build writes, in one table.

WHY THIS EXISTS

The build is GEM+'s gpl67 XML set, gpl67-carset-names.xml and ONE frame-rate XML from
gplrategen.py (plus the two optional gplffb.py preferences), with a rescaled ini alongside.
The reasoning behind each site lives in three places: the note strings in gplrategen.py,
the module docstrings in gplffb.py and gplaiscale.py, and the reports in
docs/audit-2026-09-*/. That is fine for working on a site and useless for auditing the
whole set at once.

This derives the table from the SAME tables the build uses, so it cannot drift from what is
actually emitted. Anything it says about a site is what gplrategen would write today.

    python tools/gplpatchtable.py --out docs/patch-table.md

WHAT IT DOES NOT KNOW

The containing-function column is resolved by taking the nearest NAMED symbol at or below
each address, from a snapshot of the IDB's gpl_/mod_ symbols. Named coverage is 440 of
9611 functions, so a site inside an unnamed function is attributed to whatever named
function precedes it. Rows are marked with a '?' when the gap is large enough for that to
be a real risk. Treat the column as a hint to check, not a fact.
"""

import argparse
import bisect
import io
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gplrategen as G  # noqa: E402
import gplaiscale as A  # noqa: E402

DEFAULT_INI_144 = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "patches", "gpla67-144.ini")

# IDB symbol snapshot, gpl_/mod_ only. Taken 2026-09-13 from the primary instance.
# The last block is the exception: unnamed functions that contain sites or neighbour the
# multiplayer handshake's padding sites, and the task-library exports whose IDB names are
# C++-mangled, written here demangled. An optional third column is the function's end
# address, recorded where the padding rule in owner() would otherwise misattribute a site
# near the tail of a function.
SYMBOLS = """
4012B0 gpl_aiGetDifficultySpeedScale
406620 gpl_carsetLoadMirrorMip
40B8F0 gpl_main
40BD80 gpl_startupScreenInit_640x480
40BFB0 gpl_packColourBgra
40E170 gpl_optionDetailLevel
40E260 gpl_resolveDrivingMode
40E460 gpl_applyGraphicsOptions
40ED10 gpl_optAutoviewPitboardsGet
410410 gpl_driverRecordInit
411A00 gpl_statsRecordBestLap
41AA50 gpl_uiLayoutLetterboxPanels
41AE40 gpl_screenEnterMode
41FBA0 gpl_garageAdjustGearSlot
423100 gpl_drivingViewWidgetDraw
423400 gpl_buildDrivingViewInstanceAndCamera
4234F0 gpl_cameraGetSurface
4239B0 gpl_aiFrameHook_60fpsV1Nops
439090 gpl_aiEnableGate_60fpsV1Disables
441BE0 gpl_aiSessionInitDrivers_reseedsSegmentTable
441CB0 gpl_aiComputeGlobalHype
441FB0 gpl_aiSessionName_basicRacing
442000 gpl_aiSessionName_tryingToOutbrake
442410 gpl_aiSessionName_lineImprovement
442C80 gpl_aiStateName_startOfRace
442D60 gpl_aiState_startOfRace_tick
442EF0 gpl_aiStateName_basicRacing
442F20 gpl_aiStateName_returnToRacing
4431B0 gpl_aiState_tryToOutbrake
443360 gpl_aiState_crashing_enter
443430 gpl_aiState_crashing_tick
443580 gpl_aiPredictOneTickBallistic
4437D0 gpl_aiState_crashed_tick
443880 gpl_aiState_crashed_freezeAndBrake
443980 gpl_aiState_returnToPitting_handler 443AF5
443B00 gpl_aiStateName_enteringPits
443CB0 gpl_aiStateName_leavingPits
443F30 gpl_aiStateName_enteringPitStall
443F70 gpl_aiState_inPitStall
4441D0 gpl_aiState_leavingPitStall
444940 gpl_aiStateName_returnToMissedPits
4449F0 gpl_aiStateName_limpingToPits
444A20 gpl_aiState_limpingToPits_tick
444CB0 gpl_aiStateName_forceToLine
444D30 gpl_aiRegisterFuzzyLineParams
445030 gpl_aiLineGetSpeedAt
445160 gpl_aiFollowLine_getSpeed
445550 gpl_aiIntegrateHeadingAndClampYawRate
445650 gpl_aiApproachPitStall_getSpeedGoal
445660 gpl_aiLeavingPits_getSpeedGoal
445E50 gpl_aiState_avoidLateralBlocker
4464D0 gpl_aiState_stayOnTrack
4468F0 gpl_aiState_evaluateBlocker
446EF0 gpl_aiState_blockerAheadSearch
4472E0 gpl_aiState_tryToApproachLine
4478D0 gpl_aiQueueAheadClear
447A60 gpl_aiSetDecisionHoldTicks
4481D0 gpl_aiBlockerDlatMap
448A40 gpl_aiBuildSegmentTable_FROM_LP36
448C50 gpl_aiPickRaceTarget_setDesignatedPassee
448CD0 gpl_aiSegmentSpeedFilterStep_perLapPerSegment
449650 gpl_aiLineGetDlatSpeedAt
449690 gpl_aiLineGetDlatAccelAt
4496D0 gpl_aiLineGetYawRateAt
4496F0 gpl_aiLineResetYawState
449B00 gpl_aiStateName_forceBasicPass1
449B20 gpl_aiStateName_forceBasicPass2
449B40 gpl_aiStateName_forceAbruptPass1
449B60 gpl_aiStateName_forceAbruptPass2
449C70 gpl_aiGetDlongSpeedMaximum
449C80 gpl_aiGetDlongSpeedAdjCoeff
44A400 gpl_aiLpClampMinMaxRaceSpeeds
44A640 gpl_aiSelectTrackSpeedPair
44A690 gpl_aiLpDlongSpeedAtDist
44A6E0 gpl_aiLpDlongSpeed
44A710 gpl_aiLpDlatOffsetAtDist
44A7C0 gpl_aiLpDlatOffset
44A8A0 gpl_aiLpDlatSpeed
44A920 gpl_aiLpYawRate
44A950 gpl_aiLpDlatGradient
44A9F0 gpl_aiLpNodeFlags
44B100 gpl_aiApplyCarClassParams
44B300 gpl_aiUpdateAllDrivers
44BAB0 gpl_aiOnLapCrossing_resetSegmentIndex
44BBD0 gpl_aiSetVisualAttitude
44BE20 gpl_aiSyncFromPhysics
44C220 gpl_aiUpdateThrottleAndRevs
44C6E0 gpl_aiInitLaunchBlend
44C880 gpl_aiGetDlongSpeedScale
44C8E0 gpl_aiLookaheadDlong
44C920 gpl_aiPredictDlatAt
44C9D0 gpl_aiSetLineHoldTicks
44CA20 gpl_aiIsStillInControl
44CB40 gpl_aiGetLaunchStatusFlags
44D970 gpl_aiRollMechanicalFailure
44E0A0 gpl_lpFileLoad
44E2D0 gpl_lpLoadTrackLines
44E470 gpl_lpFreeAll
450D50 gpl_carDamageIsWorse
451B60 gpl_replayWritePlayerRecords_type1_type2_perTick
451C00 gpl_bgSimState_runsSimStep
452480 gpl_simStep
452AD0 gpl_bgTask
452BC0 gpl_simInitRateGlobals
452CA0 gpl_bgServiceAndDispatch
454350 gpl_viewUpdate
454E30 gpl_mirrorViewCompute
454FB0 gpl_viewGetMirrorRecord
4550D0 gpl_camModeHasMirror
4555A0 gpl_aiComputeDriverSpeedParams
4557F0 gpl_aiRollDriverModifier
4570C0 gpl_marshalPostsInit
457770 gpl_marshalPostsTick
458D70 gpl_marshalPostAheadIndex
458E40 gpl_marshalPostTickUpdate
458EB0 gpl_marshalPostUpdateFlags
459330 gpl_starterTickUpdate
4593B0 gpl_marshalPostGreenAndBlue
4595C0 gpl_marshalPostAnimate
459D70 gpl_starterAnimate
45B010 gpl_marshalGetStarterPost
45B030 gpl_marshalGetPost
45B2B0 gpl_sessionLapStampSeconds
45B2F0 gpl_sessionLapIsInvalid
45B450 gpl_sessionLapTimeSeconds
45BA20 gpl_sessionBestLapSeconds
45BD30 gpl_sessionLapCount
45C4B0 gpl_sessionTimeSeconds
45CA40 gpl_tickCountdownDecrement_PRIBLUDA_TAP
45D520 gpl_netJoinQualifyDriver
460CF0 gpl_dlongDelta
46AB40 gpl_svTask
46BED0 gpl_carUpdateDerivedState
46C1F0 gpl_carUpdateSoundEmitters_perTick
46C7E0 gpl_carUpdateWheelSpinAndTyreSmoke
471B20 gpl_contactTestOrRetireChain
471FC0 gpl_contactBuildRelativeFrame
472890 gpl_contactBuildWorldPointAndTest
472FD0 gpl_collisionWorldBeginTick
473070 gpl_buildContactIslands
4735F0 gpl_forEachContactA_skipPlayer
4736B0 gpl_forEachContactB_skipPlayer
477280 gpl_soundSchedulerUpdate_perTick
4775D0 gpl_statsBuildTrackTable
477950 gpl_statsResetNpt
477980 gpl_statsFindTrackRecord
477A00 gpl_statsLoadPlayerSts
477B30 gpl_statsSavePlayerSts
477C00 gpl_aiApplyPlayerSkillHype
477C30 gpl_aiLearnPlayerSkill
477E90 gpl_fgTask
477F50 gpl_fgStateLoop
47AE20 gpl_particleRender
47C4D0 gpl_particleSpawnScrub_type4
47C5F0 gpl_particleSpawn_type8_streak
47C870 gpl_particleSpawn_type1_16
47C9B0 gpl_particleSpawn_type16_thunk
47C9F0 gpl_particleUpdate_perTick
47D330 gpl_debrisUpdateAllAndWriteReplay_perTick
47D430 gpl_debrisUpdate
47D910 gpl_debrisBuildRenderList_liveOrReplay
47E670 gpl_replayDebrisClearFrameCount_type11
47E680 gpl_replayAppendDebrisRecord_type11
47F290 gpl_pitBoardSetAutoview
47F2B0 gpl_pitBoardRecordDraw
47FE10 gpl_pitBoardGetRenderRecord
47FFE0 gpl_pitBoardPlaceAtPitWall
4801F0 gpl_pitBoardUpdate_perTick
480380 gpl_pitBoardComposeLapInfo
480910 gpl_pitBoardWindowTest
480960 gpl_pitBoardTrackCar_perTick
480D50 gpl_pitBoardLoadModel
4813E0 gpl_flagmanInitColours
4814D0 gpl_flagmanInitRenderObj
481640 gpl_flagmanSetFlagColour
481690 gpl_flagmanSetAnim
481710 gpl_flagmanStepAnims
481830 gpl_flagmanBindAllModels
481910 gpl_flagmanBindModel
4820D0 gpl_flagmanBroadcastState
482190 gpl_flagmanLoadModels
483C00 gpl_soundEventFlushToReplay_perTick
4843D0 gpl_bindingEvaluate
485610 gpl_bindingModifierMatchesExactly
485960 gpl_worldRenderSegmentRange
485FA0 gpl_dlongToSegmentIndex
485FF0 gpl_renderOneSegment
486310 gpl_flushSegmentDrawQueue
4873C0 gpl_setMirrorDetailLevel
487620 gpl_sceneRender
4884A0 gpl_uiDrawBarGauge
4887A0 gpl_gearboxSetupInit
4889C0 gpl_gearboxSetupApplyCarDefaults
488D20 gpl_gearsetGetTeeth
488D50 gpl_gearsetSlotMinIndex
488D60 gpl_gearsetSlotMaxIndex
488F40 gpl_gearboxSetupGetSlotIndex
4890B0 gpl_gearboxSetupSetSlotIndex
48A9A0 gpl_simFrameSetup
48B150 gpl_formatLapTime
48B240 gpl_simFrameRender
48BB40 gpl_releaseNowView
492C70 gpl_replayRingAllocFromIni
492CC0 gpl_replayRingInit
492DD0 gpl_replayShutdown
493060 gpl_replaySaveToFile
4938F0 gpl_replayBeginBlock
493970 gpl_replayAllocBlockBytes
493990 gpl_replayEndBlockPad4
493A60 gpl_replayRingAlloc_trimsOldestFrames
493BA0 gpl_replayRingBytesFree
494920 gpl_replayStepFrame
494B00 gpl_replayExtrapolateAllRecords
496860 gpl_aiSpringAccelStep
496930 gpl_aiState_tickDecisionsAndCorridorClamp
496AB0 gpl_aiFakePhysicsTick
496E40 gpl_aiIntegratePosition
497170 gpl_aiConvertPhysicsState
4972C0 gpl_aiComputeDlongSpeedGoal
497940 gpl_aiGetMaxAccel
4979B0 gpl_aiComputeTractionBudget
497AB0 gpl_aiIntegrateAltPerTick
497DC0 gpl_aiCarGapMetres
497E00 gpl_aiComputeLatAccelGoal
498080 gpl_aiDlatSpringAccel
4982F0 gpl_aiIntegrateYawPerTick
498500 gpl_aiLossOfControlCheck_pushCrashingState
498590 gpl_aiLookaheadDistance
4985E0 gpl_aiFollowingDecelSolver
4987E0 gpl_aiApplySlipstreamPair
4989E0 gpl_aiUpdateCarGaps
498CA0 gpl_netCarRecordReset
498CE0 gpl_netCarRecordBlendToPacket
499090 gpl_netCarRecordStampFromReplay
499140 gpl_netCarRecordStampFromNet
499220 gpl_netCarRecordShiftStamps
499260 gpl_netCarRecordStampLocalSim
499300 gpl_netCarExtrapolateRecord
499CB0 gpl_netCarRecordGetPresentState
49A4F0 gpl_aiMistakeModel
49B7D0 gpl_engineSetAmbientParams
49B8E0 gpl_engineWearAndFailure
49FFD0 gpl_computeLateralAccelFromSpeedRadius
4A10E0 gpl_ffbSetEnabled
4A12E0 gpl_simInitPhysicsRate
4A20C0 gpl_ffbIsEnabled
4A21C0 gpl_ffbFillControlState
4A2590 gpl_viewAttitudeSmooth_perTick
4A2770 gpl_controlsBeginTickInterpolation
4A2C90 gpl_controlsReadAxes
4A2FC0 gpl_controlsDigitalSteerUpdate
4A3110 gpl_controlsThrottleHelpUpdate
4A32A0 gpl_controlsBrakeHelpUpdate
4A3680 gpl_autoShiftDownshiftTest
4A3BC0 gpl_controlsSubstepAdvance
4A4230 gpl_ffbUpdate
4A4400 gpl_ffbRestampExtrapolationRecord
4A4500 gpl_ffbAverageAndEmit
4A4640 gpl_ffbAccumulateSample
4A4720 gpl_carSpeedMetresPerSecond
4A4910 gpl_getPhysicsSubstepsPerTick
4A4A20 gpl_simStepAllCars
4A4BD0 gpl_simPreSubstepPerContact
4A4CD0 gpl_carPredictor_listA
4A4CF0 gpl_carCorrector_listA
4A4D10 gpl_carPredictor_listB
4A4D30 gpl_carCorrector_listB
4A4EA0 gpl_simPostSubstepPerContact
4A6400 gpl_replayRegisterBlockHandlers
4A6650 gpl_replayRunPreFrameHooks
4A6680 gpl_replayRunPostFrameHooks
4A6AA0 gpl_replayFillTelemetryRecord_type2
4A6FA0 gpl_replayApplyTelemetryRecord_type2
4A7120 gpl_replayWriteCarDamageRecord_type10
4A7160 gpl_replayCarDamagePreFrameRestore_type10
4A71E0 gpl_replayApplyCarDamageRecord_type10
4A7B40 gpl_ambientSoundLevelUpdate
4A8150 gpl_fgSnapshotRegisterAll
4A8410 gpl_latchBgState
4A8510 gpl_latchPlayer
4AADB0 gpl_physSlipstreamApplyToLocalCar
4AAF60 gpl_physApplySlipstreamFollower
4AB090 gpl_physApplySlipstreamLeader
4B12E0 gpl_tickInterrupt
4BF940 gpl_netStampDelta10bitSigned
4BF960 gpl_netStampWrap10bit
4C1FF0 gpl_keyboardInit
4C2050 gpl_keyboardClearAllModifiers
4C2260 gpl_keyDown_setModifierBit
4C2350 gpl_keyUp_clearModifierBit
4C2500 gpl_inputServiceTick
4C2590 gpl_joyCalibrateAxis
4C28C0 gpl_ffbEffectSetTriple
4C28F0 gpl_ffbSetConstantForce
4C2920 gpl_ffbEffectSetSingle
4C3060 gpl_keyboardWndProc
4C3620 gpl_keyDispatchDownOrUp
4C3B90 gpl_joyReadAxes
4C3C50 gpl_joyReadButtons
4C4110 gpl_joyAxisPollWrapper
4C41B0 gpl_joyButtonPollWrapper
4C4220 gpl_diPollAllDevices
4C45E0 gpl_diPollDeviceAndReacquire
4C4640 gpl_joyExtractAxisFromDIJOYSTATE
4C46A0 gpl_joyExtractButtonFromDIJOYSTATE
4C4FF0 gpl_rasterBegin_loadsPribluda
4C5480 gpl_lodGetDetailScale
4C5490 gpl_lodSetDetailScale
4C54F0 gpl_setAuxTransformSuppressed
4C5940 gpl_matrixTranslate
4C5B60 gpl_matrixRotateAboutZ
4C5F40 gpl_matrixRotateAboutY
4C6320 gpl_matrixRotateAboutX
4C69A0 gpl_matrixTranslateEuler
4C6AE0 gpl_setViewTransform
4C7380 gpl_modelviewRow0_forward
4C73B0 gpl_modelviewRow1_left
4C73E0 gpl_modelviewRow2_up
4C7410 gpl_matrixPush
4C7630 gpl_matrixPop
4C7880 gpl_drawQueueSidePredicate
4C7AB0 gpl_cameraBind
4C7BA0 gpl_matrixLoadProjection
4C7BC0 gpl_polySetDestWrapper
4C7BF0 gpl_cameraCreate
4C7D20 gpl_cameraSetFov
4C7D60 gpl_cameraSetPrincipalPoint
4C7DA0 gpl_cameraRebuildProjection
4C8020 gpl_statsGetGfxPolysDrawn
4C8030 gpl_renderStatsReset
4C8060 gpl_renderStatsDrawOverlay
4C8710 gpl_vertexTransformWithClipPlanes
4C87D0 gpl_polyTransformClipEmit
4C88C0 gpl_polyTransformClipEmit_userPlanes
4C8C80 gpl_polyKernel_flat
4C9750 gpl_obj3dNodeFromOffset
4C9770 gpl_obj3dRelocate
4C9850 gpl_obj3dResolveExternalRefs
4C98B0 gpl_obj3dRelocateNode
4C9910 gpl_obj3dHandleByName
4C9E10 gpl_obj3dLoadFromFile
4C9EE0 gpl_obj3dOpenAndLoad
4C9FC0 gpl_obj3dDestroy
4CA0A0 gpl_obj3dConstruct
4CA1D0 gpl_resourceLoadByType
4CA420 gpl_resourceCacheAcquire
4CA580 gpl_resourceAcquire
4CC650 gpl_nodeDrawAtPose
4CE740 gpl_renderNodeDispatch
4CF1B0 gpl_nodeSizeBytes
4CF430 gpl_nodeRelocateDispatch
4CF790 gpl_nodeBranchPresent
4CF7E0 gpl_lodEvaluateVariable
4CF930 gpl_nodeRelocate_selector17
4CFBB0 gpl_vertexTransformCached
4D63D0 gpl_spriteLoadSrb
4D6490 gpl_spriteDrawVerticalAxis
4D69F0 gpl_spriteDrawScreenAligned
4D6F20 gpl_spriteDraw
4D6FA0 gpl_spriteDrawScaled
4D7A30 gpl_nodeDraw_type3Sprite
4DB5F0 gpl_paramBindingInit
4DB640 gpl_paramDescriptorInit
4DB6B0 gpl_paramLinkIntoTable
4DB850 gpl_paramUnlinkFromTable
4DB8B0 gpl_paramTableLoadIni
4DC780 gpl_libfileReadLibCfg
4DCCC0 gpl_datDirectoryLookup
4DCE70 gpl_libfileOpenFromArchive
4DCF00 gpl_libfileArchiveNameForCwd
4DD030 gpl_libfileOpenReadSearch
4DD310 gpl_datArchiveCacheOpen
4DEB30 gpl_winMain
4E21B0 gpl_rasterResolveEntryPoints
4E3AE0 gpl_viewRecordSetPosition
4E3B50 gpl_viewRecordSetEuler
4E3E30 gpl_rbCommitNextToCurrent
4E3ED0 gpl_rbIntegrateEuler
4E4140 gpl_rbIntegrateTrapezoid
4E4ED0 gpl_matrixToEuler
4E71A0 gpl_soundTaskRegister
4E7370 gpl_soundSetSamplePitch
4E74C0 gpl_sndLoadWave_lifetimeInTicks
4E8B40 gpl_inServer_THREAD_IDENTITY_NOT_NETMODE
4E8E10 gpl_bgEventQueueDrain
4E8EC0 gpl_fgSnapshotRegister
4E8F20 gpl_fgWaitForTickAndSnapshot
4E8FA0 gpl_fgSnapshotRelease
4EEF40 gpl_guiDispatchCallback
4EF110 gpl_fgEnterState_PRIBLUDA_TAP
4EF5F0 gpl_fgFrameSnapshotBegin
4EF630 gpl_fgSnapshotNotifySlot24
4EF650 gpl_fgFrameSnapshotEnd
4EF680 gpl_fgSnapshotNotifySlot20
4F08B0 gpl_uiFindNamedResourceType6
4F0A00 gpl_uiFindNamedResourceType11
4F2C70 gpl_platformGraphicsInit_once
4F3250 gpl_screenSetModeById
4F3310 gpl_screenSetModeBySize
4F3570 gpl_fgEventLoop
4F3B00 gpl_fgRunPostFrameCallbacks
4F3B60 gpl_presentIfForeground
4F40E0 gpl_frameRateMeterSample
4F41B0 gpl_messagePumpDrain
4FB0B0 gpl_gfxFullHiresDraw_theSnowPanel
4FC050 gpl_platformEventPoll
4FC250 gpl_uiElementSetPosition
4FC310 gpl_uiElementSetSize
4FC3D0 gpl_uiElementHide
4FFED0 gpl_guiInvokeWidgetCallbacks
4FFF50 gpl_uiWidgetListFindByName
500E40 gpl_fgEventDequeue
500F70 gpl_fgPumpInputAndTickEvents
5022A0 gpl_windowRedrawAndPresent
51E540 gpl_mouseAccumulateToClientPos
51E630 gpl_mouseReadClientDelta
51E7A0 gpl_rand
981357 mod_aiSeedSegmentSpeedTable_FROM_LP36
981C44 mod_aiLoadTrackAndAiParams
9839BA mod_aiLimpSpeedFloor
983BB3 mod_failuresOff
983BCA mod_ffbUpdate_failuresOn
983D47 mod_aiConvertPhysicsState
983DF2 mod_aiLappedLetLeaderBy
984068 mod_aiPickRaceTargetWithLapGuard
986CAC mod_aiClassGearingSetup
986FF0 mod_aiCarClassScale
987019 mod_aiUpdateThrottleAndRevs
98703E mod_aiApplyClassDrag_slipstreamAware
9872CC mod_aiGetMaxAccel
9873C8 mod_aiSpeedGoalWrapper
9874DD mod_aiAeroCornerSpeedScale
9875D1 mod_aiLineCornerSpeed
987844 mod_aiLpPreprocessCornerSpeeds
987959 mod_aiLpAccelFeasibilityPass
987DB2 mod_aiSessionLpInit
988552 mod_gearboxSetupInit
988576 mod_gearboxSetupApplyCarDefaults
44AC30 sub_44AC30
44D3B0 sub_44D3B0
45D9C0 sub_45D9C0
45DB00 sub_45DB00
45DBE0 sub_45DBE0
4E8070 taskGetRate
4E8420 taskBegin
4E8830 taskStart
"""

# Where each family's argument is written down in full. Order is the table order.
FAMILIES = [
    ("A", "Constants inherited from the 60 fps reference patch", """
Every site here was found by DIFFING `60FPSv2.xml` against stock and matching the
reference's value against `stock * (60/36)^k`. The exponent is read off a known-good
patch, not guessed, and then re-applied as `(144/36)^k`. `gplrategen.classify()` refuses
to call a site scaled unless some k in {2, 1, -1, -2} reproduces the reference's own value
to within 0.3 %, so a site the reference hand-tuned cannot silently enter this class.

Two reference sites are DROPPED rather than followed -- see the "reference mistakes"
section below. The exponent column is the k that was matched."""),
    ("B", "The AI rate-conversion accessors", """
`gpl_aiGetDlongSpeedMaximum` and `gpl_aiGetDlongSpeedAdjCoeff` return per-tick speeds that
only exist after gpla67.ini and the track are read, so no constant can be scaled ahead of
time. The reference plants a float in the nop padding before the functions and rewrites
each `ret` as `fmul dword ptr [0x449C6C]; ret`, converting ON READ. The float is 36/rate."""),
    ("C", "The dlat velocity stubs", """
`gpl_aiDlatSpringAccel` adds car+1046/car+1050 -- a lateral VELOCITY in m/tick -- into a
ceiling whose units are m/tick^2. The mismatch is stock and harmless at 36 Hz because the
numbers were tuned there; at 144 the velocity term is 4x too large and saturates the
lateral command whenever a car is alongside. Each `fadd` is redirected into a 16-byte stub
that folds in 36/rate."""),
    ("D", "The pit and start-of-race speed caves (v21)", """
Three speed goals come from gpla67.ini or the .lp file in m/tick at 36 Hz and are consumed
at the current rate. They cannot be fixed by scaling a constant because the VALUE IS DATA,
not code -- so each call site is redirected into a cave stub that multiplies the loaded
value by 36/rate. Without this the pit-lane AI drives at 4x the intended speed, and
Zandvoort's start limit -- the only track that sets one -- is effectively disabled."""),
    ("E", "The .lp world: 36 Hz forever", """
The racing-line file stores speeds in metres per tick at 36 Hz. That is a property of the
FILE FORMAT, so those numbers never change with the sim rate; what changes is the code that
converts them. Each site here reads a rate-carrying global inside .lp arithmetic and is
redirected to a STOCK copy of that global kept in the cave tail at 0x988920, so the .lp
maths keeps using 36 while every other reader sees 144.

The white flag at 0x459135 was the first such leak found in STOCK rather than mod code:
the marshals converted a .lp line speed to m/s with the CURRENT tick rate, so at 144 Hz
every car counted as a slow vehicle and the flag showed permanently.

The last three, D, E and F, are v30 and take the other shape: instead of redirecting a
global they convert one value at ONE consumer, because in each case the source has other
readers that need it left alone. D is the straightaway-pass separation gate, whose ratio of
a live speed to a .lp speed was 1.75x too wide with its blocker-speed modulation squashed
4x -- and whose three other readers all compare that same ratio against a rate-carrying
field and REQUIRE the factor to stay. E is the blocker decel test, where the consumer
divides by the lookahead SQUARED and the test therefore fired 4x too readily. F is the mod's
per-segment speed table, seeded from raw .lp speeds and then filtered towards a current-rate
one. The conversion floats come from the same cave tail, so no new constant was planted.
See docs/audit-2026-09-08/20-lp-leaks-D-E-F.md."""),
    ("F", "The throttle-blip machine", """
`gpl_aiUpdateThrottleAndRevs` is what the cars do on the grid: a three-state machine whose
two holds are randomised TICK COUNTS and whose two ramps are PER-TICK rates. Unscaled at
144 Hz the holds run 0.08-0.33 s instead of 0.33-1.31 s, which is audible on the grid as
blipping at least twice as fast as vanilla. Every address was checked for
exclusivity: each is referenced only from inside that function."""),
    ("G", "The flagman animation masks and the trackside scenery clock", """
`gpl_flagmanStepAnims` advances each marshal's animation when `test dl, mask` says the tick
counter's low bits are clear, so the mask IS the period minus one. Widening 3 -> 0x0F and
7 -> 0x1F keeps one animation step per 16 or 32 ticks, unchanged in wall-clock terms.

These were invisible to an address-displacement sweep: the stepper folds the field offset
into its base pointer and strides, so nothing addresses the field by displacement. It was
found by going in through the container's count global instead.

**The trackside scenery clock is in this family too, and it took a year to find because the
project had recorded it as unfixable.** The animation data really is baked into each track's
`.3do` files, but the PHASE is not: every trackside object is drawn through the 36-entry node
table at 0x00570330, and of its 36 handlers exactly one reads a clock -- node type 17, a
variable-driven child selector, whose variable kind 3 is `getBGCount() % period`. That is a
raw sim-tick count with no `dt` downstream, so 118 selector nodes across the shipped tracks
-- Spa's flags, Zandvoort's, AAIOM's bird, Spa67's and Targa's waving trees and spectator
groups -- have been cycling four times too fast in every 144 build ever made. It is the
"Moving 3do objects (flags etc..) move too fast on some tracks" line from 60FPSv2's readme.

The COUNTER is shifted, not the period: operator 5 uses the value as a direct index into the
node's case array, so it has to stay inside `0..period-1`, and the period comes from track
data the patch cannot reach. The rewrite is length-preserving -- the three bytes for the
`sar` are bought by turning `mov [esp+4], edx; fild [esp+4]` into `push edx; fild [esp];
pop eax`."""),
    ("H", "Particles and debris", """
Two shapes here. The per-tick deltas (radius growth, dust rise, alpha decay, spark gravity)
scale rate^-1 or rate^-2 like any other rate of change. The FRAME PERIODS are stored as
period-1 and tested with `test cl, imm` or `cmp [esi+14h], imm`, so the scaled value is
`(stock+1)*4 - 1`; the generator verifies the preceding opcode bytes before touching one.

Four constants could not be edited in place because their .rdata slot is shared with a
reader of a different unit. Those get an OPERAND RELOCATION: the 4-byte displacement in the
instruction is rewritten to point at a private copy in the cave at 0x988AA0, so only the
one reader moves. The type-4 puff lifetime needed no cave at all -- caching the particle
pointer in ebx freed exactly enough room for an `fmul`."""),
    ("I", "Network and replay extrapolation", """
`gpl_netCarExtrapolateRecord` predicts where a car is between packets. Offline it is
reached for the local player's own record; the rest of it runs during REPLAY PLAYBACK and
online for remote cars. Three things here, and they needed opposite treatment:

* the Gaussian confidence width and the blend-ramp slope are dimensionless weights over a
  tick delta, so they absorb the rate (rate^-1) along with the ramp's three tick bounds;
* the +/-36 packet-age windows are `cmp esi, imm8` and 144 does not fit a sign-extended
  imm8, so each becomes a jump into a cave holding `cmp esi, imm32`;
* the rec+72 ground-clearance filter is `x = (a + x) * 0.5`, ONE constant on BOTH taps.
  Substituting alpha alone gives a steady-state gain of 0.189 -- the output collapses to
  19 % of target. It needs TWO constants, alpha and 1-alpha, hence a cave.

The site at 0x53A830 is deliberately ABSENT: it is read later as `(acc + n/2) * dt`, so it
is already rate-covariant and scaling it would make remote cars lag.

Two REPLAY RECORDING gates joined this family in v30: `0x00452495` and `0x00451B8D`, both
32-tick periods that keyframe car damage and packed telemetry into the replay tape. 32 ticks
is 0.8889 s at 36 Hz and 128 is 0.8889 s at 144, so both go 1F -> 7F. Neither reader
interpolates -- both copy their record verbatim -- so the worst case on a seek is 0.889 s of
stale state, exactly what 36 Hz gives. The DEBRIS writer is deliberately left ungated: its
reader replaces the whole debris set every frame, so a frame with no debris block renders no
debris at all, and any every-Nth gate would make debris strobe. See report 19."""),
    ("J", "Geometric decays", """
A retained fraction does not scale linearly. `k` applied once per tick becomes
`k^(36/rate)` and an alpha becomes `1 - (1-k)^(36/rate)`. Applying rate^-1 to the 0.995
debris drag would give 0.2488 -- near-total stopping within a few ticks, not a slow
coast."""),
    ("K", "Parameter-descriptor defaults", """
These are the built-in defaults of ini keys, in the descriptor table at 0x554658-0x55595F.
They matter because gpla67.ini names only 9 of the 15 line states, so six states are seeded
from the descriptor and never touched by the ini rescaler. Two of them are read directly
into the track struct as well.

THIS TABLE IS WHY THE +8 REBASE MATTERS. A descriptor record is 24 bytes with its float at
base+16, so an address written 8 high lands on the NEXT record's 0x42 table tag and
unregisters the following key. v18 did exactly that to nine sites while every per-site
check passed."""),
    ("L", "Controls, view and cosmetics", """
Per-tick slew rates for the digital steering, the pedal help, the head-attitude filter and
the tacho telltale. None of these affects the AI or the physics; they are here because they
are per-tick rates of change like everything else, and unscaled they make the keyboard
controls and the helps four times faster."""),
    ("M", "Hardcoded tick counts", """
Countdowns and gates that live as instruction immediates -- not in .data, not in any ini,
and not in the reference except for one site out of nine. The complete set was found by
scanning ALL of .text for `mov dword ptr [reg+<countdown offset>], imm32`, keyed off the
six offsets `gpl_aiUpdateAllDrivers` decrements, then extended by a consumer audit for the
ones that live inside arithmetic (`rand %N` holds, an imm8 add) which that sweep could not
see. The reference patched 0x49744A and missed 0x4978E8 -- the identical instruction 1182
bytes further down the same function."""),
    ("N", "Constants found by the consumer audit", """
Everything the reference patch never touched. Each was found by reading its CONSUMER and
asking what units the expression needs, then confirmed by rate-invariance: express the
quantity per SECOND and it must not move with the rate. Exclusivity was checked on every
one -- a constant with a second reader in a different unit is either left alone or given an
operand relocation instead."""),
    ("O", "The two-tier clock: task registrations and substep constants", """
The rate the game is told to run at, and the fixed-step tier underneath it.

`N = round(288 / rate)`, so at 144 Hz `N = 2` and physics stays at 288 Hz -- unchanged
from stock, which is independent confirmation that 144 is the right target. The substep
constants are therefore emitted at their stock values, but as EXACT reciprocals: 77G
rewrote them with four-decimal roundings (0.003472 for 1/288), which integrated as though
the rate were 144.0092 Hz.

The three registrations carry the rate in a one-byte immediate that cannot hold 144 or
288, so each is replaced by a jump into a stub in the cave at `0x00988B70` that does
`push imm32` and jumps back. `taskStart` then derives the timer as `2 * simRate` at
runtime, so the sim push at `0x0040BCA8` and the timer push at `0x004E8426` must stay
consistent with each other."""),
    ("P", "Multiplayer, replay and GUI: the pieces 77G used to supply", """
Re-derived so that no third-party patch is needed. The frame-rate handshake stamps a build
signature into the join record, and the SERVER compares it during the qualify handshake, so
a 144 Hz server refuses 36 Hz, 60FPSv2 and 77G clients (report 17). The check runs only on
the server: a stock 36 Hz server has no such check and still accepts a 144 Hz client, which
then desyncs -- join only servers running the same build. The replay chunk
version carries the recording rate in its high byte, a cooperative tag for external tools
reading a frame-indexed replay. The GUI throttle delivers a widget callback when the id
differs from the last one or when 4 scheduler ticks have passed -- 77G's every-4th-callback
rule fixed held controls but dropped 3 clicks in 4."""),
    ("Q", "The GPLRank best-lap block -- NOT a rate fix", """
The one part of this patch that changes what the game RECORDS rather than how it runs, and
the only group here you might reasonably want to drop. Delete `BEST_LAP_BLOCK_SITES` from
`tools/gplrategen.py` and best-lap saving comes straight back.

GPLRank ranks drivers by comparing saved best laps, and a 144 Hz build is not the game
those rankings were set on. Lee Bowden blocked the same two stores in 77G and said so in
his FAQ: *"Saving of Best Laps is purposely disabled"*.

`gpl_statsRecordBestLap` keeps a table of best times and a parallel table of unix
timestamps. Blocking both stores leaves the table holding whatever was loaded, so nothing
new reaches the stats file. **In-race timing, the session lap list and NORMALISED PLAYER
TIME are all unaffected** -- NPT is learned from session lap times directly and never reads
this table.

**One correction to 77G.** He nops out all six bytes of the store. But `fld [ebp+arg_C]`
at `0x00411A78` has already PUSHED the lap time and that store is what pops it, so plain
nops leak one x87 register per new best lap against a stack eight deep. This emits
`fstp st(0)` instead, which pops and discards. It is the same defect class as the surviving
`fdivr` that D1 left behind."""),
    ("R", "The force-feedback smoothing filter", """
`gpl_ffbUpdate` smooths its output with a one-pole IIR whose coefficient is applied once
per CALL, so the time constant is measured in SAMPLES, not seconds: about 56 ms at 36 Hz
and 14 ms at 144. It is the only rate coupling anywhere on the FFB path -- the averager
that feeds it runs inside the physics substep loop at a fixed 288 Hz -- which is why it
lives here and not with the two FFB preferences in `gplffb.py`. It shipped as an optional
XML until v32; a rate fix that every build needs is not optional.

**Two things make it a cave rather than a constant edit.** The 0.5 is shared with an
arithmetic mean and with a half-SECOND timer, so the slot cannot move. And the stock form
is `x = (a + x) * 0.5` -- ONE constant on BOTH taps, DC gain `0.5/(1-0.5) = 1`.
Substituting alpha into that shared constant gives `0.1591/(1-0.1591) = 0.189` and
collapses the wheel to 19 % of the physics' output. A first attempt did exactly that and
was never installed. A one-pole with a preserved time constant needs TWO constants.

The replacement is not a rate^k either: `alpha = 1 - (1-0.5)^(36/rate)` = 0.1591 at 144 Hz,
where a linear alpha/4 would give 0.125 -- 27 % off, because preserving a time constant is
exponential.

"Correct" here means "matches 36 Hz stock", which is MORE smoothing than an unpatched
144 Hz build gives. A sharper wheel is a legitimate preference, but it is now a matter of
editing this table rather than of omitting a switch: the default has to be the rate-correct
one."""),
    ("Z", "Parsed but NOT emitted", """
These are in the 60 fps reference and deliberately excluded. The rate carriers are
replaced wholesale instead: `0x0040BCA9`, `0x004E71B7` and `0x004E842D` are one-byte
immediates that cannot hold 144 or 288, so this patch widens the whole instruction into a
stub in the cave at `0x00988B70`. `0x004E8078` is dropped for a different reason -- it is
a four-byte immediate that
would hold 288 perfectly well, but it is `taskGetRate`'s fallback for a mode the game
never selects, so nothing ever reads it (report 18).

The `code` and `unclassified` sites are the reference's own inserted code: stubs built in
alignment padding, plus branch displacements retargeted INTO those stubs. Copying the
branches without the stubs would jump into whatever happens to be there, so the whole
group is dropped. Report 17 re-derived what most of it does -- the multiplayer frame-rate
handshake, which this patch now implements independently -- but the remainder at
`0x00410509` and `0x0041050B` is still unresolved 60FPSv2 scaffolding."""),
]


def load_symbols():
    out = []
    for line in SYMBOLS.strip().splitlines():
        a, n, *end = line.split()
        out.append((int(a, 16), n, int(end[0], 16) if end else None))
    out.sort()
    return out


def family(site, note):
    va = site.real_va
    n = note.lower()
    if site.out is None or site.kind in ("code", "unclassified", "overflow") \
            or va in G.RATE_CARRYING_SITES:
        return "Z"
    if site.kind == "substep" or 0x988B70 <= va < 0x988BA0 \
            or va in (0x40BCA8, 0x4E71B1, 0x4E8426):
        return "O"
    if "replay rply chunk" in n or "signature" in n or "gui same-id" in n \
            or "gui throttle cave" in n or "branch retargeted into the check" in n \
            or "hop" in n or "resume the stock password" in n:
        return "P"
    if "blip machine" in n or va == 0x442CD6:
        return "F"
    if "mod .lp-world redirect" in n or "leak d" in n or "leak e" in n or "leak f" in n \
            or 0x988970 <= va < 0x9889C0 \
            or va in (0x44A943, 0x459135, 0x9874E0, 0x988920, 0x455636):
        return "E"
    if va in (0x451B8D, 0x452495):
        return "I"
    if "gplrank best-lap block" in n:
        return "Q"
    if "ffb filter" in n or va in (0x4A4389, 0x988A00, 0x988A04, 0x988A80):
        return "R"
    if 0x47C000 <= va < 0x47E000 or 0x539D00 <= va < 0x539F00 or 0x988AA0 <= va < 0x988AC0:
        return "H"
    if 0x499000 <= va < 0x49A000 or va in (0x53A83C, 0x53A854) or 0x988AC0 <= va < 0x988B20:
        return "I"
    if 0x481700 <= va < 0x481800 or va == G.SCENERY_ANIM_VA:
        return "G"
    if va in (0x442E56, 0x445650, 0x445672) or 0x988A20 <= va < 0x988A60:
        return "D"
    if va in (0x498244, 0x4982BF) or 0x988950 <= va < 0x988970:
        return "C"
    if va in (0x449C6C, 0x449C76, 0x449C86):
        return "B"
    if "geometric-" in n or 0x539F00 <= va < 0x53A000:
        return "J"
    if 0x554000 <= va < 0x556000:
        return "K"
    if 0x53AB00 <= va < 0x53AE00:
        return "L"
    if site.kind == "tick-count":
        return "M"
    if site.kind == "scaled":
        return "A"
    return "N"


def show(blob):
    """Render a site's bytes as the value a human checks, not just hex."""
    if blob is None:
        return "-"
    if len(blob) == 4:
        try:
            f, = struct.unpack("<f", blob)
            if 1e-30 < abs(f) < 1e30:
                return "%.9g" % f
        except struct.error:
            pass
    if len(blob) == 1:
        return "%d" % int.from_bytes(blob, "little", signed=True)
    if len(blob) in (2, 4):
        return "%d" % int.from_bytes(blob, "little")
    return "`%s`" % blob.hex().upper()


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def ini_rows(src, dst):
    def load(path):
        d, sec = {}, ""
        for line in io.open(path, encoding="latin-1"):
            m = A.SECTION_LINE.match(line)
            if m:
                sec = m.group("name")
                continue
            m = A.VALUE_LINE.match(line)
            if m:
                d.setdefault((sec, m.group("key")), []).append(
                    (float(m.group("value")), (m.group("rest") or "").strip()))
        return d

    a, b = load(src), load(dst)
    ratio = 4.0
    out = []
    for key in a:
        if key not in b:
            continue
        for (x, comment), (y, _) in zip(a[key], b[key]):
            if x == y:
                continue
            exp = "?"
            for e in (-2, -1, 1, 2):
                if x and abs(y - x * ratio ** e) <= max(1e-9, abs(x * ratio ** e) * 2e-3):
                    exp = "%+d" % e
            out.append((key[0], key[1], x, y, exp, comment))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    G.gplpatch.add_game_dir_arg(ap)
    ap.add_argument("--exe", help="default: <game-dir>/gplc67_36.exe")
    ap.add_argument("--reference",
                    help="default: <game-dir>/tools/60FPS/60fpsv2/60FPSv2.xml")
    ap.add_argument("--ini-36", help="default: <game-dir>/gpla67.ini.36")
    ap.add_argument("--ini-144", default=DEFAULT_INI_144)
    ap.add_argument("--rate", type=float, default=144.0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    gp = G.gplpatch
    args.exe = gp.game_file(args, "donor", "--exe",
                            "the unpatched 36 Hz reference exe", "gplc67_36.exe")
    args.reference = gp.game_file(args, "ref60", "--reference",
                                  "the 60 fps reference patch",
                                  "tools", "60FPS", "60fpsv2", "60FPSv2.xml")
    args.ini_36 = gp.game_file(args, "ini36", "--ini-36",
                               "the unpatched 36 Hz AI ini", "gpla67.ini.36")

    syms = load_symbols()
    starts = [s[0] for s in syms]

    def owner(va):
        if 0x536000 <= va < 0x600000:
            return "*.rdata/.data constant*"
        if 0x988900 <= va < 0x989000:
            return "*code cave*"
        if not (0x401000 <= va < 0x536000 or 0x980000 <= va < 0x988900):
            return "-"
        i = bisect.bisect_right(starts, va) - 1
        if i < 0:
            return "-"
        base, name, end = syms[i]
        gap = va - base

        # A site in the PADDING BETWEEN two functions belongs to neither, and the nearest
        # FOLLOWING symbol names it far better than a preceding one thousands of bytes back.
        # 0x0045D51A is `gpl_netJoinQualifyDriver -0x6`, not `gpl_marshalPostAnimate
        # +0x3F5A`. The multiplayer frame-rate handshake (family P) writes itself into that
        # padding, so most of its sites are this shape.
        # The test is RELATIVE, not a fixed distance, because this snapshot carries no
        # function sizes and so cannot know where one ends: if the site is within 64 bytes
        # before a known start AND closer to it than to the preceding one, the following
        # symbol is the honest name.
        # Where the snapshot records the preceding function's end, that settles it: a site
        # before the end is inside the function however close the next symbol is.
        if i + 1 < len(syms) and (end is None or va >= end):
            nextBase, nextName, _ = syms[i + 1]
            lead = nextBase - va
            if 0 < lead <= 0x40 and lead < gap:
                return "%s -0x%X" % (nextName, lead)

        if gap == 0:
            return name

        # A gap over 0x1000 that is ALSO not just before the next symbol means the snapshot
        # below is missing the containing function -- a rename that never reached SYMBOLS.
        # Add it in the same pass that applies it.
        return "%s +0x%X%s" % (name, gap, " ?" if gap >= 0x1000 else "")

    _, sites, substeps, physics_hz = G.build(args.exe, args.reference, args.rate)
    buckets = {}
    for s in sites:
        buckets.setdefault(family(s, s.note), []).append(s)
    # Sites the generator parsed out of the reference and then dropped. They never reach
    # `sites`, so they are collected separately rather than classified out of it.
    buckets["Z"] = list(G.DROPPED_SITES)

    n_emitted = len(sites)
    n_dropped = len(buckets["Z"])

    o = io.StringIO()
    o.write(HEADER % (args.rate, int(args.rate), substeps, physics_hz,
                      n_emitted, n_dropped, args.rate,
                      n_emitted, n_dropped))

    for tag, title, prose in FAMILIES:
        rows = sorted(buckets.get(tag, []), key=lambda s: s.real_va)
        o.write("\n### %s. %s  (%d sites)\n%s\n\n" % (tag, title, len(rows), prose))
        o.write("| address | XML address | containing function | stock | at %g Hz | k | w | what and why |\n"
                % args.rate)
        o.write("|---|---|---|---|---|---|---|---|\n")
        for s in rows:
            o.write("| `%08X` | `%08X` | %s | %s | %s | %s | %d | %s |\n" % (
                s.real_va, s.xml_va, owner(s.real_va), show(s.stock), show(s.out),
                s.exponent if s.exponent is not None else s.kind,
                len(s.stock), cell(s.note)))

    o.write(REFERENCE_MISTAKES_SECTION)
    for va, why in sorted(G.REFERENCE_MISTAKES.items()):
        o.write("| `%08X` | %s | %s |\n" % (va, owner(va), cell(why)))

    rows = ini_rows(args.ini_36, args.ini_144)
    o.write(INI_SECTION % len(rows))
    for sec, key, x, y, exp, comment in rows:
        o.write("| `[%s]` | `%s` | %g | %g | %s | %s |\n"
                % (sec, key, x, y, exp, cell(comment.lstrip("; "))))

    modtext_top = max(s.xml_va + len(s.out) for s in sites
                      if G.MODTEXT_VA <= s.xml_va < G.MODTEXT_VA + G.SECTION_SPAN)
    o.write(OTHER_XML_SECTION
            .replace("@MODTEXT_SIZE@", "0x%08X" % (modtext_top - G.MODTEXT_VA))
            .replace("@MODTEXT_TOP@", "0x%08X" % (modtext_top - 1)))

    io.open(args.out, "w", encoding="utf-8", newline="\n").write(o.getvalue())
    sys.stderr.write("wrote %s (%d sites, %d ini keys)\n"
                     % (args.out, len(sites), len(rows)))
    return 0


HEADER = """# Every patch the 144 fps build writes

GENERATED by `tools/gplpatchtable.py` from the same tables `gplrategen.py` and
`gplaiscale.py` use to build. Re-run it after changing either; do not hand-edit.

    python tools/gplpatchtable.py --out docs/patch-table.md

Target rate **%g Hz**. Physics stays at **%g x %d substeps = %g Hz**, so the substep loop
body runs 288 times a second at both rates and the per-tick tier is where rate bugs live.

**THIS PATCH IS SELF-CONTAINED.** Nothing third-party is applied alongside it -- no 77G,
no 60FPSv2 scaffolding. It emits the rate, timer and sound-callback registrations, the
substep constants, the replay version stamp, the multiplayer frame-rate handshake and the
GUI event throttle itself. Applying 77G as well would land on the same caves, and
**GPLPatchDLL has no collision detection whatsoever** -- two XMLs writing one address both
succeed, the later wins, and `GPLPatchLog.txt` stays empty.

That self-containment removes three defects by construction rather than repairing them
afterwards. 77G overwrote `gpl_tickPeriodSeconds`'s only writer, so `dt` was 0.0 for the
whole run in every 144 build made before v25. It froze the sound interpolator's
steps-per-tick to a wrong constant. Its `taskBegin` cave read `dword_94D25C` instead of
`dword_94D264`. None of those situations arises here. The substep constants also come out
as exact reciprocals rather than 77G's four-decimal roundings, which integrated as though
the rate were 144.0092 Hz.

The table below holds **%d sites**, and that is the `<BinaryPatch>` count in the shipped
file. It is not the whole story of what was read: **%d more are parsed out of the 60 fps
reference and deliberately dropped**, listed in section Z.

Those are the sites that carry the rate ITSELF and the reference's own inserted code.
Three of the four rate carriers cannot express %g in a one-byte immediate at all, so
rather than write the immediate this patch replaces the whole instruction with a jump into
a stub that does `push imm32` -- family O. The fourth, `0x004E8078`, would fit perfectly
well; it is dropped because nothing reads it (report 18).

The %d emitted are not all inherited: many are synthesised here -- the tick-count tables,
the caves, the operand relocations, the handshake -- and have no counterpart in the 60 fps
patch at all. The %d dropped are the only ones read and then discarded.

## How to read this

* **address** is the real virtual address in the built exe. **XML address** is what the
  patch file contains: GPLPatchDLL adds 8 to any address in [0x0054C5B8, 0x00971FFF], so
  for sites in that window the two differ. `_rebase_xml_addresses()` derives the second
  from the first and REFUSES if no XML address can express it.
* **k** is the exponent in `stock * (rate/36)^k`, where that applies. A word there instead
  (`rate-conversion`, `tick-count`) means the site is code or is scaled by a rule other
  than a power -- read the note.
* **w** is the width in bytes of the patched region.
* **containing function** is the nearest named IDB symbol at or below the address. A `?`
  and an offset mean the gap is large enough that the site is probably inside an unnamed
  function further down -- check before relying on it.

## The rule that governs almost all of it

*"Is it a tick count?" is the WRONG question. Ask: does the consumer already divide by the
rate?* Two constants multiplying the SAME tick delta in `gpl_netCarExtrapolateRecord`
needed OPPOSITE treatment -- one absorbs the rate, the other is already rate-covariant and
scaling it would have made remote cars lag 2.3x.

## Two tables reaching the same address

`0x00443AC9` is both a v13 line hold and a rate^+1 constant the 60 fps reference already
scaled, so it arrives from two tables with identical bytes. `_dedupe_sites()` collapses
that and REFUSES if two tables ever write the same address with DIFFERENT values, or if
two spans overlap without being the same span -- either would let XML order decide
silently. Before the guard existed the duplicate was emitted twice; the exe was correct
either way, and removing it does not change a single byte of the build.

"""

REFERENCE_MISTAKES_SECTION = """
### Sites where the reference patch is WRONG and is not followed

Both are dropped entirely, leaving stock bytes.

| address | containing function | why the reference is wrong |
|---|---|---|
"""

INI_SECTION = """
## gpla67.ini -- %d rescaled keys

`tools/gplaiscale.py` rescales the AI ini from `gpla67.ini.36`. It derives each key's
exponent from an ORACLE -- the diff between the stock 36 Hz ini and the reference's 60 Hz
ini -- then applies a table of overrides where the reference is dimensionally wrong. The
two override tables and the reasoning behind each entry are in the module's docstring; the
most consequential are the `dlat_accel_k1/k2` spring-damper pairs, which the reference
scales inconsistently (rate^-2 on the cornering pair, rate^0 on the plain pair) and which
must be rate^-2 and rate^-1 to hold both the natural frequency and the damping ratio.

`avoid_time_coeff` and its two siblings are pinned to rate^0: they multiply a
time-to-collision that is ALREADY in current-rate ticks.

| section | key | 36 Hz | 144 Hz | k | ini comment |
|---|---|---|---|---|---|
"""

OTHER_XML_SECTION = """
## The other XMLs in the build

`tools/make144.ps1` hands GPLPatchDLL a list: gpl67's own numbered patches, then ours --
`gpl67-carset-names.xml`, the frame-rate XML this table describes, and whichever of the two
optional FFB preferences were asked for. Only the last group is this project's work. No
display preference is applied to the exe: the letterbox is gplvr's business at runtime.

### `gpl67-carset-names.xml`
Cosmetic. Carset display names; nothing rate-related.

### Retired on 2026-09-09: the companion path

Four files built the exe by stacking our constants on top of Lee Bowden's 77G patch. They
were deleted from `tools/` after v29 drove clean, and are kept for the record in
`docs/audit-2026-09-08/retired/`. `make144.ps1` can no longer produce that configuration.

| file | what it did | why it is gone |
|---|---|---|
| `gplui144.py` | rewrote 77G's GUI throttle cave in place, emitting a modified copy of his whole XML -- a copy rather than an extra XML because a `<Relocation>` ADDS an entry rather than replacing one, so both relocation sets would have been applied to the new body | this patch builds that cave itself, at `0x00988BA0` |
| `79 144fps re-enable race modes v1.xml` | restored the two thunks at `0x004239E9` and `0x004239F7` that 77G stubs to `C3 90 90 90 90`, because "the AI doesn't work properly" -- true of 77G, which omits the whole of the reference's constant scaling | never stubbed here |
| `84 144fps restore tick period v1.xml` | restored the 24 stock bytes at `0x00452BE8`. **This is D1**: 77G writes `1/144` to `0x005FD134`, EIGHT BYTES LOW, so `gpl_tickPeriodSeconds` at `0x005FD13C` lost its only writer and, being in BSS, stayed 0.0 for the whole run -- in every 144 build ever made, v21 included. All nine readers use it as `dt`. | that writer is never touched here |
| `85 144fps sound interp steps v1.xml` | one dword at `0x004E7058`, 4.0 -> 2.0 -- 77G froze the sound interpolator's steps-per-tick instead of letting it be computed | the timer registration pushes 288 and the sim 144, so `R` is computed again as 2 |

Three of the four repaired damage this patch never does. That is the argument for the
whole change: a repair applied afterwards depends on load order, and load order is not
checked by anything.

Two facts from that work are still worth keeping. `gpl_soundSchedulerUpdate_perTick` sets
each voice's per-step delta as `(target - current) / R` once per SIM TICK while the sound
callback applies one delta per invocation at the TIMER rate, so `R` is *sound callbacks
per sim tick* and must equal timer/sim -- 2 at every rate. And do NOT "fix" `0x004E71B6`:
it is dead, `push 288` needs five bytes where a two-byte `push imm8` sits, and reverting
the accessor to read `dword_581C58` without also scaling that push restores the divergent
72/144 case, where the interpolator's gain is -3 and every moving sound slams between its
clamp rails.

### The two optional force-feedback XMLs -- `tools/gplffb.py`

Each is emitted only when its switch is passed, and each is a SEPARATE file so it can be
dropped without rebuilding anything else. **Neither is a rate fix**: GPL's FFB averager runs
inside the physics substep loop, so it is 288 Hz at every tick rate, and both are
preferences that are equally valid on a stock 36 Hz build. The one rate coupling on the FFB
path, the smoothing filter, is in every build as family R above; it used to be a third
optional XML here.

#### `81 ffb no spring damper v1.xml` -- `-FfbNoSpringDamper` -- NOT APPLIED
Zeroes two coefficients at `0x004A1116` and `0x004A113F`: the centring spring and the
damper GPL installs in the wheel itself at FFB init. They are DirectInput CONDITION
effects, computed by the wheel's own firmware from wheel position and velocity, so they
never pass through the tick loop and never see the car -- a fixed feel laid over the real
signal, which travels separately as a constant-force effect. A preference, not a fix, and
equally valid at 36 Hz.

#### `82 ffb deeper notch v1.xml` -- `-FfbDeeperNotch` -- NOT APPLIED
Rescales the negative half of the force curve by `depth/1.20` (25 sites), deepening the
centre notch. A preference, tunable with `-FfbNotchDepth`.

## What the build depends on that is NOT ours

### Applied to the exe

| dependency | where | stock GEM+? | required? |
|---|---|---|---|
| `05 09 10 15 20 21 22 23 24 25 26 30 31 32 33 40 45 50 60 90 95.xml` | `Mods\GEM+\gpl67\` | yes -- the standard gpl67 set | yes, this is the 1967 mod |

**There is no third-party functional dependency.** `77G 144 fps v1.0.xml` used to be a hard
one: it supplied the sites our XML omitted, three of which cannot be expressed as a byte
patch at all. Those are now widened into our own cave at `0x00988B70` and 77G is not
applied. It **must not be** -- its caves occupy the same region, and nothing in GPLPatchDLL
would tell you. The companion build was deleted on 2026-09-09; see
`docs/audit-2026-09-08/retired/`.

**`77Dv3 No Letterbox.xml` is no longer applied**, and it was never a functional dependency.
It was a hidden SIZING one. modtext's raw data only reached `0x00988C00` because that XML
asks for 0x8AE0 bytes and the file alignment rounds it up, and this patch's caves end just
inside that boundary. Without it GPLPatchDLL asserts on
`section_offset + length <= GetSectionFileSize`, which names neither the cause nor the
missing XML. The frame-rate XML now declares its own room:
`<ExeSection>` modtext `<MinimumFileSize>@MODTEXT_SIZE@</MinimumFileSize>`, derived from
its highest cave byte, `@MODTEXT_TOP@`. It is a floor, so it can grow a section another XML
sized but never shrink one.

The letterbox patch's 19-byte cave at `0x00988900..0x00988913` is why our caves start at
`0x00988920`, and that range is now simply unused. Do not bring a letterbox XML back into
the list. gplvr does the job at runtime (report 33), and the variant found in
`gplc67.exe.bak` and `gplc67.exe.aislow` puts a SECOND cave at `0x00988920`, exactly where
our adj-coeff cave lives.

### Read at build time but never applied

| dependency | used by | what for |
|---|---|---|
| `gpl.exe` | gplexegen | the patch source |
| `gplc67_36.exe` | every generator | the unpatched reference every stock value is checked against, and the CD-key donor for the two `-CarryBytes` regions |
| `60FPSv2.xml` | gplrategen | the known-good 60 fps patch every rate^k exponent is read off |
| `gpl_ai.ini.98` + `gpl_ai_60fps.ini` | gplaiscale | the 36/60 pair that acts as the exponent oracle for the ini |
| `gpla67.ini.36` | gplaiscale | the 36 Hz AI ini that is rescaled |

### Nothing protects our bytes from a later XML

Tested 2026-08 and re-tested 2026-09-08: two XMLs writing the same address with different
data BOTH succeed, the later one wins, and `GPLPatchLog.txt` stays empty. GPLPatchDLL has
no collision detection. Two things protect the build. `gplrategen.py --companion` takes any
other XML, compares it address by address and REFUSES when both write the same bytes with
different values -- it is the only collision check in the toolchain, and it is why that flag
survived the deletion of the companion build. And rule 0c: diff each build against the
previous one AND against the 36 Hz donor, asserting the changed offsets are a subset of the
intended sites. The donor half of that rule was added after 77G's destruction of
`gpl_tickPeriodSeconds` stayed invisible for the whole campaign, because 77G was in every
build being compared.

## DEFERRED: folding the ini rescale into the exe patch

**Written off as future work, 2026-09-09.** Today the build ships two artefacts: this XML,
and a rescaled `gpla67.ini` produced by `tools/gplaiscale.py`. The ini half is the awkward
one to distribute — it overwrites a file players edit themselves, it cannot be layered with
anyone else's AI tuning, and "install this exe AND replace your ini" is two steps where one
would do.

The question is whether the ini could be eliminated by patching its CONSUMERS instead:
convert each value at the point it is read, the way families B, D and E already do for the
`.lp` world, so a stock 36 Hz `gpla67.ini` keeps working unchanged at any rate.

**It is possible, and it is a large piece of work.** What makes it possible is that the ini
is not read directly by scattered code: it is parsed once into the PARAMETER-DESCRIPTOR
TABLE at `0x554658..0x55595F`, and family K already patches defaults in that table. So
there is a single funnel. What makes it large:

* **96 keys, each needing its consumer traced.** The exponent alone is not enough — the
  campaign's sharpest rule is that two constants multiplying the same tick delta can need
  opposite treatment, so every one needs the "does the consumer already divide by the rate?"
  question answered individually. That is the same work the ini oracle does today for free,
  by diffing a hand-made 60 Hz ini against the stock one.
* **The oracle disappears.** `gpl_ai.ini.98` vs `gpl_ai_60fps.ini` is what makes the current
  rescaler trustworthy: the exponents are READ OFF a known-good pair rather than reasoned
  out. Patching consumers throws that away and replaces measurement with argument.
* **Values arrive as text.** The descriptor table holds defaults, but a key present in the
  user's ini overrides its default at parse time, so the conversion has to happen after
  parsing — at the reader, or in a post-parse pass over the table.
* **Nine of fifteen line states are unnamed in the ini** and seeded from descriptor defaults
  only, so the two paths already differ and both would need covering.

The honest trade: it removes a distribution wart and gains compatibility with other people's
AI tuning, at the cost of replacing a measured, oracle-checked rescale with 96 hand-derived
exponents. Not worth doing until someone actually wants to ship this to players, and if it
is done it should keep `gplaiscale.py` alive as the CROSS-CHECK — generate both and assert
they agree.

## DEFERRED: wiring the player's slipstream into physics

**Written off as future work, 2026-09-08.** Not a rate fix and not a defect — a gameplay
change, deliberately out of scope for this patch.

GPL contains the slipstream model TWICE. The AI copy is genuinely wired: its output is
subtracted from `car+482` alongside a wind term and feeds a real drag model, and GEM+
strengthens that further with a per-class drag term. **The player copy is not.** Its entire
observable output is `car+1272`, whose six readers are wind-noise pitch and volume, the
tyre-smoke speed gate, a telemetry `v^2/r` channel, and force feedback. Nothing reaches the
rigid-body integrator, and nothing could: `car+1272` is recomputed from the velocity vector
every tick before the subtraction, and `gpl_carUpdateDerivedState` runs BEFORE
`gpl_simStepAllCars`, deriving presentation state from physics output.

This is true of **vanilla `gpl.exe` as well** -- GEM+ changes not one byte of either copy,
proved by byte-diff and by parsing all 3086 `<BinaryPatch>` and 3004 `<Relocation>` entries
across its 22 XMLs. So the player's tow has never affected the car's motion in this game.

If it is ever wanted, the value already exists and only a consumer is missing. Two candidate
injection points:

1. **At the source** -- wherever the physics turns velocity into aerodynamic force, use the
   slipstream-reduced speed. Cleanest, and not yet located: air density (1.29) appears in only
   three places in `.text`, none of them the player's drag, because the physics works from
   per-car aerodynamic coefficients rather than a literal constant.
2. **At the mod layer** -- GEM+ already added `mod_aiApplyClassDrag_slipstreamAware` for the
   AI; a player-side equivalent would be the same shape.

Either belongs in `gplvr.dll` or a clearly-labelled opt-in, never in the rate patch: it would
change the game at 36 Hz too. See `docs/audit-2026-09-08/13-slipstream-patch-and-wiring.md`.

## What is NOT in any XML

`gplvr.dll` stays runtime-only, but nothing byte-expressible belongs in it: the 144 fps
patch is meant to stand alone, with or without the VR mod. Everything above is in an XML
for that reason.
"""


if __name__ == "__main__":
    sys.exit(main())
