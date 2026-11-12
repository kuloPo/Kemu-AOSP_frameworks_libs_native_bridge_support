#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exercise actual generated Vulkan OOM branches with conversion/ABI test doubles.

After building gen_vulkan's output, pass --generated-header vulkan_xml-inl.h,
--vulkan-headers external/vulkan-headers/include and optionally --cxx clang++.
The fixture injects failed argument conversion (including a null converted
create-info) and verifies output bounds, skipped calls and normal/partial-failure
results in both directions. It does not execute a guest instruction set.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


# command, create-info argument, output argument, scalar count argument (-1: nested)
CASES = [
    ('vkCreateGraphicsPipelines', 3, 5, 2),
    ('vkCreateComputePipelines', 3, 5, 2),
    ('vkCreateRayTracingPipelinesKHR', 4, 6, 3),
    ('vkCreateRayTracingPipelinesNV', 3, 5, 2),
    ('vkCreateExecutionGraphPipelinesAMDX', 3, 5, 2),
    ('vkCreateShadersEXT', 2, 4, 1),
    ('vkCreateSharedSwapchainsKHR', 2, 4, 1),
    ('vkAllocateDescriptorSets', 1, 2, -1),
    ('vkAllocateCommandBuffers', 1, 2, -1),
]


def function(source, name):
    start = source.index('void ' + name + '(')
    end = source.index('\n}\n', start) + 3
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated-header', type=Path, required=True)
    parser.add_argument('--vulkan-headers', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    generated = args.generated_header.read_text()
    manual = (directory / 'vulkan_trampolines.cc').read_text()
    program = (directory / 'vulkan_oom_test_fixture.h').read_text()
    for name, _, _, _ in CASES:
        source = manual if name == 'vkAllocateCommandBuffers' else generated
        program += function(source, 'DoCustomTrampolineWithThunk_' + name)
        program += function(generated, 'RunGuest_' + name)
    program += '\nint main() {\n'
    for name, info, output, count in CASES:
        program += (f'  TestCommand<PFN_{name}, {info}, {output}, {count}>("{name}", '
                    f'DoCustomTrampolineWithThunk_{name}, RunGuest_{name});\n')
    program += '}\n'
    with tempfile.TemporaryDirectory(prefix='vulkan-oom-') as tmp:
        source = Path(tmp) / 'test.cc'
        executable = Path(tmp) / 'test'
        source.write_text(program)
        command = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(args.vulkan_headers.resolve()), str(source), '-o', str(executable)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
