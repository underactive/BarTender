# External Integrations

Document each third-party service, SDK, model bundle, or asset the project
depends on. Add a new `##` section per integration. Keep entries factual —
this is the doc an agent reads before touching anything that calls out of
process.

<!-- Template — duplicate this block per integration. Delete this comment
     and the example below once you have real entries. -->

## {{INTEGRATION_NAME}}

- **What:** {{ONE_LINE_DESCRIPTION}}
  <!-- What the integration is and what it provides. -->
- **Loaded via:** {{LOAD_PATH}}
  <!-- Which module/class instantiates the client or session, and where the
       credentials/files come from. -->
- **Lifecycle:** {{LIFECYCLE}}
  <!-- When it's created, when it's released, where teardown lives.
       Mention pooling/caching if relevant. -->
- **Environment / hostname gating:** {{GATING}}
  <!-- Local-only? Production-only? Behind a feature flag? Any network
       calls triggered by external data should be called out here. -->
- **Key env vars / CLI flags:** {{ENV_VARS}}
  <!-- List the env vars and flags that control this integration, with
       defaults. -->
- **Gotchas:** {{GOTCHAS}}
  <!-- Cold start costs, version pinning, license/distribution concerns,
       packaging quirks, anything that has burned someone before. -->

### Required files / credentials

<!-- If the integration needs local files or out-of-band credentials,
     describe what is needed and how to obtain or verify them. Link to
     a hash file (e.g., EXPECTED_HASHES.txt) if integrity matters. -->

| File / credential | Role | Source |
|-------------------|------|--------|
| {{FILE_OR_KEY}}   | {{ROLE}} | {{SOURCE_URL_OR_PROVIDER}} |
