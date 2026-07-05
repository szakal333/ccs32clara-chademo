/* Hardware Interface module */

#include "ccs32_globals.h"

#define CCS_VOLTAGE_SOFT_START_TARGET_MARGIN_VOLTS 5
#define CCS_VOLTAGE_SOFT_START_MAX_MARGIN_VOLTS 5
#define CCS_VOLTAGE_SOFT_START_MIN_RAMP_CURRENT_AMPS 2
#define CCS_VOLTAGE_SOFT_START_RAMP_START_MS 1000
#define CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT 100
#define CCS_VOLTAGE_SOFT_START_VOLTS_PER_AMP 2
#define CCS_VOLTAGE_SOFT_START_MIN_COMPLETE_CURRENT_AMPS 10
#define CCS_VOLTAGE_SOFT_START_COMPLETE_CURRENT_PERCENT 60
#define CCS_VOLTAGE_SOFT_START_COMPLETE_STABLE_MS 3000
#define CCS_VOLTAGE_SOFT_START_BLOCK_LOG_MS 5000
#define CCS_VOLTAGE_SOFT_START_LOG_STEP_VOLTS 5

#define CCS_VOLTAGE_SOFT_START_BLOCK_NONE 0
#define CCS_VOLTAGE_SOFT_START_BLOCK_LOW_CURRENT 1
#define CCS_VOLTAGE_SOFT_START_BLOCK_VOLTAGE_DELTA 2
#define CCS_VOLTAGE_SOFT_START_BLOCK_CURRENT_CEILING 3
#define CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_CURRENT 4
#define CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_STABLE 5

static bool _ccsVoltageSoftStartActive = false;
static bool _ccsVoltageSoftStartRamping = false;
static bool _ccsVoltageSoftStartComplete = false;
static int16_t _ccsVoltageSoftStartTargetVoltage = 0;
static int16_t _ccsVoltageSoftStartEffectiveMaxVoltage = 0;
static int16_t _ccsVoltageSoftStartNormalVoltage = 0;
static int16_t _ccsVoltageSoftStartNormalMaxVoltage = 0;
static uint32_t _ccsVoltageSoftStartStableCurrentSinceMs = 0;
static uint32_t _ccsVoltageSoftStartCompleteStableSinceMs = 0;
static uint32_t _ccsVoltageSoftStartLastRampMs = 0;
static uint32_t _ccsVoltageSoftStartLastBlockLogMs = 0;
static uint8_t _ccsVoltageSoftStartLastBlockReason = CCS_VOLTAGE_SOFT_START_BLOCK_NONE;
static int16_t _ccsVoltageSoftStartLastLoggedTargetVoltage = 0;
static int16_t _ccsVoltageSoftStartLastLoggedMaxVoltage = 0;

static int16_t absInt16(int16_t value)
{
    if (value < 0)
        return -value;

    return value;
}

static int16_t getCcsVoltageSoftStartVoltageDelta(int16_t batteryVoltage)
{
    int16_t voltageDelta = _ccs_params.EvseVoltage - batteryVoltage;
    if (voltageDelta < 0)
        return 0;

    return voltageDelta;
}

static int16_t getCcsVoltageSoftStartCompleteCurrentThreshold()
{
    int16_t requestedCurrent = _ccs_params.TargetCurrent;
    int16_t threshold = CCS_VOLTAGE_SOFT_START_MIN_COMPLETE_CURRENT_AMPS;

    if (requestedCurrent > 0)
    {
        int16_t percentThreshold = (requestedCurrent * CCS_VOLTAGE_SOFT_START_COMPLETE_CURRENT_PERCENT) / 100;
        if (percentThreshold > threshold)
            threshold = percentThreshold;

        if (threshold > requestedCurrent)
            threshold = requestedCurrent;
    }

    return threshold;
}

static int16_t getCcsVoltageSoftStartCurrentBasedCeiling(int16_t batteryVoltage, int16_t normalTargetVoltage)
{
    int16_t ceiling = batteryVoltage + CCS_VOLTAGE_SOFT_START_TARGET_MARGIN_VOLTS
        + (_ccs_params.EvseCurrent * CCS_VOLTAGE_SOFT_START_VOLTS_PER_AMP);
    int16_t initialTargetVoltage = batteryVoltage + CCS_VOLTAGE_SOFT_START_TARGET_MARGIN_VOLTS;

    if (ceiling < initialTargetVoltage)
        ceiling = initialTargetVoltage;

    if (ceiling > normalTargetVoltage)
        ceiling = normalTargetVoltage;

    return ceiling;
}

