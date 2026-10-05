# DMA2D MCAL Module

The **DMA2D (Chrom-ART accelerator) MCAL module** provides an abstraction for 2D graphical
transfers of the DMA2D peripheral on STM32 microcontrollers.  
This module is part of the MCAL layer and ensures a unified API across STM32 families.  
Different STM32 families are maintained in separate branches; switch to the correct branch for your MCU.

This branch supports **STM32F4** devices with DMA2D (STM32F427 / 429 / 437 / 439 / 469 / 479).
On other STM32F4 devices the library is empty (CMSIS device header defines no `DMA2D`) and
the test sets are not created.

---

## 🚀 Features

- Register to memory transfer - fill of a rectangular area with one color
- Memory to memory copy, copy with pixel format conversion, blending of foreground and background
- Input color modes ARGB8888, RGB888, RGB565, ARGB1555, ARGB4444, L8, AL44, AL88, L4, A8, A4
- Output color modes ARGB8888, RGB888, RGB565, ARGB1555, ARGB4444
- Alpha modes of input layers (no modification, replace, combine) and color of A8 / A4 layers
- Automatic CLUT loading (ARGB8888 / RGB888 entries) of foreground and background
- Line watermark callback, dead time between AHB accesses (AHB master timer)
- Suspend, resume and abort of a running transfer
- End of transfer / CLUT loading and errors processed by DMA2D interrupt or by `Dma2D_Task()` polling
- Transfer, configuration and CLUT access errors reported by callback

---

## Public API

### Module Information

- `dma2d_ModuleVersion_t Dma2D_Get_ModuleVersion ( void );`  
  Returns the current version of the DMA2D module.

---

### Initialization

- `dma2d_RequestState_t Dma2D_Init ( const dma2d_Config_t * const dma2dConfig );`  
  Enables the peripheral clock, resets the peripheral, configures the dead time and the DMA2D
  interrupt (ISR data handling) and stores the callbacks.

- `dma2d_RequestState_t Dma2D_Deinit ( dma2d_PeriphId_t periphId );`  
  Aborts a running transfer, disables the interrupt, resets the peripheral and disables its clock.

- `void Dma2D_Task ( void );`  
  Processes end of transfer / CLUT loading, line watermark and errors of peripherals with POLL data handling.

- `dma2d_RequestState_t Dma2D_Get_DefaultConfig ( dma2d_Config_t * const dma2dConfig );`  
  Retrieves a default configuration structure for safe initialization.

---

### Transfer

- `dma2d_RequestState_t Dma2D_Set_XferStart ( dma2d_PeriphId_t periphId, const dma2d_XferRequest_t * const xferRequest );`  
  Checks the request, programs the layers and the output and starts the transfer.

- `dma2d_RequestState_t Dma2D_Set_XferStop ( dma2d_PeriphId_t periphId );`  
  Aborts a running transfer / CLUT loading (no callback).

- `dma2d_RequestState_t Dma2D_Set_XferSuspend ( dma2d_PeriphId_t periphId );`  
  `dma2d_RequestState_t Dma2D_Set_XferResume ( dma2d_PeriphId_t periphId );`  
  Suspends / resumes a running transfer.

- `dma2d_RequestState_t Dma2D_Get_XferState ( dma2d_PeriphId_t periphId, dma2d_XferState_t * const xferState );`  
  `dma2d_RequestState_t Dma2D_Get_XferError ( dma2d_PeriphId_t periphId, dma2d_XferErrorId_t * const xferError );`  
  Return the state of the transfer (IDLE / RUNNING / SUSPENDED) and the result of the last transfer / CLUT loading.

---

### CLUT Loading

- `dma2d_RequestState_t Dma2D_Set_ClutLoad ( dma2d_PeriphId_t periphId, dma2d_Layer_t layer, const dma2d_ClutConfig_t * const clutConfig );`  
  Starts automatic loading of the CLUT of the foreground / background layer.

- `dma2d_RequestState_t Dma2D_Get_ClutLoadState ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const loadState );`  
  Returns if CLUT loading runs.

---

### Configuration

