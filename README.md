# JK BMS monitor on LILYGO T-Display

ESP-IDF firmware for the LILYGO TTGO T-Display that reads
JK BMS units over Bluetooth LE, shows them on the display and uploads their status
to a small web dashboard.

The BLE protocol is ported from [esphome-jk-bms](https://github.com/syssi/esphome-jk-bms).

## Screens

Left button: previous page. Right button: next page. Hold left for 1.5 s to pick
another BMS.

| Summary | All cells | Pack |
|:---:|:---:|:---:|
| ![summary](docs/screenshots/summary.png) | ![cells](docs/screenshots/cells.png) | ![pack](docs/screenshots/pack.png) |
| Pack voltage, current, power, state of charge, the largest cell voltage difference, charge / discharge / balancer state | Voltage of every cell, highest in red, lowest in blue | Temperatures, charge cycles, balancing current |

| Cell | Errors | Settings | Log |
|:---:|:---:|:---:|:---:|
| ![cell](docs/screenshots/cell.png) | ![errors](docs/screenshots/errors.png) | ![settings](docs/screenshots/settings.png) | ![log](docs/screenshots/log.png) |
| Voltage and resistance of one cell, the selected cell in the bar chart | Active BMS alarms (empty when there are none) | BMS settings: switches, protection voltages, currents, temperatures | The last 50 events of the BMS logbook, newest first |

| Pick BMS | Connecting | No BMS |
|:---:|:---:|:---:|
| ![pick](docs/screenshots/pick.png) | ![connecting](docs/screenshots/connecting.png) | ![no bms](docs/screenshots/no_bms.png) |
| Shown when several BMS are around or after holding the left button | While searching and connecting; errors appear below the spinner | No BMS found: it is off, away, or the JK app on a phone is connected to it |

The last page, **Board**, shows the firmware version, the Wi-Fi network and the address of the
firmware update page. Without a BMS it is one right-button press away from the NO BMS screen.

The header turns red and blinks while a BMS reports an active error. The Wi-Fi icon in the
header is crossed out in red while the board is offline.

## How it works

- **Picking a BMS.** At the first boot the board searches for JK BMS advertisements. A single
  BMS is selected automatically, several are listed. The choice is saved to flash.
- **Always on.** The display stays on and Wi-Fi stays connected; the board is meant to be
  powered from USB.
- **Reading.** The BMS stops streaming about 30 s after connecting, so it is reconnected once a
  minute for fresh data. It beeps on every command, so the firmware sends as few as possible.
- **Upload.** The status is POSTed as JSON over Wi-Fi as soon as the BMS streams after each
  (re)connection, about once a minute. The first upload after power-on carries the settings
  and the whole logbook; later ones carry new logbook events, and the settings every 10th time.
  Without a BMS the board still reports every 2 minutes, so the dashboard shows it offline.
  When the first full report of the chosen BMS is delivered, a green check mark with
  "Відправлено" shows for 10 s; if it fails, a red cross shows instead, until a later upload
  succeeds. A button press closes it.
- **Firmware update.** Open http://espbms.local/ (mDNS) or http://&lt;board IP&gt;/ from the same
  network, choose `build/jkbms.bin` and press Upload; a progress bar shows on the page and on
  the display. The new firmware goes to the other of two OTA slots. If it does not get back on
  Wi-Fi after the reboot, the next reset rolls back to the previous firmware.

## Dashboard

[`web/`](web) is a small Flask app that receives the uploads and shows them after a login:
pack values, cells, settings, the full logbook (kept beyond the 50 events the BMS stores)
and a 48 h chart. See `web/start.sh` and `web/restart.sh` for running it with gunicorn.

## Building

ESP-IDF 5.5, target `esp32`:

```sh
idf.py build flash monitor
```

The first flash goes over USB (it writes the bootloader and the partition table with the two
OTA slots); later updates can use the web page.

Secrets are not part of this repository. `main/CMakeLists.txt` reads them from
`../secrets/jkbms/firmware.env` (environment variables with the same names take precedence):

```sh
ENV_JKBMS_UPLOAD_URL=http://<server>:<port>/api/status
ENV_JKBMS_API_TOKEN=<token, same as JKBMS_API_TOKEN of the dashboard>
ENV_JKBMS_WIFI_SSID=<Wi-Fi where the board runs>
ENV_JKBMS_WIFI_PASS=<password>
ENV_JKBMS_WIFI_SSID1=<optional second network, tried next>
ENV_JKBMS_WIFI_PASS1=<password>
```

The dashboard reads `JKBMS_API_TOKEN`, `JKBMS_WEB_USER`, `JKBMS_WEB_PASSWORD` and `SECRET_KEY`
from `web/.env`.

### Screenshots

The screenshots above are taken from the device itself: build with
`CONFIG_UI_SCREENSHOT_CONSOLE=y` and run

```sh
python tools/screenshots.py /dev/ttyACM0 docs/screenshots
```

The BMS name and MAC address in the pick list are replaced with made-up ones.
