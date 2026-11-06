# flashdrive-emulator

ESP32-S3 firmware that makes a network music share look like a USB flash drive to a car head
unit — and keeps that "drive" up to date over Wi-Fi.

```
Synology (SMB)  ──Wi-Fi──►  FAT32 on microSD  ──USB-OTG MSC──►  car head unit
```

The firmware writes the volume it is simultaneously exporting over USB. That is the whole
trick, and the whole risk: one FAT volume cannot have two writers, so this firmware is
primarily an **ownership state machine**, not a download manager.

---

## Status

Working and verified on silicon. The full library syncs byte-exactly with no manual steps.

| measured on this bench | value |
|---|---|
| whole sync (61,234,228 B, 4 files) | **155–166 s → 370–400 KB/s** |
| incremental pickup (9 new files into a 411 MB library) | 977 s, 0 reconnects, 10/10 sizes match |
| network stage alone (reads, no writes) | 482–554 KB/s |
| SD card write stage alone | **1430 KiB/s** |
| byte integrity vs NAS | md5 identical: 4/4 files ×2 runs (61 MB) **and 10/10 files (411 MB)**, 0 reconnects |
| interrupted transfers | resume from `.part` at the exact byte, after re-verifying the prefix |
| at the start of development | 8 KB/s (one socket timeout per request) |

Not yet tested: a library larger than one card, and playback on a real head unit (all testing
used a Linux host as the "car"). See [Limitations](#limitations-and-roadmap).

---

## Why this exists

Car head units read USB sticks. They do not read SMB, DLNA or a NAS. Rather than carrying a
stick that has to be re-flashed by hand, this box presents a permanently-plugged-in SD card as
the flash drive, and refills that card from the NAS whenever the car stops reading it.

The design constraint that shapes every decision: **the volume is owned by exactly one side at
a time.**

```
app owns  →  sync from SMB  →  hand to USB host  →  host reads music
   ↑                                                        │
   └────── TINYUSB_EVENT_DETACHED + quiet period ───────────┘
```

Taking the volume back from a live host is technically possible and always shows the head unit a
disk that vanished mid-album. So the firmware never takes it back on a schedule. A takeover
requires **both**:

1. a physical `TINYUSB_EVENT_DETACHED` (the car let go), and
2. a quiet interval — 3 s, measured from the last SCSI read that passed through a blockdev
   wrapper the firmware installs around the SD block device, so host activity is *measured*,
   not assumed.

---

## Hardware

ESP32-S3-WROOM-1-**N16R8** (16 MB flash, 8 MB octal PSRAM) on the ESP32-S3-CAM carrier, running
ESP-IDF v6.1. The pin table below is everything the firmware depends on; the full board reference
(`BOARD.md`, every value measured rather than datasheet-copied, including the trace audit that
proved the SD bus is 1-bit) lives alongside this project in the bench tree, not in this repo.

| function | pins | note |
|---|---|---|
| microSD (SDMMC slot 1) | CLK 39, CMD 38, D0 40 | **1-bit only** — D1/D2/D3 are not routed on this carrier |
| USB-OTG (native, MSC) | D− 19, D+ 20 | the S3's own USB; `USB-Serial-JTAG` shares these pins and must stay off |
| console | UART0, GPIO43/44 | on the *other* USB-C socket, so the log survives while OTG is in the car |
| status LED | WS2812, GPIO48 | the only user interface; see below |

### What the LED means

| LED | meaning |
|---|---|
| solid green | ready — host owns the volume, media is available |
| blinking blue | sync in progress (host not connected) |
| slow amber | waiting: Wi-Fi up, app owns the disk, nothing to copy |
| blinking red ×N | error; N = class → **2** Wi-Fi, **3** SMB session, **4** SMB I/O, **5** FAT/full, **6** no card |
| solid red | fatal at boot |

The LED is driven by its own task, because the sync loop blocks inside `poll()` and SD writes and
cannot blink from inside itself.

---

## Getting started

### Prerequisites

- **ESP-IDF v6.1.** The blockdev-backed MSC driver (`TINYUSB_MSC_BDL_SUPPORTED`) needs IDF ≥ 6.0.4.
  Note that ESP32-S3 has no `SOC_SDMMC_PSRAM_DMA_CAPABLE` — that is a chip fact, not a version one,
  and it is why the read buffer must sit in internal RAM.
- No component-manager manifest exists in this repo: `components/espressif__esp_tinyusb`,
  `components/espressif__led_strip` and `components/sahlberg__libsmb2` are **vendored in-tree and
  must stay there**. A clone that deletes them does not build.
- A microSD card, formatted **superfloppy FAT32** (see below).

### Format the card — superfloppy, no partition table

ESP-IDF's FatFs treats the whole card as one volume. With a normal MBR/partition layout it reads
the partition table at LBA0 and the mount fails.

```sh
sudo dd if=/dev/zero of=/dev/sdX bs=512 count=8 conv=fsync   # wipe LBA0..7
sudo mkfs.fat -F 32 -n CAR-BRIDGE /dev/sdX                   # needs dosfstools
```

A FAT32 volume *inside a partition* is right for a card you put in a PC and wrong here.

### Build

```sh
bash build.sh              # sources the IDF export, runs set-target esp32s3, builds
# → build/bridge.bin
```

### Flash

```sh
idf.py -p /dev/ttyACM0 flash monitor     # full image: bootloader @0x0, partitions @0x8000, app @0x20000
```

For firmware-only iteration the app partition alone is enough (this is what the bench scripts do):

```sh
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash 0x20000 build/bridge.bin
```

> An **app-only** flash leaves the board in the bootloader. Use `--after hard-reset`, or
> `esptool --chip esp32s3 -p PORT flash-id`, to actually start the app.

### Provision — credentials go to NVS, never to the card

The SD card *is* the volume the car and any PC can read, so a `config.ini` on it would be a
plaintext leak by design. Wi-Fi and SMB secrets live in **NVS on internal flash**. With NVS empty
the device drops to a console prompt sequence at boot:

```
=== provisioning (values are stored in NVS on internal flash) ===
SSID / Wi-Fi password / SMB server IP / SMB share / Path in share /
SMB user / SMB password / Filter / Folder on card / SMB dialect [return = auto]
```

Answer over the console port. Secrets are never echoed. To re-provision after a bad save, wipe
only NVS (no re-flash needed):

```sh
esptool --chip esp32s3 -p /dev/ttyACM0 erase-region 0x9000 0x6000
```

**Driving it from a script:** the prompts read one line at a time and treat CRLF as a single
terminator. A serial library's input-buffer flush (pyserial's `reset_input_buffer()`) discards the
prompt bytes still in flight and then waits forever for text it just threw away — read a line at a
time until the expected prompt appears. A feeder should print field **lengths**, never values, so
credentials stay out of shell history and logs.

