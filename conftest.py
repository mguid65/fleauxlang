# Prevent pytest from collecting generated transpiler artifacts as tests.
collect_ignore_glob = [
    "fleaux_generated_module_*.py",
    "**/fleaux_generated_module_*.py",
]

