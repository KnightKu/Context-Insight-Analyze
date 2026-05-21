#define _POSIX_C_SOURCE 200809L

#include "insight_api_json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_ok(const char *name, int rc) {
    if (rc != 0) {
        fprintf(stderr, "%s: expected success, got rc=%d errno=%d (%s)\n",
                name, rc, errno, strerror(errno));
        ++failures;
    }
}

static void expect_fail(const char *name, int rc, int want_errno) {
    if (rc == 0 || errno != want_errno) {
        fprintf(stderr, "%s: expected errno=%d, got rc=%d errno=%d (%s)\n",
                name, want_errno, rc, errno, strerror(errno));
        ++failures;
    }
}

static void expect_json_valid(const char *name, const char *json) {
    if (insight_json_validate_root_object(json) != 0) {
        fprintf(stderr, "%s: invalid root JSON: %s\n", name, strerror(errno));
        ++failures;
    }
}

static void expect_str_eq(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: mismatch\n  got:  %s\n  want: %s\n", name, got, want);
        ++failures;
    }
}

static void test_extract_last_root_object(void) {
    char out[4096];

    const char *latency_only =
        "post action soft-stop at offset=123: time-window end marker reached\n"
        "{\n"
        "  \"latency_percentiles\": {\n"
        "    \"read\": {\"count\": 1, \"min_us\": 10, \"max_us\": 20, \"avg_us\": 15.00,\n"
        "      \"percentiles_us\": {\"p50\": 15}}\n"
        "  }\n"
        "}\n";
    errno = 0;
    expect_ok("extract latency stderr",
              insight_json_extract_last_root_object(latency_only, out, sizeof(out)));
    expect_json_valid("latency stderr json", out);
    expect_str_eq("latency key present",
                  strstr(out, "\"latency_percentiles\"") != NULL ? "yes" : "no", "yes");

    const char *noisy_braces =
        "log { not json } still here\n"
        "{\n"
        "  \"qd_dist\": {\"total\": 2, \"buckets\": []}\n"
        "}\n";
    errno = 0;
    expect_ok("extract after fake braces",
              insight_json_extract_last_root_object(noisy_braces, out, sizeof(out)));
    expect_json_valid("qd stderr json", out);
    expect_str_eq("qd_dist extracted", strstr(out, "\"qd_dist\"") != NULL ? "yes" : "no", "yes");

    const char *broken_span =
        "prefix { incomplete\n"
        "{\n"
        "  \"write_amplification\": 1.5\n"
        "}\n";
    errno = 0;
    expect_ok("extract skips incomplete span",
              insight_json_extract_last_root_object(broken_span, out, sizeof(out)));
    expect_str_eq("wa value", strstr(out, "\"write_amplification\": 1.5") != NULL ? "yes" : "no", "yes");

    errno = 0;
    expect_fail("extract no object", insight_json_extract_last_root_object("no braces here\n", out, sizeof(out)),
                ENODATA);
}

static void test_capture_stderr_object(void) {
    char out[4096];
    const char *payload =
        "noise\n"
        "{\n"
        "  \"latency_percentiles\": {}\n"
        "}\n";

    FILE *fp = tmpfile();
    if (fp == NULL) {
        fprintf(stderr, "tmpfile failed\n");
        ++failures;
        return;
    }
    if (fputs(payload, fp) == EOF) {
        fprintf(stderr, "fputs failed\n");
        ++failures;
        fclose(fp);
        return;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "fseek failed\n");
        ++failures;
        fclose(fp);
        return;
    }

    errno = 0;
    expect_ok("capture_stderr_object",
              insight_json_capture_stderr_object(fp, out, sizeof(out), 65536U));
    fclose(fp);
    expect_json_valid("captured json", out);
    expect_str_eq("empty latency object", strstr(out, "\"latency_percentiles\"") != NULL ? "yes" : "no", "yes");
}

