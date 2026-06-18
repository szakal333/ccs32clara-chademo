# Project instructions for Codex

This repository contains firmware for a CCS-to-CHAdeMO adapter.

## Safety-critical rules

- This is high-voltage EV charging firmware.
- Prefer minimal, high-confidence changes.
- Do not bypass safety logic.
- Do not force CCS current above the CHAdeMO car RequestCurrent.
- The adapter may limit current downward, but must not request more current than the vehicle asks for.
- Do not change D1/D2 relay sequencing, contactor sequencing, CableCheck, PreCharge, PowerDelivery, WeldingDetection, or SessionStop behavior unless the task explicitly requires it and the change is reviewed.
- Preserve safe shutdown behavior.
- Preserve the existing STOP button behavior:
  - short STOP during ChargingLoop changes current level
  - 5 seconds STOP triggers normal stop
  - 10 seconds STOP triggers forced power off

## Existing important fixes

- ORLEN compatibility fix:
  - in ccs/ipv6.cpp, after SDP response, use the SDP response Ethernet source MAC as EVSE MAC for TCP.
- TCP robustness fix:
  - in ccs/tcp.cpp, preserve TCP SYN retry behavior.

## Current known working behavior

- Manual current levels:
  - 120A -> 100A -> 80A -> 60A -> 40A -> 120A
- Switching is done with short STOP press during ChargingLoop.
- GitHub Actions is the source of truth for successful firmware build.
- Do not claim a firmware version is safe until:
  - it builds successfully in GitHub Actions
  - the diff is reviewed
  - a real adapter log is checked after test charging

## Build and validation

- Keep code comments and firmware logs in English.
- Keep GitHub commit/PR/release descriptions understandable in Polish if requested by the maintainer.
- After code changes, check the diff carefully.
- If local build tools are unavailable, still create a PR and rely on GitHub Actions.
- Never remove diagnostic logs that are useful for CCS/CHAdeMO troubleshooting unless explicitly asked.
