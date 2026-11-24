#pragma once

#include <esp_ota_ops.h>

// When this firmware was built, e.g. "Nov 20 2026 11:40:18". Set by
// scripts/build_stamp.py on every build, unlike __DATE__/__TIME__, which only
// change when the file using them happens to be recompiled.
extern const char *const BUILD_STAMP;

// The git commit it was built from, e.g. "2ef1519a3c", with "+dirty" when
// there were uncommitted changes. Its ELF is kept in builds/ (see the script).
extern const char *const BUILD_COMMIT;

// The first 16 hex digits of the SHA-256 of the ELF this firmware came from,
// as a crash report's "ELF file SHA256" line shows them. builds/*.zip records
// each build's in build.json, so a crash can be matched to its exact ELF.
inline const char *elfSha() {
    static char sha[17];
    if (!sha[0]) esp_ota_get_app_elf_sha256(sha, sizeof(sha));
    return sha;
}
