#include "fleaux_runtime.hpp"
#include <cassert>
#include <string>
using namespace fleaux::runtime;
int main() {
    // Wrap / Unwrap
    {
        Value tpl = make_tuple(make_int(1), make_int(2), make_int(3));
        assert(as_array(tpl).Size() == 3);
        Value wrapped = tpl | Wrap{};
        assert(as_array(wrapped).Size() == 1);
        Value back = std::move(wrapped) | Unwrap{};
        assert(as_array(back).Size() == 3);
    }
    // ElementAt: dynamic index, works on heterogeneous tuples
    {
        Value tpl = make_tuple(make_int(10), make_string("hello"), make_bool(true));
        Value r0 = make_tuple(tpl, make_int(0)) | ElementAt{};
        assert(to_double(r0) == 10.0);
        Value r1 = make_tuple(tpl, make_int(1)) | ElementAt{};
        assert(as_string(r1) == "hello");
        Value r2 = make_tuple(tpl, make_int(2)) | ElementAt{};
        assert(as_bool(r2) == true);
    }
    // Length
    {
        Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4));
        Value len = make_tuple(seq) | Length{};
        assert(to_double(len) == 4.0);
    }
    // Take / Drop
    {
        Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5));
        Value taken = make_tuple(seq, make_int(3)) | Take{};
        assert(as_array(taken).Size() == 3);
        assert(to_double(array_at(taken, 0)) == 1.0);
        assert(to_double(array_at(taken, 2)) == 3.0);
        Value dropped = make_tuple(seq, make_int(2)) | Drop{};
        assert(as_array(dropped).Size() == 3);
        assert(to_double(array_at(dropped, 0)) == 3.0);
    }
    // Slice: (seq, start, stop)
    {
        Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5));
        Value sliced = make_tuple(seq, make_int(1), make_int(4)) | Slice{};
        assert(as_array(sliced).Size() == 3);
        assert(to_double(array_at(sliced, 0)) == 2.0);
        assert(to_double(array_at(sliced, 2)) == 4.0);
    }
    // Arithmetic
    {
        assert(to_double(make_tuple(make_int(3), make_int(4)) | Add{}) == 7.0);
        assert(to_double(make_tuple(make_int(9), make_int(4)) | Subtract{}) == 5.0);
        assert(to_double(make_tuple(make_int(3), make_int(7)) | Multiply{}) == 21.0);
        assert(to_double(make_tuple(make_float(10.0), make_float(4.0)) | Divide{}) == 2.5);
        assert(to_double(make_tuple(make_int(2), make_int(8)) | Pow{}) == 256.0);
        assert(to_double(make_tuple(make_int(10), make_int(3)) | Mod{}) == 1.0);
        assert(to_double(make_int(5) | UnaryMinus{}) == -5.0);
        assert(as_string(make_tuple(make_string("hello"), make_string(" world")) | Add{}) == "hello world");
    }
    // Comparison / logical
    {
        assert(as_bool(make_tuple(make_int(5), make_int(3)) | GreaterThan{}));
        assert(as_bool(make_tuple(make_int(3), make_int(5)) | LessThan{}));
        assert(as_bool(make_tuple(make_int(3), make_int(3)) | Equal{}));
        assert(!as_bool(make_tuple(make_int(3), make_int(3)) | NotEqual{}));
        assert(!as_bool(make_bool(true) | Not{}));
        assert(as_bool(make_tuple(make_bool(true), make_bool(false)) | Or{}));
        assert(!as_bool(make_tuple(make_bool(true), make_bool(false)) | And{}));
    }
    // Select
    {
        Value r = make_tuple(make_bool(true), make_int(11), make_int(22)) | Select{};
        assert(to_double(r) == 11.0);
    }
    // Loop with Value state: sum 1..5
    {
        auto cf = [](Value state) -> Value {
            return make_bool(to_double(array_at(state, 0)) > 0.0);
        };
        auto sf = [](Value state) -> Value {
            double n   = to_double(array_at(state, 0));
            double acc = to_double(array_at(state, 1));
            return make_tuple(make_int(static_cast<Int>(n - 1)),
                              make_int(static_cast<Int>(acc + n)));
        };
        Value init = make_tuple(make_int(5), make_int(0));
        Value arg = make_tuple(std::move(init), make_callable_ref(cf), make_callable_ref(sf));
        Value final_state = std::move(arg) | Loop{};
        Value result = make_tuple(final_state, make_int(1)) | ElementAt{};
        assert(to_double(result) == 15.0);
    }
    // ToString / Println
    {
        assert(to_string(make_int(42))         == "42");
        assert(to_string(make_bool(true))      == "True");
        assert(to_string(make_string("hello")) == "hello");
        assert(to_string(make_tuple(make_int(1), make_int(2), make_int(3))) == "(1, 2, 3)");
        Value v2 = make_int(7) | Println{};
        assert(to_double(v2) == 7.0);
    }
    return 0;
}
