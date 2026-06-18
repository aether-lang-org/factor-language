/* embed_spike_driver.c — proves a host C program can embed Factor via the
 * forked libfactor's generic embedding API. dlopen the lib, resolve
 * factor_embed_eval_oneshot, eval "2 3 + .", expect it to print 5.
 *
 * Build:
 *   cc vm/embed_spike_driver.c -ldl -o embed_spike
 *   ./embed_spike ./libfactor.a ./factor.image
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*eval_oneshot_fn)(const char* image_path, const char* src);

int main(int argc, char** argv) {
    const char* libpath = argc > 1 ? argv[1] : "./libfactor.a";
    const char* image   = argc > 2 ? argv[2] : "./factor.image";

    void* h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }

    eval_oneshot_fn eval =
        (eval_oneshot_fn)dlsym(h, "factor_embed_eval_oneshot");
    if (!eval) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 3; }

    fprintf(stderr, "[driver] evaluating: 2 3 + .\n");
    fflush(stderr);
    int rc = eval(image, "2 3 + .");
    fprintf(stderr, "[driver] eval returned %d\n", rc);
    return rc;
}
