# tt-mlir

## Build

### Environment setup

You only need to build this once, it builds a python virtual environment with the necessary dependencies, flatbuffers, and llvm.

```bash
cmake -B env/build env
cmake --build env/build
```

### Build

```bash
source env/activate
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Note:
- Accelerate builds with ccache: `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`

### Test

```bash
source env/activate
cmake --build build -- check-ttmlir
```

### Misc

- `ttrt`: A runtime tool for parsing and executing flatbuffer binaries
```bash
source env/activate
cmake --build build -- ttrt
```
- `clang-tidy`: Run clang-tidy on the project
```bash
source env/activate
cmake --build build -- clang-tidy
```

## This repo is very much a work in progress

- mlir input tosa/linalg examples `test/ttmlir`.
- Simple test (each flag is a pass that runs in order):
```bash
./build/bin/ttmlir-opt --convert-tosa-to-ttir --ttir-to-ttnn-backend-pipeline test/ttmlir/simple_eltwise_tosa.mlir
# Or
./build/bin/ttmlir-opt --convert-tosa-to-ttir --ttir-to-ttmetal-backend-pipeline test/ttmlir/simple_eltwise_tosa.mlir
```

## Useful links / reading

- [affine dialect](https://mlir.llvm.org/docs/Dialects/Affine/)
  - Affine map is a really powerful primitive that can be used to describe most data movement patterns.
  - It can also be used to describe memory layouts.
- [linalg dialect](https://mlir.llvm.org/docs/Dialects/Linalg/)
- [tosa dialect](https://mlir.llvm.org/docs/Dialects/TOSA/)
- [memref dialect](https://mlir.llvm.org/docs/Dialects/MemRef/)
- [tosa spec](https://www.mlplatform.org/tosa/tosa_spec.html)
- [torch-mlir](https://github.com/llvm/torch-mlir)
  - See `python/resnet18.py` for an example of collecting torch-mlir output.
  - `source env/activate && python python/resnet18.py > resnet18_linalg.mlir`
  - Uncomment `output_type = "linalg-on-tensors"` to get linalg output.
  - Uncomment `resnet50` to get resnet50 output.
  - Tosa dialect by default embeds all constant tensor data + weights in the IR making it too large to checkin. Hopefully there's a way to disable this.

## Dialects

- `tt`: Common types such as, `tt.tile`, `tt.layout`, `tt.grid`, etc. and enums such as, data formats, memory spaces, iterator types etc.
- `ttir`: A high level dialect that models the tensor compute graph on tenstorrent devices. Accepts `tosa` and `linalg` input.
  - `ttir.generic`: Generically describe compute work.
  - `ttir.layout`: Convert between different tensor memory layouts and transfer between different memory spaces.
  - `tensor.pad`: Pad a tensor with a value (ie. convs)
  - `ttir.yield`: return result memref of computation in dispatch region body, lowers to `ttkernel.yield`
  - `ttir.kernel`: lowers to some backend kernel
- `ttnn`: A TTNN dialect that pattern matches to ttnn operations.
- `ttir` lowers to `ttnn` or `ttmetal` and `ttkernel` dialects:
  - `ttmetal`: Operations that dispatch work from host to device.
    - `ttmetal.dispatch`: Dispatch a grid of compute work.
  - `ttkernel`: Tenstorrent kernel library operations.
    - `ttkernel.ffi`: Call a function defined outside of this compilation context.  Usually a hand written piece of C++.
    - `ttkernel.noc_async_read`
    - `ttkernel.noc_async_write`
    - `ttkernel.cb_push_back`
    - `ttkernel.[matmul|add|multiply]`: Computations on tiles in source register space, store the result in dest register space.
    - `ttkernel.sfpu_*`: Computations on tiles in dest register space using sfpu coprocessor.
