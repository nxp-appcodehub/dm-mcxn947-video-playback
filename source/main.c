/*
* Copyright 2023 NXP
* All rights reserved.
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/* Board config */
/* .clang-format off */
#include "board.h"
#include "clock_config.h"
#include "pin_mux.h"
/* .clang-format on */

/* FatFS */
/* clang-format off */
#include "ff.h"
#include "diskio.h"
#include "fsl_sd.h"
#include "fsl_sd_disk.h"
#include "sdmmc_config.h"
/* clang-format on */

/* Debug console */
#include "fsl_debug_console.h"

/* SDK drivers */
#include "fsl_dbi_flexio_edma.h"
#include "fsl_flexio_mculcd.h"
#include "fsl_st7796s.h"

#define LCD_WIDTH       480
#define LCD_HEIGHT      320
#define LCD_BUF_HEIGHT  80
#define LCD_BUF_STRIPE  (LCD_HEIGHT / LCD_BUF_HEIGHT)
#define LCD_FLEXIO_FREQ (16 * 16 * 1000 * 1000) /* bit per second, frequency * lines */

static void app_set_cs_pin(bool set);
static void app_set_rs_pin(bool set);
static void app_lcd_done_callback(status_t status, void *userData);

static status_t sdcardWaitCardInsert(void);

static FATFS s_fileSystem;
const TCHAR  s_driverNumberBuffer[3U] = {SDDISK + '0', ':', '/'};

static uint8_t s_data_buf[2][LCD_WIDTH * LCD_BUF_HEIGHT * 2];
static volatile bool s_lcd_done = true;

static FLEXIO_MCULCD_Type s_flexio_lcd = {
    .flexioBase          = FLEXIO0,
    .busType             = kFLEXIO_MCULCD_8080,
    .dataPinStartIndex   = 16,
    .ENWRPinIndex        = 1,
    .RDPinIndex          = 0,
    .txShifterStartIndex = 0,
    .txShifterEndIndex   = 7,
    .rxShifterStartIndex = 0,
    .rxShifterEndIndex   = 7,
    .timerIndex          = 0,
    .setCSPin            = app_set_cs_pin,
    .setRSPin            = app_set_rs_pin,
    .setRDWRPin          = NULL /* Not used in 8080 mode. */
};

static dbi_flexio_edma_xfer_handle_t s_flexio_xfer_handle;

static st7796s_handle_t s_lcd_handle;

int main(void) {
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom4Clk, 1u);
    CLOCK_AttachClk(BOARD_DEBUG_UART_CLK_ATTACH);

    BOARD_InitBootPins();
    BOARD_InitBootClocks();

    CLOCK_SetClkDiv(kCLOCK_DivUSdhcClk, 3u);
    CLOCK_AttachClk(kPLL0_to_USDHC); /* 50MHz */

    CLOCK_SetClkDiv(kCLOCK_DivFlexioClk, 1U);
    CLOCK_AttachClk(kPLL0_to_FLEXIO);

    BOARD_InitDebugConsole();

    flexio_mculcd_config_t fxio_mculcd_cfg;

    FLEXIO_MCULCD_GetDefaultConfig(&fxio_mculcd_cfg);
    fxio_mculcd_cfg.baudRate_Bps = LCD_FLEXIO_FREQ;

    FLEXIO_MCULCD_Init(&s_flexio_lcd, &fxio_mculcd_cfg, CLOCK_GetFlexioClkFreq());

    DBI_FLEXIO_EDMA_CreateXferHandle(&s_flexio_xfer_handle, &s_flexio_lcd, NULL, NULL);

    const st7796s_config_t lcd_config = {
        .driverPreset    = kST7796S_DriverPresetLCDPARS035,
        .pixelFormat     = kST7796S_PixelFormatRGB565,
        .orientationMode = kST7796S_Orientation270,
        .teConfig        = kST7796S_TEDisabled,
        .invertDisplay   = true,
        .flipDisplay     = true,
        .bgrFilter       = true,
    };

    ST7796S_Init(&s_lcd_handle, &lcd_config, &g_dbiFlexioEdmaXferOps, &s_flexio_xfer_handle);
    ST7796S_EnableDisplay(&s_lcd_handle, true);
    ST7796S_SetMemoryDoneCallback(&s_lcd_handle, app_lcd_done_callback, NULL);

    if (sdcardWaitCardInsert() != kStatus_Success) {
        PRINTF("No card detected...\r\n");
        goto dead_loop;
    }

    if (f_mount(&s_fileSystem, s_driverNumberBuffer, 1U)) {
        PRINTF("Mount volume failed.\r\n");
        goto dead_loop;
    }

    PRINTF("FS mounted.\r\n");

    if (f_chdrive((char const *)&s_driverNumberBuffer[0U]) != FR_OK) {
        PRINTF("Change drive failed.\r\n");
        goto dead_loop;
    }

    FIL video_fil;

    for (;;) {
        if (f_open(&video_fil, _T("/video_480_320.bin"), FA_READ) != FR_OK) {
            PRINTF("Video file open failed.\r\n");
            goto dead_loop;
        }

        UINT     br;
        uint32_t stripe = 0;
        do {
            if (f_read(&video_fil, s_data_buf[stripe % 2], LCD_WIDTH * LCD_BUF_HEIGHT * 2, &br) != FR_OK) {
                PRINTF("Read failed.\r\n");
                goto dead_loop;
            }

            while(s_lcd_done == false) {
            }

            s_lcd_done = false;

            ST7796S_SelectArea(&s_lcd_handle, 0, (stripe % LCD_BUF_STRIPE) * LCD_BUF_HEIGHT, LCD_WIDTH - 1,
                               (stripe % LCD_BUF_STRIPE) * LCD_BUF_HEIGHT + LCD_BUF_HEIGHT - 1);

            ST7796S_WritePixels(&s_lcd_handle, (uint16_t *)s_data_buf[stripe % 2], LCD_WIDTH * LCD_BUF_HEIGHT);

            stripe++;
        } while (br > 0);
        f_close(&video_fil);
    }

dead_loop:

    for (;;) {
        /* -- */
    }

    return 0;
}

static void app_set_cs_pin(bool set) {
    GPIO_PinWrite(BOARD_INITLCDFXIOPINS_LCD_CS_GPIO, BOARD_INITLCDFXIOPINS_LCD_CS_GPIO_PIN, (uint8_t)set);
}

static void app_set_rs_pin(bool set) {
    GPIO_PinWrite(BOARD_INITLCDFXIOPINS_LCD_RS_GPIO, BOARD_INITLCDFXIOPINS_LCD_RS_GPIO_PIN, (uint8_t)set);
}

static void app_lcd_done_callback(status_t status, void *userData) {
    s_lcd_done = true;
}

static status_t sdcardWaitCardInsert(void) {
    BOARD_SD_Config(&g_sd, NULL, BOARD_SDMMC_SD_HOST_IRQ_PRIORITY, NULL);

    /* SD host init function */
    if (SD_HostInit(&g_sd) != kStatus_Success) {
        PRINTF("SD host init fail\r\n");
        return kStatus_Fail;
    }

    /* wait card insert */
    if (SD_PollingCardInsert(&g_sd, kSD_Inserted) == kStatus_Success) {
        PRINTF("Card inserted.\r\n");
        /* power off card */
        SD_SetCardPower(&g_sd, false);
        /* power on the card */
        SD_SetCardPower(&g_sd, true);
    } else {
        PRINTF("Card detect fail.\r\n");
        return kStatus_Fail;
    }

    return kStatus_Success;
}
