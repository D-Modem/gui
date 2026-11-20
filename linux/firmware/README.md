# The firmware of the MultiTech modems

| File | For | Size (octets) | SHA-1 | License | Source |
|---|---|---|---|---|---|
| mts_mt9234mu.fw | MultiTech MT9234MU and MT9234ZBA-USB (TI TUSB3410) | 13769 | `5537e93217c509129d5a870b9fe089cd58647070` | unknown | linux-firmware, before June 2026 (`WHENCE`: "Multi-Tech USB fax modems", "Licence: Unknown") |

The TUSB3410 of these modems has no firmware of its own. The
`ti_usb_3410_5052` driver of Linux downloads this file into the chip each
time you connect the modem. The driver looks for the file in
`/lib/firmware`. The MT9234ZBA-USB uses the name `mts_mt9234zba.fw` for the
same code.

To install it:

    sudo cp linux/firmware/mts_mt9234mu.fw /lib/firmware/
    sudo cp linux/firmware/mts_mt9234mu.fw /lib/firmware/mts_mt9234zba.fw

Then disconnect the modem and connect it again. `cmake --install` puts the
file in `share/dmodem-gui/firmware`. With `-DDMODEM_GUI_INSTALL_FIRMWARE=ON`,
it also puts the file in `/lib/firmware`.

Multi-Tech Systems, Inc. gave this file to the Linux kernel in 2010. The
file is TI's TUSB3410 UART firmware, compiled again by MultiTech with some
changes. The file has no copyright text and no license text. MultiTech did
not publish a license for it. In June 2026, linux-firmware removed it with
the other files that have an unknown license.

We include the file so that the modem operates without a search for it. The
AGPL-3.0 license of D-Modem and the commercial license do not apply to this
file. Multi-Tech Systems, Inc. and Texas Instruments keep their rights in it.
If you are the holder of these rights and you do not agree, tell us and we
will remove the file.
