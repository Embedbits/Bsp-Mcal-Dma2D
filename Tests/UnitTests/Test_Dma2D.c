/**
 * \author Mr.Nobody
 * \file Test_Dma2D.c
 * \ingroup Dma2D
 * \brief Unit tests of Chrom-ART accelerator (DMA2D) module (STM32L4 / STM32L4+).
 *
 * Dma2D.c is compiled unchanged with real LL drivers. DMA2D registers are
 * emulated by RegMem, RCC and NVIC modules are mocked by CMock. DMA2D ISR
 * registered in NVIC is captured by a stub and called directly.
 *
 * \note Emulated registers are plain memory - tests preset status flags (ISR)
 *       before the interrupt / task call. HW model running in background
 *       (\ref Ut_Dma2D_HwModel) finishes abort requests (START bits cleared).
 * \note Graphical buffers are placed in emulated SRAM (\ref REGMEM_SRAM_PTR) -
 *       the module writes 32-bit memory addresses to DMA2D.
 * \note Runtime context of the module is static - setUp() releases the
 *       peripheral by Dma2D_Deinit() with ignored mocks.
 */

/* ============================= INCLUDES =================================== */
#include <string.h>                         /* memset                         */
#include "unity.h"                          /* Unity testing framework        */
#include "RegMem.h"                         /* Register memory emulation      */
#include "Dma2D_Port.h"                     /* Module under test              */
#include "MockRcc_Port.h"                   /* RCC module mock                */
#include "MockNvic_Port.h"                  /* NVIC module mock               */
#include "Stm32_dma2d.h"                    /* DMA2D registers definition     */

/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

static void                 Ut_Dma2D_Reset_Mocks        ( void );
static void                 Ut_Dma2D_Ignore_PeriphMocks ( void );
static dma2d_Config_t       Ut_Dma2D_Get_Config         ( dma2d_XferMode_t xferMode );
static void                 Ut_Dma2D_Init               ( dma2d_XferMode_t xferMode );
static dma2d_OutputConfig_t Ut_Dma2D_Get_Output         ( dma2d_ColorMode_t colorMode );
static dma2d_LayerConfig_t  Ut_Dma2D_Get_Layer          ( dma2d_ColorMode_t colorMode, uint32_t sramOffset );
static void                 Ut_Dma2D_Call_Isr           ( uint32_t isrFlags );
static void                 Ut_Dma2D_Check_XferEnd      ( dma2d_XferErrorId_t expError );
static void                 Ut_Dma2D_HwModel            ( void );

static nvic_RequestState_t  Ut_Dma2D_NvicHandlerStub    ( nvic_PeriphIrqList_t irqId, const nvic_IsrCallback_t irqHandler, int callCnt );
static nvic_RequestState_t  Ut_Dma2D_NvicInactiveStub   ( nvic_PeriphIrqList_t irqId, int callCnt );

static void                 Ut_Dma2D_XferCompleteCallback ( void );
static void                 Ut_Dma2D_ClutCompleteCallback ( void );
static void                 Ut_Dma2D_WatermarkCallback    ( void );
static void                 Ut_Dma2D_ErrorCallback        ( dma2d_XferErrorId_t errorId );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** DMA2D peripheral used by tests */
#define UT_DMA2D_PERIPH                     ( DMA2D_PERIPH_1 )
#define UT_DMA2D_REG                        ( DMA2D )
#define UT_DMA2D_RCC                        ( RCC_PERIPH_DMA2D )
#define UT_DMA2D_NVIC                       ( NVIC_PERIPH_IRQ_DMA2D )

/** Interrupt priority of test configurations */
#define UT_DMA2D_PRIO                       ( 7u )

/** Dead time of test configurations */
#define UT_DMA2D_DEAD_TIME                  ( 10u )

/** Offsets of graphical buffers in emulated SRAM */
#define UT_DMA2D_FG_OFFSET                  ( 0x1000u )
#define UT_DMA2D_BG_OFFSET                  ( 0x2000u )
#define UT_DMA2D_OUT_OFFSET                 ( 0x3000u )
#define UT_DMA2D_CLUT_OFFSET                ( 0x4000u )

/** Output area of tests (pixels per line, lines, line offset) */
#define UT_DMA2D_PIXELS                     ( 40u )
#define UT_DMA2D_LINES                      ( 30u )
#define UT_DMA2D_OUT_OFFSET_PIXELS          ( 200u )

/** CR bits of transfer interrupt sources */
#define UT_DMA2D_CR_XFER_IT                 ( DMA2D_CR_TCIE | DMA2D_CR_TEIE | DMA2D_CR_CEIE )

/** CR bits of CLUT loading interrupt sources */
#define UT_DMA2D_CR_CLUT_IT                 ( DMA2D_CR_CTCIE | DMA2D_CR_CAEIE | DMA2D_CR_CEIE )

/** All CR interrupt bits */
#define UT_DMA2D_CR_ALL_IT                  ( UT_DMA2D_CR_XFER_IT | UT_DMA2D_CR_CLUT_IT | DMA2D_CR_TWIE )

/** All IFCR clear bits */
#define UT_DMA2D_IFCR_ALL                   ( DMA2D_IFCR_CTEIF  | DMA2D_IFCR_CTCIF  | DMA2D_IFCR_CTWIF | \
                                              DMA2D_IFCR_CAECIF | DMA2D_IFCR_CCTCIF | DMA2D_IFCR_CCEIF )

/* ============================== MACROS ==================================== */

/** Address of emulated SRAM at offset as 32-bit register value */
#define UT_DMA2D_SRAM_ADDR( offset )        ( REGMEM_SRAM_BASE + (uint32_t)( offset ) )

/* ========================== LOCAL VARIABLES =============================== */

/** ISR registered in NVIC */
static nvic_IsrCallback_t   utDma2D_Isr;

/** Count of Nvic_Set_PeriphIrq_Inactive() calls and its return value */
static uint32_t             utDma2D_NvicOffCnt;
static nvic_RequestState_t  utDma2D_NvicOffState;

/** Counts of callback calls */
static uint32_t             utDma2D_CompleteCnt;
static uint32_t             utDma2D_ClutCnt;
static uint32_t             utDma2D_WatermarkCnt;
static uint32_t             utDma2D_ErrorCnt;

