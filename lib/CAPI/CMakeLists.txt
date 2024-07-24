set(MLIR_C_LIB "")
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
  set(MLIR_C_LIB "MLIR-C")
endif()

add_mlir_public_c_api_library(TTMLIRCAPI
  Dialects.cpp
  TTKernelTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/ttmlir-c/

  ENABLE_AGGREGATION

  LINK_LIBS PUBLIC
  ${MLIR_C_LIB}
  MLIRTTDialect
  MLIRTTIRDialect
  MLIRTTKernelDialect
  MLIRTTIRTransforms
  MLIRTTIRAnalysis
)
