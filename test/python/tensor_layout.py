# SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# RUN: %python %s | FileCheck %s

from ttmlir.ir import *
from ttmlir.dialects import tt

ctx = Context()
tt.register_dialect(ctx)


def createTensorLayout(
    shape,
    grid,
    memorySpace=tt.MemorySpace.DeviceL1,
    collapseIntervals=[(0, -1)],
    oobVal=tt.OOBVal.Undef,
):
    if isinstance(grid, list) or isinstance(grid, tuple):
        grid = tt.ir.GridAttr.get(ctx, list(grid))
    tensorTy = RankedTensorType.get(
        shape, F32Type.get(ctx), None, Location.unknown(ctx)
    )
    layout = tt.ir.LayoutAttr.get(
        ctx, tensorTy, memorySpace, grid, collapseIntervals, oobVal
    )
    return RankedTensorType.get(shape, F32Type.get(ctx), layout, Location.unknown(ctx))


def tilize(tensor, dataType, tileShape=[32, 32]):
    assert len(tileShape) == 2
    return tt.ir.LayoutAttr.with_element_type_(
        ctx,
        tensor.encoding,
        tt.ir.TileType.get(ctx, tileShape[0], tileShape[1], dataType),
    )


def parallelize(tensor, grid, collapseIntervals=[(0, -1)]):
    if isinstance(grid, list) or isinstance(grid, tuple):
        grid = tt.ir.GridAttr.get(ctx, list(grid))
    return tt.ir.LayoutAttr.with_grid_(
        ctx, tensor.encoding, tensor.shape, grid, collapseIntervals
    )


t0 = createTensorLayout([2, 3, 64, 128], [2, 4])
# CHECK: tensor<2x3x64x128xf32, #tt.layout<(d0, d1, d2, d3) -> (d0 * 192 + d1 * 64 + d2, d3), undef, <2x4>, memref<192x32xf32, #tt.memory_space<l1>>>>
print(t0)
# CHECK: #tt.layout<(d0, d1, d2, d3) -> (d0 * 192 + d1 * 64 + d2, d3), undef, <2x4>, memref<6x1x!tt.tile<32 x 32, bfp_bf8>, #tt.memory_space<l1>>>
print(tilize(t0, tt.DataType.BFP_BFloat8).wrapped())
print(parallelize(t0, [3, 2]).wrapped())

t1 = createTensorLayout([2, 3, 64, 128], [2, 2, 4], collapseIntervals=[(1, -1)])
print(tilize(t1, tt.DataType.BFP_BFloat8).wrapped())
print(parallelize(t1, [3, 2]).wrapped())
