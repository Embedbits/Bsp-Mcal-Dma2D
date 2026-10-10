#======================== Unit tests configuration ============================#
# Unit tests of Dma2D module. Registered in module CMakeLists.txt by
# UnitTesting_AddPath() and created by UnitTesting_Generate() in "UnitTest"
# build (see EmBi_Platform/CMake/UnitTesting/README.md).

get_filename_component(DMA2D_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(MCAL_DIR  "${DMA2D_DIR}/.." ABSOLUTE)

# MCUs without DMA2D peripheral (CMSIS device header defines no DMA2D - every STM32H7
# has DMA2D, kept for the common module) have empty Dma2D_Lib - the test set is not created.
get_target_property(DMA2D_UT_RAL_DEFINES  Ral_Lib INTERFACE_COMPILE_DEFINITIONS)
get_target_property(DMA2D_UT_RAL_INCLUDES Ral_Lib INTERFACE_INCLUDE_DIRECTORIES)
list(FILTER DMA2D_UT_RAL_DEFINES INCLUDE REGEX "^STM32")
string(TOLOWER "${DMA2D_UT_RAL_DEFINES}.h" DMA2D_UT_DEVICE_HEADER)
find_file(DMA2D_UT_DEVICE_HEADER_PATH "${DMA2D_UT_DEVICE_HEADER}"
    PATHS ${DMA2D_UT_RAL_INCLUDES} NO_DEFAULT_PATH NO_CACHE REQUIRED)
file(STRINGS "${DMA2D_UT_DEVICE_HEADER_PATH}" DMA2D_UT_DMA2D_DEFINE REGEX "^#define[ \t]+DMA2D[ \t]")

if(NOT DMA2D_UT_DMA2D_DEFINE)
    message(STATUS "Dma2D unit tests skipped - ${DMA2D_UT_RAL_DEFINES} has no DMA2D peripheral")
    return()
endif()

UnitTesting_Add_Test(
    NAME            Dma2D
    TEST_SOURCE     Test_Dma2D.c
    UUT_SOURCES     ${DMA2D_DIR}/Dma2D.c
    MOCK_HEADERS    ${MCAL_DIR}/Rcc/Rcc_Port.h
                    ${MCAL_DIR}/Nvic/Nvic_Port.h
    INCLUDE_DIRS    ${DMA2D_DIR}
                    ${MCAL_DIR}/Rcc
                    ${MCAL_DIR}/Nvic
    LINK_LIBS       ut_RegMem                           # Registers in host memory
                    ut_CmsisHost                        # CMSIS core intrinsics on host
    FORCE_INCLUDES  ${UNIT_TESTING_CMSIS_HOST_HEADER}   # Replaces cmsis_gcc.h
    HOST_LIBS       Ral_Lib                             # ST LL compiled for host
    COVERAGE_EXCLUDE ".*/Ral/.*"
)
