/* can: CubeMX-style CAN2 node at 500 kbit/s (PCLK1 = 80 MHz from the PLL, blinky's scaffolding).
 *
 * Bit timing (RM0351 55.7.7): tq = (BRP + 1) x tPCLK, bit = tq x (1 + (TS1 + 1) + (TS2 + 1)).
 * Prescaler 10 (BRP 9), BS1 13 tq (TS1 12), BS2 2 tq (TS2 1):
 *   80 MHz / (10 x (1 + 13 + 2)) = 500 000 bit/s.
 * Variant CAN_PRESCALER=20 (exe can_badrate) gives 250 000 bit/s (bit-rate mismatch test).
 *
 * CAN1's clock is enabled too: CAN2's filter banks live in CAN1's register block. Filters like a
 * typical project, CAN2SB = 14 (banks 14..27 for CAN2), both to FIFO0:
 *   bank 14: 16-bit identifier list {0x101, 0x102, 0x123, 0x1F0}
 *   bank 15: 32-bit identifier/mask, standard data frames 0x300..0x30F
 * FIFO0 message-pending interrupt (CAN2_RX0). Every 10 ms: id 0x100 + NODE_ID, DLC 1, an
 * incrementing counter byte (HAL_CAN_AddTxMessage when a mailbox is free). A received 0x1F0
 * (request) is answered at once with 0x180 + NODE_ID echoing its first byte.
 *
 * RX callback: rx_count, last frame (rx_last_*), and per id the last frame and a count in
 * rx_table[] (first-come slots).
 *
 * Python controls (set_var): can_run = 0 stops the controller (HAL_CAN_Stop: init mode), 1
 * starts it again; rx_hold = 1 stops draining FIFO0 (the callback disables the FIFO0
 * interrupt and leaves the message), 0 drains again. fifo0_ovr samples RF0R.FOVR0 every 1 ms.
 * The main loop does its work only when the SysTick count changed, so spurious WFI wake-ups
 * (one per quantum end) make no register accesses and timing is quantum-independent.
 *
 * Error variants (fault confinement tests): CAN_ERR_NOTIFY=1 (exes can_err, can_abom) enables
 * the error notifications (EWG, EPV, BOF, LEC, ERR: CAN2_SCE interrupt) and records them:
 * sce_count (CAN2_SCE_IRQHandler entries), err_cb_count / err_code_or (HAL_CAN_ErrorCallback
 * calls / OR of hcan->ErrorCode, reset after each callback), err_last_esr (CAN2->ESR in the
 * last callback), boff_cb_count (callbacks reporting bus-off). CAN_ABOM=1 (can_abom) sets
 * AutoBusOff (MCR.ABOM): automatic bus-off recovery. */
#include "main.h"

#ifndef NODE_ID
#define NODE_ID 1
#endif
#ifndef CAN_PRESCALER
#define CAN_PRESCALER 10
#endif

#ifndef CAN_ERR_NOTIFY
#define CAN_ERR_NOTIFY 0
#endif
#ifndef CAN_ABOM
#define CAN_ABOM 0
#endif

#define RX_SLOTS 8

typedef struct
{
  uint32_t id;
  uint32_t count;
  uint8_t dlc;
  uint8_t data[8];
} rx_slot_t;

CAN_HandleTypeDef hcan2;

volatile uint32_t node_id = NODE_ID;
volatile uint32_t tx_count = 0;   /* frames handed to HAL_CAN_AddTxMessage */
volatile uint32_t tx_skipped = 0; /* no free mailbox / controller not started */
volatile uint8_t tx_counter = 0;  /* counter byte of the next periodic frame */
volatile uint32_t rx_count = 0;
volatile uint32_t rx_last_id = 0;
volatile uint8_t rx_last_dlc = 0;
volatile uint8_t rx_last_data[8];
volatile uint32_t rx_last_fmi = 0;
volatile rx_slot_t rx_table[RX_SLOTS];
volatile uint32_t reply_count = 0;
volatile uint32_t error_count = 0;
volatile uint8_t can_run = 1;
volatile uint8_t rx_hold = 0;
volatile uint8_t fifo0_ovr = 0;
volatile uint32_t sce_count = 0;
volatile uint32_t err_cb_count = 0;
volatile uint32_t err_code_or = 0;
volatile uint32_t err_last_esr = 0;
volatile uint32_t boff_cb_count = 0;

