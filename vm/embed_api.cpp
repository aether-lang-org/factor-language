// embed_api.cpp — generic C embedding API for libfactor (spike, issue: embeddable Factor)
//
// Goal: let a host C/C++ program (any embedder, not Aether-specific) load
// a Factor VM from a shared library, evaluate a source string, and get
// the result — the "liblua-shaped" embedding surface Factor lacks today.
//
// This first cut proves the core feasibility: a libfactor.so + a C caller
// + a bootstrapped image can init the VM and run an eval from C. It reuses
// Factor's existing `-e=<code>` eval chain (basis/eval) driven through the
// existing startup quotation, rather than inventing a new evaluator.
//
// API (all Factor-internal types hidden behind an opaque handle; nothing
// here is Aether-specific — a map-interop layer can be built ON TOP of a
// later factor_embed_set/get by any embedder):
//
//   factor_embed_eval_oneshot(image, src)  init VM, eval `src`, tear down.
//
// Re-entrant init / eval* / destroy is the next rung; the standalone
// startup quotation calls `exit`, so a persistent handle needs a dedicated
// eval quotation (begin_callback over the `eval` word) — scaffolded but
// not wired in this spike.

#include "master.hpp"

#include <stdlib.h>
#include <string.h>

namespace factor {

// Build an argv of the form {"factor", "-e=<src>"} that the existing
// command-line parser + eval chain understands. Caller frees via
// embed_free_argv.
static char** embed_make_argv(const char* src, int* out_argc) {
  char** argv = (char**)malloc(sizeof(char*) * 2);
  argv[0] = strdup("factor");
  size_t n = strlen(src);
  char* eflag = (char*)malloc(n + 4);  // "-e=" + src + NUL
  memcpy(eflag, "-e=", 3);
  memcpy(eflag + 3, src, n + 1);
  argv[1] = eflag;
  *out_argc = 2;
  return argv;
}

static void embed_free_argv(char** argv, int argc) {
  for (int i = 0; i < argc; i++) free(argv[i]);
  free(argv);
}

// Re-entrant embedding (Rung B). One persistent VM, reused across calls,
// that does NOT take the process over: the embed image's startup quotation
// is `[ boot embed-eval-once ]` (no `exit`), so c_to_factor RETURNS to the
// caller after each eval — unlike start_standalone_factor, whose
// command-line startup quot calls exit() and kills the host.
//
//   - First call: new_factor_vm + init_factor(image), then run startup
//     (which `boot`s and does the first eval), and leave the VM live.
//   - Later calls: just set OBJ_ARGS to the new {src,result} paths and
//     re-run the (already-booted) startup quot via c_to_factor.
//
// The src/result paths are passed via OBJ_ARGS (the same slot argv lands
// in); embed-eval-once reads them with (command-line). Single-threaded:
// one persistent VM guarded by the caller (the bridge serialises calls).
static factor_vm* g_embed_vm = NULL;

// One-time init of the persistent embed VM. The image must have run
// `init-remote-control` at startup (so OBJ_EVAL_CALLBACK holds the
// upstream eval-callback alien); running its startup quot here populates
// that slot and RETURNS (init-remote-control doesn't exit). After this,
// factor_vm::factor_eval_string drives re-entrant evals via the callback.
static int embed_vm_init(const char* image_path) {
  if (g_embed_vm) return 0;
  vm_parameters p;
  vm_char* argv[1];
  argv[0] = (vm_char*)"factor";
  p.init_from_args(1, argv);
  if (image_path && image_path[0])
    p.image_path = safe_strdup(image_path);
  g_embed_vm = new_factor_vm();
  g_embed_vm->init_factor(&p);

  // Initialise the command-line machinery the startup quot reads
  // (the standalone path does this too). Just argv[0]; embedded mode
  // ignores the rest and uses main-vocab = alien.remote-control.
  g_embed_vm->pass_args_to_factor(1, argv);

  // Mark the VM embedded (restores what the removed start_embedded_factor
  // C API did). The image's command-line startup checks `embedded?`:
  //   - main-vocab returns "alien.remote-control", whose MAIN
  //     (init-remote-control) installs the eval>string alien into
  //     OBJ_EVAL_CALLBACK;
  //   - command-line-startup skips its final quit() when embedded, so
  //     control RETURNS here instead of exiting the host.
  g_embed_vm->special_objects[OBJ_EMBEDDED] =
      g_embed_vm->special_objects[OBJ_CANONICAL_TRUE];

  // Run the startup quot: boots, runs alien.remote-control (installs the
  // callback), and returns (no quit, because embedded?). After this,
  // factor_eval_string drives re-entrant evals via OBJ_EVAL_CALLBACK.
  g_embed_vm->c_to_factor_toplevel(
      g_embed_vm->special_objects[OBJ_STARTUP_QUOT]);
  return 0;
}

}  // namespace factor

