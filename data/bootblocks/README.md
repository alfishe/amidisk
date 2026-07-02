# Amiga Bootblocks Catalog

This directory contains reference standard Amiga bootblocks that can be injected into the first two sectors (1024 bytes) of an Amiga partition to make it bootable.

## Catalog

| Filename | Description | Usage |
| :--- | :--- | :--- |
| `std_boot_1x.bin` | Standard AmigaOS 1.x Bootcode | Primarily used for floppies or very early AmigaOS 1.2/1.3 hard drive setups. Compatible with older ROMs that require legacy `FindResident` and `Trackdisk` structures. |
| `std_boot_2x3x.bin` | Standard AmigaOS 2.x/3.x Bootcode | The default bootcode used by standard `Install` commands from OS 2.0 through 3.2. This is the recommended bootcode for modern RDB setups using FFS, SFS, or PFS3. |

## Bootblock Anatomy
For a bootblock to be recognized by the Amiga ROM, it must be exactly 1024 bytes and contain:
1. **Signature**: `DOS\x` identifier at byte `0`.
2. **Checksum**: A 32-bit checksum at byte `4` covering the entire 1024 bytes.
3. **Executable Code**: Motorola 68000 machine code.

*(Note: The provided `.bin` files contain the standard Amiga `DOS\0` signature and a pre-calculated checksum. When injecting these into a live filesystem like `DOS\3` or `PFS\3`, the signature must be overwritten with the filesystem's specific identifier, and the 32-bit checksum must be dynamically recalculated!)*