> **SMB needs an IP.** ESP32-S3 Wi-Fi is 2.4 GHz only and there is no NetBIOS/LLMNR, so
> name resolution for a NAS won't happen. Give the server's address, and make sure the SSID can
> actually route to the LAN — an IoT VLAN with no LAN routing fails silently.

---

## Configuration reference

Everything is stored in the NVS namespace `bridge`. Blank/absent keys fall back to the defaults
shown, so a minimally-provisioned device works out of the box.

| NVS key | prompt | default | meaning |
|---|---|---|---|
| `ssid` | SSID | *(empty → provisioning)* | Wi-Fi network |
| `psk` | Wi-Fi password | *(empty)* | WPA key |
| `srv` | SMB server IP | *(empty)* | NAS address |
| `share` | SMB share | *(empty)* | share name |
| `sub` | Path in share | `/` | folder inside the share |
| `user` | SMB user | *(empty)* | account |
| `pass` | SMB password | *(empty)* | password |
| `filt` | Filter | `*.mp3` | filename filter for the walk |
| `ldir` | Folder on card | `/MUSIC` | destination directory on the card |
| `dialect` | SMB dialect | `auto` | `auto` negotiates; see below |
| `recurs` | *(not prompted)* | `1` | recurse into subfolders; the walk depth limit is a hardcoded 8 |
| `seal` | *(not prompted)* | `0` | force SMB3 sealing (untested against a server that requires it) |
| `maxmb` | *(not prompted)* | `0` → 14000 | **not enforced anywhere** — see the caveat below |
| `del` | *(not prompted)* | `1` | delete card files the share no longer has — see *Deleting what left the share* |

Provisioning prompts for the ten strings and writes `recurs=1`, `seal=0` as fixed values, so the
four integers have to be changed by an NVS write, not by answering prompts.

