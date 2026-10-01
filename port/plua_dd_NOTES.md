# plua_dd — double-double arithmetic for primeLua

A **new** library (`dd`).  Lua's `math` and the number type are untouched: the
differential tests rely on primeLua matching desktop Lua bit for bit, so higher
precision lives in its own namespace.

* `port/plua_dd.h` / `port/plua_dd.c` — the arithmetic and the functions
* `tools/gen_dd_expected.py` — mpmath at 60 digits → `tests/dd_expected.h`
* `tests/test_dd_host.c` — the host test; `make test-dd` regenerates and runs it

## Why double-double

A dd is a pair of doubles `(hi, lo)` meaning `hi + lo`, giving ~106 bits, i.e.
31–32 significant decimal digits at every operation.  On an ARM926 with no FPU
each dd operation costs ~10–20 soft-float doubles, so dd is roughly an order of
magnitude slower than `math.*` — usable, where true arbitrary precision (bignum
plus AGM) would not be.  It is also exactly what a user comparing
`print(math.sin(1))` (14 digits) with Python wants to see more of.

## Measured accuracy

`make test-dd`, 680 function cases + 10 string round trips, all against mpmath
at 60 digits.  Error in **dd-ulp** (1 dd-ulp = 2^-106 ≈ 1.08e-32 relative):

| function | cases | worst | bit-exact | note |
|---|---|---|---|---|
| sin | 70 | 4.05 | 20/70 | includes x up to 1e6 |
| cos | 70 | 2.23 | 23/70 | " |
| tan | 60 | 3.27 | 9/60 | sin/cos then divide |
| asin | 40 | 4.61 | 8/40 | via atan2, accurate at ±1 |
| acos | 40 | 2.39 | 15/40 | " |
| atan | 60 | 1.48 | 30/60 | π/4 reduction, then series |
| atan2 | 30 | 4.31 | 16/30 | |
| exp | 72 | 45.58 | 13/72 | worst for \|x\| > 300, see below |
| log | 72 | 2.66 | 36/72 | frexp + atanh series |
| log10 | 72 | 3.67 | 15/72 | log / ln10 |
| sqrt | 30 | 1.34 | 16/30 | dd Newton |
| pow | 40 | 9.99 | 9/40 | exp(y·log x) |
| hypot | 20 | 1.18 | 5/20 | dd sqrt of the sum of squares |
| π, e, ln2, ln10 | 4 | 0.00 | 4/4 | correctly rounded constants |

* **exp**: the Taylor accumulation costs about one ulp per term (26 terms), so
  large arguments give ~30 correct digits instead of 32.  The ulp limits in the
  test are asserted per function (`exp` 60, `pow` 20, trig 8, the rest 6), so a
  regression fails rather than showing a bigger number in the table.
* **Printing** (`plua_dd_to_string`): 33 significant digits, built by printing
  *both* words at full width and adding the second one's digits into the first
  with carry/borrow propagation.  Printing only 17 digits of `hi` loses its own
  tail — which is exactly where `lo` starts to matter — and that mistake made
  every printed value wrong from the 18th digit on.  The last digit can be off
  by one or two; exact integers print as integers (`dd(2)/dd(4)` → `0.5`,
  `dd(6)/dd(2)` → `3`).
* **Parsing** (`plua_dd_from_string`): digits are accumulated with Horner in
  chunks, so a 40-digit input keeps its value to ~10 dd-ulp (the exact decimal,
  not the double: `dd("0.1")` is the decimal 0.1, which is 5.55e-18 away from
  the double 0.1 — that is the point of the library).  Users should expect
  `dd("0.1") + dd("0.2") == dd("0.3")` to be `false`: each parsed value carries
  a ~1e-32 rounding, so the sum sits ~2e-32 below 0.3.

## Algorithms

Dekker/Knuth error-free transforms (`two_sum`, `quick_two_sum`, `two_prod` with
Dekker's 27-bit split — **no FMA**, ARM926 has none), renormalised add/sub/mul/
div, dd Newton for sqrt, and series with dd range reduction for the rest:
exp reduces by k·ln2 then Taylor; log uses frexp plus an atanh series; sin/cos
reduce modulo π/2 and use Taylor; atan reduces by π/4 and uses its series.

Two things the host test caught that are easy to get wrong:

1. **Argument reduction must be Cody-Waite, word by word.**  Subtracting
   `n·(π/2)` formed as one value needs 128 bits (the product is ~1e5, the last
   word contributes ~1e-28), so the double-double rounds it away: sin lost five
   digits for x ~ 1e6 (7.3e6 dd-ulp).  Subtracting the three words one at a time
   keeps every intermediate O(1).  Same story for `exp` with ln2's two words.
2. **Never divide a dd by an integer component-wise.**  `term.hi /= i; term.lo
   /= i;` rounds both words independently and drops the extra precision; `exp`
   was accurate to only ~1e-21 that way.  `plua_dd_div_d` divides properly.

## Limits worth knowing

* Argument reduction is good to |x| ~ 1e6 for sin/cos/tan (three doubles of
  π/2 = 159 bits); beyond that the reduction degrades like any fixed-precision
  scheme, and the functions document it rather than pretending.
* exp overflows/underflows exactly like a double (`|x| > 709.78` → inf).
* The parser handles decimal and falls back to `strtod` for hex floats (single
  precision for those).
* Host test build flags matter: `-ffp-contract=off` (an x86 host can fuse
  a*b+c into an FMA, the target cannot) and no fast-math.