/** Error of the last error callback */
static dma2d_XferErrorId_t  utDma2D_LastError;

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

    Ut_Dma2D_Ignore_PeriphMocks();
    (void)Dma2D_Deinit( UT_DMA2D_PERIPH );
    Ut_Dma2D_Reset_Mocks();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

    utDma2D_Isr          = NULL;
    utDma2D_NvicOffCnt   = 0u;
    utDma2D_NvicOffState = NVIC_REQUEST_OK;
    utDma2D_CompleteCnt  = 0u;
    utDma2D_ClutCnt      = 0u;
    utDma2D_WatermarkCnt = 0u;
    utDma2D_ErrorCnt     = 0u;
    utDma2D_LastError    = DMA2D_XFER_ERROR_CNT;
}


void tearDown( void )
{
    (void)RegMem_Set_ModelInactive();
}

/* ========================== MODULE VERSION ================================ */

/**
 * \brief   Dma2D_Get_ModuleVersion() returns version of the module.
 *
 * \par Expected results
 * - Version is 1.0.0.
 */
void Ut_Dma2D_Get_ModuleVersion_ReturnsVersion( void )
{
    const dma2d_ModuleVersion_t version = Dma2D_Get_ModuleVersion();

    TEST_ASSERT_EQUAL_UINT8( 1u, version.Major );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Minor );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Patch );
}

/* ========================== INITIALIZATION ================================ */

/**
 * \brief   Dma2D_Get_DefaultConfig() fills the default configuration.
 *
 * \par Expected results
 * - Peripheral 1, ISR data handling, priority 15, no dead time, no callbacks.
 * - NULL pointer: DMA2D_REQUEST_ERROR.
 */
void Ut_Dma2D_Get_DefaultConfig_FillsDefaults( void )
{
    dma2d_Config_t config;

    (void)memset( &config, 0xA5, sizeof( config ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL( (dma2d_PeriphId_t)0u, config.PeriphId );
    TEST_ASSERT_EQUAL( DMA2D_XFER_MODE_ISR, config.XferMode );
    TEST_ASSERT_EQUAL_UINT32( 15u, config.IrqPriority );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, config.DeadTimeState );
    TEST_ASSERT_EQUAL_UINT8( 0u, config.DeadTime );
    TEST_ASSERT_NULL( config.XferCompleteCallback );
    TEST_ASSERT_NULL( config.ClutCompleteCallback );
    TEST_ASSERT_NULL( config.WatermarkCallback );
    TEST_ASSERT_NULL( config.ErrorCallback );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_DefaultConfig( NULL ) );
}


/**
 * \brief   Dma2D_Init() rejects invalid configuration without any access.
 *
 * \details NULL, invalid peripheral, data handling mode and dead time state.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, no RCC / NVIC call (strict mocks), AMTCR not written.
 */
void Ut_Dma2D_Init_InvalidConfig_ReturnsErrorWithoutAccess( void )
{
    dma2d_Config_t config;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( NULL ) );

    config = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_ISR ); config.PeriphId      = DMA2D_PERIPH_CNT;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( &config ) );
    config = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_ISR ); config.XferMode      = DMA2D_XFER_MODE_CNT;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( &config ) );
    config = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_ISR ); config.DeadTimeState = (dma2d_FunctionState_t)2u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( &config ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->AMTCR );
}


/**
 * \brief   Dma2D_Init() in ISR mode enables the clock and the interrupt.
 *
 * \details Configuration with dead time 10 cycles, ISR data handling.
 *
 * \par Expected results
 * - RCC: clock enabled, reset pulse; NVIC: handler registered, priority 7, enabled.
 * - AMTCR: DT = 10, EN = 1; CR interrupt sources disabled.
 * - Transfer state IDLE, CLUT loading inactive.
 */
void Ut_Dma2D_Init_IsrMode_ClockAndInterruptEnabled( void )
{
    const dma2d_Config_t  config    = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_ISR );
    dma2d_XferState_t     xferState = DMA2D_XFER_STATE_CNT;
    dma2d_FunctionState_t clutState = DMA2D_FUNCTION_ACTIVE;

    UT_DMA2D_REG->CR = UT_DMA2D_CR_ALL_IT;

    Rcc_Set_PeriphActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Handler_ExpectAndReturn( UT_DMA2D_NVIC, NULL, NVIC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Handler_IgnoreArg_irqHandler();
    Nvic_Set_PeriphIrq_Handler_AddCallback( Ut_Dma2D_NvicHandlerStub );
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( UT_DMA2D_NVIC, UT_DMA2D_PRIO, NVIC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Active_ExpectAndReturn( UT_DMA2D_NVIC, NVIC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Init( &config ) );

    TEST_ASSERT_NOT_NULL( utDma2D_Isr );
    TEST_ASSERT_EQUAL_HEX32( ( UT_DMA2D_DEAD_TIME << DMA2D_AMTCR_DT_Pos ) | DMA2D_AMTCR_EN, UT_DMA2D_REG->AMTCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_ClutLoadState( UT_DMA2D_PERIPH, &clutState ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, clutState );
}


/**
 * \brief   Dma2D_Init() in POLL mode disables the interrupt in NVIC.
 *
 * \par Expected results
 * - RCC clock and reset, Nvic_Set_PeriphIrq_Inactive() of DMA2D line, no handler.
 */
void Ut_Dma2D_Init_PollMode_InterruptDisabled( void )
{
    const dma2d_Config_t config = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_POLL );

    Rcc_Set_PeriphActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( UT_DMA2D_NVIC, NVIC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Init( &config ) );

    TEST_ASSERT_NULL( utDma2D_Isr );
}


/**
 * \brief   Dma2D_Init() reports failures of RCC and NVIC.
 *
 * \details Clock enable fails; NVIC handler registration fails.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, peripheral not initialized (transfer start refused).
 */
void Ut_Dma2D_Init_RccOrNvicError_NotInitialized( void )
{
    const dma2d_Config_t        config  = Ut_Dma2D_Get_Config( DMA2D_XFER_MODE_ISR );
    const dma2d_OutputConfig_t  output  = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t   request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0xF800u };

    Rcc_Set_PeriphActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_ERROR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( &config ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    Ut_Dma2D_Ignore_PeriphMocks();
    Nvic_Set_PeriphIrq_Handler_Stub( NULL );
    Nvic_Set_PeriphIrq_Handler_IgnoreAndReturn( NVIC_REQUEST_ERROR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Init( &config ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
}


/**
 * \brief   Dma2D_Deinit() disables the interrupt, resets the peripheral and its clock.
 *
 * \details Initialized peripheral deinitialized, invalid peripheral, RCC failure.
 *
 * \par Expected results
 * - NVIC line disabled, RCC reset pulse and clock disabled, DMA2D_REQUEST_OK, transfer
 *   start refused afterwards.
 * - Invalid peripheral and RCC failure: DMA2D_REQUEST_ERROR.
 */
void Ut_Dma2D_Deinit_InterruptResetAndClockDisabled( void )
{
    const dma2d_OutputConfig_t output  = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0xF800u };

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    Ut_Dma2D_Reset_Mocks();

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( UT_DMA2D_NVIC, NVIC_REQUEST_OK );
    Rcc_Set_ResetActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_PeriphInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Deinit( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Deinit( DMA2D_PERIPH_CNT ) );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( UT_DMA2D_NVIC, NVIC_REQUEST_OK );
    Rcc_Set_ResetActive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_OK );
    Rcc_Set_PeriphInactive_ExpectAndReturn( UT_DMA2D_RCC, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Deinit( UT_DMA2D_PERIPH ) );
}

/* =========================== TRANSFER START =============================== */

/**
 * \brief   Register to memory transfer fills the output area.
 *
 * \details RGB565 output 40 x 30 pixels, line offset 200, fill color 0xF800, ISR mode;
 *          then DMA2D interrupt with TCIF.
 *
 * \par Expected results
 * - CR: MODE = R2M, TCIE | TEIE | CEIE, START; OPFCCR = RGB565, OCOLR = 0xF800, OMAR,
 *   OOR = 200, NLR = 40 << 16 | 30, IFCR all flags; transfer RUNNING; foreground not
 *   written.
 * - TCIF: complete callback, transfer IDLE, interrupt sources disabled, CTCIF cleared.
 */
void Ut_Dma2D_Xfer_R2M_RegistersAndCompletion( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0xF800u };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_MODE_R2M | UT_DMA2D_CR_XFER_IT | DMA2D_CR_START, UT_DMA2D_REG->CR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_OUTPUT_MODE_RGB565, UT_DMA2D_REG->OPFCCR );
    TEST_ASSERT_EQUAL_HEX32( 0xF800u, UT_DMA2D_REG->OCOLR );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_SRAM_ADDR( UT_DMA2D_OUT_OFFSET ), UT_DMA2D_REG->OMAR );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_OUT_OFFSET_PIXELS, UT_DMA2D_REG->OOR );
    TEST_ASSERT_EQUAL_HEX32( ( UT_DMA2D_PIXELS << DMA2D_NLR_PL_Pos ) | UT_DMA2D_LINES, UT_DMA2D_REG->NLR );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_IFCR_ALL, UT_DMA2D_REG->IFCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->FGMAR | UT_DMA2D_REG->FGPFCCR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );

    UT_DMA2D_REG->IFCR = 0u;
    Ut_Dma2D_Call_Isr( DMA2D_ISR_TCIF );

    Ut_Dma2D_Check_XferEnd( DMA2D_XFER_ERROR_NONE );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );
    TEST_ASSERT_EQUAL_HEX32( DMA2D_IFCR_CTCIF, UT_DMA2D_REG->IFCR );
}


