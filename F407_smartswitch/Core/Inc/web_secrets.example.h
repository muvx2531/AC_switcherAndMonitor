/* Template for the shared web password (all boards built from this tree).
 *
 * 1. Copy this file to web_secrets.h in the same folder.
 * 2. Replace the value below with your password (8-15 printable characters,
 *    no quotes or backslashes).
 * 3. Rebuild and flash.
 *
 * web_secrets.h is listed in .gitignore: never commit it. The source is public
 * (GPL), so a password committed to git is a password for every board.
 * Without web_secrets.h the firmware falls back to a per-board password derived
 * from the STM32 UID and prints it on the serial console at boot. */
#ifndef WEB_SECRETS_H
#define WEB_SECRETS_H

#define WEB_SHARED_PASSWORD "change-me"

#endif
