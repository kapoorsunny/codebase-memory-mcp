/*
 * repro_issue56.c — Reproduce-first case for OPEN bug #56.
 *
 * Bug #56: "Cross-crate call graphs stop at boundaries" (Rust)
 *
 * ROOT CAUSE (pipeline / Rust LSP path):
 *   The tree-sitter-only Rust extractor has no access to Cargo metadata
 *   at extraction time, so when it sees `crate_a::helper()` inside
 *   crate_b, it records a raw call-site for the path but has no registry
 *   entry for `crate_a::helper` — only the definitions in the *same file*
 *   were seeded.  The LSP resolver therefore cannot match the call-site to
 *   a callee QN across the crate boundary, and the resulting
 *   CBMResolvedCall is either absent or marked with low confidence and
 *   discarded.  When the pipeline writes graph edges for this project, no
 *   CALLS edge is minted for the cross-crate call — the call graph stops
 *   at the crate edge.
 *
 *   v0.8.1 added a hybrid-LSP Rust path that "materially improves" this
 *   (issue comment, maintainer 2026-06-25), but the reporter was asked to
 *   retest; the issue remains OPEN because no retest confirming resolution
 *   was provided.  The workspace-member wiring test
 *   (rustlsp_extra_cargo_wires_workspace_member in test_rust_lsp.c) only
 *   exercises the *single-file LSP* layer with a manually-parsed manifest;
 *   it does NOT verify that the full production pipeline (rh_index_files →
 *   cbm_pipeline → graph store) persists a cross-crate CALLS edge for a
 *   real multi-file Cargo workspace fixture.  That gap is what this test
 *   fills.
 *
 * FIXTURE:
 *   A minimal Cargo workspace with two crates:
 *
 *   [workspace Cargo.toml]           — workspace root, declares members
 *   crate_a/Cargo.toml               — library crate "crate_a"
 *   crate_a/src/lib.rs               — exposes `pub fn helper() {}`
 *   crate_b/Cargo.toml               — binary crate "crate_b", depends on crate_a
 *   crate_b/src/main.rs              — calls `crate_a::helper()` from `fn run()`
 *
 *   The ONLY possible cross-file CALLS edge in this fixture is:
 *     crate_b::run  →  crate_a::helper
 *   (there are no intra-crate calls because `helper` is defined in crate_a
 *   and called from crate_b).
 *
 * EXPECTED (correct) behaviour:
 *   After indexing the workspace through the production MCP pipeline, the
 *   graph store must contain at least one CALLS edge whose callee QN
 *   resolves into the `crate_a` namespace.  Specifically:
 *     rh_count_edges(store, project, "CALLS") >= 1
 *   and the single edge present crosses the crate boundary
 *   (crate_b caller → crate_a callee).
 *
 * ACTUAL (buggy) behaviour:
 *   The pipeline extracts both files, but the cross-crate path
 *   `crate_a::helper` in crate_b/src/main.rs is not resolved to a graph
 *   node in crate_a because Cargo workspace member metadata is not
 *   plumbed into the per-file extraction phase.  Result: zero CALLS edges
 *   in the store.  The ASSERT_GTE(calls, 1) below FAILS → RED.
 *
 * WHY THIS IS RED ON CURRENT CODE (even post-v0.8.1):
 *   The rustlsp_extra_cargo_wires_workspace_member unit test exercises only
 *   the LSP layer (cbm_run_rust_lsp_with_manifest called with a parsed
 *   CBMCargoManifest) and confirms the resolver *can* route
 *   `engine::boot()` to `engine.boot` when given the manifest explicitly.
 *   BUT: the production pipeline's per-file extraction path
 *   (cbm_extract_file → cbm_run_rust_lsp) does NOT receive a pre-parsed
 *   workspace manifest — it only gets the individual file's content.
 *   Cross-crate definition seeding (registering crate_a's functions as
 *   project-level defs so crate_b's calls can match them) requires the
 *   pipeline to:
 *     1. parse the workspace Cargo.toml before per-file extraction,
 *     2. index all member crates together to build a cross-crate def
 *        registry, and
 *     3. pass that combined registry into each per-file cbm_extract_file
 *        call (or run a post-extraction link pass).
 *   Steps 2–3 are not implemented in the production pipeline as of v0.8.1;
 *   the LSP manifests are only used in the unit-test call path.  Therefore
 *   a real workspace indexed through index_repository produces 0 CALLS
 *   edges for cross-crate calls, and this test remains RED.
 *
 * UNCERTAINTY:
 *   If v0.8.1's hybrid-LSP path already plumbs the workspace manifest into
 *   the production pipeline, this test may go GREEN unexpectedly — which
 *   would be the correct outcome (bug fixed).  The test is intentionally
 *   written to go GREEN when the fix lands, so it doubles as the permanent
 *   regression guard.
 */

#include "test_framework.h"
#include "repro_harness.h"

#include <string.h>

/* ── Test ───────────────────────────────────────────────────────────────── */

