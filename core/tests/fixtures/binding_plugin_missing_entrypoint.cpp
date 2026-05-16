#if defined(_WIN32)
#define FLEAUX_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define FLEAUX_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Intentionally exports a different symbol to validate missing-entrypoint handling.
FLEAUX_PLUGIN_EXPORT auto fleaux_not_the_entrypoint() -> int {
  return 0;
}