/**
 * \brief   Memory to memory transfer with pixel format conversion programs the foreground.
 *
 * \details Foreground RGB888 with line offset 3, alpha replace 0x80, output ARGB8888.
 *
 * \par Expected results
 * - CR.MODE = M2M_PFC; FGMAR = foreground, FGOR = 3, FGPFCCR = CM RGB888 | AM replace |
 *   ALPHA 0x80, OPFCCR = ARGB8888; background registers not written.
 */
void Ut_Dma2D_Xfer_M2MPfc_ForegroundProgrammed( void )
{
    const dma2d_OutputConfig_t output     = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 );
    dma2d_LayerConfig_t        foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB888, UT_DMA2D_FG_OFFSET + 1u );
    dma2d_XferRequest_t        request    = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };

    foreground.LineOffset = 3u;
    foreground.AlphaMode  = DMA2D_ALPHA_MODE_REPLACE;
    foreground.Alpha      = 0x80u;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_MODE_M2M_PFC, UT_DMA2D_REG->CR & DMA2D_CR_MODE );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_SRAM_ADDR( UT_DMA2D_FG_OFFSET + 1u ), UT_DMA2D_REG->FGMAR );
    TEST_ASSERT_EQUAL_HEX32( 3u, UT_DMA2D_REG->FGOR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_INPUT_MODE_RGB888 | LL_DMA2D_ALPHA_MODE_REPLACE | ( 0x80u << DMA2D_FGPFCCR_ALPHA_Pos ),
                             UT_DMA2D_REG->FGPFCCR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_OUTPUT_MODE_ARGB8888, UT_DMA2D_REG->OPFCCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->BGMAR | UT_DMA2D_REG->BGPFCCR );
}


/**
 * \brief   Blending transfer programs both layers, A8 layer color is masked to RGB888.
 *
 * \details Foreground A8 with color 0xFF123456 and alpha combine 0x40, background RGB565,
 *          output RGB565.
 *
 * \par Expected results
 * - CR.MODE = M2M_BLEND; FGPFCCR = CM A8 | AM combine | ALPHA 0x40, FGCOLR = 0x123456;
 *   BGMAR = background, BGPFCCR = CM RGB565 | ALPHA 0xFF.
 */
void Ut_Dma2D_Xfer_Blend_BothLayersProgrammed( void )
{
    const dma2d_OutputConfig_t output     = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    dma2d_LayerConfig_t        foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_A8, UT_DMA2D_FG_OFFSET );
    const dma2d_LayerConfig_t  background = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_BG_OFFSET );
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M_BLEND, .Foreground = &foreground, .Background = &background, .Output = &output };

    foreground.Color     = 0xFF123456u;
    foreground.AlphaMode = DMA2D_ALPHA_MODE_COMBINE;
    foreground.Alpha     = 0x40u;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_MODE_M2M_BLEND, UT_DMA2D_REG->CR & DMA2D_CR_MODE );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_INPUT_MODE_A8 | LL_DMA2D_ALPHA_MODE_COMBINE | ( 0x40u << DMA2D_FGPFCCR_ALPHA_Pos ),
                             UT_DMA2D_REG->FGPFCCR );
    TEST_ASSERT_EQUAL_HEX32( 0x123456u, UT_DMA2D_REG->FGCOLR );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_SRAM_ADDR( UT_DMA2D_BG_OFFSET ), UT_DMA2D_REG->BGMAR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_INPUT_MODE_RGB565 | ( 0xFFu << DMA2D_BGPFCCR_ALPHA_Pos ), UT_DMA2D_REG->BGPFCCR );
}


