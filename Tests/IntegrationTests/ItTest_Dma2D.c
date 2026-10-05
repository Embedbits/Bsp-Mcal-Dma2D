/**
 * \author Mr.Nobody
 * \file ItTest_Dma2D.c
 * \ingroup Dma2D
 * \brief Integration tests of Chrom-ART accelerator (DMA2D) module on target.
 *
 * Dma2D module runs on the MCU together with real RCC and NVIC modules.
 * Tests verify behavior which cannot be verified by unit tests (emulated
 * registers): pixels written by DMA2D into SRAM buffers - fill and copy of
 * rectangles with line offsets, pixel format conversion, blending with
 * deterministic alpha values (0 / 255), CLUT loading and indexed input, end of
 * transfer by interrupt and by polling, line watermark, suspend / resume and
 * abort of a running transfer.
 *
 * Every test case runs after system reset done by the test runner.
 *
 * Used resources:
 * - DMA2D (STM32F427 / 429 / 437 / 439 / 469 / 479), DMA2D interrupt
 * - SRAM buffers, no external wiring
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "IntegrationTesting.h"             /* Integration testing on target  */
#include "Dma2D_Port.h"                     /* Module under test              */
/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

static void                 It_Dma2D_Init           ( dma2d_XferMode_t xferMode, dma2d_FunctionState_t deadTimeState );
static dma2d_XferState_t    It_Dma2D_Wait_Idle      ( void );
static void                 It_Dma2D_Wait_ClutLoaded( void );
static void                 It_Dma2D_XferCompleteCallback ( void );
static void                 It_Dma2D_ClutCompleteCallback ( void );
static void                 It_Dma2D_WatermarkCallback    ( void );
static void                 It_Dma2D_ErrorCallback        ( dma2d_XferErrorId_t errorId );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Peripheral under test */
#define IT_DMA2D_ID                         ( DMA2D_PERIPH_1 )

/** Interrupt priority of the tests */
#define IT_DMA2D_PRIO                       ( 5u )

/** Maximal dead time (slows the transfer down for suspend / abort tests) */
#define IT_DMA2D_DEAD_TIME_MAX              ( 255u )

/** Maximal count of wait loops for the end of a transfer */
#define IT_DMA2D_WAIT_LOOPS                 ( 2000000u )

/** Count of loops of a running transfer checked while suspended */
#define IT_DMA2D_SUSPEND_LOOPS              ( 100000u )

/** Small frame buffer (16 x 8 pixels) */
#define IT_DMA2D_FRAME_WIDTH                ( 16u )
#define IT_DMA2D_FRAME_LINES                ( 8u )
#define IT_DMA2D_FRAME_PIXELS               ( IT_DMA2D_FRAME_WIDTH * IT_DMA2D_FRAME_LINES )

/** Big frame buffer (128 x 128 RGB565 pixels, 32 kB) */
#define IT_DMA2D_BIG_WIDTH                  ( 128u )
#define IT_DMA2D_BIG_LINES                  ( 128u )
#define IT_DMA2D_BIG_PIXELS                 ( IT_DMA2D_BIG_WIDTH * IT_DMA2D_BIG_LINES )

/** Value of untouched pixels */
#define IT_DMA2D_SENTINEL_16                ( 0xA5A5u )
#define IT_DMA2D_SENTINEL_32                ( 0xA5A5A5A5u )

/** Count of CLUT entries of the CLUT test */
#define IT_DMA2D_CLUT_CNT                   ( 4u )

/* ========================== LOCAL VARIABLES =============================== */

/** 16-bit frame buffer */
static uint16_t itDma2D_Frame16[ IT_DMA2D_FRAME_PIXELS ] __attribute__(( aligned( 4u ) ));

/** 32-bit frame buffers (source / destination) */
static uint32_t itDma2D_Source32[ IT_DMA2D_FRAME_PIXELS ] __attribute__(( aligned( 4u ) ));
static uint32_t itDma2D_Frame32[ IT_DMA2D_FRAME_PIXELS ]  __attribute__(( aligned( 4u ) ));

