/**
 * \author Mr.Nobody
 * \file BspMain.h
 * \ingroup Dma2D
 * \brief Entry point of Dma2D integration test firmware (called by StartUp).
 *
 * StartUp calls BspMain() and gets this header through the interface library
 * BspMain_Lib (created by Tests/IntegrationTests/CMakeLists.txt). BspMain() is
 * implemented by ItTarget_Dma2D.c.
 */

#ifndef DMA2D_ITTEST_BSPMAIN_H
#define DMA2D_ITTEST_BSPMAIN_H

#ifdef __cplusplus
 extern "C" {
#endif /* __cplusplus */

/* ======================== EXPORTED FUNCTIONS ============================== */

void BspMain( void );

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DMA2D_ITTEST_BSPMAIN_H */