/**
 * \brief   Memory to memory copy keeps the pixel format.
 *
 * \details M2M with foreground ARGB4444 and output ARGB4444, then output RGB565.
 *
 * \par Expected results
 * - Equal color modes: DMA2D_REQUEST_OK, CR.MODE = M2M.
 * - Different color modes: DMA2D_REQUEST_ERROR.
 */
void Ut_Dma2D_Xfer_M2M_ColorModesMustMatch( void )
{
    dma2d_OutputConfig_t       output     = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB4444 );
    const dma2d_LayerConfig_t  foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_ARGB4444, UT_DMA2D_FG_OFFSET );
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M, .Foreground = &foreground, .Output = &output };

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    output.ColorMode = DMA2D_COLOR_MODE_RGB565;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    output.ColorMode = DMA2D_COLOR_MODE_ARGB4444;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_MODE_M2M, UT_DMA2D_REG->CR & DMA2D_CR_MODE );
}


/**
 * \brief   Invalid transfer requests are refused without start.
 *
 * \details NULL request, invalid mode, no output, output without address, input-only output
 *          color mode, 0 / too many pixels per line, 0 lines, line offset too big,
 *          misaligned ARGB8888 / RGB565 output, PFC without foreground, foreground without
 *          address / with invalid color / alpha mode / misaligned, blending without
 *          background.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, CR not written, transfer IDLE.
 */
void Ut_Dma2D_Xfer_InvalidRequest_ReturnsErrorWithoutStart( void )
{
    dma2d_OutputConfig_t output     = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 );
    dma2d_LayerConfig_t  foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_FG_OFFSET );
    dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };
    dma2d_XferState_t    xferState  = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, NULL ) );
    request.Mode = DMA2D_MODE_CNT;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    request.Mode = DMA2D_MODE_M2M_PFC;
    request.Output = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    request.Output = &output;

    output.Address = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_L8 );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 ); output.PixelsPerLine = 0u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output.PixelsPerLine = DMA2D_PIXELS_PER_LINE_MAX + 1u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 ); output.NumberOfLines = 0u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 ); output.LineOffset = DMA2D_LINE_OFFSET_MAX + 1u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 ); output.Address = REGMEM_SRAM_PTR( uint8_t, UT_DMA2D_OUT_OFFSET + 2u );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 ); output.Address = REGMEM_SRAM_PTR( uint8_t, UT_DMA2D_OUT_OFFSET + 1u );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    output = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 );

    request.Foreground = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    request.Foreground = &foreground;
    foreground.Address = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_CNT, UT_DMA2D_FG_OFFSET );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_FG_OFFSET ); foreground.AlphaMode = DMA2D_ALPHA_MODE_CNT;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_FG_OFFSET + 1u );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_FG_OFFSET ); foreground.LineOffset = DMA2D_LINE_OFFSET_MAX + 1u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_RGB565, UT_DMA2D_FG_OFFSET );

    request.Mode       = DMA2D_MODE_M2M_BLEND;
    request.Background = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( DMA2D_PERIPH_CNT, &request ) );
}


/**
 * \brief   Second transfer is refused while a transfer runs.
 *
 * \details R2M started, second R2M and CLUT loading requested.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR for both, transfer still RUNNING.
 */
void Ut_Dma2D_Xfer_Running_SecondRequestRefused( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x07E0u };
    const dma2d_ClutConfig_t   clut      = { .Address = REGMEM_SRAM_PTR( uint32_t, UT_DMA2D_CLUT_OFFSET ), .ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888, .Size = 16u };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );
}

/* ============================ EVENTS / ERRORS ============================= */

/**
 * \brief   Error flags end the transfer with the error of the highest priority.
 *
 * \details R2M transfers ended by interrupts with CEIF | TEIF, TEIF | CAEIF and CAEIF.
 *
 * \par Expected results
 * - CONFIG, TRANSFER and CLUT_ACCESS reported once each, transfer IDLE, error flags
 *   cleared, complete callback not called.
 */
void Ut_Dma2D_Events_ErrorFlags_ReportedByPriority( void )
{
    const dma2d_OutputConfig_t output  = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    const struct
    {
        uint32_t            IsrFlags;
        dma2d_XferErrorId_t ErrorId;
    }   errorLut[] =
    {
        { DMA2D_ISR_CEIF | DMA2D_ISR_TEIF,  DMA2D_XFER_ERROR_CONFIG      },
        { DMA2D_ISR_TEIF | DMA2D_ISR_CAEIF, DMA2D_XFER_ERROR_TRANSFER    },
        { DMA2D_ISR_CAEIF,                  DMA2D_XFER_ERROR_CLUT_ACCESS },
    };

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    for( uint32_t idx = 0u; ( sizeof( errorLut ) / sizeof( errorLut[ 0u ] ) ) > idx; idx++ )
    {
        utDma2D_ErrorCnt = 0u;

        TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
        UT_DMA2D_REG->IFCR = 0u;
        Ut_Dma2D_Call_Isr( errorLut[ idx ].IsrFlags );

        Ut_Dma2D_Check_XferEnd( errorLut[ idx ].ErrorId );
        TEST_ASSERT_EQUAL_HEX32( DMA2D_IFCR_CCEIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CAECIF, UT_DMA2D_REG->IFCR );
        TEST_ASSERT_EQUAL_UINT32( 0u, utDma2D_CompleteCnt );
    }
}


/**
 * \brief   Flags without running operation are only cleared.
 *
 * \details No transfer, interrupt with TCIF | CTCIF | TWIF | TEIF.
 *
 * \par Expected results
 * - No callback, all flags cleared.
 */
void Ut_Dma2D_Events_NoOperation_FlagsOnlyCleared( void )
{
    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    Ut_Dma2D_Call_Isr( DMA2D_ISR_TCIF | DMA2D_ISR_CTCIF | DMA2D_ISR_TWIF | DMA2D_ISR_TEIF );

    TEST_ASSERT_EQUAL_UINT32( 0u, utDma2D_CompleteCnt + utDma2D_ClutCnt + utDma2D_WatermarkCnt + utDma2D_ErrorCnt );
}


/**
 * \brief   Line watermark is reported during the transfer.
 *
 * \details Watermark at line 10, R2M transfer, interrupt with TWIF, then TCIF; watermark
 *          disabled and next transfer started.
 *
 * \par Expected results
 * - LWR = 10, watermark read back; CR.TWIE set at start; TWIF: watermark callback, transfer
 *   still RUNNING; TCIF: complete callback.
 * - Watermark disabled: TWIE not set by next start; configuration refused while running.
 */