/** Big 16-bit frame buffer */
static uint16_t itDma2D_Big16[ IT_DMA2D_BIG_PIXELS ] __attribute__(( aligned( 4u ) ));

/** 8-bit buffer (A8 / L8 layers) */
static uint8_t itDma2D_Source8[ IT_DMA2D_FRAME_PIXELS ] __attribute__(( aligned( 4u ) ));

/** CLUT of the CLUT test (ARGB8888) */
static const uint32_t itDma2D_Clut[ IT_DMA2D_CLUT_CNT ] __attribute__(( aligned( 4u ) )) =
{
    0xFF112233u, 0xFF445566u, 0x80778899u, 0x00AABBCCu
};

/** Counts of callback calls */
static volatile uint32_t            itDma2D_CompleteCnt  = 0u;
static volatile uint32_t            itDma2D_ClutCnt      = 0u;
static volatile uint32_t            itDma2D_WatermarkCnt = 0u;
static volatile uint32_t            itDma2D_ErrorCnt     = 0u;

/** Count of completed transfers when the watermark was reported */
static volatile uint32_t            itDma2D_CompleteAtWatermark = 0xFFFFFFFFu;

/** Last error reported by error callback */
static volatile dma2d_XferErrorId_t itDma2D_ErrorId = DMA2D_XFER_ERROR_CNT;

/* ============================= TEST SETUP ================================= */

void setUp( void )
{
    itDma2D_CompleteCnt         = 0u;
    itDma2D_ClutCnt             = 0u;
    itDma2D_WatermarkCnt        = 0u;
    itDma2D_ErrorCnt            = 0u;
    itDma2D_CompleteAtWatermark = 0xFFFFFFFFu;
    itDma2D_ErrorId             = DMA2D_XFER_ERROR_CNT;

    for( uint32_t pixelIdx = 0u; IT_DMA2D_FRAME_PIXELS > pixelIdx; pixelIdx++ )
    {
        itDma2D_Frame16[ pixelIdx ]  = IT_DMA2D_SENTINEL_16;
        itDma2D_Frame32[ pixelIdx ]  = IT_DMA2D_SENTINEL_32;
        itDma2D_Source32[ pixelIdx ] = 0xFF000000u | ( pixelIdx * 0x010203u );
        itDma2D_Source8[ pixelIdx ]  = (uint8_t)( pixelIdx % IT_DMA2D_CLUT_CNT );
    }
}


void tearDown( void )
{
    /* Every test case runs after system reset */
}

/* =============================== TESTS ==================================== */

/*----------------------------- Register to memory ---------------------------*/

/**
 * \brief   Register to memory transfer fills a rectangle of the frame buffer.
 *
 * \details RGB565 frame 16 x 8, rectangle 5 x 3 pixels at column 2, line 1 (line
 *          offset 11), fill color 0x1234, ISR data handling.
 *
 * \par Expected results
 * - Transfer complete callback once, state IDLE, result NONE.
 * - Pixels of the rectangle 0x1234, all other pixels untouched.
 */
void It_Dma2D_R2M_FillsRectangleWithLineOffset( void )
{
    const dma2d_OutputConfig_t output  = { .Address = &itDma2D_Frame16[ ( 1u * IT_DMA2D_FRAME_WIDTH ) + 2u ], .LineOffset = IT_DMA2D_FRAME_WIDTH - 5u,
                                           .ColorMode = DMA2D_COLOR_MODE_RGB565, .PixelsPerLine = 5u, .NumberOfLines = 3u };
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x1234u };
    dma2d_XferErrorId_t        error   = DMA2D_XFER_ERROR_CNT;

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferError( IT_DMA2D_ID, &error ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_ERROR_NONE, error );

    for( uint32_t lineIdx = 0u; IT_DMA2D_FRAME_LINES > lineIdx; lineIdx++ )
    {
        for( uint32_t colIdx = 0u; IT_DMA2D_FRAME_WIDTH > colIdx; colIdx++ )
        {
            const uint32_t inside = ( ( 1u <= lineIdx ) && ( 3u >= lineIdx ) && ( 2u <= colIdx ) && ( 6u >= colIdx ) ) ? 1u : 0u;

            TEST_ASSERT_EQUAL_HEX16( ( 0u != inside ) ? 0x1234u : IT_DMA2D_SENTINEL_16, itDma2D_Frame16[ ( lineIdx * IT_DMA2D_FRAME_WIDTH ) + colIdx ] );
        }
    }
}


