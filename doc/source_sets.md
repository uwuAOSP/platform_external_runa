<!--
SPDX-FileCopyrightText: The uwuAOSP Project
SPDX-License-Identifier: Apache-2.0
-->

# Runtime source sets in Meow

The objective is to discover added/deleted source files without asking Soong to
regenerate its Ninja manifest. This is not an arbitrary graph-mutation system.

## Execution path

1. Soong emits the Ninja graph and source-set declarations on relevant edges.
2. The default Meow invocation enables dynamic source sets in Soong's normal
   make mode; it does not need a separate scheduler state file.
3. Soong starts the main Runa invocation with `--source-globs`. The bootstrap
   executor still runs the normal Soong regeneration checks.
4. Runa parses the existing manifest once. Before publishing graph adjacency it
   expands source sets, instantiates missing compile actions, removes obsolete
   producers, and updates consumers (including archive response-file inputs).
5. Normal Ninja dependency checking and execution proceed on the resulting graph.

There is no scheduler-side manifest traversal, GD1 generation, or GD1 refresh in
this path. Existing `.graph-delta` files and GD1-era extra JSON fields are not used.
The standalone generic GD1 interface is separate and must not be mixed with
runtime source sets.

## Compatibility and limitations

The implementation consumes the existing `runa_source_globs` (G1),
`runa_dynamic_cpp` (C1), and `runa_dynamic_aapt_*` (A1) declarations emitted by
Soong. Keeping these names avoids requiring a new on-disk protocol or rewriting
existing graphs just to change executors. The template/mapping logic is retained
for compatibility; it is not a claim that all arbitrary source kinds are supported.

- Whole-input collections support Java/Kotlin and similar existing consumers.
  Soong omits paths owned by these globs from the serialized edge. The G1
  declaration contains the patterns and excludes, not the previous members.
- Source/object groups require a compatible, already-emitted compile template
  for each source extension and a derivable output path. Auxiliary outputs must
  share that template's output stem.
- Complete AAPT2 resource sets used by Meow can use a stable resource archive.
  Runa supplies the live input list to the archive action, and AAPT2 consumes
  the compiled archive rather than dependencies on individual `.flat` files.
  Older declarations can still map and refresh individual `.flat` outputs.
- Initially empty globs cannot supply a compile template or consumer action.
  Soong keeps their ordinary invalidation until an action exists. Unsupported
  mappings fail with an explicit error instead of silently omitting sources.
- Files outside declared source globs, Android.bp changes, configuration globs,
  and globs also used for configuration retain normal Soong invalidation.
- Soong confirms that a glob has an emitted runtime consumer (and, for
  resources, a compile template) before allowing Blueprint to skip it. Merely
  requesting source-glob tracking is not sufficient. Unclaimed globs fall back
  to normal invalidation rather than risking an incomplete build.
- Expansion results are cached only within one manifest load, never across
  invocations. Additions and deletions are re-enumerated on the next run.
- Only in-memory edges are removed. Old generated files may remain on disk, but
  are no longer inputs to consumers or actions in the refreshed graph.

`--source-globs` also serves as an executor capability requirement. An older
Runa/Ninja must reject it rather than silently ignore source-set metadata after
Soong has suppressed reanalysis. Rebuild **and** synchronize the vendor Runa
binary before using the new Soong UI. Do not test new code with a stale binary.

## Verification

Go-side checks (do not build Android or C++):

```sh
cd build/soong
go test -p 1 ./android ./ui/build
cd ../blueprint
go test -race . ./pathtools
```

After separately rebuilding and synchronizing Runa, run the runtime fixtures:

```sh
cd build/soong
MEOW_TEST_RUNA=/absolute/path/to/rebuilt/runa go test ./ui/build -run '^TestRuntime' -v
```

These fixtures add/delete sources and resources, check resulting file contents,
exercise shared consumers, auxiliary outputs and response files, check no-op
rebuilds, and verify manifest content **and** mtime stay unchanged. Without an
explicit `MEOW_TEST_RUNA`, they skip; a passing Go suite alone is not evidence that
the changed native implementation executes correctly.

Product acceptance additionally requires proving no Soong graph-generation
action runs on supported source membership changes (unchanged manifest mtime
alone is insufficient because writes can use content comparison), checking real
output contents, and measuring time/RSS/swap before the first action. Changes to
Android.bp must still regenerate the graph.
