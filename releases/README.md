# Firmware revisions (local only)

One folder per revision, written by `python tools/make_release.py --notes "..."` after a
build. Only this README is tracked by git: **the images contain the web password in plain
text, so never commit, publish or send them outside the team.**

| Folder | Contents |
|---|---|
| `<version>/smartswitch-<version>.ssw` | web update (Firmware tab -> Upload -> Install and reboot) |
| `<version>/smartswitch-<version>-app.hex` | same image for ST-LINK (fit the DIS_WDT jumper) |
| `<version>/bootloader-<x.y.z>.hex` | bootloader, only for a new or fully erased board |
| `<version>/RELEASE.txt` | version, build time, git commit, SHA-256, notes |
| `test/` | test images (e.g. one that never confirms, to test the automatic rollback) |

## Going back to an older revision

- **One step back:** Firmware tab -> **Roll back** restores the firmware that ran before the
  last install (the backup slot on flash chip 2), about 4 s.
- **Any older revision:** upload that revision's `.ssw` and install it like a new one. The
  bootloader does not refuse older versions. The backup slot then holds the firmware you
  left, so Roll back returns to it.
- An image older than the last password change asks for the password it was built with.
- Images before 0.7.0 have no OLED/LED status; before 0.7.1 no watchdog fixes (boards run
  without the DIS_WDT jumper since 0.7.1, so do not go below 0.7.1 on such a board).
