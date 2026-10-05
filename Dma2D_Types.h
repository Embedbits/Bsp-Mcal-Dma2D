/**
 * \defgroup Dma2D Dma2D
 * \brief Dma2D module
 */

/**
 * \author Mr.Nobody
 * \file Dma2D_Types.h
 * \ingroup Dma2D
 * \brief Dma2D module global types definition
 *
 * This file contains the types definitions used across the module and are
 * available for other modules through Port file.
 *
 */

#ifndef DMA2D_DMA2D_TYPES_H
#define DMA2D_DMA2D_TYPES_H
/* ============================== INCLUDES ================================== */
#include "stdint.h"                         /* Module types definition        */
#include "Stm32_dma2d.h"                    /* DMA2D RAL functionality        */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Null pointer definition */
#define DMA2D_NULL_PTR                      ( ( void* ) 0u )

/** Maximal line offset in pixels (14-bit FGOR / BGOR / OOR field) */
#define DMA2D_LINE_OFFSET_MAX               ( 0x3FFFu )

/** Maximal count of pixels per line (14-bit NLR.PL field) */
#define DMA2D_PIXELS_PER_LINE_MAX           ( 0x3FFFu )

/** Maximal count of lines (16-bit NLR.NL field) */
#define DMA2D_LINES_MAX                     ( 0xFFFFu )

/** Maximal count of CLUT entries */
#define DMA2D_CLUT_SIZE_MAX                 ( 256u )

/* ========================== EXPORTED MACROS =============================== */

/* ============================== TYPEDEFS ================================== */

/** \brief Type signaling major version of SW module */
typedef uint8_t dma2d_MajorVersion_t;


/** \brief Type signaling minor version of SW module */
typedef uint8_t dma2d_MinorVersion_t;


/** \brief Type signaling patch version of SW module */
typedef uint8_t dma2d_PatchVersion_t;


/** \brief Type signaling actual version of SW module */
typedef struct
{
    dma2d_MajorVersion_t Major; /**< Major version */
    dma2d_MinorVersion_t Minor; /**< Minor version */
    dma2d_PatchVersion_t Patch; /**< Patch version */
}   dma2d_ModuleVersion_t;


/** Function status enumeration */
typedef enum
{
    DMA2D_FUNCTION_INACTIVE = 0u, /**< Function status is inactive */
    DMA2D_FUNCTION_ACTIVE         /**< Function status is active   */
}   dma2d_FunctionState_t;


/** Enumeration used to signal request processing state */
typedef enum
{
    DMA2D_REQUEST_ERROR = 0u, /**< Processing request failed  */
    DMA2D_REQUEST_OK          /**< Processing request succeed */
}   dma2d_RequestState_t;


/** DMA2D peripherals enumeration list */
typedef enum
{
#if defined(DMA2D)
    DMA2D_PERIPH_1 = 0u, /**< DMA2D peripheral 1 ID (Chrom-ART accelerator) */
#endif /* DMA2D */
    DMA2D_PERIPH_CNT     /**< Count of DMA2D peripherals */
}   dma2d_PeriphId_t;


/** \brief Interrupt priority */
typedef uint32_t dma2d_IrqPrio_t;


/** \brief Color value (ARGB8888 for layer / fill colors, see the item description) */
typedef uint32_t dma2d_Color_t;


/** \brief Alpha value (0 - transparent, 255 - opaque) */
typedef uint8_t dma2d_Alpha_t;


/** \brief Line offset - count of pixels skipped at the end of each line */
typedef uint16_t dma2d_LineOffset_t;


/** \brief Count of pixels per line */
typedef uint16_t dma2d_PixelCnt_t;


/** \brief Count of lines (also line number of the line watermark) */
typedef uint16_t dma2d_LineCnt_t;


/** \brief Count of CLUT entries (1 - \ref DMA2D_CLUT_SIZE_MAX) */
typedef uint16_t dma2d_ClutSize_t;


/** \brief Dead time between two AHB accesses in AHB clock cycles */
typedef uint8_t dma2d_DeadTime_t;


/** Transfer mode enumeration */
typedef enum
{
    DMA2D_MODE_M2M = 0u,  /**< Memory to memory - foreground copied without conversion       */
    DMA2D_MODE_M2M_PFC,   /**< Memory to memory with pixel format conversion of foreground   */
    DMA2D_MODE_M2M_BLEND, /**< Memory to memory with blending of foreground and background   */
    DMA2D_MODE_R2M,       /**< Register to memory - output area filled with one color         */
    DMA2D_MODE_CNT        /**< Count of transfer modes                                        */
}   dma2d_Mode_t;


