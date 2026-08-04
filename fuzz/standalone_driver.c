/* standalone_driver.c — run a fuzz target over corpus files without
 * libFuzzer (Apple clang ships no fuzzer runtime). Real coverage-guided
 * fuzzing runs in CI on Linux clang; this makes every corpus entry a local
 * regression test under ASan/UBSan.
 *
 * Usage: fuzz_target [file-or-dir ...]   (default: reads stdin)
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 16 * 1024 * 1024) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    free(buf);
    return 0;
}

static int run_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        int count = 0;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            if (run_file(full) == 0) count++;
        }
        closedir(d);
        fprintf(stderr, "%s: %d corpus entries OK\n", path, count);
        return 0;
    }
    return run_file(path);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, stdin);
        LLVMFuzzerTestOneInput(buf, n);
        return 0;
    }
    for (int i = 1; i < argc; i++)
        if (run_path(argv[i]) != 0) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 1;
        }
    return 0;
}