/**
 * \brief   End of transfer is processed by Dma2D_Task() in POLL mode.
 *
 * \details POLL data handling, whole RGB565 frame filled with 0xBEEF, Dma2D_Task()
 *          called until the transfer ends.
 *
 * \par Expected results
 * - Transfer complete callback once, all pixels 0xBEEF.
 */
void It_Dma2D_Poll_TaskCompletesTransfer( void )
{
    const dma2d_OutputConfig_t output  = { .Address = itDma2D_Frame16, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                           .PixelsPerLine = IT_DMA2D_FRAME_WIDTH, .NumberOfLines = IT_DMA2D_FRAME_LINES };
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0xBEEFu };

    It_Dma2D_Init( DMA2D_XFER_MODE_POLL, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );

    for( uint32_t pixelIdx = 0u; IT_DMA2D_FRAME_PIXELS > pixelIdx; pixelIdx++ )
    {
        TEST_ASSERT_EQUAL_HEX16( 0xBEEFu, itDma2D_Frame16[ pixelIdx ] );
    }
}

/*----------------------------- Memory to memory -----------------------------*/

/**
 * \brief   Memory to memory copy moves a block between frames of different width.
 *
 * \details ARGB8888 block 4 x 3 pixels from source frame 16 pixels wide (column 3,
 *          line 2) to destination frame (column 5, line 4).
 *
 * \par Expected results
 * - Destination block equals the source block, other pixels untouched.
 */
void It_Dma2D_M2M_CopiesBlock( void )
{
    const dma2d_LayerConfig_t  foreground = { .Address = &itDma2D_Source32[ ( 2u * IT_DMA2D_FRAME_WIDTH ) + 3u ], .LineOffset = IT_DMA2D_FRAME_WIDTH - 4u,
                                              .ColorMode = DMA2D_COLOR_MODE_ARGB8888, .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    const dma2d_OutputConfig_t output     = { .Address = &itDma2D_Frame32[ ( 4u * IT_DMA2D_FRAME_WIDTH ) + 5u ], .LineOffset = IT_DMA2D_FRAME_WIDTH - 4u,
                                              .ColorMode = DMA2D_COLOR_MODE_ARGB8888, .PixelsPerLine = 4u, .NumberOfLines = 3u };
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M, .Foreground = &foreground, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );
    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );

    for( uint32_t lineIdx = 0u; IT_DMA2D_FRAME_LINES > lineIdx; lineIdx++ )
    {
        for( uint32_t colIdx = 0u; IT_DMA2D_FRAME_WIDTH > colIdx; colIdx++ )
        {
            const uint32_t inside   = ( ( 4u <= lineIdx ) && ( 6u >= lineIdx ) && ( 5u <= colIdx ) && ( 8u >= colIdx ) ) ? 1u : 0u;
            const uint32_t expected = ( 0u != inside ) ? itDma2D_Source32[ ( ( lineIdx - 2u ) * IT_DMA2D_FRAME_WIDTH ) + ( colIdx - 2u ) ] : IT_DMA2D_SENTINEL_32;

            TEST_ASSERT_EQUAL_HEX32( expected, itDma2D_Frame32[ ( lineIdx * IT_DMA2D_FRAME_WIDTH ) + colIdx ] );
        }
    }
}


