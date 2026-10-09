/**
 * \author Mr.Nobody
 * \file Dma2D.h
 * \ingroup Dma2D
 * \brief Dma2D module common functionality header file.
 *
 * This file contains the common functionality used internally by the module,
 * and shall provide interface between the module and the application.
 *
 */

#ifndef DMA2D_DMA2D_H
#define DMA2D_DMA2D_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Dma2D_Types.h"                    /* Module types definitions       */
#include "Rcc_Types.h"                      /* RCC peripheral identification  */
#include "Nvic_Types.h"                     /* NVIC interrupt identification  */
/* ============================= TYPEDEFS =================================== */

/** \brief Count of loop iterations of register state polling */
typedef uint32_t dma2d_TimeoutCnt_t;


#if defined(DMA2D)
/** \brief DMA2D peripheral configuration item */
typedef struct
{
    DMA2D_TypeDef          *PeriphReg;  /**< Peripheral registers              */
    rcc_PeriphId_t          PeriphRcc;  /**< RCC peripheral identification     */
    nvic_PeriphIrqList_t    PeriphNvic; /**< NVIC interrupt identification     */
    nvic_IsrCallback_t      PeriphIsr;  /**< Interrupt service routine         */
}   dma2d_PeriphConfig_t;
#endif /* DMA2D */


/** \brief Runtime context of one DMA2D peripheral */
typedef struct
{
    dma2d_Config_t                  Config;         /**< Configuration of \ref Dma2D_Init                  */
    dma2d_FunctionState_t           InitState;      /**< Peripheral was initialized by \ref Dma2D_Init     */
    dma2d_FunctionState_t           WatermarkState; /**< Line watermark interrupt is used by transfers     */
    volatile dma2d_XferState_t      XferState;      /**< State of the transfer                             */
    volatile dma2d_FunctionState_t  ClutState;      /**< CLUT loading is running                           */
    volatile dma2d_XferErrorId_t    XferError;      /**< Result of the last transfer / CLUT loading        */
}   dma2d_PeriphContext_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Count of loop iterations of waiting for abort / register write (as other STM32U5 modules) */
#define DMA2D_TIMEOUT_RAW                   ( (dma2d_TimeoutCnt_t)0x84FCBu )

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

#ifdef __cplusplus
}
#endif

#endif /* DMA2D_DMA2D_H */
