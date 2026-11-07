/* Bionic gained these in Android 8.0 and prebuilt helpers (PulseAudio, the Vulkan bridge server)
 * reference them. Its only locale is C.UTF-8, so the answers match API 26+; mallopt reports every
 * option as unsupported, as bionic does for unknown ones. System headers are avoided: the NDK
 * declares inline fallbacks under the same names for API < 26. */

#define DD_CODESET 1

extern double strtod(const char *text, char **end);

__attribute__((visibility("default"))) char *nl_langinfo(int item) {
    return item == DD_CODESET ? (char *)"UTF-8" : (char *)"";
}

__attribute__((visibility("default"))) double strtod_l(const char *text, char **end, void *locale) {
    (void)locale;
    return strtod(text, end);
}

__attribute__((visibility("default"))) int mallopt(int option, int value) {
    (void)option;
    (void)value;
    return 0;
}