static bool isCcsVoltageSoftStartDeltaTooHigh(int16_t batteryVoltage)
{
    int16_t voltageDelta = getCcsVoltageSoftStartVoltageDelta(batteryVoltage);
    int16_t supportedDelta = CCS_VOLTAGE_SOFT_START_TARGET_MARGIN_VOLTS
        + CCS_VOLTAGE_SOFT_START_MAX_MARGIN_VOLTS
        + (_ccs_params.EvseCurrent * CCS_VOLTAGE_SOFT_START_VOLTS_PER_AMP);

    return voltageDelta > supportedDelta;
}

static const char *getCcsVoltageSoftStartBlockReasonText(uint8_t blockReason)
{
    switch (blockReason)
    {
    case CCS_VOLTAGE_SOFT_START_BLOCK_LOW_CURRENT:
        return "low current";
    case CCS_VOLTAGE_SOFT_START_BLOCK_VOLTAGE_DELTA:
        return "voltage delta";
    case CCS_VOLTAGE_SOFT_START_BLOCK_CURRENT_CEILING:
        return "current ceiling";
    case CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_CURRENT:
        return "complete current";
    case CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_STABLE:
        return "complete stable time";
    default:
        return "none";
    }
}

static void logCcsVoltageSoftStartClamp(bool force)
{
    if (!_ccsVoltageSoftStartActive)
        return;

    int16_t targetDiff = absInt16(_ccsVoltageSoftStartTargetVoltage - _ccsVoltageSoftStartLastLoggedTargetVoltage);
    int16_t maxDiff = absInt16(_ccsVoltageSoftStartEffectiveMaxVoltage - _ccsVoltageSoftStartLastLoggedMaxVoltage);

    if (!force && targetDiff < CCS_VOLTAGE_SOFT_START_LOG_STEP_VOLTS && maxDiff < CCS_VOLTAGE_SOFT_START_LOG_STEP_VOLTS)
        return;

    println("[cha] CCS voltage soft-start clamp: batt=%dV effectiveTarget=%dV effectiveMaxVoltage=%dV normalTarget=%dV normalMaxVoltage=%dV out=%dV/%dA req=%dA",
        _ccs_params.BatteryVoltage,
        _ccsVoltageSoftStartTargetVoltage,
        _ccsVoltageSoftStartEffectiveMaxVoltage,
        _ccsVoltageSoftStartNormalVoltage,
        _ccsVoltageSoftStartNormalMaxVoltage,
        _ccs_params.EvseVoltage,
        _ccs_params.EvseCurrent,
        _ccs_params.TargetCurrent);

    _ccsVoltageSoftStartLastLoggedTargetVoltage = _ccsVoltageSoftStartTargetVoltage;
    _ccsVoltageSoftStartLastLoggedMaxVoltage = _ccsVoltageSoftStartEffectiveMaxVoltage;
}

static void logCcsVoltageSoftStartBlocked(uint8_t blockReason, int16_t batteryVoltage)
{
    if (blockReason == CCS_VOLTAGE_SOFT_START_BLOCK_NONE)
        return;

    if (_ccsVoltageSoftStartLastBlockReason == blockReason
        && (system_millis - _ccsVoltageSoftStartLastBlockLogMs) < CCS_VOLTAGE_SOFT_START_BLOCK_LOG_MS)
        return;

    println("[cha] CCS voltage soft-start ramp blocked: %s batt=%dV effectiveTarget=%dV effectiveMaxVoltage=%dV normalTarget=%dV normalMaxVoltage=%dV out=%dV/%dA req=%dA delta=%dV",
        getCcsVoltageSoftStartBlockReasonText(blockReason),
        batteryVoltage,
        _ccsVoltageSoftStartTargetVoltage,
        _ccsVoltageSoftStartEffectiveMaxVoltage,
        _ccsVoltageSoftStartNormalVoltage,
        _ccsVoltageSoftStartNormalMaxVoltage,
        _ccs_params.EvseVoltage,
        _ccs_params.EvseCurrent,
        _ccs_params.TargetCurrent,
        getCcsVoltageSoftStartVoltageDelta(batteryVoltage));

    _ccsVoltageSoftStartLastBlockReason = blockReason;
    _ccsVoltageSoftStartLastBlockLogMs = system_millis;
}

