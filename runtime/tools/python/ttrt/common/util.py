# SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

import ttrt.binary
import os
import json
import importlib.machinery
import sys
import signal
import os
import io
import subprocess
import time
import socket
from pkg_resources import get_distribution
import sys
import shutil

from tt_metal.tools.profiler.common import (
    PROFILER_LOGS_DIR,
    TRACY_FILE_NAME,
    TRACY_OPS_TIMES_FILE_NAME,
    TRACY_OPS_DATA_FILE_NAME,
)

#######################################################################################
######################################**GLOBALS**######################################
#######################################################################################
TT_MLIR_HOME = os.environ.get("TT_MLIR_HOME", f"{os.getcwd()}")
TTMLIR_VENV_DIR = os.environ.get("TTMLIR_VENV_DIR", "/opt/ttmlir-toolchain/venv")

TTRT_ARTIFACTS = f"{TT_MLIR_HOME}/ttrt-artifacts"
TTRT_PERF_ARTIFACTS = f"{TTRT_ARTIFACTS}/ttrt-perf-artifacts"

os.makedirs(PROFILER_LOGS_DIR, exist_ok=True)
TRACY_OUT_FILE = os.path.join(PROFILER_LOGS_DIR, TRACY_FILE_NAME)
TRACY_OPS_FILE = os.path.join(PROFILER_LOGS_DIR, TRACY_OPS_TIMES_FILE_NAME)
TRACY_DATA_FILE = os.path.join(PROFILER_LOGS_DIR, TRACY_OPS_DATA_FILE_NAME)
TRACY_CAPTURE_TOOL = f"{TT_MLIR_HOME}/third_party/tt-metal/src/tt-metal-build/tools/profiler/bin/capture-release"
TRACY_CSVEXPROT_TOOL = f"{TT_MLIR_HOME}/third_party/tt-metal/src/tt-metal-build/tools/profiler/bin/csvexport-release"
TRACY_PARAMS_JSON = f"{TTRT_PERF_ARTIFACTS}/tracy_params.json"

if "LOGGER_LEVEL" not in os.environ:
    os.environ["LOGGER_LEVEL"] = "FATAL"
if "TT_METAL_LOGGER_LEVEL" not in os.environ:
    os.environ["TT_METAL_LOGGER_LEVEL"] = "FATAL"

#######################################################################################
#######################################**UTILS**#######################################
#######################################################################################
def ttrt_cleanup():
    subprocess.run(
        f"rm -rf {TTRT_ARTIFACTS}; mkdir -p {TTRT_ARTIFACTS}; mkdir -p {TTRT_PERF_ARTIFACTS}",
        shell=True,
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def copy_file_into_ttrt_artifact(file_path):
    try:
        if not os.path.isfile(file_path):
            raise FileNotFoundError(f"Source file '{file_path}' does not exist.")

        if not os.path.isdir(TTRT_ARTIFACTS):
            raise NotADirectoryError(
                f"Destination '{TTRT_ARTIFACTS}' is not a directory."
            )

        shutil.copy(file_path, TTRT_ARTIFACTS)
        print(f"File '{file_path}' copied to '{TTRT_ARTIFACTS}' successfully.")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")


def check_file_exists(file_path):
    try:
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"Required file '{file_path}' does not exist.")
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        sys.exit(1)


def system_desc_as_dict(desc):
    return json.loads(desc.as_json())


def check_version(fb_version):
    package_name = "ttrt"
    try:
        package_version = get_distribution(package_name).version
    except Exception as e:
        print(f"Error retrieving version: {e} for {package_name}")

    assert (
        package_version == fb_version
    ), f"{package_name}=v{package_version} does not match flatbuffer=v{fb_version}"


def mlir_sections(fbb):
    d = ttrt.binary.as_dict(fbb)
    for i, program in enumerate(d["programs"]):
        if "debug_info" not in program:
            print("// no debug info found for program:", program["name"])
            continue
        print(
            f"// program[{i}]:",
            program["name"],
            "-",
            program["debug_info"]["mlir"]["name"],
        )
        print(program["debug_info"]["mlir"]["source"], end="")


def cpp_sections(fbb):
    d = ttrt.binary.as_dict(fbb)
    for i, program in enumerate(d["programs"]):
        if "debug_info" not in program:
            print("// no debug info found for program:", program["name"])
            continue
        print(f"// program[{i}]:", program["name"])
        print(program["debug_info"]["cpp"], end="")


def program_inputs(fbb):
    d = ttrt.binary.as_dict(fbb)
    for program in d["programs"]:
        print("program:", program["name"])
        print(json.dumps(program["inputs"], indent=2))


def program_outputs(fbb):
    d = ttrt.binary.as_dict(fbb)
    for program in d["programs"]:
        print("program:", program["name"])
        print(json.dumps(program["outputs"], indent=2))


read_actions = {
    "all": lambda fbb: print(fbb.as_json()),
    "version": lambda fbb: print(
        f"Version: {fbb.version}\ntt-mlir git hash: {fbb.ttmlir_git_hash}"
    ),
    "system-desc": lambda fbb: print(
        json.dumps(ttrt.binary.as_dict(fbb)["system_desc"], indent=2)
    ),
    "mlir": mlir_sections,
    "cpp": cpp_sections,
    "inputs": program_inputs,
    "outputs": program_outputs,
}
