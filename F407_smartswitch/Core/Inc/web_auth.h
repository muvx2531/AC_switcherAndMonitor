#ifndef WEB_AUTH_H
#define WEB_AUTH_H
#include <stdbool.h>

/* Web sign-in for relay commands (Phase 2, bench version).
 * Password: derived from the STM32 UID, unique per board, printed on the serial
 * console at boot ("WEB: bench password ..."). Replaced by a stored, changeable
 * password when the Storage service exists (Phase 5/6). Plain HTTP: use only on
 * the cabinet LAN or through the router VPN (decision S7-4). */

#define WEB_AUTH_TOKEN_LEN 32  /* hex characters */

typedef enum { AUTH_OK, AUTH_BAD_PASSWORD, AUTH_LOCKED, AUTH_NO_SLOT } WebAuthResult;

void WebAuth_Init(void);
/* On AUTH_OK writes a NUL-terminated session token (WEB_AUTH_TOKEN_LEN hex). */
WebAuthResult WebAuth_Login(const char *password, char token[WEB_AUTH_TOKEN_LEN + 1]);
/* Valid, unexpired session (30 min idle); refreshes the idle timer. */
bool WebAuth_CheckToken(const char *token);
/* Re-authentication for commands that remove power from equipment. Counts toward lockout. */
WebAuthResult WebAuth_CheckPassword(const char *password);
void WebAuth_Logout(const char *token);

#endif