void SystemClock_Config(void);
static void MX_CAN2_Init(void);
static void CAN2_Filters(void);
static void can_send(uint32_t id, const uint8_t *data, uint32_t dlc);

int main(void)
{
  uint32_t last, now, next = 10;
  uint8_t running = 1, holding = 0;

  HAL_Init();
  SystemClock_Config();
  MX_CAN2_Init();
  CAN2_Filters();
  if (HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
  {
    Error_Handler();
  }
#if CAN_ERR_NOTIFY
  if (HAL_CAN_ActivateNotification(&hcan2, CAN_IT_ERROR_WARNING | CAN_IT_ERROR_PASSIVE | CAN_IT_BUSOFF |
                                               CAN_IT_LAST_ERROR_CODE | CAN_IT_ERROR) != HAL_OK)
  {
    Error_Handler();
  }
#endif
  if (HAL_CAN_Start(&hcan2) != HAL_OK)
  {
    Error_Handler();
  }

  last = HAL_GetTick();
  while (1)
  {
    __WFI();
    now = HAL_GetTick();
    if (now == last)
    {
      continue; /* not a SysTick wake-up: nothing to do (no register access) */
    }
    last = now;

    if (can_run != running)
    {
      running = can_run;
      if ((running ? HAL_CAN_Start(&hcan2) : HAL_CAN_Stop(&hcan2)) != HAL_OK)
      {
        error_count++;
      }
    }
    if (rx_hold != holding)
    {
      holding = rx_hold;
      if (!holding && HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
      {
        error_count++;
      }
    }
    fifo0_ovr = __HAL_CAN_GET_FLAG(&hcan2, CAN_FLAG_FOV0) ? 1 : 0;

    if (now >= next)
    {
      uint8_t b = tx_counter;
      next += 10;
      can_send(0x100 + NODE_ID, &b, 1);
      tx_counter = (uint8_t)(b + 1);
    }
  }
}

static void can_send(uint32_t id, const uint8_t *data, uint32_t dlc)
{
  CAN_TxHeaderTypeDef h = {0};
  uint32_t mailbox;
  uint8_t buf[8] = {0};

  for (uint32_t i = 0; i < dlc; i++)
  {
    buf[i] = data[i];
  }
  h.StdId = id;
  h.IDE = CAN_ID_STD;
  h.RTR = CAN_RTR_DATA;
  h.DLC = dlc;
  h.TransmitGlobalTime = DISABLE;
  if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan2) == 0 || HAL_CAN_AddTxMessage(&hcan2, &h, buf, &mailbox) != HAL_OK)
  {
    tx_skipped++;
    return;
  }
  tx_count++;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef h;
  uint8_t data[8] = {0};
  uint32_t id;
  int slot = -1;

  if (rx_hold)
  {
    /* stop draining: leave the message in FIFO0 and stop the interrupt (it is a level) */
    HAL_CAN_DeactivateNotification(hcan, CAN_IT_RX_FIFO0_MSG_PENDING);
    return;
  }
  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &h, data) != HAL_OK)
  {
    error_count++;
    return;
  }
  id = h.IDE == CAN_ID_STD ? h.StdId : h.ExtId;
  rx_count++;
  rx_last_id = id;
  rx_last_dlc = (uint8_t)h.DLC;
  rx_last_fmi = h.FilterMatchIndex;
  for (int i = 0; i < 8; i++)
  {
    rx_last_data[i] = data[i];
  }
  for (int i = 0; i < RX_SLOTS; i++)
  {
    if (rx_table[i].count != 0 && rx_table[i].id == id)
    {
      slot = i;
      break;
    }
    if (rx_table[i].count == 0 && slot < 0)
    {
      slot = i;
      break;
    }
  }
  if (slot >= 0)
  {
    rx_table[slot].id = id;
    rx_table[slot].count++;
    rx_table[slot].dlc = (uint8_t)h.DLC;
    for (int i = 0; i < 8; i++)
    {
      rx_table[slot].data[i] = data[i];
    }
  }
  if (id == 0x1F0)
  {
    reply_count++;
    can_send(0x180 + NODE_ID, data, 1);
  }
}

