// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0
#include "tt/runtime/runtime.h"
#include "hostdevcommon/common_values.hpp"
#include "tt/runtime/detail/ttnn.h"
#include "tt/runtime/utils.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#include "ttnn/multi_device.hpp"
#pragma clang diagnostic pop

#include "ttmlir/Target/TTNN/Target.h"
#include "ttmlir/Version.h"
#include <numeric>

namespace tt::runtime::ttnn {

static ::tt::target::Arch toFlatbuffer(::tt::ARCH arch) {
  switch (arch) {
  case ::tt::ARCH::GRAYSKULL:
    return ::tt::target::Arch::Grayskull;
  case ::tt::ARCH::WORMHOLE_B0:
    return ::tt::target::Arch::Wormhole_b0;
  case ::tt::ARCH::BLACKHOLE:
    return ::tt::target::Arch::Blackhole;
  default:
    break;
  }

  throw std::runtime_error("Unsupported arch");
}

static ::tt::target::Dim2d toFlatbuffer(CoreCoord coreCoord) {
  return ::tt::target::Dim2d(coreCoord.y, coreCoord.x);
}

static std::vector<::tt::target::ChipChannel>
getAllDeviceConnections(const vector<::ttnn::Device *> &devices) {
  std::set<std::pair<uint32_t, uint32_t>> connectionSet;

  auto addConnection = [&connectionSet](uint32_t firstChip,
                                        uint32_t secondChip) {
    if (firstChip > secondChip)
      std::swap(firstChip, secondChip);
    connectionSet.emplace(firstChip, secondChip);
  };

  for (const ::ttnn::Device *device : devices) {
    std::unordered_set<chip_id_t> connectedChips =
        device->get_ethernet_connected_device_ids();

    for (const uint32_t connection : connectedChips) {
      addConnection(device->id(), connection);
    }
  }

  std::vector<::tt::target::ChipChannel> allConnections;
  allConnections.resize(connectionSet.size());

  std::transform(
      connectionSet.begin(), connectionSet.end(), allConnections.begin(),
      [](const std::pair<uint32_t, uint32_t> &connection) {
        return ::tt::target::ChipChannel(connection.first, connection.second);
      });

  return allConnections;
}

static std::unique_ptr<std::pair<SystemDesc, DeviceIds>>
getCurrentSystemDescImpl(const ::ttnn::multi_device::DeviceMesh &deviceMesh) {
  std::vector<::ttnn::Device *> devices = deviceMesh.get_devices();
  std::sort(devices.begin(), devices.end(),
            [](const ::ttnn::Device *a, const ::ttnn::Device *b) {
              return a->id() < b->id();
            });

  std::vector<int> chipIds;
  std::vector<::flatbuffers::Offset<tt::target::ChipDesc>> chipDescs;
  std::vector<uint32_t> chipDescIndices;
  std::vector<::tt::target::ChipCapability> chipCapabilities;
  std::vector<::tt::target::ChipCoord> chipCoords;
  ::flatbuffers::FlatBufferBuilder fbb;

  for (const ::ttnn::Device *device : devices) {
    chipIds.push_back(device->id());
    // Construct chip descriptor
    ::tt::target::Dim2d deviceGrid = toFlatbuffer(device->logical_grid_size());
    chipDescs.push_back(::tt::target::CreateChipDesc(
        fbb, toFlatbuffer(device->arch()), &deviceGrid,
        device->l1_size_per_core(), device->num_dram_channels(),
        device->dram_size_per_channel(), L1_ALIGNMENT, PCIE_ALIGNMENT,
        DRAM_ALIGNMENT));
    chipDescIndices.push_back(device->id());
    // Derive chip capability
    ::tt::target::ChipCapability chipCapability =
        ::tt::target::ChipCapability::NONE;
    if (device->is_mmio_capable()) {
      chipCapability = chipCapability | ::tt::target::ChipCapability::PCIE |
                       ::tt::target::ChipCapability::HostMMIO;
    }
    chipCapabilities.push_back(chipCapability);
    // Extract chip location
    int x, y, rack, shelf;
    std::tie(x, y, rack, shelf) =
        ::tt::Cluster::instance().get_chip_location(device->id());
    chipCoords.emplace_back(rack, shelf, y, x);
  }
  // Extract chip connected channels
  std::vector<::tt::target::ChipChannel> allConnections =
      getAllDeviceConnections(devices);
  // Create SystemDesc
  auto systemDesc = ::tt::target::CreateSystemDescDirect(
      fbb, &chipDescs, &chipDescIndices, &chipCapabilities, &chipCoords,
      &allConnections);
  ::ttmlir::Version ttmlirVersion = ::ttmlir::getVersion();
  ::tt::target::Version version(ttmlirVersion.major, ttmlirVersion.minor,
                                ttmlirVersion.patch);
  auto root = ::tt::target::CreateSystemDescRootDirect(
      fbb, &version, ::ttmlir::getGitHash(), "unknown", systemDesc);
  ::tt::target::FinishSizePrefixedSystemDescRootBuffer(fbb, root);
  ::flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  if (not ::tt::target::VerifySizePrefixedSystemDescRootBuffer(verifier)) {
    throw std::runtime_error("Failed to verify system desc root buffer");
  }
  uint8_t *buf = fbb.GetBufferPointer();
  auto size = fbb.GetSize();
  auto handle = utils::malloc_shared(size);
  std::memcpy(handle.get(), buf, size);
  return std::make_unique<std::pair<SystemDesc, DeviceIds>>(SystemDesc(handle),
                                                            chipIds);
}

std::pair<SystemDesc, DeviceIds> getCurrentSystemDesc() {
  size_t numDevices = ::tt::tt_metal::GetNumAvailableDevices();
  size_t numPciDevices = ::tt::tt_metal::GetNumPCIeDevices();
  TT_FATAL(numDevices % numPciDevices == 0,
           "Unexpected non-rectangular grid of devices");
  std::vector<chip_id_t> deviceIds(numDevices);
  std::iota(deviceIds.begin(), deviceIds.end(), 0);
  ::ttnn::multi_device::DeviceGrid deviceGrid(numDevices / numPciDevices,
                                              numPciDevices);
  ::ttnn::multi_device::DeviceMesh deviceMesh =
      ::ttnn::multi_device::open_device_mesh(deviceGrid, deviceIds,
                                             DEFAULT_L1_SMALL_SIZE);
  std::exception_ptr eptr = nullptr;
  std::unique_ptr<std::pair<SystemDesc, DeviceIds>> desc;
  try {
    desc = getCurrentSystemDescImpl(deviceMesh);
  } catch (...) {
    eptr = std::current_exception();
  }
  deviceMesh.close_devices();
  if (eptr) {
    std::rethrow_exception(eptr);
  }
  return *desc;
}

template <typename T>
static BorrowedStorage createStorage(void *ptr, std::uint32_t numElements) {
  return BorrowedStorage(
      borrowed_buffer::Buffer<T>(static_cast<T *>(ptr), numElements), [] {},
      [] {});
}

static BorrowedStorage createStorage(void *ptr, std::uint32_t numElements,
                                     ::tt::target::DataType dataType) {
  switch (dataType) {
  case ::tt::target::DataType::Float32:
    return createStorage<float>(ptr, numElements);
  // case ::tt::target::DataType::Float16:
  //   return createStorage<float16>(ptr, numElements);
  case ::tt::target::DataType::BFloat16:
    return createStorage<bfloat16>(ptr, numElements);
  case ::tt::target::DataType::UInt32:
    return createStorage<std::uint32_t>(ptr, numElements);
  case ::tt::target::DataType::UInt16:
    return createStorage<std::uint16_t>(ptr, numElements);
  // case ::tt::target::DataType::UInt8:
  //   return createStorage<std::uint8_t>(ptr, numElements);
  default:
    throw std::runtime_error("Unsupported data type");
  }
}

static ::ttnn::DataType toTTNNDataType(::tt::target::DataType dataType) {
  switch (dataType) {
  case ::tt::target::DataType::Float32:
    return ::ttnn::DataType::FLOAT32;
  // case ::tt::target::DataType::Float16:
  //   return ::ttnn::DataType::FLOAT16;
  case ::tt::target::DataType::BFloat16:
    return ::ttnn::DataType::BFLOAT16;
  case ::tt::target::DataType::UInt32:
    return ::ttnn::DataType::UINT32;
  case ::tt::target::DataType::UInt16:
    return ::ttnn::DataType::UINT16;
  // case ::tt::target::DataType::UInt8:
  //   return ::ttnn::DataType::UINT8;
  default:
    throw std::runtime_error("Unsupported data type");
  }
}

Tensor createTensor(std::shared_ptr<void> data,
                    std::vector<std::uint32_t> const &shape,
                    std::vector<std::uint32_t> const &stride,
                    std::uint32_t itemsize, ::tt::target::DataType dataType) {
  std::uint32_t numElements = shape[0] * stride[0];
  auto tensor = std::make_shared<::ttnn::Tensor>(
      createStorage(data.get(), numElements, dataType), shape,
      toTTNNDataType(dataType), ::ttnn::Layout::ROW_MAJOR);
  return Tensor(tensor, data);
}

Device openDevice(std::vector<int> deviceIds) {
  assert(deviceIds.size() == 1 && "Only one device is supported for now");
  auto &device = ::ttnn::open_device(deviceIds.front());
  return Device::borrow(device);
}

void closeDevice(Device device) {
  auto &ttnn_device = device.as<::ttnn::Device>();
  ::ttnn::close_device(ttnn_device);
}

static ::tt::target::ttnn::TTNNBinary const *getBinary(Flatbuffer binary) {
  bool isTTNN = ::tt::target::ttnn::SizePrefixedTTNNBinaryBufferHasIdentifier(
      binary.handle.get());
  if (not isTTNN) {
    throw std::runtime_error("Unsupported binary format");
  }
  return ::tt::target::ttnn::GetSizePrefixedTTNNBinary(binary.handle.get());
}

Event submit(Device deviceHandle, Binary executableHandle,
             std::uint32_t programIndex,
             std::vector<Tensor> const &inputHandles,
             std::vector<Tensor> const &outputHandles) {
  ::ttnn::Device &device = deviceHandle.as<::ttnn::Device>();
  ::tt::target::ttnn::TTNNBinary const &fbb = *getBinary(executableHandle);
  std::vector<::ttnn::Tensor *> inputs;
  inputs.reserve(inputHandles.size());
  for (auto &input : inputHandles) {
    inputs.push_back(static_cast<::ttnn::Tensor *>(input.handle.get()));
  }
  std::vector<::ttnn::Tensor *> outputs;
  outputs.reserve(outputHandles.size());
  for (auto &output : outputHandles) {
    outputs.push_back(static_cast<::ttnn::Tensor *>(output.handle.get()));
  }
  tt::runtime::ttnn::runProgram(device, fbb.programs()->Get(programIndex),
                                inputs, outputs);
  return Event(nullptr);
}

void wait(Event) { throw std::runtime_error("Not implemented"); }

} // namespace tt::runtime::ttnn