using namespace factor;

// Run `src` as a `-e=<src>` snippet under a fresh VM (optionally from
// `image_path`), then tear the VM down. Internal helper behind both the
// public oneshot-eval (snippet prints to stdout) and the result-capturing
// eval (snippet writes its result to a file the caller reads back).
static int embed_run_snippet(const char* image_path, const char* src) {
  if (!src) return 1;

  // Build {"factor", ["-i=<image>",] "-e=<src>"}.
  char* a_factor = strdup("factor");
  char* a_eflag;
  {
    size_t n = strlen(src);
    a_eflag = (char*)malloc(n + 4);
    memcpy(a_eflag, "-e=", 3);
    memcpy(a_eflag + 3, src, n + 1);
  }
  char* a_iflag = NULL;
  if (image_path && image_path[0]) {
    size_t n = strlen(image_path);
    a_iflag = (char*)malloc(n + 4);
    memcpy(a_iflag, "-i=", 3);
    memcpy(a_iflag + 3, image_path, n + 1);
  }

  int argc = a_iflag ? 3 : 2;
  char** argv = (char**)malloc(sizeof(char*) * argc);
  int i = 0;
  argv[i++] = a_factor;
  if (a_iflag) argv[i++] = a_iflag;
  argv[i++] = a_eflag;

  // Inits the VM from the args, passes argv to Factor, runs the startup
  // quotation which evaluates -e=<src>.
  start_standalone_factor(argc, argv);

  free(a_factor);
  free(a_eflag);
  if (a_iflag) free(a_iflag);
  free(argv);
  return 0;
}

// One-shot eval (Rung A): evaluate `src` and let Factor print whatever it
// prints to stdout, then tear the VM down. `image_path` may be NULL to use
// the default/embedded image. Returns 0 on success.
extern "C" VM_C_API int factor_embed_eval_oneshot(const char* image_path,
                                                  const char* src) {
  return embed_run_snippet(image_path, src);
}

// Result-capturing eval (Rung B): evaluate `src` as an expression that
// leaves a single value on the data stack, and return that value rendered
// to a string (via Factor's `unparse`). The host gets the result back as a
// value it can use, not just printed.
//
// Mechanism, in the spirit of the one-shot path (no deep VM-quotation
// marshalling): the caller's source and a result path are handed in via
// the filesystem so no Factor-source escaping is needed. The wrapper
// snippet reads the source file, evaluates it with the effect
// ( -- result ), unparses the top value, and writes it to the result file.
// The bridge author drives the file plumbing; here we just take the two
// paths and synthesize the wrapper.
//
//   src_path    : file the caller has written the Factor source into
//   result_path : file this writes the unparsed result into (UTF-8)
//
// Returns 0 on success (result file written). Non-zero if args are bad.
// Whether the eval itself succeeded is observed by the caller reading the
// result file (an eval error leaves it empty / unwritten — the bridge
// treats that as an error). Keeping the value-marshalling in Factor source
// (unparse) rather than across the C boundary is what lets a later
// re-entrant API reuse the exact same wrapper.
extern "C" VM_C_API int factor_embed_eval_to_file(const char* image_path,
                                                  const char* src_path,
                                                  const char* result_path) {
  if (!src_path || !result_path) return 1;

  // One-shot path (works today): synthesize a -e= snippet that reads the
  // source, evals as ( -- result ), unparses, and writes the result file.
  // The result file IS written correctly — but start_standalone_factor
  // exit()s, so an embedding host that needs to READ the result back can't
  // use this directly (it dies inside the call). See embed_reentrant_eval
  // for the WIP that keeps the host alive.
  char snippet[4096];
  int w = snprintf(snippet, sizeof(snippet),
      "\"%s\" utf8 file-contents eval( -- result ) unparse "
      "\"%s\" utf8 set-file-contents",
      src_path, result_path);
  if (w < 0 || (size_t)w >= sizeof(snippet)) return 2;
  return embed_run_snippet(image_path, snippet);
}

