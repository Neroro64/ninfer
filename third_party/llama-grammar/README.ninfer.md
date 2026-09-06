# llama-grammar (vendored)

Vendored from [llama.cpp](https://github.com/ggml-org/llama.cpp) (MIT, see `LICENSE`) to provide
constrained decoding for NInfer structured output. Vendored files:

- `llama-grammar.{h,cpp}` from `src/llama-grammar.{h,cpp}` — GBNF parser and grammar pushdown
  automaton with per-step candidate rejection.
- `json-schema-to-grammar.{h,cpp}` from `common/json-schema-to-grammar.{h,cpp}` — JSON Schema to
  GBNF conversion, including `$ref`/`$defs` resolution, `anyOf`/`oneOf`/`allOf`, `prefixItems`,
  string length, integer bounds, formats, and anchored-regex `pattern` conversion.

## Adaptations from upstream

- `llama_vocab` is replaced by the abstract `llama_grammar_vocab` view
  (`token_to_piece`, `is_eog`, `tokenize`, `n_vocab`); NInfer bridges it to the family tokenizer
  in `src/ops/grammar`.
- `llama_token_data_array` sampling integration is replaced by
  `llama_grammar_rejected_tokens()`, which returns the rejected vocabulary ids for the current
  grammar state.
- The lazy/trigger machinery (`llama_grammar_trigger_pattern`, `trigger_tokens`, lazy init
  parameters) is removed; NInfer grammars constrain from the first constrained token.
- The debug printers (`llama_grammar_print`, `print_rule*`, `gbnf_format_literal`) and the
  `llama_grammar_get_rules`/`llama_grammar_get_stacks` test hooks are removed.
- `common_json` is replaced by `nlohmann::json` (NInfer's vendored copy under `third_party/nlohmann`).
  Local `string_repeat`/`string_split`/`string_join` helpers replace `common/common.h`.
- `json_schema_to_grammar` returns `json_schema_grammar` (grammar text plus degradation
  warnings) instead of printing warnings to stderr; `common_schema_info` and `build_grammar` are
  removed. Remote `https://` `$ref` targets are not fetched and fail conversion.
- `GGML_ASSERT` maps to `assert`, `GGML_ABORT` and upstream log-and-return-null failures throw
  `std::runtime_error`/`std::logic_error`; grammar parse failures are reported through
  `llama_grammar_parser::error`.
- `llama_grammar_accept_impl` refuses end-of-generation tokens with `std::runtime_error` while
  the grammar is unsatisfied or has a pending partial UTF-8 sequence (upstream aborted).
  `llama_grammar_is_satisfied` additionally requires that no partial UTF-8 sequence is pending.
- New NInfer entry points on the vendored core: `llama_grammar_clone_impl` (deep copy of
  compiled rules plus pushdown state in O(rules + stack elements), used for request-state
  snapshots), `llama_grammar_token_accepted` (non-mutating single-token probe), and
  `llama_grammar_accept_str`/`llama_grammar_accept_token` taking `std::string_view` pieces
  (no per-token piece copies). Accepting a token or byte string that no stack survives throws
  and leaves the grammar dead (empty stack set).

## NInfer strictness policy (JSON Schema)

A schema is compiled exactly or rejected: the converter never silently drops an applicable
assertion, because a dropped assertion broadens the accepted language and weakens the
constraint the caller asked for. Conversion throws `std::invalid_argument` for:

- Unsupported keywords: `not`, `if`/`then`/`else`, `unevaluatedProperties`, `unevaluatedItems`,
  `patternProperties`, `propertyNames`, `dependentSchemas`, `dependentRequired`,
  `dependencies`, `contains`, `minContains`, `maxContains`, `uniqueItems`, `minProperties`,
  `maxProperties`, and `multipleOf` with a value other than 1.
- Overlapping `oneOf` alternatives. Exact oneOf (exactly-one) semantics cannot be expressed by
  a pushdown automaton, so a union is emitted only when the alternatives are provably disjoint
  (disjoint static type sets or disjoint const/enum literal sets). Overlapping alternatives are
  rejected with the offending pair named; `anyOf` remains a plain union.
- Sibling assertions next to `const`/`enum`, `oneOf`/`anyOf`, or `allOf` that the corresponding
  branch would ignore, and `allOf` component keys that cannot be merged (`$ref`, `properties`,
  `required`, `enum`, `anyOf`, `"type": "object"`, and annotations are the supported set).
- Numeric bounds without an explicit `"type": "integer"` (draft-4 boolean
  `exclusiveMinimum`/`exclusiveMaximum` rejected too), or on `"type": "number"`.
- `minLength`/`maxLength` without an explicit `"type": "string"`; `minItems`/`maxItems` without
  an explicit `"type": "array"` (generic length-bounded arrays without `items` are compiled to
  repetitions of the unconstrained value primitive; bounds on tuple-form items are rejected).
- Structural keywords on a type they cannot apply to (`properties` on a string schema, `items`
  on an integer schema, `pattern` on an object schema, ...): these surface author bugs instead
  of compiling the bare primitive. Non-applicable annotations (e.g. `format` next to an
  integer) remain spec-faithful no-ops.
- `required` property names without a `properties` entry, empty `enum`s, unknown string
  `format`s (in string context), const/enum values that contradict the declared `type`, and
  `additionalItems` other than `false` alongside tuple-form items.

The NInfer-level API is `ninfer::ops::Grammar` (`include/ninfer/ops/grammar.h`):
`Grammar::schema_to_gbnf(schema_json, root_name = "")` and `Grammar::validate_gbnf(text)`
compile and validate without a vocabulary (used to reject unsupported request schemas before
enqueue); `Grammar::json_schema`/`gbnf` compile against a `GrammarTokenTable`; instances are
copyable (each copy re-copies the compiled rules and remaps its stack elements — O(rules + stack
elements) snapshots; the vocab view is shared) with `clone()`,
`accept_bytes`, `accept_token`, `would_accept`, `rejected_tokens`, `satisfied`, and
`allowed_mask`. Passing a non-empty `root_name` namespaces every schema-derived rule so several
converted schemas can be merged into one grammar text.

Determinism note: the converter keeps rules in insertion order inside `std::map` and compares
const/enum literals by `nlohmann::json::dump()` (ordered JSON object keys), so the emitted GBNF
and the oneOf disjointness decisions are stable across runs and platforms.

Update this file together with any future re-sync from upstream.
