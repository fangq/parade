/*
 * stand-in for libFuzzer, so the targets build with any C compiler: runs
 * every file given (directories are walked one level), then -runs=N
 * deterministic mutations (flips, inserts, deletes, truncation, splices)
 * of those inputs from -seed=S. Pair with ASan/UBSan for a CI smoke test;
 * use clang -fsanitize=fuzzer for real coverage-guided fuzzing.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

typedef struct {
    uint8_t* data;
    size_t size;
} input;

static input* inputs;
static size_t ninputs, capinputs;
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t next_rand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static void add_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    long n;
    uint8_t* buf;

    if (!fp) {
        return;
    }

    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);

    if (buf && (n <= 0 || fread(buf, 1, (size_t)n, fp) == (size_t)n)) {
        if (ninputs == capinputs) {
            capinputs = capinputs ? capinputs * 2 : 64;
            inputs = (input*)realloc(inputs, capinputs * sizeof(input));
        }

        inputs[ninputs].data = buf;
        inputs[ninputs].size = n > 0 ? (size_t)n : 0;
        ninputs++;
    } else {
        free(buf);
    }

    fclose(fp);
}

static void add_path(const char* path) {
    struct stat st;
    DIR* dir;
    struct dirent* e;
    char sub[4096];

    if (stat(path, &st) != 0) {
        fprintf(stderr, "skipping %s: not found\n", path);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        add_file(path);
        return;
    }

    dir = opendir(path);

    while (dir && (e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }

        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);

        if (stat(sub, &st) == 0 && S_ISREG(st.st_mode)) {
            add_file(sub);
        }
    }

    if (dir) {
        closedir(dir);
    }
}

/* one mutated copy of a random input into buf (capacity cap), returns its size */
static size_t mutate(uint8_t* buf, size_t cap) {
    static const char special[] = "<>{}[]\\$&#*_`|\"'\n\xff\xc3\xe2";
    const input* a = &inputs[next_rand() % ninputs];
    const input* b;
    size_t n = a->size < cap ? a->size : cap;
    size_t i, k, at, rounds = 1 + next_rand() % 8;

    memcpy(buf, a->data, n);

    for (k = 0; k < rounds; k++) {
        at = n ? next_rand() % n : 0;

        switch (next_rand() % 6) {
            case 0:     /* flip a bit */
                if (n) {
                    buf[at] ^= (uint8_t)(1u << (next_rand() % 8));
                }

                break;

            case 1:     /* random byte, biased to syntax characters */
                if (n) {
                    buf[at] = (next_rand() & 1) ? (uint8_t)special[next_rand() % (sizeof(special) - 1)]
                              : (uint8_t)next_rand();
                }

                break;

            case 2:     /* insert a run of one byte */
                i = 1 + next_rand() % 16;

                if (n + i <= cap) {
                    memmove(buf + at + i, buf + at, n - at);
                    memset(buf + at, (int)(next_rand() & 0xff), i);
                    n += i;
                }

                break;

            case 3:     /* delete a run of bytes */
                i = 1 + next_rand() % 64;

                if (at + i <= n) {
                    memmove(buf + at, buf + at + i, n - at - i);
                    n -= i;
                }

                break;

            case 4:     /* truncate */
                n = at;
                break;

            default:    /* overwrite with a piece of another input */
                b = &inputs[next_rand() % ninputs];

                if (b->size) {
                    size_t from = next_rand() % b->size;
                    size_t len = 1 + next_rand() % 256;

                    if (from + len > b->size) {
                        len = b->size - from;
                    }

                    if (at + len > cap) {
                        len = cap - at;
                    }

                    memcpy(buf + at, b->data + from, len);

                    if (at + len > n) {
                        n = at + len;
                    }
                }

                break;
        }
    }

    return n;
}

int main(int argc, char** argv) {
    long runs = 0, r;
    size_t i, cap = 0, n;
    uint8_t* buf;
    int a;

    for (a = 1; a < argc; a++) {
        if (strncmp(argv[a], "-runs=", 6) == 0) {
            runs = atol(argv[a] + 6);
        } else if (strncmp(argv[a], "-seed=", 6) == 0) {
            rng = (uint64_t)strtoull(argv[a] + 6, NULL, 10) * 0x9E3779B97F4A7C15ull + 1;
        } else if (argv[a][0] != '-') {
            add_path(argv[a]);
        }
    }

    for (i = 0; i < ninputs; i++) {
        LLVMFuzzerTestOneInput(inputs[i].data, inputs[i].size);

        if (inputs[i].size > cap) {
            cap = inputs[i].size;
        }
    }

    printf("%s: %lu inputs", argv[0], (unsigned long)ninputs);

    if (ninputs && runs > 0) {
        cap = cap * 2 + 256;
        buf = (uint8_t*)malloc(cap);

        for (r = 0; buf && r < runs; r++) {
            n = mutate(buf, cap);
            LLVMFuzzerTestOneInput(buf, n);
        }

        free(buf);
        printf(", %ld mutations", runs);
    }

    printf(", no crash\n");

    for (i = 0; i < ninputs; i++) {
        free(inputs[i].data);
    }

    free(inputs);
    return 0;
}
