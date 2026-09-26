# HIL — AP/OTA/Profile-Change Sequence

## Objective

Exercise the complete maintenance sequence on the ESP32-C3: long-press AP
activation, authenticated OTA upload, the hard ten-minute AP window, and
runtime profile change.

## Current AP contract

The firmware currently uses an open local AP. OTA upload authorization is
separate and requires the provisioned OTA password. A WPA2 AP is therefore
**NOT VERIFIED / NOT IMPLEMENTED** by the current source contract; this test
must not report WPA2 acceptance.

## Procedure

1. Provision an OTA password of 12–64 printable ASCII characters.
2. Trigger the AP with the GPIO10 long press and verify the AP starts.
3. Verify the AP is stopped no later than ten minutes after startup,
   regardless of client activity.
4. Perform a valid authenticated OTA upload and verify reboot.
5. Attempt an upload with an incorrect password; verify it is rejected.
6. POST `/profile` without the cable-confirmation header; verify HTTP 409.
7. Replace the physical cable set, then POST `/profile` with the confirmation
   header and verify reboot into the selected roster.
8. Record AP mode, OTA result, reboot reason, profile ID, and BLE descriptor IDs.

## Acceptance

The hard AP-window and OTA-password controls must pass. WPA2 AP acceptance is
a separate unresolved hardware/security requirement because the current
architecture intentionally exposes an open maintenance AP.
