/**
 * \author Mr.Nobody
 * \file Dma2D.c
 * \ingroup Dma2D
 * \brief Dma2D module common functionality
 *
 * STM32H7 Chrom-ART accelerator (DMA2D, every STM32H7 device, D1 / CD domain AXI master)
 * transfers rectangular graphical areas (number of lines x pixels per line with
 * line offset) without CPU:
 * - Register to memory - output area is filled with one color (\ref DMA2D_MODE_R2M).
 * - Memory to memory - foreground is copied without conversion (\ref DMA2D_MODE_M2M,
 *   foreground color mode shall equal the output color mode).
 * - Memory to memory with pixel format conversion of the foreground
 *   (\ref DMA2D_MODE_M2M_PFC) and with blending of foreground and background
 *   (\ref DMA2D_MODE_M2M_BLEND).
 *
 * Input layers support all color modes (direct ARGB8888 - ARGB4444, CLUT indexed
 * L8 / AL44 / AL88 / L4 and alpha only A8 / A4 with the color of the layer), the
 * output supports direct color modes ARGB8888 - ARGB4444. CLUT of the indexed
 * modes is loaded by \ref Dma2D_Set_ClutLoad before the transfer. STM32H7 DMA2D can
 * invert the alpha and swap red / blue of the layers and of the output.
 *
 * STM32H7 DMA2D features not covered by the interface (registers kept at reset
 * value): YCbCr input color mode with chroma sub-sampling, blending with fixed
 * foreground / background color, line offset in bytes (LOM) and byte swap of the
 * output (SB).
 *
 * Transfer is started by \ref Dma2D_Set_XferStart and runs in background. End of
 * transfer, end of CLUT loading, line watermark and errors are processed by the
 * DMA2D interrupt (\ref DMA2D_XFER_MODE_ISR) or by \ref Dma2D_Task polling the
 * flags (\ref DMA2D_XFER_MODE_POLL) and reported by the callbacks of the
 * configuration. Running transfer can be suspended, resumed and aborted.
 *
 * Module body is compiled only for MCUs with DMA2D (CMSIS device header defines
 * DMA2D), other MCUs get an empty library.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Dma2D.h"                          /* Self include                   */
#include "Dma2D_Port.h"                     /* Own port file include          */
#include "Dma2D_Types.h"                    /* Module types definitions       */
#include "Rcc_Port.h"                       /* RCC handler functionality      */
#include "Nvic_Port.h"                      /* NVIC handler functionality     */
#include "Stm32_dma2d.h"                    /* DMA2D peripheral functionality */

#if defined(DMA2D)
/* ============================== TYPEDEFS ================================== */

/** \brief Registers of one input layer (foreground / background) */
typedef struct
{
    volatile uint32_t *MemAddr;     /**< Memory address register (FGMAR / BGMAR)      */
    volatile uint32_t *Offset;      /**< Line offset register (FGOR / BGOR)           */
    volatile uint32_t *PfcCtrl;     /**< PFC control register (FGPFCCR / BGPFCCR)     */
    volatile uint32_t *Color;       /**< Color register (FGCOLR / BGCOLR)             */
    volatile uint32_t *ClutAddr;    /**< CLUT memory address register (FGCMAR / BGCMAR) */
}   dma2d_LayerReg_t;

/* =============================== MACROS =================================== */

/* ======================== FORWARD DECLARATIONS ============================ */

static dma2d_RequestState_t     Dma2D_Check_Config      ( const dma2d_Config_t * const dma2dConfig );
static dma2d_RequestState_t     Dma2D_Check_Aligned     ( const void * const address, uint32_t pixelBits );
static dma2d_RequestState_t     Dma2D_Check_PfcOptions  ( dma2d_FunctionState_t alphaInvert, dma2d_FunctionState_t redBlueSwap );
static uint32_t                 Dma2D_Get_PfcOptionBits ( dma2d_FunctionState_t alphaInvert, dma2d_FunctionState_t redBlueSwap );
static dma2d_RequestState_t     Dma2D_Check_Layer       ( const dma2d_LayerConfig_t * const layerConfig );
static dma2d_RequestState_t     Dma2D_Check_Output      ( const dma2d_OutputConfig_t * const outputConfig );
static dma2d_RequestState_t     Dma2D_Check_XferRequest ( const dma2d_XferRequest_t * const xferRequest );
static dma2d_RequestState_t     Dma2D_Check_Idle        ( dma2d_PeriphId_t periphId );
static dma2d_LayerReg_t         Dma2D_Get_LayerReg      ( dma2d_PeriphId_t periphId, dma2d_Layer_t layer );
static void                     Dma2D_Set_Layer         ( dma2d_PeriphId_t periphId, dma2d_Layer_t layer, const dma2d_LayerConfig_t * const layerConfig );
static dma2d_RequestState_t     Dma2D_Set_Abort         ( dma2d_PeriphId_t periphId );
static dma2d_RequestState_t     Dma2D_Set_IrqInit       ( dma2d_PeriphId_t periphId );
static void                     Dma2D_Set_XferEnd       ( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t errorId );
static void                     Dma2D_Set_ClutEnd       ( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t errorId );
static void                     Dma2D_Process_Events    ( dma2d_PeriphId_t periphId );

static void                     Dma2D_Dma2D1_IsrHandler ( void );

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Value of major version of SW module */
#define DMA2D_MAJOR_VERSION             ( 1u )

/** Value of minor version of SW module */
#define DMA2D_MINOR_VERSION             ( 0u )

/** Value of patch version of SW module */
#define DMA2D_PATCH_VERSION             ( 0u )

/** Interrupt sources of a transfer (complete, transfer error, configuration error) */
#define DMA2D_CR_XFER_IT                ( DMA2D_CR_TCIE | DMA2D_CR_TEIE | DMA2D_CR_CEIE )

/** Interrupt sources of CLUT loading (complete, CLUT access error, configuration error) */
#define DMA2D_CR_CLUT_IT                ( DMA2D_CR_CTCIE | DMA2D_CR_CAEIE | DMA2D_CR_CEIE )

/** All interrupt sources used by the module */
#define DMA2D_CR_ALL_IT                 ( DMA2D_CR_XFER_IT | DMA2D_CR_CLUT_IT | DMA2D_CR_TWIE )

/** All interrupt flag clear bits */
#define DMA2D_IFCR_ALL                  ( DMA2D_IFCR_CTEIF  | DMA2D_IFCR_CTCIF  | DMA2D_IFCR_CTWIF | \
                                          DMA2D_IFCR_CAECIF | DMA2D_IFCR_CCTCIF | DMA2D_IFCR_CCEIF )

/** Error flags of the status register */
#define DMA2D_ISR_ERRORS                ( DMA2D_ISR_CEIF | DMA2D_ISR_TEIF | DMA2D_ISR_CAEIF )

#if defined(DMA2D_FGPFCCR_AI)
/** Fields of PFC control register written by a transfer (CLUT fields CCM / CS are kept) */
#define DMA2D_PFCCR_XFER_MASK           ( DMA2D_FGPFCCR_CM | DMA2D_FGPFCCR_AM | DMA2D_FGPFCCR_ALPHA | \
                                          DMA2D_FGPFCCR_AI | DMA2D_FGPFCCR_RBS )
