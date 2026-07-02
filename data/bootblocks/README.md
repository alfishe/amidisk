# Amiga Bootblocks Catalog

This directory contains reference standard Amiga bootblocks that can be injected into the first two sectors (1024 bytes) of an Amiga partition to make it bootable.

## Catalog

| Filename | Description | Usage |
| :--- | :--- | :--- |
| `std_boot_1x.bin` | Standard AmigaOS 1.x Bootcode | Primarily used for floppies or very early AmigaOS 1.2/1.3 hard drive setups. Compatible with older ROMs that require legacy `FindResident` and `Trackdisk` structures. |
| `std_boot_2x3x.bin` | Standard AmigaOS 2.x/3.x Bootcode | The default bootcode used by standard `Install` commands from OS 2.0 through 3.2. This is the recommended bootcode for modern RDB setups using FFS, SFS, or PFS3. |

## Bootblock Anatomy
For a bootblock to be recognized by the Amiga ROM, it must be exactly 1024 bytes and follow this exact layout:

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| `0x00` (0) | 4 bytes | `Signature` | Identifies the filesystem (`DOS\0`, `DOS\1`, `DOS\3`, `SFS\0`, etc). |
| `0x04` (4) | 4 bytes | `Checksum` | Calculated so the 32-bit sum of the entire 1024 bytes equals `0xFFFFFFFF`. |
| `0x08` (8) | 4 bytes | `Root Block` | Pointer to the root block of the partition (used primarily by legacy FFS/OFS bootcodes). |
| `0x0C` (12)| 1012 bytes| `Boot Code` | The executable Motorola 68000 machine code. |

*(Note: The `.bin` files provided in this catalog **only contain the raw Boot Code payload** (starting at offset 12), not the full 1024 bytes. When injecting these into a partition, you must dynamically write the Signature at `0`, reserve `4` and `8`, append the `.bin` payload at `12`, pad the rest of the 1024 bytes with `\x00`, and then finally calculate and inject the Checksum at `4`!)*
