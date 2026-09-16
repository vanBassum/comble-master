# Next up

**Active work only.** Rewritten constantly, kept tiny, and an item is *removed* when it
lands or is dropped — never ticked off in place. Everything else lives in
`docs/backlog/` (work for later) or `docs/reasoning/` (why things are the way they are).
If a fact wants to survive, it does not belong in this file.

Last updated 2026-09-16.

## Now

**One USB COM port, and it is the board's number rather than a setting.** A CDC-ACM
function costs two IN endpoints and the S3 has four, so two is the ceiling and the first
host ships one of them. `usb.ports`, `usb set` and the panel's −/+ stepper are gone;
`BoardConfig::USB_COM_PORTS` is the capability and `UsbPortManager` is written for N. The
host still pairs and remembers many slaves — one of them is on the port. **No USB code
exists yet**, and the open question is how `idf.py flash` keeps working once TinyUSB takes
the connector from USB Serial/JTAG.
→ [`backlog/2026-09-16-usb-com-port.md`](backlog/2026-09-16-usb-com-port.md) ·
[`reasoning/…two-in-endpoints…`](reasoning/2026-09-16-16h26-3-a-com-port-costs-two-in-endpoints-so-how-many-there-are-is-silicon-not-a-setting.md)

**Outstanding: the DevKit and C3 SuperMini boards cannot be configured in this fork.** The
root defaults assert `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` — added for the S3's
PSRAM — and the drift guard correctly refuses a chip with no PSRAM to allocate from. Both
boards carry a `USB_COM_PORTS = 0` that has therefore never been compiled.

**The frontend is one SPA again, and the module mechanism is gone.** `UiManager`,
`UiModule`, the four `UiModule` declarations, `frontend/modules/`, `shell-contract/` and
`src/shell/` were all deleted; the pages are back in `frontend/src/pages/` with the LED
demo as `HomePage`. A relay shell serves a Strux device's page whole, which is the
fallback it already had. Nine framework managers now, not ten.
→ [`reasoning/…the-seam-was-the-product…`](reasoning/2026-09-15-12h30-a-mechanism-with-no-second-consumer-is-a-seam-the-template-pays-for.md)

**Outstanding: nothing has been driven on a device since the revert.** Both halves build
(ESP32 app 0x116940, www 143 KB in three files) and `tsc -b` is clean, but the page has
not been opened over the LAN or through the relay. Worth checking on the bench: the
sidebar survives at every width, the LED toggle round-trips, `partition status` renders
on the firmware page, and a cold load fetches all three assets without a reset.

**Outstanding: the bench C3 needs its WiFi back.** Reflashing it to 0.0.7 left NVS
without a network, so it came up on `Strux-AP-9EA851` and the relay is disabled.
Reprovision before using it to verify anything above.

**Known gap:** an upload does not surface the device's own flash position, only the
browser's upload progress.

**Putting the relay in production** — live at `https://strux.vanbassum.com`, behind
Traefik and Authentik. A device must be approved and must present its own token, or the
upgrade is refused with a 403, and pairing is one click in the dashboard. Step 9 —
secrets out of `settings list` — is the last one before the plan calls it
production-ready.
→ [`backlog/2026-08-05-relay-in-production.md`](backlog/2026-08-05-relay-in-production.md)

**Telemetry works end to end** — a manager records a point, the relay writes it to
InfluxDB, and it queries back tagged by device. No buffering yet: a point taken while
the relay is down is dropped. That and the other open ends are listed in
→ [`backlog/2026-08-05-telemetry.md`](backlog/2026-08-05-telemetry.md)
