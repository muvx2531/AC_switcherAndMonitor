#include "web_auth.h"
#include "main.h"
#include "debug_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#if __has_include("web_secrets.h")
#include "web_secrets.h"  /* untracked: see web_secrets.example.h */
#endif

extern RNG_HandleTypeDef hrng;

#define MAX_SESSIONS 4U
#define SESSION_IDLE_MS (30U * 60U * 1000U)
#define MAX_FAILURES 5U
#define LOCKOUT_MS 30000U

typedef struct { char token[WEB_AUTH_TOKEN_LEN + 1]; TickType_t lastUse; bool used; } Session;

static char password[16];
static Session sessions[MAX_SESSIONS];
static unsigned failures;
static TickType_t lockedUntil;
static bool locked;

void WebAuth_Init(void)
{
#ifdef WEB_SHARED_PASSWORD
  /* Shared password for every board, from the untracked web_secrets.h. Never logged. */
  _Static_assert(sizeof(WEB_SHARED_PASSWORD) >= 9 && sizeof(WEB_SHARED_PASSWORD) <= sizeof(password),
      "WEB_SHARED_PASSWORD must be 8-15 characters");
  (void)snprintf(password, sizeof(password), "%s", WEB_SHARED_PASSWORD);
  if (strcmp(password, "change-me") == 0)
    (void)DebugLog_Printf("WEB: WARNING shared password is still the template value\r\n");
  else
    (void)DebugLog_Printf("WEB: shared password from web_secrets.h\r\n");
#else
  /* No web_secrets.h: per-board fallback. Different salt from the MAC derivation
   * so the MAC does not reveal it. */
  uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  const uint8_t *p = (const uint8_t *)uid;
  uint64_t h = 0xcbf29ce484222325ULL ^ 0x5353574542504157ULL;  /* "SSWEBPAW" */
  for (unsigned int i = 0; i < sizeof(uid); i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
  (void)snprintf(password, sizeof(password), "ss-%08lx", (unsigned long)(h >> 24));
  (void)DebugLog_Printf("WEB: no web_secrets.h, per-board password %s\r\n", password);
#endif
}

static bool equalConstTime(const char *a, const char *b)
{
  size_t la = strlen(a), lb = strlen(b);
  unsigned diff = (unsigned)(la ^ lb);
  for (size_t i = 0; i < lb; i++) diff |= (unsigned)(a[i < la ? i : 0] ^ b[i]);
  return diff == 0U && la > 0U;
}

static WebAuthResult checkLocked(void)
{
  if (locked && (int32_t)(xTaskGetTickCount() - lockedUntil) < 0) return AUTH_LOCKED;
  locked = false;
  return AUTH_OK;
}

WebAuthResult WebAuth_CheckPassword(const char *pw)
{
  WebAuthResult r;
  taskENTER_CRITICAL();
  r = checkLocked();
  taskEXIT_CRITICAL();
  if (r != AUTH_OK) return r;
  bool ok = pw != NULL && equalConstTime(pw, password);
  taskENTER_CRITICAL();
  if (ok) failures = 0U;
  else if (++failures >= MAX_FAILURES) {
    failures = 0U; locked = true;
    lockedUntil = xTaskGetTickCount() + pdMS_TO_TICKS(LOCKOUT_MS);
  }
  taskEXIT_CRITICAL();
  return ok ? AUTH_OK : AUTH_BAD_PASSWORD;
}

WebAuthResult WebAuth_Login(const char *pw, char token[WEB_AUTH_TOKEN_LEN + 1])
{
  WebAuthResult r = WebAuth_CheckPassword(pw);
  if (r != AUTH_OK) return r;
  uint32_t rnd[4];
  for (unsigned int i = 0; i < 4U; i++)
    if (HAL_RNG_GenerateRandomNumber(&hrng, &rnd[i]) != HAL_OK) return AUTH_NO_SLOT;
  (void)snprintf(token, WEB_AUTH_TOKEN_LEN + 1, "%08lx%08lx%08lx%08lx",
      (unsigned long)rnd[0], (unsigned long)rnd[1], (unsigned long)rnd[2], (unsigned long)rnd[3]);
  TickType_t now = xTaskGetTickCount();
  taskENTER_CRITICAL();
  /* Reuse a free or expired slot, else the least recently used one. */
  unsigned pick = 0;
  for (unsigned int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].used || now - sessions[i].lastUse > pdMS_TO_TICKS(SESSION_IDLE_MS)) { pick = i; break; }
    if (sessions[i].lastUse < sessions[pick].lastUse) pick = i;
  }
  memcpy(sessions[pick].token, token, WEB_AUTH_TOKEN_LEN + 1);
  sessions[pick].lastUse = now;
  sessions[pick].used = true;
  taskEXIT_CRITICAL();
  return AUTH_OK;
}

bool WebAuth_CheckToken(const char *token)
{
  if (token == NULL || strlen(token) != WEB_AUTH_TOKEN_LEN) return false;
  bool ok = false;
  TickType_t now = xTaskGetTickCount();
  taskENTER_CRITICAL();
  for (unsigned int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].used) continue;
    if (now - sessions[i].lastUse > pdMS_TO_TICKS(SESSION_IDLE_MS)) { sessions[i].used = false; continue; }
    if (memcmp(sessions[i].token, token, WEB_AUTH_TOKEN_LEN) == 0) { sessions[i].lastUse = now; ok = true; }
  }
  taskEXIT_CRITICAL();
  return ok;
}

void WebAuth_Logout(const char *token)
{
  if (token == NULL) return;
  taskENTER_CRITICAL();
  for (unsigned int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].used && strncmp(sessions[i].token, token, WEB_AUTH_TOKEN_LEN) == 0) sessions[i].used = false;
  taskEXIT_CRITICAL();
}
