/* Hardware Interface module */

#include "ccs32_globals.h"

#define CCS_VOLTAGE_SOFT_START_MARGIN_VOLTS 5
#define CCS_VOLTAGE_SOFT_START_STABLE_CURRENT_AMPS 2
#define CCS_VOLTAGE_SOFT_START_STABLE_CURRENT_MS 1000
#define CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT 100

static bool _ccsVoltageSoftStartActive = false;
static bool _ccsVoltageSoftStartRamping = false;
static bool _ccsVoltageSoftStartComplete = false;
static int16_t _ccsVoltageSoftStartTargetVoltage = 0;
static int16_t _ccsVoltageSoftStartNormalVoltage = 0;
static uint32_t _ccsVoltageSoftStartStableCurrentSinceMs = 0;
static uint32_t _ccsVoltageSoftStartLastRampMs = 0;

static void resetCcsVoltageSoftStart()
{
    _ccsVoltageSoftStartActive = false;
    _ccsVoltageSoftStartRamping = false;
    _ccsVoltageSoftStartComplete = false;
    _ccsVoltageSoftStartTargetVoltage = 0;
    _ccsVoltageSoftStartNormalVoltage = 0;
    _ccsVoltageSoftStartStableCurrentSinceMs = 0;
    _ccsVoltageSoftStartLastRampMs = 0;
}

static int16_t getCcsVoltageSoftStartTarget()
{
    int16_t normalTargetVoltage = _ccs_params.TargetVoltage;
    int16_t batteryVoltage = _ccs_params.BatteryVoltage;

    if (_ccsVoltageSoftStartComplete && _ccs_params.TargetCurrent == 0 && _ccs_params.EvseCurrent == 0)
        resetCcsVoltageSoftStart();

    if (normalTargetVoltage <= 0 || batteryVoltage <= 0)
        return normalTargetVoltage;

    int16_t initialTargetVoltage = batteryVoltage + CCS_VOLTAGE_SOFT_START_MARGIN_VOLTS;
    if (initialTargetVoltage >= normalTargetVoltage)
        return normalTargetVoltage;

    if (_ccsVoltageSoftStartNormalVoltage != 0 && _ccsVoltageSoftStartNormalVoltage != normalTargetVoltage)
        resetCcsVoltageSoftStart();

    if (!_ccsVoltageSoftStartActive && !_ccsVoltageSoftStartComplete)
    {
        _ccsVoltageSoftStartActive = true;
        _ccsVoltageSoftStartNormalVoltage = normalTargetVoltage;
        _ccsVoltageSoftStartTargetVoltage = initialTargetVoltage;
        println("[cha] CCS voltage soft-start active: batt=%dV target=%dV normal=%dV",
            batteryVoltage,
            _ccsVoltageSoftStartTargetVoltage,
            normalTargetVoltage);
    }

    if (!_ccsVoltageSoftStartActive)
        return normalTargetVoltage;

    if (!_ccsVoltageSoftStartRamping)
    {
        if (_ccs_params.EvseCurrent >= CCS_VOLTAGE_SOFT_START_STABLE_CURRENT_AMPS)
        {
            if (_ccsVoltageSoftStartStableCurrentSinceMs == 0)
                _ccsVoltageSoftStartStableCurrentSinceMs = system_millis;

            if ((system_millis - _ccsVoltageSoftStartStableCurrentSinceMs) >= CCS_VOLTAGE_SOFT_START_STABLE_CURRENT_MS)
            {
                _ccsVoltageSoftStartRamping = true;
                _ccsVoltageSoftStartLastRampMs = system_millis;
                println("[cha] CCS voltage soft-start current stable -> ramping target");
            }
        }
        else
        {
            _ccsVoltageSoftStartStableCurrentSinceMs = 0;
        }
    }

    if (_ccsVoltageSoftStartRamping)
    {
        uint32_t elapsedMs = system_millis - _ccsVoltageSoftStartLastRampMs;
        if (elapsedMs >= CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT)
        {
            int16_t rampVolts = elapsedMs / CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT;
            _ccsVoltageSoftStartLastRampMs += rampVolts * CCS_VOLTAGE_SOFT_START_RAMP_MS_PER_VOLT;

            if (_ccsVoltageSoftStartTargetVoltage + rampVolts >= normalTargetVoltage)
            {
                _ccsVoltageSoftStartTargetVoltage = normalTargetVoltage;
                _ccsVoltageSoftStartActive = false;
                _ccsVoltageSoftStartComplete = true;
                println("[cha] CCS voltage soft-start complete: target=%dV", normalTargetVoltage);
            }
            else
            {
                _ccsVoltageSoftStartTargetVoltage += rampVolts;
            }
        }
    }

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