static void applyCcsVoltageSoftStartMaxClamp()
{
    int16_t effectiveMaxVoltage = _ccsVoltageSoftStartTargetVoltage + CCS_VOLTAGE_SOFT_START_MAX_MARGIN_VOLTS;

    if (_ccsVoltageSoftStartNormalMaxVoltage > 0 && effectiveMaxVoltage > _ccsVoltageSoftStartNormalMaxVoltage)
        effectiveMaxVoltage = _ccsVoltageSoftStartNormalMaxVoltage;

    if (effectiveMaxVoltage > 0 && _ccsVoltageSoftStartTargetVoltage > effectiveMaxVoltage)
        _ccsVoltageSoftStartTargetVoltage = effectiveMaxVoltage;

    _ccsVoltageSoftStartEffectiveMaxVoltage = effectiveMaxVoltage;
    _ccs_params.MaxVoltage = effectiveMaxVoltage;
}

static void resetCcsVoltageSoftStart()
{
    if (_ccsVoltageSoftStartNormalMaxVoltage > 0)
        _ccs_params.MaxVoltage = _ccsVoltageSoftStartNormalMaxVoltage;

    _ccsVoltageSoftStartActive = false;
    _ccsVoltageSoftStartRamping = false;
    _ccsVoltageSoftStartComplete = false;
    _ccsVoltageSoftStartTargetVoltage = 0;
    _ccsVoltageSoftStartEffectiveMaxVoltage = 0;
    _ccsVoltageSoftStartNormalVoltage = 0;
    _ccsVoltageSoftStartNormalMaxVoltage = 0;
    _ccsVoltageSoftStartStableCurrentSinceMs = 0;
    _ccsVoltageSoftStartCompleteStableSinceMs = 0;
    _ccsVoltageSoftStartLastRampMs = 0;
    _ccsVoltageSoftStartLastBlockLogMs = 0;
    _ccsVoltageSoftStartLastBlockReason = CCS_VOLTAGE_SOFT_START_BLOCK_NONE;
    _ccsVoltageSoftStartLastLoggedTargetVoltage = 0;
    _ccsVoltageSoftStartLastLoggedMaxVoltage = 0;
}

static void startCcsVoltageSoftStart(int16_t batteryVoltage, int16_t initialTargetVoltage, int16_t normalTargetVoltage, int16_t normalMaxVoltage)
{
    _ccsVoltageSoftStartActive = true;
    _ccsVoltageSoftStartRamping = false;
    _ccsVoltageSoftStartComplete = false;
    _ccsVoltageSoftStartTargetVoltage = initialTargetVoltage;
    _ccsVoltageSoftStartNormalVoltage = normalTargetVoltage;
    _ccsVoltageSoftStartNormalMaxVoltage = normalMaxVoltage;
    _ccsVoltageSoftStartStableCurrentSinceMs = 0;
    _ccsVoltageSoftStartCompleteStableSinceMs = 0;
    _ccsVoltageSoftStartLastRampMs = 0;
    _ccsVoltageSoftStartLastBlockLogMs = 0;
    _ccsVoltageSoftStartLastBlockReason = CCS_VOLTAGE_SOFT_START_BLOCK_NONE;
    _ccsVoltageSoftStartLastLoggedTargetVoltage = 0;
    _ccsVoltageSoftStartLastLoggedMaxVoltage = 0;

    applyCcsVoltageSoftStartMaxClamp();

    println("[cha] CCS voltage soft-start target clamp active: batt=%dV effectiveTarget=%dV normalTarget=%dV",
        batteryVoltage,
        _ccsVoltageSoftStartTargetVoltage,
        normalTargetVoltage);
    println("[cha] CCS voltage soft-start max voltage clamp active: batt=%dV effectiveTarget=%dV effectiveMaxVoltage=%dV normalTarget=%dV normalMaxVoltage=%dV",
        batteryVoltage,
        _ccsVoltageSoftStartTargetVoltage,
        _ccsVoltageSoftStartEffectiveMaxVoltage,
        normalTargetVoltage,
        _ccsVoltageSoftStartNormalMaxVoltage);
    logCcsVoltageSoftStartClamp(true);
}

