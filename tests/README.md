# Parser tests

Opt-in tests for the JPEG 2000 codestream parser front end. Build with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
```

## `prcl_crp_test` — progression-order (CRP) test

Drives `prepare_precinct_structure()` on a raw codestream and checks the
component/resolution/precinct (CRP) walk that the parser uses as packet identity.

```sh
# Print the built CRP order (one "c r p" triple per line):
build/prcl_crp_test path/to/stream.j2c dump

# Also run the full flush() parse path and assert every precinct parses cleanly
# and fires back in CRP order (exit non-zero on any mismatch/failure):
build/prcl_crp_test path/to/stream.j2c parse
```

Works on any single-tile HTJ2K codestream the parser supports (PCRL or PRCL
progression; a DFS marker is required, as elsewhere in this parser). To validate
`case PRCL:`, run it on a PRCL stream and a PCRL encoding of the same image and
confirm: identical precinct *set* (the CRP order is a pure local permutation), and
an exact match against an independent reference decoder's packet read order.