#if CAN_ERR_NOTIFY
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  err_cb_count++;
  err_code_or |= hcan->ErrorCode;
  err_last_esr = hcan->Instance->ESR;
  if ((hcan->ErrorCode & HAL_CAN_ERROR_BOF) != 0U)
  {
    boff_cb_count++;
  }
  hcan->ErrorCode = HAL_CAN_ERROR_NONE;
}
#endif

static void MX_CAN2_Init(void)
{
  hcan2.Instance = CAN2;
  hcan2.Init.Prescaler = CAN_PRESCALER;
  hcan2.Init.Mode = CAN_MODE_NORMAL;
  hcan2.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan2.Init.TimeSeg1 = CAN_BS1_13TQ;
  hcan2.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan2.Init.TimeTriggeredMode = DISABLE;
  hcan2.Init.AutoBusOff = CAN_ABOM ? ENABLE : DISABLE;
  hcan2.Init.AutoWakeUp = DISABLE;
  hcan2.Init.AutoRetransmission = ENABLE;
  hcan2.Init.ReceiveFifoLocked = DISABLE;
  hcan2.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void CAN2_Filters(void)
{
  CAN_FilterTypeDef f = {0};

  /* bank 14: four standard identifiers (16-bit list: STID << 5, RTR = IDE = 0) */
  f.FilterBank = 14;
  f.FilterMode = CAN_FILTERMODE_IDLIST;
  f.FilterScale = CAN_FILTERSCALE_16BIT;
  f.FilterIdLow = 0x101 << 5;
  f.FilterMaskIdLow = 0x102 << 5;
  f.FilterIdHigh = 0x123 << 5;
  f.FilterMaskIdHigh = 0x1F0 << 5;
  f.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  f.FilterActivation = CAN_FILTER_ENABLE;
  f.SlaveStartFilterBank = 14;
  if (HAL_CAN_ConfigFilter(&hcan2, &f) != HAL_OK)
  {
    Error_Handler();
  }

  /* bank 15: 32-bit mask, standard data frames 0x300..0x30F (STID[10:4], IDE and RTR compared) */
  f.FilterBank = 15;
  f.FilterMode = CAN_FILTERMODE_IDMASK;
  f.FilterScale = CAN_FILTERSCALE_32BIT;
  f.FilterIdHigh = 0x300 << 5;
  f.FilterIdLow = 0x0000;
  f.FilterMaskIdHigh = 0x7F0 << 5;
  f.FilterMaskIdLow = 0x0006; /* IDE, RTR */
  f.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  f.FilterActivation = CAN_FILTER_ENABLE;
  f.SlaveStartFilterBank = 14;
  if (HAL_CAN_ConfigFilter(&hcan2, &f) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (hcan->Instance == CAN2)
  {
    /* CubeMX enables CAN1's clock for CAN2 (shared filter banks) */
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_CAN2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* PB12 -> CAN2_RX, PB13 -> CAN2_TX */
    GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF10_CAN2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(CAN2_RX0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(CAN2_RX0_IRQn);
#if CAN_ERR_NOTIFY
    HAL_NVIC_SetPriority(CAN2_SCE_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(CAN2_SCE_IRQn);
#endif
  }
}

void CAN2_RX0_IRQHandler(void)
{
  HAL_CAN_IRQHandler(&hcan2);
}

#if CAN_ERR_NOTIFY
void CAN2_SCE_IRQHandler(void)
{
  sce_count++;
  HAL_CAN_IRQHandler(&hcan2);
}
#endif

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 40;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /* SYSCLK = HCLK = PCLK1 = PCLK2 = 80 MHz */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  error_count++;
  while (1)
  {
    __WFI();
  }
}
