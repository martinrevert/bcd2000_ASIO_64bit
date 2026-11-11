# ASIO Interface Definitions & Verification Host (Clean-Room)

## Clean-Room Interface Specification
The header files contained in `common/`:
* `asio.h`
* `iasiodrv.h`

are **independent, clean-room declarations** of the publicly specified COM ABI and structure geometry for Steinberg ASIO 2.3 on 64-bit Windows. They were authored specifically for this project to achieve binary interoperability with standard ASIO digital audio workstation hosts without incorporating, copying, or distributing proprietary code from Steinberg's closed SDK package.

The host utility located in `host/pc/hostsample.cpp` is an original test fixture developed specifically to verify COM registration, instantiation, buffer allocation, and real-time sine tone synthesis on the Behringer BCD2000.

## License
All code and definitions in this directory are distributed under the terms of the **GNU General Public License v3 or later (GPL-3.0-or-later)** with the **ASIO & Host Application Linking Exception** described in [`LICENSE`](../../LICENSE).

## Trademark Disclaimer
* **ASIO** is a registered trademark and software of Steinberg Media Technologies GmbH.