static void test_flatten_and_compose(void) {
    char flat[2048];
    char envelope[INSIGHT_JSON_BUFFER_BYTES];

    const char *wrapped =
        "{\n"
        "  \"latency_percentiles\": {\n"
        "    \"read\": {\"count\": 3, \"avg_us\": 1.25}\n"
        "  }\n"
        "}\n";

    errno = 0;
    expect_ok("flatten latency wrapper",
              insight_json_flatten_single_root_object(wrapped, flat, sizeof(flat)));
    expect_json_valid("flattened inner", flat);
    expect_str_eq("read bucket kept", strstr(flat, "\"read\"") != NULL ? "yes" : "no", "yes");

    const char *result =
        "{\n"
        "  \"count\": 3,\n"
        "  \"avg_us\": 1.25\n"
        "}\n";
    errno = 0;
    expect_ok("compose envelope",
              insight_json_compose_query_result("get_read_latency_percentiles",
                                                "/dev/nvme0n1",
                                                0,
                                                0ULL,
                                                "2024-03-09 16:00:05",
                                                "2024-03-09 16:00:15",
                                                7LL,
                                                result,
                                                envelope));
    expect_json_valid("full envelope", envelope);
    expect_str_eq("api field", strstr(envelope, "\"api\": \"get_read_latency_percentiles\"") != NULL ? "yes" : "no", "yes");
    expect_str_eq("escaped device path",
                  strstr(envelope, "\"device\": \"/dev/nvme0n1\"") != NULL ? "yes" : "no", "yes");

    const char *bad_result = "{ not quite json ";
    errno = 0;
    expect_fail("compose rejects invalid result",
                insight_json_compose_query_result("x", "/d", 0, 0ULL,
                                                  "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                                  0LL,
                                                  bad_result, envelope),
                EINVAL);

    const char *inner = "{\"total\": 0}";
    errno = 0;
    expect_ok("compose session mode",
              insight_json_compose_query_result("get_nand_write_volume",
                                                "/dev/nvme1n1",
                                                0,
                                                0ULL,
                                                "2024-03-09 16:00:05",
                                                "2024-03-09 16:00:15",
                                                42LL,
                                                inner,
                                                envelope));
    expect_json_valid("session envelope", envelope);
    expect_str_eq("session_id field", strstr(envelope, "\"session_id\": 42") != NULL ? "yes" : "no", "yes");
    expect_str_eq("time_start field", strstr(envelope, "\"time_start\": \"2024-03-09 16:00:05\"") != NULL ? "yes" : "no", "yes");

    const char *escaped_dev = "/dev/\"weird\"";
    errno = 0;
    expect_ok("compose escaped device",
              insight_json_compose_query_result("get_qd_distribution",
                                                escaped_dev,
                                                0,
                                                0ULL,
                                                "2024-03-09 16:00:05",
                                                "2024-03-09 16:00:15",
                                                1LL,
                                                inner,
                                                envelope));
    expect_json_valid("escaped envelope", envelope);
    expect_str_eq("backslash-quotes in device",
                  strstr(envelope, "\"device\": \"/dev/\\\"weird\\\"\"") != NULL ? "yes" : "no", "yes");
}

static int run_python_json_check(const char *json) {
    FILE *fp = tmpfile();
    if (fp == NULL) {
        return -1;
    }
    if (fputs(json, fp) == EOF) {
        fclose(fp);
        return -1;
    }
    fflush(fp);
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    FILE *py = popen("python3 -c 'import json,sys; json.load(sys.stdin)'", "w");
    if (py == NULL) {
        fclose(fp);
        return -1;
    }
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1U, sizeof(buf), fp)) > 0U) {
        if (fwrite(buf, 1U, got, py) != got) {
            pclose(py);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    int status = pclose(py);
    return (status == 0) ? 0 : -1;
}

static void test_python_cross_check(void) {
    char envelope[INSIGHT_JSON_BUFFER_BYTES];
    const char *result = "{\n  \"write_amplification\": 2.5\n}\n";
    if (insight_json_compose_query_result("get_write_amplification",
                                          "/dev/nvme0n1",
                                          0,
                                          0ULL,
                                          "2026-05-19 17:50:23",
                                          "2026-05-19 17:50:28",
                                          3LL,
                                          result,
                                          envelope) != 0) {
        fprintf(stderr, "compose for python check failed\n");
        ++failures;
        return;
    }
    FILE *probe = popen("python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null", "w");
    if (probe == NULL) {
        fprintf(stderr, "skip python3 json.load cross-check (python3/popen unavailable)\n");
        return;
    }
    pclose(probe);
    if (run_python_json_check(envelope) != 0) {
        fprintf(stderr, "python3 json.load cross-check failed\n");
        ++failures;
    }
}

int main(void) {
    failures = 0;
    test_extract_last_root_object();
    test_capture_stderr_object();
    test_flatten_and_compose();
    test_python_cross_check();
    if (failures != 0) {
        fprintf(stderr, "insight_api_json_tester: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
