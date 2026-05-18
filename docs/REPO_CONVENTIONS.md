# Repo Conventions

<!-- Fill in each convention for your project. Examples shown in comments. -->

- **Language:** {{LANGUAGE}}
  <!-- e.g., TypeScript (strict mode). Prefer boring, composable tech. -->
- **Boundaries:** {{BOUNDARY_STRATEGY}}
  <!-- e.g., Parse and validate all data at system edges (API responses, CLI args). Interior code trusts typed interfaces. -->
- **Tests:** {{TEST_STRATEGY}}
  <!-- e.g., Every module has co-located tests. Parsers require snapshot tests with real input samples. -->
- **Logging:** {{LOGGING_STRATEGY}}
  <!-- e.g., Structured JSON logging only. No bare console.log. -->
- **Naming:** {{NAMING_CONVENTIONS}}
  <!-- e.g., kebab-case files, PascalCase types, camelCase functions. -->
- **File size:** {{FILE_SIZE_LIMIT}}
  <!-- e.g., Keep files under 300 lines. If a file grows past that, split it. -->
- **Imports:** {{IMPORT_RULES}}
  <!-- e.g., No circular imports. Dependency direction follows the layer diagram in ARCHITECTURE.md. -->
