# Virtual raw cache policy

This is a browser compatibility policy under the approval recorded in [M3](03-game-boots.md#approved-virtual-raw-cache-policy). It does not describe a recovered console disk. Executable provenance and the selected ISO do not supply disk identity, keys or persistent cache state.

## Authority and ownership

The compiled providers own the policy. Their sources participate in the build manifest fingerprint and compiled build identity. A selected ISO or worker message cannot replace it.

Each runtime owns its raw metadata and cache storage. Fresh runtimes start independently. Stop invalidates the generation and retires every owned worker before storage is discarded. There is no durable storage, host disk access or firmware restore contract. The selected ISO remains read only.

## Partition0 metadata window

The raw `\Device\Harddisk0\Partition0` handle exposes only the title cache metadata sector:

* Sector size: 512 bytes.
* Accessible byte interval: `[0x800, 0xa00)`.
* Supported transfer: exactly 512 bytes at explicit byte offset `0x800`.
* Initial contents: every byte zero. This is deliberately fresh virtual state, not a fabricated valid cache table.
* Initialization: the genuine guest validates the sector and writes its own table. Successful writes retain exactly those bytes for subsequent reads in that runtime.
* Other offsets, lengths and unsupported completion modes return explicit errors. No larger physical disk or valid partition table is implied.

The observed synchronous alert open admits generic read, generic write and synchronize, with read and write sharing. Read and write adapters require the owned live handle, checked guest spans and null event, APC and APC context. Outputs and register cleanup are published only after actual completion. Unsupported modes do not return invented byte counts.

## Cache partition mapping

Data export 40, `HalDiskCachePartitionCount`, reports **one** cache partition. Cache table index zero maps to **Partition3**, separately from the ordinary temporary **Partition1** namespace. Neither raw device is an alias for Partition1 or the selected optical source.

The virtual cache partition owns **16 MiB** of raw backing and has a **256 entry** namespace bound. Namespace allocation units are **16 KiB**. The formatter's geometry queries return a virtual geometry of 1,024 cylinders, one track per cylinder, 32 sectors per track and 512 bytes per sector, with fixed media type 12. The partition record reports offset zero, length 16 MiB, partition number three and no physical partition type, boot or recognized filesystem claim. These are coherent virtual bounds, not physical Xbox geometry.

Before formatting, the exclusive Partition3 raw handle admits the observed read/write and synchronize access with synchronous alert mode and no intermediate buffering (`0x18`). Its implemented data operation is an explicit offset write: a nonempty sector aligned length at most **64 KiB**, a sector aligned offset and a complete range within 16 MiB. Unsupported raw reads fail explicitly. Successful writes retain the actual bytes in owned backing.

## Virtual device descriptors

The emulated cache device has the nonunique model label `Browser Virtual Cache`. Its session serial is 20 uppercase hexadecimal characters encoded from ten independent secure random bytes. The serial is stable within one disposable session and regenerated for a fresh session. It is neither a physical console identifier nor a copy or encoding of the private virtual drive key.

Data ordinals 41 and 42 expose checked ANSI descriptors with owned NUL terminated buffers. Existing profile bytes remain unchanged. Secure entropy failure refuses installation before profile storage or ownership publication; no timestamp, constant serial or zero fallback is used. Neither descriptor reveals host identity or the selected ISO path. The compiled policy and implementation participate in the build fingerprint, not the unpredictable per session serial value.

## Guest format and mounted namespace

Partition3 begins zero filled and unformatted. The genuine guest queries geometry and partition size, obtains the platform system time for its volume ID, then writes the FATX header, allocation table and empty root. No valid format is seeded by the host.

The observed dismount control validates those real bytes before sealing raw transfers. The supported initial format has:

* A 4 KiB header with FATX magic, 32 sectors per cluster, root cluster one and a zero field at header bytes 16 and 17.
* A 4 KiB FAT16 allocation table beginning with `FFF8 FFFF`, with the remaining entries zero.
* A 16 KiB root cluster filled with `FF` bytes.
* 1,023 complete usable clusters after the header and FAT, with one cluster charged to the root. A fresh mounted namespace therefore reports **1,023 total and 1,022 free allocation units**.

Dismount is not treated as mount. Only after the raw handle closes may a namespace root open perform checked lazy mount of that validated empty format. Failed validation, premature root access and transfers through a sealed handle return errors without publishing a formatted namespace.

The lifecycle is deliberately one way. After mounting, all raw reopen and transfer requests are refused for that runtime. Namespace operations retain real bytes and enforce allocation accounting in the owned temporary filesystem. The retained format bytes are not advertised as a live raw representation of later namespace changes. This explicit exclusion avoids stale raw views; it is not a general FATX implementation, remount facility or arbitrary disk image reader.

Partition1 and Partition3 remain separate volumes even for identical relative paths. Root relative handles and the guest created `Z:` alias resolve to Partition3's own mounted namespace, not to Partition1. Writes cannot reach the ISO or another session.

Authenticated integrated runs have exercised raw metadata initialization, the count export, actual guest format writes, validated dismount, raw close, lazy mount, volume query and creation of the cache alias. This accepts the documented bounded cache path, not all M3 tasks.

## Separately approved virtual drive key

After startup genuinely reached data ordinal 323 `XboxHDKey`, the user separately approved a fresh random 16 byte virtual drive key. This is a disposable compatibility identity, not a recovered console key. It must remain stable for the owned session, differ between fresh sessions and disappear with the session's storage on Stop or replacement. It cannot establish compatibility with existing console encrypted data.

The provider must use operating system secure entropy in native contracts and Web Crypto in the browser, with explicit failure if secure entropy is unavailable. It must never substitute zeros, timestamps, a constant, an ISO derived value or player supplied bytes. Key material is not logged or retained in build artifacts. The compiled identity fingerprints the policy and implementation, not the unpredictable per session value.

Installation occurs only during serial cold configuration, before diagnostic import declaration or publication. The key occupies its own bounded guest data storage. Successful installation claims the existing prepublication runtime ownership anchor so another runtime cannot bind that image; failed entropy generation must publish neither storage nor ownership. Atomic image publication remains the binder's responsibility, not a promise of concurrent provider configuration.

Secure implementation, native sanitizer and threaded Wasm contracts now pass. The rebuilt authenticated package completes genuine key reads and HMAC calls across three fresh sessions, then advances to later initialization prerequisites. This accepts the exercised virtual key path, not further cryptographic exports, existing console data compatibility or full M3.

## Validation requirements

Meaningful native sanitizer and threaded Wasm contracts must cover exact transfer bounds, invalid output atomicity, rights, sharing, foreign sessions, cancellation, fresh state and isolation. The integrated target must authenticate its retained generated bodies and matching build artifacts before execution. Real ISO traces must identify the next actually exercised boundary and show complete worker and I/O teardown.

The current raw cache implementation is not acceptance of all M3 tasks, guest asset streaming, firmware reboot or graphics initialization.
