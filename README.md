# vip_af (Plugify)

[VIP] Anti Flash: a player_blind hook clears the flash duration of a VIP whose 'antiflash' group value is > 0, with the original's four modes (teammate / self / both / any). Ported from the Pisex VIP core module VIP_AntiFlash (vip_af). Requires the vip core.

Plugify port (fork) of **VIP_AntiFlash ([VIP] Anti Flash)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.1
- Plugify dependencies: s2sdk (>=2.18.2), vip (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `vip_af-1.2.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_af`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

