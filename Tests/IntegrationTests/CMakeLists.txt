#===================== Integration tests configuration ========================#
# Integration tests of Dma2D module on target (board with DMA2D, e.g.
# NUCLEO-U5A5ZJ-Q). Registered in module CMakeLists.txt by IntegrationTesting_AddPath()
# and created by IntegrationTesting_Generate() in "IntegrationTest" build
# (see EmBi_Platform/CMake/IntegrationTesting/README.md).
#
# Transfers run between buffers in SRAM and are checked by CPU, no wiring is needed.
#
# The test set is independent - target interface (entry point, initialization,
# fault handlers, system reset) is implemented by ItTarget_Dma2D.c and
# all libraries of the firmware are listed here.

# MCUs without DMA2D peripheral (STM32U535 / U545 / U575 / U585 - CMSIS device
# header defines no DMA2D) have empty Dma2D_Lib - the test
# set is not created.
get_target_property(DMA2D_IT_RAL_DEFINES  Ral_Lib INTERFACE_COMPILE_DEFINITIONS)
get_target_property(DMA2D_IT_RAL_INCLUDES Ral_Lib INTERFACE_INCLUDE_DIRECTORIES)
list(FILTER DMA2D_IT_RAL_DEFINES INCLUDE REGEX "^STM32")
string(TOLOWER "${DMA2D_IT_RAL_DEFINES}.h" DMA2D_IT_DEVICE_HEADER)
find_file(DMA2D_IT_DEVICE_HEADER_PATH "${DMA2D_IT_DEVICE_HEADER}"
    PATHS ${DMA2D_IT_RAL_INCLUDES} NO_DEFAULT_PATH NO_CACHE REQUIRED)
file(STRINGS "${DMA2D_IT_DEVICE_HEADER_PATH}" DMA2D_IT_DMA2D_DEFINE REGEX "^#define[ \t]+DMA2D[ \t]")

if(NOT DMA2D_IT_DMA2D_DEFINE)
    message(STATUS "Dma2D integration tests skipped - ${DMA2D_IT_RAL_DEFINES} has no DMA2D peripheral")
    return()
endif()

# StartUp calls BspMain() - the entry point of the test firmware is provided by
# the test set (BspMain.h, ItTarget_Dma2D.c)
if(NOT TARGET BspMain_Lib)
    add_library(BspMain_Lib INTERFACE)
    target_include_directories(BspMain_Lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

IntegrationTesting_Add_Test(
    NAME            Dma2D
    TEST_SOURCE     ItTest_Dma2D.c
    SOURCES         ItTarget_Dma2D.c    # Target interface of the test set
    LINK_LIBS       StartUp_Lib         # Startup code (calls BspMain)
                    Nvic_Lib            # Fault handlers, system reset (ItTarget)
                    Dma2D_Lib           # Module under test
    TIMEOUT         30
)
