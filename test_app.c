#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

static long file_size(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long sz = ftell(f);
    if (sz < 0) return -1;
    if (fseek(f, 0, SEEK_SET) != 0) return -1;
    return sz;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <path_to_so> <key> <input_file> <output_file>\n", argv[0]);
        return 2;
    }

    const char* so_path = argv[1];
    const char* key_str = argv[2];
    const char* in_path = argv[3];
    const char* out_path = argv[4];

    char* endp = NULL;
    long key_long = strtol(key_str, &endp, 0);
    if (endp == key_str || *endp != '\0' || key_long < -128 || key_long > 255) {
        fprintf(stderr, "Invalid key: %s\n", key_str);
        return 2;
    }

    char key = (char)key_long;

    void* handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    dlerror();
    set_key_fn set_key = (set_key_fn)dlsym(handle, "set_key");
    const char* err1 = dlerror();

    dlerror();
    caesar_fn caesar = (caesar_fn)dlsym(handle, "caesar");
    const char* err2 = dlerror();

    if (err1 || err2 || !set_key || !caesar) {
        fprintf(stderr, "dlsym failed: %s %s\n", err1 ? err1 : "", err2 ? err2 : "");
        dlclose(handle);
        return 1;
    }

    FILE* in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "Cannot open input file '%s': %s\n", in_path, strerror(errno));
        dlclose(handle);
        return 1;
    }

    long sz = file_size(in);
    if (sz < 0) {
        fprintf(stderr, "Cannot determine file size\n");
        fclose(in);
        dlclose(handle);
        return 1;
    }

    unsigned char* buf_in = (unsigned char*)malloc((size_t)sz);
    unsigned char* buf_out = (unsigned char*)malloc((size_t)sz);

    if (!buf_in || !buf_out) {
        fprintf(stderr, "Out of memory\n");
        fclose(in);
        free(buf_in);
        free(buf_out);
        dlclose(handle);
        return 1;
    }

    size_t rd = fread(buf_in, 1, (size_t)sz, in);
    fclose(in);

    if (rd != (size_t)sz) {
        fprintf(stderr, "Read error\n");
        free(buf_in);
        free(buf_out);
        dlclose(handle);
        return 1;
    }

    set_key(key);
    caesar(buf_in, buf_out, (int)sz);

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Cannot open output file '%s': %s\n", out_path, strerror(errno));
        free(buf_in);
        free(buf_out);
        dlclose(handle);
        return 1;
    }

    size_t wr = fwrite(buf_out, 1, (size_t)sz, out);
    fclose(out);

    if (wr != (size_t)sz) {
        fprintf(stderr, "Write error\n");
        free(buf_in);
        free(buf_out);
        dlclose(handle);
        return 1;
    }

    free(buf_in);
    free(buf_out);
    dlclose(handle);

    return 0;
}
