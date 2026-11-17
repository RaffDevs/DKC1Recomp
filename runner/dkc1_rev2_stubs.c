/* Rev 2 bring-up stubs.
 *
 * The stock widescreen override pass rewrites generated C so that the
 * margin-proxy feature has an AOT definition of CODE_BF8087_M0X0. Rev 2
 * cannot use that pass yet (see docs/ROM_REVISIONS.md), and the analyzer
 * keeps BF:8087 in the interpreter tier, so the symbol is otherwise absent.
 *
 * This stub forwards to the exact interpreter dispatch -- the same path the
 * generated void alias uses. Widescreen is disabled in this configuration,
 * so the proxy is inert; the stub only keeps the link honest. Remove it when
 * the overrides are re-derived from the v1.2 bytes.
 */
#include "cpu_state.h"

RecompReturn CODE_BF8087_M0X0(CpuState *cpu) {
  return interp_tier_dispatch(cpu, 0xbf8087u);
}