#else
/** Fields of PFC control register written by a transfer (CLUT fields CCM / CS are kept) */
#define DMA2D_PFCCR_XFER_MASK           ( DMA2D_FGPFCCR_CM | DMA2D_FGPFCCR_AM | DMA2D_FGPFCCR_ALPHA )
#endif /* DMA2D_FGPFCCR_AI */

/** Fields of PFC control register written by CLUT loading */
#define DMA2D_PFCCR_CLUT_MASK           ( DMA2D_FGPFCCR_CCM | DMA2D_FGPFCCR_CS )

/** Mask of RGB888 color of a layer (FGCOLR / BGCOLR) */
#define DMA2D_LAYER_COLOR_MASK          ( DMA2D_FGCOLR_RED | DMA2D_FGCOLR_GREEN | DMA2D_FGCOLR_BLUE )

/** Count of color modes supported by the output (ARGB8888 - ARGB4444) */
#define DMA2D_OUTPUT_COLOR_MODE_CNT     ( (uint32_t)DMA2D_COLOR_MODE_ARGB4444 + 1u )

/** Size of 16-bit and 32-bit pixel in bits (alignment of the addresses) */
#define DMA2D_PIXEL_BITS_16             ( 16u )
#define DMA2D_PIXEL_BITS_32             ( 32u )

/** Alignment of 16-bit and 32-bit pixel addresses in bytes */
#define DMA2D_ALIGN_16                  ( 2u )
#define DMA2D_ALIGN_32                  ( 4u )

/** Maximal dead time (8-bit AMTCR.DT field) */
#define DMA2D_DEAD_TIME_MAX             ( 0xFFu )

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

/** \brief Configuration of DMA2D peripherals */
static const dma2d_PeriphConfig_t dma2d_PeriphConf[ ] =
{
    { .PeriphReg = DMA2D, .PeriphRcc = RCC_PERIPH_DMA2D, .PeriphNvic = NVIC_PERIPH_IRQ_DMA2D, .PeriphIsr = Dma2D_Dma2D1_IsrHandler },
};

_Static_assert( DMA2D_PERIPH_CNT == ( sizeof(dma2d_PeriphConf) / sizeof(dma2d_PeriphConfig_t) ), "Dma2D: dma2d_PeriphConf has incorrect size." );


/** \brief dma2d_Mode_t -> CR.MODE field value */
static const uint32_t dma2d_ModeLut[ DMA2D_MODE_CNT ] =
{
    [DMA2D_MODE_M2M]       = LL_DMA2D_MODE_M2M,
    [DMA2D_MODE_M2M_PFC]   = LL_DMA2D_MODE_M2M_PFC,
    [DMA2D_MODE_M2M_BLEND] = LL_DMA2D_MODE_M2M_BLEND,
    [DMA2D_MODE_R2M]       = LL_DMA2D_MODE_R2M,
};


/** \brief dma2d_ColorMode_t -> count of bits per pixel */
static const uint32_t dma2d_PixelBitsLut[ DMA2D_COLOR_MODE_CNT ] =
{
    [DMA2D_COLOR_MODE_ARGB8888] = 32u,
    [DMA2D_COLOR_MODE_RGB888]   = 24u,
    [DMA2D_COLOR_MODE_RGB565]   = 16u,
    [DMA2D_COLOR_MODE_ARGB1555] = 16u,
    [DMA2D_COLOR_MODE_ARGB4444] = 16u,
    [DMA2D_COLOR_MODE_L8]       = 8u,
    [DMA2D_COLOR_MODE_AL44]     = 8u,
    [DMA2D_COLOR_MODE_AL88]     = 16u,
    [DMA2D_COLOR_MODE_L4]       = 4u,
    [DMA2D_COLOR_MODE_A8]       = 8u,
    [DMA2D_COLOR_MODE_A4]       = 4u,
};


/** \brief Runtime context of DMA2D peripherals */
static dma2d_PeriphContext_t dma2d_PeriphContext[ DMA2D_PERIPH_CNT ];

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Returns actual version of SW module
 *
 * \return Structure with major, minor and patch version, see \ref dma2d_ModuleVersion_t
 */
dma2d_ModuleVersion_t Dma2D_Get_ModuleVersion( void )
{
    dma2d_ModuleVersion_t retVersion;

    retVersion.Major = DMA2D_MAJOR_VERSION;
    retVersion.Minor = DMA2D_MINOR_VERSION;
    retVersion.Patch = DMA2D_PATCH_VERSION;

    return ( retVersion );
}