> **`maxmb` does nothing.** It is read from NVS, stored, and printed in the boot log
> (`cap=%d MB`), but no code path consults it. The only capacity protection that exists is the
> per-file free-space check in `walk()`: a file is fetched only if the card has
> `size + 64 MB` free, and if not, the pass stops with `ERR_FAT`. Do not rely on `maxmb` to keep a
> big library off a small card — cap it with `sub`/`filt` instead.

**Dialect:** leave it `auto`. Synology DSM refuses a *pinned* SMB 2.0.2 negotiate
(`0xC00000BB`), while accepting an offer range; the ladder is `{ANY2, ANY3, ANY}`. DSM's floor is
SMB 2.1.

### Tunables in `main/bridge_main.c`

| define | value | note |
|---|---|---|
| `READ_CHUNK_MAX` | 32768 | SMB read size, capped by the server's `max_read` (8 MiB here) |
| `READ_STALL_RETRIES` | 5 | a failed read means a new connection, not a new file |
| `QUIET_AFTER_HOST_IO_US` | 3 s | idle time required after the last host SCSI read |
| `MSC_PATH` | `/msc` | must match `CONFIG_TINYUSB_MSC_MOUNT_PATH` |
| `BRIDGE_DRY_RUN` | 0 | **1** = same reads, zero writes: measures the network stage alone |
| `BRIDGE_WRITE_ONLY` | 0 | **1** = boot-time 8 MiB card throughput probe, self-deleting |

Both switches are compile-time diagnostics with a measured purpose (they produced the stage
numbers above). `BRIDGE_WRITE_ONLY` must be run with the USB host unbound, or the writes race
the ownership handover and fail partway.

---

## How the sync works

### When a pass runs — and when it does not

A pass runs **only** when the host releases the volume: a physical `TINYUSB_EVENT_DETACHED`
plus the 3 s quiet window. There is no timer, and there is **no pass at boot** — the firmware
mounts the card, hands it straight to the host, and waits. Consequences worth knowing:

- Tracks you add to the share appear on the card the **next time the car lets go of the disk**
  (ignition off, or unplug/replug) — not while you are driving, and not the moment you copy them.
- While the host owns the volume the card is untouched by design, so the bridge never has to
  revoke a disk from a live reader.
- If the host returns mid-pass, the pass aborts and the half-written file stays as a `.part`.
  The log then ends **without** a `sync done:` / `sync FAILED:` line — a missing summary is an
  interruption, not a hang. The next pass resumes that file from its last fsync watermark.

Measured on the bench: the share was swapped from a 4-file test set to a 10-track / 411 MB album
while the board was idle and host-owned. The next detach cycle picked the new files up with no
help, and one pass did this (`tools/growth_sync_test.sh`, whole run):

```
sync start: //10.119.0.4/CAR-MEDIA/ -> / (filter '*.mp3')
  continuing /09.mp3 at 25165824/43216524 B (prefix verified)
  + /09.mp3 ... + /01.mp3            9 files, 0 reconnects, 0 warnings
sync done: 9 copied (388023987 bytes), 1 already present, 0 deferred
```

977 s, **all ten files md5-identical to the NAS** (411 MB verified byte-for-byte), no
reconnects, no warnings. The resume is the interesting part: a previous pass had been
interrupted inside `09.mp3` at exactly its last 4 MiB fsync watermark (25,165,824 B), and the
next pass re-verified that prefix and continued from it instead of restarting the file.

Running it again with nothing new gave `sync done: 0 copied (0 bytes), 10 already present` in
**234 ms** — a detach that finds nothing costs the listener nothing and wears the card not at all.

### Per file

Per file: `smb2_pread` in 32 KiB chunks into an **internal, DMA-capable** buffer → `fwrite` into
`<target>.part` → `fflush`+`fsync` every 4 MiB → `rename` into place.

### What counts as "already copied"

`walk()` compares **size only**: `card_has(rel)` and if the card's size equals the share's size,
the file is skipped. So

- a brand-new file, or a file missing/renamed on the card, is fetched;
- a track you *replaced* on the share is re-fetched only if its size changed — same-size
  different-content is silently kept from the card. Delete it from the card to force a refresh;
- **files that leave the share leave the card** — with `del=1` (the default) and only under the
  guards in *Deleting what left the share* below. With `del=0` the sync is additive and orphans
  stay forever; before that feature existed, the card kept three orphan tracks after the library
  swap above.