static bool completeCcsVoltageSoftStartIfStable(int16_t batteryVoltage, int16_t normalTargetVoltage)
{
    if (_ccsVoltageSoftStartTargetVoltage < normalTargetVoltage)
    {
        _ccsVoltageSoftStartCompleteStableSinceMs = 0;
        return false;
    }

    int16_t completeCurrentThreshold = getCcsVoltageSoftStartCompleteCurrentThreshold();
    if (_ccs_params.EvseCurrent < completeCurrentThreshold)
    {
        _ccsVoltageSoftStartCompleteStableSinceMs = 0;
        logCcsVoltageSoftStartBlocked(CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_CURRENT, batteryVoltage);
        return false;
    }

    if (isCcsVoltageSoftStartDeltaTooHigh(batteryVoltage))
    {
        _ccsVoltageSoftStartCompleteStableSinceMs = 0;
        logCcsVoltageSoftStartBlocked(CCS_VOLTAGE_SOFT_START_BLOCK_VOLTAGE_DELTA, batteryVoltage);
        return false;
    }

    if (_ccsVoltageSoftStartCompleteStableSinceMs == 0)
    {
        _ccsVoltageSoftStartCompleteStableSinceMs = system_millis;
        logCcsVoltageSoftStartBlocked(CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_STABLE, batteryVoltage);
        return false;
    }

    if ((system_millis - _ccsVoltageSoftStartCompleteStableSinceMs) < CCS_VOLTAGE_SOFT_START_COMPLETE_STABLE_MS)
    {
        logCcsVoltageSoftStartBlocked(CCS_VOLTAGE_SOFT_START_BLOCK_COMPLETE_STABLE, batteryVoltage);
        return false;
    }

    _ccsVoltageSoftStartActive = false;
    _ccsVoltageSoftStartRamping = false;
    _ccsVoltageSoftStartComplete = true;
    _ccs_params.MaxVoltage = _ccsVoltageSoftStartNormalMaxVoltage;

    println("[cha] CCS voltage soft-start complete: target=%dV max=%dV out=%dA req=%dA delta=%dV",
        normalTargetVoltage,
        _ccsVoltageSoftStartNormalMaxVoltage,
        _ccs_params.EvseCurrent,
        _ccs_params.TargetCurrent,
        getCcsVoltageSoftStartVoltageDelta(batteryVoltage));

    return true;
}

static void updateCcsVoltageSoftStart()
{
    int16_t normalTargetVoltage = _ccs_params.TargetVoltage;
    int16_t normalMaxVoltage = _ccsVoltageSoftStartNormalMaxVoltage > 0
        ? _ccsVoltageSoftStartNormalMaxVoltage
        : _ccs_params.MaxVoltage;
    int16_t batteryVoltage = _ccs_params.BatteryVoltage;

    if (_ccsVoltageSoftStartComplete && _ccs_params.TargetCurrent == 0 && _ccs_params.EvseCurrent == 0)
        resetCcsVoltageSoftStart();

    if (normalTargetVoltage <= 0 || batteryVoltage <= 0)
        return;

    int16_t initialTargetVoltage = batteryVoltage + CCS_VOLTAGE_SOFT_START_TARGET_MARGIN_VOLTS;
    if (initialTargetVoltage >= normalTargetVoltage)
    {
        if (_ccsVoltageSoftStartActive)
            resetCcsVoltageSoftStart();
        return;
    }

    if (_ccsVoltageSoftStartNormalVoltage != 0 && _ccsVoltageSoftStartNormalVoltage != normalTargetVoltage)
    {
        resetCcsVoltageSoftStart();
        normalMaxVoltage = _ccs_params.MaxVoltage;
    }

    if (!_ccsVoltageSoftStartActive && !_ccsVoltageSoftStartComplete)
        startCcsVoltageSoftStart(batteryVoltage, initialTargetVoltage, normalTargetVoltage, normalMaxVoltage);

    if (!_ccsVoltageSoftStartActive)
        return;

    if (_ccsVoltageSoftStartTargetVoltage < initialTargetVoltage)
        _ccsVoltageSoftStartTargetVoltage = initialTargetVoltage;

    if (_ccs_params.EvseCurrent < CCS_VOLTAGE_SOFT_START_MIN_RAMP_CURRENT_AMPS)
    {
        _ccsVoltageSoftStartStableCurrentSinceMs = 0;
        _ccsVoltageSoftStartCompleteStableSinceMs = 0;
        applyCcsVoltageSoftStartMaxClamp();
        logCcsVoltageSoftStartClamp(false);
        logCcsVoltageSoftStartBlocked(CCS_VOLTAGE_SOFT_START_BLOCK_LOW_CURRENT, batteryVoltage);
        return;
    }

    if (_ccsVoltageSoftStartStableCurrentSinceMs == 0)
        _ccsVoltageSoftStartStableCurrentSinceMs = system_millis;

    if (!_ccsVoltageSoftStartRamping)
    {
        if ((system_millis - _ccsVoltageSoftStartStableCurrentSinceMs) >= CCS_VOLTAGE_SOFT_START_RAMP_START_MS)
        {
            _ccsVoltageSoftStartRamping = true;
            _ccsVoltageSoftStartLastRampMs = system_millis;
            println("[cha] CCS voltage soft-start current stable -> ramping target");
        }
        else
        {
            applyCcsVoltageSoftStartMaxClamp();
            logCcsVoltageSoftStartClamp(false);
            return;
        }
    }

    int16_t candidateTargetVoltage = _ccsVoltageSoftStartTargetVoltage;
    uint32_t elapsedMs = system_millis - _ccsVoltageSoftStartLastRampMs;
    if (elapsedMs >= CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT)
    {
        int16_t rampVolts = elapsedMs / CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT;
        _ccsVoltageSoftStartLastRampMs += rampVolts * CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT;
        candidateTargetVoltage += rampVolts;
    }

    int16_t currentBasedCeiling = getCcsVoltageSoftStartCurrentBasedCeiling(batteryVoltage, normalTargetVoltage);
    if (candidateTargetVoltage > currentBasedCeiling)
    {
        candidateTargetVoltage = currentBasedCeiling;
        logCcsVoltageSoftStartBlocked(isCcsVoltageSoftStartDeltaTooHigh(batteryVoltage)
            ? CCS_VOLTAGE_SOFT_START_BLOCK_VOLTAGE_DELTA
            : CCS_VOLTAGE_SOFT_START_BLOCK_CURRENT_CEILING,
            batteryVoltage);
    }

    if (candidateTargetVoltage > normalTargetVoltage)
        candidateTargetVoltage = normalTargetVoltage;

    _ccsVoltageSoftStartTargetVoltage = candidateTargetVoltage;
    applyCcsVoltageSoftStartMaxClamp();
    logCcsVoltageSoftStartClamp(false);

    completeCcsVoltageSoftStartIfStable(batteryVoltage, normalTargetVoltage);
}