// RE-ENTRANT eval-with-result (the real Rung B). Uses Factor's upstream
// embedded-control machinery and a STOCK image — no custom/saved image:
// embed_vm_init marks the VM embedded, so the image's command-line startup
// runs alien.remote-control (installing the eval>string alien into
// OBJ_EVAL_CALLBACK) and returns without quit(). factor_eval_string then
// invokes that callback (begin/end_callback under the hood) and RETURNS —
// the host process survives, unlike start_standalone_factor which exit()s.
// The VM is created once and reused across calls (persistent handle).
//
//   image_path : path to a stock factor.image (NULL -> VM default/embedded)
//   src        : Factor source to evaluate (printed output is captured)
//   returns    : a malloc'd C string with eval>string's output; caller
//                frees via factor_embed_eval_free. NULL on failure.
extern "C" VM_C_API char* factor_embed_eval(const char* image_path,
                                            const char* src) {
  if (!src) return NULL;
  if (embed_vm_init(image_path) != 0) return NULL;
  // factor_eval_string takes a mutable char*; copy in.
  return g_embed_vm->factor_eval_string((char*)src);
}

extern "C" VM_C_API void factor_embed_eval_free(char* result) {
  if (g_embed_vm && result) g_embed_vm->factor_eval_free(result);
}

// --- Host key-value hooks (generic, not Aether-specific) -------------------
//
// A host may register two callbacks so embedded Factor code can read/write a
// host-owned key-value store while it runs. This is the embedding analogue of
// liblua's host-installed C functions: the host supplies the storage, Factor
// reaches it through two exported C entry points (factor_embed_map_get /
// factor_embed_map_put) which a tiny Factor FFI prelude wraps as words. The
// hooks are host-agnostic — strings only, no host types leak into libfactor.
//
//   get(key) -> value   : returns a borrowed C string owned by the HOST
//                         (valid until the next map call); NULL if absent.
//   put(key, value)     : stores a copy host-side.
//
// Single-threaded, matching the rest of the embed API: the host serialises
// calls and sets/clears these hooks around a run.
typedef const char* (*factor_embed_map_get_fn)(const char* key);
typedef void        (*factor_embed_map_put_fn)(const char* key,
                                               const char* value);

static factor_embed_map_get_fn g_embed_map_get = NULL;
static factor_embed_map_put_fn g_embed_map_put = NULL;

extern "C" VM_C_API void factor_embed_map_set_hooks(
    factor_embed_map_get_fn get_fn, factor_embed_map_put_fn put_fn) {
  g_embed_map_get = get_fn;
  g_embed_map_put = put_fn;
}

// Called FROM Factor (via the FFI prelude) to read a host key. Returns a
// borrowed C string (host-owned) or NULL. Factor copies it into a Factor
// string at the boundary (c-string return marshalling), so the borrow only
// needs to outlive that copy.
extern "C" VM_C_API const char* factor_embed_map_get(const char* key) {
  if (!g_embed_map_get || !key) return NULL;
  return g_embed_map_get(key);
}

// Called FROM Factor to write a host key. Both strings are borrowed for the
// duration of the call; the host copies what it needs.
extern "C" VM_C_API void factor_embed_map_put(const char* key,
                                              const char* value) {
  if (!g_embed_map_put || !key || !value) return;
  g_embed_map_put(key, value);
}