/**
 * \brief   Pixel format conversion RGB565 -> ARGB8888 expands the components.
 *
 * \details Source RGB565 pixels red 0xF800, green 0x07E0, blue 0x001F, white 0xFFFF,
 *          black 0x0000 converted to ARGB8888.
 *
 * \par Expected results
 * - 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF, 0xFF000000 (opaque alpha).
 */
void It_Dma2D_M2MPfc_Rgb565ToArgb8888( void )
{
    static uint16_t            source[ 8u ] __attribute__(( aligned( 4u ) )) = { 0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu, 0x0000u };
    const uint32_t             expected[ 5u ] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu, 0xFF000000u };
    const dma2d_LayerConfig_t  foreground = { .Address = source, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                              .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    const dma2d_OutputConfig_t output     = { .Address = itDma2D_Frame32, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                              .PixelsPerLine = 5u, .NumberOfLines = 1u };
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_HEX32_ARRAY( expected, itDma2D_Frame32, 5u );
    TEST_ASSERT_EQUAL_HEX32( IT_DMA2D_SENTINEL_32, itDma2D_Frame32[ 5u ] );
}


/**
 * \brief   Pixel format conversion ARGB8888 -> RGB565 and alpha replacement.
 *
 * \details ARGB8888 red / green / blue pixels converted to RGB565 output; then
 *          ARGB8888 -> ARGB8888 with alpha replaced by 0x40.
 *
 * \par Expected results
 * - RGB565 0xF800, 0x07E0, 0x001F.
 * - ARGB8888 alpha of all pixels 0x40, color components kept.
 */
void It_Dma2D_M2MPfc_Argb8888ToRgb565AndAlphaReplace( void )
{
    static uint32_t       source[ 3u ] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
    dma2d_LayerConfig_t   foreground   = { .Address = source, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                           .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    dma2d_OutputConfig_t  output       = { .Address = itDma2D_Frame16, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                           .PixelsPerLine = 3u, .NumberOfLines = 1u };
    dma2d_XferRequest_t   request      = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );
    TEST_ASSERT_EQUAL_HEX16( 0xF800u, itDma2D_Frame16[ 0u ] );
    TEST_ASSERT_EQUAL_HEX16( 0x07E0u, itDma2D_Frame16[ 1u ] );
    TEST_ASSERT_EQUAL_HEX16( 0x001Fu, itDma2D_Frame16[ 2u ] );

    foreground.AlphaMode = DMA2D_ALPHA_MODE_REPLACE;
    foreground.Alpha     = 0x40u;
    output.Address       = itDma2D_Frame32;
    output.ColorMode     = DMA2D_COLOR_MODE_ARGB8888;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );
    TEST_ASSERT_EQUAL_HEX32( 0x40FF0000u, itDma2D_Frame32[ 0u ] );
    TEST_ASSERT_EQUAL_HEX32( 0x4000FF00u, itDma2D_Frame32[ 1u ] );
    TEST_ASSERT_EQUAL_HEX32( 0x400000FFu, itDma2D_Frame32[ 2u ] );
    TEST_ASSERT_EQUAL_UINT32( 2u, itDma2D_CompleteCnt );
}


/**
 * \brief   Blending of A8 foreground over ARGB8888 background.
 *
 * \details Background opaque blue, foreground A8 with layer color green: alpha 0xFF
 *          (pixel 0) and 0x00 (pixel 1), output ARGB8888.
 *
 * \par Expected results
 * - Pixel 0 = foreground color 0xFF00FF00, pixel 1 = background 0xFF0000FF.
 */
