#ifndef NUGGET_INSTALLER_H
#define NUGGET_INSTALLER_H
#include "types.h"

/* Returns true if NuggetFS isn't set up yet (fresh/unformatted disk, or an
   existing disk that's missing our "already installed" marker file). */
bool installer_needed(void);

/* Writes any built-in .socks programs that are missing under software/ -
   safe and cheap to call on every boot (checks first, does nothing if
   already present). Covers both a fresh install and an older install
   that predates .socks support. */
void software_ensure_builtin_programs(void);

/* Runs the interactive Welcome/disk-select/format wizard. Blocks until the
   user completes it; formats NuggetFS and writes the marker file itself. */
void installer_run(void);

#endif