void Ut_Dma2D_Watermark_ReportedDuringTransfer( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    dma2d_FunctionState_t      wmState   = DMA2D_FUNCTION_INACTIVE;
    dma2d_LineCnt_t            wmLine    = 0u;
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_LineWatermark( UT_DMA2D_PERIPH, 10u ) );
    TEST_ASSERT_EQUAL_HEX32( 10u, UT_DMA2D_REG->LWR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_LineWatermark( UT_DMA2D_PERIPH, &wmState, &wmLine ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_ACTIVE, wmState );
    TEST_ASSERT_EQUAL_UINT16( 10u, wmLine );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL_HEX32( DMA2D_CR_TWIE, UT_DMA2D_REG->CR & DMA2D_CR_TWIE );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_LineWatermark( UT_DMA2D_PERIPH, 5u ) );

    Ut_Dma2D_Call_Isr( DMA2D_ISR_TWIF );
    TEST_ASSERT_EQUAL_UINT32( 1u, utDma2D_WatermarkCnt );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );

    Ut_Dma2D_Call_Isr( DMA2D_ISR_TCIF );
    Ut_Dma2D_Check_XferEnd( DMA2D_XFER_ERROR_NONE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_LineWatermarkInactive( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & DMA2D_CR_TWIE );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_LineWatermarkInactive( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_LineWatermark( UT_DMA2D_PERIPH, NULL, &wmLine ) );
}

/* ======================= ABORT / SUSPEND / RESUME ========================= */

/**
 * \brief   Running transfer is suspended, resumed and aborted.
 *
 * \details R2M started, suspended, resumed, suspended again and aborted (HW model ends
 *          the abort); abort without running transfer.
 *
 * \par Expected results
 * - Suspend: CR.SUSP set, state SUSPENDED; resume: SUSP cleared, RUNNING; second resume
 *   refused.
 * - Abort: DMA2D_REQUEST_OK, START cleared, interrupt sources and SUSP cleared, IFCR all
 *   flags, state IDLE, no callback.
 * - Abort / suspend without transfer: abort OK, suspend refused.
 */
void Ut_Dma2D_Xfer_SuspendResumeAbort( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( Ut_Dma2D_HwModel ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferSuspend( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL_HEX32( DMA2D_CR_SUSP, UT_DMA2D_REG->CR & DMA2D_CR_SUSP );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_SUSPENDED, xferState );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferResume( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & DMA2D_CR_SUSP );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferResume( UT_DMA2D_PERIPH ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferSuspend( UT_DMA2D_PERIPH ) );
    UT_DMA2D_REG->IFCR = 0u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStop( UT_DMA2D_PERIPH ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & ( DMA2D_CR_START | DMA2D_CR_SUSP | UT_DMA2D_CR_ALL_IT ) );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_IFCR_ALL, UT_DMA2D_REG->IFCR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
    TEST_ASSERT_EQUAL_UINT32( 0u, utDma2D_CompleteCnt + utDma2D_ErrorCnt );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStop( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferSuspend( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStop( DMA2D_PERIPH_CNT ) );
}


/**
 * \brief   Abort not finished by HW is reported.
 *
 * \details R2M started, START stays set (no HW model), Dma2D_Set_XferStop().
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, ABORT requested, state IDLE (new start possible).
 */
void Ut_Dma2D_Xfer_AbortTimeout_ReturnsError( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStop( UT_DMA2D_PERIPH ) );

    TEST_ASSERT_EQUAL_HEX32( DMA2D_CR_ABORT, UT_DMA2D_REG->CR & DMA2D_CR_ABORT );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
}


/**
 * \brief   Dma2D_Deinit() and Dma2D_Init() abort a running transfer.
 *
 * \details R2M running (HW model ends the abort), Dma2D_Init() again, transfer started,
 *          Dma2D_Deinit().
 *
 * \par Expected results
 * - Both return DMA2D_REQUEST_OK, START cleared, no callback.
 */
void Ut_Dma2D_InitDeinit_RunningTransferAborted( void )
{
    const dma2d_OutputConfig_t output  = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( Ut_Dma2D_HwModel ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & DMA2D_CR_START );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Deinit( UT_DMA2D_PERIPH ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & DMA2D_CR_START );
    TEST_ASSERT_EQUAL_UINT32( 0u, utDma2D_CompleteCnt + utDma2D_ErrorCnt );
}

/* ============================== CLUT LOADING ============================== */

/**
 * \brief   CLUT of the foreground is loaded and kept by the following L8 transfer.
 *
 * \details ARGB8888 CLUT of 256 entries loaded to foreground, interrupt with CTCIF, then
 *          PFC transfer of L8 foreground.
 *
 * \par Expected results
 * - FGCMAR = CLUT address, FGPFCCR: CCM = ARGB8888, CS = 255, START; CR = CLUT interrupt
 *   sources; loading ACTIVE.
 * - CTCIF: CLUT complete callback, loading INACTIVE, CLUT interrupt sources disabled.
 * - L8 transfer: FGPFCCR CM = L8, ALPHA = 0xFF, CS = 255 kept.
 */
void Ut_Dma2D_Clut_ForegroundLoadedAndKept( void )
{
    const dma2d_ClutConfig_t   clut       = { .Address = REGMEM_SRAM_PTR( uint32_t, UT_DMA2D_CLUT_OFFSET ), .ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888, .Size = 256u };
    const dma2d_OutputConfig_t output     = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_ARGB8888 );
    const dma2d_LayerConfig_t  foreground = Ut_Dma2D_Get_Layer( DMA2D_COLOR_MODE_L8, UT_DMA2D_FG_OFFSET );
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };
    dma2d_FunctionState_t      clutState  = DMA2D_FUNCTION_INACTIVE;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );

    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_SRAM_ADDR( UT_DMA2D_CLUT_OFFSET ), UT_DMA2D_REG->FGCMAR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_CLUT_COLOR_MODE_ARGB8888 | ( 255u << DMA2D_FGPFCCR_CS_Pos ) | DMA2D_FGPFCCR_START, UT_DMA2D_REG->FGPFCCR );
    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_CR_CLUT_IT, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_ClutLoadState( UT_DMA2D_PERIPH, &clutState ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_ACTIVE, clutState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    UT_DMA2D_REG->FGPFCCR &= ~DMA2D_FGPFCCR_START;
    Ut_Dma2D_Call_Isr( DMA2D_ISR_CTCIF );

    TEST_ASSERT_EQUAL_UINT32( 1u, utDma2D_ClutCnt );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_ClutLoadState( UT_DMA2D_PERIPH, &clutState ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, clutState );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_INPUT_MODE_L8 | ( 255u << DMA2D_FGPFCCR_CS_Pos ) | ( 0xFFu << DMA2D_FGPFCCR_ALPHA_Pos ), UT_DMA2D_REG->FGPFCCR );
}


