# Bridge — resume note (2026-11-05, end of session)

## Status: the sync works end to end. Proven on silicon, byte-checked.

Cold sync of the whole library to a wiped card, then card vs NAS by md5:
**4 identical, 0 mismatched** — 2222.mp3 (1 MiB), panni-kelj-fel.mp3 (1 MiB),
34. Faust.mp3 (17 MiB), 23. Clear Sky.mp3 (39 MiB). Throughput 254–289 KB/s steady,
**0 stalls and 0 reconnects** across 61 MB. Interrupting a 40 MiB file at 12 MiB (host takes the
disk back) and re-running **resumes at exactly 12,582,912 B** and completes.

Run the proof with `sh /root/bridge/cold_sync_proof.sh 330` on HermesBoard (~9 min) and the
comparison with `sh /root/bridge/verify_card_vs_nas.sh`.

## The five defects that had to die, in the order they were found

1. **Dialect**: DSM's floor is SMB 2.1; a pinned single-dialect 2.0.2 offer is refused
   (`0xc00000bb`). Now offers a range (`SMB2_VERSION_ANY2` first). This note's old "one
   remaining bug" — solved, and it was never about the PDU shape.
2. **Path spelling**: libsmb2 takes share-relative paths with **no leading slash**. `smb_path()`
   is the single seam; `smbclient` normalises the slash and hid this for days.
3. **Transport model** (`components/sahlberg__libsmb2/lib/{socket,sync}.c`): libsmb2's non-blocking
   `poll()` loop loses receive edges on IDF lwIP → infinite hang; and once blocking, its service
   function *reads before it writes*, so each request waited a full `SO_RCVTIMEO` before being
   sent — measured 1033 ms/read = 8 KB/s. Now: blocking socket + `SO_RCVTIMEO`, send-when-queued.
   First read **24 ms**, ~263 KB/s. Plus an unconditional per-PDU deadline (the library's own
   check was gated on `fd == -1` and could never fire), and the connect phase keeps its own poll —
   `fd == -1` is legitimate while the non-blocking connect is in flight, so treat the connection as
   closed only when `fd == -1` **and** `connecting_fds_count == 0`.
4. **`mk_parents()` created a DIRECTORY named after the file** (unconditional `mkdir(path)` at the
   end), so `rename(X.part → X)` failed `EEXIST` forever — silently. This was the actual reason
   nothing ever landed on the card, and the SMB stall had been hiding it. Ancestors-only now; the
   one call site that wants the last component created does its own `mkdir`.
5. **Silent error exits**: `rename`/`fclose`/walk-ending returned `ERR_FAT` with no log. Every one
   now prints the operation, both paths and `strerror(errno)`, and a name clash is its own class
   (`ERR_CONFLICT`) that keeps the `.part` instead of deleting good bytes.

Also: `fsync` every 4 MB so resume progress is real (a 0-byte `.part` after 150 s meant the data
was in the stdio buffer); resume compares the head from `SEEK_SET` (it used to `fread` after a
`SEEK_END`, get 0 bytes, and restart from zero every time); one unreadable file no longer ends the
walk; `card_path()` no longer emits `/msc///name`.

## Measured, do not re-litigate without new numbers