/**
 * \brief Initializes DMA2D peripheral.
 *
 * Running transfer / CLUT loading of a previous initialization is aborted, the
 * peripheral clock is enabled and the peripheral is reset, the dead time (AHB master
 * timer) is configured and the DMA2D interrupt is registered and enabled in NVIC
 * (\ref DMA2D_XFER_MODE_ISR) or disabled in NVIC (\ref DMA2D_XFER_MODE_POLL).
 *
 * \pre   Configuration shall be valid; otherwise \ref DMA2D_REQUEST_ERROR is returned
 *        and nothing is changed.
 *
 * \param dma2dConfig [in]: Pointer to DMA2D configuration, see \ref dma2d_Config_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Init( const dma2d_Config_t * const dma2dConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    retState = Dma2D_Check_Config( dma2dConfig );

    if( DMA2D_REQUEST_OK == retState )
    {
        const dma2d_PeriphId_t          periphId = dma2dConfig->PeriphId;
        const rcc_PeriphId_t            rccId    = dma2d_PeriphConf[ periphId ].PeriphRcc;
        dma2d_PeriphContext_t * const   context  = &dma2d_PeriphContext[ periphId ];
        rcc_RequestState_t              rccState = RCC_REQUEST_ERROR;

        /*-------------------- Transfer of previous initialization -----------*/
        if( DMA2D_FUNCTION_ACTIVE == context->InitState )
        {
            retState = Dma2D_Set_Abort( periphId );
        }
        else
        {
            /* Peripheral was not initialized */
        }

        context->InitState = DMA2D_FUNCTION_INACTIVE;

        /*------------------------ Peripheral clock and reset -----------------*/
        if( DMA2D_REQUEST_OK == retState )
        {
            rccState = Rcc_Set_PeriphActive( rccId );

            if( RCC_REQUEST_OK == rccState )
            {
                rccState = Rcc_Set_ResetActive( rccId );
            }
            else
            {
                /* Peripheral clock activation failed */
            }

            if( RCC_REQUEST_OK == rccState )
            {
                rccState = Rcc_Set_ResetInactive( rccId );
            }
            else
            {
                /* Peripheral reset failed */
            }

            retState = ( RCC_REQUEST_OK == rccState ) ? DMA2D_REQUEST_OK : DMA2D_REQUEST_ERROR;
        }
        else
        {
            /* Running transfer of previous initialization could not be aborted */
        }

        /*----------------------------- Configuration -------------------------*/
        if( DMA2D_REQUEST_OK == retState )
        {
            context->Config         = *dma2dConfig;
            context->WatermarkState = DMA2D_FUNCTION_INACTIVE;
            context->XferState      = DMA2D_XFER_STATE_IDLE;
            context->ClutState      = DMA2D_FUNCTION_INACTIVE;
            context->XferError      = DMA2D_XFER_ERROR_NONE;

            retState = Dma2D_Set_DeadTime( periphId, dma2dConfig->DeadTimeState, dma2dConfig->DeadTime );
        }
        else
        {
            /* Peripheral clock is not available */
        }

        if( DMA2D_REQUEST_OK == retState )
        {
            retState = Dma2D_Set_IrqInit( periphId );
        }
        else
        {
            /* Dead time could not be configured */
        }

        if( DMA2D_REQUEST_OK == retState )
        {
            context->InitState = DMA2D_FUNCTION_ACTIVE;
        }
        else
        {
            /* Initialization failed */
        }
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Deinitializes DMA2D peripheral.
 *
 * Running transfer / CLUT loading is aborted (no callback), DMA2D interrupt sources are
 * disabled, the interrupt is disabled in NVIC, the peripheral is reset and its clock is
 * disabled. All steps are executed, any failure is reported.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Deinit( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( DMA2D_PERIPH_CNT > periphId )
    {
        DMA2D_TypeDef * const          periphReg  = dma2d_PeriphConf[ periphId ].PeriphReg;
        const rcc_PeriphId_t           rccId      = dma2d_PeriphConf[ periphId ].PeriphRcc;
        dma2d_PeriphContext_t * const  context    = &dma2d_PeriphContext[ periphId ];
        dma2d_RequestState_t           abortState = DMA2D_REQUEST_OK;

        /* -1- Running transfer / CLUT loading */
        if( DMA2D_FUNCTION_ACTIVE == context->InitState )
        {
            abortState = Dma2D_Set_Abort( periphId );
        }
        else
        {
            /* Peripheral was not initialized */
        }

        context->InitState = DMA2D_FUNCTION_INACTIVE;

        /* -2- Interrupt sources and NVIC */
        CLEAR_BIT( periphReg->CR, DMA2D_CR_ALL_IT );

        const nvic_RequestState_t nvicState = Nvic_Set_PeriphIrq_Inactive( dma2d_PeriphConf[ periphId ].PeriphNvic );

        /* -3- Peripheral reset and clock */
        const rcc_RequestState_t rstActState   = Rcc_Set_ResetActive( rccId );
        const rcc_RequestState_t rstInactState = Rcc_Set_ResetInactive( rccId );
        const rcc_RequestState_t clkState      = Rcc_Set_PeriphInactive( rccId );

        if( ( DMA2D_REQUEST_OK == abortState    ) &&
            ( NVIC_REQUEST_OK  == nvicState     ) &&
            ( RCC_REQUEST_OK   == rstActState   ) &&
            ( RCC_REQUEST_OK   == rstInactState ) &&
            ( RCC_REQUEST_OK   == clkState      )    )
        {
            retState = DMA2D_REQUEST_OK;
        }
        else
        {
            retState = DMA2D_REQUEST_ERROR;
        }
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Main task of module Dma2D
 *
 * This function shall be called in the main loop of the application or the task scheduler.
 * It processes end of transfer, end of CLUT loading, line watermark and errors of the
 * peripherals initialized with \ref DMA2D_XFER_MODE_POLL.
 */
void Dma2D_Task( void )
{
    for( dma2d_PeriphId_t periphId = (dma2d_PeriphId_t)0u; DMA2D_PERIPH_CNT > periphId; periphId ++ )
    {
        const dma2d_PeriphContext_t * const context = &dma2d_PeriphContext[ periphId ];

        if( ( DMA2D_FUNCTION_ACTIVE == context->InitState       ) &&
            ( DMA2D_XFER_MODE_POLL  == context->Config.XferMode )    )
        {
            Dma2D_Process_Events( periphId );
        }
        else
        {
            /* Not initialized or events processed by ISR */
        }
    }
}


/**
 * \brief Fills configuration structure with default values.
 *
 * DMA2D peripheral 1, ISR data handling, interrupt priority 15 (lowest), no dead time,
 * no callbacks.
 *
 * \param dma2dConfig [out]: Pointer to configuration structure. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_DefaultConfig( dma2d_Config_t * const dma2dConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( DMA2D_NULL_PTR != dma2dConfig )
    {
        dma2dConfig->PeriphId             = (dma2d_PeriphId_t)0u;
        dma2dConfig->XferMode             = DMA2D_XFER_MODE_ISR;
        dma2dConfig->IrqPriority          = 15u;
        dma2dConfig->DeadTimeState        = DMA2D_FUNCTION_INACTIVE;
        dma2dConfig->DeadTime             = 0u;
        dma2dConfig->XferCompleteCallback = DMA2D_NULL_PTR;
        dma2dConfig->ClutCompleteCallback = DMA2D_NULL_PTR;
        dma2dConfig->WatermarkCallback    = DMA2D_NULL_PTR;
        dma2dConfig->ErrorCallback        = DMA2D_NULL_PTR;

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Starts a transfer.
 *
 * Registers of the mode are programmed (foreground for memory to memory modes,
 * background for blending, output, fill color for register to memory), all flags are
 * cleared, interrupt sources of the transfer are enabled (ISR mode, line watermark
 * interrupt if configured by \ref Dma2D_Set_LineWatermark) and the transfer is started.
 * End of the transfer is reported by XferCompleteCallback / ErrorCallback.
 *
 * \pre   The peripheral shall be initialized, no transfer / CLUT loading shall run and the
 *        request shall be valid:
 *        - output: address aligned to the pixel size (16 / 32-bit modes), color mode
 *          ARGB8888 - ARGB4444, pixels per line and lines not zero and in range, line
 *          offset in range,
 *        - foreground (all modes except R2M) and background (blending): address aligned
 *          to the pixel size, valid color and alpha mode, line offset in range,
 *        - M2M mode: foreground color mode equal to the output color mode.
 *        Otherwise \ref DMA2D_REQUEST_ERROR is returned and nothing is started.
 *
 * \param periphId    [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param xferRequest [in]: Transfer request, see \ref dma2d_XferRequest_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_XferStart( dma2d_PeriphId_t periphId, const dma2d_XferRequest_t * const xferRequest )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    retState = Dma2D_Check_Idle( periphId );

    if( DMA2D_REQUEST_OK == retState )
    {
        retState = Dma2D_Check_XferRequest( xferRequest );
    }
    else
    {
        /* Not initialized or busy */
    }

    if( DMA2D_REQUEST_OK == retState )
    {
        DMA2D_TypeDef * const               periphReg = dma2d_PeriphConf[ periphId ].PeriphReg;
        dma2d_PeriphContext_t * const       context   = &dma2d_PeriphContext[ periphId ];
        const dma2d_OutputConfig_t * const  output    = xferRequest->Output;
        uint32_t                            itMask    = 0u;

        /* -1- Input layers */
        if( DMA2D_MODE_R2M != xferRequest->Mode )
        {
            Dma2D_Set_Layer( periphId, DMA2D_LAYER_FOREGROUND, xferRequest->Foreground );
        }
        else
        {
            /* Register to memory - output color */
            WRITE_REG( periphReg->OCOLR, xferRequest->FillColor );
        }

        if( DMA2D_MODE_M2M_BLEND == xferRequest->Mode )
        {
            Dma2D_Set_Layer( periphId, DMA2D_LAYER_BACKGROUND, xferRequest->Background );
        }
        else
        {
            /* Background is not used */
        }

        /* -2- Output */
        WRITE_REG( periphReg->OPFCCR, (uint32_t)output->ColorMode |
                                      Dma2D_Get_PfcOptionBits( output->AlphaInvert, output->RedBlueSwap ) );
        WRITE_REG( periphReg->OMAR, (uint32_t)(uintptr_t)output->Address );
        WRITE_REG( periphReg->OOR, (uint32_t)output->LineOffset );
        WRITE_REG( periphReg->NLR, ( (uint32_t)output->PixelsPerLine << DMA2D_NLR_PL_Pos ) | (uint32_t)output->NumberOfLines );

        /* -3- Flags, interrupt sources and start */
        WRITE_REG( periphReg->IFCR, DMA2D_IFCR_ALL );

        if( DMA2D_XFER_MODE_ISR == context->Config.XferMode )
        {
            itMask = DMA2D_CR_XFER_IT;

            if( DMA2D_FUNCTION_ACTIVE == context->WatermarkState )
            {
                itMask |= DMA2D_CR_TWIE;
            }
            else
            {
                /* Line watermark is not used */
            }
        }
        else
        {
            /* Flags are polled by Dma2D_Task() */
        }

        MODIFY_REG( periphReg->CR, ( DMA2D_CR_MODE | DMA2D_CR_ALL_IT | DMA2D_CR_SUSP ), ( dma2d_ModeLut[ xferRequest->Mode ] | itMask ) );

        context->XferError = DMA2D_XFER_ERROR_NONE;
        context->XferState = DMA2D_XFER_STATE_RUNNING;

        SET_BIT( periphReg->CR, DMA2D_CR_START );
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Aborts running transfer and CLUT loading (no callback is called).
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems (also if nothing was running). Otherwise (abort
 *         not finished by HW) returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_XferStop( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT      > periphId                                  ) &&
        ( DMA2D_FUNCTION_ACTIVE == dma2d_PeriphContext[ periphId ].InitState )    )
    {
        retState = Dma2D_Set_Abort( periphId );
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Suspends running transfer.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise (no running transfer) returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_XferSuspend( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT        > periphId                                  ) &&
        ( DMA2D_XFER_STATE_RUNNING == dma2d_PeriphContext[ periphId ].XferState )    )
    {
        SET_BIT( dma2d_PeriphConf[ periphId ].PeriphReg->CR, DMA2D_CR_SUSP );

        dma2d_PeriphContext[ periphId ].XferState = DMA2D_XFER_STATE_SUSPENDED;

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Resumes suspended transfer.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise (no suspended transfer) returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_XferResume( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT          > periphId                                  ) &&
        ( DMA2D_XFER_STATE_SUSPENDED == dma2d_PeriphContext[ periphId ].XferState )    )
    {
        dma2d_PeriphContext[ periphId ].XferState = DMA2D_XFER_STATE_RUNNING;

        CLEAR_BIT( dma2d_PeriphConf[ periphId ].PeriphReg->CR, DMA2D_CR_SUSP );

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns state of the transfer.
 *
 * \param periphId   [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param xferState [out]: Pointer to store the state (\ref dma2d_XferState_t). Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_XferState( dma2d_PeriphId_t periphId, dma2d_XferState_t * const xferState )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId  ) &&
        ( DMA2D_NULL_PTR  != xferState )    )
    {
        *xferState = dma2d_PeriphContext[ periphId ].XferState;
        retState   = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns result of the last transfer / CLUT loading.
 *
 * \param periphId   [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param xferError [out]: Pointer to store the result (\ref dma2d_XferErrorId_t). Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_XferError( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t * const xferError )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId  ) &&
        ( DMA2D_NULL_PTR  != xferError )    )
    {
        *xferError = dma2d_PeriphContext[ periphId ].XferError;
        retState   = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Starts automatic loading of the CLUT of an input layer.
 *
 * CLUT address, color mode and size are written to the layer registers, flags of CLUT
 * loading are cleared, its interrupt sources are enabled (ISR mode) and the loading is
 * started. End of the loading is reported by ClutCompleteCallback / ErrorCallback. The
 * loaded CLUT is used by following transfers with L8 / AL44 / AL88 / L4 layer.
 *
 * \pre   The peripheral shall be initialized, no transfer / CLUT loading shall run, CLUT
 *        address shall not be NULL (32-bit aligned for ARGB8888 entries) and the size shall
 *        be 1 - \ref DMA2D_CLUT_SIZE_MAX entries. Otherwise \ref DMA2D_REQUEST_ERROR is
 *        returned and nothing is started.
 *
 * \param periphId   [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param layer      [in]: Input layer, value from \ref dma2d_Layer_t
 * \param clutConfig [in]: CLUT configuration, see \ref dma2d_ClutConfig_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_ClutLoad( dma2d_PeriphId_t periphId, dma2d_Layer_t layer, const dma2d_ClutConfig_t * const clutConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    retState = Dma2D_Check_Idle( periphId );

    if( ( DMA2D_REQUEST_OK        == retState                ) &&
        ( DMA2D_LAYER_CNT          > layer                   ) &&
        ( DMA2D_NULL_PTR          != clutConfig              ) &&
        ( DMA2D_CLUT_COLOR_MODE_CNT > clutConfig->ColorMode  ) &&
        ( 0u                       < clutConfig->Size        ) &&
        ( DMA2D_CLUT_SIZE_MAX     >= clutConfig->Size        ) &&
        ( DMA2D_NULL_PTR          != clutConfig->Address     )    )
    {
        const uint32_t entryBits = ( DMA2D_CLUT_COLOR_MODE_ARGB8888 == clutConfig->ColorMode ) ? DMA2D_PIXEL_BITS_32 : 24u;

        retState = Dma2D_Check_Aligned( clutConfig->Address, entryBits );
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    if( DMA2D_REQUEST_OK == retState )
    {
        DMA2D_TypeDef * const           periphReg = dma2d_PeriphConf[ periphId ].PeriphReg;
        dma2d_PeriphContext_t * const   context   = &dma2d_PeriphContext[ periphId ];
        const dma2d_LayerReg_t          layerReg  = Dma2D_Get_LayerReg( periphId, layer );
        const uint32_t                  clutValue = ( (uint32_t)clutConfig->ColorMode << DMA2D_FGPFCCR_CCM_Pos ) |
                                                    ( ( (uint32_t)clutConfig->Size - 1u ) << DMA2D_FGPFCCR_CS_Pos );

        WRITE_REG( *layerReg.ClutAddr, (uint32_t)(uintptr_t)clutConfig->Address );
        MODIFY_REG( *layerReg.PfcCtrl, DMA2D_PFCCR_CLUT_MASK, clutValue );

        WRITE_REG( periphReg->IFCR, ( DMA2D_IFCR_CCTCIF | DMA2D_IFCR_CAECIF | DMA2D_IFCR_CCEIF ) );

        if( DMA2D_XFER_MODE_ISR == context->Config.XferMode )
        {
            MODIFY_REG( periphReg->CR, DMA2D_CR_ALL_IT, DMA2D_CR_CLUT_IT );
        }
        else
        {
            /* Flags are polled by Dma2D_Task() */
        }

        context->XferError = DMA2D_XFER_ERROR_NONE;
        context->ClutState = DMA2D_FUNCTION_ACTIVE;

        SET_BIT( *layerReg.PfcCtrl, DMA2D_FGPFCCR_START );
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns state of CLUT loading.
 *
 * \param periphId   [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param loadState [out]: Pointer to store the state, \ref DMA2D_FUNCTION_ACTIVE while the
 *                         loading runs. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_ClutLoadState( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const loadState )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId  ) &&
        ( DMA2D_NULL_PTR  != loadState )    )
    {
        *loadState = dma2d_PeriphContext[ periphId ].ClutState;
        retState   = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Configures line watermark - WatermarkCallback is called when the transfer
 *        reaches the line (enabled for following transfers).
 *
 * \pre   The peripheral shall be initialized and no transfer / CLUT loading shall run.
 *
 * \param periphId   [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param lineNumber [in]: Line number of the watermark
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_LineWatermark( dma2d_PeriphId_t periphId, dma2d_LineCnt_t lineNumber )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    retState = Dma2D_Check_Idle( periphId );

    if( DMA2D_REQUEST_OK == retState )
    {
        WRITE_REG( dma2d_PeriphConf[ periphId ].PeriphReg->LWR, (uint32_t)lineNumber );

        dma2d_PeriphContext[ periphId ].WatermarkState = DMA2D_FUNCTION_ACTIVE;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Disables line watermark of following transfers.
 *
 * \pre   The peripheral shall be initialized and no transfer / CLUT loading shall run.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_LineWatermarkInactive( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    retState = Dma2D_Check_Idle( periphId );

    if( DMA2D_REQUEST_OK == retState )
    {
        dma2d_PeriphContext[ periphId ].WatermarkState = DMA2D_FUNCTION_INACTIVE;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns line watermark configuration.
 *
 * \param periphId        [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param watermarkState [out]: Pointer to store the state of the watermark. Must not be NULL.
 * \param lineNumber     [out]: Pointer to store the line number (LWR). Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_LineWatermark( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const watermarkState, dma2d_LineCnt_t * const lineNumber )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId       ) &&
        ( DMA2D_NULL_PTR  != watermarkState ) &&
        ( DMA2D_NULL_PTR  != lineNumber     )    )
    {
        *watermarkState = dma2d_PeriphContext[ periphId ].WatermarkState;
        *lineNumber     = (dma2d_LineCnt_t)READ_BIT( dma2d_PeriphConf[ periphId ].PeriphReg->LWR, DMA2D_LWR_LW );

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Configures dead time between two consecutive AHB accesses of DMA2D (AHB master
 *        timer) - limits the AHB bandwidth used by DMA2D.
 *
 * \param periphId      [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param deadTimeState [in]: Dead time is used
 * \param deadTime      [in]: Dead time in AHB clock cycles
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_DeadTime( dma2d_PeriphId_t periphId, dma2d_FunctionState_t deadTimeState, dma2d_DeadTime_t deadTime )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT      > periphId      ) &&
        ( DMA2D_FUNCTION_ACTIVE >= deadTimeState )    )
    {
        const uint32_t enValue = ( DMA2D_FUNCTION_ACTIVE == deadTimeState ) ? DMA2D_AMTCR_EN : 0u;

        WRITE_REG( dma2d_PeriphConf[ periphId ].PeriphReg->AMTCR, ( (uint32_t)deadTime << DMA2D_AMTCR_DT_Pos ) | enValue );

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns dead time configuration.
 *
 * \param periphId       [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param deadTimeState [out]: Pointer to store the state of the dead time. Must not be NULL.
 * \param deadTime      [out]: Pointer to store the dead time. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_DeadTime( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const deadTimeState, dma2d_DeadTime_t * const deadTime )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId      ) &&
        ( DMA2D_NULL_PTR  != deadTimeState ) &&
        ( DMA2D_NULL_PTR  != deadTime      )    )
    {
        const uint32_t regValue = READ_REG( dma2d_PeriphConf[ periphId ].PeriphReg->AMTCR );

        *deadTimeState = ( 0u != ( DMA2D_AMTCR_EN & regValue ) ) ? DMA2D_FUNCTION_ACTIVE : DMA2D_FUNCTION_INACTIVE;
        *deadTime      = (dma2d_DeadTime_t)( ( regValue & DMA2D_AMTCR_DT ) >> DMA2D_AMTCR_DT_Pos );

        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Configures priority of DMA2D interrupt.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param irqPrio  [in]: Interrupt priority
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Set_IrqPriority( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t irqPrio )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( DMA2D_PERIPH_CNT > periphId )
    {
        const nvic_RequestState_t nvicState = Nvic_Set_PeriphIrq_Prio( dma2d_PeriphConf[ periphId ].PeriphNvic, (nvic_IrqPrio_t)irqPrio );

        retState = ( NVIC_REQUEST_OK == nvicState ) ? DMA2D_REQUEST_OK : DMA2D_REQUEST_ERROR;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns priority of DMA2D interrupt.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 * \param irqPrio [out]: Pointer to store the interrupt priority. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_IrqPriority( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t * const irqPrio )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT > periphId ) &&
        ( DMA2D_NULL_PTR  != irqPrio  )    )
    {
        nvic_IrqPrio_t            nvicPrio  = 0u;
        const nvic_RequestState_t nvicState = Nvic_Get_PeriphIrq_Prio( dma2d_PeriphConf[ periphId ].PeriphNvic, &nvicPrio );

        if( NVIC_REQUEST_OK == nvicState )
        {
            *irqPrio = (dma2d_IrqPrio_t)nvicPrio;
            retState = DMA2D_REQUEST_OK;
        }
        else
        {
            retState = DMA2D_REQUEST_ERROR;
        }
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns count of bits of one pixel of the color mode (e.g. buffer size
 *        calculation of the application).
 *
 * \param colorMode  [in]: Color mode, value from \ref dma2d_ColorMode_t
 * \param pixelBits [out]: Pointer to store the count of bits per pixel. Must not be NULL.
 *
 * \return Function processing state. Returns \ref DMA2D_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
dma2d_RequestState_t Dma2D_Get_PixelBits( dma2d_ColorMode_t colorMode, uint32_t * const pixelBits )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_COLOR_MODE_CNT > colorMode ) &&
        ( DMA2D_NULL_PTR      != pixelBits )    )
    {
        *pixelBits = dma2d_PixelBitsLut[ colorMode ];
        retState   = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}

/* =========================== LOCAL FUNCTIONS ============================== */

/**
 * \brief Checks configuration structure.
 *
 * \param dma2dConfig [in]: Pointer to configuration structure
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the configuration is valid. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_Config( const dma2d_Config_t * const dma2dConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_NULL_PTR        != dma2dConfig                ) &&
        ( DMA2D_PERIPH_CNT       > dma2dConfig->PeriphId      ) &&
        ( DMA2D_XFER_MODE_CNT    > dma2dConfig->XferMode      ) &&
        ( DMA2D_FUNCTION_ACTIVE >= dma2dConfig->DeadTimeState )    )
    {
        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks alignment of an address to the pixel size (16-bit and 32-bit pixels).
 *
 * \param address   [in]: Memory address
 * \param pixelBits [in]: Count of bits per pixel
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the address is aligned. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_Aligned( const void * const address, uint32_t pixelBits )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;
    const uintptr_t      addrValue = (uintptr_t)address;

    if( DMA2D_PIXEL_BITS_32 == pixelBits )
    {
        retState = ( 0u == ( addrValue % DMA2D_ALIGN_32 ) ) ? DMA2D_REQUEST_OK : DMA2D_REQUEST_ERROR;
    }
    else if( DMA2D_PIXEL_BITS_16 == pixelBits )
    {
        retState = ( 0u == ( addrValue % DMA2D_ALIGN_16 ) ) ? DMA2D_REQUEST_OK : DMA2D_REQUEST_ERROR;
    }
    else
    {
        /* Byte or nibble pixels - no alignment */
        retState = DMA2D_REQUEST_OK;
    }

    return ( retState );
}


/**
 * \brief Checks alpha inversion and red / blue swap of a layer or of the output.
 *
 * Alpha inversion (AI) and red / blue swap (RBS) of the PFC control registers exist on
 * every STM32H7 device - DMA2D without these bits accepts only \ref DMA2D_FUNCTION_INACTIVE.
 *
 * \param alphaInvert [in]: Alpha inversion state
 * \param redBlueSwap [in]: Red / blue swap state
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the options are available. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_PfcOptions( dma2d_FunctionState_t alphaInvert, dma2d_FunctionState_t redBlueSwap )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

#if defined(DMA2D_FGPFCCR_AI)
    if( ( DMA2D_FUNCTION_ACTIVE >= alphaInvert ) &&
        ( DMA2D_FUNCTION_ACTIVE >= redBlueSwap )    )
#else
    if( ( DMA2D_FUNCTION_INACTIVE == alphaInvert ) &&
        ( DMA2D_FUNCTION_INACTIVE == redBlueSwap )    )
#endif /* DMA2D_FGPFCCR_AI */
    {
        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        /* Option is not available or invalid */
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns PFC control register bits of alpha inversion and red / blue swap.
 *
 * Bit positions of AI / RBS are equal in FGPFCCR, BGPFCCR and OPFCCR.
 *
 * \param alphaInvert [in]: Alpha inversion state (valid)
 * \param redBlueSwap [in]: Red / blue swap state (valid)
 *
 * \return Register bits (0 on MCUs without the options)
 */
static uint32_t Dma2D_Get_PfcOptionBits( dma2d_FunctionState_t alphaInvert, dma2d_FunctionState_t redBlueSwap )
{
    uint32_t regBits = 0u;

#if defined(DMA2D_FGPFCCR_AI)
    if( DMA2D_FUNCTION_ACTIVE == alphaInvert )
    {
        regBits |= DMA2D_FGPFCCR_AI;
    }
    else
    {
        /* Alpha is not inverted */
    }

    if( DMA2D_FUNCTION_ACTIVE == redBlueSwap )
    {
        regBits |= DMA2D_FGPFCCR_RBS;
    }
    else
    {
        /* Regular RGB order */
    }
#else
    (void)alphaInvert;
    (void)redBlueSwap;
#endif /* DMA2D_FGPFCCR_AI */

    return ( regBits );
}


/**
 * \brief Checks input layer configuration.
 *
 * \param layerConfig [in]: Pointer to layer configuration
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the configuration is valid. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_Layer( const dma2d_LayerConfig_t * const layerConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_NULL_PTR        != layerConfig             ) &&
        ( DMA2D_NULL_PTR        != layerConfig->Address    ) &&
        ( DMA2D_COLOR_MODE_CNT   > layerConfig->ColorMode  ) &&
        ( DMA2D_ALPHA_MODE_CNT   > layerConfig->AlphaMode  ) &&
        ( DMA2D_LINE_OFFSET_MAX >= layerConfig->LineOffset )    )
    {
        retState = Dma2D_Check_PfcOptions( layerConfig->AlphaInvert, layerConfig->RedBlueSwap );

        if( DMA2D_REQUEST_OK == retState )
        {
            retState = Dma2D_Check_Aligned( layerConfig->Address, dma2d_PixelBitsLut[ layerConfig->ColorMode ] );
        }
        else
        {
            /* Alpha inversion / red blue swap not available */
        }
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks output configuration.
 *
 * \param outputConfig [in]: Pointer to output configuration
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the configuration is valid. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_Output( const dma2d_OutputConfig_t * const outputConfig )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_NULL_PTR              != outputConfig                          ) &&
        ( DMA2D_NULL_PTR              != outputConfig->Address                 ) &&
        ( DMA2D_OUTPUT_COLOR_MODE_CNT  > (uint32_t)outputConfig->ColorMode     ) &&
        ( DMA2D_LINE_OFFSET_MAX       >= outputConfig->LineOffset              ) &&
        ( 0u                           < outputConfig->PixelsPerLine           ) &&
        ( DMA2D_PIXELS_PER_LINE_MAX   >= outputConfig->PixelsPerLine           ) &&
        ( 0u                           < outputConfig->NumberOfLines           )    )
    {
        retState = Dma2D_Check_PfcOptions( outputConfig->AlphaInvert, outputConfig->RedBlueSwap );

        if( DMA2D_REQUEST_OK == retState )
        {
            retState = Dma2D_Check_Aligned( outputConfig->Address, dma2d_PixelBitsLut[ outputConfig->ColorMode ] );
        }
        else
        {
            /* Alpha inversion / red blue swap not available */
        }
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks transfer request (layers used by the mode, output, M2M color modes).
 *
 * \param xferRequest [in]: Pointer to transfer request
 *
 * \return Returns \ref DMA2D_REQUEST_OK if the request is valid. Otherwise returns
 *         \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_XferRequest( const dma2d_XferRequest_t * const xferRequest )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_NULL_PTR != xferRequest       ) &&
        ( DMA2D_MODE_CNT  > xferRequest->Mode )    )
    {
        retState = Dma2D_Check_Output( xferRequest->Output );
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    if( ( DMA2D_REQUEST_OK == retState          ) &&
        ( DMA2D_MODE_R2M   != xferRequest->Mode )    )
    {
        retState = Dma2D_Check_Layer( xferRequest->Foreground );
    }
    else
    {
        /* Invalid request or register to memory mode without foreground */
    }

    if( ( DMA2D_REQUEST_OK     == retState          ) &&
        ( DMA2D_MODE_M2M_BLEND == xferRequest->Mode )    )
    {
        retState = Dma2D_Check_Layer( xferRequest->Background );
    }
    else
    {
        /* Invalid request or mode without background */
    }

    if( ( DMA2D_REQUEST_OK == retState                                              ) &&
        ( DMA2D_MODE_M2M   == xferRequest->Mode                                     ) &&
        ( xferRequest->Foreground->ColorMode != xferRequest->Output->ColorMode      )    )
    {
        /* Copy without conversion keeps the pixel format */
        retState = DMA2D_REQUEST_ERROR;
    }
    else
    {
        /* Request is valid or already refused */
    }

    return ( retState );
}


/**
 * \brief Checks that the peripheral is initialized and no transfer / CLUT loading runs.
 *
 * \param periphId [in]: DMA2D peripheral identification, value from \ref dma2d_PeriphId_t
 *
 * \return Returns \ref DMA2D_REQUEST_OK if a new operation can be started. Otherwise
 *         returns \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Check_Idle( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t retState = DMA2D_REQUEST_ERROR;

    if( ( DMA2D_PERIPH_CNT        > periphId                                   ) &&
        ( DMA2D_FUNCTION_ACTIVE   == dma2d_PeriphContext[ periphId ].InitState ) &&
        ( DMA2D_XFER_STATE_IDLE   == dma2d_PeriphContext[ periphId ].XferState ) &&
        ( DMA2D_FUNCTION_INACTIVE == dma2d_PeriphContext[ periphId ].ClutState )    )
    {
        retState = DMA2D_REQUEST_OK;
    }
    else
    {
        retState = DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns registers of an input layer.
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 * \param layer    [in]: Input layer (valid)
 *
 * \return Registers of the layer
 */
static dma2d_LayerReg_t Dma2D_Get_LayerReg( dma2d_PeriphId_t periphId, dma2d_Layer_t layer )
{
    DMA2D_TypeDef * const periphReg = dma2d_PeriphConf[ periphId ].PeriphReg;
    dma2d_LayerReg_t      layerReg;

    if( DMA2D_LAYER_FOREGROUND == layer )
    {
        layerReg.MemAddr  = &periphReg->FGMAR;
        layerReg.Offset   = &periphReg->FGOR;
        layerReg.PfcCtrl  = &periphReg->FGPFCCR;
        layerReg.Color    = &periphReg->FGCOLR;
        layerReg.ClutAddr = &periphReg->FGCMAR;
    }
    else
    {
        layerReg.MemAddr  = &periphReg->BGMAR;
        layerReg.Offset   = &periphReg->BGOR;
        layerReg.PfcCtrl  = &periphReg->BGPFCCR;
        layerReg.Color    = &periphReg->BGCOLR;
        layerReg.ClutAddr = &periphReg->BGCMAR;
    }

    return ( layerReg );
}


/**
 * \brief Writes configuration of an input layer (CLUT configuration is kept).
 *
 * \param periphId    [in]: DMA2D peripheral identification (valid)
 * \param layer       [in]: Input layer (valid)
 * \param layerConfig [in]: Layer configuration (valid)
 */
static void Dma2D_Set_Layer( dma2d_PeriphId_t periphId, dma2d_Layer_t layer, const dma2d_LayerConfig_t * const layerConfig )
{
    const dma2d_LayerReg_t layerReg = Dma2D_Get_LayerReg( periphId, layer );
    const uint32_t         pfcValue = ( (uint32_t)layerConfig->ColorMode << DMA2D_FGPFCCR_CM_Pos ) |
                                      ( (uint32_t)layerConfig->AlphaMode << DMA2D_FGPFCCR_AM_Pos ) |
                                      ( (uint32_t)layerConfig->Alpha     << DMA2D_FGPFCCR_ALPHA_Pos ) |
                                      Dma2D_Get_PfcOptionBits( layerConfig->AlphaInvert, layerConfig->RedBlueSwap );

    WRITE_REG( *layerReg.MemAddr, (uint32_t)(uintptr_t)layerConfig->Address );
    WRITE_REG( *layerReg.Offset, (uint32_t)layerConfig->LineOffset );
    MODIFY_REG( *layerReg.PfcCtrl, DMA2D_PFCCR_XFER_MASK, pfcValue );
    WRITE_REG( *layerReg.Color, layerConfig->Color & DMA2D_LAYER_COLOR_MASK );
}


/**
 * \brief Aborts running transfer / CLUT loading and waits for the end of the abort.
 *
 * Interrupt sources and flags are cleared, the transfer is IDLE and CLUT loading
 * INACTIVE afterwards, no callback is called.
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 *
 * \return Returns \ref DMA2D_REQUEST_OK if nothing runs or the abort finished. Otherwise
 *         returns \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Set_Abort( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t            retState  = DMA2D_REQUEST_OK;
    DMA2D_TypeDef * const           periphReg = dma2d_PeriphConf[ periphId ].PeriphReg;
    dma2d_PeriphContext_t * const   context   = &dma2d_PeriphContext[ periphId ];

    if( ( DMA2D_XFER_STATE_IDLE != context->XferState ) ||
        ( DMA2D_FUNCTION_ACTIVE == context->ClutState )    )
    {
        SET_BIT( periphReg->CR, DMA2D_CR_ABORT );

        retState = DMA2D_REQUEST_ERROR;

        for( dma2d_TimeoutCnt_t iterationCnt = 0u; DMA2D_TIMEOUT_RAW > iterationCnt; iterationCnt ++ )
        {
            const uint32_t running = READ_BIT( periphReg->CR,      DMA2D_CR_START      ) |
                                     READ_BIT( periphReg->FGPFCCR, DMA2D_FGPFCCR_START ) |
                                     READ_BIT( periphReg->BGPFCCR, DMA2D_BGPFCCR_START );

            if( 0u == running )
            {
                retState = DMA2D_REQUEST_OK;
                break;
            }
            else
            {
                /* Abort has not yet finished */
            }
        }
    }
    else
    {
        /* Nothing runs */
    }

    CLEAR_BIT( periphReg->CR, ( DMA2D_CR_ALL_IT | DMA2D_CR_SUSP ) );
    WRITE_REG( periphReg->IFCR, DMA2D_IFCR_ALL );

    context->XferState = DMA2D_XFER_STATE_IDLE;
    context->ClutState = DMA2D_FUNCTION_INACTIVE;
    context->XferError = DMA2D_XFER_ERROR_NONE;

    return ( retState );
}