void It_Dma2D_Blend_A8ForegroundOverBackground( void )
{
    static uint8_t             alpha[ 4u ]      = { 0xFFu, 0x00u };
    static uint32_t            background[ 2u ] = { 0xFF0000FFu, 0xFF0000FFu };
    const dma2d_LayerConfig_t  fgLayer          = { .Address = alpha, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_A8,
                                                    .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0x00FF00u };
    const dma2d_LayerConfig_t  bgLayer          = { .Address = background, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                                    .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    const dma2d_OutputConfig_t output           = { .Address = itDma2D_Frame32, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                                    .PixelsPerLine = 2u, .NumberOfLines = 1u };
    const dma2d_XferRequest_t  request          = { .Mode = DMA2D_MODE_M2M_BLEND, .Foreground = &fgLayer, .Background = &bgLayer, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_HEX32( 0xFF00FF00u, itDma2D_Frame32[ 0u ] );
    TEST_ASSERT_EQUAL_HEX32( 0xFF0000FFu, itDma2D_Frame32[ 1u ] );
}

/*-------------------------------- CLUT --------------------------------------*/

/**
 * \brief   CLUT is loaded and used by L8 foreground.
 *
 * \details ARGB8888 CLUT of 4 entries loaded to foreground (CLUT complete callback),
 *          L8 frame with indices 0 - 3 converted to ARGB8888.
 *
 * \par Expected results
 * - CLUT complete callback once, loading INACTIVE.
 * - Every output pixel equals the CLUT entry of its index (alpha kept).
 */
void It_Dma2D_Clut_L8ForegroundUsesLoadedClut( void )
{
    const dma2d_ClutConfig_t   clut       = { .Address = itDma2D_Clut, .ColorMode = DMA2D_CLUT_COLOR_MODE_ARGB8888, .Size = IT_DMA2D_CLUT_CNT };
    const dma2d_LayerConfig_t  foreground = { .Address = itDma2D_Source8, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_L8,
                                              .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    const dma2d_OutputConfig_t output     = { .Address = itDma2D_Frame32, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                              .PixelsPerLine = IT_DMA2D_FRAME_WIDTH, .NumberOfLines = IT_DMA2D_FRAME_LINES };
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M_PFC, .Foreground = &foreground, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_ClutLoad( IT_DMA2D_ID, DMA2D_LAYER_FOREGROUND, &clut ) );
    It_Dma2D_Wait_ClutLoaded();
    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_ClutCnt );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    for( uint32_t pixelIdx = 0u; IT_DMA2D_FRAME_PIXELS > pixelIdx; pixelIdx++ )
    {
        TEST_ASSERT_EQUAL_HEX32( itDma2D_Clut[ itDma2D_Source8[ pixelIdx ] ], itDma2D_Frame32[ pixelIdx ] );
    }
}

/*---------------------------- Watermark / control ---------------------------*/

/**
 * \brief   Line watermark is reported before the end of the transfer.
 *
 * \details Watermark at line 64 of a 128 x 128 RGB565 fill.
 *
 * \par Expected results
 * - Watermark callback once, reported before the complete callback.
 */
void It_Dma2D_Watermark_ReportedBeforeEnd( void )
{
    const dma2d_OutputConfig_t output  = { .Address = itDma2D_Big16, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                           .PixelsPerLine = IT_DMA2D_BIG_WIDTH, .NumberOfLines = IT_DMA2D_BIG_LINES };
    const dma2d_XferRequest_t  request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x5555u };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_LineWatermark( IT_DMA2D_ID, 64u ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_WatermarkCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, itDma2D_CompleteAtWatermark );
    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );
}


/**
 * \brief   Running transfer is suspended and resumed.
 *
 * \details 128 x 128 RGB565 fill slowed down by maximal dead time, suspended right after
 *          the start, checked while suspended, resumed.
 *
 * \par Expected results
 * - While suspended: state SUSPENDED, no complete callback, last pixel not written.
 * - After resume: complete callback once, all pixels filled.
 */