- **Read chunk: 8 KiB vs 32 KiB is NOT decided by "response must fit the window".** That was my
  read of a contaminated test. Two confounders, both real:
  * with the old stack (16 KiB window, SACK off, 16-buffer pbuf pool) a 32 KiB response = 23
    segments vs an 11-segment window → window trip mid-response, drops, go-back-N. Measured
    23.0 s/MiB. Raising `TCP_WND_DEFAULT` to 65535 + `TCP_SACK_OUT=y` + `PBUF_POOL_SIZE=48`
    (not in Kconfig; overridden in `CMakeLists.txt`) **cured it: 23.0 s → 3.66 s/MiB read phase**.
  * `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, so `malloc(32768)` returns **PSRAM** — proven by
    the heap log (238 KiB internal free at a 32 KiB chunk vs 230 KiB at 8 KiB). ESP32-S3 has no
    `SOC_SDMMC_PSRAM_DMA_CAPABLE`, so SDMMC cannot DMA from it and every card write bounces
    through internal RAM. The read buffer must be `heap_caps_malloc(..., MALLOC_CAP_INTERNAL |
    MALLOC_CAP_DMA)`.
- **Stage split, measured (2 x 1 MiB file, 32 KiB chunk, same link every time):**
  network-only (dry run, no writes) **482 / 554 KB/s**; read+write **376 / 371 KB/s**.
  Per-KiB: network ~2.0 ms, card ~0.70 ms, whole sync ~2.7 ms — a sum, so *something* between them
  was the target, and it was attacked three ways. All three are documented because the arithmetic
  looked airtight and the answer was still no:
  * **PSRAM bounce copy** (buffer moved to internal DMA RAM): ~2%, i.e. noise. A 32 KiB memcpy is
    not 34% of anything; the mechanism was real, the magnitude invented.
  * **Writer task + 2 DMA slots** (read N+1 while N is on the card, drains before every
    fclose/fsync/rename): **378 / 355 KB/s**. Nothing.
  * **Same writer pinned to core 1**, keeping it off the core `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`
    gives the radio: **367 / 364 KB/s**. Nothing, slightly worse.
  **Conclusion, after four full-library runs: the two builds are TIED within noise, so the serial
  loop stays.** Same 61,234,228 B library, same bench, same link, whole-sync rate:

  | build | sample A | sample B |
  |---|---|---|
  | serial loop (shipping) | 165.7 s = 369.5 KB/s | 154.5 s = 396.4 KB/s |
  | writer task, pinned | 147.9 s = 414.1 KB/s | — |
  | writer task, unpinned | — | 152.9 s = 400.4 KB/s |

  The serial build's own spread (7.3%) is as big as the gap between the arms, and the best-matched
  pair (serial 396.4 vs overlap 400.4) is 1% apart. **The 2 x 1 MiB A/B rig could never have seen an
  8% effect** — per-file open cost dominates a 2.8 s transfer, and the noise floor is ~7%. Settling
  this properly means alternating runs, N>=5 per arm, ~2 h of bench: not worth spending to resolve a
  difference this small. Writer task reverted (`tools/revert_overlap.sh` — it flashes, and counts
  esptool's verify lines); implementation kept in `tools/apply_overlap.py` + the exact pre-revert
  file in `tools/bridge_main.pre-overlap.c` if the bus question is ever reopened.
  **Write the decision rule down before the measurement** — this session flipped its mind twice on
  this question, and only the pre-committed rule stopped it flipping a third time.
- **Card write rate measured on THIS card at last: 1430 KiB/s** (boot-time probe, unbuffered
  `fwrite`, 32 KiB writes; `tools/card_write_probe.sh`, self-cleaning file, `BRIDGE_WRITE_ONLY`
  switch, inert at 0). It errored after exactly 512 KiB because the app hands the volume to the USB
  host ~40 ms later and the handover raced it — so for a full multi-megabyte figure the probe must
  run with the host unbound (detach harness), not at boot. The 0.70 ms/KiB it did yield matches the
  stage-split derivation exactly. `BOARD.md`'s **790 KiB/s** was a different card (APPSD 120 MiB).
- **Ceilings to plan against:** the card bus is the hard one — **1-bit SDMMC, D1/D2/D3 not routed
  on this carrier**, 20 MHz. Internal DMA-capable heap is
  ~238 KiB (largest block ~160 KiB), so multi-megabyte read buffers are impossible regardless of
  what the server allows (`max read 8388608 B`).
- **`CONFIG_ESP_WIFI_RX_BA_WIN` is still UNTESTED, and the first attempt produced a fake null
  result.** `build.sh` runs `idf.py set-target`, which **deletes `sdkconfig` and regenerates it from
  `sdkconfig.defaults`** — a knob that is not in `sdkconfig.defaults` does not exist. The first test
  sed'd `sdkconfig` only, so every run in this project has used BA window 6, and the "+2%" I credited
  to the window was really `STATIC_RX_BUFFER_NUM` 10→25 (reverted: not worth 24 KiB of internal RAM).
  `tools/wifi_ba2_test.sh` does it correctly and **asserts the resolved value before running**, so it
  can fail instead of reporting a fake negative. `STATIC_RX_BUFFER_NUM` caps at 25 on this chip.
- **Wi-Fi modem sleep stays on.** `WIFI_PS_NONE` (added as a guess during the stall hunt) measured
  *slower*: 254–262 KB/s vs 274–289 KB/s with `WIFI_PS_MIN_MODEM`.
- lwIP `TCP_OOSEQ_MAX_PBUFS` / RX buffer bumps: worse. Reverted, not left in the tree.
- Host-side deletions on the card ARE seen by the device (re-downloaded exactly the removed files)
  → no remount or FatFS cache purge needed on an ownership flip.
- HTTP pivot: no. The NAS has no HTTP face (80/5000/7070 closed) and the defect was never SMB.

## Bench facts that cost time (this session's own tooling bugs, all real)

- Scripts copied to HermesBoard by `scp` arrive **without the exec bit**; `nohup ./x.sh` then fails
  `Permission denied` and the test silently measures nothing. Invoke with `sh ./x.sh` (the three in
  `/root/bridge` are chmod +x now, but the next scp undoes that).
- `pkill -f detach_test` also matches the *remote shell running that pkill* (its own argv holds the
  literal), so it kills itself, ssh exits non-zero and `set -e` aborts the run. Bracket it:
  `pkill -f "detach_[t]est"`.
- A `cmd | tail -25` pipeline reports **tail's** exit status, so every failure above looked like
  success while the run produced nothing. Never hide a test's output behind a pipe.
- **A throughput test must delete something first.** A run reported `0 copied (0 bytes), 4 already
  present` and I nearly read that as "the change made it instant": the card held the whole library,
  so there was no transfer. Wipe the two small test files from the host side, *then* trigger the
  sync (`overlap_test.sh` does exactly this and prints `card before: N entries`).
- **One apostrophe inside a single-quoted ssh block ends the quote.** A comment containing
  `harness's` handed the rest of the script to the *local* shell, which failed on `$PORT`. Keep
  remote blocks free of apostrophes and check with `sh -n` plus a scan of the block.

