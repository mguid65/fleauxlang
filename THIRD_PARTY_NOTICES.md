# Third-Party Notices (Core)

This file records third-party license notices and attributions used by the Fleaux core implementation.

## DataTree

Used as the primary data model for the language. See `core/include/fleaux/runtime/value.hpp`.

- Component path: `third_party/datatree`
- License: Boost Software License 1.0 (BSL-1.0)
- License file: `third_party/datatree/LICENSE`
- Upstream project: https://github.com/mguid65/datatree

## tl::expected

Used as the primary error handling mechanism throughout all stages of the VM.

- Component path: `third_party/tl`
- License: CC0 1.0 Universal
- License file: `third_party/tl/LICENSE`
- Upstream project: https://github.com/TartanLlama/expected

## PCRE2

Used as the standard library regex implementation under the `Std.String` namespace.

- Component sourced via conan
- License: BSD-3-Clause WITH PCRE2-exception
- For complete license terms, see the PCRE2 project license page:
  - https://github.com/PCRE2Project/pcre2/blob/main/LICENCE.md
- Upstream project: https://github.com/PCRE2Project/pcre2

## Catch2

Used as primary unit testing framework.

- Component source via conan
- License: Boost Software License 1.0 (BSL-1.0)
- For complete license terms, see the Catch2 project license page:
  - https://github.com/catchorg/Catch2/blob/devel/LICENSE.txt
- Upstream project: https://github.com/catchorg/Catch2

## Additional Attribution: libc++ monostate basis in DataTree NullType

A small portion of DataTree NullType behavior is based on LLVM libc++ `monostate`.

- Local file: `third_party/datatree/include/data_tree/node_types/detail/value_types/null_type.hpp`
- Referenced upstream file: https://github.com/llvm/llvm-project/blob/main/libcxx/include/__variant/monostate.h
- Referenced upstream notice:
  - Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions
  - SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

For the complete license terms, see the LLVM project license page:
- https://llvm.org/LICENSE.txt
