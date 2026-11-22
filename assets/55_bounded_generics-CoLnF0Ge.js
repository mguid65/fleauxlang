var e=`import Std;

// A generic parameter can carry a bound: '<T: Bound>'. A bound is a type, usually a union,
// and the parameter must be exactly one of its alternatives.
// Std declares the common bounds as aliases:
//   alias Numeric = Int64 | UInt64 | Float64;
//   alias Integer = Int64 | UInt64;
//   alias Signed = Int64 | Float64;
//   alias Comparable = Int64 | UInt64 | Float64 | String | Bool;

let Twice<T: Numeric>(x: T): T = (x, x) -> Std.Add;

21 -> Twice -> Std.Println;           // 42
1.25 -> Twice -> Std.Println;         // 2.5
7u64 -> Twice -> Std.Println;         // 14

// Inside a generic body, T may be passed to anything whose bound allows every alternative of T's bound.
let Clamp<T: Numeric>(x: T, lo: T, hi: T): T = {
  let low: T = ((x, lo) -> <, lo, x) -> Std.Select;
  ((low, hi) -> >, hi, low) -> Std.Select;
};

(15, 0, 10) -> Clamp -> Std.Println;  // 10
(-3, 0, 10) -> Clamp -> Std.Println;  // 0

// Bounds can also be written inline.
let Square<T: Int64 | UInt64>(x: T): T = (x, x) -> Std.Multiply;

12 -> Square -> Std.Println;          // 144

// Integer arithmetic is exact. Plain + - * fail at runtime on overflow; the Wrapping and
// Saturating builtins make the other behaviors explicit.
(9223372036854775807, 1) -> Std.WrappingAdd -> Std.Println;      // -9223372036854775808
(9223372036854775807, 1) -> Std.SaturatingAdd -> Std.Println;    // 9223372036854775807
(1u64, 2u64) -> Std.SaturatingSubtract -> Std.Println;           // 0

// Comparable bounds the sorting builtins.
((3, 1, 2)) -> Std.Tuple.Sort -> Std.Println;                     // (1, 2, 3)
(("pear", "apple")) -> Std.Tuple.Min -> Std.Println;             // apple
`;export{e as default};