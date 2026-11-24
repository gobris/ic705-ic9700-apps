# IC-705 / IC-9700 apps

Small apps for the **Icom IC-705 and IC-9700** that run on the radio itself, on the ICAP app
platform of the unofficial custom firmware by **Dawid SQ6EMM**.

| App | What it does | Status |
|---|---|---|
| [FREQCAL](apps/freqcal/) | Measures the reference oscillator error on a standard-frequency station (WWV/BPM, RWM, CHU or a GPSDO's 10 MHz) and tells you which way, and roughly how much, to change REF Adjust | Tested on IC-705 E, firmware 20261119v14 |

## Requirements
- The custom firmware by SQ6EMM with ICAP support, and the matching SD-card app bundle:
  [sq6emm/icom-custom-firmware](https://github.com/sq6emm/icom-custom-firmware)
- An SD card in the radio. Apps go into the `Apps` folder (`\IC-705\Apps\` or `\IC-9700\Apps\`).

## Install an app
1. Download the app's zip from [Releases](../../releases).
2. Copy the `.APP` and `.CFG` files into the radio's `Apps` folder.
3. Start it from **MENU > SET > Apps**.

If a new app does not show up in the Apps list, move an unused app (e.g. `CLOCK.APP`,
`SYSINFO.APP`) out of the folder; with the full firmware bundle the list seemed to be full.

## Build from source
Each app has a `build.sh`. You need `arm-none-eabi-gcc`, `python3` and the
[ICAP SDK](https://github.com/sq6emm/icap-sdk), cloned next to this repository (or set `ICAP_SDK`):

```sh
git clone https://github.com/sq6emm/icap-sdk
git clone https://github.com/gobris/ic705-ic9700-apps
ic705-ic9700-apps/apps/freqcal/build.sh
```

The `test/` folder has host simulations (plain `gcc`, no radio needed) for checking changes.

## Credits
Everything here stands on the work of **Dawid SQ6EMM**:
- the [custom firmware](https://github.com/sq6emm/icom-custom-firmware) for the IC-705 and IC-9700, which
  added transverter support, the decoders and the ICAP app platform, and
- the [ICAP SDK](https://github.com/sq6emm/icap-sdk) (`icap.h`, build tools and reference apps) these apps
  are built with.

Many thanks, Dawid, for opening up these radios and for publishing the SDK so quickly. Discussion of the
firmware (in Polish and English) is on [mikrofale.cafe](https://mikrofale.cafe/showthread.php?tid=1296).

## Disclaimer
These apps are unofficial and not affiliated with or endorsed by Icom. They run on unofficial firmware;
use them at your own risk. The apps only read radio state and audio through the ICAP API and never
change radio settings.

## License
MIT, see [LICENSE](LICENSE). The ICAP SDK is MIT licensed by Dawid SQ6EMM.
