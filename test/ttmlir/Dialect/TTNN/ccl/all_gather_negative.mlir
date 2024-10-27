// RUN: not ttmlir-opt --ttir-to-ttnn-backend-pipeline %s 2>&1 | FileCheck %s
// CHECK: error: 'ttir.all_gather' op Invalid dimension for all gather op
#any_device = #tt.operand_constraint<dram|l1|scalar|tile|any_device|any_device_tile>
module attributes {} {
  func.func @forward(%arg0: tensor<1x1x32x32xbf16>) -> tensor<1x1x32x128xbf16> {
    %0 = tensor.empty() : tensor<1x1x32x128xbf16>
    %1 = "ttir.all_gather"(%arg0, %0) <{dim = 4 : si32, operand_constraints = [#any_device, #any_device]}> : (tensor<1x1x32x32xbf16>, tensor<1x1x32x128xbf16>) -> tensor<1x1x32x128xbf16>
    return %1 : tensor<1x1x32x128xbf16>
  }
}