/**
 * Color mode enumeration.
 *
 * All modes can be used for foreground / background layers, output supports
 * ARGB8888, RGB888, RGB565, ARGB1555 and ARGB4444 only.
 */
typedef enum
{
    DMA2D_COLOR_MODE_ARGB8888 = 0u, /**< 32 bits per pixel, alpha, red, green, blue */
    DMA2D_COLOR_MODE_RGB888,        /**< 24 bits per pixel                          */
    DMA2D_COLOR_MODE_RGB565,        /**< 16 bits per pixel                          */
    DMA2D_COLOR_MODE_ARGB1555,      /**< 16 bits per pixel                          */
    DMA2D_COLOR_MODE_ARGB4444,      /**< 16 bits per pixel                          */
    DMA2D_COLOR_MODE_L8,            /**< 8-bit CLUT index (input only)              */
    DMA2D_COLOR_MODE_AL44,          /**< 4-bit alpha, 4-bit CLUT index (input only) */
    DMA2D_COLOR_MODE_AL88,          /**< 8-bit alpha, 8-bit CLUT index (input only) */
    DMA2D_COLOR_MODE_L4,            /**< 4-bit CLUT index (input only)              */
    DMA2D_COLOR_MODE_A8,            /**< 8-bit alpha, color of the layer (input only) */
    DMA2D_COLOR_MODE_A4,            /**< 4-bit alpha, color of the layer (input only) */
    DMA2D_COLOR_MODE_CNT            /**< Count of color modes                       */
}   dma2d_ColorMode_t;


/** Alpha mode of input layer enumeration */
typedef enum
{
    DMA2D_ALPHA_MODE_NO_MODIF = 0u, /**< Alpha of the pixel is used                          */
    DMA2D_ALPHA_MODE_REPLACE,       /**< Alpha of the pixel is replaced by alpha of the layer */
    DMA2D_ALPHA_MODE_COMBINE,       /**< Alpha of the pixel is multiplied by alpha of the layer */
    DMA2D_ALPHA_MODE_CNT            /**< Count of alpha modes                                 */
}   dma2d_AlphaMode_t;


/** CLUT color mode enumeration */
typedef enum
{
    DMA2D_CLUT_COLOR_MODE_ARGB8888 = 0u, /**< CLUT entries of 32 bits */
    DMA2D_CLUT_COLOR_MODE_RGB888,        /**< CLUT entries of 24 bits */
    DMA2D_CLUT_COLOR_MODE_CNT            /**< Count of CLUT color modes */
}   dma2d_ClutColorMode_t;


/** Input layer enumeration */
typedef enum
{
    DMA2D_LAYER_FOREGROUND = 0u, /**< Foreground layer */
    DMA2D_LAYER_BACKGROUND,      /**< Background layer */
    DMA2D_LAYER_CNT              /**< Count of layers  */
}   dma2d_Layer_t;


/** Data handling enumeration - end of transfer and errors are processed by */
typedef enum
{
    DMA2D_XFER_MODE_ISR = 0u, /**< DMA2D interrupt service routine            */
    DMA2D_XFER_MODE_POLL,     /**< Dma2D_Task() (polling of DMA2D flags)        */
    DMA2D_XFER_MODE_CNT       /**< Count of data handling modes               */
}   dma2d_XferMode_t;


/** Transfer state enumeration */
typedef enum
{
    DMA2D_XFER_STATE_IDLE = 0u, /**< No transfer is running         */
    DMA2D_XFER_STATE_RUNNING,   /**< Transfer is running            */
    DMA2D_XFER_STATE_SUSPENDED, /**< Running transfer is suspended  */
    DMA2D_XFER_STATE_CNT        /**< Count of transfer states       */
}   dma2d_XferState_t;


/** Transfer error enumeration */
typedef enum
{
    DMA2D_XFER_ERROR_NONE = 0u,     /**< No error (last transfer / CLUT load finished successfully) */
    DMA2D_XFER_ERROR_TRANSFER,      /**< AHB bus error during the transfer (TEIF)                   */
    DMA2D_XFER_ERROR_CONFIG,        /**< Configuration error detected by DMA2D (CEIF)               */
    DMA2D_XFER_ERROR_CLUT_ACCESS,   /**< CLUT accessed by CPU during automatic CLUT loading (CAEIF)  */
    DMA2D_XFER_ERROR_CNT            /**< Count of transfer errors                                   */
}   dma2d_XferErrorId_t;


