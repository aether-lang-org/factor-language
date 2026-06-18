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

}  // namespace factor

using namespace factor;

// One-shot eval: init a fresh VM from `image_path`, evaluate `src` (which
// runs through Factor's `-e=` eval chain and prints to stdout), then tear
// the VM down. Returns 0 on success. `image_path` may be NULL to use the
// default/embedded image. This is the minimal proof that a host program
// can drive an embedded Factor through libfactor.
extern "C" VM_C_API int factor_embed_eval_oneshot(const char* image_path,
                                                  const char* src) {
  if (!src) return 1;

  int argc;
  char** argv = embed_make_argv(src, &argc);

  // start_standalone_factor takes (argc, argv): it inits the VM from the
  // args (including -i=<image> if present), passes argv to Factor, and
  // runs the startup quotation — which sees -e=<src> and evaluates it.
  // Prepend -i=<image> when an explicit image path is given.
  if (image_path && image_path[0]) {
    char** argv2 = (char**)malloc(sizeof(char*) * 3);
    argv2[0] = argv[0];
    size_t n = strlen(image_path);
    char* iflag = (char*)malloc(n + 4);
    memcpy(iflag, "-i=", 3);
    memcpy(iflag + 3, image_path, n + 1);
    argv2[1] = iflag;
    argv2[2] = argv[1];
    free(argv);  // argv[0]/argv[1] now owned by argv2
    start_standalone_factor(3, argv2);
    free(argv2[1]);  // -i= flag
    free(argv2[0]);
    free(argv2[2]);
    free(argv2);
    return 0;
  }

  start_standalone_factor(argc, argv);
  embed_free_argv(argv, argc);
  return 0;
}