- `/dev/sdb` is the bridge volume (VENDOR `TinyUSB`, label `CAR-BRIDGE`). **`sda1` on the same Pi
  is an unrelated 1.9 GB stick** — pick a device by vendor+label, never "first vfat".
- The card is only mountable from Linux while the **host owns** the disk, i.e. after a detach cycle
  hands it back, and it takes a few seconds to enumerate after a flash — poll for `/dev/sdb`.
- `detach_test.sh [quiet-seconds] [capture-seconds]`; it boots the app itself via
  `flash-id --after hard-reset`. Never pyserial for capture (DTR/RTS pulse resets the board);
  capture with `stty raw -echo` + `cat`.
- Do not rebuild while a flash/scp is in flight: `build/bridge.bin` is momentarily absent and the
  old binary silently ships. That cost one full test result.
- Credentials live in `/root/.smbcreds` (600) on HermesBoard, passed by env var, never in argv/logs.

## Config / knobs

- `bridge.local.cfg` → `SMB_DIALECT=auto`; the offer ladder is `{ANY2, ANY3, ANY}` (old note's
  "default SMB 3.00" is gone — a pin is what DSM refused).
- Sync triggers ONLY on TINYUSB_EVENT_DETACHED + quiet after the last host SCSI read
  (`QUIET_AFTER_HOST_IO_US`) — never from a timer.
- Share CAR-MEDIA is non-browseable: direct connect only, and `-L`/browse is not an existence
  check. Skip-list: `@eaDir` etc. `.part` + rename pattern throughout.
- Build: `cd /home/hermes/board-schematics/bridge && bash build.sh`. Flash: `deploy_bridge.sh`
  (Pi) or app-only esptool @0x20000 as in the harness note. Provisioning re-run:
  `provision_serial.py` + `/root/prov.txt` (on Pi, 600); reprovisioning needs
  `erase-region 0x9000 0x6000` first.
- Card: superfloppy FAT32 on /dev/sdb (no MBR), 14.8 GiB, label CAR-BRIDGE.

## Open / not done

- **Throughput: closed as far as software goes.** Plan on **370-400 KB/s**, i.e. the 61 MB library
  in **2.6-2.8 min** (was 8 KB/s / 2.1 h-equivalent pain at the start of the arc, and 273 KB/s before
  the window fix). Levers measured: window+SACK+`PBUF_POOL_SIZE=48` is the only live one (+10%, and
  it cured the 32 KiB pathology); chunk size, internal-DMA buffer, and the writer task all land
  inside run-to-run variance. **45 MB chunks are impossible, not just useless**: the server caps one
  read at 8 MiB (`max_read` logged) and internal DMA heap is ~168 KiB free / 90 KiB largest block,
  while PSRAM is not SDMMC-DMA-able. The wall beyond this is hardware: 1-bit SD (D1/D2/D3 unrouted)
  sharing one chip with the radio — needs a different carrier, not more firmware.
- **Repo: `flashdrive-emulator`** (private, `github.com/dmagyar/flashdrive-emulator`). This directory
  is the repo root; the local directory stays `bridge/` because every script in `tools/` hardcodes
  that path. Baseline `ea38f24`. Push through the per-repo deploy-key alias —
  `origin = git@github.com-flashdrive-emulator:dmagyar/flashdrive-emulator.git` — because the literal
  `git@github.com:` URL is refused: it never offers the deploy key (verified both ways).
  Ignored: `build/` (175 MB) and the generated `sdkconfig`/`sdkconfig.old`. Tracked on purpose:
  `components/espressif__*` — there is no `idf_component.yml`, so the component manager will not
  fetch them and a clone without them does not build.
- No RTC: files land stamped `Jan 1 1980`. SNTP before caring about "recently added" sorting.
- `BRIDGE_DRY_RUN` (compile-time, 0) is still in the tree as a diagnostic switch.
- Not yet tested: a library larger than one card, and playback on a real head unit.
