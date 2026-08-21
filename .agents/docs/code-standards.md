# Code Standards

## Style

- **Indentation**: 2 spaces. **Line width**: 100 characters. `.clang-format`
  carries both for editors; nothing enforces them in CI
- **KISS**: the simplest thing that does the job. On a non-trivial change, stop
  and ask whether there is a shape that makes whole branches disappear - prefer
  deleting complexity to rearranging it
- **Clarity over cleverness**: clear names and obvious control flow. If a block
  needs a comment to be understood, first try to rewrite it so it does not
- **Guard clauses over nested conditions**: return early on the edge case

## C++

- Plain structs and free functions. No classes, no inheritance, no exceptions -
  this is a render loop on a microcontroller
- `static` at file scope for anything not in a header. An anonymous namespace
  for helpers that only the file it lives in should ever call
- `float`, never `double`. The S3 has a single-precision FPU and a double costs
  a software emulation in the middle of a per-pixel loop
- Beware names the standard library also has. `lerp` is in `<cmath>` from C++20
  and a helper of the same name is ambiguous rather than shadowing
- Hot loops hoist their trig. Anything that does not vary per pixel is computed
  once a frame into a prepared struct - there are 129,600 pixels

## Comments

Default to none - names and small functions are the documentation.

- Explain **why**, never **what**. Delete anything that restates the code
- The bar is consequence: comment when leaving it out would let someone break or
  misuse the thing. A constraint invisible in the code is worth a line
- Keep it to a line or two. Longer means the code should be clearer, or the
  reasoning belongs in the commit message

Most of the comments here are one of three things: a physical limit that is not
in the code (an LCD cannot switch a pixel off), a failure mode that presents as
something unrelated (a DMA race looking like a broken panel), or a decision that
looks arbitrary until you know what it prevents (why the backlight comes on
after the first flush and not before).

## Testing

There is no test runner. Nothing here can be proven without the panel, and the
panel cannot be read back - so the self-test is the harness.

`npm run firmware:flash:test` builds the `selftest` env, which draws shapes with
no renderer behind them. That is what separates a fault in the panel from a
fault in what is being drawn, and on this board it has been each of them in
turn. When something looks wrong, reach for it before theorising:

- a solid fill that is not solid is the panel or its init
- a solid fill that is clean, with the face still wrong, is the renderer
- colours in the wrong order is the byte swap
- a picture that will not hold still is the flush racing its own DMA

Arithmetic that can be checked without hardware - a distance function, a blend -
is worth compiling on the host and printing, which costs a minute and has caught
more than reasoning about it did.

## Change Discipline

- Every changed line traces to the request. No drive-by fixes
- Remove orphans **your** change creates
- Match the surrounding style even where you would do it differently
