#!/usr/bin/env python3
"""Make the pinned MSVC-oriented submodules compile with g++.

Idempotent. CMake runs this before it compiles piranha.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
RULES = ROOT / "dependencies/submodules/piranha/include/language_rules.h"

OLD = """        template <typename NativeType>
        std::string getLiteralBuiltinName() const { return ""; }
        template<> std::string getLiteralBuiltinName<piranha::native_bool>() const { return *m_literalRules.lookup(LiteralType::Boolean); }
        template<> std::string getLiteralBuiltinName<piranha::native_string>() const  { return *m_literalRules.lookup(LiteralType::String); }
        template<> std::string getLiteralBuiltinName<piranha::native_int>() const  { return *m_literalRules.lookup(LiteralType::Integer); }
        template<> std::string getLiteralBuiltinName<piranha::native_float>() const { return *m_literalRules.lookup(LiteralType::Float); }
"""

NEW = """        template <typename NativeType>
        std::string getLiteralBuiltinName() const {
            if constexpr (std::is_same<NativeType, native_bool>::value) {
                return *m_literalRules.lookup(LiteralType::Boolean);
            }
            else if constexpr (std::is_same<NativeType, native_string>::value) {
                return *m_literalRules.lookup(LiteralType::String);
            }
            else if constexpr (std::is_same<NativeType, native_int>::value) {
                return *m_literalRules.lookup(LiteralType::Integer);
            }
            else if constexpr (std::is_same<NativeType, native_float>::value) {
                return *m_literalRules.lookup(LiteralType::Float);
            }
            else {
                return "";
            }
        }
"""


VALUE = ROOT / "dependencies/submodules/piranha/include/ir_value_constant.h"

VALUE_OLD = """        template <typename _T>
        const _T validateData(const _T &data, bool recordErrors = false) { return data; }

        template <>
        const piranha::native_string validateData<piranha::native_string>(const piranha::native_string &data, bool recordErrors) {
            piranha::native_string res;
            piranha::native_string::const_iterator it = data.begin();
            while (it != data.end()) {
                char c = *it++;
                if (c == '\\\\' && it != data.end()) {
                    switch (*it++) {
                    case '\\\\':
                        c = '\\\\'; break;
                    case 'n':
                        c = '\\n'; break;
                    case 't': c = '\\t'; break;
                    default:
                        // Invalid escape sequence
                        continue;
                    }
                }
                res += c;
            }

            return res;
        }
"""

VALUE_NEW = """        template <typename _T>
        const _T validateData(const _T &data, bool recordErrors = false) {
            (void)recordErrors;
            if constexpr (std::is_same<_T, native_string>::value) {
                native_string res;
                native_string::const_iterator it = data.begin();
                while (it != data.end()) {
                    char c = *it++;
                    if (c == '\\\\' && it != data.end()) {
                        switch (*it++) {
                        case '\\\\':
                            c = '\\\\'; break;
                        case 'n':
                            c = '\\n'; break;
                        case 't': c = '\\t'; break;
                        default:
                            continue;
                        }
                    }
                    res += c;
                }
                return res;
            }
            else {
                return data;
            }
        }
"""


def patch_rules(text: str) -> tuple[str, bool]:
    changed = False
    if OLD in text:
        text = text.replace(OLD, NEW, 1)
        changed = True
    if "#include <type_traits>" not in text:
        text = text.replace("#include <string>\n", "#include <string>\n#include <type_traits>\n", 1)
        changed = True
    return text, changed


def patch_value(text: str) -> tuple[str, bool]:
    changed = False
    if VALUE_OLD in text:
        text = text.replace(VALUE_OLD, VALUE_NEW, 1)
        changed = True
    if "#include <type_traits>" not in text:
        text = text.replace('#include "memory_tracker.h"\n', '#include "memory_tracker.h"\n\n#include <type_traits>\n', 1)
        changed = True
    return text, changed


def main() -> int:
    if not RULES.exists():
        print("piranha is not checked out", file=sys.stderr)
        return 0
    rules = RULES.read_text()
    rules, rules_changed = patch_rules(rules)
    if rules_changed:
        RULES.write_text(rules)
        print("patched", RULES)
    if VALUE.exists():
        value = VALUE.read_text()
        value, value_changed = patch_value(value)
        if value_changed:
            VALUE.write_text(value)
            print("patched", VALUE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