/**
 * \brief   CLUT of the background is loaded, CLUT errors are reported.
 *
 * \details RGB888 CLUT of 16 entries loaded to background, interrupt with CAEIF.
 *
 * \par Expected results
 * - BGCMAR = CLUT address, BGPFCCR: CCM = RGB888, CS = 15, START.
 * - CAEIF: error callback CLUT_ACCESS, loading INACTIVE.
 */
void Ut_Dma2D_Clut_BackgroundLoadError_Reported( void )
{
    const dma2d_ClutConfig_t clut      = { .Address = REGMEM_SRAM_PTR( uint8_t, UT_DMA2D_CLUT_OFFSET + 1u ), .ColorMode = DMA2D_CLUT_COLOR_MODE_RGB888, .Size = 16u };
    dma2d_FunctionState_t    clutState = DMA2D_FUNCTION_ACTIVE;
    dma2d_XferErrorId_t      xferError = DMA2D_XFER_ERROR_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_BACKGROUND, &clut ) );

    TEST_ASSERT_EQUAL_HEX32( UT_DMA2D_SRAM_ADDR( UT_DMA2D_CLUT_OFFSET + 1u ), UT_DMA2D_REG->BGCMAR );
    TEST_ASSERT_EQUAL_HEX32( LL_DMA2D_CLUT_COLOR_MODE_RGB888 | ( 15u << DMA2D_BGPFCCR_CS_Pos ) | DMA2D_BGPFCCR_START, UT_DMA2D_REG->BGPFCCR );

    Ut_Dma2D_Call_Isr( DMA2D_ISR_CAEIF );

    TEST_ASSERT_EQUAL_UINT32( 1u, utDma2D_ErrorCnt );
    TEST_ASSERT_EQUAL( DMA2D_XFER_ERROR_CLUT_ACCESS, utDma2D_LastError );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_ClutLoadState( UT_DMA2D_PERIPH, &clutState ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, clutState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferError( UT_DMA2D_PERIPH, &xferError ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_ERROR_CLUT_ACCESS, xferError );
}


/**
 * \brief   Invalid CLUT loading requests are refused.
 *
 * \details Invalid layer, NULL configuration / address, size 0 and 257, invalid color mode,
 *          misaligned ARGB8888 CLUT, not initialized peripheral.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, CLUT registers not written.
 */
void Ut_Dma2D_Clut_InvalidRequest_ReturnsError( void )
{
    dma2d_ClutConfig_t clut = { .Address = REGMEM_SRAM_PTR( uint32_t, UT_DMA2D_CLUT_OFFSET ), .ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888, .Size = 16u };

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_CNT, &clut ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, NULL ) );
    clut.Size = 0u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );
    clut.Size = DMA2D_CLUT_SIZE_MAX + 1u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );
    clut.Size      = 16u;
    clut.ColorMode = DMA2D_CLUT_COLOR_MODE_CNT;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );
    clut.ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888;
    clut.Address   = REGMEM_SRAM_PTR( uint8_t, UT_DMA2D_CLUT_OFFSET + 2u );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );
    clut.Address   = NULL;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->FGCMAR | UT_DMA2D_REG->FGPFCCR );
}

/* =============================== POLL MODE ================================ */

/**
 * \brief   POLL data handling processes the flags in Dma2D_Task().
 *
 * \details POLL mode: R2M started, Dma2D_Task() without flag, with TCIF; CLUT loading and
 *          Dma2D_Task() with CTCIF.
 *
 * \par Expected results
 * - No interrupt source enabled at start; task without flag: transfer RUNNING; TCIF:
 *   complete callback; CTCIF: CLUT complete callback.
 */
void Ut_Dma2D_Poll_TaskProcessesFlags( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    const dma2d_ClutConfig_t   clut      = { .Address = REGMEM_SRAM_PTR( uint32_t, UT_DMA2D_CLUT_OFFSET ), .ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888, .Size = 16u };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );

    Dma2D_Task();
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );

    UT_DMA2D_REG->ISR = DMA2D_ISR_TCIF;
    Dma2D_Task();
    Ut_Dma2D_Check_XferEnd( DMA2D_XFER_ERROR_NONE );

    UT_DMA2D_REG->ISR = 0u;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_ClutLoad( UT_DMA2D_PERIPH, DMA2D_LAYER_FOREGROUND, &clut ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, UT_DMA2D_REG->CR & UT_DMA2D_CR_ALL_IT );
    UT_DMA2D_REG->ISR = DMA2D_ISR_CTCIF;
    Dma2D_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utDma2D_ClutCnt );
}


/**
 * \brief   Dma2D_Task() does not process flags of ISR data handling.
 *
 * \details ISR mode, R2M started, TCIF preset, Dma2D_Task().
 *
 * \par Expected results
 * - No callback, transfer RUNNING.
 */
void Ut_Dma2D_Task_IsrMode_NotProcessed( void )
{
    const dma2d_OutputConfig_t output    = Ut_Dma2D_Get_Output( DMA2D_COLOR_MODE_RGB565 );
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x001Fu };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    Ut_Dma2D_Init( DMA2D_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( UT_DMA2D_PERIPH, &request ) );

    UT_DMA2D_REG->ISR = DMA2D_ISR_TCIF;
    Dma2D_Task();

    TEST_ASSERT_EQUAL_UINT32( 0u, utDma2D_CompleteCnt );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_RUNNING, xferState );
}

/* ============================ CONFIGURATION =============================== */

/**
 * \brief   Dead time is written to AMTCR and read back.
 *
 * \par Expected results
 * - Active 200 cycles: AMTCR = 200 << 8 | EN; inactive: EN cleared; getter returns it.
 * - Invalid state / peripheral / NULL: DMA2D_REQUEST_ERROR.
 */