- `dma2d_RequestState_t Dma2D_Set_LineWatermark ( dma2d_PeriphId_t periphId, dma2d_LineCnt_t lineNumber );`  
  `dma2d_RequestState_t Dma2D_Set_LineWatermarkInactive ( dma2d_PeriphId_t periphId );`  
  `dma2d_RequestState_t Dma2D_Get_LineWatermark ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const watermarkState, dma2d_LineCnt_t * const lineNumber );`  
  Line watermark of following transfers (WatermarkCallback).

- `dma2d_RequestState_t Dma2D_Set_DeadTime ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t deadTimeState, dma2d_DeadTime_t deadTime );`  
  `dma2d_RequestState_t Dma2D_Get_DeadTime ( dma2d_PeriphId_t periphId, dma2d_FunctionState_t * const deadTimeState, dma2d_DeadTime_t * const deadTime );`  
  Dead time between two AHB accesses of DMA2D.

- `dma2d_RequestState_t Dma2D_Set_IrqPriority ( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t irqPrio );`  
  `dma2d_RequestState_t Dma2D_Get_IrqPriority ( dma2d_PeriphId_t periphId, dma2d_IrqPrio_t * const irqPrio );`  
  Priority of the DMA2D interrupt.

---

### Utilities

- `dma2d_RequestState_t Dma2D_Get_PixelBits ( dma2d_ColorMode_t colorMode, uint32_t * const pixelBits );`  
  Returns the count of bits of one pixel of the color mode.

---

## Example

Fill of a 40 x 30 pixel rectangle of a 240 pixel wide RGB565 frame buffer:

```c
static void App_FillDone( void ) { /* rectangle filled */ }

dma2d_Config_t config;

(void)Dma2D_Get_DefaultConfig( &config );
config.XferCompleteCallback = App_FillDone;
(void)Dma2D_Init( &config );

const dma2d_OutputConfig_t output =
{
    .Address       = &frameBuffer[ ( 10u * 240u ) + 20u ],   /* line 10, column 20 */
    .LineOffset    = 240u - 40u,
    .ColorMode     = DMA2D_COLOR_MODE_RGB565,
    .PixelsPerLine = 40u,
    .NumberOfLines = 30u,
};
const dma2d_XferRequest_t request = { .Mode = DMA2D_MODE_R2M, .Output = &output, .FillColor = 0xF800u /* red */ };

(void)Dma2D_Set_XferStart( DMA2D_PERIPH_1, &request );
```

---

## Usage Notes

- Always initialize the module using **`Dma2D_Init`** before calling other functions.
- Addresses of 16-bit and 32-bit pixels (layers, output, ARGB8888 CLUT) shall be aligned to the pixel size.
- Memory to memory mode (`DMA2D_MODE_M2M`) copies pixels without conversion - foreground and output
  color modes shall be equal. Use `DMA2D_MODE_M2M_PFC` for conversion.
- Fill color of `DMA2D_MODE_R2M` is coded in the output color mode (OCOLR register format).
- CLUT of L8 / AL44 / AL88 / L4 layers shall be loaded by `Dma2D_Set_ClutLoad` before the transfer,
  the loaded CLUT configuration is kept by following transfers.
- Only one operation (transfer or CLUT loading) runs at a time.
- Buffers used by DMA2D shall not be cached / shall be in memory accessible by DMA2D (CCM data RAM
  of STM32F4 is not accessible).

---

## 🧪 Tests

- Unit tests (`Tests/UnitTests`) - registers emulated by RegMem, RCC and NVIC mocked; the test set is
  created only for MCUs with DMA2D.
- Integration tests (`Tests/IntegrationTests`) - transfers checked in RAM on a board with DMA2D
  (e.g. STM32F429I-DISC1 / NUCLEO-F429ZI), no wiring needed.

---

## 🛠 CMake Integration
Add this to your module's CMakeLists.txt:
```
target_link_libraries(User_Lib PRIVATE Dma2D_Lib)
```
---

## License

This project is licensed under the **Creative Commons Attribution–NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** — [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.

---

## 🌐 Useful Links

- [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html)
- [Azure DevOps](https://azure.microsoft.com/en-us/services/devops/)
- [Embedbits Github](https://github.com/Embedbits)
- [CC BY-NC 4.0 License](https://creativecommons.org/licenses/by-nc/4.0/)