/*
 * repro_issue56_cross_crate_calls
 *
 * Index a minimal two-crate Cargo workspace through the production
 * rh_index_files pipeline and assert that the cross-crate CALLS edge
 * (crate_b::run → crate_a::helper) is present in the graph store.
 *
 * RED condition:
 *   rh_count_edges(store, project, "CALLS") == 0  (no edge was minted).
 *
 * This test is RED on current code because the production pipeline does
 * not plumb workspace-member metadata into per-file Rust extraction, so
 * cross-crate call resolution always drops to zero edges.
 */
TEST(repro_issue56_cross_crate_calls) {
    /*
     * Workspace root Cargo.toml — declares two members so the pipeline
     * (and any cargo-metadata-aware path) can discover the crate layout.
     */
    static const char workspace_toml[] =
        "[workspace]\n"
        "members = [\"crate_a\", \"crate_b\"]\n"
        "resolver = \"2\"\n";

    /*
     * crate_a: a library crate that exposes a single public function.
     * Path: crate_a/Cargo.toml
     */
    static const char crate_a_toml[] =
        "[package]\n"
        "name    = \"crate_a\"\n"
        "version = \"0.1.0\"\n"
        "edition = \"2021\"\n";

    /*
     * crate_a/src/lib.rs — the callee lives here.
     * There are NO calls inside this file, so any CALLS edge in the graph
     * MUST cross the crate boundary.
     */
    static const char crate_a_lib_rs[] =
        "/// A simple helper function exposed by crate_a.\n"
        "pub fn helper() {\n"
        "    // intentionally empty — we just need the definition\n"
        "}\n";

    /*
     * crate_b: a binary crate that depends on crate_a.
     * Path: crate_b/Cargo.toml
     */
    static const char crate_b_toml[] =
        "[package]\n"
        "name    = \"crate_b\"\n"
        "version = \"0.1.0\"\n"
        "edition = \"2021\"\n"
        "\n"
        "[dependencies]\n"
        "crate_a = { path = \"../crate_a\" }\n";

    /*
     * crate_b/src/main.rs — the caller.
     * `run()` calls `crate_a::helper()` across the crate boundary.
     * This is the only call-site in the entire fixture.
     */
    static const char crate_b_main_rs[] =
        "fn run() {\n"
        "    crate_a::helper();\n"
        "}\n"
        "\n"
        "fn main() {\n"
        "    run();\n"
        "}\n";

    static const RFile files[] = {
        { "Cargo.toml",           workspace_toml  },
        { "crate_a/Cargo.toml",   crate_a_toml    },
        { "crate_a/src/lib.rs",   crate_a_lib_rs  },
        { "crate_b/Cargo.toml",   crate_b_toml    },
        { "crate_b/src/main.rs",  crate_b_main_rs },
    };
    static const int nfiles = (int)(sizeof(files) / sizeof(files[0]));

    RProj lp;
    cbm_store_t *store = rh_index_files(&lp, files, nfiles);
    ASSERT_NOT_NULL(store);

    /*
     * Count all CALLS edges in the project graph.
     *
     * The fixture is constructed so that the ONLY possible CALLS edge is
     * the cross-crate one:
     *
     *   crate_b::run        → crate_a::helper    (cross-crate, the bug)
     *   crate_b::main       → crate_b::run       (intra-crate, should also
     *                                              resolve, but is secondary)
     *
     * On FIXED code:  calls >= 1  (at minimum the cross-crate edge exists)
     * On BUGGY code:  calls == 0  (cross-crate resolution fails; the
     *                              intra-crate main→run call may or may not
     *                              resolve depending on whether single-file
     *                              intra-crate calls work — see note below)
     *
     * NOTE on the intra-crate main→run edge:
     *   `main` and `run` live in the same file (crate_b/src/main.rs).
     *   Single-file intra-crate calls ARE expected to work even on buggy
     *   code (they don't need cross-crate metadata).  If the intra-crate
     *   edge is minted, the test would pass trivially without proving the
     *   cross-crate fix.  To make the assertion specifically about the
     *   cross-crate case, we assert calls >= 2 — requiring BOTH the
     *   intra-crate main→run AND the cross-crate run→crate_a::helper edge.
     *   If only the intra-crate edge works, calls == 1, and this assertion
     *   still FAILS (RED), which is the correct signal: cross-crate
     *   resolution is still missing.
     *
     *   Rationale for the ">= 2" threshold:
     *     calls == 0 → both intra- and cross-crate broken (very buggy)
     *     calls == 1 → only intra-crate works; cross-crate still broken (RED)
     *     calls >= 2 → both edges present; bug is fixed (GREEN)
     */
    int calls = rh_count_edges(store, lp.project, "CALLS");

    /*
     * PRIMARY ASSERTION — RED if cross-crate CALLS edge is absent.
     *
     * On current (buggy) code: cross-crate resolution drops the
     * crate_a::helper call, so `calls` is 0 or 1 (at most the intra-crate
     * main→run edge).  Either way, this ASSERT_GTE fails → RED.
     *
     * On fixed code: both edges are minted → calls >= 2 → GREEN.
     */
    ASSERT_GTE(calls, 2);

    rh_cleanup(&lp, store);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */
SUITE(repro_issue56) {
    RUN_TEST(repro_issue56_cross_crate_calls);
}