void It_Dma2D_Xfer_SuspendResume_Completes( void )
{
    const dma2d_OutputConfig_t output    = { .Address = itDma2D_Big16, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                             .PixelsPerLine = IT_DMA2D_BIG_WIDTH, .NumberOfLines = IT_DMA2D_BIG_LINES };
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x7E7Eu };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] = IT_DMA2D_SENTINEL_16;

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_ACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferSuspend( IT_DMA2D_ID ) );

    for( volatile uint32_t loopIdx = 0u; IT_DMA2D_SUSPEND_LOOPS > loopIdx; loopIdx++ )
    {
        /* Transfer stays suspended */
    }

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( IT_DMA2D_ID, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_SUSPENDED, xferState );
    TEST_ASSERT_EQUAL_UINT32( 0u, itDma2D_CompleteCnt );
    TEST_ASSERT_EQUAL_HEX16( IT_DMA2D_SENTINEL_16, itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferResume( IT_DMA2D_ID ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );

    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );
    TEST_ASSERT_EQUAL_HEX16( 0x7E7Eu, itDma2D_Big16[ 0u ] );
    TEST_ASSERT_EQUAL_HEX16( 0x7E7Eu, itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] );
}


/**
 * \brief   Running transfer is aborted without callback.
 *
 * \details 128 x 128 RGB565 fill slowed down by maximal dead time, aborted right after
 *          the start; new transfer afterwards.
 *
 * \par Expected results
 * - Dma2D_Set_XferStop() returns DMA2D_REQUEST_OK, state IDLE, no callback, last pixel
 *   not written.
 * - Next transfer completes normally.
 */
void It_Dma2D_Xfer_Abort_NoCallbackNewTransferPossible( void )
{
    const dma2d_OutputConfig_t output    = { .Address = itDma2D_Big16, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_RGB565,
                                             .PixelsPerLine = IT_DMA2D_BIG_WIDTH, .NumberOfLines = IT_DMA2D_BIG_LINES };
    const dma2d_XferRequest_t  request   = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0x3C3Cu };
    dma2d_XferState_t          xferState = DMA2D_XFER_STATE_CNT;

    itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] = IT_DMA2D_SENTINEL_16;

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_ACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStop( IT_DMA2D_ID ) );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( IT_DMA2D_ID, &xferState ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, xferState );
    TEST_ASSERT_EQUAL_UINT32( 0u, itDma2D_CompleteCnt + itDma2D_ErrorCnt );
    TEST_ASSERT_EQUAL_HEX16( IT_DMA2D_SENTINEL_16, itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_DeadTime( IT_DMA2D_ID, DMA2D_FUNCTION_INACTIVE, 0u ) );
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );
    TEST_ASSERT_EQUAL( DMA2D_XFER_STATE_IDLE, It_Dma2D_Wait_Idle() );
    TEST_ASSERT_EQUAL_UINT32( 1u, itDma2D_CompleteCnt );
    TEST_ASSERT_EQUAL_HEX16( 0x3C3Cu, itDma2D_Big16[ IT_DMA2D_BIG_PIXELS - 1u ] );
}


/**
 * \brief   Invalid requests are refused before the start.
 *
 * \details Misaligned ARGB8888 output, M2M with different color modes.
 *
 * \par Expected results
 * - DMA2D_REQUEST_ERROR, no callback, frame untouched.
 */
void It_Dma2D_Xfer_InvalidRequest_Refused( void )
{
    const dma2d_LayerConfig_t  foreground = { .Address = itDma2D_Source32, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                              .AlphaMode = DMA2D_ALPHA_MODE_NO_MODIF, .Alpha = 0xFFu, .Color = 0u };
    dma2d_OutputConfig_t       output     = { .Address = (uint8_t *)itDma2D_Frame32 + 2u, .LineOffset = 0u, .ColorMode = DMA2D_COLOR_MODE_ARGB8888,
                                              .PixelsPerLine = 4u, .NumberOfLines = 1u };
    const dma2d_XferRequest_t  request    = { .Mode = DMA2D_MODE_M2M, .Foreground = &foreground, .Output = &output };

    It_Dma2D_Init( DMA2D_XFER_MODE_ISR, DMA2D_FUNCTION_INACTIVE );

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );

    output.Address   = itDma2D_Frame32;
    output.ColorMode = DMA2D_COLOR_MODE_RGB888;
    TEST_ASSERT_EQUAL( DMA2D_REQUEST_ERROR, Dma2D_Set_XferStart( IT_DMA2D_ID, &request ) );

    TEST_ASSERT_EQUAL_UINT32( 0u, itDma2D_CompleteCnt + itDma2D_ErrorCnt );
    TEST_ASSERT_EQUAL_HEX32( IT_DMA2D_SENTINEL_32, itDma2D_Frame32[ 0u ] );
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Initializes DMA2D with test callbacks.
 *
 * \param xferMode      [in]: Data handling mode
 * \param deadTimeState [in]: Maximal dead time used (slows down the transfer)
 */
