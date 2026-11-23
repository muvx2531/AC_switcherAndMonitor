#include "tcp_echo_test.h"
#include "debug_log.h"
#include "core/net.h"
#include "core/socket.h"
#include "FreeRTOS.h"
#include "task.h"

#define TEST_TCP_PORT 5000U

static void tcpEchoTask(void *arg)
{
  (void)arg;
  for (;;) {
    Socket *listener = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
    if (listener == NULL) {
      (void)DebugLog_Printf("TCP test: socket allocation failed\r\n");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    error_t error = socketBind(listener, &IP_ADDR_ANY, TEST_TCP_PORT);
    if (!error) error = socketListen(listener, 1);
    if (error) {
      (void)DebugLog_Printf("TCP test: listen failed %ld\r\n", (long)error);
      socketClose(listener);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    (void)DebugLog_Printf("TCP test: listening on port %u\r\n", TEST_TCP_PORT);
    for (;;) {
      Socket *client = socketAccept(listener, NULL, NULL);
      if (client == NULL) break;
      (void)socketSetTimeout(client, 5000);
      uint8_t data[256];
      size_t received;
      (void)DebugLog_Printf("TCP test: client connected\r\n");
      while (socketReceive(client, data, sizeof(data), &received, 0) == NO_ERROR && received > 0) {
        size_t offset = 0;
        while (offset < received) {
          size_t sent = 0;
          error = socketSend(client, data + offset, received - offset, &sent, 0);
          if (error || sent == 0) break;
          offset += sent;
        }
        if (offset != received) break;
      }
      socketClose(client);
      (void)DebugLog_Printf("TCP test: client closed\r\n");
      (void)DebugLog_Printf("TCP test: heap=%lu min=%lu TCP stack free=%lu words net stack free=%lu words\r\n",
          (unsigned long)xPortGetFreeHeapSize(),
          (unsigned long)xPortGetMinimumEverFreeHeapSize(),
          (unsigned long)uxTaskGetStackHighWaterMark(NULL),
          (unsigned long)uxTaskGetStackHighWaterMark(netContext.taskId));
    }
    socketClose(listener);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

bool TcpEchoTest_Start(void)
{
  return xTaskCreate(tcpEchoTask, "tcpEcho", 512, NULL,
      tskIDLE_PRIORITY + 2, NULL) == pdPASS;
}