The bridge's own SMB account is read-only on the share, which is how it should be provisioned:
the device can never modify or delete what is on the NAS. Pruning only ever removes files from the
card, never from the share.

### Deleting what left the share

`walk()` only adds. After the write phase, `prune_missing()` walks the card and removes media the
share no longer has, so the card tracks the library instead of growing without bound. The pass
summary reports it as its own column (`N pruned`), and the files are printed as ` - /path`.

The order is not incidental: the write phase runs **first**, so a pass that reconnected and ended
up with an empty seen-list can never be mistaken for an empty library.

A card file is deleted only when every one of these holds:

| guard | why it exists |
|---|---|
| `del=1` (default) | deletion is irreversible, so an opt-out has to exist |
| the pass saw ≥ 1 share file | an unreachable or misconfigured share must delete nothing at all |
| the pass was not interrupted by the host | an interrupted pass has an incomplete seen-list |
| `smb2_stat()` answers **ENOENT** for that exact path | any other answer (a network hiccup) leaves the file and counts it as *kept-uncertain* |
| the name matches the filter, is not `*.part`, and is not under `BRIDGE/` | the pruner touches only media it manages, never its own status file |
| the deletion count is below half the managed files, and below a hard ceiling of 50 per pass | a mount pointed at the wrong folder looks exactly like "everything was deleted"; the cap stops one bad pass from wiping a card |

When the cap stops it, the log says so and the rest wait for the next pass — the pruner
self-converges over several passes rather than doing damage in one.

Deleting in DSM normally moves a file to the share's `#recycle`. That is still the *original path*
disappearing, so the pruner fires; `#recycle` is filtered from the walk, so a trashed track is
never re-fetched out of the recycle bin either.

This ran on the host before it ever saw hardware: `tools/prune_host_test.py` extracts
`prune_missing()` and its helpers **from `main/bridge_main.c`** (no copy, so it cannot drift) and
executes them against a real directory tree with a stubbed share — the file I/O is the real
thing. 21 checks cover: stale files deleted, live files kept, `.part`/non-matching/`BRIDGE/`
untouched, empty-share and I/O-error guards deleting nothing, the cap firing at exactly half,
subfolders removed once empty, case differences not treated as duplicates, and an abort stopping
the pass mid-way.

### Rules that are load-bearing

