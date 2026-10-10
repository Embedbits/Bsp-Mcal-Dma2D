/**
 * \author Mr.Nobody
 * \file Dma2D_Port.h
 * \ingroup Dma2D
 * \brief Dma2D module public functionality
 *
 * This file contains all available public functionality, any other files shall
 * not used outside of the module.
 *
 */

#ifndef DMA2D_DMA2D_PORT_H
#define DMA2D_DMA2D_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Dma2D_Types.h"                    /* Module types definition        */
/* ============================== TYPEDEFS ================================== */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/* ========================== EXPORTED MACROS =============================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

dma2d_ModuleVersion_t   Dma2D_Get_ModuleVersion         ( void );

dma2d_RequestState_t    Dma2D_Init                      ( const dma2d_Config_t * const dma2dConfig );
dma2d_RequestState_t    Dma2D_Deinit                    ( dma2d_PeriphId_t periphId );
void                    Dma2D_Task                      ( void );

dma2d_RequestState_t    Dma2D_Get_DefaultConfig         ( dma2d_Config_t * const dma2dConfig );

/* -------------------------------- Transfer -------------------------------- */

dma2d_RequestState_t    Dma2D_Set_XferStart             ( dma2d_PeriphId_t periphId, const dma2d_XferRequest_t * const xferRequest );
dma2d_RequestState_t    Dma2D_Set_XferStop              ( dma2d_PeriphId_t periphId );
dma2d_RequestState_t    Dma2D_Set_XferSuspend           ( dma2d_PeriphId_t periphId );
dma2d_RequestState_t    Dma2D_Set_XferResume            ( dma2d_PeriphId_t periphId );
dma2d_RequestState_t    Dma2D_Get_XferState             ( dma2d_PeriphId_t periphId, dma2d_XferState_t * const xferState );
dma2d_RequestState_t    Dma2D_Get_XferError             ( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t * const xferError );

/* ------------------------------ CLUT loading ------------------------------ */

dma2d_RequestState_t    Dma2D_Set_ClutLoad              ( dma2d_PeriphId_t periphId, dma2d_Layer_t layer, const dma2d_ClutConfig_t * const clutConfig );
dma2d_RequestState_t    Dma2D_Get_ClutLoadState         ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const loadState );

/* ------------------------------ Configuration ----------------------------- */

dma2d_RequestState_t    Dma2D_Set_LineWatermark         ( dma2d_PeriphId_t periphId, dma2d_LineCnt_t lineNumber );
dma2d_RequestState_t    Dma2D_Set_LineWatermarkInactive ( dma2d_PeriphId_t periphId );
dma2d_RequestState_t    Dma2D_Get_LineWatermark         ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const watermarkState, dma2d_LineCnt_t * const lineNumber );

dma2d_RequestState_t    Dma2D_Set_DeadTime              ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t deadTimeState, dma2d_DeadTime_t deadTime );
dma2d_RequestState_t    Dma2D_Get_DeadTime              ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const deadTimeState, dma2d_DeadTime_t * const deadTime );

dma2d_RequestState_t    Dma2D_Set_IrqPriority           ( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t irqPrio );
dma2d_RequestState_t    Dma2D_Get_IrqPriority           ( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t * const irqPrio );

/* -------------------------------- Utilities ------------------------------- */

dma2d_RequestState_t    Dma2D_Get_PixelBits             ( dma2d_ColorMode_t colorMode, uint32_t * const pixelBits );

#ifdef __cplusplus
}
#endif

#endif /* DMA2D_DMA2D_PORT_H */
