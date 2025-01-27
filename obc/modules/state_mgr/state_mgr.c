#include "state_mgr.h"
#include "timekeeper.h"
#include "telemetry_manager.h"
#include "command_manager.h"
#include "comms_manager.h"
#include "uplink_decoder.h"
#include "downlink_encoder.h"
#include "eps_manager.h"
#include "payload_manager.h"
#include "alarm_handler.h"
#include "health_collector.h"
#include "task_stats_collector.h"
#include "obc_sw_watchdog.h"
#include "logger.h"
#include "obc_errors.h"
#include "obc_logging.h"
#include "obc_state_handle.h"
#include "obc_state_defs.h"
#include "obc_scheduler_config.h"
#include "obc_reset.h"
#include "obc_reliance_fs.h"
#include "lm75bd.h"
#include "obc_board_config.h"
#include "fm25v20a.h"

#include <FreeRTOS.h>
#include <os_portmacro.h>
#include <os_queue.h>
#include <os_task.h>

#include <sys_common.h>
#include <gio.h>
#include <redposix.h>

/* Supervisor queue config */
#define STATE_MGR_QUEUE_LENGTH 10U
#define STATE_MGR_QUEUE_ITEM_SIZE sizeof(state_mgr_event_t)
#define STATE_MGR_QUEUE_RX_WAIT_PERIOD pdMS_TO_TICKS(10)
#define STATE_MGR_QUEUE_TX_WAIT_PERIOD pdMS_TO_TICKS(10)

static QueueHandle_t stateMgrQueueHandle = NULL;
static StaticQueue_t stateMgrQueue;
static uint8_t stateMgrQueueStack[STATE_MGR_QUEUE_LENGTH * STATE_MGR_QUEUE_ITEM_SIZE];

static comms_state_t commsManagerState = COMMS_STATE_DISCONNECTED;

/**
 * @brief Send all startup messages from the stateMgr task to other tasks.
 */
static void sendStartupMessages(void);

void initStateMgr(void) {
  ASSERT((stateMgrQueueStack != NULL) && (&stateMgrQueue != NULL));
  if (stateMgrQueueHandle == NULL) {
    stateMgrQueueHandle =
        xQueueCreateStatic(STATE_MGR_QUEUE_LENGTH, STATE_MGR_QUEUE_ITEM_SIZE, stateMgrQueueStack, &stateMgrQueue);
  }
}

obc_error_code_t sendToStateMgrEventQueue(state_mgr_event_t *event) {
  ASSERT(stateMgrQueueHandle != NULL);

  if (event == NULL) return OBC_ERR_CODE_INVALID_ARG;

  if (xQueueSend(stateMgrQueueHandle, (void *)event, STATE_MGR_QUEUE_TX_WAIT_PERIOD) == pdPASS)
    return OBC_ERR_CODE_SUCCESS;

  return OBC_ERR_CODE_QUEUE_FULL;
}

static void sendStartupMessages(void) {}

void obcTaskFunctionStateMgr(void *pvParameters) {
  obc_error_code_t errCode;

  ASSERT(stateMgrQueueHandle != NULL);

  initLoggerTask();
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_LOGGER);

  /* Initialize critical peripherals */
  LOG_IF_ERROR_CODE(setupFileSystem());  // microSD card
  LOG_IF_ERROR_CODE(initTime());         // RTC

  lm75bd_config_t config = {
      .devAddr = LM75BD_OBC_I2C_ADDR,
      .devOperationMode = LM75BD_DEV_OP_MODE_NORMAL,
      .osFaultQueueSize = 2,
      .osPolarity = LM75BD_OS_POL_ACTIVE_LOW,
      .osOperationMode = LM75BD_OS_OP_MODE_COMP,
      .overTempThresholdCelsius = 125.0f,
      .hysteresisThresholdCelsius = 75.0f,
  };

  LOG_IF_ERROR_CODE(lm75bdInit(&config));  // LM75BD temperature sensor (OBC)

  initFRAM();  // FRAM storage (OBC)

  // Call init functions for all tasks. TODO: Combine into obc_scheduler
  initTimekeeper();
  initAlarmHandler();
  initTelemetry();
  initCommandManager();
  initCommsManager();
  initDecodeTask();
  initTelemEncodeTask();
  initEPSManager();
  initPayloadManager();
  initHealthCollector();
#if ENABLE_TASK_STATS_COLLECTOR == 1
  initTaskStatsCollector();
#endif
  initSwWatchdog();

  /* Create all tasks*/
  taskENTER_CRITICAL();
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_TIMEKEEPER);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_ALARM_MGR);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_TELEMETRY_MGR);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_COMMAND_MGR);
  obcSchedulerCreateTaskWithArgs(OBC_SCHEDULER_CONFIG_ID_COMMS_MGR, &commsManagerState);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_COMMS_UPLINK_DECODER);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_COMMS_DOWNLINK_ENCODER);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_EPS_MGR);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_PAYLOAD_MGR);
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_HEALTH_COLLECTOR);
#if ENABLE_TASK_STATS_COLLECTOR == 1
  obcSchedulerCreateTask(OBC_SCHEDULER_CONFIG_ID_STATS_COLLECTOR);
#endif
  taskEXIT_CRITICAL();

  // TODO: Deal with errors
  LOG_IF_ERROR_CODE(changeStateOBC(OBC_STATE_INITIALIZING));

  /* Send initial messages to system queues */
  sendStartupMessages();

  // TODO: Deal with errors
  LOG_IF_ERROR_CODE(changeStateOBC(OBC_STATE_NORMAL));

  while (1) {
    state_mgr_event_t inMsg;

    if (xQueueReceive(stateMgrQueueHandle, &inMsg, STATE_MGR_QUEUE_RX_WAIT_PERIOD) != pdPASS) {
#if defined(DEBUG) && !defined(OBC_REVISION_2)
      vTaskDelay(pdMS_TO_TICKS(1000));
      gioToggleBit(STATE_MGR_DEBUG_LED_GIO_PORT, STATE_MGR_DEBUG_LED_GIO_BIT);
#endif
      continue;
    }

    switch (inMsg.eventID) {
      default:
        LOG_ERROR_CODE(OBC_ERR_CODE_UNSUPPORTED_EVENT);
    }
  }
}
