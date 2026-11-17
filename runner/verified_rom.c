#include "verified_rom.h"

#include "sha256.h"

static uint16_t s_native_nmi_pc = 0xA968u;

uint16_t Dkc1NativeNmiPc(void) { return s_native_nmi_pc; }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supported stock DKC1 USA revisions, headerless:
 * v1.0    fa8cacf5bbfc39ee6bbaa557adf89133d60d42f6cf9e1db30d5a36a469f74d15
 * v1.2/Rev 2
 *         628147468c3539283197f58f03b94df49758a332831857481ea9cc31645f0527
 * The recomp/*..cfg facts come from the v1.0 disassembly; Rev 2's bugfixes
 * are in-place patches, so both revisions share the same code layout. */
static const uint8_t kSupportedSha256s[][32] = {
  { /* DKC1 USA v1.0 */
    0xfa, 0x8c, 0xac, 0xf5, 0xbb, 0xfc, 0x39, 0xee,
    0x6b, 0xba, 0xa5, 0x57, 0xad, 0xf8, 0x91, 0x33,
    0xd6, 0x0d, 0x42, 0xf6, 0xcf, 0x9e, 0x1d, 0xb3,
    0x0d, 0x5a, 0x36, 0xa4, 0x69, 0xf7, 0x4d, 0x15,
  },
  { /* DKC1 USA v1.2 / Rev 2 */
    0x62, 0x81, 0x47, 0x46, 0x8c, 0x35, 0x39, 0x28,
    0x31, 0x97, 0xf5, 0x8f, 0x03, 0xb9, 0x4d, 0xf4,
    0x97, 0x58, 0xa3, 0x32, 0x83, 0x18, 0x57, 0x48,
    0x1e, 0xa9, 0xcc, 0x31, 0x64, 0x5f, 0x05, 0x27,
  },
};

static int IsSupportedRom(const uint8_t hash[32]) {
  for (size_t index = 0;
       index < sizeof(kSupportedSha256s) / sizeof(kSupportedSha256s[0]);
       index++) {
    if (memcmp(hash, kSupportedSha256s[index], 32) == 0) return 1;
  }
  return 0;
}

static int HexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static int ParseSha256(const char *text, uint8_t output[32]) {
  if (!text || strlen(text) != 64u) return 0;
  for (size_t index = 0; index < 32u; index++) {
    const int high = HexNibble(text[index * 2u]);
    const int low = HexNibble(text[index * 2u + 1u]);
    if (high < 0 || low < 0) return 0;
    output[index] = (uint8_t)((high << 4) | low);
  }
  return 1;
}

static int MatchesExplicitDevelopmentHash(const uint8_t hash[32]) {
  const char *text = getenv("DKC1_ALLOW_ROM_SHA256");
  uint8_t allowed[32];
  return ParseSha256(text, allowed) && memcmp(hash, allowed, sizeof allowed) == 0;
}

static void SetError(char *error, size_t error_size, const char *message) {
  if (!error || error_size == 0) return;
  (void)snprintf(error, error_size, "%s", message);
}

static void SetUnsupportedError(char *error, size_t error_size, size_t size,
                                const uint8_t hash[32]) {
  if (!error || error_size == 0) return;
  int written = snprintf(error, error_size,
                         "unsupported ROM (size=%zu sha256=", size);
  if (written < 0 || (size_t)written >= error_size) return;
  size_t used = (size_t)written;
  for (size_t i = 0; i < 32 && used + 2 < error_size; i++) {
    written = snprintf(error + used, error_size - used, "%02x", hash[i]);
    if (written != 2) return;
    used += 2;
  }
  if (used + 2 <= error_size) {
    error[used++] = ')';
    error[used] = '\0';
  }
}

uint8_t *Dkc1ReadVerifiedRom(const char *path, size_t *size_out,
                             char *error, size_t error_size) {
  if (size_out) *size_out = 0;
  if (!path || !*path || !size_out) {
    SetError(error, error_size, "invalid ROM path or output pointer");
    return NULL;
  }

  FILE *stream = fopen(path, "rb");
  if (!stream) {
    SetError(error, error_size, "unable to open ROM");
    return NULL;
  }
  if (fseek(stream, 0, SEEK_END) != 0) {
    fclose(stream);
    SetError(error, error_size, "unable to seek ROM");
    return NULL;
  }
  long length = ftell(stream);
  if (length <= 0 || fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    SetError(error, error_size, "ROM is empty or unreadable");
    return NULL;
  }

  uint8_t *file = (uint8_t *)malloc((size_t)length);
  if (!file) {
    fclose(stream);
    SetError(error, error_size, "not enough memory to load ROM");
    return NULL;
  }
  if (fread(file, 1, (size_t)length, stream) != (size_t)length) {
    free(file);
    fclose(stream);
    SetError(error, error_size, "unable to read complete ROM");
    return NULL;
  }
  if (fclose(stream) != 0) {
    free(file);
    SetError(error, error_size, "unable to close ROM after reading");
    return NULL;
  }

  size_t skip = ((size_t)length % 1024u == 512u) ? 512u : 0u;
  size_t payload_size = (size_t)length - skip;
  if (skip) memmove(file, file + skip, payload_size);

  uint8_t hash[32];
  sha256_compute(file, payload_size, hash);
  const int is_supported = IsSupportedRom(hash);
  const int is_explicit_development_rom =
      payload_size == 0x400000u && MatchesExplicitDevelopmentHash(hash);
  /* Patched cartridges belong to the separate variant loader. Accepting one
   * here would run it against stock generated code and dispatch contracts. */
  const int rom_accepted =
      payload_size == 0x400000u &&
      (is_supported || is_explicit_development_rom);
  if (!rom_accepted) {
    SetUnsupportedError(error, error_size, payload_size, hash);
    free(file);
    return NULL;
  }

  if (is_explicit_development_rom && !is_supported) {
    fprintf(stderr,
            "warning: loading an explicitly hash-pinned modified DKC1 ROM; "
            "generated code compatibility is the caller's responsibility\n");
  }

  *size_out = payload_size;
  if (payload_size > 0xFFEBu) {
    s_native_nmi_pc = (uint16_t)(file[0xFFEA] | (file[0xFFEB] << 8));
  }
  if (error && error_size) error[0] = '\0';
  return file;
}
