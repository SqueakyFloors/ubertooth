# Credits

This repository is a community fork of
[Project Ubertooth](https://github.com/greatscottgadgets/ubertooth) by
[Great Scott Gadgets](https://github.com/greatscottgadgets). All credit for
the original hardware, firmware, and host software goes to its authors and
contributors. The software is released under the GPLv2; see [COPYING](COPYING).

## Original authors

- Michael Ossmann ([@mossmann](https://github.com/mossmann)), project creator
  and hardware designer
- Dominic Spill ([@dominicgs](https://github.com/dominicgs)), Classic Bluetooth
  (Basic Rate) support, releases through 2017
- Mike Ryan ([@mikeryan](https://github.com/mikeryan)), Bluetooth Low Energy
  (BLE) sniffing engine and releases from 2018

## Upstream and community work in this fork

These changes were contributed outside this repository and incorporated here.
Details are in [CHANGELOG.md](CHANGELOG.md).

| Contributor | Contribution | Source |
|---|---|---|
| Ryan Govostes ([@rgov](https://github.com/rgov)) | Capture to stdout for `ubertooth-btle` | [upstream #362](https://github.com/greatscottgadgets/ubertooth/pull/362) |
| Rhys Perry ([@rhysperry111](https://github.com/rhysperry111)) and Arnaud Rebillout ([@elboulangero](https://github.com/elboulangero)) | Python 3.12 and modern setuptools support | [upstream #524](https://github.com/greatscottgadgets/ubertooth/pull/524) |
| Michael Ossmann ([@mossmann](https://github.com/mossmann)) | Firmware-build `ubertooth-dfu` variable fix | [upstream #485](https://github.com/greatscottgadgets/ubertooth/pull/485) |
| [@stryngs](https://github.com/stryngs) | Scapy `PcapReader` update | [upstream #519](https://github.com/greatscottgadgets/ubertooth/pull/519) |
| oliver ([@Oliver0804](https://github.com/Oliver0804)) | BLE CRC filtering and verification | [upstream #550](https://github.com/greatscottgadgets/ubertooth/pull/550) |
| Gabriele Gristina ([@matrix](https://github.com/matrix)) | Firmware signal-strength channel bounds | [upstream #481](https://github.com/greatscottgadgets/ubertooth/pull/481) |
| Hillel Chimowicz ([@hllhll](https://github.com/hllhll)) and [@shapaz](https://github.com/shapaz) | Faster, more reliable Classic Bluetooth paging | [upstream #540](https://github.com/greatscottgadgets/ubertooth/pull/540) |
| [@Kielukel](https://github.com/Kielukel) and Piotr Wasylów | Adaptive Frequency Hopping (AFH) divide-by-zero fixes | [Kielukel/ubertooth](https://github.com/Kielukel/ubertooth) |

## Upstream contributors

Everyone who contributed to Project Ubertooth upstream, as listed by GitHub:

[@AdamLaurie](https://github.com/AdamLaurie) (Adam Laurie),
[@aholler](https://github.com/aholler) (Alexander Holler),
[@BG7YWL](https://github.com/BG7YWL),
[@cathyz64](https://github.com/cathyz64),
[@cpn18](https://github.com/cpn18),
[@dmaynor](https://github.com/dmaynor) (David Maynor),
[@dominicgs](https://github.com/dominicgs) (Dominic Spill),
[@dragorn](https://github.com/dragorn) (Mike Kershaw),
[@el-han](https://github.com/el-han) (Hannes Ellinger),
[@Etherealflux](https://github.com/Etherealflux) (Steven Haussmann),
[@grvvy](https://github.com/grvvy) (Jacob Graves),
[@jboone](https://github.com/jboone) (Jared Boone),
[@jcard0na](https://github.com/jcard0na) (Javier Cardona),
[@JelteF](https://github.com/JelteF) (Jelte Fennema-Nio),
[@jonasstein](https://github.com/jonasstein) (Jonas Stein),
[@jonathanwminer](https://github.com/jonathanwminer),
[@kxynos](https://github.com/kxynos) (Konstantinos Xynos),
[@lisaparty](https://github.com/lisaparty),
[@mar-kub](https://github.com/mar-kub) (Markus Kubertzki),
[@MichalLabedzki](https://github.com/MichalLabedzki),
[@micolous](https://github.com/micolous) (Michael Farrell),
[@miek](https://github.com/miek) (Mike Walters),
[@mikeryan](https://github.com/mikeryan) (Mike Ryan),
[@mnaberez](https://github.com/mnaberez) (Mike Naberezny),
[@mnhauke](https://github.com/mnhauke),
[@mossmann](https://github.com/mossmann) (Michael Ossmann),
[@Nerade](https://github.com/Nerade) (Marcel Pörner),
[@oxzi](https://github.com/oxzi) (Alvar),
[@r0wnin](https://github.com/r0wnin),
[@rsaxvc](https://github.com/rsaxvc) (Richard Allen),
[@samyk](https://github.com/samyk) (Samy Kamkar),
[@sgstair](https://github.com/sgstair) (Stephen Stair),
[@straithe](https://github.com/straithe) (Straithe),
[@tomsaul](https://github.com/tomsaul) (Tom Saul),
[@TorstenRobitzki](https://github.com/TorstenRobitzki) (Torsten Robitzki),
[@troth](https://github.com/troth) (Theodore A. Roth),
[@unsabi](https://github.com/unsabi),
[@whiterocker](https://github.com/whiterocker) (Chris Kilgour),
[@willcode](https://github.com/willcode) (Will Code),
[@xbarte14](https://github.com/xbarte14),
[@ZeroChaos-](https://github.com/ZeroChaos-) (Zero_Chaos).

## Fork maintainer

- Russell Wagner ([@SqueakyFloors](https://github.com/SqueakyFloors))

Contributions to this fork are listed in its
[contributor graph](https://github.com/SqueakyFloors/ubertooth/graphs/contributors).
If you contributed and are missing or misattributed here, please open an issue.
