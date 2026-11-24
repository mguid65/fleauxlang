# Third-Party Notices (Core)

This file records third-party license notices and attributions used by the Fleaux core implementation.

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