- **Internal DMA RAM is not optional.** With `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, a
  32 KiB `malloc()` silently returns PSRAM — and ESP32-S3 has no `SOC_SDMMC_PSRAM_DMA_CAPABLE`,
  so SDMMC cannot DMA from it and every write gets bounced. The buffer is `heap_caps_malloc(...,
  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`.
- **A `.part` is only as good as the last `fsync`.** Without periodic flushing, 150 s of transfer
  left a **0-byte** `.part` when the host reclaimed the disk.
- **Resume is verified, not assumed.** On a `.part`, the first 512 bytes are re-read from the
  share and compared before continuing; a mismatch restarts the file. (Note: after
  `fseek(SEEK_END)` a `fread` of the head returns 0 bytes — the code seeks back to `SEEK_SET`
  for the comparison, then to the end to write.)
- **Interruption is not corruption.** A host taking the disk back mid-file keeps the `.part`;
  only a card write error discards it. A name clash is neither — the file is skipped and counted
  (`ERR_CONFLICT`), leaving the user's bytes alone.
- **Paths are share-relative with no leading slash** (`smb_path()` is the single seam). `smb2_open`
  with a leading `/` fails.
- **Junk is filtered**: `@eaDir`, `#recycle`, `#snapshot`, `@SynoResource`, `$Recycle.Bin`,
  `Thumbs.db`, `.DS_Store`, and friends, so Synology metadata never reaches the car.
- **One unreadable file does not end the pass.** It is skipped and counted; the walk stops only
  for a card-level fault or the host reclaiming the disk — and the log says which.

---

## Performance: what moved the needle, and what did not

| lever | result |
|---|---|
| blocking transport + send-before-read PDU ordering in libsmb2 | **8 KB/s → ~260 KB/s.** The library was waiting a full socket timeout per request |
| `TCP_WND_DEFAULT=65535` + `TCP_SACK_OUT=y` + `PBUF_POOL_SIZE=48` | **+10 %**, and it cured a 32 KiB-read pathology (23.0 s → 3.66 s per MiB) |
| read buffer in internal DMA RAM | correct, and cheap (~2 %) — *not* the 30 % it looked like |
| 8 KiB vs 32 KiB vs larger reads | no material difference; time scales with total bytes, not per-request overhead |
| Wi-Fi modem sleep OFF (`WIFI_PS_NONE`) | **slower** — reverted |
| `STATIC_RX_BUFFER_NUM` 10→25 | +2 % for 24 KiB of internal RAM — reverted |
| writer task + two DMA slots (overlap read and write) | **tied within noise** (serial 369.5/396.4 vs overlap 414.1/400.4 KB/s on the same 61 MB library), so it was reverted. Implementation preserved in `tools/apply_overlap.py` |
| `CONFIG_ESP_WIFI_RX_BA_WIN` 6 → 20 / 25 | **cannot be measured the naive way** — see below |

> ### Why the RX BA window is not a one-knob experiment
>
> `esp_wifi/src/wifi_init.c` fails the **build** on an illegal pairing:
>
> ```c
> #if (CONFIG_ESP_WIFI_RX_BA_WIN > CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM)
> #error "... should not be larger than WIFI_DYNAMIC_RX_BUFFER_NUM!"
> #if (CONFIG_ESP_WIFI_RX_BA_WIN > (CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM << 1))
> #error "... should not be larger than double of the WIFI_STATIC_RX_BUFFER_NUM!"
> ```
>
> With this project's `STATIC_RX_BUFFER_NUM=10` the largest legal window is **20**, so `=25` is not
> testable on its own at all: it drags the buffer count with it (25/25 is the balanced maximum on a
> non-HE chip), which costs ~24 KiB of the scarcest resource there is — internal DRAM — and that
> buffer bump alone already measured **+2 %** and was reverted. So the arms are `20/10` (the knob
> alone, free) and `25/25` (IDF's recommended pairing, +24 KiB), never `25/10`.
>
> And the trap that ate the first attempt twice: **`sdkconfig.defaults` only sets keys that are
> absent from `sdkconfig`** — so an overlay can raise `RX_BA_WIN` but silently cannot raise
> `STATIC_RX_BUFFER_NUM`, and the build dies on the `#error` above while looking like a config bug.
> Each arm therefore gets its own generated config:
>
> ```sh
> SDKCONFIG=$PWD/build-bawin20/sdkconfig.bawin20 \
>   idf.py -B build-bawin20 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bawin20" build
> ```
>
> and `tools/ba_win_matrix.sh` refuses to report a number unless the pass completed. Assert the
> resolved value in `build-*/sdkconfig.*` before believing any throughput figure.


`PBUF_POOL_SIZE` is not exposed in Kconfig (it lives in lwIP's `opt.h`, guarded), so it is
overridden in the root `CMakeLists.txt`: 16 buffers × ~1480 B ≈ 23.7 KiB cannot back a 64 KiB
window, and a segment with nowhere to land is dropped.

> **`copied ... bytes` overstates a pass that resumed.** `s_copied_bytes` adds the file's full
> size, including bytes a previous pass had already written into the `.part`. In the run above it
> reported 388,023,987 B while only 362,858,163 B crossed the wire — 7 % high, which is exactly
> the margin an A/B test would be decided on. Divide *fetched* bytes, not reported bytes, by the
> pass duration when computing throughput; the per-file `... N/41 MB` progress lines count the
> whole file as well.

**The wall is hardware.** ~400 KB/s with 1-bit SDMMC (20 MHz) sharing one chip with a 2.4 GHz
radio. Big read buffers are impossible regardless of what the server allows: internal DMA heap
is ~168 KiB free with a 90 KiB largest block, and PSRAM is not SDMMC-DMA-capable. Going beyond
this needs 4-bit wiring, i.e. a different carrier.

---

## Troubleshooting

Every row below is a defect that actually happened on this hardware.

| symptom | cause and fix |
|---|---|
| first `smb2_pread` never returns | the classic. libsmb2's non-blocking/poll model loses the edge on IDF lwIP. Fixed in the vendored lib by a blocking transport with `SO_RCVTIMEO`; if you revert it, expect an infinite hang, not an error |
| `0xC00000BB NOT_SUPPORTED` on negotiate | SMB dialect pinned to 2.0.2. Use `dialect = auto` (DSM floor is SMB 2.1) |
| `smb2_open` always fails | leading `/` in the share path. Paths are share-relative |
| transfer completes, no file appears | a **directory** is sitting on the target name — a "mkdir parents" helper that also `mkdir`s the file itself makes `rename(.part → X.mp3)` fail `EEXIST` forever. `stat` the destination before blaming the network |
| `0-byte .part` after minutes of transfer | missing periodic `fsync`; bytes were still in the stdio buffer |
| `.part` deleted every run, transfer restarts at 0 | reading the prefix from `SEEK_END` returns 0 bytes → false mismatch. Seek to `SEEK_SET` first |
| mount fails, **card is reformatted** | see [Hazard](#known-hazard-mount-failure-reformats-the-card) |
| `statvfs()` always fails | IDF's FAT VFS has no statvfs hook; the code uses `esp_vfs_fat_info()` |
| sync silently never starts | your console capture reset the board. Opening the port with pyserial pulses DTR/RTS and the CH34x turns that into an EN pulse — capture with `stty raw -echo` + `cat` instead |
| every file stamped `Jan 1 1980` | no RTC and no SNTP yet (see Limitations). Head units sort by date |
| a knob change does nothing | `build.sh` runs `idf.py set-target`, which **deletes `sdkconfig`** and regenerates it from `sdkconfig.defaults`. A knob that is not in `sdkconfig.defaults` does not exist |

### Known hazard: mount failure reformats the card

`main/bridge_main.c` currently sets `.config.format_if_mount_failed = true` with
`.do_not_format = false`. Consequence: **if the FAT mount fails — for instance a card formatted
with a partition table by a PC — the firmware wipes and rebuilds the volume.** Media and
evidence both go. A freshly-formatted card is the intended state, so this has never fired in
testing, but the safe default for a device holding someone's music library is *never* format
without an explicit instruction. Until that is changed: only insert cards formatted as described
above, and back up the card before inserting it.

---

## Bench tooling

`tools/` contains the harness that produced every number in this file. They assume the bench:
board on a serial console + OTG into a Linux host (`/dev/sdb`, vendor `TinyUSB`, label
`CAR-BRIDGE`), NAS reachable, credentials in `/root/.smbcreds` (mode 600, passed by environment
variable — never in argv, never printed).

| script | what it proves |
|---|---|
| `detach_test.sh [quiet] [capture]` | the real loop: boot the app, capture the console **passively**, de-authorize the USB device (the "ignition off" equivalent), report what the bridge did |
| `cold_sync_proof.sh` | wipe the synced files → cold sync → checksum every file against the NAS. The acceptance test |
| `growth_sync_test.sh` | the everyday case: the share gained tracks. One detach cycle must fetch exactly what is new, resume any `.part`, skip the rest, then size-check the whole library and md5 the cheapest files. Long: background it |
| `new_file_sync_test.sh` | single-file incremental proof. Drops a probe into a new folder on the share if the account can write; otherwise renames one synced file on the card (keeping the bytes as a safety net) to hit the same `walk()` branch |
| `verify_card_vs_nas.sh` | md5 the card against the share, `ok`/`MISMATCH` per file |
| `card_write_probe.sh` | card throughput in isolation (`BRIDGE_WRITE_ONLY`) |
| `split_stages_test.sh` | network stage vs network+write stage (`BRIDGE_DRY_RUN`) |
| `negmatrix.sh` / `negprobe.c` | SMB dialect matrix, run with the *actual* library |
| `readstall_probe.c`, `interleave_probe.c` | reproduce firmware SMB behaviour on a host, where it is cheaper to debug |
| `apply_overlap.py` / `revert_overlap.sh` | the writer-task experiment, in both directions (idempotent patcher + verified restore) |
| `apply_prune.py` | the deletion-propagation patcher, applied to `git show HEAD:main/bridge_main.c` — reproduces the working tree byte-identically, so the feature is reviewable as a script |
| `prune_host_test.py` | runs the **shipping** pruning code (extracted from `main/bridge_main.c`, not copied) against a real temp directory and a stubbed share: 26 checks, no hardware needed. Gate a flash on it |
| `prune_proof.sh [n]` | end-to-end deletion proof on the real card: plants `n` orphan `.mp3` the share never saw plus 3 decoys that must survive, runs one pass, then asserts the orphans are gone, the decoys are kept, and every real track is unchanged. The last assertion is the point |
| `ba_win_build.sh` | builds every `RX_BA_WIN` arm through `idf.py set-target`, asserting each arm's **resolved** symbol (baseline included) before keeping its image |
| `ba_win_matrix.sh ARM image [reps] [files]` | flash one prepared arm image, boot it, wait for enumeration, run `reps` timed passes; fails rather than reporting a missing pass |

Testing rules that saved this project, encoded in those scripts:

- **A test result belongs to whatever is in flash**, not to the source you just built. Copying a
  `.bin` to the target is not flashing it. Every script here counts `Hash of data verified`.
- **A throughput test must delete something first.** A run reporting `0 copied, 4 already
  present` measures nothing.
- Never `pyserial` for capture; `stty raw -echo` + `cat`.
- Never conclude from one sample: run-to-run spread on *identical* firmware here is ~7 %.
- **Before a null result, prove the knob moved.** Assert the resolved symbol in each arm's
  `sdkconfig` — the baseline included, or two "different" firmwares may be one configuration. Three
  of this project's measurement attempts were voided by a value that never reached the binary.

---

## Repository layout

```
main/bridge_main.c        firmware: ownership state machine, SMB walk+sync, provisioning, LED
main/smb_path.h           share-relative path conversion (single seam, unit-tested)
components/sahlberg__libsmb2   vendored libsmb2 3.0.1, patched for IDF lwIP — see below
components/espressif__*        vendored esp_tinyusb 3.0.0 + led_strip (nothing fetches these)
tools/                      bench harness, host-side probes, A/B scripts
partitions.csv              factory app @0x20000, 4 MiB (this firmware outgrows the 1 MiB default)
sdkconfig.defaults          the ONLY config input — build.sh regenerates sdkconfig from it
RESUME-NOTE.md              working notes: measured numbers, dead ends, bench quirks
```

**Vendored libsmb2 changes** (all of them are why the library works on ESP-IDF at all):
blocking transport with `SO_RCVTIMEO` replacing non-blocking+poll; send-before-read PDU ordering
when a PDU is queued (this alone took the first read from 1033 ms to 24 ms); a per-PDU deadline
that fires even when the socket stays open; `esp_random.h` guarded for non-ESP builds; and
`smb2_get_credits()` / `smb2_get_pending_pdus()` diagnostics. The root `CMakeLists.txt` relaxes
IDF's strict warnings for that one component with `-Wno-error` (visible, not silenced).

`build/` and the generated `sdkconfig`/`sdkconfig.old` are ignored on purpose.

---

## Limitations and roadmap

- **No SNTP.** There is no RTC, so files land stamped `Jan 1 1980` and "recently added" sorting
  in a head unit is meaningless. One `esp_sntp` call fixes it.
- **1-bit SD bus.** ~400 KB/s ceiling; 4-bit needs a carrier with D1–D3 routed.
- **Mount failure reformats** (see hazard above) — should become opt-in.
- **Untested**: library larger than one card; real head unit playback; SMB signing/sealing against
  a server that *requires* them.
- **`CONFIG_ESP_WIFI_RX_BA_WIN` is still untested.** `tools/wifi_ba2_test.sh` is ready and asserts
  the resolved value before running, because the first attempt measured nothing.
- **No version control of the card contents** — files deleted from the share are not removed from
  the card; it is an additive sync.

## Security notes

- Credentials live only in NVS on internal flash; **no secret is committed** (verified before the
  first push — the only `password` matches in the tree are libsmb2 API parameter names).
- This repository does contain **bench topology**: the share name in a source comment
  (`main/bridge_main.c:674`), and the NAS address plus share/user as *overridable* env defaults in
  `tools/verify_card_vs_nas.sh`, `tools/negmatrix.sh` and the host probes; `RESUME-NOTE.md` has the
  same in its tables. Nothing there is a credential, and the scripts already read `SRV=`/`SHARE=`
  from the environment, so scrubbing for publication is a few line edits. Acceptable in a private
  repo; scrub before making it public.

## Acknowledgements

- [libsmb2](https://github.com/sahlberg/libsmb2) by Ronnie Sahlberg — userspace SMB2/3 client.
- [esp_tinyusb](https://github.com/espressif/esp-tinyusb) — ESP-IDF TinyUSB component, whose MSC
  blockdev + mount-ownership API makes the one-owner-at-a-time design possible.