static int16_t getCcsVoltageSoftStartTarget()
{
    updateCcsVoltageSoftStart();

    if (!_ccsVoltageSoftStartActive)
        return _ccs_params.TargetVoltage;

    return _ccsVoltageSoftStartTargetVoltage;
}

int16_t hardwareInterface_getInletVoltage(void)
{
    // we have no inlet voltage sensor.
    return _ccs_params.EvseVoltage;
}

int16_t hardwareInterface_getAccuVoltage(void)
{
    return _ccs_params.BatteryVoltage;
}

int16_t hardwareInterface_getChargingTargetVoltage(void)
{
    return getCcsVoltageSoftStartTarget();
}

int16_t hardwareInterface_getChargingTargetCurrent(void)
{
    return _ccs_params.TargetCurrent;
}

uint8_t hardwareInterface_getSoc(void)
{
    /* SOC in percent */
    return _ccs_params.soc;
}

bool hardwareInterface_getIsAccuFull(void)
{
    // Chademo: it make more sense that charger or the car should decide when to stop, and not the adapter?
    return _ccs_params.soc == 100;
}

void hardwareInterface_setPowerRelayOn(void)
{
    println("hardwareInterface_setPowerRelayOn");
}

void hardwareInterface_setPowerRelayOff(void)
{
    resetCcsVoltageSoftStart();
    println("hardwareInterface_setPowerRelayOff");
}

void hardwareInterface_setStateB(void)
{
    resetCcsVoltageSoftStart();
    println("hardwareInterface_setStateB");
    DigIo::state_c_out_inverted.Set();
}

void hardwareInterface_setStateC(void)
{
    println("hardwareInterface_setStateC");
    DigIo::state_c_out_inverted.Clear();
}

static bool _connectorLocked = false;

void hardwareInterface_lockConnector(void)
{
    println("[ccs] Lock charging plug");
    _connectorLocked = true;
}

void hardwareInterface_unlockConnector(void)
{
    println("[ccs] Unlock charging plug");
    _connectorLocked = false;
}

bool hardwareInterface_isConnectorLocked(void)
{
    return _connectorLocked;
}

bool hardwareInterface_stopChargeRequested()
{
    return _global.powerOffPending;
}