void Ut_Dma2D_Set_DeadTime_RoundTrip( void )
{
    dma2d_FunctionState_t state    = DMA2D_FUNCTION_INACTIVE;
    dma2d_DeadTime_t      deadTime = 0u;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_DeadTime( UT_DMA2D_PERIPH, DMA2D_FUNCTION_ACTIVE, 200u ) );
    TEST_ASSERT_EQUAL_HEX32( ( 200u << DMA2D_AMTCR_DT_Pos ) | DMA2D_AMTCR_EN, UT_DMA2D_REG->AMTCR );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_DeadTime( UT_DMA2D_PERIPH, &state, &deadTime ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_ACTIVE, state );
    TEST_ASSERT_EQUAL_UINT8( 200u, deadTime );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_DeadTime( UT_DMA2D_PERIPH, DMA2D_FUNCTION_INACTIVE, 5u ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_DeadTime( UT_DMA2D_PERIPH, &state, &deadTime ) );
    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, state );
    TEST_ASSERT_EQUAL_UINT8( 5u, deadTime );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_DeadTime( UT_DMA2D_PERIPH, (dma2d_FunctionState_t)2u, 5u ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_DeadTime( DMA2D_PERIPH_CNT, DMA2D_FUNCTION_ACTIVE, 5u ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_DeadTime( UT_DMA2D_PERIPH, NULL, &deadTime ) );
}


/**
 * \brief   Interrupt priority is passed to NVIC and read from it.
 *
 * \par Expected results
 * - Nvic_Set_PeriphIrq_Prio / Nvic_Get_PeriphIrq_Prio of DMA2D line, failures reported.
 */
void Ut_Dma2D_IrqPriority_PassedToNvic( void )
{
    nvic_IrqPrio_t  nvicPrio = 3u;
    dma2d_IrqPrio_t irqPrio  = 0u;

    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( UT_DMA2D_NVIC, UT_DMA2D_PRIO, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_IrqPriority( UT_DMA2D_PERIPH, UT_DMA2D_PRIO ) );

    Nvic_Get_PeriphIrq_Prio_ExpectAndReturn( UT_DMA2D_NVIC, NULL, NVIC_REQUEST_OK );
    Nvic_Get_PeriphIrq_Prio_IgnoreArg_irqPrio();
    Nvic_Get_PeriphIrq_Prio_ReturnThruPtr_irqPrio( &nvicPrio );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_IrqPriority( UT_DMA2D_PERIPH, &irqPrio ) );
    TEST_ASSERT_EQUAL_UINT32( 3u, irqPrio );

    Nvic_Get_PeriphIrq_Prio_ExpectAndReturn( UT_DMA2D_NVIC, NULL, NVIC_REQUEST_ERROR );
    Nvic_Get_PeriphIrq_Prio_IgnoreArg_irqPrio();
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_IrqPriority( UT_DMA2D_PERIPH, &irqPrio ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_IrqPriority( DMA2D_PERIPH_CNT, UT_DMA2D_PRIO ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_IrqPriority( UT_DMA2D_PERIPH, NULL ) );
}


/**
 * \brief   Dma2D_Get_PixelBits() returns the pixel size of every color mode.
 *
 * \par Expected results
 * - 32, 24, 16, 16, 16, 8, 8, 16, 4, 8, 4 bits; invalid mode / NULL rejected.
 */
void Ut_Dma2D_Get_PixelBits_AllColorModes( void )
{
    const uint32_t expBits[ DMA2D_COLOR_MODE_CNT ] = { 32u, 24u, 16u, 16u, 16u, 8u, 8u, 16u, 4u, 8u, 4u };
    uint32_t       pixelBits = 0u;

    for( dma2d_ColorMode_t colorMode = DMA2D_COLOR_MODE_ARGB8888; DMA2D_COLOR_MODE_CNT > colorMode; colorMode++ )
    {
        TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_PixelBits( colorMode, &pixelBits ) );
        TEST_ASSERT_EQUAL_UINT32( expBits[ colorMode ], pixelBits );
    }

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_PixelBits( DMA2D_COLOR_MODE_CNT, &pixelBits ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_PixelBits( DMA2D_COLOR_MODE_RGB565, NULL ) );
}


/**
 * \brief   State getters reject invalid parameters.
 *
 * \par Expected results
 * - Invalid peripheral or NULL pointer: DMA2D_REQUEST_ERROR.
 */