static void It_Dma2D_Init( dma2d_XferMode_t xferMode, dma2d_FunctionState_t deadTimeState )
{
    dma2d_Config_t config;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_DefaultConfig( &config ) );

    config.PeriphId             = IT_DMA2D_ID;
    config.XferMode             = xferMode;
    config.IrqPriority          = IT_DMA2D_PRIO;
    config.DeadTimeState        = deadTimeState;
    config.DeadTime             = IT_DMA2D_DEAD_TIME_MAX;
    config.XferCompleteCallback = It_Dma2D_XferCompleteCallback;
    config.ClutCompleteCallback = It_Dma2D_ClutCompleteCallback;
    config.WatermarkCallback    = It_Dma2D_WatermarkCallback;
    config.ErrorCallback        = It_Dma2D_ErrorCallback;

    TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Init( &config ) );
}


/**
 * \brief Waits for the end of the transfer (calls Dma2D_Task() for POLL mode).
 *
 * \return Transfer state at the end of waiting
 */
static dma2d_XferState_t It_Dma2D_Wait_Idle( void )
{
    dma2d_XferState_t xferState = DMA2D_XFER_STATE_CNT;

    for( uint32_t loopIdx = 0u; IT_DMA2D_WAIT_LOOPS > loopIdx; loopIdx++ )
    {
        Dma2D_Task();

        TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_XferState( IT_DMA2D_ID, &xferState ) );

        if( DMA2D_XFER_STATE_IDLE == xferState )
        {
            break;
        }
        else
        {
            /* Transfer runs */
        }
    }

    return ( xferState );
}


/**
 * \brief Waits for the end of CLUT loading.
 */
static void It_Dma2D_Wait_ClutLoaded( void )
{
    dma2d_FunctionState_t loadState = DMA2D_FUNCTION_ACTIVE;

    for( uint32_t loopIdx = 0u; ( IT_DMA2D_WAIT_LOOPS > loopIdx ) && ( DMA2D_FUNCTION_ACTIVE == loadState ); loopIdx++ )
    {
        Dma2D_Task();

        TEST_ASSERT_EQUAL( DMA2D_REQUEST_OK, Dma2D_Get_ClutLoadState( IT_DMA2D_ID, &loadState ) );
    }

    TEST_ASSERT_EQUAL( DMA2D_FUNCTION_INACTIVE, loadState );
}


/** \brief Transfer complete callback */
static void It_Dma2D_XferCompleteCallback( void )
{
    itDma2D_CompleteCnt++;
}


/** \brief CLUT loading complete callback */
static void It_Dma2D_ClutCompleteCallback( void )
{
    itDma2D_ClutCnt++;
}


/** \brief Line watermark callback - stores count of completed transfers at the watermark */
static void It_Dma2D_WatermarkCallback( void )
{
    itDma2D_CompleteAtWatermark = itDma2D_CompleteCnt;
    itDma2D_WatermarkCnt++;
}


/**
 * \brief Transfer error callback.
 *
 * \param errorId [in]: Error identification
 */
static void It_Dma2D_ErrorCallback( dma2d_XferErrorId_t errorId )
{
    itDma2D_ErrorId = errorId;
    itDma2D_ErrorCnt++;
}