/**
 * \brief Registers and enables DMA2D interrupt (ISR mode) or disables it in NVIC (POLL mode).
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 *
 * \return Returns \ref DMA2D_REQUEST_OK if request was processed without problems.
 *         Otherwise returns \ref DMA2D_REQUEST_ERROR.
 */
static dma2d_RequestState_t Dma2D_Set_IrqInit( dma2d_PeriphId_t periphId )
{
    dma2d_RequestState_t                retState   = DMA2D_REQUEST_ERROR;
    const dma2d_PeriphConfig_t * const  periphConf = &dma2d_PeriphConf[ periphId ];
    const dma2d_Config_t * const        config     = &dma2d_PeriphContext[ periphId ].Config;

    CLEAR_BIT( periphConf->PeriphReg->CR, DMA2D_CR_ALL_IT );

    if( DMA2D_XFER_MODE_ISR == config->XferMode )
    {
        const nvic_RequestState_t  handlerState = Nvic_Set_PeriphIrq_Handler( periphConf->PeriphNvic, periphConf->PeriphIsr );
        const dma2d_RequestState_t prioState    = Dma2D_Set_IrqPriority( periphId, config->IrqPriority );
        const nvic_RequestState_t  actState     = Nvic_Set_PeriphIrq_Active( periphConf->PeriphNvic );

        if( ( NVIC_REQUEST_OK  == handlerState ) &&
            ( DMA2D_REQUEST_OK == prioState    ) &&
            ( NVIC_REQUEST_OK  == actState     )    )
        {
            retState = DMA2D_REQUEST_OK;
        }
        else
        {
            retState = DMA2D_REQUEST_ERROR;
        }
    }
    else
    {
        const nvic_RequestState_t nvicState = Nvic_Set_PeriphIrq_Inactive( periphConf->PeriphNvic );

        retState = ( NVIC_REQUEST_OK == nvicState ) ? DMA2D_REQUEST_OK : DMA2D_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Ends the transfer - interrupt sources of the transfer are disabled, the result is
 *        stored and reported by XferCompleteCallback / ErrorCallback.
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 * \param errorId  [in]: Result of the transfer
 */
static void Dma2D_Set_XferEnd( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t errorId )
{
    dma2d_PeriphContext_t * const context = &dma2d_PeriphContext[ periphId ];

    CLEAR_BIT( dma2d_PeriphConf[ periphId ].PeriphReg->CR, ( DMA2D_CR_XFER_IT | DMA2D_CR_TWIE | DMA2D_CR_SUSP ) );

    context->XferError = errorId;
    context->XferState = DMA2D_XFER_STATE_IDLE;

    if( DMA2D_XFER_ERROR_NONE == errorId )
    {
        if( DMA2D_NULL_PTR != context->Config.XferCompleteCallback )
        {
            context->Config.XferCompleteCallback();
        }
        else
        {
            /* Event is not reported */
        }
    }
    else if( DMA2D_NULL_PTR != context->Config.ErrorCallback )
    {
        context->Config.ErrorCallback( errorId );
    }
    else
    {
        /* Error is not reported */
    }
}


/**
 * \brief Ends CLUT loading - interrupt sources of the loading are disabled, the result is
 *        stored and reported by ClutCompleteCallback / ErrorCallback.
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 * \param errorId  [in]: Result of the loading
 */
static void Dma2D_Set_ClutEnd( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t errorId )
{
    dma2d_PeriphContext_t * const context = &dma2d_PeriphContext[ periphId ];

    CLEAR_BIT( dma2d_PeriphConf[ periphId ].PeriphReg->CR, DMA2D_CR_CLUT_IT );

    context->XferError = errorId;
    context->ClutState = DMA2D_FUNCTION_INACTIVE;

    if( DMA2D_XFER_ERROR_NONE == errorId )
    {
        if( DMA2D_NULL_PTR != context->Config.ClutCompleteCallback )
        {
            context->Config.ClutCompleteCallback();
        }
        else
        {
            /* Event is not reported */
        }
    }
    else if( DMA2D_NULL_PTR != context->Config.ErrorCallback )
    {
        context->Config.ErrorCallback( errorId );
    }
    else
    {
        /* Error is not reported */
    }
}


/**
 * \brief Processes DMA2D flags (ISR or Dma2D_Task()) - errors (configuration, transfer,
 *        CLUT access) end the running transfer / CLUT loading, line watermark is reported,
 *        end of transfer and end of CLUT loading are reported. Processed flags are cleared,
 *        flags without running operation are only cleared.
 *
 * \param periphId [in]: DMA2D peripheral identification (valid)
 */
static void Dma2D_Process_Events( dma2d_PeriphId_t periphId )
{
    DMA2D_TypeDef * const               periphReg = dma2d_PeriphConf[ periphId ].PeriphReg;
    const dma2d_PeriphContext_t * const context   = &dma2d_PeriphContext[ periphId ];
    const uint32_t                      isrFlags  = READ_REG( periphReg->ISR );

    /* -1- Errors - DMA2D stopped the operation */
    if( 0u != ( DMA2D_ISR_ERRORS & isrFlags ) )
    {
        dma2d_XferErrorId_t errorId = DMA2D_XFER_ERROR_CONFIG;

        if( 0u != ( DMA2D_ISR_CEIF & isrFlags ) )
        {
            errorId = DMA2D_XFER_ERROR_CONFIG;
        }
        else if( 0u != ( DMA2D_ISR_TEIF & isrFlags ) )
        {
            errorId = DMA2D_XFER_ERROR_TRANSFER;
        }
        else
        {
            errorId = DMA2D_XFER_ERROR_CLUT_ACCESS;
        }

        WRITE_REG( periphReg->IFCR, ( DMA2D_IFCR_CCEIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CAECIF ) );

        if( DMA2D_XFER_STATE_IDLE != context->XferState )
        {
            Dma2D_Set_XferEnd( periphId, errorId );
        }
        else if( DMA2D_FUNCTION_ACTIVE == context->ClutState )
        {
            Dma2D_Set_ClutEnd( periphId, errorId );
        }
        else
        {
            /* No operation runs - flags only cleared */
        }
    }
    else
    {
        /* No error */
    }

    /* -2- Line watermark */
    if( 0u != ( DMA2D_ISR_TWIF & isrFlags ) )
    {
        WRITE_REG( periphReg->IFCR, DMA2D_IFCR_CTWIF );

        if( ( DMA2D_XFER_STATE_IDLE != context->XferState                ) &&
            ( DMA2D_FUNCTION_ACTIVE == context->WatermarkState           ) &&
            ( DMA2D_NULL_PTR        != context->Config.WatermarkCallback )    )
        {
            context->Config.WatermarkCallback();
        }
        else
        {
            /* Watermark is not reported */
        }
    }
    else
    {
        /* Line watermark not reached */
    }

    /* -3- End of transfer */
    if( 0u != ( DMA2D_ISR_TCIF & isrFlags ) )
    {
        WRITE_REG( periphReg->IFCR, DMA2D_IFCR_CTCIF );

        if( DMA2D_XFER_STATE_IDLE != context->XferState )
        {
            Dma2D_Set_XferEnd( periphId, DMA2D_XFER_ERROR_NONE );
        }
        else
        {
            /* No transfer runs */
        }
    }
    else
    {
        /* Transfer runs */
    }

    /* -4- End of CLUT loading */
    if( 0u != ( DMA2D_ISR_CTCIF & isrFlags ) )
    {
        WRITE_REG( periphReg->IFCR, DMA2D_IFCR_CCTCIF );

        if( DMA2D_FUNCTION_ACTIVE == context->ClutState )
        {
            Dma2D_Set_ClutEnd( periphId, DMA2D_XFER_ERROR_NONE );
        }
        else
        {
            /* No CLUT loading runs */
        }
    }
    else
    {
        /* CLUT loading runs */
    }
}

/* =========================== INTERRUPT HANDLERS =========================== */

/**
 * \brief DMA2D global interrupt service routine
 */
static void Dma2D_Dma2D1_IsrHandler( void )
{
    Dma2D_Process_Events( DMA2D_PERIPH_1 );
}

/* ================================ TASKS =================================== */

#endif /* DMA2D */