/** \brief Callback of transfer complete, CLUT loading complete and line watermark */
typedef void ( dma2d_XferCallback_t )( void );


/** \brief Callback of transfer / CLUT loading error */
typedef void ( dma2d_XferErrCallback_t )( dma2d_XferErrorId_t errorId );


/**
 * \brief Input layer (foreground / background) configuration.
 *
 * Address shall be aligned to the pixel size of 16-bit (2 bytes) and 32-bit (4 bytes)
 * color modes.
 */
typedef struct
{
    const void         *Address;    /**< Address of the first pixel                                       */
    dma2d_LineOffset_t  LineOffset; /**< Pixels skipped at the end of each line (\ref DMA2D_LINE_OFFSET_MAX) */
    dma2d_ColorMode_t   ColorMode;  /**< Color mode of the layer                                          */
    dma2d_AlphaMode_t   AlphaMode;  /**< Alpha mode of the layer                                          */
    dma2d_Alpha_t       Alpha;      /**< Alpha of the layer (replace / combine alpha mode)                */
    dma2d_Color_t       Color;      /**< RGB888 color of A8 / A4 layer (bits 23:0)                        */
}   dma2d_LayerConfig_t;


/**
 * \brief Output configuration.
 *
 * Address shall be aligned to the pixel size of 16-bit (2 bytes) and 32-bit (4 bytes)
 * color modes.
 */
typedef struct
{
    void               *Address;       /**< Address of the first output pixel                                */
    dma2d_LineOffset_t  LineOffset;    /**< Pixels skipped at the end of each line (\ref DMA2D_LINE_OFFSET_MAX) */
    dma2d_ColorMode_t   ColorMode;     /**< Output color mode (ARGB8888 - ARGB4444)                          */
    dma2d_PixelCnt_t    PixelsPerLine; /**< Count of pixels per line (1 - \ref DMA2D_PIXELS_PER_LINE_MAX)    */
    dma2d_LineCnt_t     NumberOfLines; /**< Count of lines (1 - \ref DMA2D_LINES_MAX)                        */
}   dma2d_OutputConfig_t;


/** \brief Transfer request */
typedef struct
{
    dma2d_Mode_t                Mode;       /**< Transfer mode                                                    */
    const dma2d_LayerConfig_t  *Foreground; /**< Foreground layer (memory to memory modes, not used by R2M)       */
    const dma2d_LayerConfig_t  *Background; /**< Background layer (blending only)                                 */
    const dma2d_OutputConfig_t *Output;     /**< Output area                                                      */
    dma2d_Color_t               FillColor;  /**< Fill color of R2M mode coded in the output color mode (OCOLR)    */
}   dma2d_XferRequest_t;


/** \brief CLUT loading configuration */
typedef struct
{
    const void             *Address;   /**< Address of the CLUT in memory (32-bit aligned for ARGB8888) */
    dma2d_ClutColorMode_t   ColorMode; /**< Color mode of CLUT entries                                  */
    dma2d_ClutSize_t        Size;      /**< Count of entries (1 - \ref DMA2D_CLUT_SIZE_MAX)             */
}   dma2d_ClutConfig_t;


/** \brief DMA2D configuration */
typedef struct
{
    dma2d_PeriphId_t         PeriphId;             /**< DMA2D peripheral                                              */
    dma2d_XferMode_t         XferMode;             /**< End of transfer processed by ISR or Dma2D_Task()              */
    dma2d_IrqPrio_t          IrqPriority;          /**< DMA2D interrupt priority (ISR mode)                           */
    dma2d_FunctionState_t    DeadTimeState;        /**< Dead time between AHB accesses (AHB master timer) is used    */
    dma2d_DeadTime_t         DeadTime;             /**< Dead time in AHB clock cycles                                 */
    dma2d_XferCallback_t    *XferCompleteCallback; /**< Transfer complete. DMA2D_NULL_PTR if not used.                */
    dma2d_XferCallback_t    *ClutCompleteCallback; /**< CLUT loading complete. DMA2D_NULL_PTR if not used.            */
    dma2d_XferCallback_t    *WatermarkCallback;    /**< Line watermark reached. DMA2D_NULL_PTR if not used.           */
    dma2d_XferErrCallback_t *ErrorCallback;        /**< Transfer / CLUT loading error. DMA2D_NULL_PTR if not used.    */
}   dma2d_Config_t;

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

#endif /* DMA2D_DMA2D_TYPES_H */
