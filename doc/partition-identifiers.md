# Amiga RDB Partition Identifiers (DOS Types)

Amiga partitions utilize a Rigid Disk Block (RDB) and are explicitly identified by a 32-bit (4-byte) DOS Type. This identifier dictates which filesystem driver the Amiga operating system uses to mount the partition. It is often represented as a 4-character ASCII string (like `DOS\3` or `PFS3`) or as an 8-digit hexadecimal value (like `0x444F5303`).

The table below outlines the partition IDs for all common, supported filesystems on classic and modern Amiga systems.

| DOS Type (ASCII) | Hex Identifier | Description | File System Type |
|------------------|----------------|-------------|------------------|
| `DOS\0` | `0x444F5300` | Original File System | OFS (Non-international) |
| `DOS\1` | `0x444F5301` | Fast File System | FFS (Non-international) |
| `DOS\2` | `0x444F5302` | OFS International | OFS (Allows foreign characters) |
| `DOS\3` | `0x444F5303` | FFS International | FFS (Allows foreign characters) |
| `DOS\4` | `0x444F5304` | OFS Directory Cache | OFS (Dir Cache, International) |
| `DOS\5` | `0x444F5305` | FFS Directory Cache | FFS (Dir Cache, International) |
| `DOS\6` | `0x444F5306` | OFS Long Name (LNFS) | OFS (Long filenames, AmigaOS 3.2+) |
| `DOS\7` | `0x444F5307` | FFS Long Name (LNFS) | FFS (Long filenames, AmigaOS 3.2+) |
| `SFS\0` | `0x53465300` | Smart File System (v1) | SFS (Initial version) |
| `SFS\2` | `0x53465302` | Smart File System (v2) | SFS (Highly recommended for large drives) |
| `PFS0` | `0x50465300` | Professional File System | PFS / PFS2 (Floppy/Basic) |
| `PFS1` | `0x50465301` | Professional File System | PFS (Hard disk) |
| `PFS2` | `0x50465302` | Professional File System | PFS3 (Legacy identifier) |
| `PFS\3` | `0x50465303` | Professional File System | PFS3 (Standard identifier) |
| `PFS3` | `0x50465333` | Professional File System | PFS3 (Modern standard) |
| `JXFS` | `0x4A584604` | JX File System | JXFS (AmigaOS 4/MorphOS) |
| `SWAP` | `0x53574150` | Amiga Swap Partition | SWAP (Memory/Virtual) |

## Alien Filesystems
Sometimes RDB structures can contain foreign filesystems, which are typically ignored by native AmigaOS but can be accessed via 3rd-party handlers.
- `MAC\0` (`0x4D414300`): Mac OS
- `LNX\0` (`0x4C4E5800`): Linux Native
- `SWP\0` (`0x53575000`): Linux Swap
- `FAT\0` (`0x46415400`): FAT12/16
- `FAT\1` (`0x46415401`): FAT32
- `NTFS` (`0x4E544653`): Windows NTFS

*Note: The `amidisk` tool will identify all of these partitions natively when scanning an RDB layout.*
