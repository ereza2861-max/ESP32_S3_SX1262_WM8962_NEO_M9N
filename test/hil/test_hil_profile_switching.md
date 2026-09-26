# HIL — Runtime Profile Switching and Cable Confirmation

## Objective

Verify that a runtime profile change is accepted only after the physical sensor
cables have been replaced, persists `sensor/profile`, and reboots into the
selected immutable roster.

## Preconditions

- ESP32-C3 sensor node flashed with the target firmware.
- Serial provisioning completed.
- Two physically distinct profile cable sets available.
- HIL fixture can observe reboot and BLE descriptor enumeration.

## Procedure

1. Boot with a known profile and record the advertised sensor descriptor IDs.
2. POST `/profile` without `X-Profile-Cables-Changed: true`; expect HTTP 409 and
   no profile change.
3. POST `{"profile":1}` with `X-Profile-Cables-Changed: true`; expect HTTP 200
   with `rebooting=true`.
4. Observe reboot and verify the new profile is persisted under `sensor/profile`.
5. Verify only the selected profile roster is present after reboot, plus the
   global RFID event descriptor.
6. Repeat for Profiles 0, 2 and 3.
7. Attempt a profile change while leaving the old profile's cabling connected;
   the firmware cannot detect physical cable state, so the fixture must mark
   this condition as an operator/setup failure rather than firmware success.

## Acceptance

The firmware must reject missing cable confirmation, persist the selected
profile, reboot, and expose the immutable source-level roster for that profile.
Physical cable presence is HIL evidence and is not inferred from a successful
HTTP response.