void Ut_Dma2D_Getters_InvalidParams_ReturnError( void )
{
    dma2d_XferState_t     xferState;
    dma2d_XferErrorId_t   xferError;
    dma2d_FunctionState_t clutState;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_XferState( DMA2D_PERIPH_CNT, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_XferState( UT_DMA2D_PERIPH, NULL ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_XferError( DMA2D_PERIPH_CNT, &xferError ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_XferError( UT_DMA2D_PERIPH, NULL ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_ClutLoadState( DMA2D_PERIPH_CNT, &clutState ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Get_ClutLoadState( UT_DMA2D_PERIPH, NULL ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_LineWatermark( UT_DMA2D_PERIPH, 1u ) );   /* Not initialized */
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Verifies and resets all mocks (CMock memory is common for all mocks - a single mock
 *        is never reset alone).
 */
static void Ut_Dma2D_Reset_Mocks( void )
{
    MockRcc_Port_Verify();
    MockNvic_Port_Verify();

    MockRcc_Port_Destroy();
    MockNvic_Port_Destroy();

    MockRcc_Port_Init();
    MockNvic_Port_Init();
}


/**
 * \brief Resets the mocks and accepts any RCC / NVIC call (handler stored, NVIC disable
 *        counted).
 */
static void Ut_Dma2D_Ignore_PeriphMocks( void )
{
    Ut_Dma2D_Reset_Mocks();

    Rcc_Set_PeriphActive_IgnoreAndReturn( RCC_REQUEST_OK );
    Rcc_Set_PeriphInactive_IgnoreAndReturn( RCC_REQUEST_OK );
    Rcc_Set_ResetActive_IgnoreAndReturn( RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_IgnoreAndReturn( RCC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Handler_Stub( Ut_Dma2D_NvicHandlerStub );
    Nvic_Set_PeriphIrq_Prio_IgnoreAndReturn( NVIC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Active_IgnoreAndReturn( NVIC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Inactive_Stub( Ut_Dma2D_NvicInactiveStub );
}


/**
 * \brief Returns configuration of tests (priority 7, dead time 10 cycles active, test
 *        callbacks).
 *
 * \param xferMode [in]: Data handling mode
 */
static dma2d_Config_t Ut_Dma2D_Get_Config( dma2d_XferMode_t xferMode )
{
    dma2d_Config_t config;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_DefaultConfig( &config ) );

    config.PeriphId             = UT_DMA2D_PERIPH;
    config.XferMode             = xferMode;
    config.IrqPriority          = UT_DMA2D_PRIO;
    config.DeadTimeState        = DMA2D_FUNCTION_ACTIVE;
    config.DeadTime             = UT_DMA2D_DEAD_TIME;
    config.XferCompleteCallback = Ut_Dma2D_XferCompleteCallback;
    config.ClutCompleteCallback = Ut_Dma2D_ClutCompleteCallback;
    config.WatermarkCallback    = Ut_Dma2D_WatermarkCallback;
    config.ErrorCallback        = Ut_Dma2D_ErrorCallback;

    return ( config );
}


/**
 * \brief Initializes DMA2D with ignored mocks.
 *
 * \param xferMode [in]: Data handling mode
 */
static void Ut_Dma2D_Init( dma2d_XferMode_t xferMode )
{
    const dma2d_Config_t config = Ut_Dma2D_Get_Config( xferMode );

    Ut_Dma2D_Ignore_PeriphMocks();

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Init( &config ) );
}


/**
 * \brief Returns output area of tests (SRAM, 40 x 30 pixels, line offset 200).
 *
 * \param colorMode [in]: Output color mode
 */
static dma2d_OutputConfig_t Ut_Dma2D_Get_Output( dma2d_ColorMode_t colorMode )
{
    const dma2d_OutputConfig_t output =
    {
        .Address       = REGMEM_SRAM_PTR( uint8_t, UT_DMA2D_OUT_OFFSET ),
        .LineOffset    = UT_DMA2D_OUT_OFFSET_PIXELS,
        .ColorMode     = colorMode,
        .PixelsPerLine = UT_DMA2D_PIXELS,
        .NumberOfLines = UT_DMA2D_LINES,
    };

    return ( output );
}


/**
 * \brief Returns input layer of tests (no line offset, no alpha modification, black).
 *
 * \param colorMode  [in]: Color mode of the layer
 * \param sramOffset [in]: Offset of the layer in emulated SRAM
 */
static dma2d_LayerConfig_t Ut_Dma2D_Get_Layer( dma2d_ColorMode_t colorMode, uint32_t sramOffset )
{
    const dma2d_LayerConfig_t layer =
    {
        .Address    = REGMEM_SRAM_PTR( uint8_t, sramOffset ),
        .LineOffset = 0u,
        .ColorMode  = colorMode,
        .AlphaMode  = DMA2D_ALPHA_MODE_NO_MODIF,
        .Alpha      = 0xFFu,
        .Color      = 0u,
    };

    return ( layer );
}


/**
 * \brief Calls the captured DMA2D interrupt with given status flags.
 *
 * \param isrFlags [in]: Value of the ISR register
 */
static void Ut_Dma2D_Call_Isr( uint32_t isrFlags )
{
    TEST_ASSERT_NOT_NULL_MESSAGE( utDma2D_Isr, "DMA2D ISR not registered" );

    UT_DMA2D_REG->ISR = isrFlags;
    utDma2D_Isr();
}


/**
 * \brief Checks the end of the transfer - callback of the expected result, transfer IDLE and
 *        the stored error.
 *
 * \param expError [in]: Expected result of the transfer
 */
static void Ut_Dma2D_Check_XferEnd( dma2d_XferErrorId_t expError )
{
    dma2d_XferState_t   xferState = DMA2D_XFER_STATE_CNT;
    dma2d_XferErrorId_t xferError = DMA2D_XFER_ERROR_CNT;

    if( DMA2D_XFER_ERROR_NONE == expError )
    {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE( 0u, utDma2D_ErrorCnt, "Error callback" );
        TEST_ASSERT_NOT_EQUAL_MESSAGE( 0u, utDma2D_CompleteCnt, "Complete callback" );
    }
    else
    {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE( 1u, utDma2D_ErrorCnt, "Count of error callbacks" );
        TEST_ASSERT_EQUAL( expError, utDma2D_LastError );
    }

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( UT_DMA2D_PERIPH, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferError( UT_DMA2D_PERIPH, &xferError ) );
    TEST_ASSERT_EQUAL( expError, xferError );
}


/**
 * \brief HW model running in background - abort request clears START bits of the transfer and
 *        of CLUT loading (and the ABORT bit).
 *
 * Atomic bit operations do not overwrite concurrent writes of the module.
 */
static void Ut_Dma2D_HwModel( void )
{
    if( 0u != ( UT_DMA2D_REG->CR & DMA2D_CR_ABORT ) )
    {
        (void)__atomic_and_fetch( &UT_DMA2D_REG->FGPFCCR, ~DMA2D_FGPFCCR_START, __ATOMIC_SEQ_CST );
        (void)__atomic_and_fetch( &UT_DMA2D_REG->BGPFCCR, ~DMA2D_BGPFCCR_START, __ATOMIC_SEQ_CST );
        (void)__atomic_and_fetch( &UT_DMA2D_REG->CR, ~( DMA2D_CR_START | DMA2D_CR_ABORT ), __ATOMIC_SEQ_CST );
    }
    else
    {
        /* No abort request */
    }
}


/** \brief Nvic_Set_PeriphIrq_Handler() stub - stores the handler */
static nvic_RequestState_t Ut_Dma2D_NvicHandlerStub( nvic_PeriphIrqList_t irqId, const nvic_IsrCallback_t irqHandler, int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( UT_DMA2D_NVIC, irqId );
    TEST_ASSERT_NOT_NULL( irqHandler );

    utDma2D_Isr = irqHandler;

    return ( NVIC_REQUEST_OK );
}


/** \brief Nvic_Set_PeriphIrq_Inactive() stub - counts the calls */
static nvic_RequestState_t Ut_Dma2D_NvicInactiveStub( nvic_PeriphIrqList_t irqId, int callCnt )
{
    (void)irqId;
    (void)callCnt;

    utDma2D_NvicOffCnt++;

    return ( utDma2D_NvicOffState );
}


/** \brief Transfer complete callback of tests */
static void Ut_Dma2D_XferCompleteCallback( void )
{
    utDma2D_CompleteCnt++;
}


/** \brief CLUT loading complete callback of tests */
static void Ut_Dma2D_ClutCompleteCallback( void )
{
    utDma2D_ClutCnt++;
}


/** \brief Line watermark callback of tests */
static void Ut_Dma2D_WatermarkCallback( void )
{
    utDma2D_WatermarkCnt++;
}


/**
 * \brief Transfer error callback of tests.
 *
 * \param errorId [in]: Error identification
 */
static void Ut_Dma2D_ErrorCallback( dma2d_XferErrorId_t errorId )
{
    utDma2D_LastError = errorId;
    utDma2D_ErrorCnt++;
}
