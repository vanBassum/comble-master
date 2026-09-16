# The USB COM port

One Comble COM port on the WT-SC01 Plus, over the board's single USB-C, plus whatever it
takes to keep `idf.py flash` working on that same cable. The model is in place; no USB
code exists yet.

Related: [a COM port costs two IN endpoints](../reasoning/2026-09-16-16h26-3-a-com-port-costs-two-in-endpoints-so-how-many-there-are-is-silicon-not-a-setting.md) ·
[the PHY is a runtime mux](../reasoning/2026-09-16-16h26-the-s3-usb-phy-is-a-runtime-mux-and-only-the-rom-is-bound-by-the-efuse.md) ·
[esptool's reset is a contract](../reasoning/2026-09-16-16h26-2-esptools-reset-is-a-contract-the-device-implements-not-something-done-to-it.md)

## Settled

- **One port.** `BoardConfig::USB_COM_PORTS = 1`. The chip could carry two; the second is
  not spent until the flashing question below is answered. Zero on the DevKit and the C3.
- **The host owns many bonds and one assignment.** `UsbPortManager` maps port → slave
  address and asks the radio nothing. `BleHostManager` keeps the roster. The panel is the
  only place the two are read together.
- **No Windows driver.** A CDC-ACM function behind an Interface Association Descriptor
  (device class `0xEF/0x02/0x01`) binds the in-box `usbser.sys` on Windows 10 and later,
  and `esp_tinyusb`'s own descriptors already have that shape. Nothing to install, nothing
  to sign.
- **Not DFU.** It needs the `USB_PHY_SEL` eFuse on the S3 *and* a WinUSB/Zadig install on
  Windows. The eFuse would destroy this board's flashing path permanently.

## The open question: how a developer flashes

Installing TinyUSB hands the connector from USB Serial/JTAG to USB-OTG, so esptool loses
the hardware reset it uses today. The trigger is solved — the device can honour esptool's
own DTR/RTS contract and reboot itself — but the ROM re-enumerates under a different
VID/PID, so the COM port number changes and something host-side has to find it. Four
candidates, none implemented:

| | Path | `idf.py monitor` | Unbricks |
|---|---|---|---|
| **A** | Trigger → mux back to USB Serial/JTAG → the board is `303A:1001`, i.e. exactly what it is today | yes | yes |
| **B** | Trigger → `FORCE_DOWNLOAD_BOOT` + restart with the mux left on OTG → ROM at `303A:0009` | ROM CDC only | yes |
| **C** | A `CdcSessionLink` beside `WsSessionLink`/`RelaySessionLink`; firmware over `partition write` on the Comble port | over the same link | no |
| **D** | OTA over Wi-Fi — already built and proven | browser console | no |

- [ ] **Pick one.** A lands the board in the configuration the ROM, esptool and
      `idf.py monitor` already support without a single trick, and is the current
      favourite. C is the best architectural fit — `SessionLink` already has two
      transports — but costs the second CDC slot and cannot recover a bad image, so it
      wants A or B underneath it either way.
- [ ] **Host-side step.** All four need the same thing: trigger, wait, rescan, flash the
      port that appeared. `idf_ext.py` at the project root is the supported hook
      (`tools/idf_py_actions/README.md`), and its `global_action_callbacks` can rewrite
      `--port` so the command stays `idf.py flash`. `ESPTOOL_WRAPPER` also works but is
      undocumented.
- [ ] **Do not put the trigger on the data port.** A bare RTS falling edge is what any
      Windows program produces when it closes a COM port, so user software talking to a
      slave could reboot the host into download mode by accident. Either match the full
      pulse train inside a time window, or trigger from the panel / a WebSocket command
      instead.

## To prove on hardware

- [ ] **B's assumption:** that the ROM honours an app-set PHY mux and comes up in download
      mode on USB-OTG. The RTC bits survive `esp_restart()`; whether the ROM re-initialises
      them is not answerable from the source. Ten minutes on the bench.
- [ ] **Whether `USBDC_PERSIST_ENA` can be made sound** by presenting descriptors identical
      to the ROM's. Only possible in the one-port configuration — which is the one we are
      building — and it would remove the COM-port-change problem entirely. Worth half an
      hour before discarding.
- [ ] **That the connector really is native USB.** Both devices on this machine enumerate
      as `303A:1001` "USB JTAG/serial debug unit" and there is no CH340 anywhere, so it is —
      but confirm which of the two is the S3 rather than the C3 slave.

## Costs to expect

- `esp_tinyusb` publicly `REQUIRES fatfs vfs`. The fatfs component was deliberately out of
  this build after the `www` partition went away; it comes back.
- New `CONFIG_TINYUSB_*` lines in the defaults will trip the sdkconfig drift guard on an
  existing tree. That is the guard working — delete the generated `sdkconfig` and re-run
  `set-target`.
- `CONFIG_ESP_PHY_ENABLE_USB` already defaults to `y` on the S3, so Wi-Fi and USB coexist
  with no change; it costs a little Wi-Fi performance. The PM lock USB wants is
  `ESP_PM_NO_LIGHT_SLEEP`, which this project does not use, so DFS is not a problem.
