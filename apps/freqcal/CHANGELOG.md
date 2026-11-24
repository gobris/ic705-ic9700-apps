# FREQCAL changelog

## v1.1 - 2026-11-24
- RWM: windows with a keyed carrier (1 Hz pulses, Morse ID, transmitter off) are skipped with
  `carrier keyed (pulses/CW), waiting`; measuring goes on by itself when the plain carrier returns.
- RWM 10 Hz pulses: the coarse search now prefers the carrier line closest to the expected tone over
  pulse sidebands of almost the same strength, so these minutes still measure correctly.
- New setting `maxcv` (max amplitude variation of a steady carrier, default 0.6).
- Log: new `ampvar` column.
- README: WWV dials tested on air (2.5-25 MHz), RWM schedule and how FREQCAL behaves in each part.
- Tests: pulse and CW scenarios added to `test/freqcal/run.sh`.

## v1.0 - 2026-11-23
First release, tested on an IC-705 E with SQ6EMM firmware 20261119v14.
- Station picked from the dial: WWV/BPM, RWM, CHU or a GPSDO's 10 MHz; tuning hint on other frequencies.
- Coarse search within +-1.5 ppm of the reference, zoom DFT on the strongest carrier over 60 s windows.
- Two carriers within 6 dB (WWV + WWVH/BPM) skip the window; the carrier is searched again every 3 windows.
- Median of the last windows, advice `INCREASE / DECREASE REF ~x %` (IC-705: about 0.1 ppm per 1 %),
  and the error in Hz at 144 and 432 MHz.
- Optional step calibration (`cal=`) that detects the REF Adjust change by itself.
- Log file `FREQCAL.TXT`.
