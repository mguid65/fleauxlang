# Third-Party Notices (Core)

This file records third-party license notices and attributions used by the Fleaux core implementation.

## DataTree

- Component path: `third_party/datatree`
- License: Boost Software License 1.0 (BSL-1.0)
- License file: `third_party/datatree/LICENSE`
- Upstream project: https://github.com/mguid65/datatree

## tl::expected

- Component path: `third_party/tl`
- License: CC0 1.0 Universal
- License file: `third_party/tl/LICENSE`
- Upstream project: https://github.com/TartanLlama/expected

## Additional Attribution: libc++ monostate basis in DataTree NullType

A small portion of DataTree NullType behavior is based on LLVM libc++ `monostate`.

- Local file: `third_party/datatree/include/data_tree/node_types/detail/value_types/null_type.hpp`
- Referenced upstream file: https://github.com/llvm/llvm-project/blob/main/libcxx/include/__variant/monostate.h
- Referenced upstream notice:
  - Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions
  - SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

For the complete license terms, see the LLVM project license page:
- https://llvm.org/LICENSE.txt

