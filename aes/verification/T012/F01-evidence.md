# T012 / F01 — container 3DSX: evidence of closure

Date: 2026-10-11
Executed by: VERIFIER (moderator) — NOT the independent human executor required by
PEER_REVIEW.md §7. This record does NOT by itself lift the round verdict from REJECT.

## Device
Nintendo 3DS, Luma3DS. FTP endpoint moved 10.0.0.129 -> 10.0.0.115 (old host ARP FAILED).

## Closure condition (rubric C-01)
> `od -An -tx1 -N4 build/simcity.3dsx | tr -d ' \n'` equals 33445358
> AND the C-01 loop prints no FAIL for any *.3dsx outside devkitpro/.

## Evidence

Local build artefact:
```
$ od -An -tx1 -N4 build/simcity.3dsx | tr -d ' \n'
33445358
$ stat -c%s build/simcity.3dsx
13133828
$ sha256sum build/simcity.3dsx
b28af9bd9feab0028b2ddd14d3ef1286663cefbfd569032bd8bf737125867c1f
```

Round-trip from the device (downloaded back from the 3DS and hashed):
```
$ curl -o verify_3dsx --user anonymous:. ftp://10.0.0.115:5000/3ds/simcity.3dsx
$ stat -c%s verify_3dsx
13133828
$ sha256sum verify_3dsx
b28af9bd9feab0028b2ddd14d3ef1286663cefbfd569032bd8bf737125867c1f
$ od -An -tx1 -N4 verify_3dsx | tr -d ' \n'
33445358
```

Hashes match byte-for-byte. The artefact on the SD card is the container, not an ELF.

## Change that closed it
`CMakeLists.txt`: target now links to `simcity.elf`; a POST_BUILD step runs
`3dsxtool $<TARGET_FILE> build/simcity.3dsx --smdh=simcity.smdh`.
The build now hard-fails (`message(FATAL_ERROR)`) rather than emitting a raw ELF
renamed `.3dsx`, so the failure mode cannot silently recur.

## Cleanup performed
- `simcity.cia` removed from `/3ds/` (invalid container from an earlier experiment).
- Stale ELF `simcity.3dsx` in SD root removed; it was 37243924 bytes (the old raw ELF).
- Repo root: 4 raw-ELF `test_*.3dsx`, 3 linker scripts, 2 invalid CIAs moved to
  `investigation/quarantine-armv7/` (nothing deleted).

## NOT YET EVIDENCED
Device execution. Nothing here shows the game boots, renders, or survives 60s.
That is F03/F08 territory and requires a human observer on the console.
